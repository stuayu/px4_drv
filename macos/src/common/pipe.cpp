// pipe.cpp - POSIX UNIX-domain-socket implementation of px4::Pipe
// (replaces Windows overlapped-I/O version)

#include "pipe.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <poll.h>

#include "misc_posix.h"   /* for _px4_event_fd */

namespace px4 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline int get_fd(HANDLE h)
{
	return static_cast<int>(reinterpret_cast<intptr_t>(h));
}

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

Pipe::Pipe(HANDLE handle) noexcept
	: handle_(handle),
	  ol_event_{}
{
}

Pipe::~Pipe()
{
	if (!IsConnected())
		return;

	::close(get_fd(handle_));
	handle_ = INVALID_HANDLE_VALUE;
	/* ol_event_[0/1] are always null on macOS – nothing to close */
}

// ---------------------------------------------------------------------------
// Read (without / with cancel)
// ---------------------------------------------------------------------------

bool Pipe::Read(void *buf, std::size_t size, std::size_t &return_size) noexcept
{
	return Read(buf, size, return_size, nullptr);
}

bool Pipe::Read(void *buf, std::size_t size, std::size_t &return_size,
		HANDLE cancel_event) noexcept
{
	if (!IsConnected()) {
		error_.assign(EBADF, std::generic_category());
		return false;
	}

	int fd         = get_fd(handle_);
	int cancel_fd  = _px4_event_fd(cancel_event);

	for (;;) {
		struct pollfd pfds[2];
		pfds[0].fd      = fd;
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

		if (pfds[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = ::recv(fd, buf, size, 0);
			if (n < 0) {
				if (errno == EINTR) continue;
				error_.assign(errno, std::generic_category());
				return false;
			}
			if (n == 0) {
				error_.assign(ECONNRESET, std::generic_category());
				return false;
			}
			return_size = static_cast<std::size_t>(n);
			return true;
		}
	}
}

// ---------------------------------------------------------------------------
// Write (without / with cancel)
// ---------------------------------------------------------------------------

bool Pipe::Write(const void *buf, std::size_t size,
		 std::size_t &return_size) noexcept
{
	return Write(buf, size, return_size, nullptr);
}

bool Pipe::Write(const void *buf, std::size_t size, std::size_t &return_size,
		 HANDLE cancel_event) noexcept
{
	if (!IsConnected()) {
		error_.assign(EBADF, std::generic_category());
		return false;
	}

	int fd        = get_fd(handle_);
	int cancel_fd = _px4_event_fd(cancel_event);

	const char *p     = static_cast<const char *>(buf);
	std::size_t left  = size;
	return_size       = 0;

	while (left > 0) {
		struct pollfd pfds[2];
		pfds[0].fd      = fd;
		pfds[0].events  = POLLOUT;
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

		if (pfds[0].revents & POLLOUT) {
			ssize_t n = ::send(fd, p, left, MSG_NOSIGNAL);
			if (n < 0) {
				if (errno == EINTR) continue;
				error_.assign(errno, std::generic_category());
				return false;
			}
			p           += n;
			left        -= static_cast<std::size_t>(n);
			return_size += static_cast<std::size_t>(n);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Call helpers
// ---------------------------------------------------------------------------

bool Pipe::Call(void *buf, std::size_t size) noexcept
{
	return Call(buf, size, buf, size);
}

bool Pipe::Call(const void *buf_in, std::size_t size_in,
		void *buf_out, std::size_t size_out) noexcept
{
	std::size_t ret_size;

	if (!Write(buf_in, size_in, ret_size) || ret_size != size_in)
		return false;

	if (!Read(buf_out, size_out, ret_size) || ret_size != size_out)
		return false;

	return true;
}

} // namespace px4
