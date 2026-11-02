// device_factory.cpp

#include "device_factory.hpp"

#include <cwctype>

#include "px4_device.hpp"
#include "pxmlt_device.hpp"
#include "isdb2056_device.hpp"
#include "isdbt2071_device.hpp"
#include "t230c_device.hpp"
#include "siano_device.hpp"

namespace px4 {

namespace {

struct TypeEntry {
	DeviceType type;
	const wchar_t *name;
};

constexpr TypeEntry kTypes[] = {
	{ DeviceType::PX4, L"PX4" },
	{ DeviceType::PXMLT, L"PXMLT" },
	{ DeviceType::ISDB2056, L"ISDB2056" },
	{ DeviceType::ISDBT2071, L"ISDBT2071" },
	{ DeviceType::T230C, L"T230C" },
	{ DeviceType::SIANO, L"SIANO" },
};

template <typename T>
CreatedDevice Make(const std::wstring &path, const DeviceDefinition &definition,
	std::uintptr_t index, ReceiverManager &receiver_manager)
{
	CreatedDevice created;
	auto device = std::make_shared<T>(path, definition, index, receiver_manager);
	created.init_result = device->Init();
	if (!created.init_result) {
		created.has_card_reader = device->HasCardReader();
		created.device = std::move(device);
	}
	return created;
}

} // namespace

DeviceType ParseDeviceType(const std::wstring &name) noexcept
{
	for (const auto &entry : kTypes)
		if (name == entry.name)
			return entry.type;
	return DeviceType::UNKNOWN;
}

const wchar_t *DeviceTypeName(DeviceType type) noexcept
{
	for (const auto &entry : kTypes)
		if (type == entry.type)
			return entry.name;
	return L"UNKNOWN";
}

CreatedDevice CreateDevice(DeviceType type, const std::wstring &path,
	const DeviceDefinition &definition, std::uintptr_t index,
	ReceiverManager &receiver_manager)
{
	/* 機種追加時に型と DeviceType の対応をこの場で確認できるよう生成分岐を明示する。 */
	switch (type) {
	case DeviceType::PX4:
		return Make<Px4Device>(path, definition, index, receiver_manager);
	case DeviceType::PXMLT:
		return Make<PxMltDevice>(path, definition, index, receiver_manager);
	case DeviceType::ISDB2056:
		return Make<Isdb2056Device>(path, definition, index, receiver_manager);
	case DeviceType::ISDBT2071:
		return Make<Isdbt2071Device>(path, definition, index, receiver_manager);
	case DeviceType::T230C:
		return Make<T230CDevice>(path, definition, index, receiver_manager);
	case DeviceType::SIANO:
		/* 同じ USB ID の RIO 機能ごとに一受信機を登録する。 */
		return Make<SianoDevice>(path, definition, index, receiver_manager);
	default:
		break;
	}
	CreatedDevice created;
	created.init_result = -EINVAL;
	return created;
}

} // namespace px4
