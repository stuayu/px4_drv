// SPDX-License-Identifier: GPL-2.0-or-later
#include "siano_wire.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

namespace {

void check(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "Siano wire assertion failed at line %d\n", line);
		std::abort();
	}
}

#define require(condition) check((condition), __LINE__)

} // namespace

int main()
{
	{
		const std::array<std::uint8_t, 12> message = {
			0xB5, 0x02, 0x00, 0x0B, 0x0C, 0x00, 0x00, 0x00,
			0x47, 0x01, 0x02, 0x03,
		};
		px4::siano::MessageFrame frame{};
		require(px4::siano::DecodeMessage(message.data(), message.size(), 504, frame) == 0);
		require(frame.type == 693);
		require(frame.length == 12);
		require(frame.offset == 0);
		require(frame.payload_offset == 8);
		require(frame.payload_length == 4);
	}

	{
		std::array<std::uint8_t, 516> message{};
		message[0] = 0xB5;
		message[1] = 0x02;
		message[3] = 0x0B;
		message[4] = 0x0C;
		message[6] = 0x04; // split-message flag
		message[512] = 0x47;
		px4::siano::MessageFrame frame{};
		require(px4::siano::DecodeMessage(message.data(), message.size(), 504, frame) == 0);
		require(frame.offset == 504);
		require(frame.payload_offset == 512);
		require(frame.payload_length == 4);
	}

	{
		std::array<std::uint8_t, 12> message = {
			0xB5, 0x02, 0x00, 0x0B, 0x11, 0x00, 0x00, 0x00,
			0, 0, 0, 0,
		};
		px4::siano::MessageFrame frame{};
		require(px4::siano::DecodeMessage(message.data(), message.size(), 0, frame) == -EMSGSIZE);
		require(px4::siano::DecodeMessage(nullptr, message.size(), 0, frame) == -EINVAL);
	}

	{
		const std::array<std::uint8_t, 16> firmware = {
			0x78, 0x56, 0x34, 0x12, 0x04, 0x00, 0x00, 0x00,
			0x00, 0x10, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD,
		};
		px4::siano::FirmwareHeader header{};
		require(px4::siano::ParseFirmwareHeader(firmware.data(), firmware.size(), header) == 0);
		require(header.checksum == 0x12345678U);
		require(header.length == 4U);
		require(header.start_address == 0x1000U);
		require(px4::siano::ParseFirmwareHeader(firmware.data(), 15, header) == -EBADMSG);
	}
}
