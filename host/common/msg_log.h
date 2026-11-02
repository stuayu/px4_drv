// msg_log.h - WinUSB 版と macOS 版のユーザー空間ログ (px4_log の実装)

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "px4_log.h"

#ifdef __cplusplus
extern "C" {
#endif

enum msg_log_format {
	MSG_LOG_FORMAT_TEXT = 0,
	MSG_LOG_FORMAT_JSONL = 1,
};

/* 設定値を1つ適用する。key は INI と環境変数で共通の名前で、
 * LogLevel、LogCategories、LogFormat、LogStatsInterval のいずれか。
 * 値が不正な場合は false を返し、設定は変えない。 */
bool msg_log_apply_setting(const char *key, const char *value);

bool msg_log_set_level(const char *name);
bool msg_log_set_categories(const char *list);
bool msg_log_set_format(const char *name);
bool msg_log_set_stats_interval(const char *seconds);
int msg_log_get_level(void);
/* 受信統計の間隔 (ミリ秒)。0 は出力しないことを表す。 */
uint32_t msg_log_get_stats_interval_ms(void);

/* レベルとカテゴリ判定の純粋関数。設定値を引数に取るため、試験でも使える。 */
bool msg_log_is_enabled_for(int configured_level, uint32_t category_mask, int level, int category);
bool msg_log_parse_level(const char *name, int *level);
bool msg_log_parse_categories(const char *list, uint32_t *mask);
const char *msg_log_level_name(int level);
const char *msg_log_level_tag(int level);

/* 1行分の書式化。戻り値は書き込んだ長さ (NUL を除く) で、size を超えた分は切り詰める。 */
size_t msg_log_format_text(char *buf, size_t size, uint64_t t_ms, int level,
			   int category, const char *prefix, const char *message);
size_t msg_log_format_jsonl(char *buf, size_t size, uint64_t t_ms, int level,
			    int category, const char *prefix, const char *message);
/* JSON 文字列の中身として安全な形へ変換する。切り詰めても escape の途中では止めない。 */
size_t msg_log_json_escape(char *buf, size_t size, const char *src);

/* 実際の出力。prefix は "driver device" などの表示名 (空でもよい)。 */
void msg_log_emit(const char *prefix, int level, int category, const char *fmt, ...);
void msg_log_emit_v(const char *prefix, int level, int category, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

/* 既存の msg_err などは、既定のカテゴリ server として出力する。 */
#define msg_log_at(level, category, format, ...)						\
	do {											\
		if (px4_log_enabled((level), (category)))					\
			msg_log_emit(msg_prefix, (level), (category), format, ##__VA_ARGS__);	\
	} while (0)
