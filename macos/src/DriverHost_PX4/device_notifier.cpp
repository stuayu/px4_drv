// device_notifier.cpp - macOS libusb hotplug implementation

#ifdef msg_prefix
#undef msg_prefix
#endif
#define msg_prefix "px4_libusb"

#include "device_notifier.hpp"
#include "msg.h"

#include <cstdio>
#include <cwchar>

namespace px4 {

DeviceNotifier::DeviceNotifier(DeviceNotifyHandler *handler,
			       std::unordered_map<uint32_t, GUID> vidpid_to_guid)
	: handler_(handler),
	  vidpid_to_guid_(std::move(vidpid_to_guid)),
	  hotplug_handle_(0),
	  running_(true)
{
	if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
		throw DeviceNotifierError("DeviceNotifier: libusb hotplug not supported");

	/* MyGica (0x0572) や Siano (0x3275) など PLEX 以外の VID も扱うため全ベンダーで登録し、
	 * 通知は設定済みの VID/PID 表で絞り込む。 */
	int ret = libusb_hotplug_register_callback(
		nullptr,
		static_cast<libusb_hotplug_event>(
			LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
			LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
		static_cast<libusb_hotplug_flag>(0), /* no ENUMERATE; Search() handles initial scan */
		LIBUSB_HOTPLUG_MATCH_ANY,
		LIBUSB_HOTPLUG_MATCH_ANY,
		LIBUSB_HOTPLUG_MATCH_ANY,
		HotplugCallback,
		this,
		&hotplug_handle_);
	if (ret != LIBUSB_SUCCESS)
		throw DeviceNotifierError("DeviceNotifier: libusb_hotplug_register_callback() failed");

	try {
		notify_th_ = std::thread(&DeviceNotifier::NotifyWorker, this);
		th_ = std::thread(&DeviceNotifier::Worker, this);
	} catch (...) {
		/* スレッド生成失敗でも登録と起動済みスレッドを残さない */
		libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);
		StopNotifyWorker();
		throw;
	}
}

DeviceNotifier::~DeviceNotifier()
{
	libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);

	running_.store(false);

	/* Interrupt the event loop so the worker thread exits promptly */
	libusb_interrupt_event_handler(nullptr);

	if (th_.joinable())
		th_.join();

	/* イベント生成を止めてから通知を停止し、handler の寿命内で完了を待つ */
	StopNotifyWorker();
}

int LIBUSB_CALL DeviceNotifier::HotplugCallback(libusb_context * /*ctx*/,
						  libusb_device *device,
						  libusb_hotplug_event event,
						  void *user_data)
{
	auto *self = static_cast<DeviceNotifier *>(user_data);

	struct libusb_device_descriptor desc = {};
	if (libusb_get_device_descriptor(device, &desc) != LIBUSB_SUCCESS)
		return 0;

	uint32_t key = (static_cast<uint32_t>(desc.idVendor) << 16) |
		       static_cast<uint32_t>(desc.idProduct);

	auto it = self->vidpid_to_guid_.find(key);
	if (it == self->vidpid_to_guid_.end())
		return 0;

	wchar_t path[32];
	swprintf(path, 32, L"%u:%u",
		 static_cast<unsigned>(libusb_get_bus_number(device)),
		 static_cast<unsigned>(libusb_get_device_address(device)));

	DeviceNotifyType type = (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
				? DeviceNotifyType::ARRIVAL
				: DeviceNotifyType::REMOVE;

	/* 同期 USB 転送は libusb コールバック内で実行できないため、値だけを渡す */
	try {
		{
			std::lock_guard<std::mutex> lock(self->queue_mtx_);
			if (self->notify_stopping_)
				return 0;
			self->queue_.push_back({type, it->second, path});
		}
		self->queue_cv_.notify_one();
	} catch (...) {
		/* C のコールバック境界へ例外を伝播させない */
		msg_err("DeviceNotifier: could not queue notification. (path: %ls)\n", path);
	}
	return 0;
}

void DeviceNotifier::Worker()
{
	while (running_.load()) {
		struct timeval tv = { 0, 100000 }; /* 100 ms */
		libusb_handle_events_timeout(nullptr, &tv);
	}
}

void DeviceNotifier::StopNotifyWorker()
{
	{
		std::lock_guard<std::mutex> lock(queue_mtx_);
		notify_stopping_ = true;
		/* 終了中の新たな初期化を避け、未処理通知は破棄する */
		queue_.clear();
	}
	queue_cv_.notify_one();
	if (notify_th_.joinable())
		notify_th_.join();
}

void DeviceNotifier::NotifyWorker()
{
	std::unique_lock<std::mutex> lock(queue_mtx_);
	while (true) {
		queue_cv_.wait(lock, [this] { return notify_stopping_ || !queue_.empty(); });
		if (notify_stopping_)
			break;

		auto notification = std::move(queue_.front());
		queue_.pop_front();
		/* 到着・離脱を一つのスレッドで順序通り処理し、USB 待機中はキューをロックしない */
		lock.unlock();
		handler_->Handle(notification.type, notification.interface_guid,
				 notification.path.c_str());
		lock.lock();
	}
}

} // namespace px4
