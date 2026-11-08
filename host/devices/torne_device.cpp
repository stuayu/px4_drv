// SPDX-License-Identifier: GPL-2.0-only

#include "torne_device.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <new>
#include <thread>

#ifdef __APPLE__
#include <libusb.h>
#endif

#include "msg.h"
#include "util.hpp"

namespace px4 {
namespace {
	constexpr unsigned int kControlTimeoutMs = 1000;
	constexpr unsigned int kStreamBufferSize = 1536 * 32;
	constexpr std::uint8_t kStreamEndpoint = 0x81;
}

class TorneDevice::Receiver final : public ReceiverBase {
public:
	explicit Receiver(TorneDevice &parent) : ReceiverBase(0), parent_(parent) {}
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
	TorneDevice &parent_;
};

TorneDevice::TorneDevice(const std::wstring &path,
			 const DeviceDefinition &definition,
			 std::uintptr_t index,
			 ReceiverManager &receiver_manager)
	: DeviceBase(path, definition, index, receiver_manager)
{
	if (definition.receivers.size() != 1 || definition.receivers[0].index != 0 ||
	    definition.receivers[0].systems != SystemType::ISDB_T)
		throw DeviceError("torne requires exactly one ISDB-T receiver.");
}

TorneDevice::~TorneDevice()
{
	Term();
}

int TorneDevice::Init()
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (initialized_)
		return -EALREADY;
	const auto &config = device_def_.configs;
	const unsigned int packets = util::wtoui(config.Get(L"ReceiverMaxPackets", L"16384"));
	max_urbs_ = util::wtoui(config.Get(L"TorneMaxUrbs", L"8"));
	if (!packets || packets > (std::numeric_limits<std::uint32_t>::max)() / 188U ||
	    !max_urbs_ || max_urbs_ > 64)
		return -EINVAL;
	receiver_packets_ = packets;

	static const struct torne_ops ops = {
		&TorneDevice::ControlCallback,
		&TorneDevice::BulkCallback,
		&TorneDevice::SleepCallback,
		&TorneDevice::NowCallback,
		&TorneDevice::LogCallback,
	};
	torne_bridge_init(&bridge_, &ops, this);
	int ret = torne_bridge_start(&bridge_);
	if (ret) {
		msg_err("torne bridge start failed: %d\n", ret);
		return ret;
	}
	bridge_started_ = true;
	ret = torne_bridge_reset_demodulator(&bridge_);
	if (ret) {
		msg_err("torne demodulator reset failed: %d\n", ret);
		return ret;
	}
	torne_dib_setup(&dib_, &bridge_);
	ret = torne_dib_init(&dib_);
	if (ret) {
		msg_err("torne demodulator initialization failed: %d\n", ret);
		return ret;
	}
	demod_initialized_ = true;
	ret = torne_dib_set_output(&dib_, TORNE_OUT_HIGHZ);
	if (ret)
		return ret;

	stream_bus_.dev = &dev_;
	stream_bus_.type = ITEDTV_BUS_USB;
	stream_bus_.usb.dev = &usb_dev_;
	stream_bus_.usb.ctrl_timeout = kControlTimeoutMs;
	stream_bus_.usb.streaming.endpoint = kStreamEndpoint;
	stream_bus_.usb.streaming.urb_buffer_size = kStreamBufferSize;
	stream_bus_.usb.streaming.urb_num = max_urbs_;
	stream_bus_.usb.streaming.fail_on_error = true;
	ret = itedtv_bus_init(&stream_bus_);
	if (ret)
		return ret;
	bus_initialized_ = true;

	try {
		receiver_.reset(new Receiver(*this));
		buffer_ = receiver_->GetStreamBuffer();
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	}
	if (!buffer_->Alloc(static_cast<std::size_t>(receiver_packets_) * 188U))
		return -ENOMEM;
	const auto &definition = device_def_.receivers.front();
	command::ReceiverInfo info = {};
	wcscpy_s(info.device_name, device_def_.name.c_str());
	info.device_guid = device_def_.guid;
	wcscpy_s(info.receiver_name, definition.name.c_str());
	info.receiver_guid = definition.guid;
	info.systems = definition.systems;
	info.index = definition.index;
	if (!receiver_manager_.Register(info, receiver_.get()))
		return -EEXIST;
	initialized_ = true;
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_DEVICE,
		   "torne ready: host_index=%s usb_serial=%ls\n",
		   dev_.device_name, usb_serial_number_.c_str());
	return 0;
}

void TorneDevice::Term()
{
	if (initialized_.exchange(false) && receiver_)
		receiver_manager_.Unregister(receiver_.get());
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (bus_initialized_ && streaming_)
			itedtv_bus_stop_streaming(&stream_bus_);
		streaming_ = false;
		if (available_) {
			if (bridge_started_) {
				torne_bridge_stop_stream(&bridge_);
				torne_dib_set_output(&dib_, TORNE_OUT_HIGHZ);
			}
			if (demod_initialized_) {
				torne_dib_set_gpio(&dib_, 1, false);
				torne_dib_sleep(&dib_);
			}
		}
		if (buffer_started_ && buffer_)
			buffer_->Stop();
		buffer_started_ = false;
		receiver_open_ = false;
	}
	SetAvailability(false);
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (bus_initialized_) {
			itedtv_bus_term(&stream_bus_);
			bus_initialized_ = false;
		}
		receiver_.reset();
		buffer_.reset();
		bridge_started_ = false;
		demod_initialized_ = false;
		buffer_started_ = false;
	}
}

void TorneDevice::SetAvailability(bool available)
{
	available_ = available;
	if (!available && buffer_)
		buffer_->NotifyStreamFailure();
}

ReceiverBase *TorneDevice::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("receiver id out of range");
	return receiver_.get();
}

unsigned int TorneDevice::ControlTimeout() const noexcept
{
	if (deadline_ == (std::chrono::steady_clock::time_point::max)())
		return kControlTimeoutMs;
	const auto remaining = deadline_ - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero())
		return 0;
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
	return static_cast<unsigned int>(std::min<std::int64_t>(kControlTimeoutMs,
		(std::max<std::int64_t>)(1, milliseconds)));
}

int TorneDevice::ControlCallback(void *context, u8 request_type, u16 value,
				 u16 index, void *buffer, u16 length)
{
	return static_cast<TorneDevice *>(context)->Control(request_type, value,
							    index, buffer, length);
}

int TorneDevice::Control(u8 request_type, u16 value, u16 index,
			 void *buffer, u16 length)
{
	if (!available_)
		return -ENODEV;
	if (length > 64 || (length && !buffer))
		return -EINVAL;
	const unsigned int timeout = ControlTimeout();
	if (!timeout)
		return -ETIMEDOUT;
#ifdef __APPLE__
	const int ret = libusb_control_transfer(usb_dev_.handle, request_type, 0xbc,
		value, index, static_cast<unsigned char *>(buffer), length, timeout);
	if (ret < 0) {
		if (ret == LIBUSB_ERROR_TIMEOUT)
			return -ETIMEDOUT;
		if (ret == LIBUSB_ERROR_NO_DEVICE)
			return -ENODEV;
		if (ret == LIBUSB_ERROR_PIPE)
			return -EPIPE;
		return -EIO;
	}
	return ret;
#else
	WINUSB_SETUP_PACKET setup = {};
	setup.RequestType = request_type;
	setup.Request = 0xbc;
	setup.Value = value;
	setup.Index = index;
	ULONG transferred = 0;
	if (!WinUsb_ControlTransfer(usb_dev_.winusb, setup,
				    static_cast<PUCHAR>(buffer), length,
				    &transferred, nullptr)) {
		const DWORD error = GetLastError();
		if (error == ERROR_SEM_TIMEOUT)
			return -ETIMEDOUT;
		if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_NO_SUCH_DEVICE)
			return -ENODEV;
		if (error == ERROR_OPERATION_ABORTED)
			return -ECANCELED;
		return -EIO;
	}
	return static_cast<int>(transferred);
#endif
}

int TorneDevice::BulkCallback(void *context, u8 endpoint, void *buffer,
			      int length, unsigned int timeout_ms)
{
	return static_cast<TorneDevice *>(context)->Bulk(endpoint, buffer,
							 length, timeout_ms);
}

int TorneDevice::Bulk(u8 endpoint, void *buffer, int length,
		      unsigned int timeout_ms)
{
	if (!available_)
		return -ENODEV;
	if (length < 0 || (length && !buffer))
		return -EINVAL;
#ifdef __APPLE__
	int transferred = 0;
	const int ret = libusb_bulk_transfer(usb_dev_.handle, endpoint,
		static_cast<unsigned char *>(buffer), length, &transferred, timeout_ms);
	if (ret == LIBUSB_ERROR_TIMEOUT)
		return -ETIMEDOUT;
	if (ret == LIBUSB_ERROR_NO_DEVICE)
		return -ENODEV;
	if (ret)
		return -EIO;
	return transferred;
#else
	if (endpoint & 0x80) {
		ULONG transferred = 0;
		if (!WinUsb_ReadPipe(usb_dev_.winusb, endpoint,
				     static_cast<PUCHAR>(buffer), static_cast<ULONG>(length),
				     &transferred, nullptr))
			return GetLastError() == ERROR_SEM_TIMEOUT ? -ETIMEDOUT : -EIO;
		return static_cast<int>(transferred);
	}
	ULONG transferred = 0;
	if (!WinUsb_WritePipe(usb_dev_.winusb, endpoint,
			      static_cast<PUCHAR>(buffer), static_cast<ULONG>(length),
			      &transferred, nullptr))
		return GetLastError() == ERROR_SEM_TIMEOUT ? -ETIMEDOUT : -EIO;
	return static_cast<int>(transferred);
#endif
}

void TorneDevice::SleepCallback(void *, unsigned int milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

u64 TorneDevice::NowCallback(void *)
{
	return msg_now_ms();
}

void TorneDevice::LogCallback(void *, int level, const char *format, ...)
{
	char message[512] = {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (level <= TORNE_LOG_ERR)
		msg_err("torne: %s\n", message);
	else if (level == TORNE_LOG_INFO)
		msg_info("torne: %s\n", message);
	else
		msg_dbg("torne: %s\n", message);
}

int TorneDevice::StartUsbStream()
{
	if (!available_ || !demod_initialized_ || !bus_initialized_)
		return -ENODEV;
	if (streaming_)
		return stream_failed_ ? -EIO : 0;
	stream_failed_ = false;
	torne_framer_reset(&framer_);
	int ret = torne_dib_set_output(&dib_, TORNE_OUT_PARALLEL_GATED);
	if (ret)
		return ret;
	ret = torne_bridge_start_stream(&bridge_);
	if (ret) {
		torne_dib_set_output(&dib_, TORNE_OUT_HIGHZ);
		return ret;
	}
	ret = itedtv_bus_start_streaming(&stream_bus_, StreamHandler, this);
	if (ret) {
		torne_bridge_stop_stream(&bridge_);
		torne_dib_set_output(&dib_, TORNE_OUT_HIGHZ);
		return ret;
	}
	streaming_ = true;
	return 0;
}

int TorneDevice::StopUsbStream()
{
	int ret = 0;
	if (bus_initialized_ && streaming_)
		ret = itedtv_bus_stop_streaming(&stream_bus_);
	streaming_ = false;
	if (available_ && bridge_started_) {
		const int stop_ret = torne_bridge_stop_stream(&bridge_);
		if (!ret)
			ret = stop_ret;
		const int output_ret = torne_dib_set_output(&dib_, TORNE_OUT_HIGHZ);
		if (!ret)
			ret = output_ret;
	}
	torne_framer_reset(&framer_);
	return ret;
}

int TorneDevice::StreamHandler(void *context, void *data, u32 size)
{
	auto &device = *static_cast<TorneDevice *>(context);
	if (!data || !size) {
		device.stream_failed_ = true;
		if (device.buffer_)
			device.buffer_->NotifyStreamFailure();
		return -EIO;
	}
	if (size > kStreamBufferSize) {
		device.stream_failed_ = true;
		if (device.buffer_)
			device.buffer_->NotifyStreamFailure();
		return -EOVERFLOW;
	}
	if (!device.buffer_ || !device.available_)
		return -ENODEV;
	std::array<std::uint8_t, kStreamBufferSize + TORNE_TS_PACKET_SIZE> output{};
	const std::size_t output_size = torne_framer_feed(&device.framer_,
		static_cast<const std::uint8_t *>(data), size, output.data());
	if (!output_size)
		return 0;
	std::size_t written = output_size;
	if (!device.buffer_->Write(output.data(), written)) {
		device.stream_failed_ = true;
		device.buffer_->NotifyStreamFailure();
		return -ENOBUFS;
	}
	device.buffer_->NotifyWrite();
	return 0;
}

int TorneDevice::Receiver::Open()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.demod_initialized_)
		return -ENODEV;
	if (parent_.receiver_open_)
		return -EALREADY;
	const int ret = torne_dib_set_gpio(&parent_.dib_, 1, true);
	if (!ret)
		parent_.receiver_open_ = true;
	return ret;
}

void TorneDevice::Receiver::Close()
{
	SetCapture(false);
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.receiver_open_)
		return;
	if (parent_.available_ && parent_.demod_initialized_) {
		torne_dib_set_gpio(&parent_.dib_, 1, false);
		torne_dib_sleep(&parent_.dib_);
	}
	parent_.receiver_open_ = false;
}

int TorneDevice::Receiver::SetLnbVoltage(std::int32_t voltage)
{
	return voltage == 0 ? 0 : -ENOSYS;
}

int TorneDevice::Receiver::PrepareTune()
{
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if (params_.system != SystemType::ISDB_T)
		return -ENOSYS;
	if (params_.bandwidth && params_.bandwidth != 6)
		return -EINVAL;
	return 0;
}

int TorneDevice::Receiver::SetFrequency()
{
	if (params_.freq > (std::numeric_limits<std::uint32_t>::max)() / 1000U)
		return -EINVAL;
	const bool resume = parent_.streaming_;
	int ret = 0;
	if (resume)
		ret = parent_.StopUsbStream();
	if (!ret && parent_.buffer_ && !parent_.buffer_->Purge(parent_.deadline_))
		ret = -EIO;
	if (!ret) {
		torne_framer_reset(&parent_.framer_);
		ret = torne_bridge_prepare_stream(&parent_.bridge_);
	}
	if (!ret)
		ret = torne_dib_tune(&parent_.dib_, params_.freq * 1000U);
	if (resume) {
		const int restart_ret = parent_.StartUsbStream();
		if (!ret)
			ret = restart_ret;
	}
	return ret;
}

int TorneDevice::Receiver::CheckLock(bool &locked)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	locked = false;
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	struct torne_dib_status status = {};
	const int ret = torne_dib_status(&parent_.dib_, &status);
	locked = status.locked;
	return ret;
}

int TorneDevice::Receiver::SetCapture(bool capture)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if (capture == parent_.buffer_started_)
		return 0;
	if (capture) {
		parent_.buffer_->Start();
		parent_.buffer_started_ = true;
		const int ret = parent_.StartUsbStream();
		if (ret) {
			parent_.buffer_started_ = false;
			parent_.buffer_->Stop();
		}
		return ret;
	}
	const int ret = parent_.StopUsbStream();
	parent_.buffer_started_ = false;
	parent_.buffer_->Stop();
	return ret;
}

int TorneDevice::Receiver::ReadStat(command::StatType type, std::int32_t &value)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	if (type != command::StatType::CNR)
		return -ENOSYS;
	struct torne_dib_status status = {};
	const int ret = torne_dib_status(&parent_.dib_, &status);
	if (ret)
		return ret;
	if (!status.locked || !status.has_cnr)
		return -EAGAIN;
	value = status.cnr_x100 * 10;
	return 0;
}

int TorneDevice::Receiver::SetStreamId()
{
	return -ENOSYS;
}

bool TorneDevice::Receiver::TuneOverride(std::uint32_t timeout, bool &result)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.mutex_);
	result = false;
	const auto start = std::chrono::steady_clock::now();
	parent_.deadline_ = start + std::chrono::milliseconds(timeout);
	const std::uint64_t tune_start_ms = msg_now_ms();
	int ret = PrepareTune();
	if (!ret)
		ret = SetFrequency();
	const std::uint64_t freq_done_ms = msg_now_ms();
	bool locked = false;
	while (!ret && std::chrono::steady_clock::now() < parent_.deadline_) {
		ret = CheckLock(locked);
		if (ret || locked)
			break;
		const auto remaining = parent_.deadline_ - std::chrono::steady_clock::now();
		if (remaining > std::chrono::steady_clock::duration::zero())
			std::this_thread::sleep_for((std::min)(remaining,
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::milliseconds(20))));
	}
	if (!ret && !locked)
		ret = -ETIMEDOUT;
	result = !ret && locked;
	if (result) {
		stream_buf_->ArmTuneTiming(tune_start_ms, freq_done_ms - tune_start_ms,
					   msg_now_ms() - tune_start_ms,
					   params_.system, params_.freq);
	} else {
		stream_buf_->CancelTuneTiming();
		msg_log_at(ret == -ECANCELED ? PX4_LOG_DEBUG : PX4_LOG_WARN,
			   PX4_LOG_CAT_TS,
			   "torne tune failed: freq_khz=%u locked=%u result=%d\n",
			   params_.freq, locked ? 1U : 0U, ret);
	}
	parent_.deadline_ = (std::chrono::steady_clock::time_point::max)();
	return true;
}

} // namespace px4
