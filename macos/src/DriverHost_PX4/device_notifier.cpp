// device_notifier.cpp - macOS libusb hotplug implementation

#include "device_notifier.hpp"

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

	/* Register one callback for VID=0x0511 (all PLEX/ITE devices), any PID.
	 * The callback filters by PID using vidpid_to_guid_. */
	int ret = libusb_hotplug_register_callback(
		nullptr,
		static_cast<libusb_hotplug_event>(
			LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
			LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
		static_cast<libusb_hotplug_flag>(0), /* no ENUMERATE; Search() handles initial scan */
		0x0511,
		LIBUSB_HOTPLUG_MATCH_ANY,
		LIBUSB_HOTPLUG_MATCH_ANY,
		HotplugCallback,
		this,
		&hotplug_handle_);
	if (ret != LIBUSB_SUCCESS)
		throw DeviceNotifierError("DeviceNotifier: libusb_hotplug_register_callback() failed");

	th_ = std::thread(&DeviceNotifier::Worker, this);
}

DeviceNotifier::~DeviceNotifier()
{
	running_.store(false);

	libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);

	/* Interrupt the event loop so the worker thread exits promptly */
	libusb_interrupt_event_handler(nullptr);

	if (th_.joinable())
		th_.join();
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

	self->handler_->Handle(type, it->second, path);
	return 0;
}

void DeviceNotifier::Worker()
{
	while (running_.load()) {
		struct timeval tv = { 0, 100000 }; /* 100 ms */
		libusb_handle_events_timeout(nullptr, &tv);
	}
}

} // namespace px4
