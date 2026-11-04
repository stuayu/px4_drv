#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace px4::t230c {

enum class PacketEventType { continuity_error, transport_error };

struct PacketEvent {
	PacketEventType type = PacketEventType::continuity_error;
	std::uint64_t timestamp_ms = 0;
	std::uint16_t pid = 0;
	std::uint8_t previous_cc = 0;
	std::uint8_t expected_cc = 0;
	std::uint8_t current_cc = 0;
	std::uint8_t cc_delta = 0;
	std::uint8_t missing_mod16 = 0;
	bool missing_count_ambiguous = false;
	bool duplicate_payload = false;
};

struct PidStats {
	std::uint16_t pid = 0;
	std::uint64_t cc_errors = 0;
	std::uint64_t tei_packets = 0;
	std::uint64_t missing_mod16 = 0;
};

struct StreamStatsSnapshot {
	std::uint64_t input_packets = 0;
	std::uint64_t tei_packets = 0;
	std::uint64_t cc_errors = 0;
	std::uint64_t missing_mod16 = 0;
	std::uint64_t transfer_reads = 0;
	std::uint64_t transfer_bytes = 0;
	std::uint64_t short_reads = 0;
	std::uint64_t short_bytes = 0;
	std::uint64_t stream_write_failures = 0;
	std::uint64_t stream_dropped_bytes = 0;
	std::vector<PidStats> pids;
};

/* Called by the single T230C TS worker; snapshots reset interval totals only. */
class StreamDiagnostics final {
public:
	void Reset() noexcept;
	bool ObservePacket(const std::uint8_t *packet, std::uint64_t timestamp_ms,
		PacketEvent &event) noexcept;
	void RecordTransfer(std::size_t actual, std::size_t requested) noexcept;
	void RecordWrite(bool success, std::size_t requested, std::size_t written) noexcept;
	StreamStatsSnapshot TakeInterval();

private:
	static constexpr std::size_t kPidCount = 8192;
	static constexpr std::size_t kPacketSize = 188;

	std::array<std::uint8_t, kPidCount> last_cc_{};
	std::array<std::uint8_t, kPidCount> valid_{};
	std::array<std::uint8_t, kPidCount> duplicate_used_{};
	std::array<std::uint64_t, kPidCount> last_packet_hash_{};
	std::array<PidStats, kPidCount> pid_window_{};
	StreamStatsSnapshot window_{};
};

} // namespace px4::t230c
