// itedtv_bus_libusb.c - libusb transport layer for macOS (replaces itedtv_bus_winusb.c)

#include "misc_posix.h"
#include "libusb_compat.h"

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "itedtv_bus.h"

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
	struct itedtv_bus *bus;
	itedtv_bus_stream_handler_t stream_handler;
	void *handler_ctx;
	uint32_t num_urb;
	uint32_t num_works;
	struct itedtv_usb_work *works;
	volatile int streaming;
	pthread_t worker_thread;
	bool worker_running;
};

// ---------------------------------------------------------------------------
// Control transfer helpers
// ---------------------------------------------------------------------------

static int itedtv_usb_ctrl_tx(struct itedtv_bus *bus, void *buf, int len)
{
	libusb_device_handle *handle = bus->usb.dev->handle;
	int transferred = 0;
	int ret;

	if (!buf || !len)
		return -EINVAL;

	/* Endpoint 0x02: Host->Device bulk for device control */
	ret = libusb_bulk_transfer(handle, 0x02,
	                           (unsigned char *)buf, len,
	                           &transferred,
	                           bus->usb.ctrl_timeout);
	if (ret) {
		dev_err(bus->dev, "itedtv_usb_ctrl_tx: libusb_bulk_transfer() failed (%s)\n",
		        libusb_strerror(ret));
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

	if (!buf || !len || !*len)
		return -EINVAL;

	/* Endpoint 0x81: Device->Host bulk for device control */
	ret = libusb_bulk_transfer(handle, 0x81,
	                           (unsigned char *)buf, *len,
	                           &transferred,
	                           bus->usb.ctrl_timeout);
	if (ret) {
		dev_err(bus->dev, "itedtv_usb_ctrl_rx: libusb_bulk_transfer() failed (%s)\n",
		        libusb_strerror(ret));
		*len = -1;
		return -EIO;
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

	if (!buf || !len || !*len)
		return -EINVAL;

	/* Endpoint 0x84: Device->Host bulk for TS data */
	ret = libusb_bulk_transfer(handle, 0x84,
	                           (unsigned char *)buf, *len,
	                           &transferred, timeout);
	*len = transferred;

	if (ret && ret != LIBUSB_ERROR_TIMEOUT)
		return -EIO;

	return (ret == LIBUSB_ERROR_TIMEOUT) ? -ETIMEDOUT : 0;
}

// ---------------------------------------------------------------------------
// Streaming (async transfers using libusb async API)
// ---------------------------------------------------------------------------

static void LIBUSB_CALL itedtv_usb_transfer_callback(struct libusb_transfer *transfer)
{
	struct itedtv_usb_work *work = (struct itedtv_usb_work *)transfer->user_data;
	struct itedtv_usb_context *ctx = work->ctx;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->actual_length > 0 && ctx->stream_handler && ctx->streaming)
			ctx->stream_handler(ctx->handler_ctx, transfer->buffer,
			                    (uint32_t)transfer->actual_length);
	}

	work->submitted = false;

	if (ctx->streaming) {
		int ret = libusb_submit_transfer(transfer);
		if (ret == 0)
			work->submitted = true;
	}
}

static void *itedtv_usb_worker(void *arg)
{
	struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)arg;
	struct timeval tv = { 0, 100000 }; /* 100 ms */

	while (ctx->streaming) {
		libusb_handle_events_timeout(libusb_get_device(ctx->bus->usb.dev->handle)
		                                 ? NULL : NULL,
		                             &tv);
		/* Use default context */
		struct timeval tv2 = { 0, 100000 };
		libusb_handle_events_timeout(NULL, &tv2);
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

	ctx->stream_handler = stream_handler;
	ctx->handler_ctx    = context;
	buf_size = bus->usb.streaming.urb_buffer_size;
	num      = bus->usb.streaming.urb_num;

	if (num > 64)
		num = 64;

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
			pthread_mutex_unlock(&ctx->lock);
			return -ENOMEM;
		}
	}

	/* Allocate transfer buffers and libusb_transfer objects */
	uint32_t i;
	for (i = 0; i < num; i++) {
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
		libusb_fill_bulk_transfer(ctx->works[i].transfer, handle, 0x84,
		                          (unsigned char *)ctx->works[i].buffer,
		                          (int)buf_size,
		                          itedtv_usb_transfer_callback,
		                          &ctx->works[i], 0);
	}

	ctx->num_urb = i;

	if (!i) {
		pthread_mutex_unlock(&ctx->lock);
		return -ENOMEM;
	}

	/* Clear the endpoint */
	libusb_clear_halt(handle, 0x84);
	ctx->streaming = 1;

	/* Submit all transfers */
	for (i = 0; i < ctx->num_urb; i++) {
		int ret = libusb_submit_transfer(ctx->works[i].transfer);
		if (ret) {
			dev_err(bus->dev,
			        "itedtv_usb_start_streaming: libusb_submit_transfer() failed (i=%u, %s)\n",
			        i, libusb_strerror(ret));
			break;
		}
		ctx->works[i].submitted = true;
	}

	/* Start the event handler thread */
	if (pthread_create(&ctx->worker_thread, NULL, itedtv_usb_worker, ctx)) {
		ctx->streaming = 0;
		pthread_mutex_unlock(&ctx->lock);
		return -ECHILD;
	}

	ctx->worker_running = true;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming: num=%u\n", ctx->num_urb);

	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int itedtv_usb_stop_streaming(struct itedtv_bus *bus)
{
	struct itedtv_usb_context *ctx = (struct itedtv_usb_context *)bus->usb.priv;
	libusb_device_handle *handle = bus->usb.dev->handle;

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming\n");

	pthread_mutex_lock(&ctx->lock);

	ctx->streaming = 0;

	/* Cancel all pending transfers */
	for (uint32_t i = 0; i < ctx->num_urb; i++) {
		if (ctx->works[i].submitted && ctx->works[i].transfer)
			libusb_cancel_transfer(ctx->works[i].transfer);
	}

	pthread_mutex_unlock(&ctx->lock);

	/* Wait for worker thread to exit */
	if (ctx->worker_running) {
		pthread_join(ctx->worker_thread, NULL);
		ctx->worker_running = false;
	}

	/* Drain any remaining events */
	struct timeval tv = { 0, 10000 };
	libusb_handle_events_timeout(NULL, &tv);

	pthread_mutex_lock(&ctx->lock);
	ctx->stream_handler = NULL;
	ctx->handler_ctx    = NULL;
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
			if (ctx->streaming)
				itedtv_usb_stop_streaming(bus);

			/* Free transfer objects and buffers */
			for (uint32_t i = 0; i < ctx->num_works; i++) {
				if (ctx->works[i].transfer)
					libusb_free_transfer(ctx->works[i].transfer);
				free(ctx->works[i].buffer);
			}
			free(ctx->works);

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
