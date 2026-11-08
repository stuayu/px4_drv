// receiver_info_set.hpp

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "config.hpp"
#include "command.hpp"

namespace px4 {

class ReceiverInfoSet {
public:
	ReceiverInfoSet() noexcept {}
	~ReceiverInfoSet() {}

	void Load(const px4::ConfigSet &configs) noexcept;
	const px4::command::ReceiverInfo& Get(std::size_t i) const;
	std::size_t GetDynamicMemoryBytes() const noexcept { return receivers_.capacity() * sizeof(px4::command::ReceiverInfo); }

private:
	std::vector<px4::command::ReceiverInfo> receivers_;
};

} // namespace px4
