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
	bool IsLocked() const noexcept;
	int ReadCnrMilliDb(std::int32_t &value) noexcept;
	void Disconnect() noexcept;

private:
	class Impl;
	Impl *impl_;
};

} // namespace px4::siano
