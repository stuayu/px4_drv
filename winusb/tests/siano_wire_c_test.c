// SPDX-License-Identifier: GPL-2.0-or-later

#include "siano_wire_core.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

int main(void)
{
	{
		u8 message[12] = { 0xb5, 0x02, 0x00, 0x0b, 0x0c, 0x00, 0x00, 0x00,
				  0x47, 0x01, 0x02, 0x03 };
		struct siano_wire_frame frame = {};
		CHECK(siano_wire_decode_message(message, sizeof(message), 504, &frame) == 0);
		CHECK(frame.type == 693);
		CHECK(frame.length == 12);
		CHECK(frame.offset == 0);
		CHECK(frame.payload_offset == 8);
		CHECK(frame.payload_length == 4);
	}

	{
		u8 message[516] = {};
		struct siano_wire_frame frame = {};
		message[0] = 0xb5;
		message[1] = 0x02;
		message[3] = 0x0b;
		message[4] = 0x0c;
		message[6] = 0x04;
		message[512] = 0x47;
		CHECK(siano_wire_decode_message(message, sizeof(message), 504, &frame) == 0);
		CHECK(frame.offset == 504);
		CHECK(frame.payload_offset == 512);
		CHECK(frame.payload_length == 4);
	}

	{
		u8 message[12] = { 0xb5, 0x02, 0x00, 0x0b, 0x11, 0x00, 0x00, 0x00,
				  0, 0, 0, 0 };
		struct siano_wire_frame frame = {};
		CHECK(siano_wire_decode_message(message, sizeof(message), 0, &frame) == -EMSGSIZE);
		CHECK(siano_wire_decode_message(NULL, sizeof(message), 0, &frame) == -EINVAL);
	}

	{
		u8 message[8];
		u8 expected[8] = { 0x68, 0x02, 0x00, 0x0b, 0x08, 0x00, 0x00, 0x00 };
		siano_wire_pack_header(message, 616, 0, 11, sizeof(message), 0);
		CHECK(!memcmp(message, expected, sizeof(expected)));
	}

	{
		u8 firmware[16] = { 0x78, 0x56, 0x34, 0x12, 4, 0, 0, 0,
				   0x00, 0x10, 0x00, 0x00, 0xaa, 0xbb, 0xcc, 0xdd };
		struct siano_firmware_header header = {};
		CHECK(siano_wire_parse_firmware_header(firmware, sizeof(firmware), &header) == 0);
		CHECK(header.checksum == 0x12345678U);
		CHECK(header.length == 4);
		CHECK(header.start_address == 0x1000);
		CHECK(siano_wire_parse_firmware_header(firmware, 15, &header) == -EBADMSG);
	}

	(void)printf("Siano wire protocol checks passed\n");
	return 0;
}
