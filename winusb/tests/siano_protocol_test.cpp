// SPDX-License-Identifier: GPL-2.0-or-later
#include "siano_protocol.hpp"
#include "../../host/devices/siano_capture_state.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace {

void check(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "Siano protocol assertion failed at line %d\n", line);
		std::abort();
	}
}

#define require(condition) check((condition), __LINE__)

void put16(std::uint8_t *p, std::uint16_t value)
{
	p[0] = static_cast<std::uint8_t>(value);
	p[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t *p, std::uint32_t value)
{
	p[0] = static_cast<std::uint8_t>(value);
	p[1] = static_cast<std::uint8_t>(value >> 8);
	p[2] = static_cast<std::uint8_t>(value >> 16);
	p[3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> response(std::uint16_t type,
				   const std::uint8_t *payload = nullptr,
				   std::size_t payload_size = 0)
{
	std::vector<std::uint8_t> bytes(8 + payload_size);
	put16(bytes.data(), type);
	bytes[2] = 0;
	bytes[3] = 11;
	put16(bytes.data() + 4, static_cast<std::uint16_t>(bytes.size()));
	put16(bytes.data() + 6, 0);
	if (payload_size)
		std::copy(payload, payload + payload_size, bytes.begin() + 8);
	return bytes;
}

class FakeTransport final : public px4::siano::Transport {
public:
	px4::siano::Protocol *protocol = nullptr;
	std::uint32_t tune_frequency = 0;
	std::uint8_t firmware_id = 6;
	std::uint8_t supported_protocols = 1U << 6;
	std::uint16_t firmware_version = 0x0800;
	unsigned int firmware_chunks = 0;
	unsigned int pid_filter_requests = 0;
	unsigned int statistics_requests = 0;
	bool stale_lock_before_tune_response = false;
	bool signal_detected_without_statistics = false;
	bool send_ts_after_pid_filter = true;
	bool rf_locked = false;
	bool demod_locked = true;
	bool external_lna_on = false;

	int Send(const std::uint8_t *message, std::size_t length,
		 std::uint32_t) noexcept override
	{
		if (length < 8)
			return -EINVAL;
		const auto type = static_cast<std::uint16_t>(message[0]) |
			(static_cast<std::uint16_t>(message[1]) << 8);
		if (type == 668) { // GET_VERSION_EX
			std::array<std::uint8_t, 12> payload{};
			payload[4] = firmware_id;
			payload[5] = supported_protocols;
			payload[10] = static_cast<std::uint8_t>(firmware_version >> 8);
			payload[11] = static_cast<std::uint8_t>(firmware_version);
			auto reply = response(669, payload.data(), payload.size());
			return protocol->HandleTransfer(reply.data(), reply.size());
		}
		if (type == 776) { // ISDB-T tune request
			tune_frequency = static_cast<std::uint32_t>(message[8]) |
				(static_cast<std::uint32_t>(message[9]) << 8) |
				(static_cast<std::uint32_t>(message[10]) << 16) |
				(static_cast<std::uint32_t>(message[11]) << 24);
			if (stale_lock_before_tune_response) {
				auto old_lock = response(827);
				if (protocol->HandleTransfer(old_lock.data(), old_lock.size()))
					return -EIO;
			}
			auto tune_reply = response(777);
			return protocol->HandleTransfer(tune_reply.data(), tune_reply.size());
		}
		if (type == 653) { // GET_STATISTICS_EX
			++statistics_requests;
			if (signal_detected_without_statistics) {
				auto signal = response(827);
				return protocol->HandleTransfer(signal.data(), signal.size());
			}
			/* GET_STATISTICS_EX_RES は request_result の4 byte後に stats_ex を返す。 */
			std::array<std::uint8_t, 36> stats{};
			put32(stats.data(), 0); // sms_msg_statistics_info::request_result
			put32(stats.data() + 12, rf_locked ? 1 : 0);
			put32(stats.data() + 16, demod_locked && !stale_lock_before_tune_response ? 1 : 0);
			put32(stats.data() + 20, external_lna_on ? 1 : 0);
			put32(stats.data() + 24, 13); // SNR: 13 dB
			put32(stats.data() + 28, static_cast<std::uint32_t>(-74)); // RSSI: -74 dBm
			auto stats_reply = response(654, stats.data(), stats.size());
			return protocol->HandleTransfer(stats_reply.data(), stats_reply.size());
		}
		if (type == 615) { // GET_STATISTICS
			std::array<std::uint8_t, 24> stats{};
			stats[12] = stale_lock_before_tune_response ? 0 : 1;
			stats[20] = 13; // SNR: 13 dB
			auto stats_reply = response(616, stats.data(), stats.size());
			return protocol->HandleTransfer(stats_reply.data(), stats_reply.size());
		}
		if (type == 601) { // ADD_PID_FILTER_REQ
			++pid_filter_requests;
			if (send_ts_after_pid_filter) {
				std::array<std::uint8_t, 188> ts{};
				ts[0] = 0x47;
				auto data = response(693, ts.data(), ts.size());
				return protocol->HandleTransfer(data.data(), data.size());
			}
			return 0;
		}
		if (type == 660) { // firmware chunk
			++firmware_chunks;
			auto reply = response(661);
			return protocol->HandleTransfer(reply.data(), reply.size());
		}
		if (type == 662) { // firmware validity
			auto reply = response(663);
			return protocol->HandleTransfer(reply.data(), reply.size());
		}
		if (type == 664) { // cold firmware start
			firmware_id = 6;
			supported_protocols = 1U << 6;
			auto reply = response(665);
			return protocol->HandleTransfer(reply.data(), reply.size());
		}
		return 0;
	}
};

} // namespace

int main()
{
	FakeTransport transport;
	std::size_t ts_bytes = 0;
	std::uint8_t first_ts_byte = 0;
	px4::siano::Protocol protocol(transport, 0,
		[&](const std::uint8_t *data, std::size_t size) {
			ts_bytes = size;
			first_ts_byte = size ? data[0] : 0;
		});
	transport.protocol = &protocol;

	require(protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
				    std::chrono::milliseconds(100)) == 0);
	require(transport.pid_filter_requests == 0);
	require(protocol.TuneFrequencyHz(557142857U, std::chrono::milliseconds(500)) == 0);
	require(transport.pid_filter_requests == 1);
	require(transport.tune_frequency == 557142857U);
	require(protocol.IsLocked());
	std::int32_t cnr_millidb = 0;
	require(protocol.ReadCnrMilliDb(cnr_millidb) == 0);
	require(cnr_millidb == 13000);
	const unsigned int requests_before_refresh = transport.statistics_requests;
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	require(protocol.ReadCnrMilliDb(cnr_millidb) == 0);
	require(cnr_millidb == 13000);
	require(transport.statistics_requests > requests_before_refresh);

	{
		FakeTransport legacy_transport;
		legacy_transport.firmware_version = 0x0700;
		px4::siano::Protocol legacy_protocol(legacy_transport, 0, {});
		legacy_transport.protocol = &legacy_protocol;
		require(legacy_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
						  std::chrono::milliseconds(100)) == 0);
		require(legacy_protocol.TuneFrequencyHz(557142857U,
								std::chrono::milliseconds(500)) == 0);
		require(legacy_transport.pid_filter_requests == 1);
		std::int32_t legacy_cnr = 0;
		require(legacy_protocol.ReadCnrMilliDb(legacy_cnr) == 0);
		require(legacy_cnr == 13000);
	}

	{
		FakeTransport stale_transport;
		stale_transport.stale_lock_before_tune_response = true;
		px4::siano::Protocol stale_protocol(stale_transport, 0, {});
		stale_transport.protocol = &stale_protocol;
		require(stale_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
						 std::chrono::milliseconds(100)) == 0);
		require(stale_protocol.TuneFrequencyHz(557142857U,
								 std::chrono::milliseconds(250)) == -ETIMEDOUT);
	}

	std::array<std::uint8_t, 196> ts_message{};
	put16(ts_message.data(), 693);
	put16(ts_message.data() + 4, static_cast<std::uint16_t>(ts_message.size()));
	ts_message[8] = 0x47;
	require(protocol.HandleTransfer(ts_message.data(), ts_message.size()) == 0);
	require(ts_bytes == 188);
	require(first_ts_byte == 0x47);
	require(protocol.HandleTransfer(ts_message.data(), 7) == 0);
	{
		std::array<std::uint8_t, 8> invalid_header{};
		put16(invalid_header.data(), 669);
		put16(invalid_header.data() + 4, 7);
		FakeTransport invalid_transport;
		px4::siano::Protocol invalid_protocol(invalid_transport, 0, {});
		require(invalid_protocol.HandleTransfer(invalid_header.data(), invalid_header.size()) == -EBADMSG);
	}
	{
		std::array<std::uint8_t, 8> overflow_header{};
		put16(overflow_header.data(), 669);
		put16(overflow_header.data() + 4, 8);
		put16(overflow_header.data() + 6, 0x0104);
		FakeTransport overflow_transport;
		px4::siano::Protocol overflow_protocol(overflow_transport,
			(std::numeric_limits<std::size_t>::max)(), {});
		require(overflow_protocol.HandleTransfer(overflow_header.data(), overflow_header.size()) == -EOVERFLOW);
	}
	{
		std::array<std::uint8_t, 8> incomplete_version{};
		put16(incomplete_version.data(), 669);
		put16(incomplete_version.data() + 4, 8);
		require(protocol.HandleTransfer(incomplete_version.data(),
						incomplete_version.size()) == -EMSGSIZE);
	}

	{
		const auto firmware_path = std::filesystem::temp_directory_path() /
			"px4drv_siano_firmware_fixture.inp";
		const std::array<std::uint8_t, 16> firmware = {
			0, 0, 0, 0, 4, 0, 0, 0, 0x00, 0x10, 0x00, 0x00,
			0x11, 0x22, 0x33, 0x44,
		};
		{
			std::ofstream file(firmware_path, std::ios::binary);
			if (!file.write(reinterpret_cast<const char *>(firmware.data()),
					 static_cast<std::streamsize>(firmware.size())))
				return 2;
		}

		FakeTransport cold_transport;
		cold_transport.firmware_id = 255;
		cold_transport.supported_protocols = 0;
		px4::siano::Protocol cold_protocol(cold_transport, 0, {});
		cold_transport.protocol = &cold_protocol;
		require(cold_protocol.Initialize(firmware_path, std::chrono::seconds(2)) == 0);
		require(cold_transport.firmware_id == 6);
		require(cold_transport.firmware_chunks == 1);
		require(cold_transport.pid_filter_requests == 0);
		std::filesystem::remove(firmware_path);
	}

	{
		FakeTransport unlocked_transport;
		unlocked_transport.rf_locked = true;
		unlocked_transport.demod_locked = false;
		unlocked_transport.external_lna_on = true;
		px4::siano::Protocol unlocked_protocol(unlocked_transport, 0, {});
		unlocked_transport.protocol = &unlocked_protocol;
		require(unlocked_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
						    std::chrono::milliseconds(100)) == 0);
		require(unlocked_protocol.TuneFrequencyHz(557142857U,
								 std::chrono::milliseconds(250)) == -ETIMEDOUT);
		require(!unlocked_protocol.IsLocked());
	}

	{
		FakeTransport signal_only_transport;
		signal_only_transport.signal_detected_without_statistics = true;
		px4::siano::Protocol signal_only_protocol(signal_only_transport, 0, {});
		signal_only_transport.protocol = &signal_only_protocol;
		require(signal_only_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
							    std::chrono::milliseconds(100)) == 0);
		require(signal_only_protocol.TuneFrequencyHz(557142857U,
								 std::chrono::milliseconds(1000)) == -ETIMEDOUT);
		require(!signal_only_protocol.IsLocked());
		require(signal_only_transport.pid_filter_requests == 0);
	}

	{
		FakeTransport no_ts_transport;
		no_ts_transport.send_ts_after_pid_filter = false;
		px4::siano::Protocol no_ts_protocol(no_ts_transport, 0, {});
		no_ts_transport.protocol = &no_ts_protocol;
		require(no_ts_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
							 std::chrono::milliseconds(100)) == 0);
		require(no_ts_protocol.TuneFrequencyHz(557142857U,
								 std::chrono::milliseconds(250)) == -ETIMEDOUT);
		require(no_ts_transport.pid_filter_requests == 1);
	}

	{
		FakeTransport stale_ts_transport;
		px4::siano::Protocol stale_ts_protocol(stale_ts_transport, 0, {});
		stale_ts_transport.protocol = &stale_ts_protocol;
		require(stale_ts_protocol.Initialize(std::filesystem::path("unused-firmware.inp"),
							      std::chrono::milliseconds(100)) == 0);
		require(stale_ts_protocol.TuneFrequencyHz(557142857U,
								    std::chrono::milliseconds(250)) == 0);
		stale_ts_transport.send_ts_after_pid_filter = false;
		require(stale_ts_protocol.TuneFrequencyHz(563142857U,
								    std::chrono::milliseconds(250)) == -ETIMEDOUT);
	}

	{
		px4::siano::CaptureState capture;
		capture.StartBuffer();
		capture.EnableCapture();
		require(capture.IsBufferStarted());
		require(capture.IsEnabled());
		require(capture.PauseForTune());
		capture.FinishTune(false);
		require(capture.IsBufferStarted());
		require(!capture.IsEnabled());
		require(!capture.PauseForTune());
		capture.FinishTune(true);
		require(capture.IsEnabled());
	}
}
