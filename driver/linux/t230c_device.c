// SPDX-License-Identifier: GPL-2.0-only
/* MyGica T230C2 (USB 0572:C68A) の Linux 受信経路。 */

#include "print_format.h"
#include "t230c_device.h"

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>

#include "px4_device_params.h"
#include "px4_usb_params.h"

#define T230C_CTRL_TIMEOUT_MS 2000U
#define T230C_TS_URB_BUFFER_SIZE 4096U
#define T230C_TS_URB_NUM 8U
#define T230C_TS_ENDPOINT 0x82U

struct t230c_firmware_context {
	struct t230c_device *device;
	const struct firmware *firmware;
};

static void t230c_device_release(struct kref *kref);

static t230c_u64 t230c_now_ms(void *context)
{
	(void)context;
	return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

static void t230c_sleep_us(void *context, t230c_u32 usec)
{
	(void)context;
	usleep_range(usec, usec + 1000);
}

static void t230c_sleep_ms(void *context, t230c_u32 msec)
{
	(void)context;
	msleep(msec);
}

static void t230c_set_deadline(void *context, t230c_u64 deadline_ms)
{
	struct t230c_device *device = context;
	t230c_usb_set_deadline(&device->usb, deadline_ms);
}

static int t230c_bulk_transfer(void *context, t230c_u8 endpoint,
			       t230c_u8 *data, size_t size, size_t *actual,
			       t230c_u32 timeout_ms)
{
	struct t230c_device *device = context;
	int transferred = 0;
	int ret;
	unsigned int pipe;

	if (!device || !device->usb_dev || !data || !size || !actual ||
	    size > INT_MAX || !timeout_ms)
		return -EINVAL;
	pipe = endpoint & USB_DIR_IN
		? usb_rcvbulkpipe(device->usb_dev, endpoint)
		: usb_sndbulkpipe(device->usb_dev, endpoint);
	*actual = 0;
	ret = usb_bulk_msg(device->usb_dev, pipe, data, (int)size,
			&transferred, (int)timeout_ms);
	if (transferred > 0)
		*actual = transferred;
	return ret;
}

static const struct t230c_usb_ops t230c_usb_operations = {
	.bulk_transfer = t230c_bulk_transfer,
	.now_ms = t230c_now_ms,
};

static int t230c_gpio_command(struct t230c_device *device,
			      t230c_u8 port, t230c_u8 value)
{
	const t230c_u8 command[] = { 0x0e, port, value };
	t230c_u8 response;

	return t230c_usb_command(&device->usb, command, sizeof(command),
				 &response, sizeof(response),
				 T230C_CTRL_TIMEOUT_MS);
}

static int t230c_identify_state(struct t230c_device *device)
{
	int ret;

	ret = t230c_gpio_command(device, 0x04, 1);
	if (ret)
		return ret;
	msleep(20);
	ret = t230c_gpio_command(device, 0x83, 0);
	if (ret)
		return ret;
	ret = t230c_gpio_command(device, 0xc0, 1);
	if (ret)
		return ret;
	msleep(100);
	ret = t230c_gpio_command(device, 0x83, 1);
	if (ret)
		return ret;
	ret = t230c_gpio_command(device, 0xc0, 0);
	if (ret)
		return ret;
	msleep(50);
	return 0;
}

static int t230c_load_tuner_firmware(void *context, t230c_u8 rom_id,
				     const t230c_u8 **data, size_t *size)
{
	struct t230c_firmware_context *firmware_context = context;
	struct t230c_device *device = firmware_context->device;
	const char *primary = rom_id == 0x60 ? "dvb_driver_si2141_rom60.fw" :
		rom_id == 0x61 ? "dvb_driver_si2141_rom61.fw" : NULL;
	int ret = -ENOENT;

	if (!data || !size)
		return -EINVAL;
	if (primary) {
		ret = request_firmware(&firmware_context->firmware, primary,
				       device->dev);
		if (ret && ret != -ENOENT)
			return ret;
	}
	if (!firmware_context->firmware) {
		ret = request_firmware(&firmware_context->firmware,
				       "dvb-tuner-si2141-a10-01.fw",
				       device->dev);
		if (ret)
			return ret;
	}
	*data = firmware_context->firmware->data;
	*size = firmware_context->firmware->size;
	return 0;
}

static void t230c_stream_emit(void *context, const t230c_ts_u8 *packet)
{
	struct t230c_stream_context *stream = context;

	memcpy(stream->output_buffer + stream->output_size, packet,
	       T230C_TS_PACKET_SIZE);
	stream->output_size += T230C_TS_PACKET_SIZE;
	if (stream->output_size == sizeof(stream->output_buffer)) {
		ptx_chrdev_put_stream(stream->chrdev, stream->output_buffer,
				      stream->output_size);
		stream->output_size = 0;
	}
}

static int t230c_stream_handler(void *context, void *buffer, u32 length)
{
	struct t230c_stream_context *stream = context;
	unsigned long flags;
	int ret = 0;

	if (!buffer || !length || !stream->chrdev || !stream->device)
		return 0;
	spin_lock_irqsave(&stream->lock, flags);
	if (READ_ONCE(stream->device->capture_enabled)) {
		stream->output_size = 0;
		ret = t230c_ts_sync_push(&stream->synchronizer, buffer, length,
					 t230c_stream_emit, stream);
		if (stream->output_size) {
			ptx_chrdev_put_stream(stream->chrdev, stream->output_buffer,
					      stream->output_size);
			stream->output_size = 0;
		}
	}
	spin_unlock_irqrestore(&stream->lock, flags);
	return ret;
}

static void t230c_reset_stream_sync(struct t230c_device *device)
{
	unsigned long flags;

	spin_lock_irqsave(&device->stream.lock, flags);
	t230c_ts_sync_reset(&device->stream.synchronizer);
	spin_unlock_irqrestore(&device->stream.lock, flags);
}

static int t230c_stop_usb_stream(struct t230c_device *device)
{
	static const t230c_u8 stop[] = { 0x37, 0x00, 0x00 };
	int ret = 0;

	if (device->streaming && atomic_read(&device->available))
		ret = t230c_usb_command(&device->usb, stop, sizeof(stop), NULL, 0,
					T230C_CTRL_TIMEOUT_MS);
	if (device->streaming) {
		itedtv_bus_stop_streaming(&device->stream_bus);
		device->streaming = false;
	}
	t230c_reset_stream_sync(device);
	return ret;
}

static int t230c_start_usb_stream(struct t230c_device *device)
{
	static const t230c_u8 stop[] = { 0x37, 0x00, 0x00 };
	static const t230c_u8 start[] = { 0x36, 0x03, 0x00 };
	int ret;

	if (device->streaming)
		return 0;
	if (!atomic_read(&device->available))
		return -ENODEV;
	t230c_reset_stream_sync(device);
	ret = t230c_usb_command(&device->usb, stop, sizeof(stop), NULL, 0,
				T230C_CTRL_TIMEOUT_MS);
	if (ret)
		return ret;
	device->stream_bus.usb.streaming.urb_buffer_size =
		T230C_TS_URB_BUFFER_SIZE;
	device->stream_bus.usb.streaming.urb_num = T230C_TS_URB_NUM;
	device->stream_bus.usb.ctrl_timeout = T230C_CTRL_TIMEOUT_MS;
	ret = itedtv_bus_start_streaming(&device->stream_bus,
					 t230c_stream_handler, &device->stream);
	if (ret)
		return ret;
	device->streaming = true;
	msleep(20);
	ret = t230c_usb_command(&device->usb, start, sizeof(start), NULL, 0,
				T230C_CTRL_TIMEOUT_MS);
	if (ret) {
		itedtv_bus_stop_streaming(&device->stream_bus);
		device->streaming = false;
		t230c_reset_stream_sync(device);
	}
	return ret;
}

static int t230c_chrdev_init(struct ptx_chrdev *chrdev)
{
	(void)chrdev;
	return 0;
}

static int t230c_chrdev_term(struct ptx_chrdev *chrdev)
{
	(void)chrdev;
	return 0;
}

static int t230c_chrdev_open(struct ptx_chrdev *chrdev)
{
	struct t230c_device *device = chrdev->priv;
	return atomic_read(&device->available) ? 0 : -ENODEV;
}

static int t230c_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct t230c_device *device = chrdev->priv;
	int ret;

	mutex_lock(&device->operation_lock);
	WRITE_ONCE(device->capture_enabled, false);
	ret = t230c_stop_usb_stream(device);
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int t230c_chrdev_tune(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	struct t230c_device *device = chrdev->priv;
	bool was_capturing;
	int ret;

	if (!params || (params->system != PTX_ISDB_C_SYSTEM && params->system != PTX_J83B_SYSTEM))
		return -EINVAL;
	if (params->bandwidth != 6)
		return -EINVAL;
	if (!atomic_read(&device->available))
		return -ENODEV;

	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available)) {
		ret = -ENODEV;
		goto exit;
	}
	was_capturing = READ_ONCE(device->capture_enabled);
	WRITE_ONCE(device->capture_enabled, false);
	ret = t230c_stop_usb_stream(device);
	if (ret)
		goto restore_capture;
	ret = si2141_tune(&device->tuner, params->freq, 6, ~(t230c_u64)0);
	if (ret)
		goto restore_capture;
	/* Si2157 の TUNINT 待ちは参照ドライバーと同じく失敗を選局失敗にしない。 */
	(void)si2141_wait_tune(&device->tuner, ~(t230c_u64)0);
	/* ISDB-C は 5.274 Msym/s の Annex A、J.83 Annex B は 5.6 Msym/s の Annex B で選局する。
	 * J.83B は TSMF を使わず、TS をそのまま出力する。 */
	if (params->system == PTX_J83B_SYSTEM)
		ret = si2168_tune_cable(&device->demod, SI2168_CABLE_ANNEX_B, 5600, 6, ~(t230c_u64)0);
	else
		ret = si2168_tune(&device->demod, 5274, 6, ~(t230c_u64)0);
restore_capture:
	WRITE_ONCE(device->capture_enabled, was_capturing);
exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int t230c_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	struct t230c_device *device = chrdev->priv;
	t230c_s32 cnr;
	int ret;

	if (!locked)
		return -EINVAL;
	*locked = false;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available)) {
		ret = -ENODEV;
		goto exit;
	}
	ret = si2168_read_lock(&device->demod, locked, &cnr,
			       ~(t230c_u64)0);
	if (!ret && *locked && READ_ONCE(device->capture_enabled))
		ret = t230c_start_usb_stream(device);
exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int t230c_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	struct t230c_device *device = chrdev->priv;
	int ret = 0;

	mutex_lock(&device->operation_lock);
	if (status) {
		if (!atomic_read(&device->available)) {
			ret = -ENODEV;
			goto exit;
		}
		WRITE_ONCE(device->capture_enabled, true);
		ret = t230c_start_usb_stream(device);
		if (ret)
			WRITE_ONCE(device->capture_enabled, false);
	} else {
		WRITE_ONCE(device->capture_enabled, false);
		ret = t230c_stop_usb_stream(device);
	}
exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int t230c_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	struct t230c_device *device = chrdev->priv;
	bool locked = false;
	t230c_s32 cnr_millibel = 0;
	int ret;

	if (!value)
		return -EINVAL;
	*value = 0;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available)) {
		ret = -ENODEV;
		goto exit;
	}
	ret = si2168_read_lock(&device->demod, &locked, &cnr_millibel,
			       ~(t230c_u64)0);
	if (!ret && !locked)
		ret = -EAGAIN;
	if (!ret)
		*value = (u32)cnr_millibel;
exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static const struct ptx_chrdev_operations t230c_chrdev_ops = {
	.init = t230c_chrdev_init,
	.term = t230c_chrdev_term,
	.open = t230c_chrdev_open,
	.release = t230c_chrdev_release,
	.tune = t230c_chrdev_tune,
	.check_lock = t230c_chrdev_check_lock,
	.set_stream_id = NULL,
	.set_lnb_voltage = NULL,
	.set_capture = t230c_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = t230c_chrdev_read_cnr_raw,
};

int t230c_device_init(struct t230c_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion)
{
	static const char * const demod_firmware_name =
		"dvb-demod-si2168-d60-01.fw";
	int ret;
	const struct firmware *demod_firmware = NULL;
	struct t230c_firmware_context tuner_firmware = { 0 };
	t230c_u8 tuner_rom_id = 0xff;
	t230c_u8 tuner_part_id = 0xff;
	struct ptx_chrdev_config chrdev_config = { 0 };
	struct ptx_chrdev_group_config group_config = { 0 };
	struct ptx_chrdev_group *chrdev_group;
	struct usb_device *usb_dev;

	if (!device || !dev || !intf || !chrdev_ctx || !quit_completion)
		return -EINVAL;
	usb_dev = interface_to_usbdev(intf);
	if (intf->num_altsetting > 1) {
		ret = usb_set_interface(usb_dev,
					intf->cur_altsetting->desc.bInterfaceNumber, 0);
		if (ret)
			return ret;
	}
	memset(device, 0, sizeof(*device));
	get_device(dev);
	kref_init(&device->kref);
	atomic_set(&device->available, 0);
	device->dev = dev;
	device->usb_dev = usb_dev;
	device->quit_completion = quit_completion;
	mutex_init(&device->operation_lock);
	device->stream.device = device;
	spin_lock_init(&device->stream.lock);
	ret = t230c_ts_sync_init(&device->stream.synchronizer,
				 device->stream.sync_buffer,
				 sizeof(device->stream.sync_buffer));
	if (ret)
		goto fail_device_ref;
	ret = t230c_usb_init(&device->usb, &t230c_usb_operations, device,
			     T230C_CTRL_TIMEOUT_MS);
	if (ret)
		goto fail_device_ref;

	device->stream_bus.dev = dev;
	device->stream_bus.type = ITEDTV_BUS_USB;
	device->stream_bus.usb.dev = usb_dev;
	device->stream_bus.usb.ctrl_timeout = T230C_CTRL_TIMEOUT_MS;
	device->stream_bus.usb.streaming.endpoint = T230C_TS_ENDPOINT;
	device->stream_bus.usb.streaming.urb_buffer_size = T230C_TS_URB_BUFFER_SIZE;
	device->stream_bus.usb.streaming.urb_num = T230C_TS_URB_NUM;
	device->stream_bus.usb.streaming.no_dma = px4_usb_params.no_dma;
	ret = itedtv_bus_init(&device->stream_bus);
	if (ret)
		goto fail_device_ref;

	ret = request_firmware(&demod_firmware, demod_firmware_name, dev);
	if (ret) {
		dev_err(dev, "T230C: request_firmware(%s) failed (%d)\n",
			demod_firmware_name, ret);
		goto fail_bus;
	}
	ret = si2168_validate_firmware(demod_firmware->data,
				       demod_firmware->size);
	if (ret) {
		dev_err(dev, "T230C: invalid firmware %s (%d)\n",
			demod_firmware_name, ret);
		goto fail_firmware;
	}
	ret = t230c_identify_state(device);
	if (ret) {
		dev_err(dev, "T230C: bridge initialization failed (%d)\n", ret);
		goto fail_firmware;
	}
	ret = si2168_init(&device->demod, &device->usb.i2c_master, device,
			  t230c_now_ms, t230c_sleep_us, t230c_set_deadline,
			  demod_firmware->data, demod_firmware->size,
			  SI2168_T230C_TS_MODE, true);
	if (ret) {
		dev_err(dev, "T230C: Si2168 initialization failed (%d)\n", ret);
		goto fail_firmware;
	}
	tuner_firmware.device = device;
	ret = si2141_init(&device->tuner, &device->usb.i2c_master, device,
			  t230c_now_ms, t230c_sleep_ms, t230c_set_deadline,
			  t230c_load_tuner_firmware, &tuner_firmware, 0,
			  &tuner_rom_id, &tuner_part_id);
	if (tuner_firmware.firmware) {
		release_firmware(tuner_firmware.firmware);
		tuner_firmware.firmware = NULL;
	}
	if (ret) {
		dev_err(dev,
			"T230C: Si2141 initialization failed (part 0x%02x, ROM 0x%02x, error %d)\n",
			tuner_part_id, tuner_rom_id, ret);
		goto fail_firmware;
	}
	release_firmware(demod_firmware);
	demod_firmware = NULL;
	t230c_usb_set_deadline(&device->usb, ~(t230c_u64)0);

	chrdev_config.system_cap = PTX_ISDB_C_SYSTEM | PTX_J83B_SYSTEM;
	chrdev_config.ops = &t230c_chrdev_ops;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = device;

	atomic_set(&device->available, 1);
	group_config.owner_kref = &device->kref;
	group_config.owner_kref_release = t230c_device_release;
	group_config.reserved = false;
	group_config.minor_base = 0;
	group_config.chrdev_num = T230C_CHRDEV_NUM;
	group_config.chrdev_config = &chrdev_config;
	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev, &group_config,
					  &chrdev_group);
	if (ret) {
		atomic_set(&device->available, 0);
		goto fail_bus;
	}
	device->chrdev_group = chrdev_group;
	device->chrdev = &chrdev_group->chrdev[0];
	device->stream.chrdev = device->chrdev;
	dev_info(dev, "MyGica T230C2 receiver registered (bulk IN 0x82)\n");
	return 0;

fail_firmware:
	if (demod_firmware)
		release_firmware(demod_firmware);
fail_bus:
	itedtv_bus_term(&device->stream_bus);
fail_device_ref:
	mutex_destroy(&device->operation_lock);
	put_device(dev);
	return ret;
}

static void t230c_device_release(struct kref *kref)
{
	struct t230c_device *device = container_of(kref, struct t230c_device, kref);

	if (device->streaming)
		itedtv_bus_stop_streaming(&device->stream_bus);
	itedtv_bus_term(&device->stream_bus);
	mutex_destroy(&device->operation_lock);
	put_device(device->dev);
	complete(device->quit_completion);
}

void t230c_device_term(struct t230c_device *device)
{
	atomic_xchg(&device->available, 0);
	mutex_lock(&device->operation_lock);
	WRITE_ONCE(device->capture_enabled, false);
	t230c_stop_usb_stream(device);
	mutex_unlock(&device->operation_lock);
	ptx_chrdev_group_destroy(device->chrdev_group);
	kref_put(&device->kref, t230c_device_release);
}

MODULE_FIRMWARE("dvb-demod-si2168-d60-01.fw");
MODULE_FIRMWARE("dvb_driver_si2141_rom60.fw");
MODULE_FIRMWARE("dvb_driver_si2141_rom61.fw");
MODULE_FIRMWARE("dvb-tuner-si2141-a10-01.fw");
