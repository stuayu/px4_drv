// misc_posix.h - POSIX/macOS compatibility layer replacing misc_win.h

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sched.h>

// ---------------------------------------------------------------------------
// Linux kernel build-system macro stub (used by driver/print_format.h)
// ---------------------------------------------------------------------------
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "px4_drv"
#endif

// ---------------------------------------------------------------------------
// Primitive types (same layout as misc_win.h)
// ---------------------------------------------------------------------------
typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define msleep(ms)  usleep((unsigned int)((ms) * 1000))
#define mdelay(ms)  /* do nothing */

// ---------------------------------------------------------------------------
// GFP flags (unused on user-space builds)
// ---------------------------------------------------------------------------
#define GFP_KERNEL  0
#define GFP_ATOMIC  0

// ---------------------------------------------------------------------------
// Kernel-style memory allocation wrappers
// ---------------------------------------------------------------------------
static inline void *kmalloc(size_t size, int dummy)
{
	(void)dummy;
	return malloc(size);
}
static inline void *kcalloc(size_t n, size_t size, int dummy)
{
	(void)dummy;
	return calloc(n, size);
}
static inline void kfree(void *p) { free(p); }

static inline void *kzalloc(size_t size, int dummy)
{
	(void)dummy;
	void *p = malloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

// ---------------------------------------------------------------------------
// VirtualAlloc/VirtualFree replacements (used by ringbuffer.cpp)
// ---------------------------------------------------------------------------
static inline void *_px4_valloc(size_t size)
{
	return malloc(size);
}
static inline void _px4_vfree(void *p)
{
	free(p);
}
#define VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) \
	_px4_valloc((size_t)(dwSize))
#define VirtualFree(lpAddress, dwSize, dwFreeType) \
	_px4_vfree(lpAddress)
#define MEM_COMMIT   0
#define MEM_RESERVE  0
#define MEM_RELEASE  0
#define PAGE_READWRITE 0

// ---------------------------------------------------------------------------
// Windows HANDLE / event primitives (minimal stubs for portable files)
// ---------------------------------------------------------------------------
typedef void *HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

// ---------------------------------------------------------------------------
// Thread priority
// ---------------------------------------------------------------------------
static inline void _px4_set_realtime_priority(void)
{
	struct sched_param sp;
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}
#define SetThreadPriority(thread, priority)  _px4_set_realtime_priority()
#define GetCurrentThread()                   pthread_self()
#define THREAD_PRIORITY_TIME_CRITICAL        15

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------
static inline void Sleep(unsigned int ms) { usleep(ms * 1000u); }

// ---------------------------------------------------------------------------
// Mutex (maps Linux kernel mutex semantics to POSIX)
// ---------------------------------------------------------------------------
struct mutex {
	pthread_mutex_t m;
};

static inline void mutex_init(struct mutex *lock)
{
	pthread_mutex_init(&lock->m, NULL);
}
static inline void mutex_destroy(struct mutex *lock)
{
	pthread_mutex_destroy(&lock->m);
}
static inline void mutex_lock(struct mutex *lock)
{
	pthread_mutex_lock(&lock->m);
}
static inline void mutex_unlock(struct mutex *lock)
{
	pthread_mutex_unlock(&lock->m);
}

// ---------------------------------------------------------------------------
// Device descriptor (used for logging context)
// ---------------------------------------------------------------------------
struct device {
	char driver_name[64];
	char device_name[64];
};

// ---------------------------------------------------------------------------
// Logging macros (use msg_printf from msg.h)
// ---------------------------------------------------------------------------
#include "msg.h"

#define printk(format, ...)              msg_printf(format, ##__VA_ARGS__)
#define dev_print(level, dev, fmt, ...)  msg_printf("[" level "] %s %s: " fmt, \
	(dev)->driver_name, (dev)->device_name, ##__VA_ARGS__)
#define dev_err(dev,  fmt, ...)          dev_print("ERR",  dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)          dev_print("WARN", dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)          dev_print("INFO", dev, fmt, ##__VA_ARGS__)
#if defined(_DEBUG) || defined(_DEBUG_MSG)
#define dev_dbg(dev, fmt, ...)           dev_print("DBG",  dev, fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...)
#endif

// ---------------------------------------------------------------------------
// Atomic type (simple volatile int; sufficient for user-space)
// ---------------------------------------------------------------------------
typedef volatile int atomic_t;

static inline void atomic_set(atomic_t *p, int v)
{
	__sync_synchronize();
	*p = v;
	__sync_synchronize();
}
static inline int atomic_read(const atomic_t *p)
{
	__sync_synchronize();
	int v = *p;
	__sync_synchronize();
	return v;
}
#define atomic_read_acquire(p)  atomic_read(p)
#define atomic_xchg(p, v)       __sync_lock_test_and_set(p, v)

// ---------------------------------------------------------------------------
// Firmware structure (populated by misc_posix.c)
// ---------------------------------------------------------------------------
struct firmware {
	size_t size;
	const u8 *data;
};

#ifdef __cplusplus
extern "C" {
#endif
int  request_firmware(const struct firmware **fw, const char *name, struct device *dev);
void release_firmware(const struct firmware *fw);
#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// Additional includes for HANDLE event support
// ---------------------------------------------------------------------------
#include <poll.h>
#include <fcntl.h>
#include <limits.h>

// ---------------------------------------------------------------------------
// Windows DWORD / ULONG / WORD / BYTE typedefs
// ---------------------------------------------------------------------------
#ifndef DWORD
typedef unsigned int   DWORD;
typedef unsigned long  ULONG;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
#endif

// ---------------------------------------------------------------------------
// HANDLE-based event primitives (implemented as POSIX self-pipe pairs)
// Each "event HANDLE" is a heap-allocated struct _px4_event *.
// ---------------------------------------------------------------------------
struct _px4_event {
	int read_fd;
	int write_fd;
};

static inline HANDLE CreateEventW(void *attr, int manual_reset, int initial_state,
				    const wchar_t *name)
{
	(void)attr; (void)manual_reset; (void)name;
	struct _px4_event *ev = (struct _px4_event *)malloc(sizeof(*ev));
	if (!ev) return INVALID_HANDLE_VALUE;
	int fds[2];
	if (pipe(fds) < 0) { free(ev); return INVALID_HANDLE_VALUE; }
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	ev->read_fd  = fds[0];
	ev->write_fd = fds[1];
	if (initial_state) { char b = 1; (void)write(ev->write_fd, &b, 1); }
	return (HANDLE)ev;
}

static inline int SetEvent(HANDLE h)
{
	if (!h || h == INVALID_HANDLE_VALUE) return 0;
	struct _px4_event *ev = (struct _px4_event *)h;
	char b = 1;
	return (int)(write(ev->write_fd, &b, 1) == 1);
}

static inline int ResetEvent(HANDLE h)
{
	if (!h || h == INVALID_HANDLE_VALUE) return 0;
	struct _px4_event *ev = (struct _px4_event *)h;
	char b;
	while (read(ev->read_fd, &b, 1) == 1); /* drain */
	return 1;
}

static inline int CloseHandle(HANDLE h)
{
	if (!h || h == INVALID_HANDLE_VALUE) return 0;
	struct _px4_event *ev = (struct _px4_event *)h;
	close(ev->read_fd);
	close(ev->write_fd);
	free(ev);
	return 1;
}

#ifndef INFINITE
#define INFINITE        0xFFFFFFFFU
#define WAIT_OBJECT_0   0U
#define WAIT_TIMEOUT    258U
#define WAIT_FAILED     0xFFFFFFFFU
#endif

static inline unsigned int WaitForSingleObject(HANDLE h, unsigned int ms)
{
	if (!h || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
	struct _px4_event *ev = (struct _px4_event *)h;
	struct pollfd pfd;
	pfd.fd      = ev->read_fd;
	pfd.events  = POLLIN;
	pfd.revents = 0;
	int r = poll(&pfd, 1, (ms == INFINITE) ? -1 : (int)ms);
	if (r > 0 && (pfd.revents & POLLIN)) return WAIT_OBJECT_0;
	if (r == 0) return WAIT_TIMEOUT;
	return WAIT_FAILED;
}

static inline unsigned int WaitForMultipleObjects(unsigned int n, const HANDLE *handles,
						   int wait_all, unsigned int ms)
{
	(void)wait_all;
	if (!n || n > 64) return WAIT_FAILED;
	struct pollfd pfds[64];
	for (unsigned int i = 0; i < n; i++) {
		HANDLE h = handles[i];
		pfds[i].fd     = (h && h != INVALID_HANDLE_VALUE)
				 ? ((struct _px4_event *)h)->read_fd : -1;
		pfds[i].events  = POLLIN;
		pfds[i].revents = 0;
	}
	int r = poll(pfds, (nfds_t)n, (ms == INFINITE) ? -1 : (int)ms);
	if (r > 0)
		for (unsigned int i = 0; i < n; i++)
			if (pfds[i].revents & POLLIN) return WAIT_OBJECT_0 + i;
	if (r == 0) return WAIT_TIMEOUT;
	return WAIT_FAILED;
}

/* Extract the read fd from a HANDLE event (for use in custom poll loops). */
static inline int _px4_event_fd(HANDLE h)
{
	if (!h || h == INVALID_HANDLE_VALUE) return -1;
	return ((struct _px4_event *)h)->read_fd;
}

// ---------------------------------------------------------------------------
// Boolean constants
// ---------------------------------------------------------------------------
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

// ---------------------------------------------------------------------------
// Minimal Windows API stubs used by portable source files
// ---------------------------------------------------------------------------
static inline unsigned int GetLastError(void) { return (unsigned int)errno; }
#define ERROR_ALREADY_EXISTS  EEXIST

#define MB_OK        0U
#define MB_ICONERROR 0x10U
static inline int MessageBoxA(void *hwnd, const char *text, const char *caption,
			       unsigned int type)
{
	(void)hwnd; (void)type;
	fprintf(stderr, "%s: %s\n", caption ? caption : "Error", text ? text : "");
	return 0;
}

static inline void *GetModuleHandleW(const wchar_t *name) { (void)name; return NULL; }

static inline int SetCurrentDirectoryW(const wchar_t *path)
{
	char buf[1024];
	int i;
	for (i = 0; i < 1023 && path[i]; i++) buf[i] = (char)path[i];
	buf[i] = '\0';
	return chdir(buf) == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// MSVC safe string function compatibility (C++ only)
// ---------------------------------------------------------------------------
#ifdef __cplusplus
#include <cwchar>
#include <cstdarg>

template<std::size_t N, typename... Args>
inline int swprintf_s(wchar_t (&buf)[N], const wchar_t *fmt, Args... args)
{
	return swprintf(buf, N, fmt, args...);
}

template<std::size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t *src)
{
	wcsncpy(dst, src, N - 1);
	dst[N - 1] = L'\0';
	return 0;
}

template<std::size_t N>
inline int wcscat_s(wchar_t (&dst)[N], const wchar_t *src)
{
	wcscat(dst, src);
	return 0;
}

template<std::size_t N>
inline int strncpy_s(char (&dst)[N], const char *src, std::size_t count)
{
	std::size_t n = count < N - 1 ? count : N - 1;
	strncpy(dst, src, n);
	dst[n] = '\0';
	return 0;
}

template<std::size_t N, typename... Args>
inline int sprintf_s(char (&buf)[N], const char *fmt, Args... args)
{
	return snprintf(buf, N, fmt, args...);
}
#endif /* __cplusplus */
