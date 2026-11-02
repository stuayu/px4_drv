// SPDX-License-Identifier: GPL-2.0-or-later

#include "siano_wire_core.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static u16 siano_wire_read_le16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 siano_wire_read_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void siano_wire_pack_header(u8 *buffer, u16 type, u8 source, u8 destination,
			    u16 length, u16 flags)
{
	buffer[0] = (u8)type;
	buffer[1] = (u8)(type >> 8);
	buffer[2] = source;
	buffer[3] = destination;
	buffer[4] = (u8)length;
	buffer[5] = (u8)(length >> 8);
	buffer[6] = (u8)flags;
	buffer[7] = (u8)(flags >> 8);
}

int siano_wire_decode_message(const u8 *buffer, size_t actual_length,
			      size_t response_alignment,
			      struct siano_wire_frame *frame)
{
	u16 length, flags;
	size_t offset = 0;

	if (!buffer || !frame)
		return -EINVAL;
	if (actual_length < 8)
		return -EMSGSIZE;

	length = siano_wire_read_le16(buffer + 4);
	flags = siano_wire_read_le16(buffer + 6);
	if (length < 8)
		return -EBADMSG;

	if ((flags & 4) && response_alignment) {
		size_t fragment = (flags >> 8) & 3;

		if (response_alignment > (size_t)-1 - fragment)
			return -EOVERFLOW;
		offset = response_alignment + fragment;
	}
	if (offset > actual_length || length > actual_length - offset)
		return -EMSGSIZE;
	if (offset > (size_t)-1 - 8)
		return -EOVERFLOW;

	frame->type = siano_wire_read_le16(buffer);
	frame->length = length;
	frame->flags = flags;
	frame->offset = offset;
	frame->payload_offset = offset + 8;
	frame->payload_length = length - 8;
	return 0;
}

int siano_wire_parse_firmware_header(const u8 *buffer, size_t size,
				     struct siano_firmware_header *header)
{
	if (!buffer || !header || size < 12)
		return -EINVAL;

	header->checksum = siano_wire_read_le32(buffer);
	header->length = siano_wire_read_le32(buffer + 4);
	header->start_address = siano_wire_read_le32(buffer + 8);
	if ((size_t)header->length > size - 12)
		return -EBADMSG;

	return 0;
}
