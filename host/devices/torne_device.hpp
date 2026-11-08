#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "device_base.hpp"
#include "itedtv_bus.h"
#include "torne_card.h"
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
	bool HasCardReader() const noexcept override { return true; }
	int OpenCard() override;
	void CloseCard() override;
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(std::uint32_t baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;
	int SetCardIoTimeout(std::uint32_t timeout_ms) override;

private:
	class Receiver;

	static int ControlCallback(void *context, u8 request_type, u16 value,
		u16 index, void *buffer, u16 length);
	static int BulkCallback(void *context, u8 endpoint, void *buffer,
		int length, unsigned int timeout_ms);
	static void SleepCallback(void *context, unsigned int milliseconds);
	static u64 NowCallback(void *context);
	static void LogCallback(void *context, int level, const char *format, ...);
	static int CardRead32Callback(void *context, u32 address, u32 *value,
		unsigned int timeout_ms);
	static int CardWrite32Callback(void *context, u32 address, u32 value,
		unsigned int timeout_ms);
	static int CardLockCallback(void *context);
	static void CardUnlockCallback(void *context);
	static void CardSleepCallback(void *context, unsigned int milliseconds);
	static int StreamHandler(void *context, void *data, u32 size);

	int Control(u8 request_type, u16 value, u16 index, void *buffer, u16 length,
		    unsigned int timeout_ms = 0);
	int Bulk(u8 endpoint, void *buffer, int length, unsigned int timeout_ms);
	int CardRead32(u32 address, u32 *value, unsigned int timeout_ms);
	int CardWrite32(u32 address, u32 value, unsigned int timeout_ms);
	unsigned int ControlTimeout() const noexcept;
	int StartUsbStream();
	int StopUsbStream();

	mutable std::recursive_mutex mutex_;
	std::timed_mutex control_mutex_;
	std::mutex card_uart_mutex_;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> buffer_;
	struct torne_bridge bridge_ = {};
	struct torne_dib dib_ = {};
	struct torne_card_port card_port_ = {};
	struct torne_framer framer_ = {};
	struct itedtv_bus stream_bus_ = {};
	std::chrono::steady_clock::time_point deadline_ =
		(std::chrono::steady_clock::time_point::max)();
	unsigned int receiver_packets_ = 16384;
	unsigned int max_urbs_ = 8;
	std::atomic_bool available_{true};
	std::atomic_bool card_open_{false};
	std::atomic_bool stream_failed_{false};
	std::atomic_bool initialized_{false};
#if defined(_WIN32) || defined(_WIN64)
	HANDLE control_event_ = nullptr;
#endif
	bool bridge_started_ = false;
	bool demod_initialized_ = false;
	bool bus_initialized_ = false;
	bool streaming_ = false;
	bool receiver_open_ = false;
	bool buffer_started_ = false;
};

} // namespace px4
