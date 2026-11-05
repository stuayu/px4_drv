// itedtv_bus_libusb.c - libusb transport layer for macOS (replaces itedtv_bus_winusb.c)

#include "misc_posix.h"
#include "libusb_compat.h"

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "itedtv_bus.h"
#include "usb_history.h"

// ---------------------------------------------------------------------------
// Internal context
// ---------------------------------------------------------------------------

struct itedtv_usb_work {
	struct itedtv_usb_context *ctx;
	struct libusb_transfer *transfer;
	void *buffer;
	size_t size;
	bool submitted;
};

struct itedtv_usb_context {
	pthread_mutex_t lock;
	pthread_cond_t stopped;
	struct itedtv_bus *bus;
	itedtv_bus_stream_handler_t stream_handler;
	void *handler_ctx;
	uint32_t num_urb;
	uint32_t num_works;
	struct itedtv_usb_work *works;
	/* streaming と submitted は lock で保護し、cancel完了までworkを保持する。 */
	int streaming;
	pthread_t worker_thread;
	bool worker_running;
	bool stopping;
	/* 受信中に全転送を失ったことを通知済みか。次の開始で初期化する。 */
	bool stream_failed;
	/* 通知 callback の実行中は worker を止めず、停止側に完了を待たせる。 */
	bool failure_notifying;
};

// ---------------------------------------------------------------------------
// Control transfer helpers
// ---------------------------------------------------------------------------

static int itedtv_usb_error_to_errno(int error)
{
	switch (error) {
	case LIBUSB_ERROR_TIMEOUT: return -ETIMEDOUT;
	case LIBUSB_ERROR_NO_DEVICE: return -ENODEV;
	case LIBUSB_ERROR_ACCESS: return -EACCES;
	case LIBUSB_ERROR_NO_MEM: return -ENOMEM;
	case LIBUSB_ERROR_BUSY: return -EBUSY;
	case LIBUSB_ERROR_OVERFLOW: return -EOVERFLOW;
	case LIBUSB_ERROR_INTERRUPTED: return -EINTR;
	case LIBUSB_ERROR_PIPE: return -EPIPE;
	case LIBUSB_ERROR_INVALID_PARAM: return -EINVAL;
	case LIBUSB_ERROR_NOT_SUPPORTED: return -ENOTSUP;
	default: return -EIO;
	}
}

static int itedtv_usb_ctrl_tx(struct itedtv_bus *bus, void *buf, int len)
{
	libusb_device_handle *handle = bus->usb.dev->handle;
	int transferred = 0;
	int ret;

	if (!buf || len <= 0)
		return -EINVAL;

	/* Endpoint 0x02: Host->Device bulk for device control */
	ret = libusb_bulk_transfer(handle, 0x02,
	                           (unsigned char *)buf, len,
	                           &transferred,
	                           bus->usb.ctrl_timeout);
	usb_history_record(USB_HISTORY_BULK, USB_HISTORY_OUT, 0, 0, 0x02,
	                   (uint16_t)(len > 0xffff ? 0xffff : len),
	                   ret ? itedtv_usb_error_to_errno(ret) : transferred,
	                   buf, (size_t)len);
	if (ret) {
		dev_err(bus->dev, "itedtv_usb_ctrl_tx: libusb_bulk_transfer() failed (%s)\n",
		        libusb_strerror(ret));
		usb_history_dump();
		return itedtv_usb_error_to_errno(ret);
	}
	/* 部分送信を成功扱いすると、ブリッジが不完全なコマンドを受け取る。 */
	if (transferred != len) {
		dev_err(bus->dev, "itedtv_usb_ctrl_tx: short transfer (%d/%d)\n", transferred, len);
		usb_history_dump();
		return -EIO;
	}

	usleep(1000); /* 1 ms delay as in Windows driver */
	return 0;
}

static int itedtv_usb_ctrl_rx(struct itedtv_bus *bus, void *buf, int *len)
{
	libusb_device_handle *handle = bus->usb.dev->handle;
	int transferred = 0;
	int ret;
	const int requested = (len && *len > 0) ? *len : 0;

	if (!buf || !len || *len <= 0)
		return -EINVAL;

	/* Endpoint 0x81: Device->Host bulk for device control */
	ret = libusb_bulk_transfer(handle, 0x81,
	                           (unsigned char *)buf, *len,
	                           &transferred,
	                           bus->usb.ctrl_timeout);
	usb_history_record(USB_HISTORY_BULK, USB_HISTORY_IN, 0, 0, 0x81,
	                   (uint16_t)(requested > 0xffff ? 0xffff : requested),
	                   ret ? itedtv_usb_error_to_errno(ret) : transferred,
	                   buf, ret ? 0 : (size_t)transferred);
	if (ret) {
		dev_err(bus->dev, "itedtv_usb_ctrl_rx: libusb_bulk_transfer() failed (%s)\n",
		        libusb_strerror(ret));
		*len = -1;
		usb_history_dump();
		return itedtv_usb_error_to_errno(ret);
	}

	*len = transferred;
	usleep(1000);
	return 0;
}

static int itedtv_usb_stream_rx(struct itedtv_bus *bus, void *buf, int *len, int timeout)
{
	libusb_device_handle *handle = bus->usb.dev->handle;
	int transferred = 0;
	int ret;

	if (!buf || !len || *len <= 0)
		return -EINVAL;

	/* Endpoint 0x84: Device->Host bulk for TS data */
	ret = libusb_bulk_transfer(handle, 0x84,
	                           (unsigned char *)buf, *len,
	                           &transferred, timeout);
	*len = transferred;

	return ret ? itedtv_usb_error_to_errno(ret) : 0;
}

// ---------------------------------------------------------------------------
// Streaming (async transfers using libusb async API)
// ---------------------------------------------------------------------------

static bool itedtv_usb_any_submitted(const struct itedtv_usb_context *ctx)
{
	for (uint32_t i = 0; i < ctx->num_urb; i++) {
		if (ctx->works[i].submitted)
			return true;
	}
	return false;
}

static void LIBUSB_CALL itedtv_usb_transfer_callback(struct libusb_transfer *transfer)
{
	struct itedtv_usb_work *work = (struct itedtv_usb_work *)transfer->user_data;
	struct itedtv_usb_context *ctx = work->ctx;
	itedtv_bus_stream_handler_t handler = NULL;
	void *handler_ctx = NULL;
	bool notify_failure = false;
	int handler_result = 0;

	pthread_mutex_lock(&ctx->lock);
	if ((transfer->status == LIBUSB_TRANSFER_COMPLETED ||
	    (ctx->bus->usb.streaming.fail_on_error && transfer->status == LIBUSB_TRANSFER_TIMED_OUT)) &&
	    transfer->actual_length > 0 && ctx->streaming) {
		handler = ctx->stream_handler;
		handler_ctx = ctx->handler_ctx;
	}
	pthread_mutex_unlock(&ctx->lock);

	/* 受信処理中もsubmittedを保持し、停止側がhandlerの終了まで待てるようにする。 */
	if (handler)
		handler_result = handler(handler_ctx, transfer->buffer, (uint32_t)transfer->actual_length);

	pthread_mutex_lock(&ctx->lock);
	work->submitted = false;

	if (ctx->streaming && !(ctx->bus->usb.streaming.fail_on_error && handler_result) &&
	    (transfer->status == LIBUSB_TRANSFER_COMPLETED ||
	    (ctx->bus->usb.streaming.fail_on_error && transfer->status == LIBUSB_TRANSFER_TIMED_OUT))) {
		int ret = libusb_submit_transfer(transfer);
		if (ret == 0)
			work->submitted = true;
		else
			dev_err(ctx->bus->dev, "itedtv_usb_transfer_callback: resubmit failed (%s)\n",
			        libusb_strerror(ret));
	} else if (ctx->streaming && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
		/* STALL等を無条件で再送すると同じ異常完了を繰り返す。 */
		dev_err(ctx->bus->dev, "itedtv_usb_transfer_callback: transfer failed (status=%d)\n",
		        (int)transfer->status);
	}

	/*
	 * 既存機種は最後の転送を失った場合に通知する。T230C は設定本数が
	 * 維持できなくなった時点で残りも停止し、同じ通知を1回だけ送る。
	 */
	if (ctx->streaming && !work->submitted && !ctx->stream_failed &&
	    (ctx->bus->usb.streaming.fail_on_error || !itedtv_usb_any_submitted(ctx))) {
		if (ctx->bus->usb.streaming.fail_on_error) {
			/* 残りの転送も停止し、cancel 完了までバッファーを保持する。 */
			ctx->streaming = 0;
			for (uint32_t i = 0; i < ctx->num_urb; i++)
				if (ctx->works[i].submitted)
					libusb_cancel_transfer(ctx->works[i].transfer);
		}
		ctx->stream_failed = true;
		ctx->failure_notifying = true;
		handler = ctx->stream_handler;
		handler_ctx = ctx->handler_ctx;
		notify_failure = true;
	}
	pthread_mutex_unlock(&ctx->lock);

	if (!notify_failure)
		return;

	if (ctx->bus->usb.streaming.fail_on_error)
		dev_err(ctx->bus->dev, "itedtv_usb_transfer_callback: stream transfer failure\n");
	else
		dev_err(ctx->bus->dev, "itedtv_usb_transfer_callback: all stream transfers were lost\n");
	/* 上位は StreamBuffer の通知だけを行う。ctx のロックは保持しない。 */
	handler(handler_ctx, NULL, 0);

	pthread_mutex_lock(&ctx->lock);
	ctx->failure_notifying = false;
	pthread_mutex_unlock(&ctx->lock);
}

static void *itedtv_usb_worker(void *arg)
{
	struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)arg;
	for (;;) {
		bool pending;
		struct timeval tv = { 0, 100000 }; /* 100 ms */

		pthread_mutex_lock(&ctx->lock);
		pending = ctx->streaming != 0 || ctx->failure_notifying;
		for (uint32_t i = 0; i < ctx->num_urb; i++)
			pending = pending || ctx->works[i].submitted;
		pthread_mutex_unlock(&ctx->lock);
		if (!pending)
			break;
		/* cancelも非同期なので、全転送の完了通知までeventを処理する。 */
		libusb_handle_events_timeout(NULL, &tv);
	}

	return NULL;
}

static int itedtv_usb_start_streaming(struct itedtv_bus *bus,
                                      itedtv_bus_stream_handler_t stream_handler,
                                      void *context)
{
	struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)bus->usb.priv;
	libusb_device_handle *handle = bus->usb.dev->handle;
	uint32_t buf_size, num;

	if (!stream_handler)
		return -EINVAL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming\n");

	pthread_mutex_lock(&ctx->lock);
	if (ctx->streaming || ctx->worker_running || ctx->stopping) {
		pthread_mutex_unlock(&ctx->lock);
		return -EBUSY;
	}

	buf_size = bus->usb.streaming.urb_buffer_size;
	num      = bus->usb.streaming.urb_num;
	if (!buf_size || buf_size > INT_MAX || !num) {
		pthread_mutex_unlock(&ctx->lock);
		return -EINVAL;
	}
	if (bus->usb.streaming.align_stream_buffer_to_packet_size) {
		if (bus->usb.max_bulk_size <= 0) {
			pthread_mutex_unlock(&ctx->lock);
			return -EINVAL;
		}
		const uint32_t packet_size = (uint32_t)bus->usb.max_bulk_size;
		const uint32_t remainder = buf_size % packet_size;
		if (remainder) {
			const uint32_t padding = packet_size - remainder;
			if (padding > (uint32_t)INT_MAX - buf_size) {
				pthread_mutex_unlock(&ctx->lock);
				return -EINVAL;
			}
			/* T230CのFX2は512 byte単位でTSを送るため、最後のUSB packetも収まる長さにする。 */
			buf_size += padding;
		}
	}

	if (num > 64)
		num = 64;
	ctx->stream_handler = stream_handler;
	ctx->handler_ctx    = context;

	/* Reallocate work array if size changed */
	if (ctx->works && num != ctx->num_works) {
		for (uint32_t i = 0; i < ctx->num_works; i++) {
			if (ctx->works[i].transfer)
				libusb_free_transfer(ctx->works[i].transfer);
			free(ctx->works[i].buffer);
		}
		free(ctx->works);
		ctx->works = NULL;
	}

	ctx->num_works = num;

	if (!ctx->works) {
		ctx->works = (struct itedtv_usb_work *)calloc(num, sizeof(*ctx->works));
		if (!ctx->works) {
			ctx->num_works = 0;
			ctx->num_urb = 0;
			ctx->stream_handler = NULL;
			ctx->handler_ctx = NULL;
			pthread_mutex_unlock(&ctx->lock);
			return -ENOMEM;
		}
	}

	/* Allocate transfer buffers and libusb_transfer objects */
	uint32_t i;
	for (i = 0; i < num; i++) {
		/* 同じURB数で受信長が変わっても古い小さいbufferを使わない。 */
		if (ctx->works[i].buffer && ctx->works[i].size != buf_size) {
			free(ctx->works[i].buffer);
			ctx->works[i].buffer = NULL;
			ctx->works[i].size = 0;
		}
		if (!ctx->works[i].buffer) {
			ctx->works[i].buffer = malloc(buf_size);
			if (!ctx->works[i].buffer)
				break;
			ctx->works[i].size = buf_size;
		}

		if (!ctx->works[i].transfer) {
			ctx->works[i].transfer = libusb_alloc_transfer(0);
			if (!ctx->works[i].transfer)
				break;
		}

		ctx->works[i].ctx = ctx;
		libusb_fill_bulk_transfer(ctx->works[i].transfer, handle,
					  bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84,
		                          (unsigned char *)ctx->works[i].buffer,
		                          (int)buf_size,
		                          itedtv_usb_transfer_callback,
		                          &ctx->works[i], 0);
	}

	if (i != num) {
		/* 設定された転送数を確保できない場合、受信余裕を減らしたまま開始しない。 */
		ctx->num_urb = 0;
		ctx->stream_handler = NULL;
		ctx->handler_ctx = NULL;
		pthread_mutex_unlock(&ctx->lock);
		return -ENOMEM;
	}
	ctx->num_urb = i;

	/* Clear the endpoint */
	int clear_ret = libusb_clear_halt(handle, bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84);
	if (clear_ret && bus->usb.streaming.fail_on_error) {
		ctx->stream_handler = NULL;
		ctx->handler_ctx = NULL;
		pthread_mutex_unlock(&ctx->lock);
		return itedtv_usb_error_to_errno(clear_ret);
	}
	ctx->stream_failed = false;
	ctx->streaming = 1;

	/* 最初のsubmit前にworkerを作ると、thread作成失敗時に未完了転送が残らない。 */
	if (pthread_create(&ctx->worker_thread, NULL, itedtv_usb_worker, ctx)) {
		ctx->streaming = 0;
		ctx->stream_handler = NULL;
		ctx->handler_ctx = NULL;
		pthread_mutex_unlock(&ctx->lock);
		return -ECHILD;
	}
	ctx->worker_running = true;

	/* Submit all transfers */
	for (i = 0; i < ctx->num_urb; i++) {
		int ret = libusb_submit_transfer(ctx->works[i].transfer);
		if (ret) {
			dev_err(bus->dev,
			        "itedtv_usb_start_streaming: libusb_submit_transfer() failed (i=%u, %s)\n",
			        i, libusb_strerror(ret));
			ctx->streaming = 0;
			ctx->stopping = true;
			for (uint32_t j = 0; j < i; j++) {
				if (ctx->works[j].submitted)
					libusb_cancel_transfer(ctx->works[j].transfer);
			}
			pthread_mutex_unlock(&ctx->lock);
			pthread_join(ctx->worker_thread, NULL);
			pthread_mutex_lock(&ctx->lock);
			ctx->worker_running = false;
			ctx->stopping = false;
			ctx->stream_handler = NULL;
			ctx->handler_ctx = NULL;
			pthread_cond_broadcast(&ctx->stopped);
			pthread_mutex_unlock(&ctx->lock);
			return itedtv_usb_error_to_errno(ret);
		}
		ctx->works[i].submitted = true;
	}

	dev_dbg(bus->dev, "itedtv_usb_start_streaming: num=%u\n", ctx->num_urb);

	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int itedtv_usb_stop_streaming(struct itedtv_bus *bus)
{
	struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)bus->usb.priv;
	dev_dbg(bus->dev, "itedtv_usb_stop_streaming\n");

	pthread_mutex_lock(&ctx->lock);
	while (ctx->stopping)
		pthread_cond_wait(&ctx->stopped, &ctx->lock);
	if (!ctx->worker_running) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}
	ctx->stopping = true;

	ctx->streaming = 0;

	/* Cancel all pending transfers */
	for (uint32_t i = 0; i < ctx->num_urb; i++) {
		if (ctx->works[i].submitted && ctx->works[i].transfer)
			libusb_cancel_transfer(ctx->works[i].transfer);
	}

	pthread_mutex_unlock(&ctx->lock);

	/* workerは全cancel callbackを処理してから終了する。 */
	pthread_join(ctx->worker_thread, NULL);

	pthread_mutex_lock(&ctx->lock);
	ctx->worker_running = false;
	ctx->stopping = false;
	ctx->stream_handler = NULL;
	ctx->handler_ctx    = NULL;
	pthread_cond_broadcast(&ctx->stopped);
	pthread_mutex_unlock(&ctx->lock);

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming: done\n");
	return 0;
}

// ---------------------------------------------------------------------------
// Init / Term
// ---------------------------------------------------------------------------

int itedtv_bus_init(struct itedtv_bus *bus)
{
	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx;

		if (!bus->usb.dev || !bus->usb.dev->handle)
			return -EINVAL;

		ctx = (struct itedtv_usb_context *)calloc(1, sizeof(*ctx));
		if (!ctx)
			return -ENOMEM;

		pthread_mutex_init(&ctx->lock, NULL);
		pthread_cond_init(&ctx->stopped, NULL);
		ctx->bus = bus;

		bus->usb.priv = ctx;

		if (!bus->usb.max_bulk_size)
			bus->usb.max_bulk_size = 512;

		bus->ops.ctrl_tx        = itedtv_usb_ctrl_tx;
		bus->ops.ctrl_rx        = itedtv_usb_ctrl_rx;
		bus->ops.stream_rx      = itedtv_usb_stream_rx;
		bus->ops.start_streaming = itedtv_usb_start_streaming;
		bus->ops.stop_streaming  = itedtv_usb_stop_streaming;

		break;
	}
	default:
		return -EINVAL;
	}

	return 0;
}

int itedtv_bus_term(struct itedtv_bus *bus)
{
	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)bus->usb.priv;

		if (ctx) {
			pthread_mutex_lock(&ctx->lock);
			bool need_stop = ctx->streaming || ctx->worker_running || ctx->stopping;
			pthread_mutex_unlock(&ctx->lock);
			if (need_stop)
				itedtv_usb_stop_streaming(bus);

			/* Free transfer objects and buffers */
			for (uint32_t i = 0; i < ctx->num_works; i++) {
				if (ctx->works[i].transfer)
					libusb_free_transfer(ctx->works[i].transfer);
				free(ctx->works[i].buffer);
			}
			free(ctx->works);

			pthread_cond_destroy(&ctx->stopped);
			pthread_mutex_destroy(&ctx->lock);
			free(ctx);
		}
		break;
	}
	default:
		break;
	}

	memset(bus, 0, sizeof(*bus));
	return 0;
}
