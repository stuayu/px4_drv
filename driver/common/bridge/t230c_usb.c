/* SPDX-License-Identifier: GPL-2.0-only */

#include "t230c_usb.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define T230C_USB_MAX_COMMAND_SIZE 64U
#define T230C_USB_MAX_I2C_SIZE 60U

static t230c_u32 t230c_usb_timeout(struct t230c_usb *usb,
				   t230c_u32 timeout_ms)
{
	t230c_u64 remaining;

	if (!usb || !timeout_ms)
		return 0;
	if (usb->deadline_ms == ~(t230c_u64)0 || !usb->ops->now_ms)
		return timeout_ms;
	{
		t230c_u64 now = usb->ops->now_ms(usb->context);
		if (now >= usb->deadline_ms)
			return 0;
		remaining = usb->deadline_ms - now;
	}
	if (remaining > ~(t230c_u32)0)
		remaining = ~(t230c_u32)0;
	if (remaining < timeout_ms)
		timeout_ms = (t230c_u32)remaining;
	return timeout_ms;
}

int t230c_usb_init(struct t230c_usb *usb, const struct t230c_usb_ops *ops,
		   void *context, t230c_u32 timeout_ms)
{
	if (!usb || !ops || !ops->bulk_transfer || !timeout_ms)
		return -EINVAL;

	memset(usb, 0, sizeof(*usb));
	usb->ops = ops;
	usb->context = context;
	usb->ctrl_timeout_ms = timeout_ms;
	usb->deadline_ms = ~(t230c_u64)0;
	usb->i2c_master.gate_ctrl = t230c_usb_i2c_gate_ctrl;
	usb->i2c_master.request = t230c_usb_i2c_request;
	usb->i2c_master.priv = usb;
	return 0;
}

void t230c_usb_set_deadline(struct t230c_usb *usb, t230c_u64 deadline_ms)
{
	if (usb)
		usb->deadline_ms = deadline_ms;
}

int t230c_usb_command(struct t230c_usb *usb, const t230c_u8 *write,
		      size_t write_size, t230c_u8 *response,
		      size_t response_size, t230c_u32 timeout_ms)
{
	size_t actual = 0;
	int ret;

	if (!usb || !usb->ops || !write || !write_size ||
	    write_size > T230C_USB_MAX_COMMAND_SIZE ||
	    response_size > T230C_USB_MAX_COMMAND_SIZE ||
	    (response_size && !response))
		return -EINVAL;
	timeout_ms = t230c_usb_timeout(usb, timeout_ms);
	if (!timeout_ms)
		return -ETIMEDOUT;
	ret = usb->ops->bulk_transfer(usb->context, 0x01,
				     (t230c_u8 *)write, write_size,
				     &actual, timeout_ms);
	if (ret)
		return ret;
	if (actual != write_size)
		return -EIO;
	if (!response_size)
		return 0;

	actual = 0;
	timeout_ms = t230c_usb_timeout(usb, timeout_ms);
	if (!timeout_ms)
		return -ETIMEDOUT;
	ret = usb->ops->bulk_transfer(usb->context, 0x81, response,
				     response_size, &actual, timeout_ms);
	if (ret)
		return ret;
	return actual == response_size ? 0 : -EIO;
}

int t230c_usb_i2c_transfer(struct t230c_usb *usb, t230c_u8 address,
			   const t230c_u8 *write, size_t write_size,
			   t230c_u8 *read, size_t read_size,
			   t230c_u32 timeout_ms)
{
	t230c_u8 command[T230C_USB_MAX_COMMAND_SIZE];
	t230c_u8 response[T230C_USB_MAX_I2C_SIZE + 1];
	size_t command_size;
	size_t response_size;
	int ret;

	if (!usb || address > 0x7f || write_size > T230C_USB_MAX_I2C_SIZE ||
	    read_size > T230C_USB_MAX_I2C_SIZE ||
	    (write_size && !write) || (read_size && !read) ||
	    (!write_size && !read_size))
		return -EINVAL;

	if (!write_size) {
		command[0] = 0x09;
		command[1] = 0;
		command[2] = (t230c_u8)read_size;
		command[3] = address;
		command_size = 4;
	} else if (!read_size) {
		command[0] = 0x08;
		command[1] = address;
		command[2] = (t230c_u8)write_size;
		memcpy(command + 3, write, write_size);
		command_size = write_size + 3;
	} else {
		command[0] = 0x09;
		command[1] = (t230c_u8)write_size;
		command[2] = (t230c_u8)read_size;
		command[3] = address;
		memcpy(command + 4, write, write_size);
		command_size = write_size + 4;
	}

	response_size = read_size ? read_size + 1 : 1;
	ret = t230c_usb_command(usb, command, command_size,
				response, response_size, timeout_ms);
	if (!ret && read_size)
		memcpy(read, response + 1, read_size);
	return ret;
}

int t230c_usb_i2c_request(void *context,
			  const struct i2c_comm_request *requests, int count)
{
	struct t230c_usb *usb = context;
	const struct i2c_comm_request *write_request = NULL;
	const struct i2c_comm_request *read_request = NULL;
	t230c_u32 timeout_ms;

	if (!usb || !requests || (count != 1 && count != 2))
		return -EINVAL;
	if (count == 1) {
		if (requests[0].req == I2C_WRITE_REQUEST)
			write_request = &requests[0];
		else if (requests[0].req == I2C_READ_REQUEST)
			read_request = &requests[0];
		else
			return -EINVAL;
	} else {
		if (requests[0].req != I2C_WRITE_REQUEST ||
		    requests[1].req != I2C_READ_REQUEST ||
		    requests[0].addr != requests[1].addr)
			return -EINVAL;
		write_request = &requests[0];
		read_request = &requests[1];
	}
	if ((write_request && (write_request->len <= 0 || !write_request->data)) ||
	    (read_request && (read_request->len <= 0 || !read_request->data)))
		return -EINVAL;
	if ((write_request && write_request->addr > 0x7f) ||
	    (read_request && read_request->addr > 0x7f))
		return -EINVAL;
	timeout_ms = t230c_usb_timeout(usb, usb->ctrl_timeout_ms);
	if (!timeout_ms)
		return -ETIMEDOUT;
	return t230c_usb_i2c_transfer(usb,
				      (t230c_u8)(write_request ? write_request->addr : read_request->addr),
				      write_request ? write_request->data : NULL,
				      write_request ? (size_t)write_request->len : 0,
				      read_request ? read_request->data : NULL,
				      read_request ? (size_t)read_request->len : 0,
				      timeout_ms);
}

int t230c_usb_i2c_gate_ctrl(void *context, bool open)
{
	struct t230c_usb *usb = context;
	const t230c_u8 command[] = { 0xc0, 0x0d, open ? 0x01 : 0x00 };
	t230c_u8 response;

	return t230c_usb_i2c_transfer(usb, 0x64, command, sizeof(command),
				      &response, sizeof(response),
				      usb ? usb->ctrl_timeout_ms : 0);
}
