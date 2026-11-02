// SPDX-License-Identifier: GPL-2.0-or-later
/* Siano RIO backend for PLEX PX-Q1UD/PX-S1UD. */

#include "print_format.h"
#include "siano_device.h"

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "px4_device_params.h"
#include "px4_usb_params.h"
#include "siano_protocol_core.h"
#include "siano_protocol_defs.h"
#include "siano_wire_core.h"
#include "ts_sync.h"

#define SIANO_FIRMWARE_NAME "isdbt_rio.inp"
#define SIANO_MAX_FIRMWARE_SIZE (1024U * 1024U)
#define SIANO_COMMAND_TIMEOUT_MS 1000U
#define SIANO_INIT_TIMEOUT_MS 30000U
#define SIANO_USB_BUFFER_SIZE 0x2000U
#define SIANO_USB_URB_NUM 10U
#define SIANO_TS_SYNC_COUNT 4U
#define SIANO_TS_SYNC_SIZE (188U * SIANO_TS_SYNC_COUNT)

static void siano_device_release(struct kref *kref);

static u32 siano_read_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void siano_write_le32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static void siano_pack_header(u8 *message, u16 type, u8 source,
			      u8 destination, u16 length)
{
	siano_wire_pack_header(message, type, source, destination, length, 0);
}

static int siano_send(struct siano_device *device, const u8 *message,
		      size_t length, unsigned long deadline)
{
	int timeout;
	int ret;
	unsigned long remaining;

	if (!atomic_read(&device->available))
		return -ENODEV;
	if (length > INT_MAX || length < SIANO_SMS_HEADER_SIZE)
		return -EINVAL;

	remaining = deadline - jiffies;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	timeout = min_t(unsigned long,
			max_t(unsigned long, 1, jiffies_to_msecs(remaining)),
			SIANO_COMMAND_TIMEOUT_MS);

	mutex_lock(&device->control_lock);
	if (!atomic_read(&device->available)) {
		ret = -ENODEV;
		goto exit;
	}
	if (READ_ONCE(device->protocol_failed)) {
		ret = -EIO;
		goto exit;
	}
	device->bus.usb.ctrl_timeout = timeout;
	ret = itedtv_bus_ctrl_tx(&device->bus, (void *)message, (int)length);
exit:
	mutex_unlock(&device->control_lock);
	return ret;
}

static int siano_send_wait(struct siano_device *device, const u8 *message,
			   size_t length, u16 response_type,
			   unsigned long deadline)
{
	unsigned long wait_time;
	unsigned long flags;
	int ret;

	if (!atomic_read(&device->available))
		return -ENODEV;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;

	reinit_completion(&device->response_completion);
	spin_lock_irqsave(&device->state_lock, flags);
	device->expected_response = response_type;
	device->received_response = 0;
	device->response_error = 0;
	device->response_received = false;
	spin_unlock_irqrestore(&device->state_lock, flags);

	ret = siano_send(device, message, length, deadline);
	if (ret)
		goto fail;

	wait_time = deadline - jiffies;
	if (!wait_time)
		goto timeout;
	wait_time = min_t(unsigned long, wait_time,
			 msecs_to_jiffies(SIANO_COMMAND_TIMEOUT_MS));
	if (!wait_time)
		wait_time = 1;
	if (!wait_for_completion_timeout(&device->response_completion, wait_time))
		goto timeout;

	spin_lock_irqsave(&device->state_lock, flags);
	if (device->response_received &&
	    device->received_response == response_type)
		ret = device->response_error;
	else
		ret = -ENODEV;
	device->expected_response = 0;
	spin_unlock_irqrestore(&device->state_lock, flags);
	return ret;

timeout:
	ret = -ETIMEDOUT;
fail:
	spin_lock_irqsave(&device->state_lock, flags);
	device->expected_response = 0;
	if (ret != -ENOENT && ret != -EACCES)
		device->protocol_failed = true;
	spin_unlock_irqrestore(&device->state_lock, flags);
	return ret;
}

static void siano_stream_process(struct siano_stream_context *stream,
				 u8 **buffer, u32 *length)
{
	u8 *p = *buffer;
	u32 remain = *length;

	while (remain) {
		u32 packets = 0;
		bool partial_packet = false;

		while (true) {
			if ((packets + 1) * 188 <= remain) {
				if (!px4_ts_has_plain_sync(p[packets * 188]))
					break;
			} else {
				partial_packet = true;
				break;
			}
			packets++;
		}

		if (packets < SIANO_TS_SYNC_COUNT) {
			if (partial_packet)
				break;
			p++;
			remain--;
			continue;
		}

		ptx_chrdev_put_stream(stream->chrdev, p, 188 * packets);
		p += 188 * packets;
		remain -= 188 * packets;
		if (partial_packet)
			break;
	}

	*buffer = p;
	*length = remain;
}

static int siano_stream_handler(void *context, void *buffer, u32 length)
{
	struct siano_device *device = context;
	struct siano_protocol_event event;
	struct siano_stream_context *stream = &device->stream;
	unsigned long flags;
	int ret;

	if (!buffer || !length)
		return 0;

	ret = siano_protocol_decode_event(buffer, length,
					  device->response_alignment, &event);
	if (ret) {
		dev_warn_ratelimited(device->dev,
			"Siano: malformed USB message (length %u, error %d)\n",
			length, ret);
		return 0;
	}

	if (event.kind == SIANO_EVENT_TS) {
		if (!READ_ONCE(device->capture_enabled) || !stream->chrdev)
			return 0;

		spin_lock_irqsave(&stream->lock, flags);
		if (!READ_ONCE(device->capture_enabled)) {
			spin_unlock_irqrestore(&stream->lock, flags);
			return 0;
		}
		{
			u8 *p = (u8 *)event.payload;
			u32 remain = event.payload_length;

			while (stream->remain_len && remain) {
				u32 take = min(remain,
					SIANO_TS_SYNC_SIZE - stream->remain_len);
				u8 *pending;

				memcpy(stream->remain_buf + stream->remain_len, p, take);
				stream->remain_len += take;
				p += take;
				remain -= take;
				if (stream->remain_len < SIANO_TS_SYNC_SIZE)
					break;

				pending = stream->remain_buf;
				siano_stream_process(stream, &pending,
						    &stream->remain_len);
				if (stream->remain_len)
					memmove(stream->remain_buf, pending,
						stream->remain_len);
			}

			if (!stream->remain_len && remain) {
				siano_stream_process(stream, &p, &remain);
				if (remain) {
					memcpy(stream->remain_buf, p, remain);
					stream->remain_len = remain;
				}
			}
		}
		spin_unlock_irqrestore(&stream->lock, flags);
		return 0;
	}

	spin_lock_irqsave(&device->state_lock, flags);
	if (event.kind == SIANO_EVENT_RESPONSE) {
		if (event.message_type == SIANO_MSG_GET_VERSION_EX_RES) {
			device->firmware_id = event.firmware_id;
			device->supported_protocols = event.supported_protocols;
			device->firmware_version = event.firmware_version;
		}
		if (device->expected_response == event.message_type &&
		    !device->response_received) {
			device->received_response = event.message_type;
			device->response_error = 0;
			device->response_received = true;
			complete(&device->response_completion);
		}
	} else if (event.kind == SIANO_EVENT_STATISTICS) {
		device->locked = event.locked;
		if (event.has_cnr) {
			device->cnr_db = event.cnr_db;
			device->stats_valid = true;
		}
	} else if (event.kind == SIANO_EVENT_SIGNAL_DETECTED) {
		device->locked = true;
	} else if (event.kind == SIANO_EVENT_NO_SIGNAL) {
		device->locked = false;
	}
	spin_unlock_irqrestore(&device->state_lock, flags);

	return 0;
}

static int siano_get_version(struct siano_device *device,
			     unsigned long deadline)
{
	u8 message[SIANO_SMS_HEADER_SIZE];

	siano_pack_header(message, SIANO_MSG_GET_VERSION_EX_REQ,
			  0, SIANO_HIF_TASK, sizeof(message));
	return siano_send_wait(device, message, sizeof(message),
			       SIANO_MSG_GET_VERSION_EX_RES, deadline);
}

static int siano_load_firmware(struct siano_device *device,
			       const struct firmware *firmware,
			       u8 current_mode, unsigned long deadline)
{
	int ret;
	size_t offset;
	u32 address;
	u8 message[SIANO_SMS_HEADER_SIZE + 4 + SIANO_SMS_MAX_PAYLOAD_SIZE];
	u8 *payload;
	struct siano_firmware_header header;

	if (firmware->size < 12 || firmware->size > SIANO_MAX_FIRMWARE_SIZE)
		return -EBADMSG;
	ret = siano_wire_parse_firmware_header(firmware->data,
						firmware->size, &header);
	if (ret)
		return ret;
	if (firmware->size > (size_t)(0xffffffffU - header.start_address))
		return -EFBIG;

	payload = kzalloc(firmware->size, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	memcpy(payload, firmware->data + 12, header.length);
	address = header.start_address;

	if (current_mode != SIANO_DEVICE_MODE_NONE) {
		u8 reload[SIANO_SMS_HEADER_SIZE];

		siano_pack_header(reload, SIANO_MSG_SW_RELOAD_START_REQ,
				  0, SIANO_HIF_TASK, sizeof(reload));
		ret = siano_send_wait(device, reload, sizeof(reload),
				      SIANO_MSG_SW_RELOAD_START_RES, deadline);
		if (ret)
			goto exit;
		if (header.length < 24) {
			ret = -EBADMSG;
			goto exit;
		}
		address = siano_read_le32(payload + 20);
		if (firmware->size > (size_t)(0xffffffffU - address)) {
			ret = -EFBIG;
			goto exit;
		}
	}

	for (offset = 0; offset < firmware->size;) {
		size_t chunk = min_t(size_t, SIANO_SMS_MAX_PAYLOAD_SIZE,
				     firmware->size - offset);
		size_t message_length = SIANO_SMS_HEADER_SIZE + 4 + chunk;

		siano_pack_header(message, SIANO_MSG_DATA_DOWNLOAD_REQ,
				  0, SIANO_HIF_TASK, message_length);
		siano_write_le32(message + SIANO_SMS_HEADER_SIZE, address);
		memcpy(message + SIANO_SMS_HEADER_SIZE + 4,
		       payload + offset, chunk);
		ret = siano_send_wait(device, message, message_length,
				      SIANO_MSG_DATA_DOWNLOAD_RES, deadline);
		if (ret)
			goto exit;
		offset += chunk;
		address += chunk;
	}

	{
		u8 validity[SIANO_SMS_HEADER_SIZE + 12];

		siano_pack_header(validity, SIANO_MSG_DATA_VALIDITY_REQ,
				  0, SIANO_HIF_TASK, sizeof(validity));
		siano_write_le32(validity + SIANO_SMS_HEADER_SIZE,
				 header.start_address);
		siano_write_le32(validity + SIANO_SMS_HEADER_SIZE + 4,
				 header.length);
		siano_write_le32(validity + SIANO_SMS_HEADER_SIZE + 8, 0);
		ret = siano_send_wait(device, validity, sizeof(validity),
				      SIANO_MSG_DATA_VALIDITY_RES, deadline);
		if (ret)
			goto exit;
	}

	if (current_mode == SIANO_DEVICE_MODE_NONE) {
		u8 trigger[SIANO_SMS_HEADER_SIZE + 20];

		siano_pack_header(trigger, SIANO_MSG_SWDOWNLOAD_TRIGGER_REQ,
				  0, SIANO_HIF_TASK, sizeof(trigger));
		siano_write_le32(trigger + SIANO_SMS_HEADER_SIZE,
				 header.start_address);
		siano_write_le32(trigger + SIANO_SMS_HEADER_SIZE + 4,
				 SIANO_DEVICE_MODE_ISDBT_BDA);
		siano_write_le32(trigger + SIANO_SMS_HEADER_SIZE + 8, 0x200);
		siano_write_le32(trigger + SIANO_SMS_HEADER_SIZE + 12, 0);
		siano_write_le32(trigger + SIANO_SMS_HEADER_SIZE + 16, 4);
		ret = siano_send_wait(device, trigger, sizeof(trigger),
				      SIANO_MSG_SWDOWNLOAD_TRIGGER_RES, deadline);
	} else {
		u8 execute[SIANO_SMS_HEADER_SIZE];

		siano_pack_header(execute, SIANO_MSG_SW_RELOAD_EXEC_REQ,
				  0, SIANO_HIF_TASK, sizeof(execute));
		ret = siano_send(device, execute, sizeof(execute), deadline);
	}
	if (ret)
		goto exit;

	if (time_after_eq(jiffies + msecs_to_jiffies(400), deadline)) {
		ret = -ETIMEDOUT;
		goto exit;
	}
	msleep(400);

exit:
	kfree(payload);
	return ret;
}

static int siano_protocol_init(struct siano_device *device)
{
	int ret;
	unsigned long deadline = jiffies + msecs_to_jiffies(SIANO_INIT_TIMEOUT_MS);
	u8 firmware_id, supported_protocols;
	u16 firmware_version;
	const struct firmware *firmware = NULL;

	ret = siano_get_version(device, deadline);
	if (ret)
		return ret;

	spin_lock_irq(&device->state_lock);
	firmware_id = device->firmware_id;
	supported_protocols = device->supported_protocols;
	firmware_version = device->firmware_version;
	spin_unlock_irq(&device->state_lock);

	if (firmware_id != SIANO_DEVICE_MODE_ISDBT_BDA) {
		if (!(supported_protocols &
		      (1U << SIANO_DEVICE_MODE_ISDBT_BDA))) {
			ret = request_firmware(&firmware, SIANO_FIRMWARE_NAME,
					       device->dev);
			if (ret)
				return ret;
			ret = siano_load_firmware(device, firmware, firmware_id,
						  deadline);
			release_firmware(firmware);
			if (ret)
				return ret;
			ret = siano_get_version(device, deadline);
			if (ret)
				return ret;
			spin_lock_irq(&device->state_lock);
			firmware_id = device->firmware_id;
			supported_protocols = device->supported_protocols;
			firmware_version = device->firmware_version;
			spin_unlock_irq(&device->state_lock);
		}

		if (firmware_id != SIANO_DEVICE_MODE_ISDBT_BDA &&
		    firmware_version >= 0x0800) {
			u8 init[SIANO_SMS_HEADER_SIZE + 4];

			siano_pack_header(init, SIANO_MSG_INIT_DEVICE_REQ,
					  0, SIANO_HIF_TASK, sizeof(init));
			siano_write_le32(init + SIANO_SMS_HEADER_SIZE,
					 SIANO_DEVICE_MODE_ISDBT_BDA);
			ret = siano_send_wait(device, init, sizeof(init),
					      SIANO_MSG_INIT_DEVICE_RES, deadline);
			if (ret)
				return ret;
		}
	}

	spin_lock_irq(&device->state_lock);
	firmware_id = device->firmware_id;
	supported_protocols = device->supported_protocols;
	if (firmware_id == SIANO_DEVICE_MODE_ISDBT_BDA ||
	    (supported_protocols & (1U << SIANO_DEVICE_MODE_ISDBT_BDA)))
		device->protocol_initialized = true;
	else
		ret = -EOPNOTSUPP;
	spin_unlock_irq(&device->state_lock);

	return ret;
}

static int siano_ensure_streaming(struct siano_device *device)
{
	int ret;

	if (device->streaming)
		return 0;
	spin_lock_irq(&device->stream.lock);
	device->stream.remain_len = 0;
	spin_unlock_irq(&device->stream.lock);
	ret = itedtv_bus_start_streaming(&device->bus,
					 siano_stream_handler, device);
	if (!ret)
		device->streaming = true;
	return ret;
}

static int siano_ensure_protocol(struct siano_device *device)
{
	int ret;
	unsigned long flags;
	bool initialized, failed;

	spin_lock_irqsave(&device->state_lock, flags);
	initialized = device->protocol_initialized;
	failed = device->protocol_failed;
	spin_unlock_irqrestore(&device->state_lock, flags);
	if (!atomic_read(&device->available))
		return -ENODEV;
	if (failed)
		return -EIO;
	ret = siano_ensure_streaming(device);
	if (ret || initialized)
		return ret;
	ret = siano_protocol_init(device);
	if (ret) {
		if (device->streaming) {
			itedtv_bus_stop_streaming(&device->bus);
			device->streaming = false;
		}
		return ret;
	}
	return 0;
}

static int siano_chrdev_init(struct ptx_chrdev *chrdev)
{
	chrdev->params.system = PTX_ISDB_T_SYSTEM;
	return 0;
}

static int siano_chrdev_term(struct ptx_chrdev *chrdev)
{
	return 0;
}

static int siano_chrdev_open(struct ptx_chrdev *chrdev)
{
	struct siano_device *device = chrdev->priv;

	if (!atomic_read(&device->available))
		return -ENODEV;
	/* add_group() が cdev を公開した直後に open されても受信先を設定する。 */
	device->chrdev = chrdev;
	device->stream.chrdev = chrdev;
	return 0;
}

static int siano_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct siano_device *device = chrdev->priv;
	unsigned long flags;

	mutex_lock(&device->operation_lock);
	WRITE_ONCE(device->capture_enabled, false);
	if (device->streaming) {
		itedtv_bus_stop_streaming(&device->bus);
		device->streaming = false;
	}
	spin_lock_irqsave(&device->stream.lock, flags);
	device->stream.remain_len = 0;
	spin_unlock_irqrestore(&device->stream.lock, flags);
	mutex_unlock(&device->operation_lock);
	return 0;
}

static int siano_chrdev_tune(struct ptx_chrdev *chrdev,
			     struct ptx_tune_params *params)
{
	struct siano_device *device = chrdev->priv;
	unsigned long flags;
	unsigned long deadline;
	u64 frequency_hz;
	u8 message[SIANO_SMS_HEADER_SIZE + 16];
	int ret;
	bool was_capturing;

	if (params->system != PTX_ISDB_T_SYSTEM)
		return -EINVAL;
	frequency_hz = (u64)params->freq * 1000U;
	if (!frequency_hz || frequency_hz > 0xffffffffU)
		return -ERANGE;

	mutex_lock(&device->operation_lock);
	ret = siano_ensure_protocol(device);
	if (ret)
		goto exit;

	was_capturing = READ_ONCE(device->capture_enabled);
	WRITE_ONCE(device->capture_enabled, false);
	spin_lock_irqsave(&device->stream.lock, flags);
	device->stream.remain_len = 0;
	spin_unlock_irqrestore(&device->stream.lock, flags);
	spin_lock_irqsave(&device->state_lock, flags);
	device->locked = false;
	device->stats_valid = false;
	spin_unlock_irqrestore(&device->state_lock, flags);

	deadline = jiffies + msecs_to_jiffies(3000);
	siano_pack_header(message, SIANO_MSG_ISDBT_TUNE_REQ,
			  SIANO_DVBT_BDA_CONTROL_MSG_ID, SIANO_HIF_TASK,
			  sizeof(message));
	siano_write_le32(message + SIANO_SMS_HEADER_SIZE,
			 (u32)frequency_hz);
	siano_write_le32(message + SIANO_SMS_HEADER_SIZE + 4,
			 SIANO_BW_ISDBT_13SEG);
	siano_write_le32(message + SIANO_SMS_HEADER_SIZE + 8, 12000000U);
	siano_write_le32(message + SIANO_SMS_HEADER_SIZE + 12, 0);
	ret = siano_send_wait(device, message, sizeof(message),
			      SIANO_MSG_ISDBT_TUNE_RES, deadline);
	if (!ret) {
		/* Tune 応答より先に届いた前局のロック通知を次局へ持ち越さない。 */
		spin_lock_irqsave(&device->state_lock, flags);
		device->locked = false;
		device->stats_valid = false;
		device->next_stats_request = 0;
		spin_unlock_irqrestore(&device->state_lock, flags);

		if (!device->pid_filter_configured) {
			u8 filter[SIANO_SMS_HEADER_SIZE + 4];

			siano_pack_header(filter, SIANO_MSG_ADD_PID_FILTER_REQ,
					  SIANO_DVBT_BDA_CONTROL_MSG_ID,
					  SIANO_HIF_TASK, sizeof(filter));
			siano_write_le32(filter + SIANO_SMS_HEADER_SIZE,
					 SIANO_PID_CATCH_ALL);
			ret = siano_send(device, filter, sizeof(filter),
					 jiffies + msecs_to_jiffies(
						 SIANO_COMMAND_TIMEOUT_MS));
			if (!ret)
				device->pid_filter_configured = true;
		}
	}
	WRITE_ONCE(device->capture_enabled, was_capturing);
exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int siano_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	struct siano_device *device = chrdev->priv;
	unsigned long flags;
	unsigned long next;
	u16 firmware_version;
	int ret = 0;

	mutex_lock(&device->operation_lock);
	if (!atomic_read(&device->available)) {
		ret = -ENODEV;
		goto exit;
	}
	spin_lock_irqsave(&device->state_lock, flags);
	*locked = device->locked;
	next = device->next_stats_request;
	firmware_version = device->firmware_version;
	spin_unlock_irqrestore(&device->state_lock, flags);

	if (device->streaming && time_after_eq(jiffies, next)) {
		u8 message[SIANO_SMS_HEADER_SIZE];
		u16 request = firmware_version >= 0x0800
			? SIANO_MSG_GET_STATISTICS_EX_REQ
			: SIANO_MSG_GET_STATISTICS_REQ;

		siano_pack_header(message, request, SIANO_DVBT_BDA_CONTROL_MSG_ID,
				  SIANO_HIF_TASK, sizeof(message));
		ret = siano_send(device, message, sizeof(message),
				 jiffies + msecs_to_jiffies(SIANO_COMMAND_TIMEOUT_MS));
		if (!ret) {
			spin_lock_irqsave(&device->state_lock, flags);
			device->next_stats_request = jiffies + msecs_to_jiffies(100);
			*locked = device->locked;
			spin_unlock_irqrestore(&device->state_lock, flags);
		}
	}

	exit:
	mutex_unlock(&device->operation_lock);
	return ret;
}

static int siano_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	struct siano_device *device = chrdev->priv;
	int ret = 0;
	unsigned long flags;

	mutex_lock(&device->operation_lock);
	if (status) {
		ret = siano_ensure_protocol(device);
		if (!ret) {
			spin_lock_irqsave(&device->stream.lock, flags);
			device->stream.remain_len = 0;
			spin_unlock_irqrestore(&device->stream.lock, flags);
			WRITE_ONCE(device->capture_enabled, true);
		}
	} else {
		WRITE_ONCE(device->capture_enabled, false);
		if (device->streaming) {
			ret = itedtv_bus_stop_streaming(&device->bus);
			device->streaming = false;
		}
		spin_lock_irqsave(&device->stream.lock, flags);
		device->stream.remain_len = 0;
		spin_unlock_irqrestore(&device->stream.lock, flags);
	}
	mutex_unlock(&device->operation_lock);
	return ret;
}

static struct ptx_chrdev_operations siano_chrdev_ops = {
	.init = siano_chrdev_init,
	.term = siano_chrdev_term,
	.open = siano_chrdev_open,
	.release = siano_chrdev_release,
	.tune = siano_chrdev_tune,
	.check_lock = siano_chrdev_check_lock,
	.set_stream_id = NULL,
	.set_lnb_voltage = NULL,
	.set_capture = siano_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = NULL,
};

static int siano_find_endpoints(struct siano_device *device,
				struct usb_interface *intf)
{
	int i;
	bool has_control_out = false;
	struct usb_host_interface *setting = intf->cur_altsetting;

	if (setting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	for (i = 0; i < setting->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *endpoint =
			&setting->endpoint[i].desc;
		u16 max_packet;

		if (!usb_endpoint_xfer_bulk(endpoint))
			continue;
		if (endpoint->bEndpointAddress == 0x02)
			has_control_out = true;
		if (usb_endpoint_dir_in(endpoint) && !device->in_endpoint) {
			device->in_endpoint = endpoint->bEndpointAddress;
			max_packet = usb_endpoint_maxp(endpoint);
			device->response_alignment = max_packet > SIANO_SMS_HEADER_SIZE
				? max_packet - SIANO_SMS_HEADER_SIZE : 0;
		}
	}

	return device->in_endpoint && has_control_out ? 0 : -ENODEV;
}

int siano_device_init(struct siano_device *device, struct device *dev,
		      struct usb_interface *intf,
		      struct ptx_chrdev_context *chrdev_ctx,
		      struct completion *quit_completion)
{
	int ret;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ptx_chrdev_config chrdev_config = {};
	struct ptx_chrdev_group_config group_config = {};

	if (intf->num_altsetting > 1) {
		ret = usb_set_interface(usb_dev,
					intf->cur_altsetting->desc.bInterfaceNumber, 0);
		if (ret)
			return ret;
	}

	ret = siano_find_endpoints(device, intf);
	if (ret)
		return ret;

	get_device(dev);
	kref_init(&device->kref);
	atomic_set(&device->available, 0);
	device->dev = dev;
	device->quit_completion = quit_completion;
	device->firmware_id = SIANO_DEVICE_MODE_NONE;
	mutex_init(&device->operation_lock);
	mutex_init(&device->control_lock);
	spin_lock_init(&device->state_lock);
	spin_lock_init(&device->stream.lock);
	init_completion(&device->response_completion);
	device->stream.chrdev = NULL;
	device->stream.remain_len = 0;

	device->bus.dev = dev;
	device->bus.type = ITEDTV_BUS_USB;
	device->bus.usb.dev = usb_dev;
	device->bus.usb.ctrl_timeout = SIANO_COMMAND_TIMEOUT_MS;
	device->bus.usb.streaming.urb_buffer_size = SIANO_USB_BUFFER_SIZE;
	device->bus.usb.streaming.urb_num = SIANO_USB_URB_NUM;
	device->bus.usb.streaming.endpoint = device->in_endpoint;
	device->bus.usb.streaming.no_dma = px4_usb_params.no_dma;
	ret = itedtv_bus_init(&device->bus);
	if (ret)
		goto fail_device_ref;

	chrdev_config.system_cap = PTX_ISDB_T_SYSTEM;
	chrdev_config.ops = &siano_chrdev_ops;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = device;

	atomic_set(&device->available, 1);
	group_config.owner_kref = &device->kref;
	group_config.owner_kref_release = siano_device_release;
	group_config.reserved = false;
	group_config.minor_base = 0;
	group_config.chrdev_num = SIANO_CHRDEV_NUM;
	group_config.chrdev_config = &chrdev_config;
	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev, &group_config,
					  &device->chrdev_group);
	if (ret)
		goto fail_bus;

	device->chrdev = &device->chrdev_group->chrdev[0];
	device->stream.chrdev = device->chrdev;
	dev_info(dev, "Siano RIO receiver registered (bulk IN endpoint 0x%02x).\n",
		 device->in_endpoint);
	return 0;

fail_bus:
	atomic_set(&device->available, 0);
	itedtv_bus_term(&device->bus);
fail_device_ref:
	put_device(dev);
	return ret;
}

static void siano_device_release(struct kref *kref)
{
	struct siano_device *device = container_of(kref, struct siano_device, kref);

	if (device->streaming)
		itedtv_bus_stop_streaming(&device->bus);
	itedtv_bus_term(&device->bus);
	mutex_destroy(&device->control_lock);
	mutex_destroy(&device->operation_lock);
	put_device(device->dev);
	complete(device->quit_completion);
}

void siano_device_term(struct siano_device *device)
{
	unsigned long flags;

	atomic_xchg(&device->available, 0);
	spin_lock_irqsave(&device->state_lock, flags);
	device->protocol_failed = true;
	device->locked = false;
	device->response_error = -ENODEV;
	device->response_received = true;
	device->received_response = device->expected_response;
	device->expected_response = 0;
	spin_unlock_irqrestore(&device->state_lock, flags);
	complete_all(&device->response_completion);

	mutex_lock(&device->operation_lock);
	WRITE_ONCE(device->capture_enabled, false);
	if (device->streaming) {
		itedtv_bus_stop_streaming(&device->bus);
		device->streaming = false;
	}
	mutex_unlock(&device->operation_lock);

	ptx_chrdev_group_destroy(device->chrdev_group);
	kref_put(&device->kref, siano_device_release);
}
