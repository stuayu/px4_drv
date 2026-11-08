#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace px4 {

// GetTsStream 等は短い参照を保持し、終了処理は新規参照を止めてから所有参照を外す。
template <typename T>
class StreamQueueSlot final {
public:
	using Pointer = std::shared_ptr<T>;

	void Install(Pointer queue)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_ = std::move(queue);
		accepting_ = false;
	}

	Pointer GetInstalled() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_;
	}

	Pointer Acquire() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return accepting_ ? queue_ : Pointer{};
	}

	bool Enable()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!queue_)
			return false;
		accepting_ = true;
		return true;
	}

	Pointer Disable()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		accepting_ = false;
		return queue_;
	}

	Pointer Reset()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		accepting_ = false;
		return std::exchange(queue_, Pointer{});
	}

private:
	mutable std::mutex mutex_;
	Pointer queue_;
	bool accepting_ = false;
};

} // namespace px4
