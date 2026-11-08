/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TORNE_CARD_H
#define TORNE_CARD_H

#include "torne_core.h"

/*
 * CECH-ZD1 J のカード UART レジスター操作を USB ブリッジから分離する。
 * lock/unlock はレジスター1回の read/write だけを保護し、sleep_ms はロックの外で呼ぶ。
 */
struct torne_card_io_ops {
	int (*read32)(void *context, u32 address, u32 *value, unsigned int timeout_ms);
	int (*write32)(void *context, u32 address, u32 value, unsigned int timeout_ms);
	int (*lock)(void *context);
	void (*unlock)(void *context);
	void (*sleep_ms)(void *context, unsigned int milliseconds);
	u64 (*now_ms)(void *context);
};

struct torne_card_port {
	const struct torne_card_io_ops *ops;
	void *context;
	u64 deadline_ms;
};

#ifdef __cplusplus
extern "C" {
#endif

void torne_card_port_init(struct torne_card_port *port,
			  const struct torne_card_io_ops *ops, void *context);
int torne_card_port_set_timeout(struct torne_card_port *port, unsigned int timeout_ms);
int torne_card_detect(struct torne_card_port *port, bool *present);
int torne_card_reset(struct torne_card_port *port);
int torne_card_set_baudrate(struct torne_card_port *port, u32 baudrate);
int torne_card_rx_ready(struct torne_card_port *port, bool *ready);
/* *length は呼び出し前に容量、成功後に読み取ったバイト数を表す。 */
int torne_card_read(struct torne_card_port *port, u8 *data, u8 *length);
int torne_card_write(struct torne_card_port *port, const u8 *data, u16 length);
int torne_card_power_down(struct torne_card_port *port);

#ifdef __cplusplus
}
#endif

#endif /* TORNE_CARD_H */
