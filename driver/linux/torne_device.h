// SPDX-License-Identifier: GPL-2.0-only
#ifndef __TORNE_DEVICE_H__
#define __TORNE_DEVICE_H__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "itedtv_bus.h"
#include "ptx_chrdev.h"
#include "torne_core.h"
#include "torne_dib.h"

#define TORNE_CHRDEV_NUM 1U

struct torne_device {
	struct kref kref;
	atomic_t available;
	struct device *dev;
	struct usb_device *usb_dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct ptx_chrdev *chrdev;
	struct mutex operation_lock;
	struct itedtv_bus stream_bus;
	struct torne_bridge bridge;
	struct torne_dib dib;
	struct torne_framer *framer;
	u8 *ts_output;
	spinlock_t stream_lock;
	struct delayed_work idle_work;
	unsigned long last_data;
	bool bus_initialized;
	bool bridge_up;
	bool ready;
	bool capture_enabled;
	bool streaming;
};

int torne_device_init(struct torne_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion);
void torne_device_term(struct torne_device *device);

#endif
