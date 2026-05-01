// device_notifier.hpp - macOS libusb hotplug device notifier
// (replaces Windows WM_DEVICECHANGE / RegisterDeviceNotificationW version)

#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <stdexcept>

#include <libusb.h>

#include "guid_compat.h"

namespace px4 {

enum class DeviceNotifyType {
	UNDEFINED = 0,
	ARRIVAL,
	REMOVE,
};

class DeviceNotifyHandler {
public:
	virtual ~DeviceNotifyHandler() {}

	virtual void Handle(DeviceNotifyType type,
			    const GUID &interface_guid,
			    const wchar_t *path) noexcept = 0;
};

class DeviceNotifier final {
public:
	/* vidpid_to_guid: maps (vid<<16|pid) -> device_interface_guid */
	explicit DeviceNotifier(DeviceNotifyHandler *handler,
				std::unordered_map<uint32_t, GUID> vidpid_to_guid);
	~DeviceNotifier();

	DeviceNotifier(const DeviceNotifier &) = delete;
	DeviceNotifier &operator=(const DeviceNotifier &) = delete;
	DeviceNotifier(DeviceNotifier &&) = delete;
	DeviceNotifier &operator=(DeviceNotifier &&) = delete;

private:
	static int LIBUSB_CALL HotplugCallback(libusb_context *ctx,
					       libusb_device *device,
					       libusb_hotplug_event event,
					       void *user_data);
	void Worker();

	DeviceNotifyHandler *handler_;
	std::unordered_map<uint32_t, GUID> vidpid_to_guid_;
	libusb_hotplug_callback_handle hotplug_handle_;
	std::atomic_bool running_;
	std::thread th_;
};

class DeviceNotifierError : public std::runtime_error {
public:
	explicit DeviceNotifierError(const std::string &what_arg)
		: runtime_error(what_arg.c_str()) {}
};

} // namespace px4
