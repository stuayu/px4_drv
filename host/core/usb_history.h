// usb_history.h - 直近の USB / I2C 制御転送の履歴 (障害調査用)
//
// 既定は無効。LogLevel=trace または LogUsbHistory=true で有効になる。
// 記録は受信スレッドから呼ばれるため、待機ロックも動的確保も行わない。同じ ring slot
// への同時書き込みが競合した場合は、そのイベントを捨てる best-effort 履歴である。
// C からも呼べるように C 互換の宣言だけを置く。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum usb_history_kind {
	USB_HISTORY_CONTROL = 0,	/* ベンダー制御転送 (request、value、index を持つ) */
	USB_HISTORY_BULK = 1,		/* バルク転送 (index にエンドポイント番号を入れる) */
};

enum usb_history_dir {
	USB_HISTORY_IN = 0,		/* デバイスからホストへ */
	USB_HISTORY_OUT = 1,		/* ホストからデバイスへ */
};

#define USB_HISTORY_CAPACITY	256
#define USB_HISTORY_DATA_BYTES	16

struct usb_history_entry {
	uint64_t t_ms;			/* msg_now_ms() の時刻 */
	uint8_t kind;			/* enum usb_history_kind */
	uint8_t dir;			/* enum usb_history_dir */
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;		/* 要求した長さ */
	int32_t result;			/* 転送した長さ。負なら errno 相当の値 */
	uint8_t data_len;		/* data に入っているバイト数 (最大 16) */
	uint8_t data[USB_HISTORY_DATA_BYTES];
};

void usb_history_set_enabled(bool enabled);
bool usb_history_is_enabled(void);

/* 1件記録する。無効なら何もしない。data は先頭 16 byte だけ写す。 */
void usb_history_record(enum usb_history_kind kind, enum usb_history_dir dir,
			uint8_t request, uint16_t value, uint16_t index,
			uint16_t length, int result, const void *data, size_t data_len);

/* 古い順に最大 max 件を out へ写す。戻り値は写した件数。
 * 書き込みと同時に実行しても壊れた件は返さない (書き込み中の件は飛ばす)。 */
size_t usb_history_copy(struct usb_history_entry *out, size_t max);

/* 直近の履歴を INFO で書き出す (カテゴリ usb)。戻り値は書き出した件数。
 * 失敗を検出した呼び出し側が、障害の前後の転送を残すために使う。 */
size_t usb_history_dump(void);

#ifdef __cplusplus
}
#endif
