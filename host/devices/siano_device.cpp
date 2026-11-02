// SPDX-License-Identifier: GPL-2.0-or-later
#include "siano_device.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libusb.h>
#endif

#include "msg.h"
#include "util.hpp"

namespace px4 {

class SianoDevice::BusTransport final : public siano::Transport {
public:
	explicit BusTransport(SianoDevice &device) noexcept : device_(device) {}
	int Send(const std::uint8_t *message, std::size_t length,
		 std::uint32_t timeout_ms) noexcept override
	{
		return device_.SendControl(message, length, timeout_ms);
	}

private:
	SianoDevice &device_;
};

class SianoDevice::Receiver final : public ReceiverBase {
public:
	explicit Receiver(SianoDevice &parent)
		: ReceiverBase(0), parent_(parent)
	{
	}
	~Receiver() override;
	int Open() override;
	void Close() override;
	int CheckLock(bool &locked) override;
	int SetLnbVoltage(std::int32_t voltage) override;
	int SetCapture(bool capture) override;
	int ReadStat(command::StatType type, std::int32_t &value) override;

protected:
	bool TuneOverride(std::uint32_t timeout, bool &result) override;
	int PrepareTune() override;
	int SetFrequency() override;
	int SetStreamId() override;

private:
	SianoDevice &parent_;
	bool buffer_started_ = false;
};

SianoDevice::SianoDevice(const std::wstring &path, const DeviceDefinition &definition,
			 std::uintptr_t index, ReceiverManager &receiver_manager)
	: DeviceBase(path, definition, index, receiver_manager)
{
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_SERVER,
		   "Siano RIO registration: host_index=%s path=%ls\n", dev_.device_name,
		   path.c_str());
	if (definition.receivers.size() != 1 || definition.receivers[0].index != 0 ||
	    definition.receivers[0].systems != SystemType::ISDB_T)
		throw DeviceError("Siano RIO requires exactly one ISDB-T receiver.");
}

SianoDevice::~SianoDevice()
{
	Term();
}

int SianoDevice::FindStreamEndpoint(std::uint8_t &endpoint,
				    std::size_t &response_alignment) const
{
	endpoint = 0;
	response_alignment = 0;
	bool has_control_out = false;
#ifdef _WIN32
	USB_INTERFACE_DESCRIPTOR interface_descriptor = {};
	if (!WinUsb_QueryInterfaceSettings(usb_dev_.winusb, 0, &interface_descriptor))
		return -EIO;
	/* SMS USB は interface 0 の bulk IN を TS と応答の共通経路に使う。
	 * 制御用の bulk OUT 0x02 が IN より後に並ぶ記述子もあるため、最初の IN で打ち切らずに全 endpoint を調べる。 */
	for (UCHAR index = 0; index < interface_descriptor.bNumEndpoints; ++index) {
		WINUSB_PIPE_INFORMATION pipe = {};
		if (!WinUsb_QueryPipe(usb_dev_.winusb, 0, index, &pipe))
			return -EIO;
		if (pipe.PipeType != UsbdPipeTypeBulk)
			continue;
		if (pipe.PipeId == 0x02)
			has_control_out = true;
		if (!(pipe.PipeId & 0x80) || endpoint)
			continue;
		endpoint = pipe.PipeId;
		response_alignment = pipe.MaximumPacketSize > 8
			? static_cast<std::size_t>(pipe.MaximumPacketSize - 8) : 0;
	}
#elif defined(__APPLE__)
	libusb_device *usb_device = libusb_get_device(usb_dev_.handle);
	libusb_config_descriptor *config = nullptr;
	int ret = libusb_get_active_config_descriptor(usb_device, &config);
	if (ret != LIBUSB_SUCCESS)
		return -EIO;
	for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
		const auto &interface = config->interface[i];
		if (!interface.num_altsetting || interface.altsetting[0].bInterfaceNumber != 0)
			continue;
		/* Linux upstream smsusb と同じく alternate setting 0 の endpoint を使う。 */
		const auto &setting = interface.altsetting[0];
		for (std::uint8_t e = 0; e < setting.bNumEndpoints; ++e) {
			const auto &desc = setting.endpoint[e];
			if ((desc.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
				continue;
			if (desc.bEndpointAddress == 0x02)
				has_control_out = true;
			if (!(desc.bEndpointAddress & LIBUSB_ENDPOINT_IN) || endpoint)
				continue;
			endpoint = desc.bEndpointAddress;
			const auto max_packet = desc.wMaxPacketSize & 0x07ff;
			response_alignment = max_packet > 8 ? max_packet - 8 : 0;
		}
	}
	libusb_free_config_descriptor(config);
#else
	return -ENOTSUP;
#endif
	/* upstream smsusb は control OUT を endpoint 0x02 固定で送るため、descriptor との一致を確認する。 */
	return endpoint && has_control_out ? 0 : -ENODEV;
}

int SianoDevice::Init()
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (initialized_)
		return -EALREADY;

	const auto &config = device_def_.configs;
	const unsigned int configured_packets = util::wtoui(
		config.Get(L"ReceiverMaxPackets", L"16384"));
	if (!configured_packets || configured_packets >
	    (std::numeric_limits<std::uint32_t>::max)() / 188U)
		throw DeviceError("Siano RIO: invalid receiver buffer size.");
	receiver_packets_ = configured_packets;

	std::filesystem::path firmware = config.Get(L"SianoFirmware", L"isdbt_rio.inp");
	std::filesystem::path executable_directory;
#ifdef _WIN32
	wchar_t executable_path[32768];
	const DWORD executable_length = GetModuleFileNameW(nullptr, executable_path, 32768);
	if (executable_length && executable_length < 32768)
		executable_directory = std::filesystem::path(executable_path).parent_path();
#elif defined(__APPLE__)
	char executable_path[4096];
	uint32_t executable_length = sizeof(executable_path);
	if (_NSGetExecutablePath(executable_path, &executable_length) == 0)
		executable_directory = std::filesystem::path(executable_path).parent_path();
#endif
	if (!executable_directory.empty() && firmware.is_relative())
		firmware = executable_directory / firmware;
	firmware_path_ = std::move(firmware);

	/* デバイス一覧のロック下で長いファームウェア転送を行わないよう、ここでは受信機登録までに留める。 */
	const int endpoint_ret = FindStreamEndpoint(stream_endpoint_, response_alignment_);
	if (endpoint_ret)
		throw DeviceError("Siano RIO: cannot find a bulk IN endpoint.");

	stream_bus_.dev = &dev_;
	stream_bus_.type = ITEDTV_BUS_USB;
	stream_bus_.usb.dev = &usb_dev_;
	stream_bus_.usb.ctrl_timeout = 10000;
	stream_bus_.usb.streaming.urb_buffer_size = 0x2000;
	stream_bus_.usb.streaming.urb_num = 8;
	stream_bus_.usb.streaming.endpoint = stream_endpoint_;
	stream_bus_.usb.streaming.fail_on_error = true;
	const int bus_ret = itedtv_bus_init(&stream_bus_);
	if (bus_ret)
		throw DeviceError("Siano RIO: USB transport initialization failed.");
	bus_initialized_ = true;

	receiver_.reset(new Receiver(*this));
	buffer_ = receiver_->GetStreamBuffer();
	transport_.reset(new BusTransport(*this));
	protocol_.reset(new siano::Protocol(*transport_, response_alignment_,
		[this](const std::uint8_t *data, std::size_t size) {
			if (!capture_enabled_ || !buffer_)
				return;
			std::size_t output_size = size;
			if (buffer_->Write(data, output_size))
				buffer_->NotifyWrite();
		}));

	command::ReceiverInfo info = {};
	wcscpy_s(info.device_name, device_def_.name.c_str());
	info.device_guid = device_def_.guid;
	wcscpy_s(info.receiver_name, device_def_.receivers[0].name.c_str());
	info.receiver_guid = device_def_.receivers[0].guid;
	info.systems = device_def_.receivers[0].systems;
	info.index = device_def_.receivers[0].index;
	if (!receiver_manager_.Register(info, receiver_.get()))
		throw DeviceError("Siano RIO: receiver registration failed.");
	initialized_ = true;
	return 0;
}

void SianoDevice::Term()
{
	if (initialized_)
		receiver_manager_.Unregister(receiver_.get());
	SetAvailability(false);
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (streaming_ && bus_initialized_)
			itedtv_bus_stop_streaming(&stream_bus_);
		streaming_ = false;
	}
	if (protocol_)
		protocol_->Disconnect();
	protocol_.reset();
	transport_.reset();
	receiver_.reset();
	buffer_.reset();
	if (bus_initialized_) {
		itedtv_bus_term(&stream_bus_);
		bus_initialized_ = false;
	}
	initialized_ = false;
}

void SianoDevice::SetAvailability(bool available)
{
	available_ = available;
	if (!available) {
		if (protocol_)
			protocol_->Disconnect();
		if (buffer_)
			buffer_->NotifyStreamFailure();
	}
}

ReceiverBase *SianoDevice::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("Siano RIO receiver id out of range");
	return receiver_.get();
}

int SianoDevice::StartUsbStream()
{
	if (!available_ || !bus_initialized_ || !protocol_)
		return -ENODEV;
	if (stream_failed_)
		return -EIO;
	if (streaming_)
		return 0;
	stream_failed_ = false;
	/* RIO は制御応答も TS も同じ bulk IN で返すため、選局中も非同期読出しを保つ。 */
	const int ret = itedtv_bus_start_streaming(&stream_bus_, StreamHandler, this);
	if (!ret)
		streaming_ = true;
	return ret;
}

int SianoDevice::StopUsbStream()
{
	if (!streaming_ || !bus_initialized_)
		return 0;
	const int ret = itedtv_bus_stop_streaming(&stream_bus_);
	streaming_ = false;
	return ret;
}

int SianoDevice::EnsureProtocolInitialized(std::chrono::milliseconds timeout)
{
	if (!available_ || !protocol_)
		return -ENODEV;
	if (protocol_initialized_)
		return 0;
	if (timeout <= std::chrono::milliseconds::zero())
		return -ETIMEDOUT;
	int ret = StartUsbStream();
	if (ret) {
		msg_err("Siano RIO: StartUsbStream during initialization failed (ret: %d)\n", ret);
		return ret;
	}
	/* Firmware transfer runs after receiver registration, outside the global device-list lock. */
	ret = protocol_->Initialize(firmware_path_, timeout);
	if (ret) {
		msg_err("Siano RIO: protocol initialization failed (ret: %d)\n", ret);
		StopUsbStream();
		if (ret != -ENOENT && ret != -EACCES)
			protocol_->Disconnect();
		return ret;
	}
	msg_dbg("Siano RIO: protocol initialization succeeded\n");
	protocol_initialized_ = true;
	return 0;
}

int SianoDevice::SendControl(const std::uint8_t *message, std::size_t length,
			     std::uint32_t timeout_ms) noexcept
{
	if (!available_ || !bus_initialized_)
		return -ENODEV;
	if (!message || !length ||
	    length > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
	    !timeout_ms)
		return -EINVAL;
	stream_bus_.usb.ctrl_timeout = static_cast<int>((std::min<std::uint32_t>(
		timeout_ms, static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))));
	return itedtv_bus_ctrl_tx(&stream_bus_, const_cast<std::uint8_t *>(message),
			  static_cast<int>(length));
}

int SianoDevice::StreamHandler(void *context, void *data, std::uint32_t size)
{
	auto &device = *static_cast<SianoDevice *>(context);
	if (!data) {
		msg_err("Siano RIO: USB stream ended unexpectedly; disconnecting protocol.\n");
		device.stream_failed_ = true;
		if (device.protocol_)
			device.protocol_->Disconnect();
		if (device.buffer_)
			device.buffer_->NotifyStreamFailure();
		return 0;
	}
	if (!device.protocol_)
		return -ENODEV;
	/* USB の空転送は Siano フレームではないため、受信状態を壊さず読み捨てる。 */
	if (size == 0)
		return 0;
	/* USB 転送単位を Siano メッセージへ分解し、BDA data だけを TS バッファーへ渡す。 */
	const int ret = device.protocol_->HandleTransfer(
		static_cast<const std::uint8_t *>(data), size);
	if (ret) {
		msg_err("Siano RIO: message decode failed (ret: %d, size: %u).\n", ret, size);
		device.stream_failed_ = true;
		device.protocol_->Disconnect();
		if (device.buffer_)
			device.buffer_->NotifyStreamFailure();
	}
	return ret;
}

SianoDevice::Receiver::~Receiver()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.mutex_);
	parent_.close_cond_.wait(lock, [this] { return !parent_.receiver_open_; });
}

int SianoDevice::Receiver::Open()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.initialized_ || !parent_.protocol_)
		return -ENODEV;
	if (parent_.receiver_open_)
		return -EALREADY;
	parent_.receiver_open_ = true;
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_SERVER,
		   "Siano receiver Open: host_index=%s\n", parent_.dev_.device_name);
	return 0;
}

void SianoDevice::Receiver::Close()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.receiver_open_)
		return;
	SetCapture(false);
	parent_.receiver_open_ = false;
	parent_.StopUsbStream();
	lock.unlock();
	parent_.close_cond_.notify_all();
}

int SianoDevice::Receiver::CheckLock(bool &locked)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.receiver_open_ || !parent_.protocol_)
		return -ENODEV;
	locked = parent_.protocol_->IsLocked();
	return 0;
}

int SianoDevice::Receiver::SetLnbVoltage(std::int32_t voltage)
{
	return voltage == 0 ? 0 : -ENOSYS;
}

int SianoDevice::Receiver::SetCapture(bool capture)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!capture) {
		parent_.capture_enabled_ = false;
		if (buffer_started_) {
			stream_buf_->Stop();
			buffer_started_ = false;
		}
		return parent_.StopUsbStream();
	}
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	int ret = parent_.EnsureProtocolInitialized();
	if (ret)
		return ret;
	if (!buffer_started_) {
		if (!stream_buf_->Alloc(188U * parent_.receiver_packets_)) {
			parent_.StopUsbStream();
			return -ENOMEM;
		}
		stream_buf_->Start();
		buffer_started_ = true;
	}
	ret = parent_.StartUsbStream();
	if (ret) {
		parent_.capture_enabled_ = false;
		stream_buf_->Stop();
		buffer_started_ = false;
		return ret;
	}
	parent_.capture_enabled_ = true;
	return 0;
}

int SianoDevice::Receiver::ReadStat(command::StatType type, std::int32_t &value)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	value = 0;
	if (!parent_.available_ || !parent_.receiver_open_ || !parent_.protocol_)
		return -ENODEV;
	if (type == command::StatType::SIGNAL_STRENGTH)
		return -ENOSYS;
	if (type != command::StatType::CNR)
		return -ENOSYS;
	return parent_.protocol_->ReadCnrMilliDb(value);
}

bool SianoDevice::Receiver::TuneOverride(std::uint32_t timeout, bool &result)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	result = false;
	if (PrepareTune())
		return true;
	if (timeout == 0)
		return true;
	const std::uint64_t frequency_hz = static_cast<std::uint64_t>(params_.freq) * 1000U;
	if (frequency_hz > (std::numeric_limits<std::uint32_t>::max)())
		return true;
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeout);
	const auto init_remaining = deadline - std::chrono::steady_clock::now();
	if (init_remaining <= std::chrono::steady_clock::duration::zero())
		return true;
	const auto init_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(init_remaining);
	int ret = parent_.EnsureProtocolInitialized(init_timeout);
	if (ret)
		return true;
	/* 直前チャンネルの転送が選局後へ混ざらないよう、選局中は TS を破棄する。 */
	const bool was_capturing = parent_.capture_enabled_;
	parent_.capture_enabled_ = false;
	if (buffer_started_ && !stream_buf_->Purge(deadline)) {
		parent_.capture_enabled_ = was_capturing;
		return true;
	}
	ret = parent_.StartUsbStream();
	if (ret)
		return true;
	const auto remaining = deadline - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero()) {
		parent_.capture_enabled_ = was_capturing;
		return true;
	}
	const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_TUNER,
		   "Siano tune start: host_index=%s freq_khz=%u\n",
		   parent_.dev_.device_name, static_cast<unsigned int>(params_.freq));
	ret = parent_.protocol_->TuneFrequencyHz(static_cast<std::uint32_t>(frequency_hz),
						 std::chrono::milliseconds((std::max<std::int64_t>)(1, remaining_ms)));
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_TUNER,
		   "Siano tune result: host_index=%s freq_khz=%u ret=%d\n",
		   parent_.dev_.device_name, static_cast<unsigned int>(params_.freq), ret);
	result = ret == 0;
	if (result && was_capturing) {
		parent_.capture_enabled_ = true;
	} else {
		parent_.capture_enabled_ = false;
		parent_.StopUsbStream();
	}
	return true;
}

int SianoDevice::Receiver::PrepareTune()
{
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if (params_.system != SystemType::ISDB_T)
		return -ENOSYS;
	if (params_.bandwidth && params_.bandwidth != 6)
		return -EINVAL;
	if (!params_.freq)
		return -EINVAL;
	return 0;
}

int SianoDevice::Receiver::SetFrequency()
{
	/* 選局要求全体の絶対期限を使う TuneOverride 以外からは呼び出さない。 */
	return -ENOSYS;
}

int SianoDevice::Receiver::SetStreamId()
{
	return -ENOSYS;
}

} // namespace px4
