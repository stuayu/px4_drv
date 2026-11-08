// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CECH-ZD1 J のカード UART レジスター層。
 * UART のレジスター配置と初期化順は torne_drv の GPL-2.0-only 実装に基づく。
 * ATR と T=1 状態機械は全機種共通の host/core/smart_card.cpp が担当する。
 */
#include "torne_card.h"

#define CARD_ENABLE 0xfea000
#define CARD_LINES 0xfea004
#define CARD_BAUD 0xfea008
#define CARD_SEND 0xfea00c
#define CARD_CONTROL 0xfea010
#define CARD_FIFO 0xfea014
#define CARD_IRQ_MASK 0xfea018
#define CARD_IRQ 0xfea01c
#define CARD_CWT 0xfea020
#define CARD_BWT 0xfea024
#define CARD_TX 0xfea028
#define CARD_RX 0xfea02c
#define CARD_POWER 0xfea034
#define CARD_SENT_LRC 0xfea038
#define CARD_RECV_LRC 0xfea03c
#define CARD_NO_CARD (1u << 10)
#define CARD_IO_DEFAULT_TIMEOUT_MS 1000U

struct card_speed {
	u32 rate;
	u32 baud;
	u32 cwt;
	u32 bwt;
	u32 control;
};

static const struct card_speed card_speeds[] = {
	{ 9600, 0xba7f, 9600, 0, 0 },
	{ 19200, 0x5d5d, 107, 36559, 1u << 16 },
	{ 38400, 0x2eae, 107, 0xffff, 1u << 16 },
};

static int port_valid(const struct torne_card_port *port)
{
	return port && port->ops && port->ops->read32 && port->ops->write32 &&
		port->ops->sleep_ms && port->ops->now_ms &&
		(!!port->ops->lock == !!port->ops->unlock);
}

static int begin_io(struct torne_card_port *port)
{
	return port->ops->lock ? port->ops->lock(port->context) : 0;
}

static void end_io(struct torne_card_port *port)
{
	if (port->ops->unlock)
		port->ops->unlock(port->context);
}

static int set_timeout(struct torne_card_port *port, unsigned int timeout_ms)
{
	if (!port_valid(port) || !timeout_ms)
		return -EINVAL;
	port->deadline_ms = port->ops->now_ms(port->context) + timeout_ms;
	return 0;
}

static int remaining_timeout(struct torne_card_port *port, unsigned int *timeout_ms)
{
	const u64 now = port->ops->now_ms(port->context);
	u64 remaining;

	if (now >= port->deadline_ms)
		return -ETIMEDOUT;
	remaining = port->deadline_ms - now;
	*timeout_ms = remaining > (u64)~0u ? (unsigned int)~0u : (unsigned int)remaining;
	return *timeout_ms ? 0 : -ETIMEDOUT;
}

static int read_reg(struct torne_card_port *port, u32 address, u32 *value)
{
	unsigned int timeout_ms;
	int ret = begin_io(port);

	if (ret)
		return ret;
	ret = remaining_timeout(port, &timeout_ms);
	if (!ret)
		ret = port->ops->read32(port->context, address, value, timeout_ms);
	if (!ret && port->ops->now_ms(port->context) >= port->deadline_ms)
		ret = -ETIMEDOUT;
	end_io(port);
	return ret;
}

static int write_reg(struct torne_card_port *port, u32 address, u32 value)
{
	unsigned int timeout_ms;
	int ret = begin_io(port);

	if (ret)
		return ret;
	ret = remaining_timeout(port, &timeout_ms);
	if (!ret)
		ret = port->ops->write32(port->context, address, value, timeout_ms);
	if (!ret && port->ops->now_ms(port->context) >= port->deadline_ms)
		ret = -ETIMEDOUT;
	end_io(port);
	return ret;
}

static int update_bits(struct torne_card_port *port, u32 address, u32 clear,
		       u32 set)
{
	u32 value;
	int ret = read_reg(port, address, &value);

	if (ret)
		return ret;
	return write_reg(port, address, (value & ~clear) | set);
}

static int remember_error(int first, int next)
{
	return first ? first : next;
}

static int power_down_registers(struct torne_card_port *port)
{
	int ret = 0;

	/* 停止時は後続の書き込みも試し、可能な範囲でカード電源を落とす。 */
	ret = remember_error(ret, update_bits(port, CARD_LINES, 4, 0));
	ret = remember_error(ret, write_reg(port, CARD_FIFO, 0));
	ret = remember_error(ret, update_bits(port, CARD_ENABLE, 4, 0));
	ret = remember_error(ret, update_bits(port, CARD_LINES, 0xa, 0));
	ret = remember_error(ret, write_reg(port, CARD_IRQ_MASK, 1));
	ret = remember_error(ret, write_reg(port, CARD_POWER, 0));
	return ret;
}

void torne_card_port_init(struct torne_card_port *port,
			  const struct torne_card_io_ops *ops, void *context)
{
	if (!port)
		return;
	port->ops = ops;
	port->context = context;
	port->deadline_ms = ops && ops->now_ms ?
		ops->now_ms(context) + CARD_IO_DEFAULT_TIMEOUT_MS : 0;
}

int torne_card_port_set_timeout(struct torne_card_port *port, unsigned int timeout_ms)
{
	return set_timeout(port, timeout_ms);
}

int torne_card_detect(struct torne_card_port *port, bool *present)
{
	u32 irq = 0;
	int ret;

	if (!port_valid(port) || !present)
		return -EINVAL;
	*present = false;
	ret = read_reg(port, CARD_IRQ, &irq);
	if (!ret)
		*present = !(irq & CARD_NO_CARD);
	return ret;
}

int torne_card_reset(struct torne_card_port *port)
{
	int ret;

	if (!port_valid(port))
		return -EINVAL;
	ret = power_down_registers(port);
	if (ret)
		return ret;

	/* UART のリセット時間は USB/制御ロックを保持せずに待つ。 */
	port->ops->sleep_ms(port->context, 10);
	ret = write_reg(port, CARD_IRQ, 0x3ff);
	if (!ret)
		ret = write_reg(port, CARD_POWER, 1);
	if (!ret)
		ret = update_bits(port, CARD_LINES, 4, 0);
	if (!ret)
		ret = write_reg(port, CARD_FIFO, 0);
	if (!ret)
		ret = write_reg(port, CARD_ENABLE, 0);
	if (!ret)
		ret = write_reg(port, CARD_ENABLE, 7);
	if (!ret)
		ret = update_bits(port, CARD_LINES, 0, 8);
	if (!ret)
		ret = write_reg(port, CARD_BAUD, card_speeds[0].baud);
	if (ret)
		return ret;

	port->ops->sleep_ms(port->context, 2);
	ret = update_bits(port, CARD_LINES, 0, 2);
	if (!ret)
		ret = write_reg(port, CARD_RECV_LRC, 0x3b);
	if (!ret)
		ret = write_reg(port, CARD_CWT, card_speeds[0].cwt);
	if (!ret)
		ret = write_reg(port, CARD_BWT, card_speeds[0].bwt);
	if (!ret)
		ret = write_reg(port, CARD_IRQ_MASK, 0x1ff);
	if (ret)
		return ret;

	port->ops->sleep_ms(port->context, 1);
	ret = update_bits(port, CARD_LINES, 0, 4);
	return ret;
}

int torne_card_set_baudrate(struct torne_card_port *port, u32 baudrate)
{
	unsigned int index;
	int ret;

	if (!port_valid(port))
		return -EINVAL;
	for (index = 0; index < sizeof(card_speeds) / sizeof(card_speeds[0]); ++index)
		if (card_speeds[index].rate == baudrate)
			break;
	if (index == sizeof(card_speeds) / sizeof(card_speeds[0]))
		return -EINVAL;

	ret = write_reg(port, CARD_BAUD, card_speeds[index].baud);
	if (!ret)
		ret = write_reg(port, CARD_CONTROL, card_speeds[index].control);
	if (!ret)
		ret = write_reg(port, CARD_CWT, card_speeds[index].cwt);
	if (!ret)
		ret = write_reg(port, CARD_BWT, card_speeds[index].bwt);
	return ret;
}

int torne_card_rx_ready(struct torne_card_port *port, bool *ready)
{
	u32 fifo = 0;
	int ret;

	if (!port_valid(port) || !ready)
		return -EINVAL;
	*ready = false;
	ret = read_reg(port, CARD_FIFO, &fifo);
	if (!ret)
		*ready = (fifo >> 16) != 0;
	return ret;
}

int torne_card_read(struct torne_card_port *port, u8 *data, u8 *length)
{
	u32 fifo = 0;
	unsigned int capacity, count, index;
	int ret;

	if (!port_valid(port) || !data || !length || !*length)
		return -EINVAL;
	capacity = *length;
	*length = 0;
	ret = read_reg(port, CARD_FIFO, &fifo);
	count = (fifo >> 16) < capacity ? fifo >> 16 : capacity;
	for (index = 0; !ret && index < count; ++index) {
		u32 value = 0;
		ret = read_reg(port, CARD_RX, &value);
		if (!ret)
			data[index] = (u8)value;
	}
	if (!ret)
		*length = (u8)count;
	return ret;
}

int torne_card_write(struct torne_card_port *port, const u8 *data, u16 length)
{
	unsigned int index;
	int ret;

	if (!port_valid(port) || !data || !length || length > 255)
		return -EINVAL;
	ret = write_reg(port, CARD_RECV_LRC, 0);
	if (!ret)
		ret = write_reg(port, CARD_SENT_LRC, 0);
	for (index = 0; !ret && index < length; ++index)
		ret = write_reg(port, CARD_TX, data[index]);
	if (!ret)
		ret = write_reg(port, CARD_IRQ, 0x3ff);
	if (!ret)
		ret = write_reg(port, CARD_CONTROL, 0x200);
	if (!ret)
		ret = write_reg(port, CARD_SEND, 1);
	return ret;
}

int torne_card_power_down(struct torne_card_port *port)
{
	int ret;

	if (!port_valid(port))
		return -EINVAL;
	ret = set_timeout(port, CARD_IO_DEFAULT_TIMEOUT_MS);
	if (ret)
		return ret;
	ret = power_down_registers(port);
	return ret;
}
