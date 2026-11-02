// SPDX-License-Identifier: GPL-2.0-or-later
// Siano SMS framing and RIO ISDB-T control follow drivers/media/common/siano
// in Linux and the GPL-2.0-or-later Khronos31/siano-userland implementation.

#include "siano_protocol.hpp"

#include "siano_wire.hpp"

#include <algorithm>
#include <array>
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
			if (ret)
				return ret;

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
				stats_valid_ = false;
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
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				/* 信号ロック通知が TUNE 応答より先に届く場合があるため、応答後に状態を切り直す。 */
				locked_ = false;
				stats_valid_ = false;
			}

			for (;;) {
				bool locked = false;
				bool stats_valid = false;
				{
					std::unique_lock<std::mutex> lock(state_mutex_);
					if (disconnected_)
						return -ENODEV;
					locked = locked_;
					stats_valid = stats_valid_;
					if (!locked) {
						const auto remaining = deadline - std::chrono::steady_clock::now();
						if (remaining <= std::chrono::steady_clock::duration::zero())
							return -ETIMEDOUT;
						response_changed_.wait_until(
							lock,
							(std::min)(deadline, std::chrono::steady_clock::now() +
								std::chrono::milliseconds(kTunePollIntervalMs)),
							[this] { return disconnected_ || locked_; });
						if (disconnected_)
							return -ENODEV;
						locked = locked_;
						stats_valid = stats_valid_;
					}
				}
				if (locked) {
					if (!stats_valid) {
						std::array<std::uint8_t, kHeaderSize> stats{};
						const Version version = GetVersionSnapshot();
						const auto request = version.firmware_version >= 0x0800
							? kGetStatisticsExRequest : kGetStatisticsRequest;
						PackHeader(stats.data(), request, kBdaControlMessage, kHifTask,
							   static_cast<std::uint16_t>(stats.size()));
						(void)Send(stats.data(), stats.size(), deadline);
					}
					return ConfigureCatchAllPid(deadline);
				}

				std::array<std::uint8_t, kHeaderSize> stats{};
				const Version version = GetVersionSnapshot();
				const auto request = version.firmware_version >= 0x0800
					? kGetStatisticsExRequest : kGetStatisticsRequest;
				PackHeader(stats.data(), request, kBdaControlMessage, kHifTask,
					   static_cast<std::uint16_t>(stats.size()));
				ret = Send(stats.data(), stats.size(), deadline);
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
		if (ret)
			return ret;
		/* Mode/lock を判定する必須フィールドが欠けた応答は、成功扱いにしない。 */
		if ((frame.type == kGetVersionResponse && frame.payload_length < 12) ||
		    (frame.type == kGetStatisticsExResponse && frame.payload_length < 20) ||
		    (frame.type == kGetStatisticsResponse && frame.payload_length < 16))
			return -EMSGSIZE;

		if (frame.type == kBdaData) {
			try {
				if (ts_handler_)
					ts_handler_(buffer + frame.payload_offset, frame.payload_length);
				return 0;
			} catch (...) {
				return -EIO;
			}
		}

		std::lock_guard<std::mutex> lock(state_mutex_);
		const std::uint8_t *payload = buffer + frame.payload_offset;
		if (frame.type == kGetVersionResponse && frame.payload_length >= 12) {
			version_.firmware_id = payload[4];
			version_.supported_protocols = payload[5];
			version_.firmware_version =
				(static_cast<std::uint16_t>(payload[10]) << 8) | payload[11];
		} else if (frame.type == kGetStatisticsExResponse && frame.payload_length >= 20) {
			locked_ = ReadLe32(payload + 16) != 0;
			if (frame.payload_length >= 28) {
				/* Linux Siano DVB API は SNR を 0.001 dB 単位で公開する。 */
				const auto scaled = static_cast<std::int64_t>(ReadLeS32(payload + 24)) * 1000;
				cnr_millidb_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled,
					std::numeric_limits<std::int32_t>::min(),
					std::numeric_limits<std::int32_t>::max()));
				stats_valid_ = true;
			}
		} else if (frame.type == kGetStatisticsResponse && frame.payload_length >= 16) {
			locked_ = ReadLe32(payload + 12) != 0;
			if (frame.payload_length >= 24) {
				const auto scaled = static_cast<std::int64_t>(ReadLeS32(payload + 20)) * 1000;
				cnr_millidb_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled,
					std::numeric_limits<std::int32_t>::min(),
					std::numeric_limits<std::int32_t>::max()));
				stats_valid_ = true;
			}
		} else if (frame.type == kSignalDetected) {
			locked_ = true;
		} else if (frame.type == kNoSignal) {
			locked_ = false;
		}

		if (frame.type < response_counts_.size() &&
		    response_counts_[frame.type] != std::numeric_limits<std::uint32_t>::max())
			response_counts_[frame.type]++;
		response_changed_.notify_all();
		return 0;
	}

	bool IsLocked() const noexcept
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		return locked_ && !disconnected_;
	}

	int ReadCnrMilliDb(std::int32_t &value) const noexcept
	{
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
		}
		response_changed_.notify_all();
	}

private:
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
	Version version_{};
	std::int32_t cnr_millidb_ = 0;
	bool locked_ = false;
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

bool Protocol::IsLocked() const noexcept
{
	return impl_->IsLocked();
}

int Protocol::ReadCnrMilliDb(std::int32_t &value) const noexcept
{
	return impl_->ReadCnrMilliDb(value);
}

void Protocol::Disconnect() noexcept
{
	impl_->Disconnect();
}

} // namespace px4::siano
