/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T230C_TS_SYNC_H__
#define __T230C_TS_SYNC_H__


#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 t230c_ts_u8;
#else
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t t230c_ts_u8;
#endif

#define T230C_TS_PACKET_SIZE 188U
#define T230C_TS_SYNC_COUNT 4U

struct t230c_ts_sync {
	t230c_ts_u8 *pending;
	size_t capacity;
	size_t pending_size;
	bool synchronized;
};

typedef void (*t230c_ts_sync_emit_fn)(void *context,
				      const t230c_ts_u8 *packet);

#ifdef __cplusplus
extern "C" {
#endif

int t230c_ts_sync_init(struct t230c_ts_sync *sync,
		       t230c_ts_u8 *storage, size_t capacity);
void t230c_ts_sync_reset(struct t230c_ts_sync *sync);
int t230c_ts_sync_push(struct t230c_ts_sync *sync,
		       const t230c_ts_u8 *data, size_t size,
		       t230c_ts_sync_emit_fn emit, void *context);

#ifdef __cplusplus
}
#endif

#endif
