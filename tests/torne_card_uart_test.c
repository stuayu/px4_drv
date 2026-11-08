/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_card.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CARD_LINES 0xfea004
#define CARD_ENABLE 0xfea000
#define CARD_BAUD 0xfea008
#define CARD_SEND 0xfea00c
#define CARD_CONTROL 0xfea010
#define CARD_FIFO 0xfea014
#define CARD_IRQ 0xfea01c
#define CARD_CWT 0xfea020
#define CARD_BWT 0xfea024
#define CARD_TX 0xfea028
#define CARD_RX 0xfea02c
#define CARD_POWER 0xfea034
#define CARD_SENT_LRC 0xfea038
#define CARD_RECV_LRC 0xfea03c

struct mock_uart {
	u32 irq;
	u32 registers[16];
	u8 rx[512];
	size_t rx_length;
	size_t rx_offset;
	u8 tx[512];
	size_t tx_length;
	u32 sent;
	u64 now_ms;
	unsigned int lock_depth;
	unsigned int lock_count;
	unsigned int unlock_count;
	unsigned int accesses_in_lock;
	unsigned int max_accesses_in_lock;
	unsigned int sleep_ms[8];
	unsigned int sleep_count;
	unsigned int transfer_delay_ms;
	unsigned int write_timeouts[32];
	unsigned int write_timeout_count;
	bool slept_while_locked;
	u32 fail_address;
	int fail_error;
};

static int mock_read(void *context, u32 address, u32 *value, unsigned int timeout_ms)
{
	struct mock_uart *mock = context;
	if (mock->transfer_delay_ms > timeout_ms) {
		mock->now_ms += timeout_ms;
		return -ETIMEDOUT;
	}
	mock->now_ms += mock->transfer_delay_ms;
	if (address == mock->fail_address && mock->fail_error)
		return mock->fail_error;
	if (mock->lock_depth && ++mock->accesses_in_lock > mock->max_accesses_in_lock)
		mock->max_accesses_in_lock = mock->accesses_in_lock;
	if (address == CARD_IRQ) {
		*value = mock->irq;
		return 0;
	}
	if (address == CARD_FIFO) {
		*value = (u32)(mock->rx_length - mock->rx_offset) << 16;
		return 0;
	}
	if (address == CARD_RX) {
		if (mock->rx_offset >= mock->rx_length)
			return -EIO;
		*value = mock->rx[mock->rx_offset++];
		return 0;
	}
	*value = mock->registers[(address - CARD_ENABLE) / 4];
	return 0;
}

static int mock_write(void *context, u32 address, u32 value, unsigned int timeout_ms)
{
	struct mock_uart *mock = context;
	if (mock->write_timeout_count < sizeof(mock->write_timeouts) / sizeof(mock->write_timeouts[0]))
		mock->write_timeouts[mock->write_timeout_count++] = timeout_ms;
	if (mock->transfer_delay_ms > timeout_ms) {
		mock->now_ms += timeout_ms;
		return -ETIMEDOUT;
	}
	mock->now_ms += mock->transfer_delay_ms;
	if (address == mock->fail_address && mock->fail_error)
		return mock->fail_error;
	if (mock->lock_depth && ++mock->accesses_in_lock > mock->max_accesses_in_lock)
		mock->max_accesses_in_lock = mock->accesses_in_lock;
	if (address == CARD_TX) {
		if (mock->tx_length == sizeof(mock->tx))
			return -ENOSPC;
		mock->tx[mock->tx_length++] = (u8)value;
		return 0;
	}
	if (address == CARD_SEND) {
		mock->sent = value;
		return 0;
	}
	if (address == CARD_FIFO && value == 0) {
		mock->rx_length = 0;
		mock->rx_offset = 0;
	}
	if (address >= CARD_ENABLE && address <= CARD_RECV_LRC)
		mock->registers[(address - CARD_ENABLE) / 4] = value;
	return 0;
}

static int mock_lock(void *context)
{
	struct mock_uart *mock = context;
	mock->lock_depth++;
	mock->lock_count++;
	mock->accesses_in_lock = 0;
	return 0;
}

static void mock_unlock(void *context)
{
	struct mock_uart *mock = context;
	if (mock->lock_depth)
		mock->lock_depth--;
	mock->unlock_count++;
}

static void mock_sleep(void *context, unsigned int milliseconds)
{
	struct mock_uart *mock = context;
	mock->now_ms += milliseconds;
	if (mock->lock_depth)
		mock->slept_while_locked = true;
	if (mock->sleep_count < sizeof(mock->sleep_ms) / sizeof(mock->sleep_ms[0]))
		mock->sleep_ms[mock->sleep_count++] = milliseconds;
}

static u64 mock_now_ms(void *context)
{
	return ((struct mock_uart *)context)->now_ms;
}

static const struct torne_card_io_ops mock_ops = {
	.read32 = mock_read,
	.write32 = mock_write,
	.lock = mock_lock,
	.unlock = mock_unlock,
	.sleep_ms = mock_sleep,
	.now_ms = mock_now_ms,
};

static int check(bool condition, const char *message)
{
	if (condition)
		return 1;
	fprintf(stderr, "%s\n", message);
	return 0;
}

int main(void)
{
	struct mock_uart mock = { 0 };
	struct torne_card_port port;
	bool present = true, ready = false;
	u8 data[4] = { 0 }, length;
	const u8 tx[] = { 0x00, 0x01, 0x02 };
	int ok = 1;

	torne_card_port_init(&port, &mock_ops, &mock);
	mock.irq = 1u << 10;
	ok &= check(torne_card_detect(&port, &present) == 0 && !present,
		"CARD_IRQ removal state was not reported.");
	mock.irq = 0;
	ok &= check(torne_card_detect(&port, &present) == 0 && present,
		"CARD_IRQ insertion state was not reported.");

	ok &= check(torne_card_reset(&port) == 0 &&
		mock.registers[(CARD_POWER - CARD_ENABLE) / 4] == 1 &&
		mock.registers[(CARD_ENABLE - CARD_ENABLE) / 4] == 7 &&
		mock.registers[(CARD_BAUD - CARD_ENABLE) / 4] == 0xba7f &&
		(mock.registers[(CARD_LINES - CARD_ENABLE) / 4] & 0x0e) == 0x0e &&
		mock.sleep_count == 3 && mock.sleep_ms[0] == 10 &&
		mock.sleep_ms[1] == 2 && mock.sleep_ms[2] == 1 &&
		!mock.slept_while_locked,
		"Reset did not initialize the UART or slept while holding the I/O lock.");

	ok &= check(torne_card_set_baudrate(&port, 38400) == 0 &&
		mock.registers[(CARD_BAUD - CARD_ENABLE) / 4] == 0x2eae &&
		mock.registers[(CARD_CONTROL - CARD_ENABLE) / 4] == 0x10000 &&
		mock.registers[(CARD_CWT - CARD_ENABLE) / 4] == 107 &&
		mock.registers[(CARD_BWT - CARD_ENABLE) / 4] == 0xffff,
		"38400 baud UART parameters were not applied.");
	ok &= check(torne_card_set_baudrate(&port, 115200) == -EINVAL,
		"An unsupported UART baud rate was accepted.");

	mock.rx[0] = 0x90;
	mock.rx[1] = 0x00;
	mock.rx[2] = 0x01;
	mock.rx_length = 3;
	ok &= check(torne_card_rx_ready(&port, &ready) == 0 && ready,
		"UART FIFO readiness was not reported.");
	length = 2;
	ok &= check(torne_card_read(&port, data, &length) == 0 && length == 2 &&
		data[0] == 0x90 && data[1] == 0x00,
		"UART FIFO bytes were not read up to the caller's capacity.");
	length = 1;
	ok &= check(torne_card_read(&port, data, &length) == 0 && length == 1 &&
		data[0] == 0x01,
		"UART FIFO remainder was not preserved.");

	ok &= check(torne_card_write(&port, tx, sizeof(tx)) == 0 &&
		mock.tx_length == sizeof(tx) && memcmp(mock.tx, tx, sizeof(tx)) == 0 &&
		mock.sent == 1 &&
		mock.registers[(CARD_CONTROL - CARD_ENABLE) / 4] == 0x200 &&
		mock.registers[(CARD_SENT_LRC - CARD_ENABLE) / 4] == 0 &&
		mock.registers[(CARD_RECV_LRC - CARD_ENABLE) / 4] == 0,
		"UART bytes were not sent as a complete card frame.");

	mock.write_timeout_count = 0;
	mock.transfer_delay_ms = 250;
	ok &= check(torne_card_port_set_timeout(&port, 700) == 0 &&
		torne_card_write(&port, tx, sizeof(tx)) == -ETIMEDOUT &&
		mock.write_timeout_count == 3 && mock.write_timeouts[0] == 700 &&
		mock.write_timeouts[1] == 450 && mock.write_timeouts[2] == 200,
		"The UART frame continued after its absolute I/O deadline.");
	mock.transfer_delay_ms = 0;
	(void)torne_card_port_set_timeout(&port, 1000);

	mock.fail_address = CARD_IRQ;
	mock.fail_error = -EIO;
	ok &= check(torne_card_detect(&port, &present) == -EIO,
		"UART register access failure was hidden.");
	mock.fail_address = 0;
	mock.fail_error = 0;
	torne_card_power_down(&port);
	ok &= check(mock.lock_depth == 0 && mock.lock_count == mock.unlock_count &&
		mock.max_accesses_in_lock == 1,
		"UART I/O lock exceeded one register read or write.");

	printf("torne_card_uart_test: %s\n", ok ? "passed" : "failed");
	return ok ? 0 : 1;
}
