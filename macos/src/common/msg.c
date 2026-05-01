// msg.c - POSIX logging implementation (replaces Windows msg.c)

#include "msg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static enum msg_mode mode = MSG_MODE_NONE;
static FILE *log_file = NULL;

void msg_set_mode(enum msg_mode new_mode)
{
	mode = new_mode;
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
