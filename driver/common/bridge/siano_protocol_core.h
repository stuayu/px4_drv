// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef __SIANO_PROTOCOL_CORE_H__
#define __SIANO_PROTOCOL_CORE_H__

#include "siano_wire_core.h"

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef int32_t s32;
#endif

enum siano_event_kind {
	SIANO_EVENT_OTHER = 0,
	SIANO_EVENT_TS,
	SIANO_EVENT_RESPONSE,
	SIANO_EVENT_STATISTICS,
	SIANO_EVENT_SIGNAL_DETECTED,
	SIANO_EVENT_NO_SIGNAL,
};

struct siano_protocol_event {
	enum siano_event_kind kind;
	u16 message_type;
	const u8 *payload;
	size_t payload_length;
	u8 firmware_id;
	u8 supported_protocols;
	u16 firmware_version;
	bool locked;
	bool has_cnr;
	s32 cnr_db;
};

#ifdef __cplusplus
extern "C" {
#endif

int siano_protocol_decode_event(const u8 *buffer, size_t actual_length,
				size_t response_alignment,
				struct siano_protocol_event *event);

#ifdef __cplusplus
}
#endif

#endif
