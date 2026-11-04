// SPDX-License-Identifier: GPL-2.0-or-later
// Siano SMS framing and RIO ISDB-T control follow drivers/media/common/siano
// in Linux and the GPL-2.0-or-later Khronos31/siano-userland implementation.

#include "siano_protocol.hpp"

#include "siano_wire.hpp"
#include "msg.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace px4::siano {
namespace {

constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kMaxPayloadSize = 240;
constexpr std::uint16_t kGetVersionRequest = 668;
constexpr std::uint16_t kGetVersionResponse = 669;
constexpr std::uint16_t kInitDeviceRequest = 578;
constexpr std::uint16_t kInitDeviceResponse = 579;
constexpr std::uint16_t kAddPidFilterRequest = 601;
constexpr std::uint16_t kGetStatisticsRequest = 615;
constexpr std::uint16_t kGetStatisticsExRequest = 653;
constexpr std::uint16_t kGetStatisticsResponse = 616;
constexpr std::uint16_t kGetStatisticsExResponse = 654;
constexpr std::uint16_t kDataDownloadRequest = 660;
constexpr std::uint16_t kDataDownloadResponse = 661;
constexpr std::uint16_t kDataValidityRequest = 662;
constexpr std::uint16_t kDataValidityResponse = 663;
constexpr std::uint16_t kSoftwareDownloadTriggerRequest = 664;
constexpr std::uint16_t kSoftwareDownloadTriggerResponse = 665;
constexpr std::uint16_t kBdaData = 693;
constexpr std::uint16_t kReloadStartRequest = 702;
constexpr std::uint16_t kReloadStartResponse = 703;
constexpr std::uint16_t kReloadExecuteRequest = 704;
constexpr std::uint16_t kIsdbtTuneRequest = 776;
constexpr std::uint16_t kIsdbtTuneResponse = 777;
constexpr std::uint16_t kSignalDetected = 827;
constexpr std::uint16_t kNoSignal = 828;
constexpr std::uint8_t kHifTask = 11;
constexpr std::uint8_t kBdaControlMessage = 201;
constexpr std::uint32_t kModeIsdbtBda = 6;
constexpr std::uint32_t kBandwidthIsdbt13Segment = 8;
constexpr std::uint32_t kIsdbtSampleRate = 12000000;
constexpr std::uint32_t kCatchAllPid = 0x2000;
constexpr std::uint32_t kFirmwareModeNone = 255;
constexpr std::uint32_t kFirmwareModeIsdbt = 6;
constexpr std::uint32_t kTunePollIntervalMs = 100; // upstream sms DVB driver と同じく統計要求を最大 10 Hz に抑える。
constexpr std::uint32_t kFirmwareStartWaitMs = 400; // Siano RIO のファームウェア起動後に置く待機時間。

void WriteLe16(std::uint8_t *p, std::uint16_t value) noexcept
{
	p[0] = static_cast<std::uint8_t>(value);
	p[1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteLe32(std::uint8_t *p, std::uint32_t value) noexcept
{
	p[0] = static_cast<std::uint8_t>(value);
	p[1] = static_cast<std::uint8_t>(value >> 8);
	p[2] = static_cast<std::uint8_t>(value >> 16);
	p[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t ReadLe32(const std::uint8_t *p) noexcept
{
	return static_cast<std::uint32_t>(p[0]) |
		(static_cast<std::uint32_t>(p[1]) << 8) |
		(static_cast<std::uint32_t>(p[2]) << 16) |
		(static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t ReadLeS32(const std::uint8_t *p) noexcept
{
	const std::uint32_t bits = ReadLe32(p);
	std::int32_t value = 0;
	static_assert(sizeof(value) == sizeof(bits), "32-bit Siano statistic expected");
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void PackHeader(std::uint8_t *message, std::uint16_t type, std::uint8_t source,
		std::uint8_t destination, std::uint16_t length) noexcept
{
	WriteLe16(message, type);
	message[2] = source;
	message[3] = destination;
	WriteLe16(message + 4, length);
	WriteLe16(message + 6, 0);
}

std::uint32_t RemainingMilliseconds(
	const std::chrono::steady_clock::time_point &deadline) noexcept
{
	const auto remaining = deadline - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero())
		return 0;
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
	const auto ms = (ns + 999999) / 1000000;
	return static_cast<std::uint32_t>((std::min<std::int64_t>)(
		ms, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

class Protocol::Impl final {
public:
	Impl(Transport &transport, std::size_t response_alignment, TsHandler handler)
		: transport_(transport), response_alignment_(response_alignment),
		  ts_handler_(std::move(handler))
	{
	}

	int Initialize(const std::filesystem::path &firmware_path,
		       std::chrono::milliseconds timeout) noexcept
	{
		try {
			if (timeout <= std::chrono::milliseconds::zero())
				return -ETIMEDOUT;
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				pid_filter_configured_ = false;
			}
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			int ret = GetVersion(deadline);
			if (ret) {
				msg_err("Siano protocol: GET_VERSION failed (ret: %d)\n", ret);
				return ret;
			}

			Version current = GetVersionSnapshot();
			/* 起動済みの ISDB-T BDA モードへ INIT_DEVICE を重ねると HIF が止まるため、再投入しない。 */
			if (current.firmware_id != kFirmwareModeIsdbt) {
				if (!(current.supported_protocols & (1U << kFirmwareModeIsdbt))) {
					ret = LoadFirmware(firmware_path, current.firmware_id, deadline);
					if (ret)
						return ret;
					ret = GetVersion(deadline);
					if (ret)
						return ret;
					current = GetVersionSnapshot();
				}

				if (current.firmware_id != kFirmwareModeIsdbt &&
				    current.firmware_version >= 0x0800) {
					std::array<std::uint8_t, kHeaderSize + 4> message{};
					PackHeader(message.data(), kInitDeviceRequest, 0, kHifTask,
						   static_cast<std::uint16_t>(message.size()));
					WriteLe32(message.data() + kHeaderSize, kModeIsdbtBda);
					ret = SendAndWait(message.data(), message.size(),
							 kInitDeviceResponse, deadline);
					if (ret)
						return ret;
				}
			}

			current = GetVersionSnapshot();
			if (current.firmware_id != kFirmwareModeIsdbt &&
			    !(current.supported_protocols & (1U << kFirmwareModeIsdbt)))
				return -ENOTSUP;

			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				initialized_ = true;
			}
			return 0;
		} catch (const std::bad_alloc &) {
			return -ENOMEM;
		} catch (...) {
			return -EIO;
		}
	}

	int TuneFrequencyHz(std::uint32_t frequency_hz,
			    std::chrono::milliseconds timeout) noexcept
	{
		try {
			if (frequency_hz == 0 || timeout <= std::chrono::milliseconds::zero())
				return -EINVAL;
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				if (!initialized_)
					return -EPIPE;
				if (disconnected_)
					return -ENODEV;
				locked_ = false;
				lock_status_valid_ = false;
				stats_valid_ = false;
				next_statistics_request_ = {};
			}
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			std::array<std::uint8_t, kHeaderSize + 16> message{};
			PackHeader(message.data(), kIsdbtTuneRequest, kBdaControlMessage,
				   kHifTask, static_cast<std::uint16_t>(message.size()));
			WriteLe32(message.data() + kHeaderSize, frequency_hz);
			WriteLe32(message.data() + kHeaderSize + 4, kBandwidthIsdbt13Segment);
			WriteLe32(message.data() + kHeaderSize + 8, kIsdbtSampleRate);
			WriteLe32(message.data() + kHeaderSize + 12, 0);
			int ret = SendAndWait(message.data(), message.size(),
					      kIsdbtTuneResponse, deadline);
			if (ret)
				return ret;
			/* TUNE応答前の旧チャンネルデータを診断ログへ混ぜない。 */
			first_bda_data_logged_.store(false, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				/* 信号ロック通知が TUNE 応答より先に届く場合があるため、応答後に状態を切り直す。 */
				locked_ = false;
				lock_status_valid_ = false;
				stats_valid_ = false;
				first_data_received_ = false;
			}

			for (;;) {
				bool locked = false;
				bool lock_status_valid = false;
				bool stats_valid = false;
				{
					std::unique_lock<std::mutex> lock(state_mutex_);
					if (disconnected_)
						return -ENODEV;
					locked = locked_;
					lock_status_valid = lock_status_valid_;
					stats_valid = stats_valid_;
					if (!lock_status_valid || !locked) {
						const auto remaining = deadline - std::chrono::steady_clock::now();
						if (remaining <= std::chrono::steady_clock::duration::zero())
							return -ETIMEDOUT;
						response_changed_.wait_until(
							lock,
							(std::min)(deadline, std::chrono::steady_clock::now() +
								std::chrono::milliseconds(kTunePollIntervalMs)),
							[this] { return disconnected_ || (lock_status_valid_ && locked_); });
						if (disconnected_)
							return -ENODEV;
						locked = locked_;
						lock_status_valid = lock_status_valid_;
						stats_valid = stats_valid_;
					}
				}
				if (lock_status_valid && locked) {
					if (!stats_valid) {
						std::array<std::uint8_t, kHeaderSize> stats{};
						const Version version = GetVersionSnapshot();
						const auto request = version.firmware_version >= 0x0800
							? kGetStatisticsExRequest : kGetStatisticsRequest;
						PackHeader(stats.data(), request, kBdaControlMessage, kHifTask,
							   static_cast<std::uint16_t>(stats.size()));
						(void)Send(stats.data(), stats.size(), deadline);
						ScheduleNextStatisticsRequest();
					}
					ret = ConfigureCatchAllPid(deadline);
					if (ret)
						return ret;
					/* demod lockだけでは受信開始を確認できない。BDA_DATAを受け取るまで同じ期限で待つ。 */
					std::unique_lock<std::mutex> lock(state_mutex_);
					const bool received = response_changed_.wait_until(lock, deadline, [this] {
						return disconnected_ || first_data_received_;
					});
					if (disconnected_)
						return -ENODEV;
					return received && first_data_received_ ? 0 : -ETIMEDOUT;
				}

				std::array<std::uint8_t, kHeaderSize> stats{};
				const Version version = GetVersionSnapshot();
				const auto request = version.firmware_version >= 0x0800
					? kGetStatisticsExRequest : kGetStatisticsRequest;
				PackHeader(stats.data(), request, kBdaControlMessage, kHifTask,
					   static_cast<std::uint16_t>(stats.size()));
				ret = Send(stats.data(), stats.size(), deadline);
				ScheduleNextStatisticsRequest();
				if (ret && ret != -ETIMEDOUT)
					return ret;
			}
		} catch (const std::bad_alloc &) {
			return -ENOMEM;
		} catch (...) {
			return -EIO;
		}
	}

	int HandleTransfer(const std::uint8_t *buffer, std::size_t actual_length) noexcept
	{
		MessageFrame frame{};
		const int ret = DecodeMessage(buffer, actual_length, response_alignment_, frame);
		if (ret) {
			const std::uint16_t message_type = buffer && actual_length >= 2
				? static_cast<std::uint16_t>(buffer[0] |
					(static_cast<std::uint16_t>(buffer[1]) << 8)) : 0;
			RecordDecodeError(ret, actual_length, message_type);
			/* 実長不足はsmsusbと同様にこの転送だけを捨てる。宣言長不正や
			 * オフセット計算のoverflowは形式破損なので上位へ失敗を伝える。 */
			if (ret == -EMSGSIZE)
				return 0;
			return ret;
		}
		const std::uint8_t *payload = buffer + frame.payload_offset;
		if (frame.type == kGetStatisticsExResponse) {
			const auto read_u32_at = [&](std::size_t offset) {
				return frame.payload_length >= offset + sizeof(std::uint32_t)
					? ReadLe32(payload + offset) : (std::numeric_limits<std::uint32_t>::max)();
			};
			msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_DEMOD,
				   "Siano GET_STATISTICS_EX_RES payload=%zu raw0=%u raw4=%u raw8=%u raw12=%u raw16=%u raw20=%u raw24=%u raw28=%u raw32=%u raw36=%u\n",
				   frame.payload_length, read_u32_at(0), read_u32_at(4),
				   read_u32_at(8), read_u32_at(12), read_u32_at(16),
				   read_u32_at(20), read_u32_at(24), read_u32_at(28),
				   read_u32_at(32), read_u32_at(36));
		} else if (frame.type == kGetStatisticsResponse) {
			msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_DEMOD,
				   "Siano GET_STATISTICS_RES payload=%zu raw0=%u raw12=%u raw16=%u raw20=%u raw24=%u\n",
				   frame.payload_length,
				   frame.payload_length >= 4 ? ReadLe32(payload) : 0,
				   frame.payload_length >= 16 ? ReadLe32(payload + 12) : 0,
				   frame.payload_length >= 20 ? ReadLe32(payload + 16) : 0,
				   frame.payload_length >= 24 ? ReadLe32(payload + 20) : 0,
				   frame.payload_length >= 28 ? ReadLe32(payload + 24) : 0);
		} else if (frame.type == kSignalDetected || frame.type == kNoSignal) {
			msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_DEMOD,
				   "Siano signal event type=%u\n", frame.type);
		}
		/* Mode/lock を判定する必須フィールドが欠けた応答は、成功扱いにしない。 */
		if ((frame.type == kGetVersionResponse && frame.payload_length < 12) ||
		    (frame.type == kGetStatisticsExResponse && frame.payload_length < 20) ||
		    (frame.type == kGetStatisticsResponse && frame.payload_length < 16)) {
			RecordPayloadError(actual_length);
			return -EMSGSIZE;
		}

		if (frame.type == kBdaData) {
			/* TSが一件も届かない原因を調べるため、debug時だけ選局後最初のBDA_DATAを記録する。 */
			if (px4_log_enabled(PX4_LOG_DEBUG, PX4_LOG_CAT_TS) &&
			    !first_bda_data_logged_.exchange(true, std::memory_order_relaxed))
				msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_TS,
					   "Siano first BDA_DATA payload=%zu bytes\n", frame.payload_length);
			try {
				if (ts_handler_)
					ts_handler_(buffer + frame.payload_offset, frame.payload_length);
			} catch (...) {
				return -EIO;
			}
			if (frame.payload_length) {
				{
					std::lock_guard<std::mutex> lock(state_mutex_);
					first_data_received_ = true;
				}
				response_changed_.notify_all();
			}
			return 0;
		}

		std::lock_guard<std::mutex> lock(state_mutex_);
		if (frame.type == kGetVersionResponse && frame.payload_length >= 12) {
			version_.firmware_id = payload[4];
			version_.supported_protocols = payload[5];
			version_.firmware_version =
				(static_cast<std::uint16_t>(payload[10]) << 8) | payload[11];
		} else if (frame.type == kGetStatisticsExResponse && frame.payload_length >= 20) {
			/* GET_STATISTICS_EX_RES は request_result (4 byte) の後に stats_ex を返す。
			 * demod lock は stats_ex+12、SNR は dB 単位の stats_ex+20。 */
			locked_ = ReadLe32(payload + 16) != 0;
			lock_status_valid_ = true;
			if (frame.payload_length >= 28) {
				/* 内部CNRのミリdBへ変換し、BonDriver境界でdBに戻す。 */
				const auto scaled = static_cast<std::int64_t>(ReadLeS32(payload + 24)) * 1000;
				cnr_millidb_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled,
					std::numeric_limits<std::int32_t>::min(),
					std::numeric_limits<std::int32_t>::max()));
				stats_valid_ = true;
			}
		} else if (frame.type == kGetStatisticsResponse && frame.payload_length >= 16) {
			locked_ = ReadLe32(payload + 12) != 0;
			lock_status_valid_ = true;
			if (frame.payload_length >= 24) {
				const auto scaled = static_cast<std::int64_t>(ReadLeS32(payload + 20)) * 1000;
				cnr_millidb_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled,
					std::numeric_limits<std::int32_t>::min(),
					std::numeric_limits<std::int32_t>::max()));
				stats_valid_ = true;
			}
		} else if (frame.type == kSignalDetected) {
			/* 信号検出通知は復調ロックの確認ではない。統計応答を待って判定する。 */
		} else if (frame.type == kNoSignal) {
			locked_ = false;
			lock_status_valid_ = false;
			stats_valid_ = false;
		}

		if (frame.type < response_counts_.size() &&
		    response_counts_[frame.type] != std::numeric_limits<std::uint32_t>::max())
			response_counts_[frame.type]++;
		response_changed_.notify_all();
		return 0;
	}

	Protocol::Diagnostics GetDiagnostics() const noexcept
	{
		return {
			decode_emsgsize_count_.load(std::memory_order_relaxed),
			decode_ebadmsg_count_.load(std::memory_order_relaxed),
			decode_eoverflow_count_.load(std::memory_order_relaxed),
			decode_other_count_.load(std::memory_order_relaxed),
			decode_dropped_bytes_.load(std::memory_order_relaxed),
			decode_last_error_ms_.load(std::memory_order_relaxed),
			decode_emsgsize_bda_data_count_.load(std::memory_order_relaxed),
			decode_emsgsize_bda_data_bytes_.load(std::memory_order_relaxed),
			decode_last_message_type_.load(std::memory_order_relaxed),
			payload_short_count_.load(std::memory_order_relaxed),
			payload_dropped_bytes_.load(std::memory_order_relaxed),
			payload_last_error_ms_.load(std::memory_order_relaxed),
			stream_write_failure_count_.load(std::memory_order_relaxed),
			stream_dropped_bytes_.load(std::memory_order_relaxed),
			stream_last_error_ms_.load(std::memory_order_relaxed),
		};
	}

	void RecordStreamWriteFailure(std::size_t requested_bytes,
				      std::size_t written_bytes) noexcept
	{
		stream_write_failure_count_.fetch_add(1, std::memory_order_relaxed);
		if (written_bytes < requested_bytes)
			stream_dropped_bytes_.fetch_add(requested_bytes - written_bytes,
						std::memory_order_relaxed);
		stream_last_error_ms_.store(msg_now_ms(), std::memory_order_relaxed);
	}

	bool IsLocked() const noexcept
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		return locked_ && !disconnected_;
	}

	int ReadCnrMilliDb(std::int32_t &value) noexcept
	{
		Version version{};
		bool request_statistics = false;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			if (disconnected_)
				return -ENODEV;
			if (std::chrono::steady_clock::now() >= next_statistics_request_) {
				/* Linux Siano checks lock/statistics at most 10 Hz. Keep the
				 * asynchronous Host cache fresh without sending on every read. */
				next_statistics_request_ = std::chrono::steady_clock::now() +
					std::chrono::milliseconds(kTunePollIntervalMs);
				version = version_;
				request_statistics = true;
			}
		}

		if (request_statistics) {
			std::array<std::uint8_t, kHeaderSize> message{};
			const auto request = version.firmware_version >= 0x0800
				? kGetStatisticsExRequest : kGetStatisticsRequest;
			PackHeader(message.data(), request, kBdaControlMessage, kHifTask,
				   static_cast<std::uint16_t>(message.size()));
			const auto deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(kTunePollIntervalMs);
			const int ret = Send(message.data(), message.size(), deadline);
			if (ret)
				msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_DEMOD,
					   "Siano statistics refresh request failed (ret=%d).\n", ret);
		}

		std::lock_guard<std::mutex> lock(state_mutex_);
		if (disconnected_)
			return -ENODEV;
		if (!locked_)
			return -EAGAIN;
		if (!stats_valid_)
			return -ENOSYS;
		value = cnr_millidb_;
		return 0;
	}

	void Disconnect() noexcept
	{
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			disconnected_ = true;
			locked_ = false;
			lock_status_valid_ = false;
		}
		response_changed_.notify_all();
	}

private:
	void RecordDecodeError(int error, std::size_t dropped_bytes,
			       std::uint16_t message_type) noexcept
	{
		switch (error) {
		case -EMSGSIZE:
			decode_emsgsize_count_.fetch_add(1, std::memory_order_relaxed);
			break;
		case -EBADMSG:
			decode_ebadmsg_count_.fetch_add(1, std::memory_order_relaxed);
			break;
		case -EOVERFLOW:
			decode_eoverflow_count_.fetch_add(1, std::memory_order_relaxed);
			break;
		default:
			decode_other_count_.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		decode_dropped_bytes_.fetch_add(dropped_bytes, std::memory_order_relaxed);
		decode_last_error_ms_.store(msg_now_ms(), std::memory_order_relaxed);
		decode_last_message_type_.store(message_type, std::memory_order_relaxed);
		if (error == -EMSGSIZE && message_type == kBdaData) {
			decode_emsgsize_bda_data_count_.fetch_add(1, std::memory_order_relaxed);
			decode_emsgsize_bda_data_bytes_.fetch_add(dropped_bytes,
								std::memory_order_relaxed);
		}
	}

	void RecordPayloadError(std::size_t dropped_bytes) noexcept
	{
		payload_short_count_.fetch_add(1, std::memory_order_relaxed);
		payload_dropped_bytes_.fetch_add(dropped_bytes, std::memory_order_relaxed);
		payload_last_error_ms_.store(msg_now_ms(), std::memory_order_relaxed);
	}

	struct Version final {
		std::uint8_t firmware_id = 255;
		std::uint8_t supported_protocols = 0;
		std::uint16_t firmware_version = 0;
	};

	using Deadline = std::chrono::steady_clock::time_point;

	Version GetVersionSnapshot() const noexcept
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		return version_;
	}

	void ScheduleNextStatisticsRequest() noexcept
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		next_statistics_request_ = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(kTunePollIntervalMs);
	}

	int Send(const std::uint8_t *message, std::size_t length,
		 const Deadline &deadline) noexcept
	{
		const std::uint32_t timeout = RemainingMilliseconds(deadline);
		if (!timeout)
			return -ETIMEDOUT;
		if (!message || length < kHeaderSize)
			return -EINVAL;
		try {
			std::lock_guard<std::mutex> tx_lock(tx_mutex_);
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				if (disconnected_)
					return -ENODEV;
			}
			return transport_.Send(message, length, timeout);
		} catch (...) {
			return -EIO;
		}
	}

	int SendAndWait(const std::uint8_t *message, std::size_t length,
			std::uint16_t response_type, const Deadline &deadline) noexcept
	{
		if (response_type >= response_counts_.size())
			return -EINVAL;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			response_counts_[response_type] = 0;
		}
		int ret = Send(message, length, deadline);
		if (ret)
			return ret;

		std::unique_lock<std::mutex> lock(state_mutex_);
		const bool received = response_changed_.wait_until(lock, deadline, [&] {
			return response_counts_[response_type] != 0 || disconnected_;
		});
		if (response_counts_[response_type] != 0)
			return 0;
		if (disconnected_)
			return -ENODEV;
		return received ? -EIO : -ETIMEDOUT;
	}

	int GetVersion(const Deadline &deadline) noexcept
	{
		std::array<std::uint8_t, kHeaderSize> message{};
		PackHeader(message.data(), kGetVersionRequest, 0, kHifTask,
			   static_cast<std::uint16_t>(message.size()));
		return SendAndWait(message.data(), message.size(), kGetVersionResponse, deadline);
	}

	int ConfigureCatchAllPid(const Deadline &deadline) noexcept
	{
		std::lock_guard<std::mutex> pid_lock(pid_filter_mutex_);
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			if (pid_filter_configured_)
				return 0;
		}
		/* RIO firmware の catch-all PID filter は初回 tune 成功後に設定し、応答は待たない。 */
		std::array<std::uint8_t, kHeaderSize + 4> message{};
		PackHeader(message.data(), kAddPidFilterRequest, kBdaControlMessage,
			   kHifTask, static_cast<std::uint16_t>(message.size()));
		WriteLe32(message.data() + kHeaderSize, kCatchAllPid);
		const int ret = Send(message.data(), message.size(), deadline);
		if (ret)
			return ret;
		std::lock_guard<std::mutex> lock(state_mutex_);
		pid_filter_configured_ = true;
		return 0;
	}

	int LoadFirmware(const std::filesystem::path &path, std::uint8_t current_mode,
			 const Deadline &deadline)
	{
		if (path.empty())
			return -ENOENT;
		errno = 0;
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return -(errno ? errno : ENOENT);
		file.seekg(0, std::ios::end);
		const auto end = file.tellg();
		if (end < 12)
			return -EBADMSG;
		const auto file_size = static_cast<std::uintmax_t>(static_cast<std::streamoff>(end));
		if (file_size > std::numeric_limits<std::size_t>::max() ||
		    file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			return -EFBIG;
		const auto size = static_cast<std::size_t>(file_size);
		file.seekg(0, std::ios::beg);
		std::vector<std::uint8_t> file_data(size);
		if (!file.read(reinterpret_cast<char *>(file_data.data()),
			       static_cast<std::streamsize>(file_data.size())))
			return -EIO;

		FirmwareHeader header{};
		int ret = ParseFirmwareHeader(file_data.data(), file_data.size(), header);
		if (ret)
			return ret;
		if (size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) -
			   header.start_address)
			return -EFBIG;
		/* family-2 ローダーは画像末尾の12 byteも転送するため、未定義領域を0で埋める。 */
		std::vector<std::uint8_t> payload(size, 0);
		std::copy_n(file_data.data() + 12, header.length, payload.data());
		std::uint32_t address = header.start_address;

		if (current_mode != kFirmwareModeNone) {
			std::array<std::uint8_t, kHeaderSize> message{};
			PackHeader(message.data(), kReloadStartRequest, 0, kHifTask,
				   static_cast<std::uint16_t>(message.size()));
			ret = SendAndWait(message.data(), message.size(), kReloadStartResponse, deadline);
			if (ret)
				return ret;
			if (header.length < 24)
				return -EBADMSG;
			address = ReadLe32(payload.data() + 20);
		}

		for (std::size_t offset = 0; offset < size;) {
			const std::size_t chunk = (std::min)(kMaxPayloadSize, size - offset);
			std::array<std::uint8_t, kHeaderSize + 4 + kMaxPayloadSize> message{};
			const auto message_length = kHeaderSize + 4 + chunk;
			PackHeader(message.data(), kDataDownloadRequest, 0, kHifTask,
				   static_cast<std::uint16_t>(message_length));
			WriteLe32(message.data() + kHeaderSize, address);
			std::copy_n(payload.data() + offset, chunk, message.data() + kHeaderSize + 4);
			ret = SendAndWait(message.data(), message_length, kDataDownloadResponse, deadline);
			if (ret)
				return ret;
			offset += chunk;
			address += static_cast<std::uint32_t>(chunk);
		}

		{
			std::array<std::uint8_t, kHeaderSize + 12> message{};
			PackHeader(message.data(), kDataValidityRequest, 0, kHifTask,
				   static_cast<std::uint16_t>(message.size()));
			WriteLe32(message.data() + kHeaderSize, header.start_address);
			WriteLe32(message.data() + kHeaderSize + 4, header.length);
			WriteLe32(message.data() + kHeaderSize + 8, 0);
			ret = SendAndWait(message.data(), message.size(), kDataValidityResponse, deadline);
			if (ret)
				return ret;
		}

		if (current_mode == kFirmwareModeNone) {
			std::array<std::uint8_t, kHeaderSize + 20> message{};
			PackHeader(message.data(), kSoftwareDownloadTriggerRequest, 0, kHifTask,
				   static_cast<std::uint16_t>(message.size()));
			WriteLe32(message.data() + kHeaderSize, header.start_address);
			WriteLe32(message.data() + kHeaderSize + 4, 6);
			WriteLe32(message.data() + kHeaderSize + 8, 0x200);
			WriteLe32(message.data() + kHeaderSize + 12, 0);
			WriteLe32(message.data() + kHeaderSize + 16, 4);
			ret = SendAndWait(message.data(), message.size(),
					  kSoftwareDownloadTriggerResponse, deadline);
		} else {
			std::array<std::uint8_t, kHeaderSize> message{};
			PackHeader(message.data(), kReloadExecuteRequest, 0, kHifTask,
				   static_cast<std::uint16_t>(message.size()));
			ret = Send(message.data(), message.size(), deadline);
		}
		if (ret)
			return ret;
		const auto wake_deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(kFirmwareStartWaitMs);
		if (wake_deadline > deadline)
			return -ETIMEDOUT;
		std::this_thread::sleep_until(wake_deadline);
		return 0;
	}

	Transport &transport_;
	const std::size_t response_alignment_;
	TsHandler ts_handler_;
	mutable std::mutex state_mutex_;
	std::mutex tx_mutex_;
	std::mutex pid_filter_mutex_;
	std::condition_variable response_changed_;
	std::array<std::uint32_t, 1024> response_counts_{};
	std::atomic<bool> first_bda_data_logged_{false};
	std::atomic_uint64_t decode_emsgsize_count_{0};
	std::atomic_uint64_t decode_ebadmsg_count_{0};
	std::atomic_uint64_t decode_eoverflow_count_{0};
	std::atomic_uint64_t decode_other_count_{0};
	std::atomic_uint64_t decode_dropped_bytes_{0};
	std::atomic_uint64_t decode_last_error_ms_{0};
	std::atomic_uint64_t decode_emsgsize_bda_data_count_{0};
	std::atomic_uint64_t decode_emsgsize_bda_data_bytes_{0};
	std::atomic_uint16_t decode_last_message_type_{0};
	std::atomic_uint64_t payload_short_count_{0};
	std::atomic_uint64_t payload_dropped_bytes_{0};
	std::atomic_uint64_t payload_last_error_ms_{0};
	std::atomic_uint64_t stream_write_failure_count_{0};
	std::atomic_uint64_t stream_dropped_bytes_{0};
	std::atomic_uint64_t stream_last_error_ms_{0};
	Version version_{};
	std::int32_t cnr_millidb_ = 0;
	std::chrono::steady_clock::time_point next_statistics_request_{};
	bool locked_ = false;
	bool lock_status_valid_ = false;
	bool first_data_received_ = false;
	bool stats_valid_ = false;
	bool pid_filter_configured_ = false;
	bool initialized_ = false;
	bool disconnected_ = false;
};

Protocol::Protocol(Transport &transport, std::size_t response_alignment,
		   TsHandler ts_handler)
	: impl_(new Impl(transport, response_alignment, std::move(ts_handler)))
{
}

Protocol::~Protocol()
{
	delete impl_;
}

int Protocol::Initialize(const std::filesystem::path &firmware_path,
			 std::chrono::milliseconds timeout) noexcept
{
	return impl_->Initialize(firmware_path, timeout);
}

int Protocol::TuneFrequencyHz(std::uint32_t frequency_hz,
			      std::chrono::milliseconds timeout) noexcept
{
	return impl_->TuneFrequencyHz(frequency_hz, timeout);
}

int Protocol::HandleTransfer(const std::uint8_t *buffer,
			     std::size_t actual_length) noexcept
{
	return impl_->HandleTransfer(buffer, actual_length);
}

Protocol::Diagnostics Protocol::GetDiagnostics() const noexcept
{
	return impl_->GetDiagnostics();
}

void Protocol::RecordStreamWriteFailure(std::size_t requested_bytes,
					std::size_t written_bytes) noexcept
{
	impl_->RecordStreamWriteFailure(requested_bytes, written_bytes);
}

bool Protocol::IsLocked() const noexcept
{
	return impl_->IsLocked();
}

int Protocol::ReadCnrMilliDb(std::int32_t &value) noexcept
{
	return impl_->ReadCnrMilliDb(value);
}

void Protocol::Disconnect() noexcept
{
	impl_->Disconnect();
}

} // namespace px4::siano
