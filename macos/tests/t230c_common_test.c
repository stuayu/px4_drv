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
	t230c_u8 response[64];
	size_t response_size;
	t230c_u8 endpoint;
	unsigned int calls;
	t230c_u64 now_ms;
	bool short_out;
	bool short_in;
};

static int mock_bulk_transfer(void *context, t230c_u8 endpoint,
			      t230c_u8 *data, size_t size, size_t *actual,
			      t230c_u32 timeout_ms)
{
	struct usb_mock *mock = context;
	if (!actual || !timeout_ms)
		return -EINVAL;
	mock->endpoint = endpoint;
	mock->calls++;
	if (endpoint == 0x01) {
		memcpy(mock->out, data, size);
		mock->out_size = size;
		*actual = mock->short_out && size ? size - 1 : size;
		return 0;
	}
	if (endpoint != 0x81)
		return -EINVAL;
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
	return 0;
}
