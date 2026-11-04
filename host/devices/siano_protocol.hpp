#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace px4::siano {

class Transport {
public:
	virtual ~Transport() = default;
	virtual int Send(const std::uint8_t *message, std::size_t length,
			 std::uint32_t timeout_ms) noexcept = 0;
};

class Protocol final {
public:
	using TsHandler = std::function<void(const std::uint8_t *, std::size_t)>;

	struct Diagnostics final {
		std::uint64_t decode_emsgsize_count = 0;
		std::uint64_t decode_ebadmsg_count = 0;
		std::uint64_t decode_eoverflow_count = 0;
		std::uint64_t decode_other_count = 0;
		std::uint64_t decode_dropped_bytes = 0;
		std::uint64_t decode_last_error_ms = 0;
		std::uint64_t decode_emsgsize_bda_data_count = 0;
		std::uint64_t decode_emsgsize_bda_data_bytes = 0;
		std::uint16_t decode_last_message_type = 0;
		std::uint64_t payload_short_count = 0;
		std::uint64_t payload_dropped_bytes = 0;
		std::uint64_t payload_last_error_ms = 0;
		std::uint64_t stream_write_failure_count = 0;
		std::uint64_t stream_dropped_bytes = 0;
		std::uint64_t stream_last_error_ms = 0;
	};

	Protocol(Transport &transport, std::size_t response_alignment,
		 TsHandler ts_handler);
	~Protocol();
	Protocol(const Protocol &) = delete;
	Protocol &operator=(const Protocol &) = delete;

	int Initialize(const std::filesystem::path &firmware_path,
		       std::chrono::milliseconds timeout) noexcept;
	int TuneFrequencyHz(std::uint32_t frequency_hz,
			    std::chrono::milliseconds timeout) noexcept;
	int HandleTransfer(const std::uint8_t *buffer,
			   std::size_t actual_length) noexcept;
	Diagnostics GetDiagnostics() const noexcept;
	void RecordStreamWriteFailure(std::size_t requested_bytes,
				      std::size_t written_bytes) noexcept;
	bool IsLocked() const noexcept;
	int ReadCnrMilliDb(std::int32_t &value) noexcept;
	void Disconnect() noexcept;

private:
	class Impl;
	Impl *impl_;
};

} // namespace px4::siano
