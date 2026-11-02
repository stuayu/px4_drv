/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux drivers/media/tuners/si2157.c の Si2141 コマンド処理を共通化。 */

#include "si2141.h"
#include "si2168.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define SI2141_ADDRESS 0x60U
#define SI2141_ARGUMENT_SIZE 30U
#define SI2141_COMMAND_TIMEOUT_MS 80U
#define SI2141_TUNE_TIMEOUT_MS 40U
#define SI2141_FW_RECORD_SIZE 17U

static t230c_u64 si2141_deadline(struct si2141 *tuner,
				  t230c_u64 requested_deadline)
{
	t230c_u64 now = tuner->now_ms(tuner->clock_context);
	t230c_u64 command_deadline = now + SI2141_COMMAND_TIMEOUT_MS;

	if (command_deadline < now)
		command_deadline = ~(t230c_u64)0;
	return requested_deadline < command_deadline
		? requested_deadline : command_deadline;
}

static int si2141_transfer(struct si2141 *tuner, const t230c_u8 *write,
			   size_t write_size, t230c_u8 *read,
			   size_t read_size, t230c_u64 deadline_ms)
{
	struct i2c_comm_request request[1];
	int ret, close_ret = 0;
	bool gate_open = tuner->i2c && tuner->i2c->gate_ctrl;

	if (tuner->set_deadline)
		tuner->set_deadline(tuner->clock_context, deadline_ms);
	if (gate_open) {
		ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
		if (ret)
			return ret;
	}
	request[0].addr = SI2141_ADDRESS;
	if (write_size) {
		request[0].req = I2C_WRITE_REQUEST;
		request[0].data = (t230c_u8 *)write;
		request[0].len = (int)write_size;
	} else {
		request[0].req = I2C_READ_REQUEST;
		request[0].data = read;
		request[0].len = (int)read_size;
	}
	ret = i2c_comm_master_request(tuner->i2c, request, 1);
	if (gate_open)
		close_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret ? ret : close_ret;
}

static int si2141_command(struct si2141 *tuner, const t230c_u8 *command,
			  size_t command_size, size_t response_size,
			  t230c_u64 requested_deadline,
			  t230c_u8 *response)
{
	t230c_u8 args[SI2141_ARGUMENT_SIZE];
	t230c_u64 deadline;
	t230c_u64 now;
	int ret;

	if (!tuner || !tuner->now_ms || !command || !command_size ||
	    command_size > sizeof(args) || response_size > sizeof(args) ||
	    (response_size && !response))
		return -EINVAL;
	deadline = si2141_deadline(tuner, requested_deadline);
	if (tuner->now_ms(tuner->clock_context) >= deadline)
		return -ETIMEDOUT;
	ret = si2141_transfer(tuner, command, command_size, NULL, 0, deadline);
	if (ret || !response_size)
		goto exit;
	while ((now = tuner->now_ms(tuner->clock_context)) < deadline) {
		memset(args, 0, response_size);
		ret = si2141_transfer(tuner, NULL, 0, args, response_size, deadline);
		if (ret)
			goto exit;
		if (tuner->now_ms(tuner->clock_context) >= deadline) {
			ret = -ETIMEDOUT;
			goto exit;
		}
		if (args[0] & 0x80) {
			if (args[0] & 0x40) {
				ret = -EAGAIN;
				goto exit;
			}
			memcpy(response, args, response_size);
			ret = 0;
			goto exit;
		}
	}
	ret = -ETIMEDOUT;
exit:
	if (tuner->set_deadline)
		tuner->set_deadline(tuner->clock_context, requested_deadline);
	return ret;
}

static int si2141_download(struct si2141 *tuner, const t230c_u8 *firmware,
			   size_t firmware_size)
{
	size_t offset;
	int ret = si2168_validate_firmware(firmware, firmware_size);

	if (ret)
		return ret;
	for (offset = 0; offset < firmware_size; offset += SI2141_FW_RECORD_SIZE) {
		t230c_u8 response;
		ret = si2141_command(tuner, firmware + offset + 1,
				     firmware[offset], 1,
				     ~(t230c_u64)0, &response);
		if (ret)
			return ret;
	}
	return 0;
}

int si2141_init(struct si2141 *tuner,
		const struct i2c_comm_master *i2c,
		void *clock_context,
		t230c_u64 (*now_ms)(void *context),
		void (*sleep_ms)(void *context, t230c_u32 msec),
		void (*set_deadline)(void *context, t230c_u64 deadline_ms),
		si2141_firmware_loader load_firmware,
		void *firmware_context, t230c_u8 if_port,
		t230c_u8 *rom_id, t230c_u8 *part_id)
{
	static const t230c_u8 xtal_trim[] = { 0x15, 0x00, 0x02, 0x04 };
	static const t230c_u8 power_up[] = {
		0xc0, 0x00, 0x0d, 0x0e, 0x00, 0x01, 0x01, 0x01, 0x01, 0x03
	};
	static const t230c_u8 wake_up[] = { 0xc0, 0x08, 0x01, 0x02, 0x00, 0x00, 0x01 };
	static const t230c_u8 chip_info[] = { 0x02 };
	static const t230c_u8 start_firmware[] = { 0x01, 0x01 };
	static const t230c_u8 firmware_version[] = { 0x11 };
	static const t230c_u8 enable_flags[][6] = {
		{ 0x14, 0x00, 0x01, 0x05, 0x01, 0x00 },
		{ 0x14, 0x00, 0x01, 0x06, 0x01, 0x00 },
		{ 0x14, 0x00, 0x01, 0x07, 0x01, 0x00 }
	};
	t230c_u8 response[13];
	const t230c_u8 *firmware = NULL;
	size_t firmware_size = 0;
	t230c_u8 detected_rom;
	t230c_u8 detected_part;
	int ret;
	size_t i;

	if (!tuner || !i2c || !now_ms || !sleep_ms || !load_firmware)
		return -EINVAL;
	memset(tuner, 0, sizeof(*tuner));
	tuner->i2c = i2c;
	tuner->clock_context = clock_context;
	tuner->now_ms = now_ms;
	tuner->sleep_ms = sleep_ms;
	tuner->set_deadline = set_deadline;
	tuner->if_port = if_port;

	/* 水晶トリム照会の失敗はコールド起動として扱う。 */
	(void)si2141_command(tuner, xtal_trim, sizeof(xtal_trim), 4,
			     ~(t230c_u64)0, response);
	ret = si2141_command(tuner, power_up, sizeof(power_up), 1,
			     ~(t230c_u64)0, response);
	/* Si2141 は power-up 応答の error bit を返す実装がある。 */
	if (ret && ret != -EAGAIN)
		return ret;
	ret = si2141_command(tuner, wake_up, sizeof(wake_up), 1,
			     ~(t230c_u64)0, response);
	if (ret)
		return ret;
	ret = si2141_command(tuner, chip_info, sizeof(chip_info), sizeof(response),
			     ~(t230c_u64)0, response);
	if (ret)
		return ret;
	detected_part = response[2];
	detected_rom = response[12];
	if (part_id)
		*part_id = detected_part;
	if (rom_id)
		*rom_id = detected_rom;
	if (detected_part != 41)
		return -ENODEV;
	ret = load_firmware(firmware_context, detected_rom, &firmware,
			    &firmware_size);
	if (ret)
		return ret;
	ret = si2141_download(tuner, firmware, firmware_size);
	if (!ret)
		ret = si2141_command(tuner, start_firmware,
				     sizeof(start_firmware), 1,
				     ~(t230c_u64)0, response);
	if (!ret)
		ret = si2141_command(tuner, firmware_version,
				     sizeof(firmware_version), 10,
				     ~(t230c_u64)0, response);
	for (i = 0; !ret && i < sizeof(enable_flags) / sizeof(enable_flags[0]); ++i)
		ret = si2141_command(tuner, enable_flags[i], sizeof(enable_flags[i]),
				     1, ~(t230c_u64)0, response);
	return ret;
}

static void si2141_add_command(struct si2141_command *commands,
			       size_t *count, const t230c_u8 *data,
			       t230c_u8 length, t230c_u8 response_length)
{
	struct si2141_command *command = &commands[(*count)++];

	memcpy(command->data, data, length);
	command->length = length;
	command->response_length = response_length;
}

int si2141_build_tune_commands(t230c_u32 frequency_khz,
			       t230c_u32 bandwidth_mhz,
			       struct si2141_command *commands,
			       size_t capacity, size_t *count)
{
	static const t230c_u8 property_0703[] = {
		0x14, 0x00, 0x03, 0x07, 0x36, 0x00
	};
	static const t230c_u8 property_0702[] = {
		0x14, 0x00, 0x02, 0x07, 0x00, 0x00
	};
	static const t230c_u8 property_0706[] = {
		0x14, 0x00, 0x06, 0x07, 0x88, 0x13
	};
	t230c_u32 frequency_hz;
	t230c_u8 command[8];
	size_t n = 0;

	if (!commands || !count || capacity < SI2141_MAX_COMMANDS ||
	    bandwidth_mhz != 6 || frequency_khz > 0xffffffffU / 1000U)
		return -EINVAL;
	si2141_add_command(commands, &n, property_0703,
			   sizeof(property_0703), 4);
	si2141_add_command(commands, &n, property_0702,
			   sizeof(property_0702), 4);
	si2141_add_command(commands, &n, property_0706,
			   sizeof(property_0706), 4);
	frequency_hz = frequency_khz * 1000U;
	command[0] = 0x41;
	command[1] = 0x00;
	command[2] = 0x00;
	command[3] = 0x00;
	command[4] = (t230c_u8)frequency_hz;
	command[5] = (t230c_u8)(frequency_hz >> 8);
	command[6] = (t230c_u8)(frequency_hz >> 16);
	command[7] = (t230c_u8)(frequency_hz >> 24);
	si2141_add_command(commands, &n, command, 8, 1);
	*count = n;
	return 0;
}

int si2141_tune(struct si2141 *tuner, t230c_u32 frequency_khz,
		t230c_u32 bandwidth_mhz, t230c_u64 deadline_ms)
{
	struct si2141_command commands[SI2141_MAX_COMMANDS];
	size_t count, i;
	int ret;

	if (!tuner)
		return -EINVAL;
	ret = si2141_build_tune_commands(frequency_khz, bandwidth_mhz,
					commands, SI2141_MAX_COMMANDS, &count);
	if (ret)
		return ret;
	commands[1].data[4] = tuner->if_port;
	for (i = 0; i < count; ++i) {
		t230c_u8 response[SI2141_ARGUMENT_SIZE];
		ret = si2141_command(tuner, commands[i].data, commands[i].length,
				     commands[i].response_length,
				     deadline_ms, response);
		if (ret)
			return ret;
	}
	return 0;
}

int si2141_wait_tune(struct si2141 *tuner, t230c_u64 deadline_ms)
{
	t230c_u8 response;
	t230c_u64 local_deadline;
	t230c_u64 now;
	bool received = false;
	int ret;

	if (!tuner || !tuner->now_ms || !tuner->sleep_ms)
		return -EINVAL;
	now = tuner->now_ms(tuner->clock_context);
	local_deadline = now + SI2141_TUNE_TIMEOUT_MS;
	if (local_deadline < now)
		local_deadline = ~(t230c_u64)0;
	if (deadline_ms < local_deadline)
		local_deadline = deadline_ms;
	while (tuner->now_ms(tuner->clock_context) < local_deadline) {
		ret = si2141_transfer(tuner, NULL, 0, &response, 1,
				      local_deadline);
		if (ret)
			goto exit;
		received = true;
		if ((response & 0x81) == 0x81)
			break;
		tuner->sleep_ms(tuner->clock_context, 5);
	}
	ret = received && (response & 0xc0) == 0x80 ? 0 : -ETIMEDOUT;
exit:
	if (tuner->set_deadline)
		tuner->set_deadline(tuner->clock_context, deadline_ms);
	return ret;
}
