// SPDX-License-Identifier: GPL-2.0-only
/* CECH-ZD1 J Linux backend using the shared MB86H47A / DiB8096 core. */

#include "print_format.h"
#include "torne_device.h"

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>

#include "px4_device_params.h"
#include "px4_usb_params.h"

#define TORNE_CTRL_TIMEOUT_MS 1000U
#define TORNE_STREAM_URB_SIZE (1536U * 32U)
#define TORNE_STREAM_URB_NUM 8U
#define TORNE_STREAM_ENDPOINT 0x81U
#define TORNE_IDLE_FILL_MS 100U
#define TORNE_IDLE_PERIOD_MS 20U
#define TORNE_IDLE_PACKETS 64U
#define TORNE_OUTPUT_SIZE (TORNE_STREAM_URB_SIZE + TORNE_TS_PACKET_SIZE)

static void torne_device_release(struct kref *kref);

static int torne_control_timeout(void *priv, u8 request_type, u16 value,
				u16 index, void *buffer, u16 length,
				unsigned int timeout_ms)
{
	struct torne_device *device = priv;
	unsigned long deadline;
	unsigned long remaining;
	unsigned int pipe;
	int ret;

	if (!device || !device->usb_dev || !atomic_read(&device->available))
		return -ENODEV;
	if (length > 64 || (length && !buffer))
		return -EINVAL;
	if (!timeout_ms)
		return -ETIMEDOUT;
	if (timeout_ms > TORNE_CTRL_TIMEOUT_MS)
		timeout_ms = TORNE_CTRL_TIMEOUT_MS;
	deadline = jiffies + max_t(unsigned long, 1, msecs_to_jiffies(timeout_ms));
	for (;;) {
		if (mutex_trylock(&device->control_lock))
			break;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		if (msleep_interruptible(1))
			return -ERESTARTSYS;
	}
	if (!atomic_read(&device->available)) {
		mutex_unlock(&device->control_lock);
		return -ENODEV;
	}
	if (time_after_eq(jiffies, deadline)) {
		mutex_unlock(&device->control_lock);
		return -ETIMEDOUT;
	}
	remaining = deadline - jiffies;
	timeout_ms = max_t(unsigned int, 1, jiffies_to_msecs(remaining));
	pipe = (request_type & USB_DIR_IN) ? usb_rcvctrlpipe(device->usb_dev, 0) :
		usb_sndctrlpipe(device->usb_dev, 0);
	ret = usb_control_msg(device->usb_dev, pipe, 0xbc, request_type,
			      value, index, buffer, length, timeout_ms);
	mutex_unlock(&device->control_lock);
	return ret;
}

static int torne_control(void *priv, u8 request_type, u16 value, u16 index,
			 void *buffer, u16 length)
{
	return torne_control_timeout(priv, request_type, value, index, buffer, length,
				     TORNE_CTRL_TIMEOUT_MS);
}

static int torne_bulk(void *priv, u8 endpoint, void *buffer, int length,
		      unsigned int timeout_ms)
{
	(void)priv;
	(void)endpoint;
	(void)buffer;
	(void)length;
	(void)timeout_ms;
	return -ENOSYS;
}

static void torne_sleep(void *priv, unsigned int milliseconds)
{
	(void)priv;
	msleep(milliseconds);
}

static u64 torne_now_ms(void *priv)
{
	(void)priv;
	return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

static void torne_log(void *priv, int level, const char *format, ...)
{
	struct torne_device *device = priv;
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	if (level <= TORNE_LOG_ERR)
		dev_err(device->dev, "%pV\n", &vaf);
	else
		dev_info(device->dev, "%pV\n", &vaf);
	va_end(args);
}

static const struct torne_ops torne_operations = {
	.control = torne_control,
	.bulk = torne_bulk,
	.msleep = torne_sleep,
	.now_ms = torne_now_ms,
	.log = torne_log,
};

static int torne_card_read32(void *context, u32 address, u32 *value,
			     unsigned int timeout_ms)
{
	struct torne_device *device = context;
	u8 data[4];
	u16 request_value = (u16)(((address >> 8) & 0xff00) | (address >> 24));
	int ret;

	if (!device || !value || !atomic_read(&device->available) || !device->bridge_up)
		return -ENODEV;
	ret = torne_control_timeout(device, 0xc0, request_value, (u16)address,
				     data, sizeof(data), timeout_ms);
	if (ret < 0)
		return ret;
	if (ret != (int)sizeof(data))
		return -EIO;
	*value = (u32)data[0] << 24 | (u32)data[1] << 16 | (u32)data[2] << 8 | data[3];
	return 0;
}

static int torne_card_write32(void *context, u32 address, u32 value,
			      unsigned int timeout_ms)
{
	struct torne_device *device = context;
	u8 data[4] = { value >> 24, value >> 16, value >> 8, value };
	u16 request_value = (u16)(((address >> 8) & 0xff00) | (address >> 24));
	int ret;

	if (!device || !atomic_read(&device->available) || !device->bridge_up)
		return -ENODEV;
	ret = torne_control_timeout(device, 0x40, request_value, (u16)address,
				     data, sizeof(data), timeout_ms);
	if (ret < 0)
		return ret;
	return ret == (int)sizeof(data) ? 0 : -EIO;
}

static int torne_card_lock(void *context)
{
	struct torne_device *device = context;

	if (mutex_lock_interruptible(&device->card_uart_lock))
		return -ERESTARTSYS;
	if (!atomic_read(&device->available) || !device->bridge_up) {
		mutex_unlock(&device->card_uart_lock);
		return -ENODEV;
	}
	return 0;
}

static void torne_card_unlock(void *context)
{
	struct torne_device *device = context;

	mutex_unlock(&device->card_uart_lock);
}

static void torne_card_sleep(void *context, unsigned int milliseconds)
{
	(void)context;
	msleep(milliseconds);
}

static u64 torne_card_now_ms(void *context)
{
	(void)context;
	return div_u64(ktime_get_ns(), NSEC_PER_MSEC);
}

static const struct torne_card_io_ops torne_card_io_operations = {
	.read32 = torne_card_read32,
	.write32 = torne_card_write32,
	.lock = torne_card_lock,
	.unlock = torne_card_unlock,
	.sleep_ms = torne_card_sleep,
	.now_ms = torne_card_now_ms,
};

static int torne_card_power_on(void *context)
{
	struct torne_device *device = context;

	return atomic_read(&device->available) && device->bridge_up ? 0 : -ENODEV;
}

static int torne_card_power_off(void *context)
{
	struct torne_device *device = context;

	/* USB 切断後はカード電源参照の後始末だけを行い、解放済みデバイスへ転送しない。 */
	return !atomic_read(&device->available) ? 0 :
		torne_card_power_down(&device->card_port);
}

static int torne_backend_card_set_timeout(void *context, u32 timeout_ms)
{
	struct torne_device *device = context;

	return torne_card_port_set_timeout(&device->card_port, timeout_ms);
}

static int torne_backend_card_detect(void *context, bool *present)
{
	struct torne_device *device = context;

	return torne_card_detect(&device->card_port, present);
}

static int torne_backend_card_reset(void *context)
{
	struct torne_device *device = context;

	return torne_card_reset(&device->card_port);
}

static int torne_backend_card_set_baudrate(void *context, u32 baudrate)
{
	struct torne_device *device = context;

	return torne_card_set_baudrate(&device->card_port, baudrate);
}

static int torne_backend_card_rx_ready(void *context, bool *ready)
{
	struct torne_device *device = context;

	return torne_card_rx_ready(&device->card_port, ready);
}

static int torne_backend_card_read(void *context, struct px4_card_frame *frame)
{
	struct torne_device *device = context;

	return torne_card_read(&device->card_port, frame->data, &frame->length);
}

static int torne_backend_card_write(void *context, const struct px4_card_frame *frame)
{
	struct torne_device *device = context;

	return torne_card_write(&device->card_port, frame->data, frame->length);
}

static const struct px4_card_operations torne_card_operations = {
	.power_on = torne_card_power_on,
	.power_off = torne_card_power_off,
	.set_timeout = torne_backend_card_set_timeout,
	.detect = torne_backend_card_detect,
	.reset = torne_backend_card_reset,
	.set_baudrate = torne_backend_card_set_baudrate,
	.rx_ready = torne_backend_card_rx_ready,
	.read = torne_backend_card_read,
	.write = torne_backend_card_write,
};

static void torne_stream_reset(struct torne_device *device)
{
	unsigned long flags;

	if (!device->framer)
		return;
	spin_lock_irqsave(&device->stream_lock, flags);
	torne_framer_reset(device->framer);
	spin_unlock_irqrestore(&device->stream_lock, flags);
}

static int torne_stream_handler(void *context, void *buffer, u32 length)
{
	struct torne_device *device = context;
	unsigned long flags;
	size_t output_size;
	int ret = 0;

	if (!device || !buffer || !length || !device->chrdev ||
	    !device->framer || !device->ts_output)
		return 0;
	if (length > TORNE_STREAM_URB_SIZE)
		return -EOVERFLOW;
	spin_lock_irqsave(&device->stream_lock, flags);
	if (READ_ONCE(device->capture_enabled)) {
		output_size = torne_framer_feed(device->framer, buffer, length,
						device->ts_output);
		if (output_size) {
			ptx_chrdev_put_stream(device->chrdev, device->ts_output,
					      output_size);
			device->last_data = jiffies;
		}
	}
	spin_unlock_irqrestore(&device->stream_lock, flags);
	return ret;
}

static void torne_idle_work(struct work_struct *work)
{
	struct torne_device *device = container_of(to_delayed_work(work),
							   struct torne_device, idle_work);
	unsigned long flags;
	unsigned int packet;
	unsigned int i;

	if (!READ_ONCE(device->capture_enabled) || !READ_ONCE(device->streaming))
		return;
	spin_lock_irqsave(&device->stream_lock, flags);
	if (device->ts_output && time_after(jiffies,
			device->last_data + msecs_to_jiffies(TORNE_IDLE_FILL_MS))) {
		for (packet = 0; packet < TORNE_IDLE_PACKETS; ++packet) {
			u8 *ts = device->ts_output + packet * TORNE_TS_PACKET_SIZE;

			ts[0] = 0x47;
			ts[1] = 0x1f;
			ts[2] = 0xff;
			ts[3] = 0x10;
			for (i = 4; i < TORNE_TS_PACKET_SIZE; ++i)
				ts[i] = 0xff;
		}
		ptx_chrdev_put_stream(device->chrdev, device->ts_output,
				      TORNE_IDLE_PACKETS * TORNE_TS_PACKET_SIZE);
	}
	spin_unlock_irqrestore(&device->stream_lock, flags);
	if (READ_ONCE(device->capture_enabled) && READ_ONCE(device->streaming))
		schedule_delayed_work(&device->idle_work,
				      msecs_to_jiffies(TORNE_IDLE_PERIOD_MS));
}

/* Capture reads 192-byte bridge units and publishes only the 188-byte TS payloads. */
static int torne_start_stream(struct torne_device *device)
{
	int ret;

	if (device->streaming)
		return 0;
	if (!atomic_read(&device->available) || !device->ready)
		return -ENODEV;
	if (!device->framer) {
		device->framer = kvzalloc(sizeof(*device->framer), GFP_KERNEL);
		device->ts_output = kvzalloc(TORNE_OUTPUT_SIZE, GFP_KERNEL);
		if (!device->framer || !device->ts_output) {
			kvfree(device->framer);
			kvfree(device->ts_output);
			device->framer = NULL;
			device->ts_output = NULL;
			return -ENOMEM;
		}
	}
	torne_stream_reset(device);
	ret = torne_dib_set_output(&device->dib, TORNE_OUT_PARALLEL_GATED);
	if (ret)
		return ret;
	ret = torne_bridge_start_stream(&device->bridge);
	if (ret)
		goto fail_output;
	ret = itedtv_bus_start_streaming(&device->stream_bus,
					 torne_stream_handler, device);
	if (ret)
		goto fail_bridge;
	device->streaming = true;
	device->last_data = jiffies;
	schedule_delayed_work(&device->idle_work,
			      msecs_to_jiffies(TORNE_IDLE_PERIOD_MS));
	return 0;

fail_bridge:
	torne_bridge_stop_stream(&device->bridge);
fail_output:
	torne_dib_set_output(&device->dib, TORNE_OUT_HIGHZ);
	return ret;
}

static int torne_stop_stream(struct torne_device *device)
{
	int ret = 0;

	cancel_delayed_work_sync(&device->idle_work);
	if (device->streaming)
		ret = itedtv_bus_stop_streaming(&device->stream_bus);
	device->streaming = false;
	if (atomic_read(&device->available) && device->ready) {
		const int stop_ret = torne_bridge_stop_stream(&device->bridge);
		if (!ret)
			ret = stop_ret;
		const int output_ret = torne_dib_set_output(&device->dib, TORNE_OUT_HIGHZ);
		if (!ret)
			ret = output_ret;
	}
	torne_stream_reset(device);
	return ret;
}

static int torne_chrdev_init(struct ptx_chrdev *chrdev)
{
	(void)chrdev;
	return 0;
}

static int torne_chrdev_term(struct ptx_chrdev *chrdev)
{
	(void)chrdev;
	return 0;
}

static int torne_chrdev_open(struct ptx_chrdev *chrdev)
{
	struct torne_device *device = chrdev->priv;
	int ret;

	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available) || !device->ready)
		ret = -ENODEV;
	else
		ret = torne_dib_set_gpio(&device->dib, 1, true);
	mutex_unlock(&device->operation_lock);
	if (!ret)
		kref_get(&device->kref);
	return ret;
}

static int torne_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct torne_device *device = chrdev->priv;

	mutex_lock(&device->operation_lock);
	if (atomic_read(&device->available) && device->ready) {
		WRITE_ONCE(device->capture_enabled, false);
		torne_stop_stream(device);
		torne_dib_set_gpio(&device->dib, 1, false);
		torne_dib_sleep(&device->dib);
	}
	mutex_unlock(&device->operation_lock);
	kref_put(&device->kref, torne_device_release);
	return 0;
}

static int torne_chrdev_tune(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	struct torne_device *device = chrdev->priv;
	bool resume;
	int ret;

	if (!params || params->system != PTX_ISDB_T_SYSTEM ||
	    params->freq > U32_MAX / 1000U)
		return -EINVAL;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available) || !device->ready) {
		ret = -ENODEV;
		goto out;
	}
	resume = device->streaming;
	if (resume)
		torne_stop_stream(device);
	ret = torne_bridge_prepare_stream(&device->bridge);
	if (!ret)
		ret = torne_dib_tune(&device->dib, params->freq * 1000U);
	if (resume) {
		const int restart_ret = torne_start_stream(device);
		if (!ret)
			ret = restart_ret;
	}
out:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int torne_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	struct torne_device *device = chrdev->priv;
	struct torne_dib_status status;
	int ret;

	if (!locked)
		return -EINVAL;
	*locked = false;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available) || !device->ready)
		ret = -ENODEV;
	else {
		ret = torne_dib_status(&device->dib, &status);
		if (!ret)
			*locked = status.locked;
	}
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int torne_chrdev_set_lnb_voltage(struct ptx_chrdev *chrdev, int voltage)
{
	(void)chrdev;
	return voltage == 0 ? 0 : -EOPNOTSUPP;
}

static int torne_chrdev_set_capture(struct ptx_chrdev *chrdev, bool capture)
{
	struct torne_device *device = chrdev->priv;
	int ret = 0;

	mutex_lock(&device->operation_lock);
	if (capture) {
		if (!atomic_read(&device->available) || !device->ready) {
			ret = -ENODEV;
			goto out;
		}
		WRITE_ONCE(device->capture_enabled, true);
		ret = torne_start_stream(device);
		if (ret)
			WRITE_ONCE(device->capture_enabled, false);
	} else {
		WRITE_ONCE(device->capture_enabled, false);
		ret = torne_stop_stream(device);
	}
out:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int torne_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	struct torne_device *device = chrdev->priv;
	struct torne_dib_status status;
	int ret;

	if (!value)
		return -EINVAL;
	*value = 0;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available) || !device->ready)
		ret = -ENODEV;
	else {
		ret = torne_dib_status(&device->dib, &status);
		if (!ret && (!status.locked || !status.has_cnr))
			ret = -EAGAIN;
		/* ptx GET_CNR は torne_drv と同じ 0.01 dB 単位を返す。 */
		if (!ret)
			*value = (u32)status.cnr_x100;
	}
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int torne_chrdev_read_signal_strength(struct ptx_chrdev *chrdev,
						     u32 *value)
{
	struct torne_device *device = chrdev->priv;
	struct torne_dib_status status;
	int ret;

	if (!value)
		return -EINVAL;
	*value = 0;
	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available) || !device->ready)
		ret = -ENODEV;
	else {
		ret = torne_dib_status(&device->dib, &status);
		/* AGC 表からの推定値。校正済み dBm としては扱わない。 */
		if (!ret)
			*value = status.strength_dbm_x1000 > 0 ? 0 :
				(u32)(-status.strength_dbm_x1000);
	}
	mutex_unlock(&device->operation_lock);
	return ret;
}

static const struct ptx_chrdev_operations torne_chrdev_ops = {
	.init = torne_chrdev_init,
	.term = torne_chrdev_term,
	.open = torne_chrdev_open,
	.release = torne_chrdev_release,
	.tune = torne_chrdev_tune,
	.check_lock = torne_chrdev_check_lock,
	.set_stream_id = NULL,
	.set_lnb_voltage = torne_chrdev_set_lnb_voltage,
	.set_capture = torne_chrdev_set_capture,
	.read_signal_strength = torne_chrdev_read_signal_strength,
	.read_cnr = NULL,
	.read_cnr_raw = torne_chrdev_read_cnr_raw,
};

static void torne_device_release(struct kref *kref)
{
	struct torne_device *device = container_of(kref, struct torne_device, kref);

	cancel_delayed_work_sync(&device->idle_work);
	if (device->streaming)
		itedtv_bus_stop_streaming(&device->stream_bus);
	if (device->bus_initialized)
		itedtv_bus_term(&device->stream_bus);
	kvfree(device->framer);
	kvfree(device->ts_output);
	mutex_destroy(&device->card_uart_lock);
	mutex_destroy(&device->control_lock);
	mutex_destroy(&device->operation_lock);
	put_device(device->dev);
	complete(device->quit_completion);
}

int torne_device_init(struct torne_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion)
{
	struct ptx_chrdev_config chrdev_config = { 0 };
	struct ptx_chrdev_group_config group_config = { 0 };
	struct ptx_chrdev_group *chrdev_group;
	struct usb_device *usb_dev;
	int ret;

	if (!device || !dev || !intf || !chrdev_ctx || !quit_completion)
		return -EINVAL;
	usb_dev = interface_to_usbdev(intf);
	ret = usb_set_interface(usb_dev, intf->cur_altsetting->desc.bInterfaceNumber, 0);
	if (ret)
		return ret;
	memset(device, 0, sizeof(*device));
	get_device(dev);
	kref_init(&device->kref);
	atomic_set(&device->available, 0);
	device->dev = dev;
	device->usb_dev = usb_dev;
	device->quit_completion = quit_completion;
	mutex_init(&device->operation_lock);
	mutex_init(&device->control_lock);
	mutex_init(&device->card_uart_lock);
	spin_lock_init(&device->stream_lock);
	INIT_DELAYED_WORK(&device->idle_work, torne_idle_work);
	device->stream_bus.dev = dev;
	device->stream_bus.type = ITEDTV_BUS_USB;
	device->stream_bus.usb.dev = usb_dev;
	device->stream_bus.usb.ctrl_timeout = TORNE_CTRL_TIMEOUT_MS;
	device->stream_bus.usb.streaming.endpoint = TORNE_STREAM_ENDPOINT;
	device->stream_bus.usb.streaming.urb_buffer_size = TORNE_STREAM_URB_SIZE;
	device->stream_bus.usb.streaming.urb_num = TORNE_STREAM_URB_NUM;
	device->stream_bus.usb.streaming.no_dma = px4_usb_params.no_dma;
	ret = itedtv_bus_init(&device->stream_bus);
	if (ret)
		goto fail_device;
	device->bus_initialized = true;
	/* bridge の起動中も制御転送を受け付ける。probe 失敗時は fail_bus で戻す。 */
	atomic_set(&device->available, 1);
	torne_bridge_init(&device->bridge, &torne_operations, device);
	torne_card_port_init(&device->card_port, &torne_card_io_operations, device);
	ret = torne_bridge_start(&device->bridge);
	if (ret) {
		dev_err(dev, "torne MB86H47A bridge start failed: %d\n", ret);
		goto fail_bus;
	}
	device->bridge_up = true;
	ret = torne_bridge_reset_demodulator(&device->bridge);
	if (ret) {
		dev_err(dev, "torne DiB8096 reset failed: %d\n", ret);
		goto fail_bus;
	}
	torne_dib_setup(&device->dib, &device->bridge);
	ret = torne_dib_init(&device->dib);
	if (ret) {
		dev_err(dev, "torne DiB8096 initialization failed: %d\n", ret);
		goto fail_bus;
	}
	device->ready = true;
	ret = torne_dib_set_output(&device->dib, TORNE_OUT_HIGHZ);
	if (ret)
		goto fail_bus;

	chrdev_config.system_cap = PTX_ISDB_T_SYSTEM;
	chrdev_config.ops = &torne_chrdev_ops;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = device;
	atomic_set(&device->available, 1);
	group_config.owner_kref = &device->kref;
	group_config.owner_kref_release = torne_device_release;
	group_config.reserved = false;
	group_config.minor_base = 0;
	group_config.chrdev_num = TORNE_CHRDEV_NUM;
	group_config.chrdev_config = &chrdev_config;
	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev, &group_config,
					  &chrdev_group);
	if (ret) {
		atomic_set(&device->available, 0);
		goto fail_bus;
	}
	device->chrdev_group = chrdev_group;
	device->chrdev = &chrdev_group->chrdev[0];
	{
		struct px4_card_backend card_backend = {
			.it930x = NULL,
			.operations = &torne_card_operations,
			.lock = &device->operation_lock,
			.available = &device->available,
			.context = device,
		};
		ret = px4_card_register(dev, &card_backend, &device->card);
		if (ret)
			dev_warn(dev, "Card reader registration failed: %d\n", ret);
	}
	dev_info(dev, "torne receiver registered (bulk IN 0x81)\n");
	return 0;

fail_bus:
	if (device->bridge_up) {
		atomic_set(&device->available, 1);
		torne_bridge_stop_stream(&device->bridge);
		if (device->ready) {
			torne_dib_set_output(&device->dib, TORNE_OUT_HIGHZ);
			torne_dib_sleep(&device->dib);
		}
		device->bridge_up = false;
		device->ready = false;
	}
	atomic_set(&device->available, 0);
	if (device->bus_initialized) {
		itedtv_bus_term(&device->stream_bus);
		device->bus_initialized = false;
	}
fail_device:
	mutex_destroy(&device->card_uart_lock);
	mutex_destroy(&device->control_lock);
	mutex_destroy(&device->operation_lock);
	put_device(dev);
	return ret;
}

void torne_device_term(struct torne_device *device)
{
	/* 受付を閉じてからカード UART の VCC を停止し、最後にチューナーの利用可能状態を閉じる。 */
	px4_card_unregister(device->card);
	device->card = NULL;
	atomic_xchg(&device->available, 0);
	mutex_lock(&device->operation_lock);
	WRITE_ONCE(device->capture_enabled, false);
	torne_stop_stream(device);
	mutex_unlock(&device->operation_lock);
	ptx_chrdev_group_destroy(device->chrdev_group);
	kref_put(&device->kref, torne_device_release);
}
