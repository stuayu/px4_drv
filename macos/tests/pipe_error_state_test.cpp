#include "pipe_error_state.hpp"

#include <atomic>
#include <thread>
#include <vector>

int main()
{
	px4::PipeErrorState state;
	std::atomic<bool> invalid{false};
	std::vector<std::thread> workers;

	workers.emplace_back([&] {
		for (int i = 0; i < 100000; ++i)
			state.assign(i, std::generic_category());
	});
	workers.emplace_back([&] {
		for (int i = 0; i < 100000; ++i)
			state.assign(i, std::system_category());
	});
	workers.emplace_back([&] {
		for (int i = 0; i < 100000; ++i) {
			const auto error = state.Get();
			if (error.value() < 0 || error.value() >= 100000)
				invalid.store(true);
		}
	});

	for (auto &worker : workers)
		worker.join();
	return invalid.load() ? 1 : 0;
}
