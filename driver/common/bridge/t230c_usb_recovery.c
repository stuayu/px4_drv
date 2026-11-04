/* SPDX-License-Identifier: GPL-2.0-only */
#include "t230c_usb_recovery.h"

#include <errno.h>
#include <stddef.h>

int t230c_usb_recover_for_init(const struct t230c_usb_recovery_ops *ops,
			       void *context)
{
	static const uint8_t all_pipes[] = { 0x01, 0x81, 0x82 };
	static const uint8_t input_pipes[] = { 0x81, 0x82 };
	size_t i;
	int ret;

	if (!ops || !ops->stop_stream ||
	    ((ops->abort_pipe == NULL) != (ops->reset_pipe == NULL)) ||
	    (ops->flush_pipe && !ops->abort_pipe))
		return -EINVAL;

	if (ops->abort_pipe) {
		for (i = 0; i < sizeof(all_pipes) / sizeof(all_pipes[0]); ++i) {
			ret = ops->abort_pipe(context, all_pipes[i]);
			if (ret)
				return ret;
		}
	}

	if (ops->flush_pipe) {
		for (i = 0; i < sizeof(input_pipes) / sizeof(input_pipes[0]); ++i) {
			ret = ops->flush_pipe(context, input_pipes[i]);
			if (ret)
				return ret;
		}
	}

	if (ops->reset_pipe) {
		for (i = 0; i < sizeof(all_pipes) / sizeof(all_pipes[0]); ++i) {
			ret = ops->reset_pipe(context, all_pipes[i]);
			if (ret)
				return ret;
		}
	}

	/* 前のHostが異常終了してもFX2 FIFOとTS出力を初期化する。 */
	return ops->stop_stream(context);
}
