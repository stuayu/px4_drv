/* SPDX-License-Identifier: GPL-2.0-only */

#include "t230c_ts_sync.h"
#include "ts_sync.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

int t230c_ts_sync_init(struct t230c_ts_sync *sync,
		       t230c_ts_u8 *storage, size_t capacity)
{
	if (!sync || !storage ||
	    capacity < T230C_TS_PACKET_SIZE * T230C_TS_SYNC_COUNT)
		return -EINVAL;
	sync->pending = storage;
	sync->capacity = capacity;
	sync->pending_size = 0;
	sync->synchronized = false;
	return 0;
}

void t230c_ts_sync_reset(struct t230c_ts_sync *sync)
{
	if (!sync)
		return;
	sync->pending_size = 0;
	sync->synchronized = false;
}

static void t230c_ts_sync_process(struct t230c_ts_sync *sync,
				  t230c_ts_sync_emit_fn emit,
				  void *context)
{
	size_t position = 0;

	while (position < sync->pending_size) {
		size_t count;
		bool invalid = false;

		if (!px4_ts_has_plain_sync(sync->pending[position])) {
			sync->synchronized = false;
			position++;
			continue;
		}
		if (!sync->synchronized) {
			for (count = 1; count < T230C_TS_SYNC_COUNT &&
			     position + count * T230C_TS_PACKET_SIZE < sync->pending_size;
			     ++count) {
				if (!px4_ts_has_plain_sync(
					sync->pending[position + count * T230C_TS_PACKET_SIZE])) {
					invalid = true;
					break;
				}
			}
			if (invalid) {
				position++;
				continue;
			}
			if (count < T230C_TS_SYNC_COUNT)
				break;
			sync->synchronized = true;
		}
		if (sync->pending_size - position < T230C_TS_PACKET_SIZE)
			break;
		emit(context, sync->pending + position);
		position += T230C_TS_PACKET_SIZE;
	}
	if (position) {
		sync->pending_size -= position;
		if (sync->pending_size)
			memmove(sync->pending, sync->pending + position,
				sync->pending_size);
	}
}

int t230c_ts_sync_push(struct t230c_ts_sync *sync,
		       const t230c_ts_u8 *data, size_t size,
		       t230c_ts_sync_emit_fn emit, void *context)
{
	if (!sync || !sync->pending || !emit || (!data && size))
		return -EINVAL;
	while (size) {
		size_t available = sync->capacity - sync->pending_size;
		size_t take;

		if (!available) {
			t230c_ts_sync_process(sync, emit, context);
			if (sync->pending_size)
				memmove(sync->pending, sync->pending + 1,
					--sync->pending_size);
			sync->synchronized = false;
			continue;
		}
		take = size < available ? size : available;
		memcpy(sync->pending + sync->pending_size, data, take);
		sync->pending_size += take;
		data += take;
		size -= take;
		t230c_ts_sync_process(sync, emit, context);
	}
	return 0;
}
