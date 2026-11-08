/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PX4_SI2168_H__
#define __PX4_SI2168_H__

#include "t230c_usb.h"

#define SI2168_T230C_TS_MODE 0x26U
#define SI2168_MAX_COMMANDS 17U

struct si2168_command {
	t230c_u8 data[8];
	t230c_u8 length;
	t230c_u8 response_length;
};

/*
 * ケーブル方式。Annex A は DVB-C、Annex B は J.83 Annex B (米国 ClearQAM、日本の
 * スカパー！プレミアムサービス光が使う方式)。両者は配信方式の値、シンボルレートの
 * プロパティ、状態要求の3点だけが違う。
 */
enum si2168_cable_annex {
	SI2168_CABLE_ANNEX_A = 0,
	SI2168_CABLE_ANNEX_B = 1,
};

struct si2168 {
	const struct i2c_comm_master *i2c;
	void *clock_context;
	t230c_u64 (*now_ms)(void *context);
	void (*sleep_us)(void *context, t230c_u32 usec);
	void (*set_deadline)(void *context, t230c_u64 deadline_ms);
	t230c_u8 ts_mode;
	bool ts_clock_inv;
	enum si2168_cable_annex annex;
};

#ifdef __cplusplus
extern "C" {
#endif

int si2168_validate_firmware(const t230c_u8 *data, size_t size);
int si2168_build_tune_commands(t230c_u32 symbol_rate_ksps,
			       t230c_u32 bandwidth_mhz, t230c_u8 ts_mode,
			       bool ts_clock_inv,
			       struct si2168_command *commands,
			       size_t capacity, size_t *count);
int si2168_build_cable_tune_commands(enum si2168_cable_annex annex,
				     t230c_u32 symbol_rate_ksps,
				     t230c_u32 bandwidth_mhz, t230c_u8 ts_mode,
				     bool ts_clock_inv,
				     struct si2168_command *commands,
				     size_t capacity, size_t *count);
int si2168_init(struct si2168 *demod,
		const struct i2c_comm_master *i2c,
		void *clock_context,
		t230c_u64 (*now_ms)(void *context),
		void (*sleep_us)(void *context, t230c_u32 usec),
		void (*set_deadline)(void *context, t230c_u64 deadline_ms),
		const t230c_u8 *firmware, size_t firmware_size,
		t230c_u8 ts_mode, bool ts_clock_inv);
int si2168_set_ts_bus(struct si2168 *demod, bool enabled,
		      t230c_u64 deadline_ms);
int si2168_tune(struct si2168 *demod, t230c_u32 symbol_rate_ksps,
		t230c_u32 bandwidth_mhz, t230c_u64 deadline_ms);
int si2168_tune_cable(struct si2168 *demod, enum si2168_cable_annex annex,
		      t230c_u32 symbol_rate_ksps, t230c_u32 bandwidth_mhz,
		      t230c_u64 deadline_ms);
int si2168_read_lock(struct si2168 *demod, bool *locked,
		     t230c_s32 *cnr_millibel, t230c_u64 deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
