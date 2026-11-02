// ts_stats.cpp - 受信 TS の統計と continuity_counter の判定

#include "ts_stats.hpp"

#include <algorithm>

namespace px4 {

namespace {

constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::uint16_t kNoGapPid = 0xffff;
/* adaptation_field_length の上限。188 byte の packet では 4 byte の header を除いた 183 byte。 */
constexpr std::uint8_t kMaxAdaptationLength = 183;

std::uint16_t packet_pid(const std::uint8_t *packet) noexcept
{
	return static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
}

} // namespace

TsContinuityChecker::TsContinuityChecker() noexcept
	: last_cc_{},
	valid_{},
	last_gap_pid_(kNoGapPid)
{
}

void TsContinuityChecker::Reset() noexcept
{
	last_cc_.fill(0);
	valid_.fill(0);
	last_gap_pid_ = kNoGapPid;
}

bool TsContinuityChecker::Check(const std::uint8_t *packet) noexcept
{
	const std::uint16_t pid = packet_pid(packet);
	if (pid == kNullPid)
		return false;

	const std::uint8_t adaptation_control = packet[3] & 0x30;
	const bool has_adaptation = (adaptation_control & 0x20) != 0;
	const bool has_payload = (adaptation_control & 0x10) != 0;
	const std::uint8_t cc = packet[3] & 0x0f;

	/* adaptation_field_length が packet の内側に収まる場合だけ flags を読む。
	 * 長さ 0 は flags を持たない。 */
	bool discontinuity = false;
	if (has_adaptation && packet[4] >= 1 && packet[4] <= kMaxAdaptationLength)
		discontinuity = (packet[5] & 0x80) != 0;

	if (discontinuity) {
		/* この PID の連続性はこの packet から新しく始まる。 */
		valid_[pid] = 0;
		if (has_payload) {
			last_cc_[pid] = cc;
			valid_[pid] = 1;
		}
		return false;
	}

	/* payload の無い packet は continuity_counter を進めない。 */
	if (!has_payload)
		return false;

	bool gap = false;
	if (valid_[pid]) {
		const std::uint8_t expected = static_cast<std::uint8_t>((last_cc_[pid] + 1) & 0x0f);
		/* 直前と同じ値は ISO/IEC 13818-1 が許す重複 packet。 */
		if (cc != expected && cc != last_cc_[pid])
			gap = true;
	}

	last_cc_[pid] = cc;
	valid_[pid] = 1;
	if (gap)
		last_gap_pid_ = pid;
	return gap;
}

TsStatsCounter::TsStatsCounter() noexcept
	: stats_{},
	last_input_ms_(0)
{
}

void TsStatsCounter::Reset(std::uint64_t now_ms) noexcept
{
	cc_.Reset();
	stats_ = TsStatsSnapshot{};
	stats_.last_gap_pid = kNoGapPid;
	last_input_ms_ = now_ms;
}

void TsStatsCounter::AddInput(const std::uint8_t *buf, std::size_t size, std::uint64_t now_ms) noexcept
{
	if (!buf || size == 0)
		return;

	/* 無受信時間は、前回のデータ到着から今回までの間隔の最大値。 */
	if (now_ms > last_input_ms_)
		stats_.max_gap_ms = std::max(stats_.max_gap_ms, now_ms - last_input_ms_);
	last_input_ms_ = now_ms;

	stats_.input_bytes += size;

	for (std::size_t offset = 0; offset + TsContinuityChecker::kPacketSize <= size;
	     offset += TsContinuityChecker::kPacketSize) {
		const std::uint8_t *packet = buf + offset;

		if (packet[0] != kSyncByte) {
			stats_.sync_errors++;
			continue;
		}

		stats_.input_packets++;
		if (packet[1] & 0x80)
			stats_.tei_packets++;
		if (packet_pid(packet) == TsContinuityChecker::kNullPid)
			stats_.null_packets++;

		if (cc_.Check(packet)) {
			stats_.cc_gaps++;
			stats_.last_gap_pid = cc_.LastGapPid();
		}
	}
}

} // namespace px4
