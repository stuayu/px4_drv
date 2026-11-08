#include "io_queue.hpp"
#include "deferred_release.hpp"
#include "stream_queue_slot.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

class TestProvider final : public px4::IoQueue::IoProvider {
public:
	bool Start() override { return true; }
	void Stop() override {}

	bool Do(void *buffer, std::size_t &size) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const char *value = calls_++ == 0 ? "OLD!" : "NEW!";
		std::memcpy(buffer, value, 4);
		size = 4;
		condition_.notify_all();
		return true;
	}

	bool WaitForCalls(int calls, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return condition_.wait_for(lock, timeout, [&] { return calls_ >= calls; });
	}

private:
	std::mutex mutex_;
	std::condition_variable condition_;
	int calls_ = 0;
};

class BlockingProvider final : public px4::IoQueue::IoProvider {
public:
	bool Start() override { return true; }
	void Stop() override {}

	bool Do(void *, std::size_t &) override
	{
		std::unique_lock<std::mutex> lock(mutex_);
		entered_ = true;
		condition_.notify_all();
		condition_.wait(lock, [&] { return released_; });
		return false;
	}

	bool WaitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return condition_.wait_for(lock, std::chrono::seconds(1), [&] { return entered_; });
	}

	void Release()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			released_ = true;
		}
		condition_.notify_all();
	}

private:
	std::mutex mutex_;
	std::condition_variable condition_;
	bool entered_ = false;
	bool released_ = false;
};

bool TestPurgeKeepsReturnedBufferUntilNextRead()
{
	TestProvider provider;
	px4::IoQueue queue(px4::IoQueue::IoOperation::READ, provider, 4, 1, 1, 0);
	if (!queue.Start())
		return false;

	if (!queue.WaitDataBuffer(std::chrono::milliseconds(1000))) {
		queue.Stop();
		return false;
	}

	void *buffer = nullptr;
	std::size_t size = 0;
	std::size_t remain = 0;
	if (!queue.ReadBuffer(&buffer, size, remain, false) || size != 4 ||
		std::memcmp(buffer, "OLD!", 4) != 0) {
		queue.Stop();
		return false;
	}

	queue.PurgeDataBuffer();
	const bool recycled = provider.WaitForCalls(2, std::chrono::milliseconds(250));
	const bool preserved = std::memcmp(buffer, "OLD!", 4) == 0;

	void *next_buffer = nullptr;
	std::size_t next_size = 0;
	std::size_t next_remain = 0;
	const bool released_on_next_call = !queue.ReadBuffer(&next_buffer, next_size, next_remain, false);
	const bool next_available = queue.WaitDataBuffer(std::chrono::milliseconds(1000));
	const bool next_read_succeeded = next_available && queue.ReadBuffer(&next_buffer, next_size, next_remain, false);
	const bool next_read_is_new = next_read_succeeded && next_size == 4 && std::memcmp(next_buffer, "NEW!", 4) == 0;
	queue.Stop();

	if (!preserved)
		std::cerr << "PurgeDataBuffer reused the buffer returned by ReadBuffer" << std::endl;
	if (recycled && preserved)
		std::cerr << "provider reused a returned buffer without overwriting it" << std::endl;
	return preserved && !recycled && released_on_next_call && next_read_is_new;
}

bool TestReleasedPointerCanOutliveItsQueue()
{
	TestProvider provider;
	std::unique_ptr<px4::IoQueue::IoBuffer> retained;
	{
		px4::IoQueue queue(px4::IoQueue::IoOperation::READ, provider, 4, 1, 1, 0);
		if (!queue.Start() || !queue.WaitDataBuffer(std::chrono::milliseconds(1000)))
			return false;

		void *buffer = nullptr;
		std::size_t size = 0;
		std::size_t remain = 0;
		if (!queue.ReadBuffer(&buffer, size, remain, false) || size != 4 ||
			std::memcmp(buffer, "OLD!", 4) != 0) {
			queue.Stop();
			return false;
		}

		queue.Stop();
		void *after_stop = nullptr;
		std::size_t after_stop_size = 0;
		std::size_t after_stop_remain = 0;
		if (queue.WaitDataBuffer(std::chrono::milliseconds(0)) ||
			queue.ReadBuffer(&after_stop, after_stop_size, after_stop_remain, false))
			return false;
		retained = queue.DetachLastReturnedBuffer();
		if (!retained || retained->buf.get() != buffer)
			return false;
	}

	if (!retained || retained->actual_length != 4 ||
		std::memcmp(retained->buf.get(), "OLD!", 4) != 0) {
		std::cerr << "detached GetTsStream buffer did not survive queue destruction" << std::endl;
		return false;
	}
	return true;
}

bool TestStopDoesNotWaitForWaitTsStreamTimeout()
{
	BlockingProvider provider;
	px4::IoQueue queue(px4::IoQueue::IoOperation::READ, provider, 4, 1, 1, 0);
	if (!queue.Start() || !provider.WaitUntilEntered()) {
		provider.Release();
		queue.Stop();
		return false;
	}

	std::mutex mutex;
	std::condition_variable condition;
	bool waiter_started = false;
	bool wait_result = true;
	bool wait_done = false;
	std::thread waiter([&] {
		{
			std::lock_guard<std::mutex> lock(mutex);
			waiter_started = true;
		}
		condition.notify_all();
		wait_result = queue.WaitDataBuffer(std::chrono::milliseconds(0));
		{
			std::lock_guard<std::mutex> lock(mutex);
			wait_done = true;
		}
		condition.notify_all();
	});

	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!condition.wait_for(lock, std::chrono::seconds(1), [&] { return waiter_started; })) {
			provider.Release();
			lock.unlock();
			waiter.join();
			queue.Stop();
			return false;
		}
	}

	std::thread stopper([&] { queue.Stop(); });
	bool woke_before_provider = false;
	{
		std::unique_lock<std::mutex> lock(mutex);
		woke_before_provider = condition.wait_for(lock, std::chrono::milliseconds(500), [&] { return wait_done; });
	}
	provider.Release();
	waiter.join();
	stopper.join();
	if (!woke_before_provider || wait_result) {
		std::cerr << "WaitDataBuffer held the queue lock while waiting for data" << std::endl;
		return false;
	}
	return true;
}

struct ProbeQueue final {
	explicit ProbeQueue(std::atomic<int> &destroyed) : destroyed_(destroyed) {}
	~ProbeQueue() { ++destroyed_; }
	bool Read() const { return destroyed_.load() == 0; }
	std::atomic<int> &destroyed_;
};

bool TestClosingKeepsQueueAliveForAnActiveRead()
{
	std::atomic<int> destroyed{0};
	px4::StreamQueueSlot<ProbeQueue> slot;
	slot.Install(std::make_shared<ProbeQueue>(destroyed));
	if (!slot.Enable())
		return false;

	std::mutex mutex;
	std::condition_variable condition;
	bool acquired = false;
	bool continue_read = false;
	bool read_succeeded = false;
	std::thread reader([&] {
		auto queue = slot.Acquire();
		{
			std::lock_guard<std::mutex> lock(mutex);
			acquired = static_cast<bool>(queue);
		}
		condition.notify_all();

		std::unique_lock<std::mutex> lock(mutex);
		condition.wait(lock, [&] { return continue_read; });
		lock.unlock();
		read_succeeded = queue && queue->Read();
	});

	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!condition.wait_for(lock, std::chrono::seconds(1), [&] { return acquired; })) {
			continue_read = true;
			lock.unlock();
			condition.notify_all();
			reader.join();
			return false;
		}
	}

	if (!slot.Disable() || slot.Acquire()) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			continue_read = true;
		}
		condition.notify_all();
		reader.join();
		return false;
	}
	slot.Reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		continue_read = true;
	}
	condition.notify_all();
	reader.join();

	return read_succeeded && destroyed.load() == 1;
}

struct ProbeShell final {
	explicit ProbeShell(std::atomic<int> &destroyed) : destroyed_(destroyed) {}
	~ProbeShell() { ++destroyed_; }
	void Close() { closed_ = true; }
	bool Call() const { return !closed_; }
	std::atomic<int> &destroyed_;
	bool closed_ = false;
};

struct CountedBuffer final {
	CountedBuffer(std::size_t bytes, std::atomic<std::size_t> &live_bytes)
		: data_(new std::uint8_t[bytes]), bytes_(bytes), live_bytes_(live_bytes)
	{
		live_bytes_ += bytes_;
	}
	~CountedBuffer() { live_bytes_ -= bytes_; }

	std::unique_ptr<std::uint8_t[]> data_;
	std::size_t bytes_;
	std::atomic<std::size_t> &live_bytes_;
};

bool TestDeferredReleasePoolsStayBoundedAndExpire()
{
	constexpr std::size_t max_shells = 8;
	constexpr std::size_t max_buffers = 4;
	constexpr std::size_t buffer_bytes = 188 * 1024;
	const auto grace = std::chrono::milliseconds(25);
	std::atomic<int> destroyed_shells{0};
	std::atomic<int> destroyed_queues{0};
	std::atomic<std::size_t> live_buffer_bytes{0};
	px4::DeferredReleasePool<ProbeShell> shells(grace, max_shells);
	px4::DeferredReleasePool<CountedBuffer> buffers(grace, max_buffers);

	for (int i = 0; i < 2000; ++i) {
		{
			px4::StreamQueueSlot<ProbeQueue> slot;
			slot.Install(std::make_shared<ProbeQueue>(destroyed_queues));
			if (!slot.Enable())
				return false;
			auto active_read = slot.Acquire();
			if (!active_read)
				return false;
			slot.Disable();
			slot.Reset();
		}

		shells.Retain(new ProbeShell(destroyed_shells), sizeof(ProbeShell));
		buffers.Retain(std::make_unique<CountedBuffer>(buffer_bytes, live_buffer_bytes), buffer_bytes);
		if (shells.RetainedCount() > max_shells ||
			shells.RetainedBytes() > (max_shells * sizeof(ProbeShell)) ||
			buffers.RetainedCount() > max_buffers ||
			buffers.RetainedBytes() > (max_buffers * buffer_bytes) ||
			live_buffer_bytes.load() > (max_buffers * buffer_bytes)) {
			std::cerr << "deferred release pool exceeded its configured memory bound" << std::endl;
			return false;
		}
	}

	if (shells.RetainedCount() == 0 || buffers.RetainedCount() == 0 ||
		live_buffer_bytes.load() == 0)
		return false;

	std::this_thread::sleep_for(grace + std::chrono::milliseconds(10));
	shells.Retain(new ProbeShell(destroyed_shells), sizeof(ProbeShell));
	buffers.Retain(std::make_unique<CountedBuffer>(buffer_bytes, live_buffer_bytes), buffer_bytes);
	if (shells.RetainedCount() != 1 || buffers.RetainedCount() != 1 || destroyed_shells.load() != 2000)
		return false;

	std::this_thread::sleep_for(grace + std::chrono::milliseconds(10));
	shells.SweepExpired();
	buffers.SweepExpired();
	return destroyed_queues.load() == 2000 && shells.RetainedCount() == 0 && buffers.RetainedCount() == 0 &&
		destroyed_shells.load() == 2001 && shells.RetainedBytes() == 0 &&
		buffers.RetainedBytes() == 0 &&
		live_buffer_bytes.load() == 0;
}

bool TestReleaseRetainsShellForAnInFlightCall()
{
	std::atomic<int> destroyed{0};
	bool result = false;
	{
		px4::DeferredReleasePool<ProbeShell> pool;
		auto *shell = new ProbeShell(destroyed);
		std::mutex mutex;
		std::condition_variable condition;
		bool entered = false;
		bool continue_call = false;
		std::thread call([&] {
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			condition.notify_all();
			condition.wait(lock, [&] { return continue_call; });
			lock.unlock();
			result = shell->Call();
		});

		{
			std::unique_lock<std::mutex> lock(mutex);
			if (!condition.wait_for(lock, std::chrono::seconds(1), [&] { return entered; })) {
				continue_call = true;
				lock.unlock();
				condition.notify_all();
				call.join();
				delete shell;
				return false;
			}
		}

		shell->Close();
		pool.Retain(shell);
		{
			std::lock_guard<std::mutex> lock(mutex);
			continue_call = true;
		}
		condition.notify_all();
		call.join();
		if (result || destroyed.load() != 0)
			return false;
	}

	return destroyed.load() == 1;
}

} // namespace

int main()
{
	if (!TestPurgeKeepsReturnedBufferUntilNextRead())
		return 1;
	if (!TestReleasedPointerCanOutliveItsQueue())
		return 1;
	if (!TestStopDoesNotWaitForWaitTsStreamTimeout())
		return 1;
	if (!TestClosingKeepsQueueAliveForAnActiveRead()) {
		std::cerr << "closing destroyed the queue during an active read" << std::endl;
		return 1;
	}
	if (!TestReleaseRetainsShellForAnInFlightCall()) {
		std::cerr << "Release destroyed the interface during an in-flight call" << std::endl;
		return 1;
	}
	if (!TestDeferredReleasePoolsStayBoundedAndExpire())
		return 1;
	return 0;
}
