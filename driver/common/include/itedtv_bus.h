// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x bus driver definitions (itedtv_bus.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __ITEDTV_BUS_H__
#define __ITEDTV_BUS_H__

#if defined(_WIN32) || defined(_WIN64)
#include <stdint.h>
#endif

#ifdef __linux__
#include <linux/device.h>
#include <linux/usb.h>
#elif defined(__APPLE__)
#include "misc_posix.h"
#include "libusb_compat.h"
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#include "winusb_compat.h"
#endif

enum itedtv_bus_type {
	ITEDTV_BUS_NONE = 0,
	ITEDTV_BUS_USB,
};

/*
 * macOS の libusb 転送層は、受信中に全転送を失った時点で buf == NULL、len == 0 として
 * 1回だけ呼ぶ。handler は buf を参照せず、受信を続けられないことを上位へ伝えて戻る。
 * WinUSB は fail_on_error の指定時に異常を同じ方法で通知する。Linux は NULL を渡さない。
 */
typedef int (*itedtv_bus_stream_handler_t)(void *context, void *buf, u32 len);

struct itedtv_bus;

struct itedtv_bus_operations {
	int (*ctrl_tx)(struct itedtv_bus *bus, void *buf, int len);
	int (*ctrl_rx)(struct itedtv_bus *bus, void *buf, int *len);
	int (*stream_rx)(struct itedtv_bus *bus,
			 void *buf, int *len,
			 int timeout);
	int (*start_streaming)(struct itedtv_bus *bus,
			       itedtv_bus_stream_handler_t stream_handler,
			       void *context);
	int (*stop_streaming)(struct itedtv_bus *bus);
};

struct itedtv_bus {
	struct device *dev;
	enum itedtv_bus_type type;
	union {
		struct {
			struct usb_device *dev;
			int ctrl_timeout;
			int max_bulk_size;
			struct {
				u32 urb_buffer_size;
				u32 urb_num;
				/* 0 は既存機種の 0x84。別機種は USB descriptor の値を指定する。 */
				u8 endpoint;
				/* T230C はFX2の受信overflowを避けるため有効化する。IT930xの転送長は従来どおり、Sianoの0x2000 byteは512 byte境界のままにする。 */
				bool align_stream_buffer_to_packet_size;
				bool fail_on_error;
				/* WinUSB worker がread完了を処理した間隔を診断する。通常は無効。 */
				bool measure_stream_timing;
				bool no_dma;	/* Linux 版で使用する。 */
				bool no_raw_io;	/* Windows の WinUSB 版で使用する。 */
			} streaming;
			void *priv;
		} usb;
	};
	struct itedtv_bus_operations ops;
};

#if defined(_WIN32) || defined(_WIN64)
/* WinUSB のオーバーラップ TS 読み出しに関する前回取得以降のカウンター。 */
struct itedtv_usb_stream_stats {
	uint64_t completed_reads;
	uint64_t completed_bytes;
	uint64_t short_reads;
	uint64_t short_bytes;
	uint64_t read_errors;
	uint64_t read_timeouts;
	uint64_t submit_errors;
	uint64_t wait_timeouts;
	uint64_t wait_errors;
	uint64_t max_worker_observed_completion_interval_us;
	uint32_t last_error;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif
int itedtv_bus_init(struct itedtv_bus *bus);
int itedtv_bus_term(struct itedtv_bus *bus);
#if defined(_WIN32) || defined(_WIN64)
/* 前回の呼び出し以降のカウンターを読み出して初期化する。 */
int itedtv_bus_take_usb_stream_stats(struct itedtv_bus *bus,
				     struct itedtv_usb_stream_stats *stats);
#endif
#ifdef __cplusplus
}
#endif

static inline int itedtv_bus_ctrl_tx(struct itedtv_bus *bus,
				     void *buf, int len)
{
	if (!bus || !bus->ops.ctrl_tx)
		return -EINVAL;

	return bus->ops.ctrl_tx(bus, buf, len);
}

static inline int itedtv_bus_ctrl_rx(struct itedtv_bus *bus,
				     void *buf, int *len)
{
	if (!bus || !bus->ops.ctrl_rx)
		return -EINVAL;

	return bus->ops.ctrl_rx(bus, buf, len);
}

static inline int itedtv_bus_stream_rx(struct itedtv_bus *bus,
				       void *buf, int *len,
				       int timeout)
{
	if (!bus || !bus->ops.stream_rx)
		return -EINVAL;

	return bus->ops.stream_rx(bus, buf, len, timeout);
}

static inline int itedtv_bus_start_streaming(struct itedtv_bus *bus,
					     itedtv_bus_stream_handler_t stream_handler,
					     void *context)
{
	if (!bus || !bus->ops.start_streaming)
		return -EINVAL;

	return bus->ops.start_streaming(bus, stream_handler, context);
}

static inline int itedtv_bus_stop_streaming(struct itedtv_bus *bus)
{
	if (!bus || !bus->ops.stop_streaming)
		return -EINVAL;

	return bus->ops.stop_streaming(bus);
}

#endif
