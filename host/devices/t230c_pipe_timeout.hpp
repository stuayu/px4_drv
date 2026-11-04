#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace px4::t230c {

/* 短 timeout で読み捨てた後、共有 pipe の元の timeout を必ず戻す。 */
template <typename GetTimeout, typename SetTimeout, typename ReadPipe>
int ReadWithTemporaryTimeout(void *context, std::uint8_t endpoint,
			     std::uint8_t *data, std::size_t size,
			     std::size_t &actual, std::uint32_t timeout_ms,
			     GetTimeout &&get_timeout,
			     SetTimeout &&set_timeout,
			     ReadPipe &&read_pipe)
{
	actual = 0;
	if (!context || !data || !size || !timeout_ms)
		return -EINVAL;

	std::uint32_t previous_timeout = 0;
	int ret = get_timeout(context, endpoint, previous_timeout);
	if (ret)
		return ret;
	ret = set_timeout(context, endpoint, timeout_ms);
	if (ret)
		return ret;

	ret = read_pipe(context, endpoint, data, size, actual);
	const int restore_ret = set_timeout(context, endpoint, previous_timeout);
	return restore_ret ? restore_ret : ret;
}

} // namespace px4::t230c
