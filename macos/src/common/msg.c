// msg.c - POSIX logging implementation (replaces Windows msg.c)

#include "msg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static enum msg_mode mode = MSG_MODE_NONE;
static FILE *log_file = NULL;
/* 起動からの経過時間の基準。最初の msg_set_mode() で決め、以後は変えない。 */
static uint64_t epoch_ms = 0;
static int epoch_set = 0;

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void msg_set_mode(enum msg_mode new_mode)
{
	if (!epoch_set) {
		epoch_ms = monotonic_ms();
		epoch_set = 1;
	}
	mode = new_mode;
}

uint64_t msg_now_ms(void)
{
	return monotonic_ms() - epoch_ms;
}

bool msg_open_file(const wchar_t *path)
{
	if (log_file)
		fclose(log_file);

	/* Convert wchar_t path to narrow string (ASCII paths only) */
	char buf[1024];
	int i;
	for (i = 0; i < 1023 && path[i]; i++)
		buf[i] = (char)path[i];
	buf[i] = '\0';

	log_file = fopen(buf, "a");
	return log_file != NULL;
}

void msg_close_file(void)
{
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}

int msg_printf(const char *format, ...)
{
	va_list args;
	char buf[1024];
	int c;

	if (mode == MSG_MODE_NONE)
		return 0;

	va_start(args, format);
	c = vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	if (c < 0) return 0;

	if (mode & MSG_MODE_CONSOLE)
		fputs(buf, stderr);

	if ((mode & MSG_MODE_LOG_FILE) && log_file)
		fputs(buf, log_file);

	return c;
}
