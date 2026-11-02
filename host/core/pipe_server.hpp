// pipe_server.hpp

#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

#include "pipe.hpp"

namespace px4 {

class PipeServer final : public px4::Pipe {
public:
	struct PipeServerConfig {
		bool stream_pipe;
		bool stream_read;
		std::size_t out_buffer_size;
		std::size_t in_buffer_size;
		std::uint32_t default_timeout;
	};

	PipeServer() noexcept;
	~PipeServer();

	bool Accept(const std::wstring &name, const PipeServerConfig &config, HANDLE ready_event, HANDLE cancel_event, DWORD timeout = INFINITE) noexcept;
	/* 接続単位の I/O を終え、相手へ切断を通知する。 */
	void Shutdown() noexcept;

private:
	bool shutdown_ = false;
};

} // namespace px4
