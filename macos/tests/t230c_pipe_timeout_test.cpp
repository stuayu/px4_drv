#include "t230c_pipe_timeout.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

struct MockPipe {
	std::uint32_t timeout = 0;
	std::uint32_t temporary_timeout = 0;
	int read_result = 0;
	bool fail_restore = false;
	std::vector<std::uint32_t> set_values;
	int calls = 0;
};

static int get_timeout(void *context, std::uint8_t, std::uint32_t &timeout)
{
	auto &mock = *static_cast<MockPipe *>(context);
	++mock.calls;
	timeout = mock.timeout;
	return 0;
}

static int set_timeout(void *context, std::uint8_t, std::uint32_t timeout)
{
	auto &mock = *static_cast<MockPipe *>(context);
	mock.set_values.push_back(timeout);
	if (mock.fail_restore && timeout != mock.temporary_timeout)
		return -EIO;
	mock.timeout = timeout;
	return 0;
}

static int read_pipe(void *context, std::uint8_t, std::uint8_t *data,
		     std::size_t size, std::size_t &actual)
{
	auto &mock = *static_cast<MockPipe *>(context);
	++mock.calls;
	if (mock.read_result)
		return mock.read_result;
	actual = size;
	if (size)
		std::memset(data, 0x47, size);
	return 0;
}

int main()
{
	std::uint8_t data[188] = {};
	std::size_t actual = 0;
	MockPipe success;
	success.temporary_timeout = 5;
	CHECK(px4::t230c::ReadWithTemporaryTimeout(&success, 0x82, data,
		sizeof(data), actual, success.temporary_timeout,
		get_timeout, set_timeout, read_pipe) == 0);
	CHECK(success.timeout == 0 && actual == sizeof(data));
	CHECK(success.set_values.size() == 2 && success.set_values[0] == 5 &&
	      success.set_values[1] == 0);

	MockPipe timed_out;
	timed_out.temporary_timeout = 5;
	timed_out.read_result = -ETIMEDOUT;
	actual = 0;
	CHECK(px4::t230c::ReadWithTemporaryTimeout(&timed_out, 0x82, data,
		sizeof(data), actual, timed_out.temporary_timeout,
		get_timeout, set_timeout, read_pipe) == -ETIMEDOUT);
	CHECK(timed_out.timeout == 0 && timed_out.set_values.size() == 2 &&
	      timed_out.set_values[1] == 0);

	MockPipe restore_failed;
	restore_failed.timeout = 0;
	restore_failed.temporary_timeout = 5;
	restore_failed.fail_restore = true;
	CHECK(px4::t230c::ReadWithTemporaryTimeout(&restore_failed, 0x82, data,
		sizeof(data), actual, restore_failed.temporary_timeout,
		get_timeout, set_timeout, read_pipe) == -EIO);
	CHECK(restore_failed.timeout == 5);
	return 0;
}
