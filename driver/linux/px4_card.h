// SPDX-License-Identifier: GPL-2.0-only
/*
 * 内蔵カードリーダーのデバイスファイル (/dev/px4card/<ID>) の機種共通層
 *
 * カードの検出・リセット・UART の送受信は IT930x の共通処理で行う
 * 機種ごとの差は、ロック・利用可能状態・チューナーと共有する基板電源の参照だけに閉じ込める
 */

#ifndef __PX4_CARD_H__
#define __PX4_CARD_H__

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/mutex.h>

#include "it930x.h"
#include "px4_card_ioctl.h"

struct px4_card;
struct usb_device;

struct px4_card_operations {
	int (*power_on)(void *context);
	int (*power_off)(void *context);
	int (*set_timeout)(void *context, u32 timeout_ms);
	int (*detect)(void *context, bool *present);
	int (*reset)(void *context);
	int (*set_baudrate)(void *context, u32 baudrate);
	int (*rx_ready)(void *context, bool *ready);
	int (*read)(void *context, struct px4_card_frame *frame);
	int (*write)(void *context, const struct px4_card_frame *frame);
};

struct px4_card_backend {
	struct it930x_bridge *it930x;
	/* 非 IT930x ブリッジは各操作で必要なロック範囲を自分で管理する */
	const struct px4_card_operations *operations;
	/* IT930x バックエンドではチューナーと共有するロック。operations 使用時は各 callback がロックを取る */
	struct mutex *lock;
	/* USB 切断で 0 になる機種の利用可能状態 */
	atomic_t *available;
	/*
	 * lock を保持した状態で呼ばれ、チューナーと共有する基板電源のカード側の参照を取得・解放する
	 * 切断後にも解放のために呼ばれるため、利用可能でなければハードウェアへ触れずに参照だけを戻す
	 * カードの検出と通信に基板電源を必要としない機種は NULL にする (受信回路をカードのために動作させない)
	 */
	int (*set_power)(void *context, bool on);
	void *context;
};

/* モジュールの読み込み時と取り外し時に、USB ドライバーの登録の前と解除の後で呼ぶ */
int px4_card_init(void);
void px4_card_exit(void);
/* 戻り値は kfree() で解放する ID 文字列か ERR_PTR (シリアル番号を使えなければ -EINVAL) */
char *px4_card_reader_id(struct usb_device *usb);
int px4_card_register(struct device *parent,
		      const struct px4_card_backend *backend,
		      struct px4_card **result);
void px4_card_unregister(struct px4_card *card);

#endif
