// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "device_base.hpp"
#include "itedtv_bus.h"
#include "../core/ts_stream_diagnostics.hpp"
#include "siano_capture_state.hpp"
#include "siano_protocol.hpp"

namespace px4 {

/* PX-S1UD V2.0 / PX-Q1UD は Siano RIO の USB 機能ごとに登録し、1 機能を1受信機として扱う。 */
class SianoDevice final : public DeviceBase {
public:
	SianoDevice(const std::wstring &path, const DeviceDefinition &definition,
		    std::uintptr_t index, ReceiverManager &receiver_manager);
	~SianoDevice() override;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	ReceiverBase *GetReceiver(int id) const override;

private:
	class Receiver;
	class BusTransport;
	struct ControlEvent final {
		std::uint64_t sequence = 0;
		std::uint64_t start_ms = 0;
		std::uint64_t end_ms = 0;
		std::uint16_t message_type = 0;
		std::size_t length = 0;
		int result = 0;
	};

	int FindStreamEndpoint(std::uint8_t &endpoint, std::size_t &response_alignment) const;
	int StartUsbStream();
	int StopUsbStream();
	int EnsureProtocolInitialized(
		std::chrono::milliseconds timeout = std::chrono::seconds(30));
	void LogStreamStats(std::uint64_t now_ms, bool force = false) noexcept;
	int SendControl(const std::uint8_t *message, std::size_t length,
			std::uint32_t timeout_ms) noexcept;
	static int StreamHandler(void *context, void *data, std::uint32_t size);

	std::recursive_mutex mutex_;
	std::condition_variable_any close_cond_;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> buffer_;
	std::unique_ptr<BusTransport> transport_;
	std::unique_ptr<siano::Protocol> protocol_;
	itedtv_bus stream_bus_ = {};
	std::filesystem::path firmware_path_;
	ts::StreamDiagnostics stream_diagnostics_;
	std::mutex control_trace_mutex_;
	std::vector<ControlEvent> control_trace_;
	std::uint64_t control_trace_sequence_ = 0;
	std::uint64_t control_trace_reported_ = 0;
	std::size_t response_alignment_ = 0;
	std::size_t receiver_packets_ = 16384;
	std::uint64_t stats_report_ms_ = 0;
	std::uint64_t max_protocol_handler_time_us_ = 0;
	std::uint8_t stream_endpoint_ = 0;
	std::atomic_bool available_{true};
	std::atomic_bool initialized_{false};
	std::atomic_bool stream_failed_{false};
	bool bus_initialized_ = false;
	bool streaming_ = false;
	bool protocol_initialized_ = false;
	bool receiver_open_ = false;
	siano::CaptureState capture_state_;
};

} // namespace px4
