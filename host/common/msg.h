// msg.h

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wctype.h>

#include "px4_log.h"
#include "msg_log.h"

enum msg_mode {
	MSG_MODE_NONE = 0x0000,
	MSG_MODE_CONSOLE = 0x0001,
	MSG_MODE_DEBUGGER = 0x0002,
	MSG_MODE_LOG_FILE = 0x0004
};

#ifdef __cplusplus
extern "C" {
#endif
void msg_set_mode(enum msg_mode new_mode);
bool msg_open_file(const wchar_t *path);
void msg_close_file();
int msg_printf(const char *format, ...);
/* msg_set_mode() を呼んだ時刻からの単調増加の経過時間 (ミリ秒)。 */
uint64_t msg_now_ms(void);
#ifdef __cplusplus
}
#endif

#ifndef msg_prefix
#define msg_prefix	""
#endif

/* 従来の msg_err などは、各ファイルの msg_prefix をプレフィックスにして
 * カテゴリ server で出力する。レベル判定は書式化の前に行う。 */
#define msg_err(format, ...)	msg_log_at(PX4_LOG_ERROR, PX4_LOG_CAT_SERVER, format, ##__VA_ARGS__)
#define msg_warn(format, ...)	msg_log_at(PX4_LOG_WARN, PX4_LOG_CAT_SERVER, format, ##__VA_ARGS__)
#define msg_info(format, ...)	msg_log_at(PX4_LOG_INFO, PX4_LOG_CAT_SERVER, format, ##__VA_ARGS__)
/* DEBUG は Release ビルドでも LogLevel=debug で出す。 */
#define msg_dbg(format, ...)	msg_log_at(PX4_LOG_DEBUG, PX4_LOG_CAT_SERVER, format, ##__VA_ARGS__)
