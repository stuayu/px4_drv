// msg_log.c - WinUSB 版と macOS 版で共通のユーザー空間ログ

#include "msg_log.h"

#include <stdio.h>
#include <string.h>

#include "msg.h"

/*
 * 設定値は起動時に一度だけ変更する。受信スレッドからは読むだけなので、
 * 読み出しは volatile で十分であり、ロックを取らない。
 */
static volatile int g_level = PX4_LOG_INFO;
static volatile uint32_t g_categories = PX4_LOG_CAT_MASK_ALL;
static volatile int g_format = MSG_LOG_FORMAT_TEXT;
/* Stream-side statistics are opt-in because their formatter writes synchronously. */
static volatile uint32_t g_stats_interval_ms = 0;

/* 1行の上限。msg_printf の内部バッファ (1024 byte) を超えると切り詰めではなく
 * 失敗扱いになるため、それより十分小さくする。 */
#define MSG_LOG_LINE_SIZE	1000
/* 1メッセージの上限。超えた分は vsnprintf で切り詰める。 */
#define MSG_LOG_MESSAGE_SIZE	384

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char to_lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 前後の空白を除いた文字列 [*begin, *end) を比べる。大文字と小文字は区別しない。 */
static bool token_equals(const char *begin, const char *end, const char *name)
{
	while (begin < end && is_space(*begin))
		begin++;
	while (end > begin && is_space(end[-1]))
		end--;

	for (; begin < end && *name; begin++, name++) {
		if (to_lower_ascii(*begin) != to_lower_ascii(*name))
			return false;
	}
	return begin == end && *name == '\0';
}

static bool token_is_empty(const char *begin, const char *end)
{
	while (begin < end && is_space(*begin))
		begin++;
	return begin == end;
}

static int category_from_token(const char *begin, const char *end)
{
	int cat;

	for (cat = 0; cat < PX4_LOG_CAT_COUNT; cat++) {
		if (token_equals(begin, end, px4_log_category_name(cat)))
			return cat;
	}
	return -1;
}

bool msg_log_parse_level(const char *name, int *level)
{
	static const struct {
		const char *name;
		int level;
	} table[] = {
		{ "error", PX4_LOG_ERROR },
		{ "warn", PX4_LOG_WARN },
		{ "info", PX4_LOG_INFO },
		{ "debug", PX4_LOG_DEBUG },
		{ "trace", PX4_LOG_TRACE },
	};
	const char *end;
	size_t i;

	if (!name || !level)
		return false;

	end = name + strlen(name);
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (token_equals(name, end, table[i].name)) {
			*level = table[i].level;
			return true;
		}
	}
	return false;
}

bool msg_log_parse_categories(const char *list, uint32_t *mask)
{
	const char *p;
	uint32_t result = 0;
	bool any = false;

	if (!mask)
		return false;

	if (!list || token_is_empty(list, list + strlen(list)) || token_equals(list, list + strlen(list), "all")) {
		*mask = PX4_LOG_CAT_MASK_ALL;
		return true;
	}

	p = list;
	while (true) {
		const char *token_end = strchr(p, ',');
		if (!token_end)
			token_end = p + strlen(p);

		if (!token_is_empty(p, token_end)) {
			int cat = category_from_token(p, token_end);
			if (cat < 0)
				return false;
			result |= 1u << cat;
			any = true;
		}

		if (*token_end == '\0')
			break;
		p = token_end + 1;
	}

	if (!any)
		return false;
	*mask = result;
	return true;
}

const char *msg_log_level_name(int level)
{
	switch (level) {
	case PX4_LOG_ERROR: return "error";
	case PX4_LOG_WARN: return "warn";
	case PX4_LOG_INFO: return "info";
	case PX4_LOG_DEBUG: return "debug";
	case PX4_LOG_TRACE: return "trace";
	default: return "unknown";
	}
}

const char *msg_log_level_tag(int level)
{
	switch (level) {
	case PX4_LOG_ERROR: return "ERR";
	case PX4_LOG_WARN: return "WARN";
	case PX4_LOG_INFO: return "INFO";
	case PX4_LOG_DEBUG: return "DBG";
	case PX4_LOG_TRACE: return "TRACE";
	default: return "?";
	}
}

bool msg_log_is_enabled_for(int configured_level, uint32_t category_mask, int level, int category)
{
	if (level < PX4_LOG_ERROR || level > PX4_LOG_TRACE)
		return false;
	if (level > configured_level)
		return false;
	/* ERROR はカテゴリの設定に関係なく出す。障害を見落とさないため。 */
	if (level == PX4_LOG_ERROR)
		return true;
	if (category < 0 || category >= PX4_LOG_CAT_COUNT)
		return false;
	return (category_mask & (1u << category)) != 0;
}

bool msg_log_set_level(const char *name)
{
	int level;

	if (!msg_log_parse_level(name, &level))
		return false;
	g_level = level;
	return true;
}

bool msg_log_set_categories(const char *list)
{
	uint32_t mask;

	if (!msg_log_parse_categories(list, &mask))
		return false;
	g_categories = mask;
	return true;
}

bool msg_log_set_format(const char *name)
{
	const char *end;

	if (!name)
		return false;
	end = name + strlen(name);
	if (token_equals(name, end, "text")) {
		g_format = MSG_LOG_FORMAT_TEXT;
		return true;
	}
	if (token_equals(name, end, "jsonl")) {
		g_format = MSG_LOG_FORMAT_JSONL;
		return true;
	}
	return false;
}

bool msg_log_set_stats_interval(const char *seconds)
{
	const char *p;
	unsigned long value = 0;

	if (!seconds || !*seconds)
		return false;

	for (p = seconds; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
		value = value * 10 + (unsigned long)(*p - '0');
		/* 1日を超える間隔は受け付けない。 */
		if (value > 86400)
			return false;
	}

	g_stats_interval_ms = (uint32_t)(value * 1000);
	return true;
}

int msg_log_get_level(void)
{
	return g_level;
}

uint32_t msg_log_get_stats_interval_ms(void)
{
	return g_stats_interval_ms;
}

bool msg_log_apply_setting(const char *key, const char *value)
{
	if (!key || !value)
		return false;
	if (!strcmp(key, "LogLevel"))
		return msg_log_set_level(value);
	if (!strcmp(key, "LogCategories"))
		return msg_log_set_categories(value);
	if (!strcmp(key, "LogFormat"))
		return msg_log_set_format(value);
	if (!strcmp(key, "LogStatsInterval"))
		return msg_log_set_stats_interval(value);
	return false;
}

size_t msg_log_format_text(char *buf, size_t size, uint64_t t_ms, int level,
			   int category, const char *prefix, const char *message)
{
	int n;

	if (!buf || !size)
		return 0;
	if (!prefix)
		prefix = "";
	if (!message)
		message = "";

	n = snprintf(buf, size, "[%llums] [%s] [%s] %s%s%s",
		     (unsigned long long)t_ms, px4_log_category_name(category),
		     msg_log_level_tag(level), prefix, *prefix ? ": " : "", message);
	if (n < 0) {
		buf[0] = '\0';
		return 0;
	}
	return ((size_t)n < size) ? (size_t)n : size - 1;
}

/* 出力先の残り容量を管理する。末尾の追加に必要な分は reserve として残す。 */
struct json_out {
	char *buf;
	size_t size;
	size_t pos;
};

static void json_put(struct json_out *o, const char *s)
{
	while (*s && o->pos + 1 < o->size)
		o->buf[o->pos++] = *s++;
	o->buf[o->pos] = '\0';
}

/* src を JSON 文字列の中身として書き出す。reserve 分の空きは残す。
 * 容量が足りない場合は、エスケープ列や UTF-8 の多バイト文字の途中で止めない。 */
static void json_put_escaped(struct json_out *o, const char *src, size_t reserve)
{
	const unsigned char *p = (const unsigned char *)src;
	static const char hex[] = "0123456789abcdef";

	while (*p) {
		char tmp[8];
		size_t len = 0;
		size_t seq = 1;
		size_t i;

		if (*p == '"' || *p == '\\') {
			tmp[len++] = '\\';
			tmp[len++] = (char)*p;
		} else if (*p == '\n') {
			tmp[len++] = '\\';
			tmp[len++] = 'n';
		} else if (*p == '\r') {
			tmp[len++] = '\\';
			tmp[len++] = 'r';
		} else if (*p == '\t') {
			tmp[len++] = '\\';
			tmp[len++] = 't';
		} else if (*p == '\b') {
			tmp[len++] = '\\';
			tmp[len++] = 'b';
		} else if (*p == '\f') {
			tmp[len++] = '\\';
			tmp[len++] = 'f';
		} else if (*p < 0x20) {
			tmp[len++] = '\\';
			tmp[len++] = 'u';
			tmp[len++] = '0';
			tmp[len++] = '0';
			tmp[len++] = hex[(*p >> 4) & 0xf];
			tmp[len++] = hex[*p & 0xf];
		} else if (*p >= 0xf0) {
			/* UTF-8 の多バイト文字は1文字単位で扱う。 */
			seq = 4;
			tmp[len++] = (char)*p;
		} else if (*p >= 0xe0) {
			seq = 3;
			tmp[len++] = (char)*p;
		} else if (*p >= 0xc0) {
			seq = 2;
			tmp[len++] = (char)*p;
		} else {
			tmp[len++] = (char)*p;
		}

		/* 多バイト文字は、途中の終端 (NUL) まで読めている場合だけ書く。 */
		if (seq > 1) {
			size_t k;
			for (k = 1; k < seq; k++) {
				if (!p[k])
					return;
			}
			for (k = 1; k < seq; k++)
				tmp[len++] = (char)p[k];
		}

		if (o->pos + len + reserve + 1 > o->size)
			return;

		for (i = 0; i < len; i++)
			o->buf[o->pos++] = tmp[i];
		o->buf[o->pos] = '\0';
		p += seq;
	}
}

size_t msg_log_json_escape(char *buf, size_t size, const char *src)
{
	struct json_out o = { buf, size, 0 };

	if (!buf || !size)
		return 0;
	buf[0] = '\0';
	if (src)
		json_put_escaped(&o, src, 0);
	return o.pos;
}

size_t msg_log_format_jsonl(char *buf, size_t size, uint64_t t_ms, int level,
			    int category, const char *prefix, const char *message)
{
	struct json_out o = { buf, size, 0 };
	char head[128];
	size_t msg_len;
	/* 末尾は "}\n" と NUL の分を残す。 */
	const size_t tail_reserve = 2;

	if (!buf || !size)
		return 0;
	if (!prefix)
		prefix = "";
	if (!message)
		message = "";

	snprintf(head, sizeof(head), "{\"t_ms\":%llu,\"level\":\"%s\",\"cat\":\"%s\",\"dev\":\"",
		 (unsigned long long)t_ms, msg_log_level_name(level),
		 px4_log_category_name(category));
	json_put(&o, head);
	json_put_escaped(&o, prefix, tail_reserve + 16);
	json_put(&o, "\",\"msg\":\"");

	/* JSON の1行に改行を入れないため、末尾の改行は落とす。 */
	msg_len = strlen(message);
	while (msg_len > 0 && (message[msg_len - 1] == '\n' || message[msg_len - 1] == '\r'))
		msg_len--;
	{
		char trimmed[MSG_LOG_MESSAGE_SIZE];
		size_t n = msg_len < sizeof(trimmed) - 1 ? msg_len : sizeof(trimmed) - 1;
		memcpy(trimmed, message, n);
		trimmed[n] = '\0';
		json_put_escaped(&o, trimmed, tail_reserve);
	}

	json_put(&o, "\"}\n");
	return o.pos;
}

int px4_log_enabled(int level, int category)
{
	return msg_log_is_enabled_for(g_level, g_categories, level, category) ? 1 : 0;
}

void msg_log_emit_v(const char *prefix, int level, int category, const char *fmt, va_list ap)
{
	char message[MSG_LOG_MESSAGE_SIZE];
	char line[MSG_LOG_LINE_SIZE];
	size_t len;

	if (!prefix)
		prefix = "";
	if (!fmt)
		return;

	/* 呼び出し元の ap を読むだけで、この関数の外へ渡さない。 */
	vsnprintf(message, sizeof(message), fmt, ap);

	if (g_format == MSG_LOG_FORMAT_JSONL) {
		len = msg_log_format_jsonl(line, sizeof(line), msg_now_ms(), level, category, prefix, message);
	} else {
		len = msg_log_format_text(line, sizeof(line) - 2, msg_now_ms(), level, category, prefix, message);
		/* 既存の表示と同じく、1メッセージを1行にそろえる。 */
		if (len == 0 || line[len - 1] != '\n') {
			line[len++] = '\n';
			line[len] = '\0';
		}
	}

	msg_printf("%s", line);
}

void msg_log_emit(const char *prefix, int level, int category, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	msg_log_emit_v(prefix, level, category, fmt, ap);
	va_end(ap);
}

void px4_log_emit_dev(const char *driver, const char *device, int level,
		      int category, const char *fmt, ...)
{
	char prefix[128];
	va_list ap;

	if (!driver)
		driver = "";
	if (!device)
		device = "";
	snprintf(prefix, sizeof(prefix), "%s %s", driver, device);
	/* 前後の空白は落とす。デバイス名が空の場合も同じ見た目にする。 */
	if (!*device)
		snprintf(prefix, sizeof(prefix), "%s", driver);

	va_start(ap, fmt);
	msg_log_emit_v(prefix, level, category, fmt, ap);
	va_end(ap);
}
