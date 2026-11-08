#include "t230c_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <thread>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "msg.h"
#include "t230c_pipe_timeout.hpp"
#include "util.hpp"
#include "t230c_tuning.hpp"

namespace px4 {
namespace {
/* 1 read で2 byte残留し次の read で空になった実測に、3応答分の上限を置く。 */
constexpr unsigned int kT230CResyncReadTimeoutMs = 5;
constexpr unsigned int kT230CResyncMaxReadsPerEndpoint = 4;
}

class T230CDevice::Receiver final : public ReceiverBase {
public:
	explicit Receiver(T230CDevice &parent);
	~Receiver() override;
	int Open() override;
	void Close() override;
	int SetLnbVoltage(std::int32_t voltage) override;
	int SetCapture(bool capture) override;
	int CheckLock(bool &locked) override;
	int ReadStat(command::StatType type, std::int32_t &value) override;

protected:
	bool TuneOverride(std::uint32_t timeout, bool &result) override;
	int PrepareTune() override;
	int SetFrequency() override;
	int SetStreamId() override;

private:
	int StartUsbStream();
	int StopUsbStream();

	T230CDevice &parent_;
	std::condition_variable_any close_cond_;
	/* クライアントの SET_CAPTURE(true) で配信バッファーを開始済みか。 */
	bool buffer_started_ = false;
};

/* 設定の受信機数が機器の構成と異なる場合は登録前に拒否する。 */
T230CDevice::T230CDevice(const std::wstring &path, const DeviceDefinition &definition,
	std::uintptr_t index, ReceiverManager &receiver_manager)
	: DeviceBase(path, definition, index, receiver_manager)
{
	if (definition.receivers.size() != 1 || definition.receivers[0].index != 0)
		throw DeviceError("T230C requires exactly one receiver.");
}

/* 登録済みの受信機と転送を USB ハンドルより先に解放する。 */
T230CDevice::~T230CDevice()
{
	Term();
}

/* 個々の USB 待機も選局操作の残り時間を超えないようにする。 */
unsigned int T230CDevice::ControlTimeout() const
{
	if (deadline_ == (t230c::Deadline::max)())
		return 2000;
	auto remaining = deadline_ - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero())
		return 0;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
	return static_cast<unsigned int>(std::min<std::int64_t>(2000, (std::max<std::int64_t>)(1, ms)));
}

/* コマンド送信の短い転送は失敗として上位へ返す。 */
int T230CDevice::TransferOut(const std::uint8_t *data, std::size_t size,
			     unsigned int timeout_ms)
{
	const unsigned int control_timeout = ControlTimeout();
	const unsigned int timeout = timeout_ms && timeout_ms < control_timeout
		? timeout_ms : control_timeout;
	if (!timeout)
		return -ETIMEDOUT;
#ifdef __APPLE__
	int actual = 0;
	int ret = libusb_bulk_transfer(usb_dev_.handle, 0x01, const_cast<unsigned char *>(data),
		static_cast<int>(size), &actual, timeout);
	if (ret == LIBUSB_ERROR_TIMEOUT)
		return -ETIMEDOUT;
	return ret || actual != static_cast<int>(size) ? -EIO : 0;
#else
	ULONG pipe_timeout = timeout;
	if (!WinUsb_SetPipePolicy(usb_dev_.winusb, 0x01, PIPE_TRANSFER_TIMEOUT, sizeof(pipe_timeout), &pipe_timeout))
		return -EIO;
	ULONG actual = 0;
	if (!WinUsb_WritePipe(usb_dev_.winusb, 0x01, const_cast<PUCHAR>(data), static_cast<ULONG>(size), &actual, nullptr))
		return GetLastError() == ERROR_SEM_TIMEOUT ? -ETIMEDOUT : -EIO;
	return actual == size ? 0 : -EIO;
#endif
}

/* コマンド応答の待機も操作全体の期限内に収める。 */
int T230CDevice::TransferIn(std::uint8_t endpoint, std::uint8_t *data,
			    std::size_t size, std::size_t &actual,
			    unsigned int timeout_ms)
{
	const unsigned int control_timeout = ControlTimeout();
	const unsigned int timeout = timeout_ms && timeout_ms < control_timeout
		? timeout_ms : control_timeout;
	actual = 0;
	if (!timeout)
		return -ETIMEDOUT;
#ifdef __APPLE__
	int transferred = 0;
	int ret = libusb_bulk_transfer(usb_dev_.handle, endpoint, data, static_cast<int>(size), &transferred, timeout);
	actual = static_cast<std::size_t>(std::max(transferred, 0));
	if (ret == LIBUSB_ERROR_TIMEOUT)
		return -ETIMEDOUT;
	return ret ? -EIO : 0;
#else
	return t230c::ReadWithTemporaryTimeout(this, endpoint, data, size,
		actual, timeout,
		[](void *context, std::uint8_t pipe, std::uint32_t &previous) {
			auto &device = *static_cast<T230CDevice *>(context);
			ULONG value = 0;
			ULONG value_size = sizeof(value);
			if (!WinUsb_GetPipePolicy(device.usb_dev_.winusb, pipe,
						  PIPE_TRANSFER_TIMEOUT, &value_size, &value))
				return -EIO;
			previous = value;
			return 0;
		},
		[](void *context, std::uint8_t pipe, std::uint32_t value) {
			auto &device = *static_cast<T230CDevice *>(context);
			ULONG pipe_timeout = value;
			return WinUsb_SetPipePolicy(device.usb_dev_.winusb, pipe,
						    PIPE_TRANSFER_TIMEOUT,
						    sizeof(pipe_timeout), &pipe_timeout)
				? 0 : -EIO;
		},
		[](void *context, std::uint8_t pipe, std::uint8_t *buffer,
		   std::size_t buffer_size, std::size_t &transferred) {
		auto &device = *static_cast<T230CDevice *>(context);
		ULONG actual_size = 0;
		if (!WinUsb_ReadPipe(device.usb_dev_.winusb, pipe, buffer,
				     static_cast<ULONG>(buffer_size), &actual_size, nullptr))
			return GetLastError() == ERROR_SEM_TIMEOUT ? -ETIMEDOUT : -EIO;
		transferred = actual_size;
		return 0;
		});
#endif
}

/* USB ブリッジの応答は要求したバイト数だけ消費する。 */
int T230CDevice::Command(const std::uint8_t *data, std::size_t size,
	std::uint8_t *response, std::size_t response_size)
{
	return t230c_usb_command(&usb_core_, data, size, response,
				 response_size, ControlTimeout());
}

int T230CDevice::UsbBulkTransfer(void *context, t230c_u8 endpoint,
				t230c_u8 *data, std::size_t size,
				std::size_t *actual, t230c_u32 timeout_ms)
{
	auto &device = *static_cast<T230CDevice *>(context);
	if (!actual || !timeout_ms)
		return -ETIMEDOUT;
	*actual = 0;
	if (endpoint == 0x01) {
		int ret = device.TransferOut(data, size, timeout_ms);
		if (!ret)
			*actual = size;
		return ret;
	}
	if (endpoint == 0x81 || endpoint == 0x82)
		return device.TransferIn(endpoint, data, size, *actual, timeout_ms);
	return -EINVAL;
}

/* Si2141 は Si2168 の I²C ゲートの先にあり、転送ごとに開閉する。 */
int T230CDevice::I2c(std::uint8_t address, const t230c::Bytes &write, t230c::Bytes &read)
{
	if (address != 0x60)
		return I2cRaw(address, write, read);
	t230c::Bytes response;
	int ret = I2cRaw(0x64, {0xc0, 0x0d, 0x01}, response);
	if (ret)
		return ret;
	ret = I2cRaw(address, write, read);
	response.clear();
	int close_ret = I2cRaw(0x64, {0xc0, 0x0d, 0x00}, response);
	return ret ? ret : close_ret;
}

/* ブリッジ固有の長さ制限と応答ヘッダーをこの層で扱う。 */
int T230CDevice::I2cRaw(std::uint8_t address, const t230c::Bytes &write, t230c::Bytes &read)
{
	/* Si2141 と write を含む I2C request は副作用があるため再送しない。 */
	if (address == 0x64 && write.empty() && !read.empty())
		return t230c_usb_si2168_read(&usb_core_, address, read.data(), read.size(),
				  ControlTimeout(), kT230CResyncReadTimeoutMs,
				  kT230CResyncMaxReadsPerEndpoint);
	if (read.empty())
		return t230c_usb_i2c_transfer(&usb_core_, address,
				write.empty() ? nullptr : write.data(), write.size(),
				nullptr, 0, ControlTimeout());
	return t230c_usb_i2c_transfer(&usb_core_, address,
				write.empty() ? nullptr : write.data(), write.size(),
				read.data(), read.size(), ControlTimeout());
}

/* ファームウェアとハードウェアの準備後にだけ受信機を公開する。 */
int T230CDevice::Init()
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (initialized_)
		return -EALREADY;
	static const struct t230c_usb_ops usb_ops = {
		UsbBulkTransfer,
		nullptr,
	};
	int usb_ret = t230c_usb_init(&usb_core_, &usb_ops, this, 2000);
	if (usb_ret)
		return usb_ret;
	/* 最初の USB command より前に response FIFO と前回の TS を排出する。 */
	struct t230c_usb_resync_stats resync_stats = {};
	int resync_ret = t230c_usb_resynchronize(&usb_core_, kT230CResyncReadTimeoutMs,
						 kT230CResyncMaxReadsPerEndpoint,
						 &resync_stats);
	char ep81_preview[64 * 2 + 1] = {};
	for (unsigned int i = 0; i < resync_stats.ep81_preview_size; ++i)
		std::snprintf(ep81_preview + i * 2, sizeof(ep81_preview) - i * 2,
			      "%02x", resync_stats.ep81_preview[i]);
	if (resync_ret) {
		msg_err("T230C response resync failed: ret=%d, EP81 reads=%u bytes=%llu preview=%s, EP82 reads=%u bytes=%llu\n",
			resync_ret, resync_stats.ep81_reads,
			static_cast<unsigned long long>(resync_stats.ep81_bytes), ep81_preview,
			resync_stats.ep82_reads,
			static_cast<unsigned long long>(resync_stats.ep82_bytes));
		throw DeviceError("T230C response resynchronization failed (ret=" +
				  std::to_string(resync_ret) + ").");
	}
	msg_info("T230C response resync: EP81 reads=%u bytes=%llu preview=%s, EP82 reads=%u bytes=%llu limit=%u\n",
		 resync_stats.ep81_reads,
		 static_cast<unsigned long long>(resync_stats.ep81_bytes),
		 ep81_preview,
		 resync_stats.ep82_reads,
		 static_cast<unsigned long long>(resync_stats.ep82_bytes),
		 static_cast<unsigned int>(resync_stats.ep82_limit_reached));
	const auto &config = device_def_.configs;
	xfer_packets_ = util::wtoui(config.Get(L"XferPackets", L"22"));
	max_urbs_ = util::wtoui(config.Get(L"MaxUrbs", L"8"));
	receiver_packets_ = util::wtoui(config.Get(L"ReceiverMaxPackets", L"16384"));
	experimental_isdbc_ = util::wtob(config.Get(L"ExperimentalISDBC", L"false"));
	if (xfer_packets_ < 4 || xfer_packets_ > INT32_MAX / 188 ||
		!max_urbs_ || max_urbs_ > 64 || !receiver_packets_ || receiver_packets_ > UINT32_MAX / 188)
		throw DeviceError("T230C: invalid stream buffer configuration.");

	std::wstring demod_path = config.Get(L"Si2168Firmware", L"dvb-demod-si2168-d60-01.fw");
	t230c::TunerFirmwarePaths tuner_paths = {
		config.Get(L"Si2141FirmwareRom60", L"dvb_driver_si2141_rom60.fw"),
		config.Get(L"Si2141FirmwareRom61", L"dvb_driver_si2141_rom61.fw"),
		config.Get(L"Si2141Firmware", L"dvb-tuner-si2141-a10-01.fw")
	};
	std::filesystem::path exe_dir;
#ifdef _WIN32
	wchar_t exe_path[32768];
	DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, 32768);
	if (exe_len && exe_len < 32768)
		exe_dir = std::filesystem::path(exe_path).parent_path();
#elif defined(__APPLE__)
	char exe_path[4096];
	uint32_t exe_len = sizeof(exe_path);
	if (_NSGetExecutablePath(exe_path, &exe_len) == 0)
		exe_dir = std::filesystem::path(exe_path).parent_path();
#endif
	if (!exe_dir.empty()) {
		if (std::filesystem::path(demod_path).is_relative())
			demod_path = (exe_dir / demod_path).wstring();
		for (auto *path : {&tuner_paths.rom60, &tuner_paths.rom61, &tuner_paths.fallback}) {
			if (std::filesystem::path(*path).is_relative())
				*path = (exe_dir / *path).wstring();
		}
	}
	std::string error;
	if (t230c::LoadFirmwareFile(demod_path, demod_fw_, error))
		throw DeviceError(std::string("T230C firmware: ") + error);

	/* ブリッジの GPIO は Linux の identify_state と同じ順序で切り替える。 */
	const std::uint8_t gpio[][2] = {{4, 1}, {0x83, 0}, {0xc0, 1}, {0x83, 1}, {0xc0, 0}};
	const unsigned int waits[] = {20, 0, 100, 0, 50};
	for (unsigned int i = 0; i < 5; ++i) {
		int ret = t230c_usb_gpio_set(&usb_core_, gpio[i][0], gpio[i][1],
					     ControlTimeout(), kT230CResyncReadTimeoutMs,
					     kT230CResyncMaxReadsPerEndpoint);
		if (ret)
			throw DeviceError("T230C GPIO initialization failed at step " + std::to_string(i) +
				" (ret=" + std::to_string(ret) + ").");
		if (waits[i])
			std::this_thread::sleep_for(std::chrono::milliseconds(waits[i]));
	}
	silicon_.reset(new t230c::Silicon(
		[this](std::uint8_t address, const t230c::Bytes &write, t230c::Bytes &read, t230c::Deadline deadline) {
			const auto previous = deadline_;
			deadline_ = (std::min)(deadline_, deadline);
			try {
				int ret = I2c(address, write, read);
				deadline_ = previous;
				return ret;
			} catch (...) {
				deadline_ = previous;
				throw;
			}
		}, [](unsigned int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }));
	int init_ret = silicon_->Initialize(demod_fw_,
		[&tuner_paths](std::uint8_t rom, t230c::Bytes &firmware, std::string &detail) {
			return t230c::LoadTunerFirmware(rom, tuner_paths, firmware, detail);
		}, error);
	if (init_ret)
		throw DeviceError("T230C Silicon Labs initialization failed: " +
			(error.empty() ? std::to_string(init_ret) : error));

	/* 共通転送層は指定本数を全投入できた場合だけ開始を成功とする。 */
	stream_bus_.dev = &dev_;
	stream_bus_.type = ITEDTV_BUS_USB;
	stream_bus_.usb.dev = &usb_dev_;
	stream_bus_.usb.ctrl_timeout = 2000;
	stream_bus_.usb.streaming.endpoint = 0x82;
	stream_bus_.usb.streaming.align_stream_buffer_to_packet_size = true;
	stream_bus_.usb.streaming.fail_on_error = true;
	stream_bus_.usb.streaming.no_raw_io = util::wtob(config.Get(L"NoRawIo", L"false"));
	int ret = itedtv_bus_init(&stream_bus_);
	if (ret)
		return ret;
	bus_initialized_ = true;
	try {
		synchronizer_.reset(new t230c::TsSynchronizer(static_cast<std::size_t>(xfer_packets_) * 188 * 2));
	} catch (const std::bad_alloc &) {
		throw DeviceError("T230C: unable to allocate TS synchronization buffer.");
	}
	receiver_.reset(new Receiver(*this));
	buffer_ = receiver_->GetStreamBuffer();
	const auto &definition = device_def_.receivers.front();
	command::ReceiverInfo info = {};
	wcscpy_s(info.device_name, device_def_.name.c_str());
	info.device_guid = device_def_.guid;
	wcscpy_s(info.receiver_name, definition.name.c_str());
	info.receiver_guid = definition.guid;
	info.systems = definition.systems;
	// J.83 Annex B は ISDB-C と同じ ExperimentalISDBC の設定で有効にする (地上波の受信機には追加しない)
	if (experimental_isdbc_)
		info.systems |= SystemType::J83B;
	info.index = definition.index;
	receiver_manager_.Register(info, receiver_.get());
	initialized_ = true;
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_DEVICE,
		   "T230C device ready: host_index=%s usb_serial=%ls reader_id=%ls\n",
		   dev_.device_name, usb_serial_number_.c_str(), GetCardReaderName().c_str());
	return 0;
}

/* 受付停止後、転送と接続を回収してから USB 状態を解放する。 */
void T230CDevice::Term()
{
	if (initialized_)
		receiver_manager_.Unregister(receiver_.get());
	SetAvailability(false);
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (bus_initialized_ && streaming_) {
			itedtv_bus_stop_streaming(&stream_bus_);
			LogStreamStats(msg_now_ms(), true);
		}
		streaming_ = false;
	}
	receiver_.reset();
	if (bus_initialized_) {
		itedtv_bus_term(&stream_bus_);
		bus_initialized_ = false;
	}
	buffer_.reset();
	silicon_.reset();
	initialized_ = false;
}

/* デバイス一覧のロック下では通知だけ行い、転送の回収は Term で待つ。 */
void T230CDevice::SetAvailability(bool available)
{
	available_ = available;
	if (!available && buffer_)
		buffer_->NotifyStreamFailure();
}

/* この機種が公開する受信機は 0 番だけ。 */
ReceiverBase *T230CDevice::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("receiver id out of range");
	return receiver_.get();
}

void T230CDevice::RecordCnr(std::int32_t cnr_millibel) noexcept
{
	if (!msg_log_get_stats_interval_ms() ||
	    !px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS))
		return;
	cnr_sample_count_.fetch_add(1, std::memory_order_relaxed);
	cnr_sample_sum_.fetch_add(cnr_millibel, std::memory_order_relaxed);
	auto minimum = cnr_sample_min_.load(std::memory_order_relaxed);
	while (cnr_millibel < minimum &&
	       !cnr_sample_min_.compare_exchange_weak(minimum, cnr_millibel,
						      std::memory_order_relaxed)) {}
	auto maximum = cnr_sample_max_.load(std::memory_order_relaxed);
	while (cnr_millibel > maximum &&
	       !cnr_sample_max_.compare_exchange_weak(maximum, cnr_millibel,
						      std::memory_order_relaxed)) {}
}

void T230CDevice::ResetCnrStats() noexcept
{
	cnr_sample_count_.store(0, std::memory_order_relaxed);
	cnr_sample_sum_.store(0, std::memory_order_relaxed);
	cnr_sample_min_.store((std::numeric_limits<std::int32_t>::max)(), std::memory_order_relaxed);
	cnr_sample_max_.store((std::numeric_limits<std::int32_t>::lowest)(), std::memory_order_relaxed);
}

void T230CDevice::LogStreamStats(std::uint64_t now_ms, bool force) noexcept
{
	const std::uint32_t interval_ms = msg_log_get_stats_interval_ms();
	if (!interval_ms || !px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS) ||
	    (!force && now_ms >= stats_report_ms_ &&
			     now_ms - stats_report_ms_ < interval_ms))
		return;
	const std::uint64_t elapsed_ms = now_ms >= stats_report_ms_ ? now_ms - stats_report_ms_ : 0;
	stats_report_ms_ = now_ms;

	const auto stats = stream_diagnostics_.TakeInterval();
	const std::uint64_t emitted_bytes = stream_output_packets_ * T230C_TS_PACKET_SIZE;
	const std::size_t pending_bytes = synchronizer_ ? synchronizer_->PendingSize() : 0;
	const std::uint64_t consumed_bytes = emitted_bytes + pending_bytes;
	const std::uint64_t sync_discarded_total = stream_input_bytes_ > consumed_bytes
		? stream_input_bytes_ - consumed_bytes : 0;
	const std::uint64_t sync_discarded_delta = sync_discarded_total >= sync_discarded_bytes_
		? sync_discarded_total - sync_discarded_bytes_ : sync_discarded_total;
	sync_discarded_bytes_ = sync_discarded_total;

	const std::uint64_t cnr_count = cnr_sample_count_.exchange(0, std::memory_order_relaxed);
	const std::int64_t cnr_sum = cnr_sample_sum_.exchange(0, std::memory_order_relaxed);
	const std::int32_t cnr_min = cnr_sample_min_.exchange(
		(std::numeric_limits<std::int32_t>::max)(), std::memory_order_relaxed);
	const std::int32_t cnr_max = cnr_sample_max_.exchange(
		(std::numeric_limits<std::int32_t>::lowest)(), std::memory_order_relaxed);

	std::uint64_t winusb_completed_reads = 0;
	std::uint64_t winusb_completed_bytes = 0;
	std::uint64_t winusb_short_reads = 0;
	std::uint64_t winusb_short_bytes = 0;
	std::uint64_t winusb_read_errors = 0;
	std::uint64_t winusb_read_timeouts = 0;
	std::uint64_t winusb_submit_errors = 0;
	std::uint64_t winusb_wait_timeouts = 0;
	std::uint64_t winusb_wait_errors = 0;
	std::uint32_t winusb_last_error = 0;
#if defined(_WIN32) || defined(_WIN64)
	struct itedtv_usb_stream_stats usb_stats = {};
	const int usb_stats_ret = bus_initialized_
		? itedtv_bus_take_usb_stream_stats(&stream_bus_, &usb_stats) : -ENODEV;
	if (!usb_stats_ret) {
		winusb_completed_reads = usb_stats.completed_reads;
		winusb_completed_bytes = usb_stats.completed_bytes;
		winusb_short_reads = usb_stats.short_reads;
		winusb_short_bytes = usb_stats.short_bytes;
		winusb_read_errors = usb_stats.read_errors;
		winusb_read_timeouts = usb_stats.read_timeouts;
		winusb_submit_errors = usb_stats.submit_errors;
		winusb_wait_timeouts = usb_stats.wait_timeouts;
		winusb_wait_errors = usb_stats.wait_errors;
		winusb_last_error = usb_stats.last_error;
	}
#else
	const int usb_stats_ret = -ENOSYS;
#endif

	const double cnr_average = cnr_count
		? static_cast<double>(cnr_sum) / static_cast<double>(cnr_count) / 1000.0 : 0.0;
	const double cnr_min_db = cnr_count ? static_cast<double>(cnr_min) / 1000.0 : 0.0;
	const double cnr_max_db = cnr_count ? static_cast<double>(cnr_max) / 1000.0 : 0.0;
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "T230C TS stats: host_index=%s elapsed_ms=%llu bytes=%llu packets=%llu cc_errors=%llu missing_mod16=%llu tei=%llu sync_discarded_bytes=%llu\n",
		   dev_.device_name, static_cast<unsigned long long>(elapsed_ms),
		   static_cast<unsigned long long>(stats.transfer_bytes),
		   static_cast<unsigned long long>(stats.input_packets),
		   static_cast<unsigned long long>(stats.cc_errors),
		   static_cast<unsigned long long>(stats.missing_mod16),
		   static_cast<unsigned long long>(stats.tei_packets),
		   static_cast<unsigned long long>(sync_discarded_delta));
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "T230C handler stats: host_index=%s elapsed_ms=%llu reads=%llu bytes=%llu short_reads=%llu short_bytes=%llu write_failures=%llu dropped_bytes=%llu\n",
		   dev_.device_name, static_cast<unsigned long long>(elapsed_ms),
		   static_cast<unsigned long long>(stats.transfer_reads),
		   static_cast<unsigned long long>(stats.transfer_bytes),
		   static_cast<unsigned long long>(stats.short_reads),
		   static_cast<unsigned long long>(stats.short_bytes),
		   static_cast<unsigned long long>(stats.stream_write_failures),
		   static_cast<unsigned long long>(stats.stream_dropped_bytes));
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "T230C WinUSB stats: host_index=%s elapsed_ms=%llu result=%d reads=%llu bytes=%llu short_reads=%llu short_bytes=%llu read_errors=%llu read_timeouts=%llu submit_errors=%llu wait_timeouts=%llu wait_errors=%llu last_error=%u\n",
		   dev_.device_name, static_cast<unsigned long long>(elapsed_ms), usb_stats_ret,
		   static_cast<unsigned long long>(winusb_completed_reads),
		   static_cast<unsigned long long>(winusb_completed_bytes),
		   static_cast<unsigned long long>(winusb_short_reads),
		   static_cast<unsigned long long>(winusb_short_bytes),
		   static_cast<unsigned long long>(winusb_read_errors),
		   static_cast<unsigned long long>(winusb_read_timeouts),
		   static_cast<unsigned long long>(winusb_submit_errors),
		   static_cast<unsigned long long>(winusb_wait_timeouts),
		   static_cast<unsigned long long>(winusb_wait_errors),
		   static_cast<unsigned int>(winusb_last_error));
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "T230C CNR stats: host_index=%s elapsed_ms=%llu samples=%llu avg=%.2f min=%.2f max=%.2f\n",
		   dev_.device_name, static_cast<unsigned long long>(elapsed_ms),
		   static_cast<unsigned long long>(cnr_count), cnr_average, cnr_min_db, cnr_max_db);
	for (const auto &pid : stats.pids) {
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "T230C PID stats: host_index=%s pid=0x%04x cc_errors=%llu missing_mod16=%llu tei=%llu\n",
			   dev_.device_name, static_cast<unsigned int>(pid.pid),
			   static_cast<unsigned long long>(pid.cc_errors),
			   static_cast<unsigned long long>(pid.missing_mod16),
			   static_cast<unsigned long long>(pid.tei_packets));
	}
}

/* 転送層からの失敗通知では上位に知らせるだけで、worker 自身を join しない。 */
int T230CDevice::StreamHandler(void *context, void *data, std::uint32_t size)
{
	auto &device = *static_cast<T230CDevice *>(context);
	if (!data) {
		device.stream_failed_ = true;
		device.buffer_->NotifyStreamFailure();
		device.LogStreamStats(msg_now_ms(), true);
		return 0;
	}
	const bool collect_stats = msg_log_get_stats_interval_ms() &&
		px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	if (collect_stats) {
		device.stream_input_bytes_ += size;
		device.stream_diagnostics_.RecordTransfer(size,
			device.stream_bus_.usb.streaming.urb_buffer_size);
	}
	try {
		device.synchronizer_->Push(static_cast<std::uint8_t *>(data), size,
			[&device](const std::uint8_t *packet) {
				const bool collect = msg_log_get_stats_interval_ms() &&
					px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
				if (collect)
					++device.stream_output_packets_;
				px4::ts::PacketEvent event{};
				if (collect && device.stream_diagnostics_.ObservePacket(packet, msg_now_ms(), event)) {
					if (event.type == px4::ts::PacketEventType::continuity_error) {
						const char *missing = event.missing_count_ambiguous ? "ambiguous" : nullptr;
						if (missing) {
							msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
								   "T230C TS event: host_index=%s type=CC pid=0x%04x previous=%u expected=%u current=%u delta=%u missing_mod16=%s duplicate_payload=%u timestamp_ms=%llu\n",
								   device.dev_.device_name, static_cast<unsigned int>(event.pid),
								   static_cast<unsigned int>(event.previous_cc),
								   static_cast<unsigned int>(event.expected_cc),
								   static_cast<unsigned int>(event.current_cc),
								   static_cast<unsigned int>(event.cc_delta), missing,
								   event.duplicate_payload ? 1U : 0U,
								   static_cast<unsigned long long>(event.timestamp_ms));
						} else {
							msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
								   "T230C TS event: host_index=%s type=CC pid=0x%04x previous=%u expected=%u current=%u delta=%u missing_mod16=%u duplicate_payload=%u timestamp_ms=%llu\n",
								   device.dev_.device_name, static_cast<unsigned int>(event.pid),
								   static_cast<unsigned int>(event.previous_cc),
								   static_cast<unsigned int>(event.expected_cc),
								   static_cast<unsigned int>(event.current_cc),
								   static_cast<unsigned int>(event.cc_delta),
								   static_cast<unsigned int>(event.missing_mod16),
								   event.duplicate_payload ? 1U : 0U,
								   static_cast<unsigned long long>(event.timestamp_ms));
						}
					} else {
						msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
							   "T230C TS event: host_index=%s type=TEI pid=0x%04x cc=%u timestamp_ms=%llu\n",
							   device.dev_.device_name, static_cast<unsigned int>(event.pid),
							   static_cast<unsigned int>(event.current_cc),
							   static_cast<unsigned long long>(event.timestamp_ms));
					}
				}
				std::size_t length = 188;
				const bool written = device.buffer_->Write(packet, length);
				if (collect)
					device.stream_diagnostics_.RecordWrite(written, 188, length);
			});
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	}
	device.buffer_->NotifyWrite();
	device.LogStreamStats(msg_now_ms(), false);
	return 0;
}

/* 受信機はデバイスと同じ寿命を持ち、開閉を単一ロックで管理する。 */
T230CDevice::Receiver::Receiver(T230CDevice &parent)
	: ReceiverBase(0), parent_(parent)
{
}

/* 利用中の接続が Close するまで参照先の解放を待つ。 */
T230CDevice::Receiver::~Receiver()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.mutex_);
	close_cond_.wait(lock, [this] { return !parent_.receiver_open_; });
}

/* 1 基の受信機を複数接続から同時に開かない。 */
int T230CDevice::Receiver::Open()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.initialized_)
		return -ENODEV;
	if (parent_.receiver_open_)
		return -EALREADY;
	parent_.receiver_open_ = true;
	return 0;
}

/* 配信を止めてから開放を通知し、終了処理との寿命をそろえる。 */
void T230CDevice::Receiver::Close()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.receiver_open_)
		return;
	SetCapture(false);
	parent_.receiver_open_ = false;
	lock.unlock();
	close_cond_.notify_all();
}

/* CATV 専用機に LNB 給電機能はなく、OFF のみ受け付ける。 */
int T230CDevice::Receiver::SetLnbVoltage(std::int32_t voltage)
{
	return voltage == 0 ? 0 : -ENOSYS;
}

/* ISDB-S のストリーム ID 指定は対象外。 */
int T230CDevice::Receiver::SetStreamId()
{
	return -ENOSYS;
}

/* 要求の開始から選局コマンド、ロック確認、受信開始まで同じ期限を使う。 */
bool T230CDevice::Receiver::TuneOverride(std::uint32_t timeout, bool &result)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	const auto started = std::chrono::steady_clock::now();
	parent_.deadline_ = started + std::chrono::milliseconds(timeout);
	result = false;
	/* 失敗した段階・戻り値・経過時間を記録し、ロック待ちの期限切れと
	 * 制御エラーを実機ログで区別できるようにする。 */
	const char *stage = "prepare";
	int ret = PrepareTune();
	bool locked = false;
	if (!ret) {
		stage = "set_frequency";
		ret = SetFrequency();
	}
	if (!ret) {
		stage = "lock";
		while (std::chrono::steady_clock::now() < parent_.deadline_) {
			ret = CheckLock(locked);
			if (ret || locked)
				break;
			auto remaining = parent_.deadline_ - std::chrono::steady_clock::now();
			if (remaining > std::chrono::steady_clock::duration::zero())
				std::this_thread::sleep_for((std::min)(remaining, std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(20))));
		}
		ret = t230c::ResolveTuneLockResult(ret, locked);
		result = ret == 0;
	}
	if (result) {
		/* ロックより前に届いた TS で判断しないよう、ロック後に TSMF の解析をやり直し、
		 * 指定した TS が見つかるか、ないと分かるまで同じ期限で待つ。 */
		stage = "tsmf";
		stream_buf_->RestartTsmf();
		result = false;
		while (std::chrono::steady_clock::now() < parent_.deadline_) {
			const tsmf_filter_state state = stream_buf_->GetTsmfState();
			if (state != TSMF_FILTER_SEARCHING) {
				result = state == TSMF_FILTER_FOUND;
				if (!result)
					ret = -ENODATA;
				break;
			}
			auto remaining = parent_.deadline_ - std::chrono::steady_clock::now();
			if (remaining > std::chrono::steady_clock::duration::zero())
				std::this_thread::sleep_for((std::min)(remaining, std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(20))));
		}
		if (!result && !ret)
			ret = -ETIMEDOUT;
	}
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	if (result)
		msg_info("T230C tune: freq=%u kHz locked in %lld ms\n", params_.freq, static_cast<long long>(elapsed_ms));
	else
		msg_err("T230C tune: freq=%u kHz failed at %s (ret=%d, locked=%d) after %lld ms\n",
			params_.freq, stage, ret, locked ? 1 : 0, static_cast<long long>(elapsed_ms));
	if (!result && parent_.streaming_) {
		/* 期限切れ後も USB 転送と FX2 の出力を回収する。 */
		parent_.deadline_ = (t230c::Deadline::max)();
		StopUsbStream();
	}
	parent_.deadline_ = (t230c::Deadline::max)();
	return true;
}

/* 未確認の ISDB-C 経路は設定で明示的に有効化された場合だけ選局する。 */
int T230CDevice::Receiver::PrepareTune()
{
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if ((params_.system != SystemType::ISDB_C && params_.system != SystemType::J83B) || !parent_.experimental_isdbc_)
		return -ENOSYS;
	if (params_.bandwidth && params_.bandwidth != 6)
		return -EINVAL;
	return 0;
}

/* 前のチャンネルの端数と配信バッファーを破棄してから選局する。 */
int T230CDevice::Receiver::SetFrequency()
{
	int ret = StopUsbStream();
	if (ret)
		return ret;
	if (!stream_buf_->Purge(parent_.deadline_))
		return -EIO;
	/* J.83 Annex B は TSMF を使わないため、分離を止めて TS をそのまま通す。
	 * ISDB-C の TSMF は共通の TSMF フィルターで選ぶ。ヘッダーのない搬送波は TS0 の指定でそのまま出力する。
	 * 選局前に USB 転送を止めて配信バッファーを破棄しているため、前の搬送波の TS を捨てる数は 0 にする。 */
	const bool j83b = params_.system == SystemType::J83B;
	if (j83b) {
		stream_buf_->StopTsmf();
	} else if (!stream_buf_->StartTsmf(params_.tsmf_stream, false, 0)) {
		return -ENOMEM;
	}
	/* 選局要求にシンボルレートは含まれないため、ISDB-C は 5.274 Msps、J.83 Annex B は 5.6 Msps を使う。 */
	const std::uint32_t symbol_rate = j83b ? 5600 : 5274;
	if (!parent_.silicon_)
		return -ENODEV;
	ret = parent_.silicon_->Tune(params_.freq, symbol_rate, 6, parent_.deadline_, j83b);
	/* ファームウェア読み込み後の最初の選局だけ、コマンド応答待ちが期限切れになる
	 * (実機で毎回の初回のみ -ETIMEDOUT、2回目以降は約 190 ms でロック)。
	 * 選局コマンドは再送しても同じ状態になるため、TUNE の残り時間内で1回だけ送り直す。 */
	if (ret == -ETIMEDOUT && std::chrono::steady_clock::now() < parent_.deadline_) {
		msg_info("T230C tune: freq=%u kHz command timed out; retrying once\n", params_.freq);
		ret = parent_.silicon_->Tune(params_.freq, symbol_rate, 6, parent_.deadline_, j83b);
	}
	return ret;
}

/* 復調器の TS ロック確認後にだけ USB 配信を始める。 */
int T230CDevice::Receiver::CheckLock(bool &locked)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	locked = false;
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	std::int32_t cnr = 0;
	int ret = parent_.silicon_->ReadLock(locked, cnr, parent_.deadline_);
	/* 選局で止めた USB 転送を、クライアントが受信中のときだけロック後に再開する。 */
	if (!ret && locked && buffer_started_)
		ret = StartUsbStream();
	return ret;
}

/* CNR はロック時にのみ有効で、他の統計は未対応として返す。 */
int T230CDevice::Receiver::ReadStat(command::StatType type, std::int32_t &value)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (type != command::StatType::CNR)
		return -ENOSYS;
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	bool locked = false;
	int ret = parent_.silicon_->ReadLock(locked, value, parent_.deadline_);
	if (!ret && locked)
		parent_.RecordCnr(value);
	return ret ? ret : (locked ? 0 : -EAGAIN);
}

/* FX2 の TS 出力と USB 転送だけを止める。配信バッファーは止めない。
 * StreamBuffer::Stop() は DriverHost の配信接続を終わらせ、同じ接続では再開できないため、
 * 選局の途中で呼ぶと以後の TS がクライアントへ届かなくなる (2026-10 実機で確認)。 */
int T230CDevice::Receiver::StopUsbStream()
{
	int ret = 0;
	if (parent_.streaming_ && parent_.available_) {
		const std::uint8_t stop[] = {0x37, 0, 0};
		ret = parent_.TransferOut(stop, sizeof(stop));
	}
	if (parent_.bus_initialized_ && parent_.streaming_)
		itedtv_bus_stop_streaming(&parent_.stream_bus_);
	parent_.streaming_ = false;
	parent_.LogStreamStats(msg_now_ms(), true);
	if (parent_.synchronizer_)
		parent_.synchronizer_->Reset();
	return ret;
}

/* 全転送を投入してから FX2 の出力を開始する (dvbsky.c と同じ 0x37 → 20 ms → 0x36 の順)。 */
int T230CDevice::Receiver::StartUsbStream()
{
	if (parent_.streaming_)
		return parent_.stream_failed_ ? -EIO : 0;
	parent_.synchronizer_->Reset();
	if (msg_log_get_stats_interval_ms() && px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS)) {
		parent_.stream_diagnostics_.Reset();
		parent_.stream_input_bytes_ = 0;
		parent_.stream_output_packets_ = 0;
		parent_.sync_discarded_bytes_ = 0;
	}
	parent_.stats_report_ms_ = msg_now_ms();
	parent_.ResetCnrStats();
#if defined(_WIN32) || defined(_WIN64)
	if (parent_.bus_initialized_) {
		struct itedtv_usb_stream_stats discarded_stats = {};
		(void)itedtv_bus_take_usb_stream_stats(&parent_.stream_bus_, &discarded_stats);
	}
#endif
	parent_.stream_failed_ = false;
	const std::uint8_t stop[] = {0x37, 0, 0};
	int ret = parent_.TransferOut(stop, sizeof(stop));
	if (ret)
		return ret;
	parent_.stream_bus_.usb.streaming.urb_buffer_size = parent_.xfer_packets_ * 188;
	parent_.stream_bus_.usb.streaming.urb_num = parent_.max_urbs_;
	parent_.stream_bus_.usb.ctrl_timeout = parent_.ControlTimeout();
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_DEVICE,
		   "T230C stream config: host_index=%s endpoint=0x%02x urbs=%u urb_bytes=%u packets_per_urb=%u no_raw_io=%u max_bulk_size=%u receiver_packets=%u\n",
		   parent_.dev_.device_name, parent_.stream_bus_.usb.streaming.endpoint,
		   parent_.stream_bus_.usb.streaming.urb_num,
		   parent_.stream_bus_.usb.streaming.urb_buffer_size,
		   parent_.xfer_packets_,
		   parent_.stream_bus_.usb.streaming.no_raw_io ? 1U : 0U,
		   parent_.stream_bus_.usb.max_bulk_size,
		   parent_.receiver_packets_);
	if (!parent_.stream_bus_.usb.ctrl_timeout)
		return -ETIMEDOUT;
	ret = itedtv_bus_start_streaming(&parent_.stream_bus_, StreamHandler, &parent_);
	if (ret)
		return ret;
	parent_.streaming_ = true;
	if (parent_.deadline_ != (t230c::Deadline::max)() &&
		std::chrono::steady_clock::now() + std::chrono::milliseconds(20) >= parent_.deadline_)
		ret = -ETIMEDOUT;
	if (!ret) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const std::uint8_t start[] = {0x36, 3, 0};
		ret = parent_.TransferOut(start, sizeof(start));
	}
	if (ret || parent_.stream_failed_) {
		itedtv_bus_stop_streaming(&parent_.stream_bus_);
		parent_.streaming_ = false;
		return ret ? ret : -EIO;
	}
	return 0;
}

/* 配信バッファーの開始・停止はクライアントの SET_CAPTURE だけで行う。 */
int T230CDevice::Receiver::SetCapture(bool capture)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!capture) {
		int ret = StopUsbStream();
		stream_buf_->Stop();
		buffer_started_ = false;
		return ret;
	}
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if (!buffer_started_) {
		if (!stream_buf_->Alloc(static_cast<std::size_t>(188) * parent_.receiver_packets_))
			return -ENOMEM;
		stream_buf_->Start();
		buffer_started_ = true;
	}
	int ret = StartUsbStream();
	if (ret) {
		stream_buf_->Stop();
		buffer_started_ = false;
	}
	return ret;
}

} // namespace px4
