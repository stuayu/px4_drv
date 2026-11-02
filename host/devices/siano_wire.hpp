// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace px4::siano {

struct MessageFrame final {
	std::uint16_t type = 0;
	std::uint16_t length = 0;
	std::uint16_t flags = 0;
	std::size_t offset = 0;
	std::size_t payload_offset = 0;
	std::size_t payload_length = 0;
};

struct FirmwareHeader final {
	std::uint32_t checksum = 0;
	std::uint32_t length = 0;
	std::uint32_t start_address = 0;
};

int DecodeMessage(const std::uint8_t *buffer, std::size_t actual_length,
		  std::size_t response_alignment, MessageFrame &frame) noexcept;
int ParseFirmwareHeader(const std::uint8_t *buffer, std::size_t size,
			FirmwareHeader &header) noexcept;

} // namespace px4::siano
