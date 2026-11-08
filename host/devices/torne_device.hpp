#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "device_base.hpp"
#include "itedtv_bus.h"
#include "torne_core.h"
#include "torne_dib.h"

namespace px4 {

class TorneDevice final : public DeviceBase {
public:
	TorneDevice(const std::wstring &path, const DeviceDefinition &definition,
		    std::uintptr_t index, ReceiverManager &receiver_manager);
	~TorneDevice() override;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	ReceiverBase *GetReceiver(int id) const override;

private:
	class Receiver;

	static int ControlCallback(void *context, u8 request_type, u16 value,
		u16 index, void *buffer, u16 length);
	static int BulkCallback(void *context, u8 endpoint, void *buffer,
		int length, unsigned int timeout_ms);
	static void SleepCallback(void *context, unsigned int milliseconds);
	static u64 NowCallback(void *context);
	static void LogCallback(void *context, int level, const char *format, ...);
	static int StreamHandler(void *context, void *data, u32 size);

	int Control(u8 request_type, u16 value, u16 index, void *buffer, u16 length);
	int Bulk(u8 endpoint, void *buffer, int length, unsigned int timeout_ms);
	unsigned int ControlTimeout() const noexcept;
	int StartUsbStream();
	int StopUsbStream();

	mutable std::recursive_mutex mutex_;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> buffer_;
	struct torne_bridge bridge_ = {};
	struct torne_dib dib_ = {};
	struct torne_framer framer_ = {};
	struct itedtv_bus stream_bus_ = {};
	std::chrono::steady_clock::time_point deadline_ =
		(std::chrono::steady_clock::time_point::max)();
	unsigned int receiver_packets_ = 16384;
	unsigned int max_urbs_ = 8;
	std::atomic_bool available_{true};
	std::atomic_bool stream_failed_{false};
	std::atomic_bool initialized_{false};
	bool bridge_started_ = false;
	bool demod_initialized_ = false;
	bool bus_initialized_ = false;
	bool streaming_ = false;
	bool receiver_open_ = false;
	bool buffer_started_ = false;
};

} // namespace px4
