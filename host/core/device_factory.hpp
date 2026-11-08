// device_factory.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "device_base.hpp"
#include "device_definition_set.hpp"
#include "receiver_manager.hpp"

namespace px4 {

/*
 * INI の Type 値に対応する機種系統。WinUSB 版と macOS 版は同じ表を使うため、
 * 一方の OS だけで機種を追加すると device_factory_test が失敗する。
 */
enum class DeviceType : std::uint32_t {
	UNKNOWN = 0,
	PX4,
	PXMLT,
	ISDB2056,
	ISDBT2071,
	T230C,
	SIANO,
	TORNE,
};

/* INI の Type 文字列から機種系統を引く。未知の値は UNKNOWN。 */
DeviceType ParseDeviceType(const std::wstring &name) noexcept;
const wchar_t *DeviceTypeName(DeviceType type) noexcept;

struct CreatedDevice final {
	std::shared_ptr<DeviceBase> device;
	bool has_card_reader = false;
	int init_result = 0;
};

/*
 * 機種系統に応じた device class を作って Init() まで行う。Init() が失敗した
 * 場合は device を空にして init_result に負の errno を返す。コンストラクタの
 * DeviceError は呼び出し側へそのまま送出する。
 */
CreatedDevice CreateDevice(DeviceType type, const std::wstring &path,
	const DeviceDefinition &definition, std::uintptr_t index,
	ReceiverManager &receiver_manager);

} // namespace px4
