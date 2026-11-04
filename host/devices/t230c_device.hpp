#pragma once

#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>

#include "device_base.hpp"
#include "t230c_silicon.hpp"
#include "t230c_stream_stats.hpp"
#include "itedtv_bus.h"
#include "t230c_usb.h"

namespace px4 {

class T230CDevice final : public DeviceBase {
public:
	T230CDevice(const std::wstring &path, const DeviceDefinition &definition,
		std::uintptr_t index, ReceiverManager &receiver_manager);
	~T230CDevice() override;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	ReceiverBase *GetReceiver(int id) const override;

private:
	class Receiver;

	int Command(const std::uint8_t *data, std::size_t size, std::uint8_t *response, std::size_t response_size);
	int I2c(std::uint8_t address, const t230c::Bytes &write, t230c::Bytes &read);
	int I2cRaw(std::uint8_t address, const t230c::Bytes &write, t230c::Bytes &read);
	int TransferOut(const std::uint8_t *data, std::size_t size,
		unsigned int timeout_ms = 0);
	int TransferIn(std::uint8_t endpoint, std::uint8_t *data, std::size_t size,
		std::size_t &actual, unsigned int timeout_ms);
	static int UsbBulkTransfer(void *context, t230c_u8 endpoint,
			   t230c_u8 *data, std::size_t size,
			   std::size_t *actual, t230c_u32 timeout_ms);
	unsigned int ControlTimeout() const;
	void RecordCnr(std::int32_t cnr_millibel) noexcept;
	void ResetCnrStats() noexcept;
	void LogStreamStats(std::uint64_t now_ms, bool force) noexcept;
	static int StreamHandler(void *context, void *data, std::uint32_t size);

	std::recursive_mutex mutex_;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> buffer_;
	std::unique_ptr<t230c::TsSynchronizer> synchronizer_;
	std::unique_ptr<t230c::Silicon> silicon_;
	t230c::StreamDiagnostics stream_diagnostics_;
	struct t230c_usb usb_core_{};
	itedtv_bus stream_bus_ = {};
	t230c::Deadline deadline_ = (t230c::Deadline::max)();
	t230c::Bytes demod_fw_;
	unsigned int xfer_packets_ = 22;
	unsigned int max_urbs_ = 8;
	unsigned int receiver_packets_ = 16384;
	std::uint64_t stats_report_ms_ = 0;
	std::uint64_t stream_input_bytes_ = 0;
	std::uint64_t stream_output_packets_ = 0;
	std::uint64_t sync_discarded_bytes_ = 0;
	std::atomic<std::uint64_t> cnr_sample_count_{0};
	std::atomic<std::int64_t> cnr_sample_sum_{0};
	std::atomic<std::int32_t> cnr_sample_min_{(std::numeric_limits<std::int32_t>::max)()};
	std::atomic<std::int32_t> cnr_sample_max_{(std::numeric_limits<std::int32_t>::lowest)()};
	bool experimental_isdbc_ = false;
	bool receiver_open_ = false;
	bool bus_initialized_ = false;
	bool streaming_ = false;
	std::atomic_bool stream_failed_{false};
	std::atomic_bool available_{true};
	std::atomic_bool initialized_{false};
};

} // namespace px4
