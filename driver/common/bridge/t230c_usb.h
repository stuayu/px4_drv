/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T230C_USB_H__
#define __T230C_USB_H__

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t t230c_u8;
typedef uint16_t t230c_u16;
typedef uint32_t t230c_u32;
typedef uint64_t t230c_u64;
typedef int32_t t230c_s32;
#endif

#include "i2c_comm.h"

#ifdef __KERNEL__
typedef u8 t230c_u8;
typedef u16 t230c_u16;
typedef u32 t230c_u32;
typedef u64 t230c_u64;
typedef s32 t230c_s32;
#endif

struct t230c_usb_ops {
	int (*bulk_transfer)(void *context, t230c_u8 endpoint,
			     t230c_u8 *data, size_t size, size_t *actual,
			     t230c_u32 timeout_ms);
	t230c_u64 (*now_ms)(void *context);
};

struct t230c_usb {
	const struct t230c_usb_ops *ops;
	void *context;
	t230c_u32 ctrl_timeout_ms;
	t230c_u64 deadline_ms;
	struct i2c_comm_master i2c_master;
};

#ifdef __cplusplus
extern "C" {
#endif

int t230c_usb_init(struct t230c_usb *usb, const struct t230c_usb_ops *ops,
		   void *context, t230c_u32 timeout_ms);
void t230c_usb_set_deadline(struct t230c_usb *usb, t230c_u64 deadline_ms);
int t230c_usb_command(struct t230c_usb *usb, const t230c_u8 *write,
		      size_t write_size, t230c_u8 *response,
		      size_t response_size, t230c_u32 timeout_ms);
int t230c_usb_i2c_transfer(struct t230c_usb *usb, t230c_u8 address,
			   const t230c_u8 *write, size_t write_size,
			   t230c_u8 *read, size_t read_size,
			   t230c_u32 timeout_ms);
int t230c_usb_i2c_request(void *context,
			  const struct i2c_comm_request *requests, int count);
int t230c_usb_i2c_gate_ctrl(void *context, bool open);

#ifdef __cplusplus
}
#endif

#endif
