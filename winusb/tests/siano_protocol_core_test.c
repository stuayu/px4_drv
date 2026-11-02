// SPDX-License-Identifier: GPL-2.0-or-later

#include "siano_protocol_core.h"
#include "siano_protocol_defs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static void put_le16(u8 *p, u16 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
}

static void put_le32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static void make_message(u8 *message, u16 type, size_t payload_size)
{
	memset(message, 0, SIANO_SMS_HEADER_SIZE + payload_size);
	put_le16(message, type);
	message[3] = SIANO_HIF_TASK;
	put_le16(message + 4, (u16)(SIANO_SMS_HEADER_SIZE + payload_size));
}

int main(void)
{
	{
		u8 message[SIANO_SMS_HEADER_SIZE + 12];
		struct siano_protocol_event event = {};
		make_message(message, SIANO_MSG_GET_VERSION_EX_RES, 12);
		message[12] = SIANO_DEVICE_MODE_ISDBT_BDA;
		message[13] = 1U << SIANO_DEVICE_MODE_ISDBT_BDA;
		message[18] = 0x08;
		message[19] = 0x00;
		CHECK(siano_protocol_decode_event(message, sizeof(message), 0, &event) == 0);
		CHECK(event.kind == SIANO_EVENT_RESPONSE);
		CHECK(event.message_type == SIANO_MSG_GET_VERSION_EX_RES);
		CHECK(event.firmware_id == SIANO_DEVICE_MODE_ISDBT_BDA);
		CHECK(event.supported_protocols == (1U << SIANO_DEVICE_MODE_ISDBT_BDA));
		CHECK(event.firmware_version == 0x0800);
	}

	{
		u8 message[SIANO_SMS_HEADER_SIZE + 28];
		struct siano_protocol_event event = {};
		make_message(message, SIANO_MSG_GET_STATISTICS_EX_RES, 28);
		put_le32(message + 8 + 16, 1);
		put_le32(message + 8 + 24, 13);
		CHECK(siano_protocol_decode_event(message, sizeof(message), 0, &event) == 0);
		CHECK(event.kind == SIANO_EVENT_STATISTICS);
		CHECK(event.locked);
		CHECK(event.has_cnr);
		CHECK(event.cnr_db == 13);
	}

	{
		u8 message[SIANO_SMS_HEADER_SIZE + 4];
		struct siano_protocol_event event = {};
		make_message(message, SIANO_MSG_DVBT_BDA_DATA, 4);
		message[8] = 0x47;
		CHECK(siano_protocol_decode_event(message, sizeof(message), 0, &event) == 0);
		CHECK(event.kind == SIANO_EVENT_TS);
		CHECK(event.payload_length == 4);
		CHECK(event.payload[0] == 0x47);
	}

	{
		u8 message[SIANO_SMS_HEADER_SIZE + 4] = {};
		struct siano_protocol_event event = {};
		make_message(message, SIANO_MSG_GET_STATISTICS_EX_RES, 4);
		CHECK(siano_protocol_decode_event(message, sizeof(message), 0, &event) == -EMSGSIZE);
	}

	(void)printf("Siano protocol core checks passed\n");
	return 0;
}
