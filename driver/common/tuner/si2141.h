/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PX4_SI2141_H__
#define __PX4_SI2141_H__

#include "t230c_usb.h"

#define SI2141_MAX_COMMANDS 4U

struct si2141_command {
	t230c_u8 data[8];
	t230c_u8 length;
	t230c_u8 response_length;
};

struct si2141 {
	const struct i2c_comm_master *i2c;
	void *clock_context;
	t230c_u64 (*now_ms)(void *context);
	void (*sleep_ms)(void *context, t230c_u32 msec);
	void (*set_deadline)(void *context, t230c_u64 deadline_ms);
	t230c_u8 if_port;
};

typedef int (*si2141_firmware_loader)(void *context, t230c_u8 rom_id,
				      const t230c_u8 **data, size_t *size);

#ifdef __cplusplus
extern "C" {
#endif

int si2141_build_tune_commands(t230c_u32 frequency_khz,
			       t230c_u32 bandwidth_mhz,
			       struct si2141_command *commands,
			       size_t capacity, size_t *count);
int si2141_init(struct si2141 *tuner,
		const struct i2c_comm_master *i2c,
		void *clock_context,
		t230c_u64 (*now_ms)(void *context),
		void (*sleep_ms)(void *context, t230c_u32 msec),
		void (*set_deadline)(void *context, t230c_u64 deadline_ms),
		si2141_firmware_loader load_firmware,
		void *firmware_context, t230c_u8 if_port,
		t230c_u8 *rom_id, t230c_u8 *part_id);
int si2141_tune(struct si2141 *tuner, t230c_u32 frequency_khz,
		t230c_u32 bandwidth_mhz, t230c_u64 deadline_ms);
int si2141_wait_tune(struct si2141 *tuner, t230c_u64 deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
