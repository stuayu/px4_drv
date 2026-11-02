/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __SIANO_WIRE_H__
#define __SIANO_WIRE_H__

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/stddef.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

struct siano_wire_frame {
	u16 type;
	u16 length;
	u16 flags;
	size_t offset;
	size_t payload_offset;
	size_t payload_length;
};

struct siano_firmware_header {
	u32 checksum;
	u32 length;
	u32 start_address;
};

#ifdef __cplusplus
extern "C" {
#endif

void siano_wire_pack_header(u8 *buffer, u16 type, u8 source, u8 destination,
			    u16 length, u16 flags);
int siano_wire_decode_message(const u8 *buffer, size_t actual_length,
			      size_t response_alignment,
			      struct siano_wire_frame *frame);
int siano_wire_parse_firmware_header(const u8 *buffer, size_t size,
				     struct siano_firmware_header *header);

#ifdef __cplusplus
}
#endif

#endif
