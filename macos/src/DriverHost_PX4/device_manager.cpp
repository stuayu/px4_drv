// device_manager.cpp - macOS libusb-based device manager
// (replaces Windows SetupAPI version)

#ifdef msg_prefix
#undef msg_prefix
#endif
#define msg_prefix "px4_libusb"

#include "device_manager.hpp"
#include "msg.h"

#include <algorithm>
#include <cwchar>
#include <cstdlib>

#include <libusb.h>


namespace px4 {

// ---------------------------------------------------------------------------
// NotifyHandler
// ---------------------------------------------------------------------------

void DeviceManager::NotifyHandler::Handle(px4::DeviceNotifyType type,
					   const GUID &interface_guid,
					   const wchar_t *path) noexcept
{
	try {
		std::wstring wpath(path);

		/* Normalise to lower-case for consistent map keys */
		std::transform(wpath.begin(), wpath.end(), wpath.begin(), ::towlower);

		switch (type) {
		case px4::DeviceNotifyType::ARRIVAL:
			parent_.Add(wpath, parent_.device_map_.at(interface_guid));
			break;
		case px4::DeviceNotifyType::REMOVE:
			parent_.Remove(wpath);
			break;
		default:
			break;
		}
	} catch (const std::out_of_range &) {}
}

// ---------------------------------------------------------------------------
// DeviceManager
// ---------------------------------------------------------------------------

DeviceManager::DeviceManager(const px4::DeviceDefinitionSet &device_defs,
			     px4::ReceiverManager &receiver_manager)
	: device_map_(),
	  receiver_manager_(receiver_manager),
	  mtx_(),
	  index_(0),
	  card_reader_generation_(0),
	  handler_(*this)
{
	auto &all_devs = device_defs.GetAll();

	/* Build device_map_ (keyed by device_interface_guid) and vidpid_to_guid */
	std::unordered_map<uint32_t, GUID> vidpid_to_guid;

	for (auto it = all_devs.cbegin(); it != all_devs.cend(); ++it) {
		const DeviceType type = ParseDeviceType(it->first);

		if (type == DeviceType::UNKNOWN) continue;

		for (auto &dev_def : it->second) {
			device_map_.emplace(dev_def.device_interface_guid,
					    std::make_pair(type, dev_def));

			/* Read VendorId / ProductId from per-device Config section */
			std::wstring vid_str = dev_def.configs.Get(L"VendorId", L"0");
			std::wstring pid_str = dev_def.configs.Get(L"ProductId", L"0");

			uint32_t vid = static_cast<uint32_t>(
				wcstoul(vid_str.c_str(), nullptr, 0));
			uint32_t pid = static_cast<uint32_t>(
				wcstoul(pid_str.c_str(), nullptr, 0));

			if (vid && pid)
				vidpid_to_guid[(vid << 16) | pid] =
					dev_def.device_interface_guid;
		}
	}

	/* Start hotplug notifier for dynamic connect/disconnect */
	notifier_.reset(new px4::DeviceNotifier(&handler_,
						std::move(vidpid_to_guid)));

	/* Initial enumeration: scan for already-connected devices */
	for (auto it = device_map_.cbegin(); it != device_map_.cend(); ++it)
		Search(it->first, it->second);
}

DeviceManager::~DeviceManager()
{
	notifier_.reset();
}

// ---------------------------------------------------------------------------
// Search – enumerate connected USB devices matching VendorId/ProductId
// (guid parameter is unused on macOS; VID/PID come from def.second.configs)
// ---------------------------------------------------------------------------

void DeviceManager::Search(const GUID & /*guid*/,
			   const std::pair<DeviceType, px4::DeviceDefinition> &def)
{
	std::wstring vid_str = def.second.configs.Get(L"VendorId", L"0");
	std::wstring pid_str = def.second.configs.Get(L"ProductId", L"0");

	uint16_t vid = static_cast<uint16_t>(
		wcstoul(vid_str.c_str(), nullptr, 0));
	uint16_t pid = static_cast<uint16_t>(
		wcstoul(pid_str.c_str(), nullptr, 0));

	if (!vid || !pid) return;

	libusb_device **list = nullptr;
	ssize_t cnt = libusb_get_device_list(nullptr, &list);
	if (cnt < 0) return;

	for (ssize_t i = 0; i < cnt; i++) {
		struct libusb_device_descriptor desc = {};
		if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
			continue;

		if (desc.idVendor != vid || desc.idProduct != pid)
			continue;

		wchar_t path[32];
		swprintf(path, 32, L"%u:%u",
			 static_cast<unsigned>(libusb_get_bus_number(list[i])),
			 static_cast<unsigned>(libusb_get_device_address(list[i])));

		Add(std::wstring(path), def);
	}

	libusb_free_device_list(list, 1);
}

// ---------------------------------------------------------------------------
// Add / Remove / Exists
// ---------------------------------------------------------------------------

void DeviceManager::Add(const std::wstring &path,
			const std::pair<DeviceType, px4::DeviceDefinition> &def)
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (Exists(path)) return;

	try {
		const CreatedDevice created = CreateDevice(def.first, path, def.second,
			++index_, receiver_manager_);
		if (created.device) {
			if (created.has_card_reader)
				card_reader_generation_.fetch_add(1);
			devices_.emplace(path, created.device);
		} else {
			msg_err("px4::DeviceManager::Add: %ls Init() failed. (path: %ls, ret: %d)\n",
				DeviceTypeName(def.first), path.c_str(), created.init_result);
		}
	} catch (const std::exception &ex) {
		msg_err("DeviceManager::Add: %s (path: %ls)\n", ex.what(), path.c_str());
	} catch (...) {
		msg_err("DeviceManager::Add: unknown error (path: %ls)\n", path.c_str());
	}
}

void DeviceManager::Remove(const std::wstring &path)
{
	std::shared_ptr<DeviceBase> device;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (!Exists(path)) return;
		device = devices_.at(path);
		device->SetAvailability(false);
		if (device->HasCardReader())
			card_reader_generation_.fetch_add(1);
		devices_.erase(path);
	}
	device.reset();
}

bool DeviceManager::Exists(const std::wstring &path) const
{
	return !!devices_.count(path);
}

std::vector<std::wstring> DeviceManager::ListCardReaders() const
{
	std::vector<std::wstring> readers;
	std::lock_guard<std::mutex> lock(mtx_);
	for (const auto &entry : devices_) {
		if (entry.second->HasCardReader())
			readers.emplace_back(entry.second->GetCardReaderName());
	}
	std::sort(readers.begin(), readers.end());
	return readers;
}

std::shared_ptr<DeviceBase> DeviceManager::FindCardReader(const std::wstring &name) const
{
	std::lock_guard<std::mutex> lock(mtx_);
	for (const auto &entry : devices_) {
		if (entry.second->HasCardReader() && entry.second->GetCardReaderName() == name)
			return entry.second;
	}
	return nullptr;
}

std::uint64_t DeviceManager::GetCardReaderGeneration() const noexcept
{
	return card_reader_generation_.load();
}

} // namespace px4
