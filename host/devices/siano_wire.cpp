// SPDX-License-Identifier: GPL-2.0-or-later
#include "siano_wire.hpp"
#include "siano_wire_core.h"

namespace px4::siano {

int DecodeMessage(const std::uint8_t *buffer, std::size_t actual_length,
		  std::size_t response_alignment, MessageFrame &frame) noexcept
{
	struct siano_wire_frame decoded = {};
	const int ret = siano_wire_decode_message(buffer, actual_length,
						  response_alignment, &decoded);
	if (ret)
		return ret;
	frame.type = decoded.type;
	frame.length = decoded.length;
	frame.flags = decoded.flags;
	frame.offset = decoded.offset;
	frame.payload_offset = decoded.payload_offset;
	frame.payload_length = decoded.payload_length;
	return 0;
}

int ParseFirmwareHeader(const std::uint8_t *buffer, std::size_t size,
			FirmwareHeader &header) noexcept
{
	struct siano_firmware_header parsed = {};
	const int ret = siano_wire_parse_firmware_header(buffer, size, &parsed);
	if (ret)
		return ret;
	header.checksum = parsed.checksum;
	header.length = parsed.length;
	header.start_address = parsed.start_address;
	return 0;
}

} // namespace px4::siano
