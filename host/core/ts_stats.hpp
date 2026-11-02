// ts_stats.hpp - 受信 TS の統計 (障害の切り分け用)
//
// 受信経路の StreamBuffer::Write() から呼ぶ。ロックは持たず、呼び出し側が
// 既存のロックの内側で使う。ここには OS 依存の処理を置かない。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace px4 {

/* TS の continuity_counter の欠落を、PID ごとの直前の値と比べて判定する。 */
class TsContinuityChecker final {
public:
	static constexpr std::size_t kPacketSize = 188;
	static constexpr std::uint16_t kNullPid = 0x1fff;

	TsContinuityChecker() noexcept;

	void Reset() noexcept;

	/* 同期の取れた 188 byte の packet 1 つを検査する。欠落を見つけたら true を返す。
	 * 次の条件は欠落にしない。
	 *  - NULL packet (PID 0x1fff)
	 *  - adaptation field の discontinuity_indicator が立っている packet
	 *    (その PID の連続性は次の packet から新しく始まる)
	 *  - payload を持たない packet (adaptation_field_control が 0 または 2)
	 *  - 直前と同じ continuity_counter の packet (ISO/IEC 13818-1 が1回の重複を許す) */
	bool Check(const std::uint8_t *packet) noexcept;

	/* 直近で欠落を見つけた PID。欠落が無ければ 0xffff。 */
	std::uint16_t LastGapPid() const noexcept { return last_gap_pid_; }

private:
	std::array<std::uint8_t, 8192> last_cc_;
	std::array<std::uint8_t, 8192> valid_;
	std::uint16_t last_gap_pid_;
};

/* 1回の報告に出す値。 */
struct TsStatsSnapshot {
	std::uint64_t input_packets;
	std::uint64_t input_bytes;
	std::uint64_t tei_packets;
	std::uint64_t sync_errors;
	std::uint64_t null_packets;
	std::uint64_t cc_gaps;
	std::uint16_t last_gap_pid;
	/* 入力が途切れていた最長の時間 (ミリ秒) */
	std::uint64_t max_gap_ms;
};

class TsStatsCounter final {
public:
	TsStatsCounter() noexcept;

	/* 統計を初期化し、時刻 now_ms を入力の基準にする。 */
	void Reset(std::uint64_t now_ms) noexcept;

	/* 受信データを数える。size が 188 の倍数でない端数は同期を判定できないため読まない。
	 * 同期バイトが 0x47 でない packet は sync_errors に数え、CC は検査しない。 */
	void AddInput(const std::uint8_t *buf, std::size_t size, std::uint64_t now_ms) noexcept;

	const TsStatsSnapshot &Get() const noexcept { return stats_; }

private:
	TsContinuityChecker cc_;
	TsStatsSnapshot stats_;
	std::uint64_t last_input_ms_;
};

} // namespace px4
