#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "device_base.hpp"
#include "t230c_silicon.hpp"
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
	static int StreamHandler(void *context, void *data, std::uint32_t size);

	std::recursive_mutex mutex_;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> buffer_;
	std::unique_ptr<t230c::TsSynchronizer> synchronizer_;
	std::unique_ptr<t230c::Silicon> silicon_;
	struct t230c_usb usb_core_{};
	itedtv_bus stream_bus_ = {};
	t230c::Deadline deadline_ = (t230c::Deadline::max)();
	t230c::Bytes demod_fw_;
	unsigned int xfer_packets_ = 816;
	unsigned int max_urbs_ = 8;
	unsigned int receiver_packets_ = 16384;
	bool experimental_isdbc_ = false;
	bool receiver_open_ = false;
	bool bus_initialized_ = false;
	bool streaming_ = false;
	std::atomic_bool stream_failed_{false};
	std::atomic_bool available_{true};
	std::atomic_bool initialized_{false};
};

} // namespace px4
