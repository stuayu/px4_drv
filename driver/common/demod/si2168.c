/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux drivers/media/dvb-frontends/si2168.c のコマンド処理を共通化。 */

#include "si2168.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define SI2168_ADDRESS 0x64U
#define SI2168_ARGUMENT_SIZE 30U
#define SI2168_COMMAND_TIMEOUT_MS 140U
#define SI2168_FW_RECORD_SIZE 17U

static t230c_u64 si2168_deadline(struct si2168 *demod,
				  t230c_u64 requested_deadline)
{
	t230c_u64 now = demod->now_ms(demod->clock_context);
	t230c_u64 command_deadline = now + SI2168_COMMAND_TIMEOUT_MS;

	if (command_deadline < now)
		command_deadline = ~(t230c_u64)0;
	return requested_deadline < command_deadline
		? requested_deadline : command_deadline;
}

static int si2168_transfer(struct si2168 *demod, const t230c_u8 *write,
			   size_t write_size, t230c_u8 *read,
			   size_t read_size, t230c_u64 deadline_ms)
{
	struct i2c_comm_request request[1];

	if (demod->set_deadline)
		demod->set_deadline(demod->clock_context, deadline_ms);
	request[0].addr = SI2168_ADDRESS;
	if (write_size) {
		request[0].req = I2C_WRITE_REQUEST;
		request[0].data = (t230c_u8 *)write;
		request[0].len = (int)write_size;
	} else {
		request[0].req = I2C_READ_REQUEST;
		request[0].data = read;
		request[0].len = (int)read_size;
	}
	return i2c_comm_master_request(demod->i2c, request, 1);
}

static int si2168_command(struct si2168 *demod, const t230c_u8 *command,
			  size_t command_size, size_t response_size,
			  t230c_u64 requested_deadline, t230c_u8 *response)
{
	t230c_u8 args[SI2168_ARGUMENT_SIZE] = { 0 };
	t230c_u64 deadline;
	t230c_u64 now;
	int ret;

	if (!demod || !demod->now_ms || !demod->sleep_us ||
	    !command || !command_size || command_size > sizeof(args) ||
	    response_size > sizeof(args) || (response_size && !response))
		return -EINVAL;
	deadline = si2168_deadline(demod, requested_deadline);
	if (demod->now_ms(demod->clock_context) >= deadline)
		return -ETIMEDOUT;
	ret = si2168_transfer(demod, command, command_size, NULL, 0, deadline);
	if (ret || !response_size)
		goto exit;

	while ((now = demod->now_ms(demod->clock_context)) < deadline) {
		memset(args, 0, response_size);
		ret = si2168_transfer(demod, NULL, 0, args, response_size, deadline);
		if (ret)
			goto exit;
		if (demod->now_ms(demod->clock_context) >= deadline) {
			ret = -ETIMEDOUT;
			goto exit;
		}
		if (args[0] & 0x80) {
			if (args[0] & 0x40) {
				ret = -EIO;
				goto exit;
			}
			memcpy(response, args, response_size);
			ret = 0;
			goto exit;
		}
		demod->sleep_us(demod->clock_context, 3000);
	}
	ret = args[0] & 0x40 ? -EIO : -ETIMEDOUT;
exit:
	if (demod->set_deadline)
		demod->set_deadline(demod->clock_context, requested_deadline);
	return ret;
}

int si2168_validate_firmware(const t230c_u8 *data, size_t size)
{
	size_t offset;

	if (!data || !size || size % SI2168_FW_RECORD_SIZE)
		return -EINVAL;
	for (offset = 0; offset < size; offset += SI2168_FW_RECORD_SIZE) {
		if (!data[offset] || data[offset] > 16)
			return -EINVAL;
	}
	return 0;
}

static int si2168_download(struct si2168 *demod, const t230c_u8 *firmware,
			   size_t firmware_size)
{
	size_t offset;
	int ret;

	ret = si2168_validate_firmware(firmware, firmware_size);
	if (ret)
		return ret;
	for (offset = 0; offset < firmware_size; offset += SI2168_FW_RECORD_SIZE) {
		size_t length = firmware[offset];
		t230c_u8 response;
		ret = si2168_command(demod, firmware + offset + 1, length, 1,
				     ~(t230c_u64)0, &response);
		if (ret)
			return ret;
	}
	return 0;
}

int si2168_init(struct si2168 *demod,
		const struct i2c_comm_master *i2c,
		void *clock_context,
		t230c_u64 (*now_ms)(void *context),
		void (*sleep_us)(void *context, t230c_u32 usec),
		void (*set_deadline)(void *context, t230c_u64 deadline_ms),
		const t230c_u8 *firmware, size_t firmware_size,
		t230c_u8 ts_mode, bool ts_clock_inv)
{
	static const t230c_u8 initialize[] = {
		0xc0, 0x12, 0x00, 0x0c, 0x00, 0x0d, 0x16,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const t230c_u8 power_up[] = {
		0xc0, 0x06, 0x01, 0x0f, 0x00, 0x20, 0x20, 0x01
	};
	static const t230c_u8 chip_id[] = { 0x02 };
	static const t230c_u8 start_firmware[] = { 0x01, 0x01 };
	static const t230c_u8 firmware_version[] = { 0x11 };
	t230c_u8 response[13];
	int ret;

	if (!demod || !i2c || !now_ms || !sleep_us ||
	    si2168_validate_firmware(firmware, firmware_size))
		return -EINVAL;

	memset(demod, 0, sizeof(*demod));
	demod->i2c = i2c;
	demod->clock_context = clock_context;
	demod->now_ms = now_ms;
	demod->sleep_us = sleep_us;
	demod->set_deadline = set_deadline;
	demod->ts_mode = ts_mode;
	demod->ts_clock_inv = ts_clock_inv;
	demod->annex = SI2168_CABLE_ANNEX_A;

	ret = si2168_command(demod, initialize, sizeof(initialize), 0,
			     ~(t230c_u64)0, NULL);
	if (!ret)
		ret = si2168_command(demod, power_up, sizeof(power_up), 1,
				     ~(t230c_u64)0, response);
	if (!ret)
		ret = si2168_command(demod, chip_id, sizeof(chip_id), sizeof(response),
				     ~(t230c_u64)0, response);
	if (!ret)
		ret = si2168_download(demod, firmware, firmware_size);
	if (!ret)
		ret = si2168_command(demod, start_firmware, sizeof(start_firmware), 1,
				     ~(t230c_u64)0, response);
	if (!ret)
		ret = si2168_command(demod, firmware_version, sizeof(firmware_version), 10,
				     ~(t230c_u64)0, response);
	if (!ret)
		ret = si2168_set_ts_bus(demod, true, ~(t230c_u64)0);
	return ret;
}

int si2168_set_ts_bus(struct si2168 *demod, bool enabled,
		      t230c_u64 deadline_ms)
{
	t230c_u8 clock_command[] = { 0x14, 0x00, 0x0d, 0x10, 0xe8, 0x03 };
	t230c_u8 mode_command[] = { 0x14, 0x00, 0x01, 0x10, 0x10, 0x00 };
	t230c_u8 response[4];
	int ret;

	if (!demod)
		return -EINVAL;
	if (demod->ts_mode & 0x20) {
		ret = si2168_command(demod, clock_command, sizeof(clock_command),
				     sizeof(response), deadline_ms, response);
		if (ret)
			return ret;
		mode_command[4] = 0x20;
	}
	if (enabled)
		mode_command[4] |= demod->ts_mode;
	ret = si2168_command(demod, mode_command, sizeof(mode_command),
			     sizeof(response), deadline_ms, response);
	return ret;
}

static void si2168_add_command(struct si2168_command *commands,
			       size_t *count, const t230c_u8 *data,
			       t230c_u8 length, t230c_u8 response_length)
{
	struct si2168_command *command = &commands[(*count)++];

	memcpy(command->data, data, length);
	command->length = length;
	command->response_length = response_length;
}

int si2168_build_tune_commands(t230c_u32 symbol_rate_ksps,
			       t230c_u32 bandwidth_mhz, t230c_u8 ts_mode,
			       bool ts_clock_inv,
			       struct si2168_command *commands,
			       size_t capacity, size_t *count)
{
	return si2168_build_cable_tune_commands(SI2168_CABLE_ANNEX_A,
						symbol_rate_ksps, bandwidth_mhz,
						ts_mode, ts_clock_inv,
						commands, capacity, count);
}

int si2168_build_cable_tune_commands(enum si2168_cable_annex annex,
				     t230c_u32 symbol_rate_ksps,
				     t230c_u32 bandwidth_mhz, t230c_u8 ts_mode,
				     bool ts_clock_inv,
				     struct si2168_command *commands,
				     size_t capacity, size_t *count)
{
	static const t230c_u8 initial[][6] = {
		{ 0x88, 0x02, 0x02, 0x02, 0x02, 0x00 },
		{ 0x89, 0x21, 0x06, 0x11, 0x89, 0xf0 },
		{ 0x51, 0x03, 0x00, 0x00, 0x00, 0x00 },
		{ 0x12, 0x08, 0x04, 0x00, 0x00, 0x00 },
		{ 0x14, 0x00, 0x0c, 0x10, 0x12, 0x00 },
		{ 0x14, 0x00, 0x06, 0x10, 0x24, 0x00 },
		{ 0x14, 0x00, 0x07, 0x10, 0x00, 0x24 },
		{ 0x14, 0x00, 0x0a, 0x10, 0x36, 0x00 },
		{ 0x14, 0x00, 0x02, 0x11, 0x00, 0x00 },
		{ 0x14, 0x00, 0x0f, 0x10, 0x10, 0x00 },
		{ 0x14, 0x00, 0x09, 0x10, 0xe3, 0x08 },
		{ 0x14, 0x00, 0x08, 0x10, 0xd7, 0x05 },
		{ 0x14, 0x00, 0x01, 0x12, 0x00, 0x00 },
		{ 0x14, 0x00, 0x01, 0x03, 0x0c, 0x00 },
		{ 0x85, 0x00, 0x00, 0x00, 0x00, 0x00 }
	};
	static const t230c_u8 response_lengths[] = {
		5, 3, 12, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1
	};
	t230c_u16 symbol_rate;
	size_t i, n = 0;

	if (!commands || !count || capacity < SI2168_MAX_COMMANDS ||
	    symbol_rate_ksps < 1000 || symbol_rate_ksps > 7200 ||
	    bandwidth_mhz != 6)
		return -EINVAL;
	if (annex != SI2168_CABLE_ANNEX_A && annex != SI2168_CABLE_ANNEX_B)
		return -EINVAL;
	symbol_rate = (t230c_u16)symbol_rate_ksps;
	for (i = 0; i < sizeof(initial) / sizeof(initial[0]); ++i) {
		t230c_u8 data[6];
		t230c_u8 length = i == 0 ? 5 : i == 2 ? 2 : i == 3 ? 3 :
			i == 14 ? 1 : 6;

		memcpy(data, initial[i], sizeof(data));
		if (i == 8) {
			/* 0x1102 は Annex A、0x1602 は Annex B のシンボルレート */
			data[3] = annex == SI2168_CABLE_ANNEX_B ? 0x16 : 0x11;
			data[4] = (t230c_u8)symbol_rate;
			data[5] = (t230c_u8)(symbol_rate >> 8);
		}
		if ((i == 10 || i == 11) && !ts_clock_inv)
			data[5] |= 0x10;
		/* 配信方式は Annex A が 0x30、Annex B が 0x10 */
		if (i == 7)
			data[4] = (t230c_u8)((annex == SI2168_CABLE_ANNEX_B ? 0x10 : 0x30) | bandwidth_mhz);
		si2168_add_command(commands, &n, data, length,
				   response_lengths[i]);
	}
	if (ts_mode & 0x20) {
		static const t230c_u8 manual_clock[] = {
			0x14, 0x00, 0x0d, 0x10, 0xe8, 0x03
		};
		si2168_add_command(commands, &n, manual_clock,
				   sizeof(manual_clock), 4);
	}
	{
		t230c_u8 mode[] = { 0x14, 0x00, 0x01, 0x10, 0x10, 0x00 };

		if (ts_mode & 0x20)
			mode[4] = 0x20;
		mode[4] |= ts_mode;
		si2168_add_command(commands, &n, mode, sizeof(mode), 4);
	}
	*count = n;
	return 0;
}

int si2168_tune(struct si2168 *demod, t230c_u32 symbol_rate_ksps,
		t230c_u32 bandwidth_mhz, t230c_u64 deadline_ms)
{
	return si2168_tune_cable(demod, SI2168_CABLE_ANNEX_A, symbol_rate_ksps,
				 bandwidth_mhz, deadline_ms);
}

int si2168_tune_cable(struct si2168 *demod, enum si2168_cable_annex annex,
		      t230c_u32 symbol_rate_ksps, t230c_u32 bandwidth_mhz,
		      t230c_u64 deadline_ms)
{
	struct si2168_command commands[SI2168_MAX_COMMANDS];
	size_t count, i;
	int ret;

	if (!demod)
		return -EINVAL;
	ret = si2168_build_cable_tune_commands(annex, symbol_rate_ksps,
					       bandwidth_mhz,
					       demod->ts_mode,
					       demod->ts_clock_inv,
					       commands, SI2168_MAX_COMMANDS,
					       &count);
	if (ret)
		return ret;
	/* ロック確認は、この選局で送った方式の状態要求を使う */
	demod->annex = annex;
	for (i = 0; i < count; ++i) {
		t230c_u8 response[SI2168_ARGUMENT_SIZE];
		ret = si2168_command(demod, commands[i].data,
				     commands[i].length,
				     commands[i].response_length,
				     deadline_ms, response);
		if (ret)
			return ret;
	}
	return 0;
}

int si2168_read_lock(struct si2168 *demod, bool *locked,
		     t230c_s32 *cnr_millibel, t230c_u64 deadline_ms)
{
	static const t230c_u8 annex_a_command[] = { 0x90, 0x01 };
	static const t230c_u8 annex_b_command[] = { 0x98, 0x01 };
	t230c_u8 response[10];
	const t230c_u8 *command;
	size_t command_size, response_size;
	int ret;

	if (!demod || !locked || !cnr_millibel)
		return -EINVAL;
	if (demod->annex == SI2168_CABLE_ANNEX_B) {
		command = annex_b_command;
		command_size = sizeof(annex_b_command);
		response_size = 10;
	} else {
		command = annex_a_command;
		command_size = sizeof(annex_a_command);
		response_size = 9;
	}
	ret = si2168_command(demod, command, command_size, response_size,
			     deadline_ms, response);
	if (ret)
		return ret;
	*locked = ((response[2] >> 1) & 0x03) == 0x03;
	*cnr_millibel = *locked ? (t230c_s32)response[3] * 250 : 0;
	return 0;
}
