// usb_history.cpp - 制御転送の履歴。記録側はロックを取らず、動的確保もしない。

#include "usb_history.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "msg.h"

namespace {

/*
 * 1件分の領域。すべてのフィールドを原子変数で持ち、stamp で整合性を確かめる。
 * 書き込み側は stamp を 0 にしてから値を書き、最後に n+1 を書く (n は記録番号)。
 * 読み出し側は stamp が n+1 のときだけ値を採り、読み終えた後も stamp が変わって
 * いないことを確かめる。書き込み中の件は読み出し側が飛ばすため、壊れた値は返らない。
 */
struct Slot {
	/* 同じ modulo slot を複数の USB thread が同時に上書きしない。競合した記録は捨て、
	 * 受信スレッドを待たせない。atomic_flag は標準で lock-free。 */
	std::atomic_flag writer = ATOMIC_FLAG_INIT;
	std::atomic<std::uint64_t> stamp{0};
	std::atomic<std::uint64_t> t_ms{0};
	/* kind(0-7) dir(8-15) request(16-23) data_len(24-31) index(32-47) length(48-63) */
	std::atomic<std::uint64_t> meta{0};
	/* value(0-15) result(16-47, 32bit の符号なし表現) */
	std::atomic<std::uint64_t> fields{0};
	std::atomic<std::uint64_t> data[2]{};
};

Slot g_ring[USB_HISTORY_CAPACITY];
std::atomic<std::uint64_t> g_next{0};
std::atomic<bool> g_enabled{false};

std::uint64_t pack_meta(std::uint8_t kind, std::uint8_t dir, std::uint8_t request,
			std::uint8_t data_len, std::uint16_t index, std::uint16_t length)
{
	return static_cast<std::uint64_t>(kind) |
		(static_cast<std::uint64_t>(dir) << 8) |
		(static_cast<std::uint64_t>(request) << 16) |
		(static_cast<std::uint64_t>(data_len) << 24) |
		(static_cast<std::uint64_t>(index) << 32) |
		(static_cast<std::uint64_t>(length) << 48);
}

} // namespace

extern "C" void usb_history_set_enabled(bool enabled)
{
	g_enabled.store(enabled, std::memory_order_relaxed);
}

extern "C" bool usb_history_is_enabled(void)
{
	return g_enabled.load(std::memory_order_relaxed);
}

extern "C" void usb_history_record(enum usb_history_kind kind, enum usb_history_dir dir,
				   std::uint8_t request, std::uint16_t value, std::uint16_t index,
				   std::uint16_t length, int result, const void *data, size_t data_len)
{
	if (!g_enabled.load(std::memory_order_relaxed))
		return;

	const std::uint64_t n = g_next.fetch_add(1, std::memory_order_relaxed);
	Slot &slot = g_ring[n % USB_HISTORY_CAPACITY];
	if (slot.writer.test_and_set(std::memory_order_acquire))
		return;
	/*
	 * ticket の発行から slot 確保までに止まった書き手が、同じ slot を先に
	 * 使った新しい記録を上書きしないよう、新しい stamp があれば捨てる。
	 */
	if (slot.stamp.load(std::memory_order_acquire) > n + 1) {
		slot.writer.clear(std::memory_order_release);
		return;
	}

	const std::uint8_t copy_len = static_cast<std::uint8_t>(
		data && data_len ? (data_len < USB_HISTORY_DATA_BYTES ? data_len : USB_HISTORY_DATA_BYTES) : 0);
	std::uint8_t bytes[USB_HISTORY_DATA_BYTES] = {};
	if (copy_len)
		std::memcpy(bytes, data, copy_len);

	slot.stamp.store(0, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);

	slot.t_ms.store(msg_now_ms(), std::memory_order_relaxed);
	slot.meta.store(pack_meta(static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(dir),
				  request, copy_len, index, length),
			std::memory_order_relaxed);
	slot.fields.store(static_cast<std::uint64_t>(value) |
			  (static_cast<std::uint64_t>(static_cast<std::uint32_t>(result)) << 16),
			  std::memory_order_relaxed);
	for (int word = 0; word < 2; word++) {
		std::uint64_t packed = 0;
		for (int i = 0; i < 8; i++)
			packed |= static_cast<std::uint64_t>(bytes[word * 8 + i]) << (8 * i);
		slot.data[word].store(packed, std::memory_order_relaxed);
	}

	slot.stamp.store(n + 1, std::memory_order_release);
	slot.writer.clear(std::memory_order_release);
}

extern "C" size_t usb_history_copy(struct usb_history_entry *out, size_t max)
{
	const std::uint64_t next = g_next.load(std::memory_order_acquire);
	const std::uint64_t first = next > USB_HISTORY_CAPACITY ? next - USB_HISTORY_CAPACITY : 0;
	size_t count = 0;

	if (!out)
		return 0;

	for (std::uint64_t n = first; n < next && count < max; n++) {
		Slot &slot = g_ring[n % USB_HISTORY_CAPACITY];
		if (slot.stamp.load(std::memory_order_acquire) != n + 1)
			continue;

		const std::uint64_t t_ms = slot.t_ms.load(std::memory_order_relaxed);
		const std::uint64_t meta = slot.meta.load(std::memory_order_relaxed);
		const std::uint64_t fields = slot.fields.load(std::memory_order_relaxed);
		std::uint8_t bytes[USB_HISTORY_DATA_BYTES];
		for (int word = 0; word < 2; word++) {
			const std::uint64_t packed = slot.data[word].load(std::memory_order_relaxed);
			for (int i = 0; i < 8; i++)
				bytes[word * 8 + i] = static_cast<std::uint8_t>(packed >> (8 * i));
		}

		std::atomic_thread_fence(std::memory_order_acquire);
		if (slot.stamp.load(std::memory_order_relaxed) != n + 1)
			continue;

		struct usb_history_entry &e = out[count++];
		e.t_ms = t_ms;
		e.kind = static_cast<std::uint8_t>(meta & 0xff);
		e.dir = static_cast<std::uint8_t>((meta >> 8) & 0xff);
		e.request = static_cast<std::uint8_t>((meta >> 16) & 0xff);
		e.data_len = static_cast<std::uint8_t>((meta >> 24) & 0xff);
		e.index = static_cast<std::uint16_t>((meta >> 32) & 0xffff);
		e.length = static_cast<std::uint16_t>((meta >> 48) & 0xffff);
		e.value = static_cast<std::uint16_t>(fields & 0xffff);
		e.result = static_cast<std::int32_t>(static_cast<std::uint32_t>((fields >> 16) & 0xffffffffu));
		std::memcpy(e.data, bytes, sizeof(e.data));
	}

	return count;
}

extern "C" size_t usb_history_dump(void)
{
	struct usb_history_entry entries[USB_HISTORY_CAPACITY];
	size_t count;
	size_t i;

	if (!px4_log_enabled(PX4_LOG_INFO, PX4_LOG_CAT_USB))
		return 0;

	if (!usb_history_is_enabled()) {
		return 0;
	}

	count = usb_history_copy(entries, USB_HISTORY_CAPACITY);
	msg_log_emit("usb history", PX4_LOG_INFO, PX4_LOG_CAT_USB,
		     "%zu entries, oldest first\n", count);

	for (i = 0; i < count; i++) {
		const struct usb_history_entry &e = entries[i];
		char hex[USB_HISTORY_DATA_BYTES * 3 + 1];
		size_t j;
		size_t pos = 0;

		for (j = 0; j < e.data_len && j < USB_HISTORY_DATA_BYTES; j++) {
			const int n = snprintf(hex + pos, sizeof(hex) - pos, "%02x ", e.data[j]);
			if (n < 0 || static_cast<size_t>(n) >= sizeof(hex) - pos)
				break;
			pos += static_cast<size_t>(n);
		}
		hex[pos] = '\0';

		msg_log_emit("usb history", PX4_LOG_INFO, PX4_LOG_CAT_USB,
			     "#%zu t=%llums %s %s req=0x%02x val=0x%04x idx=0x%04x len=%u res=%d data=%s\n",
			     i, static_cast<unsigned long long>(e.t_ms),
			     e.kind == USB_HISTORY_BULK ? "bulk" : "ctrl",
			     e.dir == USB_HISTORY_IN ? "in" : "out",
			     e.request, e.value, e.index, e.length, e.result, hex);
	}

	return count;
}
