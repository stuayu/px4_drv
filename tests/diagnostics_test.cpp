// Unit tests for shared diagnostic logging, USB history, and TS counters.

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "msg_log.h"
#include "signal_stat.h"
#include "ts_stats.hpp"
#include "usb_history.h"

namespace {

using Packet = std::array<std::uint8_t, 188>;

Packet MakePacket(std::uint16_t pid, std::uint8_t cc, bool payload = true)
{
	Packet packet{};
	packet[0] = 0x47;
	packet[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1f);
	packet[2] = static_cast<std::uint8_t>(pid);
	packet[3] = static_cast<std::uint8_t>((payload ? 0x10 : 0x20) | (cc & 0x0f));
	if (!payload)
		packet[4] = 0;
	return packet;
}

void TestLogSettingsAndFormatting()
{
	int level = 0;
	std::uint32_t categories = 0;
	char line[256]{};

	assert(msg_log_parse_level(" DeBuG ", &level));
	assert(level == PX4_LOG_DEBUG);
	assert(!msg_log_parse_level("verbose", &level));
	assert(msg_log_parse_categories(" usb, ts ", &categories));
	assert(categories == ((1u << PX4_LOG_CAT_USB) | (1u << PX4_LOG_CAT_TS)));
	assert(!msg_log_parse_categories("usb,unknown", &categories));
	assert(categories == ((1u << PX4_LOG_CAT_USB) | (1u << PX4_LOG_CAT_TS)));

	assert(msg_log_is_enabled_for(PX4_LOG_INFO, 1u << PX4_LOG_CAT_TS,
				      PX4_LOG_INFO, PX4_LOG_CAT_TS));
	assert(!msg_log_is_enabled_for(PX4_LOG_INFO, 1u << PX4_LOG_CAT_TS,
				       PX4_LOG_INFO, PX4_LOG_CAT_USB));
	assert(msg_log_is_enabled_for(PX4_LOG_INFO, 0,
				      PX4_LOG_ERROR, PX4_LOG_CAT_USB));

	assert(msg_log_get_stats_interval_ms() == 0);
	assert(msg_log_set_stats_interval("1"));
	assert(msg_log_get_stats_interval_ms() == 1000);
	assert(!msg_log_set_stats_interval("86401"));
	assert(msg_log_get_stats_interval_ms() == 1000);
	assert(!msg_log_set_stats_interval("184467440737095516160000000000000000000000"));
	assert(msg_log_get_stats_interval_ms() == 1000);

	const std::string prefix = "dev\"ice";
	const std::string message = "first\nsecond\t\"quoted\"\n";
	const std::size_t size = msg_log_format_jsonl(line, sizeof(line), 42,
		PX4_LOG_WARN, PX4_LOG_CAT_USB, prefix.c_str(), message.c_str());
	assert(size > 0);
	assert(std::strstr(line, "\"t_ms\":42") != nullptr);
	assert(std::strstr(line, "dev\\\"ice") != nullptr);
	assert(std::strstr(line, "first\\nsecond\\t\\\"quoted\\\"") != nullptr);
	assert(line[size - 1] == '\n');
	assert(std::strchr(line, '\r') == nullptr);
	assert(line[size] == '\0');

	std::int32_t cnr_mdb = 0;
	assert(signal_cn_isdbc_cxd2856er_mdb(126, &cnr_mdb) == 0);
	assert(cnr_mdb > 0);
}

void TestTsContinuityAndCounters()
{
	px4::TsContinuityChecker continuity;
	px4::TsStatsCounter stats;
	std::array<std::uint8_t, 188 * 6> bytes{};

	const Packet packets[] = {
		MakePacket(100, 0),
		MakePacket(100, 1),
		MakePacket(100, 3), // one missing payload packet
		MakePacket(0x1fff, 7),
		MakePacket(101, 5, false), // adaptation only does not advance CC
		MakePacket(102, 0),
	};
	for (std::size_t i = 0; i < 6; ++i)
		std::memcpy(bytes.data() + i * 188, packets[i].data(), 188);
	bytes[5 * 188 + 1] |= 0x80; // transport_error_indicator

	stats.Reset(100);
	stats.AddInput(bytes.data(), bytes.size(), 150);
	const auto &snapshot = stats.Get();
	assert(snapshot.input_packets == 6);
	assert(snapshot.input_bytes == bytes.size());
	assert(snapshot.tei_packets == 1);
	assert(snapshot.null_packets == 1);
	assert(snapshot.cc_gaps == 1);
	assert(snapshot.last_gap_pid == 100);
	assert(snapshot.sync_errors == 0);
	assert(snapshot.max_gap_ms == 50);

	Packet bad_sync = MakePacket(103, 0);
	bad_sync[0] = 0;
	stats.AddInput(bad_sync.data(), bad_sync.size(), 151);
	assert(stats.Get().sync_errors == 1);

	continuity.Reset();
	Packet packet = MakePacket(200, 4);
	assert(!continuity.Check(packet.data()));
	assert(!continuity.Check(packet.data())); // a duplicate packet is tolerated
	packet = MakePacket(200, 9, false); // adaptation-only does not advance the counter
	assert(!continuity.Check(packet.data()));
	packet = MakePacket(200, 5);
	assert(!continuity.Check(packet.data()));

	packet = MakePacket(200, 12);
	packet[3] = static_cast<std::uint8_t>(0x30 | 12); // adaptation + payload
	packet[4] = 1;
	packet[5] = 0x80; // discontinuity_indicator resets this PID
	assert(!continuity.Check(packet.data()));
	packet = MakePacket(200, 13);
	assert(!continuity.Check(packet.data()));

	packet = MakePacket(200, 2);
	packet[3] = static_cast<std::uint8_t>(0x30 | 2);
	packet[4] = 184; // invalid adaptation length; its flags must not be read
	packet[5] = 0x80;
	assert(continuity.Check(packet.data()));
}

void TestUsbHistoryRing()
{
	usb_history_set_enabled(true);
	for (std::uint32_t i = 0; i < USB_HISTORY_CAPACITY + 7; ++i) {
		const std::uint8_t payload[] = { static_cast<std::uint8_t>(i), 0xa5 };
		usb_history_record(USB_HISTORY_CONTROL, USB_HISTORY_OUT,
				  static_cast<std::uint8_t>(i), static_cast<std::uint16_t>(i),
				  0x1234, 2, static_cast<int>(i), payload, sizeof(payload));
	}

	std::array<usb_history_entry, USB_HISTORY_CAPACITY> entries{};
	const auto count = usb_history_copy(entries.data(), entries.size());
	assert(count == entries.size());
	assert(entries.front().request == 7);
	assert(entries.front().value == 7);
	assert(entries.front().result == 7);
	assert(entries.front().data_len == 2);
	assert(entries.front().data[0] == 7 && entries.front().data[1] == 0xa5);
	assert(entries.back().request == static_cast<std::uint8_t>(USB_HISTORY_CAPACITY + 6));
	usb_history_set_enabled(false);
}

void TestUsbHistoryConcurrentWriters()
{
	constexpr unsigned int thread_count = 24;
	constexpr unsigned int records_per_thread = 20000;
	std::atomic<unsigned int> ready{0};
	std::atomic<bool> start{false};
	std::atomic<unsigned int> ticket{0};
	std::vector<std::thread> writers;
	writers.reserve(thread_count);
	usb_history_set_enabled(true);

	for (unsigned int thread = 0; thread < thread_count; ++thread) {
		writers.emplace_back([&] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			for (unsigned int i = 0; i < records_per_thread; ++i) {
				const auto marker = static_cast<std::uint8_t>(ticket.fetch_add(1, std::memory_order_relaxed) % 251);
				const std::uint8_t payload[] = { marker, static_cast<std::uint8_t>(marker ^ 0xa5) };
				usb_history_record(USB_HISTORY_CONTROL, USB_HISTORY_OUT, marker,
					static_cast<std::uint16_t>(marker * 257u + 17u),
					static_cast<std::uint16_t>(marker * 251u + 9u),
					static_cast<std::uint16_t>(marker + 1u),
					static_cast<int>(marker * 4099u + 31u), payload, sizeof(payload));
			}
		});
	}
	while (ready.load(std::memory_order_acquire) != thread_count)
		std::this_thread::yield();
	start.store(true, std::memory_order_release);
	for (auto &writer : writers)
		writer.join();

	std::array<usb_history_entry, USB_HISTORY_CAPACITY> entries{};
	const auto count = usb_history_copy(entries.data(), entries.size());
	for (std::size_t i = 0; i < count; ++i) {
		const auto marker = entries[i].request;
		assert(marker < 251);
		assert(entries[i].value == marker * 257u + 17u);
		assert(entries[i].index == marker * 251u + 9u);
		assert(entries[i].length == marker + 1u);
		assert(entries[i].result == static_cast<int>(marker * 4099u + 31u));
		assert(entries[i].data_len == 2);
		assert(entries[i].data[0] == marker);
		assert(entries[i].data[1] == static_cast<std::uint8_t>(marker ^ 0xa5));
	}
	usb_history_set_enabled(false);
}

} // namespace

int main()
{
	TestLogSettingsAndFormatting();
	TestTsContinuityAndCounters();
	TestUsbHistoryRing();
	TestUsbHistoryConcurrentWriters();
	std::cout << "diagnostics_test: all checks passed\n";
	return 0;
}
