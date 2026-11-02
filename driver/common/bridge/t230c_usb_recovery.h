/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T230C_USB_RECOVERY_H__
#define __T230C_USB_RECOVERY_H__

#include <stdint.h>

struct t230c_usb_recovery_ops {
	int (*abort_pipe)(void *context, uint8_t endpoint);
	int (*flush_pipe)(void *context, uint8_t endpoint);
	int (*reset_pipe)(void *context, uint8_t endpoint);
	int (*stop_stream)(void *context);
};

#ifdef __cplusplus
extern "C" {
#endif

int t230c_usb_recover_for_init(const struct t230c_usb_recovery_ops *ops,
			       void *context);

#ifdef __cplusplus
}
#endif

#endif
