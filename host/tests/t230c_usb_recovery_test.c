#include "t230c_usb_recovery.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

struct recovery_call {
	char operation;
	uint8_t endpoint;
};

struct recovery_mock {
	struct recovery_call calls[16];
	size_t count;
	char fail_operation;
	uint8_t fail_endpoint;
};

static int record_call(struct recovery_mock *mock, char operation,
		       uint8_t endpoint)
{
	if (mock->count >= sizeof(mock->calls) / sizeof(mock->calls[0]))
		return -ERANGE;
	mock->calls[mock->count].operation = operation;
	mock->calls[mock->count].endpoint = endpoint;
	mock->count++;
	if (operation == mock->fail_operation && endpoint == mock->fail_endpoint)
		return -EIO;
	return 0;
}

static int abort_pipe(void *context, uint8_t endpoint)
{
	return record_call(context, 'A', endpoint);
}

static int flush_pipe(void *context, uint8_t endpoint)
{
	return record_call(context, 'F', endpoint);
}

static int reset_pipe(void *context, uint8_t endpoint)
{
	return record_call(context, 'R', endpoint);
}

static int stop_stream(void *context)
{
	return record_call(context, 'S', 0);
}

int main(void)
{
	static const struct t230c_usb_recovery_ops ops = {
		abort_pipe, flush_pipe, reset_pipe, stop_stream
	};
	static const struct recovery_call expected[] = {
		{ 'A', 0x01 }, { 'A', 0x81 }, { 'A', 0x82 },
		{ 'F', 0x81 }, { 'F', 0x82 },
		{ 'R', 0x01 }, { 'R', 0x81 }, { 'R', 0x82 },
		{ 'S', 0 }
	};
	struct recovery_mock mock = { 0 };
	size_t i;

	CHECK(t230c_usb_recover_for_init(&ops, &mock) == 0);
	CHECK(mock.count == sizeof(expected) / sizeof(expected[0]));
	for (i = 0; i < mock.count; ++i) {
		CHECK(mock.calls[i].operation == expected[i].operation);
		CHECK(mock.calls[i].endpoint == expected[i].endpoint);
	}

	mock.count = 0;
	mock.fail_operation = 'F';
	mock.fail_endpoint = 0x81;
	CHECK(t230c_usb_recover_for_init(&ops, &mock) == -EIO);
	CHECK(mock.count == 4);

	CHECK(t230c_usb_recover_for_init(NULL, &mock) == -EINVAL);
	return 0;
}
