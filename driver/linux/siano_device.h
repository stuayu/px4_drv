// SPDX-License-Identifier: GPL-2.0-only
#ifndef __SIANO_DEVICE_H__
#define __SIANO_DEVICE_H__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "itedtv_bus.h"
#include "ptx_chrdev.h"

#define SIANO_CHRDEV_NUM 1

struct siano_stream_context {
	struct ptx_chrdev *chrdev;
	spinlock_t lock;
	u8 remain_buf[188 * 4];
	u32 remain_len;
};

struct siano_device {
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct ptx_chrdev *chrdev;
	struct itedtv_bus bus;
	struct mutex operation_lock;
	struct mutex control_lock;
	spinlock_t state_lock;
	struct completion response_completion;
	struct siano_stream_context stream;
	u16 expected_response;
	u16 received_response;
	int response_error;
	u8 in_endpoint;
	size_t response_alignment;
	u8 firmware_id;
	u8 supported_protocols;
	u16 firmware_version;
	s32 cnr_db;
	unsigned long next_stats_request;
	bool response_received;
	bool protocol_initialized;
	bool protocol_failed;
	bool locked;
	bool stats_valid;
	bool pid_filter_configured;
	bool capture_enabled;
	bool streaming;
};

int siano_device_init(struct siano_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion);
void siano_device_term(struct siano_device *device);

#endif
