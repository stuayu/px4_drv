// SPDX-License-Identifier: GPL-2.0-or-later
#include "siano_device.hpp"

#include <algorithm>
#include <array>
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
	const unsigned int configured_urbs = util::wtoui(
		config.Get(L"SianoMaxUrbs", L"8"));
	if (!configured_packets || configured_packets >
	    (std::numeric_limits<std::uint32_t>::max)() / 188U)
		throw DeviceError("Siano RIO: invalid receiver buffer size.");
	if (!configured_urbs || configured_urbs > 64)
		throw DeviceError("Siano RIO: invalid WinUSB URB count.");
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
	/* Linux smsusb は 10 本だが、Windows では同じ 0x2000 byte のまま本数を実測比較できるよう設定化し、既定8本を維持する。 */
	stream_bus_.usb.streaming.urb_num = configured_urbs;
	stream_bus_.usb.streaming.endpoint = stream_endpoint_;
	stream_bus_.usb.streaming.fail_on_error = true;
	stream_bus_.usb.streaming.measure_stream_timing =
		msg_log_get_stats_interval_ms() && px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	control_trace_.reserve(64);
	const int bus_ret = itedtv_bus_init(&stream_bus_);
	if (bus_ret)
		throw DeviceError("Siano RIO: USB transport initialization failed.");
	bus_initialized_ = true;

	receiver_.reset(new Receiver(*this));
	buffer_ = receiver_->GetStreamBuffer();
	transport_.reset(new BusTransport(*this));
	protocol_.reset(new siano::Protocol(*transport_, response_alignment_,
		[this](const std::uint8_t *data, std::size_t size) {
			if (!capture_state_.IsEnabled() || !buffer_)
				return;
			const bool collect_stats = msg_log_get_stats_interval_ms() &&
				px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
			if (collect_stats) {
				for (std::size_t offset = 0; offset + 188U <= size; offset += 188U) {
					ts::PacketEvent event{};
					if (!stream_diagnostics_.ObservePacket(data + offset, msg_now_ms(), event))
						continue;
					if (event.type == ts::PacketEventType::continuity_error) {
						if (event.missing_count_ambiguous) {
							msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
								   "Siano TS event: host_index=%s type=CC pid=0x%04x previous=%u expected=%u current=%u delta=%u missing_mod16=ambiguous duplicate_payload=%u timestamp_ms=%llu\n",
								   dev_.device_name, static_cast<unsigned int>(event.pid),
								   static_cast<unsigned int>(event.previous_cc),
								   static_cast<unsigned int>(event.expected_cc),
								   static_cast<unsigned int>(event.current_cc),
								   static_cast<unsigned int>(event.cc_delta),
								   event.duplicate_payload ? 1U : 0U,
								   static_cast<unsigned long long>(event.timestamp_ms));
						} else {
							msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
								   "Siano TS event: host_index=%s type=CC pid=0x%04x previous=%u expected=%u current=%u delta=%u missing_mod16=%u duplicate_payload=%u timestamp_ms=%llu\n",
								   dev_.device_name, static_cast<unsigned int>(event.pid),
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
							   "Siano TS event: host_index=%s type=TEI pid=0x%04x cc=%u timestamp_ms=%llu\n",
							   dev_.device_name, static_cast<unsigned int>(event.pid),
							   static_cast<unsigned int>(event.current_cc),
							   static_cast<unsigned long long>(event.timestamp_ms));
					}
				}
			}
			std::size_t output_size = size;
			const bool written = buffer_->Write(data, output_size);
			if (collect_stats)
				stream_diagnostics_.RecordWrite(written, size, output_size);
			if (written) {
				buffer_->NotifyWrite();
			} else if (protocol_) {
				protocol_->RecordStreamWriteFailure(size, output_size);
			}
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
	if (protocol_) {
		protocol_->Disconnect();
		const auto stats = protocol_->GetDiagnostics();
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_SERVER,
			   "Siano transfer statistics: host_index=%s decode_emsgsize=%llu decode_ebadmsg=%llu decode_eoverflow=%llu decode_other=%llu decode_dropped_bytes=%llu decode_last_ms=%llu decode_emsgsize_bda_data=%llu decode_emsgsize_bda_data_bytes=%llu decode_last_type=%u payload_short=%llu payload_dropped_bytes=%llu payload_last_ms=%llu stream_write_failures=%llu stream_dropped_bytes=%llu stream_last_ms=%llu.\n",
			   dev_.device_name,
			   static_cast<unsigned long long>(stats.decode_emsgsize_count),
			   static_cast<unsigned long long>(stats.decode_ebadmsg_count),
			   static_cast<unsigned long long>(stats.decode_eoverflow_count),
			   static_cast<unsigned long long>(stats.decode_other_count),
			   static_cast<unsigned long long>(stats.decode_dropped_bytes),
			   static_cast<unsigned long long>(stats.decode_last_error_ms),
			   static_cast<unsigned long long>(stats.decode_emsgsize_bda_data_count),
			   static_cast<unsigned long long>(stats.decode_emsgsize_bda_data_bytes),
			   static_cast<unsigned int>(stats.decode_last_message_type),
			   static_cast<unsigned long long>(stats.payload_short_count),
			   static_cast<unsigned long long>(stats.payload_dropped_bytes),
			   static_cast<unsigned long long>(stats.payload_last_error_ms),
			   static_cast<unsigned long long>(stats.stream_write_failure_count),
			   static_cast<unsigned long long>(stats.stream_dropped_bytes),
			   static_cast<unsigned long long>(stats.stream_last_error_ms));
	}
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
	if (msg_log_get_stats_interval_ms() && px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS)) {
		stream_diagnostics_.Reset();
		stats_report_ms_ = msg_now_ms();
		max_protocol_handler_time_us_ = 0;
#if defined(_WIN32) || defined(_WIN64)
		struct itedtv_usb_stream_stats discarded_stats = {};
		(void)itedtv_bus_take_usb_stream_stats(&stream_bus_, &discarded_stats);
#endif
	}
	stream_failed_ = false;
	/* RIO は制御応答も TS も同じ bulk IN で返すため、選局中も非同期読出しを保つ。 */
	const bool collect_stats = msg_log_get_stats_interval_ms() &&
		px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	const std::uint64_t start_ms = collect_stats ? msg_now_ms() : 0;
	const int ret = itedtv_bus_start_streaming(&stream_bus_, StreamHandler, this);
	if (!ret)
		streaming_ = true;
	if (collect_stats)
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "Siano stream start event: host_index=%s start_ms=%llu end_ms=%llu ret=%d\n",
			   dev_.device_name, static_cast<unsigned long long>(start_ms),
			   static_cast<unsigned long long>(msg_now_ms()), ret);
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
	if (msg_log_get_stats_interval_ms() && px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS)) {
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "Siano stream config: host_index=%s endpoint=0x%02x raw_io=%u urb_count=%u urb_bytes=%u max_packet=%u response_alignment=%zu stats_interval_ms=%u\n",
			   dev_.device_name, stream_endpoint_,
			   stream_bus_.usb.streaming.no_raw_io ? 0U : 1U,
			   stream_bus_.usb.streaming.urb_num,
			   stream_bus_.usb.streaming.urb_buffer_size,
			   stream_bus_.usb.max_bulk_size, response_alignment_,
			   msg_log_get_stats_interval_ms());
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
	const bool collect_stats = msg_log_get_stats_interval_ms() &&
		px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	const std::uint64_t start_ms = collect_stats ? msg_now_ms() : 0;
	const std::uint16_t message_type = length >= 2
		? static_cast<std::uint16_t>(message[0] |
			(static_cast<std::uint16_t>(message[1]) << 8)) : 0;
	const int ret = itedtv_bus_ctrl_tx(&stream_bus_, const_cast<std::uint8_t *>(message),
					 static_cast<int>(length));
	if (collect_stats) {
		const std::uint64_t end_ms = msg_now_ms();
		std::lock_guard<std::mutex> trace_lock(control_trace_mutex_);
		ControlEvent event{};
		event.sequence = ++control_trace_sequence_;
		event.start_ms = start_ms;
		event.end_ms = end_ms;
		event.message_type = message_type;
		event.length = length;
		event.result = ret;
		if (control_trace_.size() >= 64)
			control_trace_.erase(control_trace_.begin());
		control_trace_.push_back(event);
	}
	return ret;
}

void SianoDevice::LogStreamStats(std::uint64_t now_ms, bool force) noexcept
{
	const std::uint32_t interval_ms = msg_log_get_stats_interval_ms();
	if (!interval_ms || !px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS) ||
	    (!force && now_ms >= stats_report_ms_ && now_ms - stats_report_ms_ < interval_ms))
		return;
	const std::uint64_t window_start_ms = stats_report_ms_;
	const std::uint64_t elapsed_ms = now_ms >= stats_report_ms_ ? now_ms - stats_report_ms_ : 0;
	stats_report_ms_ = now_ms;
	const auto stats = stream_diagnostics_.TakeInterval();
	std::uint64_t usb_reads = 0;
	std::uint64_t usb_bytes = 0;
	std::uint64_t usb_short_reads = 0;
	std::uint64_t usb_short_bytes = 0;
	std::uint64_t usb_read_errors = 0;
	std::uint64_t usb_read_timeouts = 0;
	std::uint64_t usb_submit_errors = 0;
	std::uint64_t usb_wait_timeouts = 0;
	std::uint64_t usb_wait_errors = 0;
	std::uint64_t usb_max_worker_observed_completion_interval_us = 0;
	std::uint32_t usb_last_error = 0;
	const std::uint64_t max_protocol_handler_time_us = max_protocol_handler_time_us_;
	max_protocol_handler_time_us_ = 0;
	int usb_stats_ret = -ENOSYS;
#if defined(_WIN32) || defined(_WIN64)
	struct itedtv_usb_stream_stats usb_stats = {};
	usb_stats_ret = bus_initialized_
		? itedtv_bus_take_usb_stream_stats(&stream_bus_, &usb_stats) : -ENODEV;
	if (!usb_stats_ret) {
		usb_reads = usb_stats.completed_reads;
		usb_bytes = usb_stats.completed_bytes;
		usb_short_reads = usb_stats.short_reads;
		usb_short_bytes = usb_stats.short_bytes;
		usb_read_errors = usb_stats.read_errors;
		usb_read_timeouts = usb_stats.read_timeouts;
		usb_submit_errors = usb_stats.submit_errors;
		usb_wait_timeouts = usb_stats.wait_timeouts;
		usb_wait_errors = usb_stats.wait_errors;
		usb_max_worker_observed_completion_interval_us =
			usb_stats.max_worker_observed_completion_interval_us;
		usb_last_error = usb_stats.last_error;
	}
#endif
	const auto protocol_stats = protocol_ ? protocol_->GetDiagnostics() : siano::Protocol::Diagnostics{};
	std::array<ControlEvent, 64> control_events{};
	std::size_t control_event_count = 0;
	{
		std::lock_guard<std::mutex> trace_lock(control_trace_mutex_);
		for (const auto &event : control_trace_) {
			if (event.sequence > control_trace_reported_ &&
			    control_event_count < control_events.size())
				control_events[control_event_count++] = event;
		}
		if (control_event_count)
			control_trace_reported_ = control_events[control_event_count - 1].sequence;
	}
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "Siano TS stats: host_index=%s window_start_ms=%llu window_end_ms=%llu elapsed_ms=%llu packets=%llu cc_errors=%llu missing_mod16=%llu tei=%llu handler_reads=%llu handler_bytes=%llu handler_short_reads=%llu handler_short_bytes=%llu writes_failed=%llu dropped_bytes=%llu decode_total_errors=%llu decode_total_dropped_bytes=%llu\n",
		   dev_.device_name, static_cast<unsigned long long>(window_start_ms),
		   static_cast<unsigned long long>(now_ms), static_cast<unsigned long long>(elapsed_ms),
		   static_cast<unsigned long long>(stats.input_packets),
		   static_cast<unsigned long long>(stats.cc_errors),
		   static_cast<unsigned long long>(stats.missing_mod16),
		   static_cast<unsigned long long>(stats.tei_packets),
		   static_cast<unsigned long long>(stats.transfer_reads),
		   static_cast<unsigned long long>(stats.transfer_bytes),
		   static_cast<unsigned long long>(stats.short_reads),
		   static_cast<unsigned long long>(stats.short_bytes),
		   static_cast<unsigned long long>(stats.stream_write_failures),
		   static_cast<unsigned long long>(stats.stream_dropped_bytes),
		   static_cast<unsigned long long>(protocol_stats.decode_emsgsize_count +
			protocol_stats.decode_ebadmsg_count + protocol_stats.decode_eoverflow_count +
			protocol_stats.decode_other_count),
		   static_cast<unsigned long long>(protocol_stats.decode_dropped_bytes));
	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
		   "Siano WinUSB stats: host_index=%s window_start_ms=%llu window_end_ms=%llu elapsed_ms=%llu result=%d reads=%llu bytes=%llu short_reads=%llu short_bytes=%llu read_errors=%llu read_timeouts=%llu submit_errors=%llu wait_timeouts=%llu wait_errors=%llu max_worker_observed_completion_interval_ms=%.3f max_protocol_handler_ms=%.3f last_error=%u\n",
		   dev_.device_name, static_cast<unsigned long long>(window_start_ms),
		   static_cast<unsigned long long>(now_ms), static_cast<unsigned long long>(elapsed_ms), usb_stats_ret,
		   static_cast<unsigned long long>(usb_reads),
		   static_cast<unsigned long long>(usb_bytes),
		   static_cast<unsigned long long>(usb_short_reads),
		   static_cast<unsigned long long>(usb_short_bytes),
		   static_cast<unsigned long long>(usb_read_errors),
		   static_cast<unsigned long long>(usb_read_timeouts),
		   static_cast<unsigned long long>(usb_submit_errors),
		   static_cast<unsigned long long>(usb_wait_timeouts),
		   static_cast<unsigned long long>(usb_wait_errors),
		   static_cast<double>(usb_max_worker_observed_completion_interval_us) / 1000.0,
		   static_cast<double>(max_protocol_handler_time_us) / 1000.0,
		   static_cast<unsigned int>(usb_last_error));
	for (const auto &pid : stats.pids) {
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "Siano PID stats: host_index=%s pid=0x%04x cc_errors=%llu missing_mod16=%llu tei=%llu\n",
			   dev_.device_name, static_cast<unsigned int>(pid.pid),
			   static_cast<unsigned long long>(pid.cc_errors),
			   static_cast<unsigned long long>(pid.missing_mod16),
			   static_cast<unsigned long long>(pid.tei_packets));
	}
	for (std::size_t i = 0; i < control_event_count; ++i) {
		const auto &event = control_events[i];
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "Siano control event: host_index=%s message_type=0x%04x bytes=%zu start_ms=%llu end_ms=%llu duration_ms=%llu ret=%d\n",
			   dev_.device_name, static_cast<unsigned int>(event.message_type), event.length,
			   static_cast<unsigned long long>(event.start_ms),
			   static_cast<unsigned long long>(event.end_ms),
			   static_cast<unsigned long long>(event.end_ms >= event.start_ms
				? event.end_ms - event.start_ms : 0), event.result);
	}
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
		device.LogStreamStats(msg_now_ms(), true);
		return 0;
	}
	if (!device.protocol_)
		return -ENODEV;
	/* USB の空転送は Siano フレームではないため、受信状態を壊さず読み捨てる。 */
	if (size == 0)
		return 0;
	const bool collect_stats = msg_log_get_stats_interval_ms() &&
		px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS);
	if (collect_stats)
		device.stream_diagnostics_.RecordTransfer(size,
			device.stream_bus_.usb.streaming.urb_buffer_size);
	/* USB 転送単位を Siano メッセージへ分解し、BDA data だけを TS バッファーへ渡す。 */
	/* HandleTransfer はProtocol decodeからStreamBuffer::Writeまでを同期実行する。 */
	const auto handler_start = collect_stats ? std::chrono::steady_clock::now()
							 : std::chrono::steady_clock::time_point{};
	const int ret = device.protocol_->HandleTransfer(
		static_cast<const std::uint8_t *>(data), size);
	if (collect_stats) {
		const auto handler_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - handler_start).count();
		if (handler_elapsed > 0 && static_cast<std::uint64_t>(handler_elapsed) >
		    device.max_protocol_handler_time_us_)
			device.max_protocol_handler_time_us_ = static_cast<std::uint64_t>(handler_elapsed);
	}
	if (ret) {
		msg_err("Siano RIO: message decode failed (ret: %d, size: %u).\n", ret, size);
		device.stream_failed_ = true;
		device.protocol_->Disconnect();
		if (device.buffer_)
			device.buffer_->NotifyStreamFailure();
	}
	device.LogStreamStats(msg_now_ms(), false);
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
		const bool buffer_started = parent_.capture_state_.IsBufferStarted();
		parent_.capture_state_.StopBuffer();
		if (buffer_started) {
			stream_buf_->Stop();
		}
		return parent_.StopUsbStream();
	}
	if (!parent_.available_ || !parent_.receiver_open_)
		return -ENODEV;
	int ret = parent_.EnsureProtocolInitialized();
	if (ret)
		return ret;
	if (!parent_.capture_state_.IsBufferStarted()) {
		if (!stream_buf_->Alloc(188U * parent_.receiver_packets_)) {
			parent_.StopUsbStream();
			return -ENOMEM;
		}
		stream_buf_->Start();
		parent_.capture_state_.StartBuffer();
	}
	ret = parent_.StartUsbStream();
	if (ret) {
		stream_buf_->Stop();
		parent_.capture_state_.StopBuffer();
		return ret;
	}
	parent_.capture_state_.EnableCapture();
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
	const bool was_capturing = parent_.capture_state_.PauseForTune();
	if (parent_.capture_state_.IsBufferStarted() && !stream_buf_->Purge(deadline)) {
		parent_.capture_state_.RestoreAfterTunePreparation(was_capturing);
		return true;
	}
	ret = parent_.StartUsbStream();
	if (ret)
		return true;
	const auto remaining = deadline - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero()) {
		parent_.capture_state_.RestoreAfterTunePreparation(was_capturing);
		return true;
	}
	const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
	const std::uint64_t tune_start_ms = msg_now_ms();
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_TUNER,
		   "Siano tune start: host_index=%s freq_khz=%u timestamp_ms=%llu\n",
		   parent_.dev_.device_name, static_cast<unsigned int>(params_.freq),
		   static_cast<unsigned long long>(tune_start_ms));
	ret = parent_.protocol_->TuneFrequencyHz(static_cast<std::uint32_t>(frequency_hz),
						 std::chrono::milliseconds((std::max<std::int64_t>)(1, remaining_ms)));
	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_TUNER,
		   "Siano tune result: host_index=%s freq_khz=%u ret=%d start_ms=%llu end_ms=%llu\n",
		   parent_.dev_.device_name, static_cast<unsigned int>(params_.freq), ret,
		   static_cast<unsigned long long>(tune_start_ms),
		   static_cast<unsigned long long>(msg_now_ms()));
	if (msg_log_get_stats_interval_ms() && px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_TS))
		msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_TS,
			   "Siano tune event: host_index=%s freq_khz=%u result=%d start_ms=%llu end_ms=%llu\n",
			   parent_.dev_.device_name, static_cast<unsigned int>(params_.freq), ret,
			   static_cast<unsigned long long>(tune_start_ms),
			   static_cast<unsigned long long>(msg_now_ms()));
	result = ret == 0;
	parent_.capture_state_.FinishTune(result);
	if (!result)
		parent_.StopUsbStream();
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
