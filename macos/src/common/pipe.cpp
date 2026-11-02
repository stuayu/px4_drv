// pipe.cpp - POSIX UNIX-domain-socket implementation of px4::Pipe
// (replaces Windows overlapped-I/O version)

#include "pipe.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>

#include <sys/socket.h>
#include <poll.h>

#include "misc_posix.h"   /* for _px4_event_fd */

namespace px4 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/*
 * データ接続は同じ Pipe の受信を接続スレッド、送信を配信スレッドから行うため、
 * 切断時は両方が同時に失敗を記録する。記録だけを直列化し、GetError() は失敗した
 * 呼び出しと同じスレッドから読む (現在の読み手は Accept() の失敗を扱う1か所だけ)。
 */
static std::mutex g_error_mtx;

static void set_error(std::error_condition &error, int value)
{
	std::lock_guard<std::mutex> lock(g_error_mtx);
	error.assign(value, std::generic_category());
}

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
		set_error(error_, EBADF);
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
			set_error(error_, errno);
			return false;
		}

		if (nfds == 2 && (pfds[1].revents & POLLIN)) {
			set_error(error_, ECANCELED);
			return false;
		}

		if (pfds[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = ::recv(fd, buf, size, 0);
			if (n < 0) {
				if (errno == EINTR) continue;
				set_error(error_, errno);
				return false;
			}
			if (n == 0) {
				set_error(error_, ECONNRESET);
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
		set_error(error_, EBADF);
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
			set_error(error_, errno);
			return false;
		}

		if (nfds == 2 && (pfds[1].revents & POLLIN)) {
			set_error(error_, ECANCELED);
			return false;
		}

		/*
		 * macOS の UNIX ソケットは、相手が閉じると POLLOUT を返さず POLLHUP だけを返す。
		 * POLLOUT だけを待つと poll() が即座に戻り続けて送信へ進まないため、異常を示す
		 * 事象でも送信を試み、カーネルが返す EPIPE などの失敗をそのまま返す。
		 */
		if (pfds[0].revents & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) {
			ssize_t n = ::send(fd, p, left, MSG_NOSIGNAL);
			if (n < 0) {
				if (errno == EINTR) continue;
				set_error(error_, errno);
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
