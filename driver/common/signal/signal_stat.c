// SPDX-License-Identifier: GPL-2.0-only
/*
 * 信号品質の換算と誤り率の計算 (signal_stat.c)
 *
 * 内部の固定小数点:
 *   log2(x) は Q24 (2^24 倍) で求める。仮数部 [1, 2) を 2 乗して整数部を 1 bit ずつ取り出す。
 *   log10 と ln は log2 に定数を掛けて求める。定数は 2^32 倍した整数。
 *   多項式は Horner 法で、係数を Q16 の P (dB または無次元の値) へ掛けながら評価する。
 *   除算の丸めは、カーネルで 64 bit の除算を使うため div_s64() / div64_u64() を使う。
 */

#include "signal_stat.h"

#ifdef __linux__
#include <linux/errno.h>
#include <linux/math64.h>
#define SIG_DIV_S64(n, d)	div_s64((n), (d))
#define SIG_DIV_U64(n, d)	div64_u64((n), (d))
#else
#include <errno.h>
#define SIG_DIV_S64(n, d)	((n) / (d))
#define SIG_DIV_U64(n, d)	((n) / (d))
#endif

/* log10(2) * 2^32、ln(2) * 2^32 */
#define SIG_LOG10_2_Q32	1292913986ULL
#define SIG_LN2_Q32	2977044472ULL

/* 1 階層あたりの総ビット数: 204 byte のパケット * 8 bit */
#define SIG_RS_PACKET_BITS	(204ULL * 8ULL)

/* 誤り率の基準 (10^9 = 1 ppb) */
#define SIG_PPB	1000000000ULL

/* log2(x) * 2^24。x >= 1 */
static u64 sig_log2_q24(u32 x)
{
	u32 n = 0;
	u64 m;
	u64 frac = 0;
	int i;

	while (n < 31 && (x >> (n + 1)))
		n++;

	/* 仮数部を Q31 の [1, 2) へ正規化する */
	m = (u64)x << (31 - n);
	for (i = 0; i < 24; i++) {
		m = (m * m) >> 31;
		if (m >= (2ULL << 31)) {
			m >>= 1;
			frac |= 1ULL << (23 - i);
		}
	}

	return ((u64)n << 24) | frac;
}

/* 10000 * log10(x) * 2^24 = (10 dB を mdB で表した値) * 2^24。x >= 1 */
static s64 sig_log10_mdb_q24(u32 x)
{
	const u64 log2_q24 = sig_log2_q24(x);

	return (s64)((log2_q24 * SIG_LOG10_2_Q32) >> 32) * 10000;
}

/* ln(x) * 2^24。x >= 1 */
static s64 sig_ln_q24(u32 x)
{
	const u64 log2_q24 = sig_log2_q24(x);

	return (s64)((log2_q24 * SIG_LN2_Q32) >> 32);
}

/* 平方根の整数部 (ビット単位の 2 進法) */
static u64 sig_isqrt_u64(u64 x)
{
	u64 res = 0;
	u64 bit = 1ULL << 62;

	while (bit > x)
		bit >>= 2;

	while (bit) {
		if (x >= res + bit) {
			x -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}

	return res;
}

/*
 * Horner 法で係数列を評価する。coef は 0 次の係数から順に並べ、各係数は
 * 「求める値の 1000 倍」の単位を持つ。P は Q16 の変数。戻り値は Q16 の 1000 倍の値。
 */
static s64 sig_horner_q16(const s64 *coef_high_to_low, int count, s64 p_q16)
{
	s64 acc = coef_high_to_low[0] * 65536;
	int i;

	for (i = 1; i < count; i++)
		acc = ((acc * p_q16) >> 16) + coef_high_to_low[i] * 65536;

	return acc;
}

/* Q20 gives the steep ISDB-S polynomial enough precision to stay within 1 mdB. */
static s64 sig_horner_q20(const s64 *coef_high_to_low, int count, s64 p_q20)
{
	s64 acc = coef_high_to_low[0] * (1LL << 20);
	int i;

	for (i = 1; i < count; i++)
		acc = ((acc * p_q20) >> 20) + coef_high_to_low[i] * (1LL << 20);

	return acc;
}

/*
 * Q16 の値を mdB へ換算する。scale は係数の単位 (1000 または 10) と Q16 を掛けたもの。
 * 小数部は切り捨てる。従来の host の static_cast<int32_t> と同じ向きにするため。
 */
static s32 sig_to_mdb(s64 acc, s64 scale_q16)
{
	return (s32)SIG_DIV_S64(acc, (s32)scale_q16);
}

int signal_cn_isdbt_cxd2856er_mdb(u16 raw, s32 *mdb)
{
	s64 v;

	if (!mdb)
		return -EINVAL;
	if (!raw)
		return -EAGAIN;

	/* 10000 log10(raw) - 9031 を Q24 で求め、整数へ切り捨てる (従来の static_cast と同じ向き) */
	v = sig_log10_mdb_q24(raw) - 9031LL * (1LL << 24);
	*mdb = (s32)SIG_DIV_S64(v, 1 << 24);

	return 0;
}

int signal_cn_isdbc_cxd2856er_mdb(u16 raw, s32 *mdb)
{
	s64 v;
	u32 estimate;

	if (!mdb)
		return -EINVAL;
	if (!raw)
		return -EAGAIN;

	estimate = (raw < 126) ? 126 : raw;

	/* 95941 - 9500 ln(estimate) [mdB] */
	v = (s64)95941 * (1LL << 24) - sig_ln_q24(estimate) * 9500;
	*mdb = (s32)SIG_DIV_S64(v, 1 << 24);

	return 0;
}

/* 表は raw の降順 (val の大きい順) で、cnr は昇順 */
struct sig_cn_isdbs_entry {
	u16 val;
	u32 cnr;
};

static const struct sig_cn_isdbs_entry sig_isdbs_cn_table[] = {
	{ 0x5af, 0 }, { 0x597, 100 }, { 0x57e, 200 }, { 0x567, 300 },
	{ 0x550, 400 }, { 0x539, 500 }, { 0x522, 600 }, { 0x50c, 700 },
	{ 0x4f6, 800 }, { 0x4e1, 900 }, { 0x4cc, 1000 }, { 0x4b6, 1100 },
	{ 0x4a1, 1200 }, { 0x48c, 1300 }, { 0x477, 1400 }, { 0x463, 1500 },
	{ 0x44f, 1600 }, { 0x43c, 1700 }, { 0x428, 1800 }, { 0x416, 1900 },
	{ 0x403, 2000 }, { 0x3ef, 2100 }, { 0x3dc, 2200 }, { 0x3c9, 2300 },
	{ 0x3b6, 2400 }, { 0x3a4, 2500 }, { 0x392, 2600 }, { 0x381, 2700 },
	{ 0x36f, 2800 }, { 0x35f, 2900 }, { 0x34e, 3000 }, { 0x33d, 3100 },
	{ 0x32d, 3200 }, { 0x31d, 3300 }, { 0x30d, 3400 }, { 0x2fd, 3500 },
	{ 0x2ee, 3600 }, { 0x2df, 3700 }, { 0x2d0, 3800 }, { 0x2c2, 3900 },
	{ 0x2b4, 4000 }, { 0x2a6, 4100 }, { 0x299, 4200 }, { 0x28c, 4300 },
	{ 0x27f, 4400 }, { 0x272, 4500 }, { 0x265, 4600 }, { 0x259, 4700 },
	{ 0x24d, 4800 }, { 0x241, 4900 }, { 0x236, 5000 }, { 0x22b, 5100 },
	{ 0x220, 5200 }, { 0x215, 5300 }, { 0x20a, 5400 }, { 0x200, 5500 },
	{ 0x1f6, 5600 }, { 0x1ec, 5700 }, { 0x1e2, 5800 }, { 0x1d8, 5900 },
	{ 0x1cf, 6000 }, { 0x1c6, 6100 }, { 0x1bc, 6200 }, { 0x1b3, 6300 },
	{ 0x1aa, 6400 }, { 0x1a2, 6500 }, { 0x199, 6600 }, { 0x191, 6700 },
	{ 0x189, 6800 }, { 0x181, 6900 }, { 0x179, 7000 }, { 0x171, 7100 },
	{ 0x169, 7200 }, { 0x161, 7300 }, { 0x15a, 7400 }, { 0x153, 7500 },
	{ 0x14b, 7600 }, { 0x144, 7700 }, { 0x13d, 7800 }, { 0x137, 7900 },
	{ 0x130, 8000 }, { 0x12a, 8100 }, { 0x124, 8200 }, { 0x11e, 8300 },
	{ 0x118, 8400 }, { 0x112, 8500 }, { 0x10c, 8600 }, { 0x107, 8700 },
	{ 0x101, 8800 }, { 0x0fc, 8900 }, { 0x0f7, 9000 }, { 0x0f2, 9100 },
	{ 0x0ec, 9200 }, { 0x0e7, 9300 }, { 0x0e2, 9400 }, { 0x0dd, 9500 },
	{ 0x0d8, 9600 }, { 0x0d4, 9700 }, { 0x0cf, 9800 }, { 0x0ca, 9900 },
	{ 0x0c6, 10000 }, { 0x0c2, 10100 }, { 0x0be, 10200 }, { 0x0b9, 10300 },
	{ 0x0b5, 10400 }, { 0x0b1, 10500 }, { 0x0ae, 10600 }, { 0x0aa, 10700 },
	{ 0x0a6, 10800 }, { 0x0a3, 10900 }, { 0x09f, 11000 }, { 0x09b, 11100 },
	{ 0x098, 11200 }, { 0x095, 11300 }, { 0x091, 11400 }, { 0x08e, 11500 },
	{ 0x08b, 11600 }, { 0x088, 11700 }, { 0x085, 11800 }, { 0x082, 11900 },
	{ 0x07f, 12000 }, { 0x07c, 12100 }, { 0x07a, 12200 }, { 0x077, 12300 },
	{ 0x074, 12400 }, { 0x072, 12500 }, { 0x06f, 12600 }, { 0x06d, 12700 },
	{ 0x06b, 12800 }, { 0x068, 12900 }, { 0x066, 13000 }, { 0x064, 13100 },
	{ 0x061, 13200 }, { 0x05f, 13300 }, { 0x05d, 13400 }, { 0x05b, 13500 },
	{ 0x059, 13600 }, { 0x057, 13700 }, { 0x055, 13800 }, { 0x053, 13900 },
	{ 0x051, 14000 }, { 0x04f, 14100 }, { 0x04e, 14200 }, { 0x04c, 14300 },
	{ 0x04a, 14400 }, { 0x049, 14500 }, { 0x047, 14600 }, { 0x045, 14700 },
	{ 0x044, 14800 }, { 0x042, 14900 }, { 0x041, 15000 }, { 0x03f, 15100 },
	{ 0x03e, 15200 }, { 0x03c, 15300 }, { 0x03b, 15400 }, { 0x03a, 15500 },
	{ 0x038, 15600 }, { 0x037, 15700 }, { 0x036, 15800 }, { 0x034, 15900 },
	{ 0x033, 16000 }, { 0x032, 16100 }, { 0x031, 16200 }, { 0x030, 16300 },
	{ 0x02f, 16400 }, { 0x02e, 16500 }, { 0x02d, 16600 }, { 0x02c, 16700 },
	{ 0x02b, 16800 }, { 0x02a, 16900 }, { 0x029, 17000 }, { 0x028, 17100 },
	{ 0x027, 17200 }, { 0x026, 17300 }, { 0x025, 17400 }, { 0x024, 17500 },
	{ 0x023, 17600 }, { 0x022, 17800 }, { 0x021, 17900 }, { 0x020, 18000 },
	{ 0x01f, 18200 }, { 0x01e, 18300 }, { 0x01d, 18500 }, { 0x01c, 18700 },
	{ 0x01b, 18900 }, { 0x01a, 19000 }, { 0x019, 19200 }, { 0x018, 19300 },
	{ 0x017, 19500 }, { 0x016, 19700 }, { 0x015, 19900 }, { 0x014, 20000 },
};

int signal_cn_isdbs_cxd2856er_mdb(u16 raw, s32 *mdb)
{
	const size_t count = sizeof(sig_isdbs_cn_table) / sizeof(sig_isdbs_cn_table[0]);
	size_t lo = 0, hi = count;
	size_t idx;

	if (!mdb)
		return -EINVAL;

	/* std::lower_bound と同じ二分探索: 最初の val <= raw の行を求める */
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;

		if (sig_isdbs_cn_table[mid].val > raw)
			lo = mid + 1;
		else
			hi = mid;
	}
	idx = lo;

	if (idx == count) {
		idx = count - 1;
	} else if (sig_isdbs_cn_table[idx].val != raw && idx != 0) {
		idx--;
	}

	*mdb = (s32)sig_isdbs_cn_table[idx].cnr;

	return 0;
}

int signal_cn_tc90522_t_mdb(u32 cndat, s32 *mdb)
{
	static const s64 coef[] = { 24, -1600, 39800, 549100, 3096500 };
	s64 p_q16;
	s64 acc;

	if (!mdb)
		return -EINVAL;
	if (!cndat)
		return -EAGAIN;

	/*
	 * P [dB] = 10 log10(5505024 / cndat)。
	 * sig_log10_mdb_q24() は 10000 log10 * 2^24 = P[mdB] * 2^24 を返すので、
	 * P * 2^16 [dB] は差を 1000 * 256 で割って得る。
	 */
	p_q16 = (sig_log10_mdb_q24(5505024) - sig_log10_mdb_q24(cndat)) / (1000 * 256);

	acc = sig_horner_q16(coef, sizeof(coef) / sizeof(coef[0]), p_q16);
	*mdb = sig_to_mdb(acc, 1000LL << 16);

	return 0;
}

int signal_cn_tc90522_s_mdb(u16 cn, s32 *mdb)
{
	/* 係数は 10 倍した整数 ([mdB] の 0.1 単位) */
	static const s64 coef[] = { -16346, 143410, -502590, 889770, -895650, 588570 };
	u64 x;
	s64 p_q20;
	s64 acc;

	if (!mdb)
		return -EINVAL;
	if (cn < 3000)
		return -EAGAIN;

	/* P = sqrt(cn - 3000) / 64 を Q20 で求める。sqrt(x << 32) = sqrt(x) * 2^16 */
	x = (u64)(cn - 3000) << 32;
	p_q20 = (s64)(sig_isqrt_u64(x) >> 2);

	acc = sig_horner_q20(coef, sizeof(coef) / sizeof(coef[0]), p_q20);
	*mdb = sig_to_mdb(acc, 10LL << 20);

	return 0;
}

int signal_error_ppb(u64 errors, u64 total, s32 *ppb)
{
	u64 remainder;
	u64 result = 0;
	int digit_index;

	if (!ppb)
		return -EINVAL;
	if (!total)
		return -EAGAIN;
	if (errors > total)
		return -EINVAL;

	if (errors == total) {
		*ppb = (s32)SIG_PPB;
		return 0;
	}

	/*
	 * Compute floor(errors * 10^9 / total) as nine decimal digits. Repeated
	 * modular addition avoids overflowing the 64-bit product for long-lived
	 * counters.
	 */
	remainder = errors;
	for (digit_index = 0; digit_index < 9; digit_index++) {
		u64 scaled_remainder = 0;
		int digit = 0;
		int add;

		for (add = 0; add < 10; add++) {
			if (scaled_remainder >= total - remainder) {
				scaled_remainder -= total - remainder;
				digit++;
			} else {
				scaled_remainder += remainder;
			}
		}
		remainder = scaled_remainder;
		result = result * 10 + (u64)digit;
	}
	*ppb = (s32)result;

	return 0;
}

static void sig_add_layer(u64 errors, u64 packets, u64 *sum_errors, u64 *sum_bits)
{
	/* 総数が 0 の階層は、構成されていない階層として扱う */
	if (!packets)
		return;

	*sum_errors += errors;
	*sum_bits += packets * SIG_RS_PACKET_BITS;
}

int signal_parse_tc90522_t_ber(const u8 *buf, u64 *errors, u64 *bits)
{
	u64 sum_errors = 0, sum_bits = 0;
	int i;
	if (!buf || !errors || !bits)
		return -EINVAL;

	for (i = 0; i < 3; i++) {
		const u64 e = ((u64)buf[i * 3] << 16) | ((u64)buf[i * 3 + 1] << 8) | buf[i * 3 + 2];
		const u64 p = ((u64)buf[9 + i * 2] << 8) | buf[9 + i * 2 + 1];

		sig_add_layer(e, p, &sum_errors, &sum_bits);
	}

	*errors = sum_errors;
	*bits = sum_bits;

	return 0;
}

int signal_parse_tc90522_s_ber(const u8 *buf, u64 *errors, u64 *bits)
{
	u64 sum_errors = 0, sum_bits = 0;
	int i;
	if (!buf || !errors || !bits)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		const u64 e = ((u64)buf[i * 5] << 16) | ((u64)buf[i * 5 + 1] << 8) | buf[i * 5 + 2];
		const u64 p = ((u64)buf[i * 5 + 3] << 8) | buf[i * 5 + 4];

		sig_add_layer(e, p, &sum_errors, &sum_bits);
	}

	*errors = sum_errors;
	*bits = sum_bits;

	return 0;
}

int signal_parse_cxd2856er_isdbc_ber(const u8 *ber, u8 period, u64 *errors, u64 *bits)
{
	const u32 period_exp = period & 0x1f;
	u64 e;
	u64 total;
	if (!ber || !errors || !bits)
		return -EINVAL;
	e = ((u64)(ber[0] & 0x3f) << 16) | ((u64)ber[1] << 8) | ber[2];

	if (!(ber[0] & 0x80))
		return -EAGAIN;

	total = (1ULL << period_exp) * SIG_RS_PACKET_BITS;
	if (period_exp <= 11 && e > total)
		return -EAGAIN;

	*errors = e;
	*bits = total;

	return 0;
}
