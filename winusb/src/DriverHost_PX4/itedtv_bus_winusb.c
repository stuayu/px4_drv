// itedev_bus_winusb.c

#if !defined(_WIN32) && !defined(_WIN64)
#error Unsupported platform.
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <process.h>

#include <windows.h>

#include "itedtv_bus.h"
#include "usb_history.h"

struct itedtv_usb_context;

struct itedtv_usb_work {
	struct itedtv_usb_context *ctx;
	OVERLAPPED ol;
	void *buffer;
	size_t size;
	bool can_submit;
};

struct itedtv_usb_context {
	CRITICAL_SECTION lock;
	HANDLE ctrl_event[2];
	struct itedtv_bus *bus;
	itedtv_bus_stream_handler_t stream_handler;
	void *ctx;
	uint32_t num_urb;
	bool no_raw_io;
	uint32_t num_works;
	struct itedtv_usb_work *works;
	LONG streaming;
	HANDLE worker_thread;
	HANDLE startup_event;
	LONG startup_result;
	volatile LONG64 completed_reads;
	volatile LONG64 completed_bytes;
	volatile LONG64 short_reads;
	volatile LONG64 short_bytes;
	volatile LONG64 read_errors;
	volatile LONG64 read_timeouts;
	volatile LONG64 submit_errors;
	volatile LONG64 wait_timeouts;
	volatile LONG64 wait_errors;
	volatile LONG64 max_completion_interval_us;
	volatile LONG last_error;
};

static void itedtv_usb_record_error(struct itedtv_usb_context *ctx,
				    volatile LONG64 *counter, DWORD error)
{
	InterlockedIncrement64(counter);
	InterlockedExchange(&ctx->last_error, (LONG)error);
}

static void itedtv_usb_record_max(volatile LONG64 *maximum, LONG64 value)
{
	LONG64 current = InterlockedCompareExchange64(maximum, 0, 0);
	while (value > current) {
		LONG64 previous = InterlockedCompareExchange64(maximum, value, current);
		if (previous == current)
			break;
		current = previous;
	}
}

static int winerr_to_errno(struct device *dev)
{
	int ret = 0;

	switch (GetLastError()) {
	case ERROR_SUCCESS:
		ret = 0;
		break;

	case ERROR_INVALID_HANDLE:
		ret = EINVAL;
		break;

	case ERROR_IO_PENDING:
	case ERROR_IO_INCOMPLETE:
		ret = ETIMEDOUT;
		break;

	case ERROR_NOT_ENOUGH_MEMORY:
		ret = ENOMEM;
		break;

	case ERROR_SEM_TIMEOUT:
		ret = ETIMEDOUT;
		break;

	case ERROR_BAD_COMMAND:
		ret = EIO;
		break;

	default:
		dev_err(dev, "winusb unknown error: 0x%08x\n", GetLastError());
		ret = EINVAL;
		break;
	}

	return ret;
}

static int itedtv_usb_ctrl_tx(struct itedtv_bus *bus, void *buf, int len)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	ULONG rlen = 0;
	OVERLAPPED ol;

	if (!buf || !len)
		return -EINVAL;

	ol.hEvent = ctx->ctrl_event[0];
	ResetEvent(ol.hEvent);

	/* Endpoint 0x02: Host->Device bulk endpoint for controlling the device */
	if (!WinUsb_WritePipe(dev->winusb, 0x02, buf, len, NULL, &ol)) {
		if (GetLastError() == ERROR_IO_PENDING)
			WaitForSingleObject(ol.hEvent, bus->usb.ctrl_timeout);
		else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_ctrl_tx: WinUsb_WritePipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, &ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
	}
	usb_history_record(USB_HISTORY_BULK, USB_HISTORY_OUT, 0, 0, 0x02,
			   (uint16_t)(len > 0xffff ? 0xffff : len),
			   ret ? ret : (int)rlen, buf, (size_t)len);

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, 0x02);
		usb_history_dump();
	}

	return ret;
}

static int itedtv_usb_ctrl_rx(struct itedtv_bus *bus, void *buf, int *len)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	ULONG rlen = 0;
	OVERLAPPED ol;

	if (!buf || !len || !*len)
		return -EINVAL;

	ol.hEvent = ctx->ctrl_event[1];
	ResetEvent(ol.hEvent);

	/* Endpoint 0x81: Device->Host bulk endpoint for controlling the device */
	if (!WinUsb_ReadPipe(dev->winusb, 0x81, buf, *len, NULL, &ol)) {
		if (GetLastError() == ERROR_IO_PENDING)
			WaitForSingleObject(ol.hEvent, bus->usb.ctrl_timeout);
		else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_ctrl_rx: WinUsb_ReadPipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, &ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
	}
	usb_history_record(USB_HISTORY_BULK, USB_HISTORY_IN, 0, 0, 0x81,
			   (uint16_t)(*len > 0xffff ? 0xffff : *len),
			   ret ? ret : (int)rlen, buf, ret ? 0 : (size_t)rlen);

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, 0x81);
		*len = -1;
		usb_history_dump();
	} else {
		*len = rlen;
	}

	return ret;
}

static int itedtv_usb_stream_rx(struct itedtv_bus *bus, void *buf, int *len, int timeout)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	ULONG rlen = 0;
	OVERLAPPED ol;

	if (!buf || !len || !*len)
		return -EINVAL;

	ol.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!ol.hEvent) {
		*len = 0;
		return -winerr_to_errno(bus->dev);
	}

	/* Device->Host bulk endpoint for receiving TS from the device. */
	if (!WinUsb_ReadPipe(dev->winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), buf, *len, NULL, &ol)) {
		if (GetLastError() == ERROR_IO_PENDING)
			WaitForSingleObject(ol.hEvent, timeout);
		else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_stream_rx: WinUsb_ReadPipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, &ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
	}

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84));
		*len = -1;
	} else {
		*len = rlen;
	}

	CloseHandle(ol.hEvent);
	return ret;
}

static int itedtv_usb_alloc_work_buffers(struct itedtv_usb_context *ctx, uint32_t buf_size)
{
	uint32_t i;
	struct itedtv_bus *bus = ctx->bus;
	struct usb_device *dev = bus->usb.dev;
	uint32_t num = ctx->num_works;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		void *p;

		works[i].ctx = ctx;

		if (!works[i].ol.hEvent) {
			works[i].ol.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (!works[i].ol.hEvent) {
				dev_err(bus->dev, "itedtv_usb_alloc_work_buffers: CreateEventW() failed. (i: %u)\n", i);
				break;
			}
		}

		if (works[i].buffer && works[i].size != buf_size) {
			VirtualFree(works[i].buffer, 0, MEM_RELEASE);
			works[i].buffer = NULL;
		}

		if (!works[i].buffer) {
			p = VirtualAlloc(NULL, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (!p) {
				dev_err(bus->dev, "itedtv_usb_alloc_work_buffers: VirtualAlloc() failed. (i: %u)\n", i);
				CloseHandle(works[i].ol.hEvent);
				works[i].ol.hEvent = NULL;
				break;
			}

			dev_dbg(bus->dev, "itedtv_usb_alloc_work_buffers: p: %p, buf_size: %u\n", p, buf_size);

			works[i].buffer = p;
			works[i].size = buf_size;
		}

		works[i].can_submit = true;
	}

	ctx->num_urb = i;

	if (!i)
		return -ENOMEM;

	return 0;
}

static void itedtv_usb_free_work_buffers(struct itedtv_usb_context *ctx)
{
	uint32_t i;
	struct usb_device *dev = ctx->bus->usb.dev;
	uint32_t num = ctx->num_works;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return;

	for (i = 0; i < num; i++) {
		if (works[i].ol.hEvent) {
			CloseHandle(works[i].ol.hEvent);
			works[i].ol.hEvent = NULL;
		}

		if (works[i].buffer) {
			VirtualFree(works[i].buffer, 0, MEM_RELEASE);
			works[i].buffer = NULL;
		}

		works[i].size = 0;
		works[i].can_submit = false;
	}

	ctx->num_urb = 0;

	return;
}

static void itedtv_usb_clean_context(struct itedtv_usb_context *ctx)
{
	if (ctx->works) {
		itedtv_usb_free_work_buffers(ctx);
		free(ctx->works);
	}

	ctx->stream_handler = NULL;
	ctx->ctx = NULL;
	ctx->num_urb = 0;
	ctx->num_works = 0;
	ctx->works = NULL;
}

unsigned __stdcall itedtv_winusb_worker(void *arg)
{
	struct itedtv_usb_context *ctx = arg;
	struct itedtv_bus *bus = ctx->bus;
	uint32_t i, num, next_idx = 0;
	struct itedtv_usb_work *works;
	WINUSB_INTERFACE_HANDLE winusb;
	UCHAR raw_io;
	int startup_result = 0;
	LARGE_INTEGER qpc_frequency = { 0 };
	LARGE_INTEGER previous_completion = { 0 };
	BOOL have_previous_completion = FALSE;

	dev_dbg(bus->dev, "itedtv_winusb_worker: start\n");

	num = ctx->num_urb;
	works = ctx->works;
	winusb = bus->usb.dev->winusb;
	raw_io = (ctx->no_raw_io) ? 0 : 1;
	if (bus->usb.streaming.measure_stream_timing)
		QueryPerformanceFrequency(&qpc_frequency);

	if (!WinUsb_SetPipePolicy(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), RAW_IO, sizeof(raw_io), &raw_io)) {
		startup_result = -winerr_to_errno(bus->dev);
		dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_SetPipePolicy(RAW_IO, %u) failed.\n", raw_io);
		goto startup_failed;
	}

	for (i = 0; i < num; i++) {
		if (!works[i].can_submit)
			continue;

		works[i].can_submit = false;
		ResetEvent(works[i].ol.hEvent);

		if (WinUsb_ReadPipe(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), works[i].buffer, (ULONG)works[i].size, NULL, &works[i].ol))
			continue;

		if (GetLastError() == ERROR_IO_PENDING)
			continue;

		{
			DWORD error = GetLastError();
			itedtv_usb_record_error(ctx, &ctx->submit_errors, error);
			SetLastError(error);
			startup_result = -winerr_to_errno(bus->dev);
			dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_ReadPipe() 1 failed. (%u, %u)\n", i, error);
		}
		works[i].can_submit = true;

		if (bus->usb.streaming.fail_on_error)
			goto startup_failed;
		num = i;
		break;
	}

	dev_dbg(bus->dev, "itedtv_winusb_worker: num: %u\n", num);
	if (!num)
		goto startup_failed;

	InterlockedExchange(&ctx->startup_result, 0);
	SetEvent(ctx->startup_event);

	while (ctx->streaming) {
		uint32_t idx = next_idx;
		DWORD ret, rlen = 0;
		struct itedtv_usb_work *work = &works[idx];

		if (work->can_submit) {
			dev_dbg(bus->dev, "itedtv_winusb_worker: can_submit %u\n", idx);
			for (i = (idx + 1) % num; i != idx; i = (i + 1) % num) {
				if (!works[i].can_submit)
					break;
			}
			if (i == idx)
				break;

			idx = i;
			work = &works[idx];
		}

		ret = WaitForSingleObject(work->ol.hEvent, 500);
		if (bus->usb.streaming.fail_on_error && !ctx->streaming)
			break;
		if (ret == WAIT_TIMEOUT) {
			InterlockedIncrement64(&ctx->wait_timeouts);
			dev_dbg(bus->dev, "itedtv_winusb_worker: timeout %u\n", idx);
			continue;
		} else if (ret == WAIT_FAILED) {
			itedtv_usb_record_error(ctx, &ctx->wait_errors, GetLastError());
			if (bus->usb.streaming.fail_on_error)
				ctx->stream_handler(ctx->ctx, NULL, 0);
			break;
		}

		if (WinUsb_GetOverlappedResult(winusb, &work->ol, &rlen, TRUE)) {
			LARGE_INTEGER completion_time = { 0 };
			/* worker が完了イベントを処理した時刻の間隔を測る。USB stack 内の完了時刻そのものではない。 */
			if (bus->usb.streaming.measure_stream_timing &&
			    qpc_frequency.QuadPart > 0 && QueryPerformanceCounter(&completion_time)) {
				if (have_previous_completion && completion_time.QuadPart >= previous_completion.QuadPart) {
					const LONG64 interval_us = (LONG64)((completion_time.QuadPart -
						previous_completion.QuadPart) * 1000000LL / qpc_frequency.QuadPart);
					itedtv_usb_record_max(&ctx->max_completion_interval_us, interval_us);
				}
				previous_completion = completion_time;
				have_previous_completion = TRUE;
			}
			InterlockedIncrement64(&ctx->completed_reads);
			InterlockedExchangeAdd64(&ctx->completed_bytes, (LONG64)rlen);
			if ((size_t)rlen < work->size) {
				InterlockedIncrement64(&ctx->short_reads);
				InterlockedExchangeAdd64(&ctx->short_bytes, (LONG64)(work->size - rlen));
			}
			int handler_ret = ctx->stream_handler(ctx->ctx, work->buffer, rlen);
			if (handler_ret && bus->usb.streaming.fail_on_error) {
				work->can_submit = true;
				ctx->stream_handler(ctx->ctx, NULL, 0);
				break;
			}
		} else {
			DWORD error = GetLastError();
			InterlockedIncrement64(&ctx->read_errors);
			InterlockedExchange(&ctx->last_error, (LONG)error);
			if (error == ERROR_SEM_TIMEOUT)
				InterlockedIncrement64(&ctx->read_timeouts);
			if (bus->usb.streaming.fail_on_error && error != ERROR_SEM_TIMEOUT) {
				work->can_submit = true;
				ctx->stream_handler(ctx->ctx, NULL, 0);
				break;
			}
		}

		next_idx = (idx + 1) % num;
		ResetEvent(work->ol.hEvent);

		if (WinUsb_ReadPipe(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), work->buffer, (ULONG)work->size, NULL, &work->ol))
			continue;

		if (GetLastError() == ERROR_IO_PENDING)
			continue;

		{
			DWORD error = GetLastError();
			itedtv_usb_record_error(ctx, &ctx->submit_errors, error);
			dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_ReadPipe() 2 failed. (%u, %u)\n", idx, error);
		}
		work->can_submit = true;
		if (bus->usb.streaming.fail_on_error) {
			ctx->stream_handler(ctx->ctx, NULL, 0);
			break;
		}
	}

	WinUsb_AbortPipe(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84));
	if (bus->usb.streaming.fail_on_error) {
		/* Abort 後も完了を回収してから OVERLAPPED とバッファーを解放する。 */
		for (i = 0; i < ctx->num_urb; i++) {
			ULONG transferred;
			if (!works[i].can_submit)
				WinUsb_GetOverlappedResult(winusb, &works[i].ol, &transferred, TRUE);
		}
	}

	if (raw_io) {
		raw_io = 0;
		WinUsb_SetPipePolicy(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), RAW_IO, sizeof(raw_io), &raw_io);
	}

	goto exit;

startup_failed:
	if (bus->usb.streaming.fail_on_error) {
		WinUsb_AbortPipe(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84));
		for (i = 0; i < ctx->num_urb; i++) {
			ULONG transferred;
			if (!works[i].can_submit)
				WinUsb_GetOverlappedResult(winusb, &works[i].ol, &transferred, TRUE);
		}
	}
	InterlockedExchange(&ctx->streaming, 0);
	InterlockedExchange(&ctx->startup_result, startup_result ? startup_result : -EIO);
	SetEvent(ctx->startup_event);
	if (raw_io) {
		raw_io = 0;
		WinUsb_SetPipePolicy(winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), RAW_IO, sizeof(raw_io), &raw_io);
	}

exit:
	dev_dbg(bus->dev, "itedtv_winusb_worker: exit\n");
	return 0;
}

static int itedtv_usb_start_streaming(struct itedtv_bus *bus, itedtv_bus_stream_handler_t stream_handler, void *context)
{
	int ret = 0;
	u32 buf_size, requested_buf_size, num;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	struct itedtv_usb_work *works;
	UCHAR endpoint = bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84;

	if (!stream_handler)
		return -EINVAL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming\n");

	EnterCriticalSection(&ctx->lock);

	ctx->stream_handler = stream_handler;
	ctx->ctx = context;
	ctx->no_raw_io = bus->usb.streaming.no_raw_io;

	buf_size = bus->usb.streaming.urb_buffer_size;
	requested_buf_size = buf_size;
	num = bus->usb.streaming.urb_num;

	if (!buf_size || !num || num > 64) {
		ret = -EINVAL;
		goto fail;
	}

	if (!ctx->no_raw_io && (buf_size % bus->usb.max_bulk_size))
		buf_size += bus->usb.max_bulk_size - (buf_size % bus->usb.max_bulk_size);

	dev_dbg(bus->dev,
		"WinUSB stream pipe: endpoint=0x%02x RAW_IO=%u urbs=%u requested_bytes=%u transfer_bytes=%u max_packet=%u\n",
		endpoint, ctx->no_raw_io ? 0U : 1U, num, requested_buf_size, buf_size,
		bus->usb.max_bulk_size);

	if (ctx->works && num != ctx->num_works) {
		itedtv_usb_free_work_buffers(ctx);
		free(ctx->works);
		ctx->works = NULL;
	}

	ctx->num_works = num;

	if (!ctx->works) {
		ctx->works = calloc(ctx->num_works, sizeof(*works));
		if (!ctx->works) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	ret = itedtv_usb_alloc_work_buffers(ctx, buf_size);
	if (!ret && bus->usb.streaming.fail_on_error && ctx->num_urb != num)
		ret = -ENOMEM;
	if (ret)
		goto fail;

	ctx->startup_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!ctx->startup_event) {
		ret = -winerr_to_errno(bus->dev);
		goto fail;
	}
	InterlockedExchange(&ctx->startup_result, -EIO);

	if (!WinUsb_ResetPipe(bus->usb.dev->winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84)) &&
	    bus->usb.streaming.fail_on_error) {
		ret = -winerr_to_errno(bus->dev);
		goto fail;
	}
	InterlockedExchange(&ctx->streaming, 1);

	ctx->worker_thread = (HANDLE)_beginthreadex(NULL, 0, itedtv_winusb_worker, ctx, 0, NULL);
	if (!ctx->worker_thread) {
		ret = -EAGAIN;
		dev_err(bus->dev, "itedtv_usb_start_streaming: _beginthreadex() failed.\n");
		goto fail;
	}

	if (!SetThreadPriority(ctx->worker_thread, THREAD_PRIORITY_TIME_CRITICAL))
		dev_dbg(bus->dev, "itedtv_usb_start_streaming: SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL) failed.\n");

	/* 最初の USB 転送を投入できたことを確認してから受信開始を成功として返す */
	{
		DWORD wait_result = WaitForSingleObject(ctx->startup_event, bus->usb.ctrl_timeout);
		if (wait_result == WAIT_OBJECT_0)
			ret = InterlockedCompareExchange(&ctx->startup_result, 0, 0);
		else if (wait_result == WAIT_TIMEOUT)
			ret = -ETIMEDOUT;
		else
			ret = -EIO;
	}
	if (ret)
		goto fail;

	CloseHandle(ctx->startup_event);
	ctx->startup_event = NULL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming: num: %u\n", num);

	LeaveCriticalSection(&ctx->lock);

	return ret;

fail:
	InterlockedExchange(&ctx->streaming, 0);

	if (ctx->worker_thread) {
		WaitForSingleObject(ctx->worker_thread, INFINITE);
		CloseHandle(ctx->worker_thread);
		ctx->worker_thread = NULL;
	}

	if (ctx->startup_event) {
		CloseHandle(ctx->startup_event);
		ctx->startup_event = NULL;
	}

	itedtv_usb_clean_context(ctx);

	LeaveCriticalSection(&ctx->lock);

	return ret;
}

static int itedtv_usb_stop_streaming(struct itedtv_bus *bus)
{
	struct itedtv_usb_context *ctx = bus->usb.priv;

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming\n");

	EnterCriticalSection(&ctx->lock);

	if (!ctx->worker_thread) {
		LeaveCriticalSection(&ctx->lock);
		return 0;
	}
	InterlockedExchange(&ctx->streaming, 0);
	if (bus->usb.streaming.fail_on_error)
		WinUsb_AbortPipe(bus->usb.dev->winusb, bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84);

	WaitForSingleObject(ctx->worker_thread, INFINITE);
	CloseHandle(ctx->worker_thread);
	ctx->worker_thread = NULL;

	itedtv_usb_clean_context(ctx);

	LeaveCriticalSection(&ctx->lock);

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming: exit\n");
	return 0;
}

int itedtv_bus_init(struct itedtv_bus *bus)
{
	int ret = 0;

	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		UCHAR raw_io;
		struct itedtv_usb_context *ctx;

		if (!bus->usb.dev) {
			ret = -EINVAL;
			break;
		}

		if (bus->usb.dev->descriptor.bcdUSB < 0x0110) {
			ret = -EIO;
			break;
		}

		raw_io = bus->usb.streaming.endpoint && bus->usb.streaming.no_raw_io ? 0 : 1;
		if (!WinUsb_SetPipePolicy(bus->usb.dev->winusb, (bus->usb.streaming.endpoint ? bus->usb.streaming.endpoint : 0x84), RAW_IO, sizeof(raw_io), &raw_io)) {
			ret = -winerr_to_errno(bus->dev);
			break;
		}

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}

		InitializeCriticalSection(&ctx->lock);
		for (int i = 0; i < 2; i++) {
			ctx->ctrl_event[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (!ctx->ctrl_event[i]) {
				ret = -winerr_to_errno(bus->dev);
				break;
			}
		}
		if (ret && bus->usb.streaming.fail_on_error) {
			for (int i = 0; i < 2; i++)
				if (ctx->ctrl_event[i])
					CloseHandle(ctx->ctrl_event[i]);
			DeleteCriticalSection(&ctx->lock);
			free(ctx);
			break;
		}
		ctx->bus = bus;
		ctx->stream_handler = NULL;
		ctx->ctx = NULL;
		ctx->num_urb = 0;
		ctx->num_works = 0;
		ctx->works = NULL;
		ctx->streaming = 0;
		ctx->worker_thread = NULL;
		ctx->startup_event = NULL;
		ctx->startup_result = 0;

		bus->usb.priv = ctx;

		if (!bus->usb.max_bulk_size)
			bus->usb.max_bulk_size = (bus->usb.dev->descriptor.bcdUSB == 0x0110) ? 64 : 512;

		bus->ops.ctrl_tx = itedtv_usb_ctrl_tx;
		bus->ops.ctrl_rx = itedtv_usb_ctrl_rx;
		bus->ops.stream_rx = itedtv_usb_stream_rx;
		bus->ops.start_streaming = itedtv_usb_start_streaming;
		bus->ops.stop_streaming = itedtv_usb_stop_streaming;

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int itedtv_bus_term(struct itedtv_bus *bus)
{
	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx = bus->usb.priv;

		if (ctx) {
			itedtv_usb_stop_streaming(bus);
			CloseHandle(ctx->ctrl_event[0]);
			CloseHandle(ctx->ctrl_event[1]);
			DeleteCriticalSection(&ctx->lock);
			free(ctx);
		}

		break;
	}

	default:
		break;
	}

	dev_dbg(bus->dev, "itedtv_bus_term: exit\n");

	memset(bus, 0, sizeof(*bus));

	return 0;
}

int itedtv_bus_take_usb_stream_stats(struct itedtv_bus *bus,
				     struct itedtv_usb_stream_stats *stats)
{
	struct itedtv_usb_context *ctx;

	if (!bus || !stats || bus->type != ITEDTV_BUS_USB || !bus->usb.priv)
		return -EINVAL;
	ctx = bus->usb.priv;
	stats->completed_reads = (uint64_t)InterlockedExchange64(&ctx->completed_reads, 0);
	stats->completed_bytes = (uint64_t)InterlockedExchange64(&ctx->completed_bytes, 0);
	stats->short_reads = (uint64_t)InterlockedExchange64(&ctx->short_reads, 0);
	stats->short_bytes = (uint64_t)InterlockedExchange64(&ctx->short_bytes, 0);
	stats->read_errors = (uint64_t)InterlockedExchange64(&ctx->read_errors, 0);
	stats->read_timeouts = (uint64_t)InterlockedExchange64(&ctx->read_timeouts, 0);
	stats->submit_errors = (uint64_t)InterlockedExchange64(&ctx->submit_errors, 0);
	stats->wait_timeouts = (uint64_t)InterlockedExchange64(&ctx->wait_timeouts, 0);
	stats->wait_errors = (uint64_t)InterlockedExchange64(&ctx->wait_errors, 0);
	stats->max_worker_observed_completion_interval_us =
		(uint64_t)InterlockedExchange64(&ctx->max_completion_interval_us, 0);
	stats->last_error = (uint32_t)InterlockedExchange(&ctx->last_error, 0);
	return 0;
}
