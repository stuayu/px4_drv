// SPDX-License-Identifier: GPL-2.0-or-later

#include "siano_protocol_core.h"
#include "siano_protocol_defs.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static u16 siano_protocol_read_be16(const u8 *p)
{
	return ((u16)p[0] << 8) | p[1];
}

static u32 siano_protocol_read_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static s32 siano_protocol_read_le_s32(const u8 *p)
{
	u32 value = siano_protocol_read_le32(p);

	if (value <= 0x7fffffffU)
		return (s32)value;

	/* 負数の2の補数を、符号なし演算でオーバーフローなく復元する。 */
	return -1 - (s32)(~value);
}

static bool siano_protocol_is_response(u16 type)
{
	switch (type) {
	case SIANO_MSG_GET_VERSION_EX_RES:
	case SIANO_MSG_INIT_DEVICE_RES:
	case SIANO_MSG_ADD_PID_FILTER_RES:
	case SIANO_MSG_GET_STATISTICS_RES:
	case SIANO_MSG_GET_STATISTICS_EX_RES:
	case SIANO_MSG_DATA_DOWNLOAD_RES:
	case SIANO_MSG_DATA_VALIDITY_RES:
	case SIANO_MSG_SWDOWNLOAD_TRIGGER_RES:
	case SIANO_MSG_SW_RELOAD_START_RES:
	case SIANO_MSG_ISDBT_TUNE_RES:
		return true;
	default:
		return false;
	}
}

int siano_protocol_decode_event(const u8 *buffer, size_t actual_length,
				size_t response_alignment,
				struct siano_protocol_event *event)
{
	int ret;
	struct siano_wire_frame frame;
	const u8 *payload;

	if (!event)
		return -EINVAL;
	ret = siano_wire_decode_message(buffer, actual_length,
				       response_alignment, &frame);
	if (ret)
		return ret;

	memset(event, 0, sizeof(*event));
	event->message_type = frame.type;
	event->payload = buffer + frame.payload_offset;
	event->payload_length = frame.payload_length;
	payload = event->payload;

	switch (frame.type) {
	case SIANO_MSG_DVBT_BDA_DATA:
		event->kind = SIANO_EVENT_TS;
		break;
	case SIANO_MSG_GET_VERSION_EX_RES:
		if (frame.payload_length < 12)
			return -EMSGSIZE;
		event->kind = SIANO_EVENT_RESPONSE;
		event->firmware_id = payload[4];
		event->supported_protocols = payload[5];
		event->firmware_version = siano_protocol_read_be16(payload + 10);
		break;
	case SIANO_MSG_GET_STATISTICS_EX_RES:
		if (frame.payload_length < 20)
			return -EMSGSIZE;
		event->kind = SIANO_EVENT_STATISTICS;
		event->locked = siano_protocol_read_le32(payload + 16) != 0;
		if (frame.payload_length >= 28) {
			event->has_cnr = true;
			event->cnr_db = siano_protocol_read_le_s32(payload + 24);
		}
		break;
	case SIANO_MSG_GET_STATISTICS_RES:
		if (frame.payload_length < 16)
			return -EMSGSIZE;
		event->kind = SIANO_EVENT_STATISTICS;
		event->locked = siano_protocol_read_le32(payload + 12) != 0;
		if (frame.payload_length >= 24) {
			event->has_cnr = true;
			event->cnr_db = siano_protocol_read_le_s32(payload + 20);
		}
		break;
	case SIANO_MSG_SIGNAL_DETECTED_IND:
		event->kind = SIANO_EVENT_SIGNAL_DETECTED;
		event->locked = true;
		break;
	case SIANO_MSG_NO_SIGNAL_IND:
		event->kind = SIANO_EVENT_NO_SIGNAL;
		break;
	default:
		if (siano_protocol_is_response(frame.type))
			event->kind = SIANO_EVENT_RESPONSE;
		break;
	}

	return 0;
}
