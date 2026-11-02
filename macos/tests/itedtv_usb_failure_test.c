#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include <libusb.h>

#include "itedtv_bus.h"
#include "libusb_compat.h"

/* libusbだけを模擬し、実際のitedtv_bus開始・停止処理を通す。 */
static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;
static struct libusb_transfer *active[64];
static bool cancelled[64];
static int submit_count;
static int fail_at;
static int freed_active;
static int undersized_buffer;
static int bulk_result;
static bool bulk_short;
static bool fail_work_calloc;
static int alloc_count;
static int fail_alloc_at = -1;
static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t handler_cond = PTHREAD_COND_INITIALIZER;
static bool handler_entered, handler_release, cancel_attempted, stop_returned;
static int failure_notifications;
static bool block_failure_notification;
static int restart_in_notification = 1;
static int discard_stream(void *context, void *data, uint32_t length);
static int pending_count(void);

void *itedtv_test_calloc(size_t count, size_t size)
{
	return fail_work_calloc ? NULL : calloc(count, size);
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
	(void)iso_packets;
	if (alloc_count++ == fail_alloc_at)
		return NULL;
	return calloc(1, sizeof(struct libusb_transfer));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i) {
		if (active[i] == transfer) {
			++freed_active;
			active[i] = NULL;
			cancelled[i] = false;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
	pthread_mutex_lock(&mock_lock);
	if (malloc_size(transfer->buffer) < (size_t)transfer->length)
		++undersized_buffer;
	if (submit_count++ == fail_at) {
		pthread_mutex_unlock(&mock_lock);
		return LIBUSB_ERROR_IO;
	}
	for (size_t i = 0; i < 64; ++i) {
		if (!active[i]) {
			active[i] = transfer;
			cancelled[i] = false;
			pthread_mutex_unlock(&mock_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	return LIBUSB_ERROR_NO_MEM;
}

static bool run_resize(void)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	bus.usb.streaming.urb_buffer_size = 188 * 4;
	bus.usb.streaming.urb_num = 1;
	fail_at = -1;
	submit_count = 0;
	freed_active = 0;
	undersized_buffer = 0;
	if (itedtv_bus_init(&bus))
		return false;
	int ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
	if (!ret)
		ret = itedtv_bus_stop_streaming(&bus);
	bus.usb.streaming.urb_buffer_size = 188 * 64;
	if (!ret)
		ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
	if (!ret)
		ret = itedtv_bus_stop_streaming(&bus);
	itedtv_bus_term(&bus);
	if (ret || undersized_buffer || freed_active || pending_count()) {
		fprintf(stderr, "buffer resize: ret=%d undersized=%d freed-active=%d\n",
			ret, undersized_buffer, freed_active);
		return false;
	}
	return true;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
	pthread_mutex_lock(&handler_lock);
	cancel_attempted = true;
	pthread_cond_broadcast(&handler_cond);
	pthread_mutex_unlock(&handler_lock);
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i) {
		if (active[i] == transfer) {
			cancelled[i] = true;
			pthread_mutex_unlock(&mock_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	return LIBUSB_ERROR_NOT_FOUND;
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
	struct libusb_transfer *transfer = NULL;
	(void)ctx;
	(void)tv;
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i) {
		if (active[i] && cancelled[i]) {
			transfer = active[i];
			active[i] = NULL;
			cancelled[i] = false;
			break;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	if (transfer) {
		transfer->status = LIBUSB_TRANSFER_CANCELLED;
		transfer->callback(transfer);
	} else {
		usleep(1000);
	}
	return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
		unsigned char *data, int length, int *transferred, unsigned int timeout)
{
	(void)handle; (void)endpoint; (void)data; (void)timeout;
	*transferred = bulk_short ? length - 1 : length;
	return bulk_result;
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint)
{
	(void)handle; (void)endpoint;
	return 0;
}

libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *handle)
{
	(void)handle;
	return (libusb_device *)(uintptr_t)1;
}

const char *LIBUSB_CALL libusb_strerror(enum libusb_error code)
{
	(void)code;
	return "injected I/O failure";
}

static int discard_stream(void *context, void *data, uint32_t length)
{
	(void)context; (void)data; (void)length;
	return 0;
}

static int pending_count(void)
{
	int count = 0;
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i)
		count += active[i] != NULL;
	pthread_mutex_unlock(&mock_lock);
	return count;
}

static bool run_control_failures(void)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	unsigned char data[8] = {0};
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	if (itedtv_bus_init(&bus))
		return false;
	bulk_result = 0;
	bulk_short = true;
	int short_ret = itedtv_bus_ctrl_tx(&bus, data, sizeof(data));
	bulk_short = false;
	bulk_result = LIBUSB_ERROR_TIMEOUT;
	int timeout_ret = itedtv_bus_ctrl_tx(&bus, data, sizeof(data));
	bulk_result = LIBUSB_ERROR_NO_DEVICE;
	int unplug_ret = itedtv_bus_ctrl_tx(&bus, data, sizeof(data));
	int length = sizeof(data);
	int rx_ret = itedtv_bus_ctrl_rx(&bus, data, &length);
	length = sizeof(data);
	int stream_ret = itedtv_bus_stream_rx(&bus, data, &length, 100);
	bulk_result = 0;
	itedtv_bus_term(&bus);
	if (short_ret != -EIO || timeout_ret != -ETIMEDOUT || unplug_ret != -ENODEV ||
	    rx_ret != -ENODEV || stream_ret != -ENODEV) {
		fprintf(stderr, "transfer errors: short=%d timeout=%d tx=%d rx=%d stream=%d\n",
			short_ret, timeout_ret, unplug_ret, rx_ret, stream_ret);
		return false;
	}
	return true;
}

static bool run_partial_allocation_failure(void)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	bus.usb.streaming.urb_buffer_size = 188 * 4;
	bus.usb.streaming.urb_num = 4;
	alloc_count = 0;
	fail_alloc_at = 1;
	fail_at = -1;
	if (itedtv_bus_init(&bus))
		return false;
	int ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
	if (!ret)
		itedtv_bus_stop_streaming(&bus);
	bool ok = ret == -ENOMEM && pending_count() == 0;
	fail_alloc_at = -1;
	if (ok) {
		ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
		ok = !ret && !itedtv_bus_stop_streaming(&bus) && !pending_count();
	}
	itedtv_bus_term(&bus);
	if (!ok)
		fprintf(stderr, "partial transfer allocation: ret=%d\n", ret);
	return ok;
}

static bool run_work_array_allocation_failure(void)
{
	pid_t child = fork();
	if (child < 0)
		return false;
	if (!child) {
		struct rlimit no_core = {0, 0};
		setrlimit(RLIMIT_CORE, &no_core);
		struct device device = {0};
		struct usb_device usb = {0};
		struct itedtv_bus bus = {0};
		usb.handle = (libusb_device_handle *)(uintptr_t)1;
		bus.dev = &device;
		bus.type = ITEDTV_BUS_USB;
		bus.usb.dev = &usb;
		bus.usb.streaming.urb_buffer_size = 188 * 4;
		bus.usb.streaming.urb_num = 4;
		if (itedtv_bus_init(&bus))
			_exit(2);
		fail_work_calloc = true;
		int ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
		fail_work_calloc = false;
		itedtv_bus_term(&bus);
		_exit(ret == -ENOMEM ? 0 : 1);
	}
	int status = 0;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "work array allocation: teardown failed\n");
		return false;
	}
	return true;
}

static bool run_stall_completion(void)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	bus.usb.streaming.urb_buffer_size = 188 * 4;
	bus.usb.streaming.urb_num = 1;
	fail_at = -1;
	fail_alloc_at = -1;
	submit_count = 0;
	if (itedtv_bus_init(&bus) || itedtv_bus_start_streaming(&bus, discard_stream, NULL))
		return false;
	pthread_mutex_lock(&mock_lock);
	struct libusb_transfer *transfer = active[0];
	active[0] = NULL;
	pthread_mutex_unlock(&mock_lock);
	if (!transfer)
		return false;
	transfer->status = LIBUSB_TRANSFER_STALL;
	transfer->callback(transfer);
	bool ok = submit_count == 1;
	itedtv_bus_stop_streaming(&bus);
	itedtv_bus_term(&bus);
	if (!ok)
		fprintf(stderr, "STALL completion was resubmitted\n");
	return ok;
}

static int blocking_stream(void *context, void *data, uint32_t length)
{
	(void)context; (void)data; (void)length;
	pthread_mutex_lock(&handler_lock);
	handler_entered = true;
	pthread_cond_broadcast(&handler_cond);
	while (!handler_release)
		pthread_cond_wait(&handler_cond, &handler_lock);
	pthread_mutex_unlock(&handler_lock);
	return 0;
}

static void *complete_in_another_event_thread(void *unused)
{
	struct libusb_transfer *transfer = NULL;
	(void)unused;
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i) {
		if (active[i]) {
			transfer = active[i];
			active[i] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	if (transfer) {
		transfer->status = LIBUSB_TRANSFER_COMPLETED;
		transfer->actual_length = 188;
		transfer->callback(transfer);
	}
	return NULL;
}

static void *stop_in_another_thread(void *arg)
{
	struct itedtv_bus *bus = arg;
	itedtv_bus_stop_streaming(bus);
	pthread_mutex_lock(&handler_lock);
	stop_returned = true;
	pthread_cond_broadcast(&handler_cond);
	pthread_mutex_unlock(&handler_lock);
	return NULL;
}

static bool run_callback_in_flight(void)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	pthread_t completion, stop;
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	bus.usb.streaming.urb_buffer_size = 188 * 4;
	bus.usb.streaming.urb_num = 1;
	fail_at = -1;
	submit_count = 0;
	freed_active = 0;
	handler_entered = handler_release = cancel_attempted = stop_returned = false;
	if (itedtv_bus_init(&bus) || itedtv_bus_start_streaming(&bus, blocking_stream, NULL))
		return false;
	pthread_create(&completion, NULL, complete_in_another_event_thread, NULL);
	pthread_mutex_lock(&handler_lock);
	while (!handler_entered)
		pthread_cond_wait(&handler_cond, &handler_lock);
	pthread_mutex_unlock(&handler_lock);
	pthread_create(&stop, NULL, stop_in_another_thread, &bus);

	pthread_mutex_lock(&handler_lock);
	while (!cancel_attempted)
		pthread_cond_wait(&handler_cond, &handler_lock);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 50000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	while (!stop_returned && pthread_cond_timedwait(&handler_cond, &handler_lock, &deadline) == 0) {}
	bool ok = !stop_returned;
	handler_release = true;
	pthread_cond_broadcast(&handler_cond);
	pthread_mutex_unlock(&handler_lock);
	pthread_join(completion, NULL);
	pthread_join(stop, NULL);
	itedtv_bus_term(&bus);
	if (!ok || freed_active || pending_count()) {
		fprintf(stderr, "stop returned before an in-flight callback completed\n");
		return false;
	}
	return true;
}

/* データを捨て、全転送を失った通知 (buf == NULL) だけを数える。 */
static int failure_stream(void *context, void *data, uint32_t length)
{
	if (data)
		return 0;
	/* ctx のロックを保持したまま通知すると、ここで開始要求が止まる。 */
	if (context)
		restart_in_notification = itedtv_bus_start_streaming(context, failure_stream, context);
	pthread_mutex_lock(&handler_lock);
	if (length)
		failure_notifications = -1000;
	++failure_notifications;
	handler_entered = true;
	pthread_cond_broadcast(&handler_cond);
	while (block_failure_notification && !handler_release)
		pthread_cond_wait(&handler_cond, &handler_lock);
	pthread_mutex_unlock(&handler_lock);
	return 0;
}

static int notifications(void)
{
	pthread_mutex_lock(&handler_lock);
	int count = failure_notifications;
	pthread_mutex_unlock(&handler_lock);
	return count;
}

static struct libusb_transfer *take_active(void)
{
	struct libusb_transfer *transfer = NULL;
	pthread_mutex_lock(&mock_lock);
	for (size_t i = 0; i < 64; ++i) {
		if (active[i]) {
			transfer = active[i];
			active[i] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&mock_lock);
	return transfer;
}

static void complete(struct libusb_transfer *transfer, enum libusb_transfer_status status, int length)
{
	transfer->status = status;
	transfer->actual_length = length;
	transfer->callback(transfer);
}

static void reset_failure_state(void)
{
	pthread_mutex_lock(&handler_lock);
	failure_notifications = 0;
	block_failure_notification = false;
	handler_entered = handler_release = cancel_attempted = stop_returned = false;
	pthread_mutex_unlock(&handler_lock);
	fail_at = -1;
	fail_alloc_at = -1;
	submit_count = 0;
	freed_active = 0;
}

static bool init_bus(struct itedtv_bus *bus, struct device *device, struct usb_device *usb, uint32_t urbs)
{
	memset(bus, 0, sizeof(*bus));
	memset(device, 0, sizeof(*device));
	memset(usb, 0, sizeof(*usb));
	usb->handle = (libusb_device_handle *)(uintptr_t)1;
	bus->dev = device;
	bus->type = ITEDTV_BUS_USB;
	bus->usb.dev = usb;
	bus->usb.streaming.urb_buffer_size = 188 * 4;
	bus->usb.streaming.urb_num = urbs;
	return itedtv_bus_init(bus) == 0;
}

/* 最後の転送を失ったときだけ、1回だけ上位へ知らせる。 */
static bool run_failure_notification(enum libusb_transfer_status status)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	reset_failure_state();
	if (!init_bus(&bus, &device, &usb, 2) || itedtv_bus_start_streaming(&bus, failure_stream, NULL))
		return false;
	struct libusb_transfer *first = take_active();
	struct libusb_transfer *second = take_active();
	bool ok = first && second;
	if (ok) {
		complete(first, status, 0);
		/* 1つ残っていれば受信を続け、接続を切らない。 */
		ok = notifications() == 0;
		complete(second, status, 0);
		ok = ok && notifications() == 1 && submit_count == 2;
	}
	ok = !itedtv_bus_stop_streaming(&bus) && ok && notifications() == 1;
	/* 停止後は通知状態を消し、同じ bus で受信を再開できる。 */
	ok = ok && !itedtv_bus_start_streaming(&bus, failure_stream, NULL) &&
		!itedtv_bus_stop_streaming(&bus) && notifications() == 1 && !pending_count();
	itedtv_bus_term(&bus);
	if (!ok)
		fprintf(stderr, "failure notification (status=%d): notifications=%d submits=%d\n",
			(int)status, notifications(), submit_count);
	return ok;
}

/* 正常完了後の再 submit 失敗も、その転送を失ったものとして扱う。 */
static bool run_resubmit_failure_notification(void)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	reset_failure_state();
	if (!init_bus(&bus, &device, &usb, 1) || itedtv_bus_start_streaming(&bus, failure_stream, NULL))
		return false;
	fail_at = 1;
	struct libusb_transfer *transfer = take_active();
	bool ok = transfer != NULL;
	if (ok) {
		complete(transfer, LIBUSB_TRANSFER_COMPLETED, 188);
		ok = notifications() == 1;
	}
	ok = !itedtv_bus_stop_streaming(&bus) && ok;
	itedtv_bus_term(&bus);
	if (!ok)
		fprintf(stderr, "resubmit failure was not reported: notifications=%d\n", notifications());
	return ok;
}

/* 停止によるキャンセル完了と、停止後に届いた異常完了は通知しない。 */
static bool run_stop_does_not_notify(void)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	reset_failure_state();
	if (!init_bus(&bus, &device, &usb, 4) || itedtv_bus_start_streaming(&bus, failure_stream, NULL))
		return false;
	bool ok = !itedtv_bus_stop_streaming(&bus) && notifications() == 0 && !pending_count();
	itedtv_bus_term(&bus);
	if (!ok)
		fprintf(stderr, "stop reported a stream failure: notifications=%d\n", notifications());
	return ok;
}

static void *fail_last_transfer(void *arg)
{
	complete((struct libusb_transfer *)arg, LIBUSB_TRANSFER_NO_DEVICE, 0);
	return NULL;
}

/*
 * 通知の実行中も停止側は待ち、callback の完了前に資源を解放しない。
 * 通知 handler から開始要求を出し、通知中に ctx のロックを保持していないことも確かめる。
 */
static bool run_failure_notification_in_flight(void)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	pthread_t failure, stop;
	reset_failure_state();
	block_failure_notification = true;
	restart_in_notification = 1;
	if (!init_bus(&bus, &device, &usb, 1) || itedtv_bus_start_streaming(&bus, failure_stream, &bus))
		return false;
	struct libusb_transfer *transfer = take_active();
	if (!transfer)
		return false;
	pthread_create(&failure, NULL, fail_last_transfer, transfer);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	pthread_mutex_lock(&handler_lock);
	while (!handler_entered && pthread_cond_timedwait(&handler_cond, &handler_lock, &deadline) == 0) {}
	bool entered = handler_entered;
	pthread_mutex_unlock(&handler_lock);
	if (!entered) {
		pthread_join(failure, NULL);
		itedtv_bus_stop_streaming(&bus);
		itedtv_bus_term(&bus);
		block_failure_notification = false;
		fprintf(stderr, "failure notification in flight: no notification\n");
		return false;
	}
	pthread_create(&stop, NULL, stop_in_another_thread, &bus);

	pthread_mutex_lock(&handler_lock);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 50000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	while (!stop_returned && pthread_cond_timedwait(&handler_cond, &handler_lock, &deadline) == 0) {}
	bool waited = !stop_returned;
	handler_release = true;
	pthread_cond_broadcast(&handler_cond);
	pthread_mutex_unlock(&handler_lock);
	pthread_join(failure, NULL);
	pthread_join(stop, NULL);
	itedtv_bus_term(&bus);
	block_failure_notification = false;
	/* 受信中の bus への開始要求は、ロックを取得できれば -EBUSY で戻る。 */
	if (restart_in_notification != -EBUSY || !waited || notifications() != 1) {
		fprintf(stderr, "failure notification in flight: restart=%d waited=%d notifications=%d\n",
			restart_in_notification, waited, notifications());
		return false;
	}
	return true;
}

static bool run_case(int failed_submission)
{
	struct device device = {0};
	struct usb_device usb = {0};
	struct itedtv_bus bus = {0};
	usb.handle = (libusb_device_handle *)(uintptr_t)1;
	bus.dev = &device;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb;
	bus.usb.streaming.urb_buffer_size = 188 * 4;
	bus.usb.streaming.urb_num = 4;
	if (itedtv_bus_init(&bus))
		return false;

	fail_at = failed_submission;
	submit_count = 0;
	freed_active = 0;
	int ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
	if (!ret)
		itedtv_bus_stop_streaming(&bus);
	bool ok = ret < 0 && pending_count() == 0;

	/* 失敗の後も再初期化せずに受信を再開できることを確認する。 */
	fail_at = -1;
	submit_count = 0;
	if (ok) {
		ret = itedtv_bus_start_streaming(&bus, discard_stream, NULL);
		if (ret)
			ok = false;
		else if (itedtv_bus_stop_streaming(&bus))
			ok = false;
		if (pending_count())
			ok = false;
	}
	itedtv_bus_term(&bus);
	if (freed_active || pending_count())
		ok = false;
	if (!ok)
		fprintf(stderr, "submit failure at %d: ret=%d pending=%d freed-active=%d\n",
			failed_submission, ret, pending_count(), freed_active);
	return ok;
}

/* T230C の 0x82 と指定数を投入し、タイムアウトだけ再投入する。 */
static bool run_t230c_stream_failures(void)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	reset_failure_state();
	if (!init_bus(&bus, &device, &usb, 4))
		return false;
	bus.usb.streaming.endpoint = 0x82;
	bus.usb.streaming.fail_on_error = true;
	if (itedtv_bus_start_streaming(&bus, failure_stream, NULL)) {
		itedtv_bus_term(&bus);
		return false;
	}
	bool ok = pending_count() == 4 && submit_count == 4;
	struct libusb_transfer *transfer = take_active();
	ok = ok && transfer && transfer->endpoint == 0x82 && transfer->length == 188 * 4;
	if (transfer)
		complete(transfer, LIBUSB_TRANSFER_TIMED_OUT, 0);
	ok = ok && notifications() == 0 && pending_count() == 4 && submit_count == 5;
	transfer = take_active();
	if (transfer)
		complete(transfer, LIBUSB_TRANSFER_NO_DEVICE, 0);
	else
		ok = false;
	ok = ok && notifications() == 1;
	ok = !itedtv_bus_stop_streaming(&bus) && ok && pending_count() == 0;
	itedtv_bus_term(&bus);
	return ok && freed_active == 0;
}

/* 受信処理側の失敗でも転送を再投入せず、異常を1回だけ通知する。 */
static int reject_stream(void *context, void *data, uint32_t length)
{
	if (data)
		return -ENOMEM;
	return failure_stream(context, data, length);
}

static bool run_t230c_handler_failure(void)
{
	struct device device;
	struct usb_device usb;
	struct itedtv_bus bus;
	reset_failure_state();
	if (!init_bus(&bus, &device, &usb, 2))
		return false;
	bus.usb.streaming.endpoint = 0x82;
	bus.usb.streaming.fail_on_error = true;
	if (itedtv_bus_start_streaming(&bus, reject_stream, NULL)) {
		itedtv_bus_term(&bus);
		return false;
	}
	struct libusb_transfer *transfer = take_active();
	bool ok = transfer != NULL;
	if (transfer)
		complete(transfer, LIBUSB_TRANSFER_COMPLETED, 188);
	ok = ok && notifications() == 1 && submit_count == 2;
	ok = !itedtv_bus_stop_streaming(&bus) && ok && pending_count() == 0;
	itedtv_bus_term(&bus);
	return ok && freed_active == 0;
}

int main(void)
{
	bool first = run_case(0);
	bool later = run_case(3);
	bool resize = run_resize();
	bool callback = run_callback_in_flight();
	bool control = run_control_failures();
	bool allocation = run_partial_allocation_failure();
	bool oom = run_work_array_allocation_failure();
	bool stall = run_stall_completion();
	bool notify_stall = run_failure_notification(LIBUSB_TRANSFER_STALL);
	bool notify_unplug = run_failure_notification(LIBUSB_TRANSFER_NO_DEVICE);
	bool notify_resubmit = run_resubmit_failure_notification();
	bool stop_quiet = run_stop_does_not_notify();
	bool notify_in_flight = run_failure_notification_in_flight();
	bool t230c_failures = run_t230c_stream_failures();
	bool t230c_handler_failure = run_t230c_handler_failure();
	bool ok = first && later && resize && callback && control && allocation && oom && stall &&
		notify_stall && notify_unplug && notify_resubmit && stop_quiet && notify_in_flight && t230c_failures && t230c_handler_failure;
	if (ok)
		puts("itedtv_usb_failure_test: passed");
	return ok ? 0 : 1;
}
