#include "t230c_stream_stats.hpp"

#include <algorithm>

namespace px4::t230c {
namespace {
constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::uint16_t kNullPid = 0x1fff;
constexpr std::uint8_t kMaxAdaptationLength = 183;

std::uint16_t PacketPid(const std::uint8_t *packet) noexcept
{
	return static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
}

std::uint64_t PacketHash(const std::uint8_t *packet) noexcept
{
	std::uint64_t hash = 14695981039346656037ULL;
	for (std::size_t i = 0; i < 188; ++i) {
		hash ^= packet[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}
} // namespace

void StreamDiagnostics::Reset() noexcept
{
	last_cc_.fill(0);
	valid_.fill(0);
	duplicate_used_.fill(0);
	last_packet_hash_.fill(0);
	pid_window_.fill(PidStats{});
	window_ = StreamStatsSnapshot{};
}

bool StreamDiagnostics::ObservePacket(const std::uint8_t *packet,
				      std::uint64_t timestamp_ms,
				      PacketEvent &event) noexcept
{
	if (!packet || packet[0] != kSyncByte)
		return false;

	const std::uint16_t pid = PacketPid(packet);
	const std::uint8_t cc = packet[3] & 0x0f;
	++window_.input_packets;

	if (pid == kNullPid)
		return false;

	PidStats &pid_stats = pid_window_[pid];
	pid_stats.pid = pid;

	if (packet[1] & 0x80) {
		++window_.tei_packets;
		++pid_stats.tei_packets;
		valid_[pid] = 0;
		duplicate_used_[pid] = 0;
		event = PacketEvent{};
		event.type = PacketEventType::transport_error;
		event.timestamp_ms = timestamp_ms;
		event.pid = pid;
		event.current_cc = cc;
		return true;
	}

	const std::uint8_t adaptation_control = (packet[3] >> 4) & 0x03;
	const bool has_adaptation = (adaptation_control & 0x02) != 0;
	const bool has_payload = (adaptation_control & 0x01) != 0;
	bool discontinuity = false;
	if (has_adaptation && packet[4] >= 1 && packet[4] <= kMaxAdaptationLength)
		discontinuity = (packet[5] & 0x80) != 0;

	const std::uint64_t packet_hash = has_payload ? PacketHash(packet) : 0;
	if (discontinuity) {
		valid_[pid] = 0;
		duplicate_used_[pid] = 0;
		if (has_payload) {
			last_cc_[pid] = cc;
			last_packet_hash_[pid] = packet_hash;
			valid_[pid] = 1;
		}
		return false;
	}

	/* Adaptation-only packets do not advance the continuity counter. */
	if (!has_payload)
		return false;

	bool gap = false;
	bool same_cc = false;
	bool duplicate_payload = false;
	std::uint8_t expected_cc = static_cast<std::uint8_t>((last_cc_[pid] + 1) & 0x0f);
	std::uint8_t delta = 1;
	if (valid_[pid]) {
		same_cc = cc == last_cc_[pid];
		expected_cc = static_cast<std::uint8_t>((last_cc_[pid] + 1) & 0x0f);
		delta = static_cast<std::uint8_t>((cc - last_cc_[pid]) & 0x0f);
		duplicate_payload = same_cc && packet_hash == last_packet_hash_[pid];
		if (same_cc) {
			/* One identical retransmission is allowed by the TS specification. */
			if (duplicate_used_[pid] || !duplicate_payload)
				gap = true;
			duplicate_used_[pid] = 1;
		} else {
			duplicate_used_[pid] = 0;
			if (cc != expected_cc)
				gap = true;
		}
	}

	last_cc_[pid] = cc;
	last_packet_hash_[pid] = packet_hash;
	valid_[pid] = 1;
	if (!gap)
		return false;

	const std::uint8_t missing_mod16 = delta ? static_cast<std::uint8_t>(delta - 1) : 0;
	++window_.cc_errors;
	++pid_stats.cc_errors;
	window_.missing_mod16 += missing_mod16;
	pid_stats.missing_mod16 += missing_mod16;
	event = PacketEvent{};
	event.type = PacketEventType::continuity_error;
	event.timestamp_ms = timestamp_ms;
	event.pid = pid;
	event.previous_cc = static_cast<std::uint8_t>((cc - delta) & 0x0f);
	event.expected_cc = expected_cc;
	event.current_cc = cc;
	event.cc_delta = delta;
	event.missing_mod16 = missing_mod16;
	event.missing_count_ambiguous = same_cc;
	event.duplicate_payload = duplicate_payload;
	return true;
}

void StreamDiagnostics::RecordTransfer(std::size_t actual, std::size_t requested) noexcept
{
	++window_.transfer_reads;
	window_.transfer_bytes += actual;
	if (actual < requested) {
		++window_.short_reads;
		window_.short_bytes += requested - actual;
	}
}

void StreamDiagnostics::RecordWrite(bool success, std::size_t requested,
				    std::size_t written) noexcept
{
	if (success)
		return;
	++window_.stream_write_failures;
	const std::size_t actual = (std::min)(requested, written);
	window_.stream_dropped_bytes += requested - actual;
}

StreamStatsSnapshot StreamDiagnostics::TakeInterval()
{
	StreamStatsSnapshot snapshot = window_;
	snapshot.pids.clear();
	for (std::size_t pid = 0; pid < kPidCount; ++pid) {
		const PidStats &stats = pid_window_[pid];
		if (stats.cc_errors || stats.tei_packets)
			snapshot.pids.push_back(stats);
	}
	window_ = StreamStatsSnapshot{};
	pid_window_.fill(PidStats{});
	return snapshot;
}

} // namespace px4::t230c
