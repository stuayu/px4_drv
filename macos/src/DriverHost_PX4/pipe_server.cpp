// pipe_server.cpp - UNIX-domain-socket server (replaces Windows named-pipe version)

#include "pipe_server.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include "misc_posix.h"   /* for _px4_event_fd */

namespace px4 {

// ---------------------------------------------------------------------------
// Per-name listening socket cache
// Multiple PipeServer::Accept() calls reuse the same server fd.
// ---------------------------------------------------------------------------

static std::mutex         g_srv_mtx;
static std::unordered_map<std::string, int> g_srv_fds;

static int get_or_create_server_sock(const std::string &path)
{
	std::lock_guard<std::mutex> lock(g_srv_mtx);

	auto it = g_srv_fds.find(path);
	if (it != g_srv_fds.end())
		return it->second;

	int srv_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv_fd < 0) return -1;

	::unlink(path.c_str()); /* remove stale socket file if any */

	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

	if (::bind(srv_fd, reinterpret_cast<struct sockaddr *>(&addr),
		   sizeof(addr)) < 0) {
		::close(srv_fd);
		return -1;
	}

	if (::listen(srv_fd, 16) < 0) {
		::close(srv_fd);
		return -1;
	}

	g_srv_fds[path] = srv_fd;
	return srv_fd;
}

// Call from main on exit to clean up socket files
void pipe_server_cleanup()
{
	std::lock_guard<std::mutex> lock(g_srv_mtx);
	for (auto &kv : g_srv_fds) {
		::close(kv.second);
		::unlink(kv.first.c_str());
	}
	g_srv_fds.clear();
}

// ---------------------------------------------------------------------------
// PipeServer
// ---------------------------------------------------------------------------

PipeServer::PipeServer() noexcept
	: Pipe()
{
}

PipeServer::~PipeServer()
{
	if (!IsConnected())
		return;

	/* Graceful shutdown before base Pipe::~Pipe() calls close() */
	int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
	::shutdown(fd, SHUT_RDWR);
}

bool PipeServer::Accept(const std::wstring &name,
			const PipeServerConfig & /*config*/,
			HANDLE ready_event,
			HANDLE cancel_event,
			DWORD  /*timeout*/) noexcept
{
	if (IsConnected())
		return false;

	/* Convert wstring name to socket path /tmp/<name>.sock */
	std::string narrow;
	for (wchar_t wc : name) {
		if (wc < 128) narrow += static_cast<char>(wc);
	}
	std::string sock_path = "/tmp/" + narrow + ".sock";

	int srv_fd = get_or_create_server_sock(sock_path);
	if (srv_fd < 0) {
		error_.assign(errno, std::generic_category());
		return false;
	}

	/* Signal that we are now listening and ready to accept */
	if (ready_event)
		SetEvent(ready_event);

	int cancel_fd = _px4_event_fd(cancel_event);

	for (;;) {
		struct pollfd pfds[2];
		pfds[0].fd      = srv_fd;
		pfds[0].events  = POLLIN;
		pfds[0].revents = 0;
		int nfds = 1;

		if (cancel_fd >= 0) {
			pfds[1].fd      = cancel_fd;
			pfds[1].events  = POLLIN;
			pfds[1].revents = 0;
			nfds = 2;
		}

		int r = ::poll(pfds, nfds, -1);
		if (r < 0) {
			if (errno == EINTR) continue;
			error_.assign(errno, std::generic_category());
			return false;
		}

		if (nfds == 2 && (pfds[1].revents & POLLIN)) {
			error_.assign(ECANCELED, std::generic_category());
			return false;
		}

		if (!(pfds[0].revents & POLLIN)) continue;

		int client_fd = ::accept(srv_fd, nullptr, nullptr);
		if (client_fd < 0) {
			if (errno == EINTR) continue;
			error_.assign(errno, std::generic_category());
			return false;
		}

		SetHandle(reinterpret_cast<HANDLE>(
			static_cast<intptr_t>(client_fd)));
		return true;
	}
}

} // namespace px4
