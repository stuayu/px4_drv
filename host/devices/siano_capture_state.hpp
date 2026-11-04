// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>

namespace px4::siano {

/* 受信バッファーの要求状態と、現在 TS を渡せる状態を分けて保持する。 */
class CaptureState final {
public:
	bool IsBufferStarted() const noexcept { return buffer_started_; }
	bool IsEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

	void StartBuffer() noexcept { buffer_started_ = true; }
	void StopBuffer() noexcept
	{
		buffer_started_ = false;
		enabled_.store(false, std::memory_order_release);
	}

	void EnableCapture() noexcept
	{
		enabled_.store(buffer_started_, std::memory_order_release);
	}

	bool PauseForTune() noexcept
	{
		return enabled_.exchange(false, std::memory_order_acq_rel);
	}

	void RestoreAfterTunePreparation(bool was_enabled) noexcept
	{
		enabled_.store(was_enabled && buffer_started_, std::memory_order_release);
	}

	void FinishTune(bool succeeded) noexcept
	{
		/* 前回の選局失敗で一時無効になっていても、開始済みバッファーへ戻す。 */
		enabled_.store(succeeded && buffer_started_, std::memory_order_release);
	}

private:
	bool buffer_started_ = false;
	std::atomic_bool enabled_{false};
};

} // namespace px4::siano
