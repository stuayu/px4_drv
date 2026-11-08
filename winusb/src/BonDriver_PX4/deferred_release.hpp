#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace px4 {

// Release 後の短い猶予だけ公開オブジェクトを保持する。
// 期限切れと上限超過の項目は、次の Retain/Create でロック外から破棄する。
template <typename T>
class DeferredReleasePool final {
public:
	using Clock = std::chrono::steady_clock;

	explicit DeferredReleasePool(std::chrono::milliseconds grace = std::chrono::milliseconds(30000),
		std::size_t max_count = 8)
		: grace_(grace), max_count_(max_count), retained_bytes_(0)
	{}

	void Retain(T *object, std::size_t bytes = sizeof(T))
	{
		if (!object)
			return;
		Retain(std::unique_ptr<T>(object), bytes);
	}

	void Retain(std::unique_ptr<T> object, std::size_t bytes = sizeof(T))
	{
		if (!object)
			return;

		std::vector<Entry> retired;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto now = Clock::now();
			SweepExpiredLocked(now, retired);
			objects_.push_back(Entry{ now, std::move(object), bytes });
			retained_bytes_ += bytes;
			while (objects_.size() > max_count_) {
				retained_bytes_ -= objects_.front().bytes;
				retired.emplace_back(std::move(objects_.front()));
				objects_.pop_front();
			}
		}
		// object の destructor が個別 mutex を取るため、pool mutex の外で破棄する。
		retired.clear();
	}

	void SweepExpired()
	{
		std::vector<Entry> retired;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			SweepExpiredLocked(Clock::now(), retired);
		}
		retired.clear();
	}

	std::size_t RetainedCount() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return objects_.size();
	}

	std::size_t RetainedBytes() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return retained_bytes_;
	}

private:
	struct Entry final {
		Clock::time_point retained_at;
		std::unique_ptr<T> object;
		std::size_t bytes;
	};

	void SweepExpiredLocked(Clock::time_point now, std::vector<Entry> &retired)
	{
		for (auto it = objects_.begin(); it != objects_.end();) {
			if ((now - it->retained_at) >= grace_) {
				retained_bytes_ -= it->bytes;
				retired.emplace_back(std::move(*it));
				it = objects_.erase(it);
			} else {
				++it;
			}
		}
	}

	std::chrono::milliseconds grace_;
	std::size_t max_count_;
	mutable std::mutex mutex_;
	std::deque<Entry> objects_;
	std::size_t retained_bytes_;
};

} // namespace px4
