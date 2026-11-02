// SPDX-License-Identifier: GPL-2.0-only
#ifndef __T230C_DEVICE_H__
#define __T230C_DEVICE_H__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>

#include "itedtv_bus.h"
#include "ptx_chrdev.h"
#include "si2168.h"
#include "si2141.h"
#include "t230c_usb.h"
#include "t230c_ts_sync.h"

#define T230C_CHRDEV_NUM 1U
#define T230C_TS_SYNC_BUFFER_SIZE (T230C_TS_PACKET_SIZE * T230C_TS_SYNC_COUNT)
#define T230C_TS_OUTPUT_SIZE (T230C_TS_PACKET_SIZE * 16U)

struct t230c_device;

struct t230c_stream_context {
	struct t230c_device *device;
	struct ptx_chrdev *chrdev;
	spinlock_t lock;
	struct t230c_ts_sync synchronizer;
	u8 sync_buffer[T230C_TS_SYNC_BUFFER_SIZE];
	u8 output_buffer[T230C_TS_OUTPUT_SIZE];
	size_t output_size;
};

struct t230c_device {
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct usb_device *usb_dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct ptx_chrdev *chrdev;
	struct itedtv_bus stream_bus;
	struct t230c_usb usb;
	struct si2168 demod;
	struct si2141 tuner;
	struct mutex operation_lock;
	struct t230c_stream_context stream;
	bool capture_enabled;
	bool streaming;
};

int t230c_device_init(struct t230c_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion);
void t230c_device_term(struct t230c_device *device);

#endif
