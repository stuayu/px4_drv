/* px4_log.h - 共通層 (カーネルとユーザー空間の両方でコンパイルされる) のログ API
 *
 * レベルは ERROR < WARN < INFO < DEBUG < TRACE の順に詳細になる。
 * カテゴリは機能ごとの分類で、ユーザー空間では LogCategories で出力を絞れる。
 *
 * - Linux カーネル: ERROR / WARN / INFO は dev_err / dev_warn / dev_info に、
 *   DEBUG / TRACE は dev_dbg に写像する。dev_dbg は dynamic debug で有効化する。
 * - ユーザー空間 (WinUSB / macOS): 実行時のレベル判定を msg_log.c で行う。
 *   無効なレベルのマクロは引数を評価せず、Release ビルドでも DEBUG / TRACE を出せる。
 */
#pragma once

enum px4_log_level {
	PX4_LOG_ERROR = 1,
	PX4_LOG_WARN = 2,
	PX4_LOG_INFO = 3,
	PX4_LOG_DEBUG = 4,
	PX4_LOG_TRACE = 5,
};

enum px4_log_category {
	PX4_LOG_CAT_USB = 0,
	PX4_LOG_CAT_I2C,
	PX4_LOG_CAT_DEMOD,
	PX4_LOG_CAT_TUNER,
	PX4_LOG_CAT_TS,
	PX4_LOG_CAT_CARD,
	PX4_LOG_CAT_AUTH,
	PX4_LOG_CAT_SERVER,
	PX4_LOG_CAT_DEVICE,
	PX4_LOG_CAT_COUNT,
};

#define PX4_LOG_CAT_MASK_ALL	((1u << PX4_LOG_CAT_COUNT) - 1u)

/* 設定ファイルと環境変数で使うカテゴリ名。順序は enum px4_log_category と一致させる。 */
static inline const char *px4_log_category_name(int category)
{
	static const char *const names[PX4_LOG_CAT_COUNT] = {
		"usb", "i2c", "demod", "tuner", "ts", "card", "auth", "server", "device",
	};

	if (category < 0 || category >= PX4_LOG_CAT_COUNT)
		return "?";
	return names[category];
}

#if defined(__KERNEL__)

/* 出力の振り分けは実行時の level 値で行う。呼び出し側は定数を渡すため、
 * 不要な分岐は通常のコンパイラ最適化で消える。 */
#define px4_log(dev, level, category, fmt, ...)					\
	do {									\
		switch (level) {						\
		case PX4_LOG_ERROR:						\
			dev_err((dev), "[%s] " fmt, px4_log_category_name(category),	\
				##__VA_ARGS__);					\
			break;							\
		case PX4_LOG_WARN:						\
			dev_warn((dev), "[%s] " fmt, px4_log_category_name(category),	\
				##__VA_ARGS__);					\
			break;							\
		case PX4_LOG_INFO:						\
			dev_info((dev), "[%s] " fmt, px4_log_category_name(category),	\
				##__VA_ARGS__);					\
			break;							\
		case PX4_LOG_DEBUG:						\
			dev_dbg((dev), "[%s] " fmt, px4_log_category_name(category),	\
				##__VA_ARGS__);					\
			break;							\
		default:							\
			dev_dbg((dev), "[trace][%s] " fmt, px4_log_category_name(category),	\
				##__VA_ARGS__);					\
			break;							\
		}								\
	} while (0)

#else /* ユーザー空間 */

#ifdef __cplusplus
extern "C" {
#endif

/* msg_log.c で実装する。level と category が出力条件を満たすかを返す。 */
int px4_log_enabled(int level, int category);
/* driver と device は struct device の名前。プレフィックスは "driver device" になる。 */
void px4_log_emit_dev(const char *driver, const char *device, int level,
		      int category, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/* dev は driver_name と device_name を持つ struct device を指す。 */
#define px4_log(dev, level, category, fmt, ...)					\
	do {									\
		if (px4_log_enabled((level), (category)))				\
			px4_log_emit_dev((dev)->driver_name, (dev)->device_name,	\
					 (level), (category), (fmt), ##__VA_ARGS__);	\
	} while (0)

#endif /* __KERNEL__ */

/* レベル別マクロ。カテゴリは PX4_LOG_CAT_* を渡す。 */
#define px4_log_err(dev, category, fmt, ...)	\
	px4_log((dev), PX4_LOG_ERROR, (category), fmt, ##__VA_ARGS__)
#define px4_log_warn(dev, category, fmt, ...)	\
	px4_log((dev), PX4_LOG_WARN, (category), fmt, ##__VA_ARGS__)
#define px4_log_info(dev, category, fmt, ...)	\
	px4_log((dev), PX4_LOG_INFO, (category), fmt, ##__VA_ARGS__)
#define px4_log_debug(dev, category, fmt, ...)	\
	px4_log((dev), PX4_LOG_DEBUG, (category), fmt, ##__VA_ARGS__)
#define px4_log_trace(dev, category, fmt, ...)	\
	px4_log((dev), PX4_LOG_TRACE, (category), fmt, ##__VA_ARGS__)
