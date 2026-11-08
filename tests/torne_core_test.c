#include "torne_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mock_usb {
	u8 *memory;
	size_t memory_size;
	u64 clock;
	u32 alive;
	u16 value;
	u16 index;
	u8 request_type;
	unsigned calls;
};

static struct mock_usb *active_mock;

static u32 decode_address(u16 value, u16 index)
{
	return (u32)(value >> 8) << 16 | (u32)(value & 0xff) << 24 | index;
}

static u32 load_be32(const u8 *bytes)
{
	return (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 | bytes[3];
}

static void store_be32(u8 *bytes, u32 value)
{
	bytes[0] = (u8)(value >> 24);
	bytes[1] = (u8)(value >> 16);
	bytes[2] = (u8)(value >> 8);
	bytes[3] = (u8)value;
}

static int mock_control(void *priv, u8 request_type, u16 value, u16 index,
	void *buffer, u16 length)
{
	struct mock_usb *mock = priv;
	u8 *bytes = buffer;
	mock->request_type = request_type;
	mock->value = value;
	mock->index = index;
	mock->calls++;
	const u32 address = decode_address(value, index);
	if (!length || length > 64 || address > mock->memory_size ||
	    length > mock->memory_size - address)
		return -EINVAL;
	if (request_type == 0x40) {
		memcpy(mock->memory + address, bytes, length);
		return length;
	}
	if (request_type == 0xc0) {
		if (address == 0x1700000 + 0xa3f00 + 0x7c && length == 4)
			store_be32(bytes, ++mock->alive);
		else
			memcpy(bytes, mock->memory + address, length);
		return length;
	}
	return -EINVAL;
}

static int mock_bulk(void *priv, u8 endpoint, void *buffer, int length, unsigned int timeout_ms)
{
	(void)priv;
	(void)endpoint;
	(void)buffer;
	(void)length;
	(void)timeout_ms;
	return -EIO;
}

static void mock_msleep(void *priv, unsigned int milliseconds)
{
	((struct mock_usb *)priv)->clock += milliseconds;
}

static u64 mock_now_ms(void *priv)
{
	return ((struct mock_usb *)priv)->clock++;
}

static const struct torne_ops mock_ops = {
	.control = mock_control,
	.bulk = mock_bulk,
	.msleep = mock_msleep,
	.now_ms = mock_now_ms,
};

static int test_control_memory_transfer(void)
{
	struct mock_usb *mock = active_mock;
	struct torne_bridge bridge;
	const u32 address = 0x00123456;
	torne_bridge_init(&bridge, &mock_ops, mock);
	torne_mem_write32(&bridge, address, 0x12345678);
	if (mock->calls != 1 || mock->request_type != 0x40 || mock->value != 0x1200 ||
	    mock->index != 0x3456 || load_be32(mock->memory + address) != 0x12345678) {
		fprintf(stderr, "mock USB write did not encode the bridge memory request\n");
		return 0;
	}
	if (torne_mem_read32(&bridge, address) != 0x12345678 || mock->calls != 2 ||
	    mock->request_type != 0xc0 || mock->value != 0x1200 || mock->index != 0x3456) {
		fprintf(stderr, "mock USB read did not decode the bridge memory response\n");
		return 0;
	}
	return 1;
}

static int test_bridge_startup_sequence(void)
{
	static const struct { u32 address, value; } handler[] = {
		{ 0xe9ac0, 0x9b800000 }, { 0xe9ac4, 0xaa04e40f },
		{ 0xe9e0c, 0x9b800000 }, { 0xe9e10, 0xaa04e40d },
		{ 0xe9d94, 0xb4279f8d },
	};
	struct mock_usb *mock = active_mock;
	struct torne_bridge bridge;
	torne_bridge_init(&bridge, &mock_ops, mock);
	for (size_t i = 0; i < sizeof(handler) / sizeof(handler[0]); ++i)
		store_be32(mock->memory + handler[i].address, handler[i].value);
	if (torne_bridge_start(&bridge) || torne_bridge_reset_demodulator(&bridge) ||
	    torne_bridge_prepare_stream(&bridge) || torne_bridge_start_stream(&bridge) ||
	    !bridge.streaming || torne_bridge_stop_stream(&bridge) || bridge.streaming) {
		fprintf(stderr, "mock MB86H47A initialization or stream state transition failed\n");
		return 0;
	}
	return 1;
}

static int test_split_stream_framing(void)
{
	u8 raw[2 * 192] = { 0 };
	u8 output[2 * 188] = { 0 };
	struct torne_framer framer;
	torne_framer_reset(&framer);
	for (size_t packet = 0; packet < 2; ++packet) {
		u8 *ts = raw + packet * 192 + 4;
		ts[0] = 0x47;
		for (size_t i = 1; i < 188; ++i)
			ts[i] = (u8)(packet + i);
	}
	if (torne_framer_feed(&framer, raw, 97, output) != 0 ||
	    torne_framer_feed(&framer, raw + 97, sizeof(raw) - 97, output) != sizeof(output) ||
	    framer.len != 0 || memcmp(output, raw + 4, 188) != 0 ||
	    memcmp(output + 188, raw + 196, 188) != 0) {
		fprintf(stderr, "split 192-byte bridge packets were not framed as 188-byte TS\n");
		return 0;
	}
	return 1;
}

int main(void)
{
	const size_t memory_size = 0x1800000;
	active_mock = calloc(1, sizeof(*active_mock));
	if (!active_mock)
		return 1;
	active_mock->memory = calloc(1, memory_size);
	active_mock->memory_size = memory_size;
	if (!active_mock->memory) {
		free(active_mock);
		return 1;
	}
	const int memory_ok = test_control_memory_transfer();
	const int framing_ok = test_split_stream_framing();
	const int startup_ok = test_bridge_startup_sequence();
	free(active_mock->memory);
	free(active_mock);
	return memory_ok && framing_ok && startup_ok ? 0 : 1;
}
