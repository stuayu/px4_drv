#include "../../host/core/ts_stream_diagnostics.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); return __LINE__; } } while (0)

namespace {
using Packet = std::array<std::uint8_t, 188>;

Packet MakePacket(std::uint16_t pid, std::uint8_t cc, std::uint8_t payload)
{
    Packet packet{};
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1f);
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = static_cast<std::uint8_t>(0x10 | (cc & 0x0f));
    packet[4] = payload;
    return packet;
}
}

int main()
{
    using namespace px4::ts;
    constexpr std::uint16_t pid = 0x0123;
    StreamDiagnostics diagnostics;
    diagnostics.Reset();

    Packet packet = MakePacket(pid, 14, 0x31);
    PacketEvent event{};
    CHECK(!diagnostics.ObservePacket(packet.data(), 1000, event));

    // One identical retransmission is allowed.
    CHECK(!diagnostics.ObservePacket(packet.data(), 1010, event));

    // A repeated CC with changed bytes is an error, but its missing-packet
    // count cannot be derived from the four-bit counter value.
    Packet changed_duplicate = MakePacket(pid, 14, 0x32);
    CHECK(diagnostics.ObservePacket(changed_duplicate.data(), 1020, event));
    CHECK(event.type == PacketEventType::continuity_error);
    CHECK(event.timestamp_ms == 1020 && event.pid == pid);
    CHECK(event.previous_cc == 14 && event.expected_cc == 15 && event.current_cc == 14);
    CHECK(event.cc_delta == 0 && event.missing_count_ambiguous);
    CHECK(!event.duplicate_payload);

    // CC delta 4 means three missing packets modulo 16.
    Packet jump = MakePacket(pid, 2, 0x33);
    CHECK(diagnostics.ObservePacket(jump.data(), 1030, event));
    CHECK(event.type == PacketEventType::continuity_error);
    CHECK(event.previous_cc == 14 && event.expected_cc == 15 && event.current_cc == 2);
    CHECK(event.cc_delta == 4 && event.missing_mod16 == 3);
    CHECK(!event.missing_count_ambiguous);

    // An adaptation-only packet does not advance CC.
    Packet adaptation_only = MakePacket(pid, 9, 0x00);
    adaptation_only[3] = static_cast<std::uint8_t>(0x20 | 9);
    adaptation_only[4] = 0;
    CHECK(!diagnostics.ObservePacket(adaptation_only.data(), 1040, event));
    Packet next = MakePacket(pid, 3, 0x34);
    CHECK(!diagnostics.ObservePacket(next.data(), 1050, event));

    // discontinuity_indicator resets continuity for this PID.
    Packet discontinuity = MakePacket(pid, 9, 0x00);
    discontinuity[3] = static_cast<std::uint8_t>(0x30 | 9);
    discontinuity[4] = 1;
    discontinuity[5] = 0x80;
    CHECK(!diagnostics.ObservePacket(discontinuity.data(), 1060, event));
    Packet after_discontinuity = MakePacket(pid, 10, 0x35);
    CHECK(!diagnostics.ObservePacket(after_discontinuity.data(), 1070, event));

    Packet tei = MakePacket(pid, 11, 0x36);
    tei[1] |= 0x80;
    CHECK(diagnostics.ObservePacket(tei.data(), 1080, event));
    CHECK(event.type == PacketEventType::transport_error);
    CHECK(event.timestamp_ms == 1080 && event.pid == pid);
    Packet after_tei = MakePacket(pid, 0, 0x37);
    CHECK(!diagnostics.ObservePacket(after_tei.data(), 1090, event));

    diagnostics.RecordTransfer(4000, 4096);
    diagnostics.RecordTransfer(4096, 4096);
    diagnostics.RecordWrite(false, 188, 0);
    diagnostics.RecordWrite(true, 188, 188);

    const auto snapshot = diagnostics.TakeInterval();
    CHECK(snapshot.input_packets == 10);
    CHECK(snapshot.tei_packets == 1);
    CHECK(snapshot.cc_errors == 2);
    CHECK(snapshot.missing_mod16 == 3);
    CHECK(snapshot.transfer_reads == 2 && snapshot.transfer_bytes == 8096);
    CHECK(snapshot.short_reads == 1 && snapshot.short_bytes == 96);
    CHECK(snapshot.stream_write_failures == 1 && snapshot.stream_dropped_bytes == 188);
    CHECK(snapshot.pids.size() == 1);
    CHECK(snapshot.pids[0].pid == pid);
    CHECK(snapshot.pids[0].cc_errors == 2 && snapshot.pids[0].tei_packets == 1);
    CHECK(snapshot.pids[0].missing_mod16 == 3);

    // Taking an interval snapshot clears counters but preserves CC continuity.
    Packet next_interval = MakePacket(pid, 1, 0x38);
    CHECK(!diagnostics.ObservePacket(next_interval.data(), 2000, event));
    const auto next_snapshot = diagnostics.TakeInterval();
    CHECK(next_snapshot.input_packets == 1);
    CHECK(next_snapshot.cc_errors == 0 && next_snapshot.tei_packets == 0);
    CHECK(next_snapshot.transfer_reads == 0 && next_snapshot.stream_write_failures == 0);
    CHECK(next_snapshot.pids.empty());
    return 0;
}
