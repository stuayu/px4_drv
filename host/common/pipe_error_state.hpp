#pragma once

#include <mutex>
#include <system_error>

namespace px4 {

class PipeErrorState final {
public:
	void assign(int value, const std::error_category &category) noexcept
	{
		std::lock_guard<std::mutex> lock(mutex_);
		condition_.assign(value, category);
	}

	std::error_condition Get() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return condition_;
	}

private:
	mutable std::mutex mutex_;
	std::error_condition condition_;
};

} // namespace px4
