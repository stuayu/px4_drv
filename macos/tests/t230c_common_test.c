#include "t230c_usb.h"
#include "si2168.h"
#include "si2141.h"
#include "t230c_ts_sync.h"

#include <errno.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

struct usb_mock {
	t230c_u8 out[64];
	size_t out_size;
	unsigned int out_calls;
	t230c_u8 response[64];
	size_t response_size;
	t230c_u8 endpoint;
	unsigned int calls;
	t230c_u64 now_ms;
	bool short_out;
	bool short_in;
	struct {
		t230c_u8 endpoint;
		int result;
		t230c_u8 data[188];
		size_t size;
	} scripted[32];
	unsigned int scripted_count;
	unsigned int scripted_index;
	t230c_u8 call_endpoints[64];
	t230c_u32 call_timeouts[64];
};

static int mock_bulk_transfer(void *context, t230c_u8 endpoint,
			      t230c_u8 *data, size_t size, size_t *actual,
			      t230c_u32 timeout_ms)
{
	struct usb_mock *mock = context;
	if (!actual || !timeout_ms)
		return -EINVAL;
	mock->endpoint = endpoint;
	if (mock->calls < sizeof(mock->call_endpoints) / sizeof(mock->call_endpoints[0])) {
		mock->call_endpoints[mock->calls] = endpoint;
		mock->call_timeouts[mock->calls] = timeout_ms;
	}
	mock->calls++;
	if (endpoint == 0x01) {
		memcpy(mock->out, data, size);
		mock->out_size = size;
		mock->out_calls++;
		*actual = mock->short_out && size ? size - 1 : size;
		return 0;
	}
	if (endpoint != 0x81 && endpoint != 0x82)
		return -EINVAL;
	if (mock->scripted_index < mock->scripted_count) {
		const unsigned int index = mock->scripted_index++;
		if (mock->scripted[index].endpoint != endpoint)
			return -EPROTO;
		if (mock->scripted[index].size > size)
			return -EMSGSIZE;
		memcpy(data, mock->scripted[index].data, mock->scripted[index].size);
		*actual = mock->scripted[index].size;
		return mock->scripted[index].result;
	}
	if (endpoint == 0x82) {
		*actual = 0;
		return -ETIMEDOUT;
	}
	if (mock->response_size > size)
		return -EMSGSIZE;
	memcpy(data, mock->response, mock->response_size);
	*actual = mock->short_in ? size - 1 : mock->response_size;
	return 0;
}

static t230c_u64 mock_now_ms(void *context)
{
	return ((struct usb_mock *)context)->now_ms;
}

static void script_read(struct usb_mock *mock, t230c_u8 endpoint, int result,
			const t230c_u8 *data, size_t size)
{
	const unsigned int index = mock->scripted_count++;
	mock->scripted[index].endpoint = endpoint;
	mock->scripted[index].result = result;
	mock->scripted[index].size = size;
	if (size)
		memcpy(mock->scripted[index].data, data, size);
}

static void reset_script(struct usb_mock *mock)
{
	mock->scripted_count = 0;
	mock->scripted_index = 0;
	mock->calls = 0;
	mock->out_calls = 0;
	memset(mock->call_endpoints, 0, sizeof(mock->call_endpoints));
	memset(mock->call_timeouts, 0, sizeof(mock->call_timeouts));
}

struct emitted_packets {
	t230c_u8 data[188 * 8];
	size_t count;
};

static void emit_packet(void *context, const t230c_ts_u8 *packet)
{
	struct emitted_packets *emitted = context;
	memcpy(emitted->data + emitted->count * 188, packet, 188);
	emitted->count++;
}

/*
 * J.83 Annex B は Annex A と次の2点だけが違う (TBS の si2168.c dd0e720e、
 * set_frontend の SYS_DVBC_ANNEX_B 分岐と status の 0x98 を参照)。
 * - 配信方式の値 0x10 (Annex A は 0x30) をプロパティ 0x0a10 に書く
 * - シンボルレートをプロパティ 0x1602 に書く (Annex A は 0x1102)
 * - 状態要求は 0x98 01 で、応答は10バイト (Annex A は 0x90 01 と9バイト)
 */
struct si2168_i2c_mock {
	t230c_u8 writes[32][8];
	size_t write_sizes[32];
	unsigned int write_count;
	t230c_u8 status[16];
};

static int si2168_i2c_mock_request(void *priv, const struct i2c_comm_request *req, int num)
{
	struct si2168_i2c_mock *mock = priv;

	if (num != 1 || req[0].addr != 0x64)
		return -EINVAL;
	if (req[0].req == I2C_WRITE_REQUEST) {
		if (mock->write_count >= 32 || req[0].len > 8)
			return -EMSGSIZE;
		memcpy(mock->writes[mock->write_count], req[0].data, (size_t)req[0].len);
		mock->write_sizes[mock->write_count++] = (size_t)req[0].len;
		return 0;
	}
	if (req[0].req == I2C_READ_REQUEST) {
		if (req[0].len > (int)sizeof(mock->status))
			return -EMSGSIZE;
		memcpy(req[0].data, mock->status, (size_t)req[0].len);
		return 0;
	}
	return -EINVAL;
}

static t230c_u64 si2168_i2c_mock_now(void *context)
{
	(void)context;
	return 0;
}

static void si2168_i2c_mock_sleep(void *context, t230c_u32 usec)
{
	(void)context;
	(void)usec;
}

static int si2168_mock_has_write(const struct si2168_i2c_mock *mock, const t230c_u8 *data, size_t size)
{
	unsigned int i;

	for (i = 0; i < mock->write_count; ++i)
		if (mock->write_sizes[i] == size && memcmp(mock->writes[i], data, size) == 0)
			return 1;
	return 0;
}

static int j83b_annex_b_tests(void)
{
	static const t230c_u8 annex_b_delivery[] = { 0x14, 0x00, 0x0a, 0x10, 0x16, 0x00 };
	static const t230c_u8 annex_b_symbol[] = { 0x14, 0x00, 0x02, 0x16, 0xe0, 0x15 };
	static const t230c_u8 annex_a_delivery[] = { 0x14, 0x00, 0x0a, 0x10, 0x36, 0x00 };
	static const t230c_u8 annex_b_status[] = { 0x98, 0x01 };
	static const t230c_u8 annex_a_status[] = { 0x90, 0x01 };
	struct si2168_command commands[SI2168_MAX_COMMANDS];
	struct si2168_i2c_mock mock;
	struct i2c_comm_master master;
	struct si2168 demod;
	size_t count = 0;
	bool locked = false;
	t230c_s32 cnr = 0;

	/* 5.6 Msym/s の J.83B コマンドは、配信方式と0x1602のシンボルレートを含む */
	CHECK(si2168_build_cable_tune_commands(SI2168_CABLE_ANNEX_B, 5600, 6, 0x26, true,
					       commands, SI2168_MAX_COMMANDS, &count) == 0);
	CHECK(count == SI2168_MAX_COMMANDS);
	CHECK(commands[7].length == sizeof(annex_b_delivery) &&
	      memcmp(commands[7].data, annex_b_delivery, sizeof(annex_b_delivery)) == 0);
	CHECK(commands[8].length == sizeof(annex_b_symbol) &&
	      memcmp(commands[8].data, annex_b_symbol, sizeof(annex_b_symbol)) == 0);

	/* Annex A は従来の 0x30 と 0x1102 を使い、出力を変えない */
	CHECK(si2168_build_cable_tune_commands(SI2168_CABLE_ANNEX_A, 5274, 6, 0x26, true,
					       commands, SI2168_MAX_COMMANDS, &count) == 0);
	CHECK(memcmp(commands[7].data, annex_a_delivery, sizeof(annex_a_delivery)) == 0);
	CHECK(commands[7].data[4] == 0x36 && commands[8].data[3] == 0x11);
	CHECK(commands[8].data[4] == 0x9a && commands[8].data[5] == 0x14);

	/* 不正な方式は拒否する */
	CHECK(si2168_build_cable_tune_commands((enum si2168_cable_annex)7, 5600, 6, 0x26, true,
					       commands, SI2168_MAX_COMMANDS, &count) == -EINVAL);

	/* 模擬 I2C で、J.83B の選局と状態要求の書き込み列を確かめる */
	memset(&mock, 0, sizeof(mock));
	mock.status[0] = 0x80;
	mock.status[2] = 0x06;
	mock.status[3] = 0x30;
	master.gate_ctrl = NULL;
	master.request = si2168_i2c_mock_request;
	master.priv = &mock;
	memset(&demod, 0, sizeof(demod));
	demod.i2c = &master;
	demod.now_ms = si2168_i2c_mock_now;
	demod.sleep_us = si2168_i2c_mock_sleep;
	demod.ts_mode = 0x26;
	demod.ts_clock_inv = true;
	CHECK(si2168_tune_cable(&demod, SI2168_CABLE_ANNEX_B, 5600, 6, ~(t230c_u64)0) == 0);
	CHECK(si2168_mock_has_write(&mock, annex_b_delivery, sizeof(annex_b_delivery)));
	CHECK(si2168_mock_has_write(&mock, annex_b_symbol, sizeof(annex_b_symbol)));
	CHECK(!si2168_mock_has_write(&mock, annex_a_delivery, sizeof(annex_a_delivery)));

	/* ロック確認は 0x98 を使う。Annex A の 0x90 は送らない */
	mock.write_count = 0;
	CHECK(si2168_read_lock(&demod, &locked, &cnr, ~(t230c_u64)0) == 0);
	CHECK(mock.write_count == 1 && mock.write_sizes[0] == sizeof(annex_b_status) &&
	      memcmp(mock.writes[0], annex_b_status, sizeof(annex_b_status)) == 0);
	CHECK(locked && cnr == 0x30 * 250);

	/* Annex A へ戻すと、状態要求も 0x90 に戻る */
	CHECK(si2168_tune(&demod, 5274, 6, ~(t230c_u64)0) == 0);
	mock.write_count = 0;
	CHECK(si2168_read_lock(&demod, &locked, &cnr, ~(t230c_u64)0) == 0);
	CHECK(mock.write_count == 1 && mock.write_sizes[0] == sizeof(annex_a_status) &&
	      memcmp(mock.writes[0], annex_a_status, sizeof(annex_a_status)) == 0);
	return 0;
}

int main(void)
{
	static const struct t230c_usb_ops ops = {
		.bulk_transfer = mock_bulk_transfer,
		.now_ms = mock_now_ms,
	};
	struct usb_mock mock = { 0 };
	struct t230c_usb usb;
	t230c_u8 command[] = { 0x36, 0x03, 0x00 };
	t230c_u8 response[3] = { 0 };
	t230c_u8 write_data[] = { 0x41, 0x00, 0x00, 0x00 };
	t230c_u8 read_data[2] = { 0 };
	struct i2c_comm_request request[2] = { 0 };
	t230c_u8 firmware[17] = { 6 };
	t230c_ts_u8 sync_storage[188 * 4];
	struct t230c_ts_sync sync;
	t230c_ts_u8 packets[188 * 5];
	struct emitted_packets emitted = { { 0 }, 0 };
	unsigned int i;
	unsigned int calls;

	CHECK(t230c_usb_init(&usb, &ops, &mock, 2000) == 0);
	mock.response[0] = 0x80;
	mock.response_size = 1;
	CHECK(t230c_usb_command(&usb, command, sizeof(command), response, 1, 2000) == 0);
	CHECK(mock.endpoint == 0x81 && mock.out_size == sizeof(command));
	CHECK(memcmp(mock.out, command, sizeof(command)) == 0 && response[0] == 0x80);

	request[0].req = I2C_WRITE_REQUEST;
	request[0].addr = 0x60;
	request[0].data = write_data;
	request[0].len = sizeof(write_data);
	CHECK(t230c_usb_i2c_request(&usb, request, 1) == 0);
	CHECK(mock.out_size == sizeof(write_data) + 3);
	CHECK(memcmp(mock.out, (t230c_u8[]){ 0x08, 0x60, 4, 0x41, 0, 0, 0 },
		     sizeof(write_data) + 3) == 0);

	request[0].req = I2C_READ_REQUEST;
	request[0].addr = 0x64;
	request[0].data = read_data;
	request[0].len = sizeof(read_data);
	mock.response[0] = 0x80;
	mock.response[1] = 0x12;
	mock.response[2] = 0x34;
	mock.response_size = 3;
	CHECK(t230c_usb_i2c_request(&usb, request, 1) == 0);
	CHECK(mock.out_size == 4 && memcmp(mock.out, (t230c_u8[]){ 0x09, 0, 2, 0x64 }, 4) == 0);
	CHECK(read_data[0] == 0x12 && read_data[1] == 0x34);

	request[0].req = I2C_WRITE_REQUEST;
	request[0].addr = 0x64;
	request[0].data = write_data;
	request[0].len = 1;
	request[1].req = I2C_READ_REQUEST;
	request[1].addr = 0x64;
	request[1].data = read_data;
	request[1].len = 2;
	CHECK(t230c_usb_i2c_request(&usb, request, 2) == 0);
	CHECK(mock.out_size == 5 && memcmp(mock.out, (t230c_u8[]){ 0x09, 1, 2, 0x64, 0x41 }, 5) == 0);

	request[0].len = 61;
	CHECK(t230c_usb_i2c_request(&usb, request, 1) == -EINVAL);
	request[0].len = 1;
	CHECK(t230c_usb_i2c_request(&usb, request, 3) == -EINVAL);

	calls = mock.calls;
	mock.now_ms = 10;
	t230c_usb_set_deadline(&usb, 10);
	CHECK(t230c_usb_command(&usb, command, sizeof(command), NULL, 0, 2000) == -ETIMEDOUT);
	CHECK(mock.calls == calls);
	t230c_usb_set_deadline(&usb, ~(t230c_u64)0);

	mock.short_out = true;
	CHECK(t230c_usb_command(&usb, command, sizeof(command), NULL, 0, 2000) == -EIO);
	mock.short_out = false;
	mock.short_in = true;
	mock.response_size = 1;
	CHECK(t230c_usb_command(&usb, command, sizeof(command), response, 1, 2000) == -EIO);
	mock.short_in = false;

	/* 初期化前に EP81 と EP82 の残留データを短い期限で空にする。 */
	reset_script(&mock);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x08 }, 1);
	script_read(&mock, 0x81, -ETIMEDOUT, NULL, 0);
	script_read(&mock, 0x82, 0, (t230c_u8[188]){ 0 }, 188);
	script_read(&mock, 0x82, -ETIMEDOUT, NULL, 0);
	struct t230c_usb_resync_stats stats = { 0 };
	CHECK(t230c_usb_resynchronize(&usb, 3, 4, &stats) == 0);
	CHECK(stats.ep81_bytes == 1 && stats.ep82_bytes == 188);
	CHECK(stats.ep81_reads == 2 && stats.ep82_reads == 2);
	CHECK(stats.ep81_preview_size == 1 && stats.ep81_preview[0] == 0x08);
	CHECK(mock.calls == 4 && mock.call_endpoints[0] == 0x81 &&
	      mock.call_endpoints[2] == 0x82);
	CHECK(mock.out_calls == 0);
	CHECK(mock.call_timeouts[0] == 3 && mock.call_timeouts[3] == 3);

	/* 上限までデータが続く場合は、未排出の可能性を残したまま成功しない。 */
	reset_script(&mock);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x01 }, 1);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x02 }, 1);
	CHECK(t230c_usb_resynchronize(&usb, 3, 2, &stats) == -EOVERFLOW);
	CHECK(stats.ep81_bytes == 2 && mock.calls == 2);

	/* EP82 may still carry live TS after HostKill; its initial drain remains bounded. */
	reset_script(&mock);
	script_read(&mock, 0x81, -ETIMEDOUT, NULL, 0);
	for (i = 0; i < 4; ++i)
		script_read(&mock, 0x82, 0, (t230c_u8[188]){ 0x47 }, 188);
	CHECK(t230c_usb_resynchronize(&usb, 3, 4, &stats) == 0);
	CHECK(stats.ep81_reads == 1 && stats.ep82_reads == 4 &&
	      stats.ep82_bytes == 188 * 4 && stats.ep82_limit_reached);

	/* GPIO の level 設定は応答長不一致後に同期して一度だけ再送する。 */
	reset_script(&mock);
	script_read(&mock, 0x81, 0, NULL, 0);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x08 }, 1);
	script_read(&mock, 0x81, -ETIMEDOUT, NULL, 0);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x80 }, 1);
	CHECK(t230c_usb_gpio_set(&usb, 0x83, 0, 100, 3, 4) == 0);
	CHECK(mock.out_calls == 2 && mock.out_size == 3 &&
	      memcmp(mock.out, (t230c_u8[]){ 0x0e, 0x83, 0 }, 3) == 0);
	CHECK(mock.scripted_index == mock.scripted_count);

	/* 純粋な Si2168 read は再送するが、I/O write が短いときは再送しない。 */
	reset_script(&mock);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x08 }, 1);
	script_read(&mock, 0x81, -ETIMEDOUT, NULL, 0);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x80, 0x12, 0x34 }, 3);
	CHECK(t230c_usb_si2168_read(&usb, 0x64, read_data, sizeof(read_data), 100, 3, 4) == 0);
	CHECK(mock.out_calls == 2 && mock.out_size == 4 &&
	      memcmp(mock.out, (t230c_u8[]){ 0x09, 0, 2, 0x64 }, 4) == 0);
	CHECK(read_data[0] == 0x12 && read_data[1] == 0x34);
	reset_script(&mock);
	CHECK(t230c_usb_si2168_read(&usb, 0x60, read_data, sizeof(read_data), 100, 3, 4) == -EINVAL);
	CHECK(mock.out_calls == 0);
	reset_script(&mock);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x08 }, 1);
	script_read(&mock, 0x81, -ETIMEDOUT, NULL, 0);
	script_read(&mock, 0x81, 0, (t230c_u8[]){ 0x08 }, 1);
	CHECK(t230c_usb_si2168_read(&usb, 0x64, read_data, sizeof(read_data), 100, 3, 4) == -EIO);
	CHECK(mock.out_calls == 2);
	reset_script(&mock);
	mock.short_out = true;
	CHECK(t230c_usb_gpio_set(&usb, 0x83, 0, 100, 3, 4) == -EIO);
	CHECK(mock.out_calls == 1);
	mock.short_out = false;

	CHECK(si2168_validate_firmware(firmware, sizeof(firmware)) == 0);
	firmware[0] = 17;
	CHECK(si2168_validate_firmware(firmware, sizeof(firmware)) == -EINVAL);

	CHECK(t230c_ts_sync_init(&sync, sync_storage, sizeof(sync_storage)) == 0);
	for (i = 0; i < 5; ++i) {
		memset(packets + i * 188, (int)i, 188);
		packets[i * 188] = 0x47;
	}
	CHECK(t230c_ts_sync_push(&sync, packets, 333, emit_packet, &emitted) == 0);
	CHECK(emitted.count == 0);
	CHECK(t230c_ts_sync_push(&sync, packets + 333, sizeof(packets) - 333,
				 emit_packet, &emitted) == 0);
	CHECK(emitted.count == 5 && memcmp(emitted.data, packets, sizeof(packets)) == 0);
	t230c_ts_sync_reset(&sync);
	CHECK(sync.pending_size == 0 && !sync.synchronized);
	CHECK(j83b_annex_b_tests() == 0);
	return 0;
}
