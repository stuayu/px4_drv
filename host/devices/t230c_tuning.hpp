#pragma once

#include <cerrno>

namespace px4::t230c {

/* ロック確認が正常応答のまま期限まで未ロックなら、timeout として記録する。 */
inline int ResolveTuneLockResult(int check_result, bool locked) noexcept
{
	if (check_result)
		return check_result;
	return locked ? 0 : -ETIMEDOUT;
}

} // namespace px4::t230c
