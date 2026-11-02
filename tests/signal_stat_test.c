// SPDX-License-Identifier: GPL-2.0-only
/*
 * signal_stat の換算と誤り率の単体試験。
 *
 * 浮動小数点で書いた参照式 (メインラインのコメントと従来の host 実装の式) と比べ、
 * 整数実装の差が 1 mdB 以内であることを全範囲で確かめる。
 * 既知の値と表の境界は、参照式を使わずに確かめる。
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "signal_stat.h"

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

/* 従来の host 実装と同じ式 (double の値を int へ切り捨てる) */
static s32 ref_isdbt_cxd(u16 raw)
{
	return (s32)(log10((double)raw) * 10000 - 9031);
}

static s32 ref_isdbc_cxd(u16 raw)
{
	const double estimate = (raw < 126) ? 126 : raw;

	return (s32)(-9500.0 * log(estimate) + 95941.0);
}

static s32 ref_tc90522_t(u32 cndat)
{
	const double p = 10 * log10(5505024 / (double)cndat);

	return (s32)((0.024 * p * p * p * p) - (1.6 * p * p * p) + (39.8 * p * p) + (549.1 * p) + 3096.5);
}

static s32 ref_tc90522_s(u16 cn)
{
	const double p = sqrt((double)(cn - 3000)) / 64;

	return (s32)((-1634.6 * p * p * p * p * p) + (14341 * p * p * p * p) - (50259 * p * p * p) +
		     (88977 * p * p) - (89565 * p) + 58857);
}

/* 検証した最大差の記録 (試験の出力に使う) */
static s32 max_diff_isdbt, max_diff_isdbc, max_diff_t, max_diff_s;
static int value_differences_printed;

static void check_within_one(s32 ours, s32 ref, s32 *max_diff, const char *name, u32 input)
{
	const s32 diff = (ours > ref) ? ours - ref : ref - ours;

	if (diff > *max_diff)
		*max_diff = diff;
	if (diff > 1) {
		if (value_differences_printed < 10)
			fprintf(stderr, "%s(%u): ours=%d ref=%d\n", name, input, ours, ref);
		value_differences_printed++;
		failures++;
	}
}

static void test_isdbt_cxd_formula(void)
{
	s32 v;
	u32 raw;

	/* raw = 1 は log10 = 0 なので -9031 */
	CHECK(signal_cn_isdbt_cxd2856er_mdb(1, &v) == 0 && v == -9031);
	/* raw = 10 は 10000 - 9031 = 969 */
	CHECK(signal_cn_isdbt_cxd2856er_mdb(10, &v) == 0 && (v == 968 || v == 969));
	CHECK(signal_cn_isdbt_cxd2856er_mdb(0, &v) == -EAGAIN);

	for (raw = 1; raw <= 0xffff; raw++) {
		if (signal_cn_isdbt_cxd2856er_mdb((u16)raw, &v))
			CHECK(0);
		check_within_one(v, ref_isdbt_cxd((u16)raw), &max_diff_isdbt, "isdbt", raw);
	}
}

static void test_isdbc_cxd_formula(void)
{
	s32 v;
	u32 raw;

	CHECK(signal_cn_isdbc_cxd2856er_mdb(0, &v) == -EAGAIN);
	/* 126 未満は 126 として扱う (従来の式と同じ) */
	CHECK(signal_cn_isdbc_cxd2856er_mdb(1, &v) == 0);
	CHECK(v == ref_isdbc_cxd(126));

	for (raw = 1; raw <= 0xffff; raw++) {
		if (signal_cn_isdbc_cxd2856er_mdb((u16)raw, &v))
			CHECK(0);
		check_within_one(v, ref_isdbc_cxd((u16)raw), &max_diff_isdbc, "isdbc", raw);
	}
}

static void test_tc90522_t_formula(void)
{
	s32 v;
	u32 cndat;

	CHECK(signal_cn_tc90522_t_mdb(0, &v) == -EAGAIN);
	/* 5505024 は P = 0 で、係数の定数項 3096.5 mdB を切り捨てた 3096 */
	CHECK(signal_cn_tc90522_t_mdb(5505024, &v) == 0 && v == 3096);

	for (cndat = 1; cndat <= 0xffffff; cndat += (cndat < 65536) ? 1 : 97) {
		if (signal_cn_tc90522_t_mdb(cndat, &v))
			CHECK(0);
		check_within_one(v, ref_tc90522_t(cndat), &max_diff_t, "tc90522_t", cndat);
	}
}

static void test_tc90522_s_formula(void)
{
	s32 v;
	u32 cn;

	CHECK(signal_cn_tc90522_s_mdb(2999, &v) == -EAGAIN);
	/* cn = 3000 は P = 0 で、定数項 58857 mdB */
	CHECK(signal_cn_tc90522_s_mdb(3000, &v) == 0 && v == 58857);

	for (cn = 3000; cn <= 0xffff; cn++) {
		if (signal_cn_tc90522_s_mdb((u16)cn, &v))
			CHECK(0);
		check_within_one(v, ref_tc90522_s((u16)cn), &max_diff_s, "tc90522_s", cn);
	}
}

/*
 * ISDB-S 表の境界。表は raw の降順で、表の値そのものは完全に一致する。
 * 表の間の値は raw が大きい側 (C/N が低い側) の行へ丸める。
 */
static void test_isdbs_table(void)
{
	s32 v;

	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x5af, &v) == 0 && v == 0);
	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x597, &v) == 0 && v == 100);
	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x4cc, &v) == 0 && v == 1000);
	/* 最も小さい raw は表の最後の行 */
	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x0000, &v) == 0 && v == 20000);
	/* 0x5b0 は表の先頭より大きいので先頭行 */
	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x5b0, &v) == 0 && v == 0);
	/* 0x596 は 0x597 (100 mdB) と 0x57e (200 mdB) の間。raw が大きい 0x597 の行へ丸める */
	CHECK(signal_cn_isdbs_cxd2856er_mdb(0x596, &v) == 0 && v == 100);
}

static void test_error_ppb(void)
{
	s32 v;

	CHECK(signal_error_ppb(1, 1000000000ULL, &v) == 0 && v == 1);
	CHECK(signal_error_ppb(0, 10, &v) == 0 && v == 0);
	CHECK(signal_error_ppb(5, 10, &v) == 0 && v == 500000000);
	CHECK(signal_error_ppb(10, 10, &v) == 0 && v == 1000000000);
	CHECK(signal_error_ppb(0, 0, &v) == -EAGAIN);
	CHECK(signal_error_ppb(11, 10, &v) == -EINVAL);
	/* errors * 1e9 must not overflow before division. */
	CHECK(signal_error_ppb(UINT64_MAX - 1, UINT64_MAX, &v) == 0 && v == 999999999);
	CHECK(signal_error_ppb(UINT64_MAX, UINT64_MAX, &v) == 0 && v == 1000000000);
	CHECK(signal_error_ppb(0, 1, NULL) == -EINVAL);
}

static void test_tc90522_ber_parse(void)
{
	/* 階層 A: 誤り 0x100、パケット 1。階層 B: 空。階層 C: 誤り 2、パケット 2 */
	u8 t[15] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
		     0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };
	/* 階層 A: 誤り 0x000100、パケット 1。階層 B: 誤り 0x000010、パケット 1。 */
	u8 s[10] = { 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x10, 0x00, 0x01 };
	u64 errors = 0, bits = 0;

	CHECK(signal_parse_tc90522_t_ber(t, &errors, &bits) == 0);
	CHECK(errors == 0x100 + 2);
	CHECK(bits == (1 + 2) * 204ULL * 8);

	errors = bits = 0;
	CHECK(signal_parse_tc90522_s_ber(s, &errors, &bits) == 0);
	CHECK(errors == 0x100 + 0x10);
	CHECK(bits == (1 + 1) * 204ULL * 8);
	CHECK(signal_parse_tc90522_t_ber(NULL, &errors, &bits) == -EINVAL);
	CHECK(signal_parse_tc90522_s_ber(s, NULL, &bits) == -EINVAL);
}

static void test_cxd2856er_isdbc_ber_parse(void)
{
	const u8 valid[3] = { 0x80, 0x00, 0x05 };
	const u8 invalid[3] = { 0x00, 0x00, 0x05 };
	const u8 over[3] = { 0x80, 0xff, 0xff };
	u64 errors = 0, bits = 0;

	/* 周期 2^10 パケット = 1024 * 1632 bit */
	CHECK(signal_parse_cxd2856er_isdbc_ber(valid, 0x0a, &errors, &bits) == 0);
	CHECK(errors == 5 && bits == 1024ULL * 1632);

	CHECK(signal_parse_cxd2856er_isdbc_ber(invalid, 0x0a, &errors, &bits) == -EAGAIN);
	/* 周期が短いときに誤りが総数を超えるのは読み取りの失敗として扱う */
	CHECK(signal_parse_cxd2856er_isdbc_ber(over, 0x05, &errors, &bits) == -EAGAIN);
	CHECK(signal_parse_cxd2856er_isdbc_ber(NULL, 0x0a, &errors, &bits) == -EINVAL);
}

static void test_null_outputs(void)
{
	const u8 zero[15] = {};
	const u8 ber[3] = { 0x80, 0, 1 };
	u64 errors = 0, bits = 0;

	CHECK(signal_cn_isdbt_cxd2856er_mdb(10, NULL) == -EINVAL);
	CHECK(signal_cn_isdbs_cxd2856er_mdb(10, NULL) == -EINVAL);
	CHECK(signal_cn_isdbc_cxd2856er_mdb(126, NULL) == -EINVAL);
	CHECK(signal_cn_tc90522_t_mdb(1, NULL) == -EINVAL);
	CHECK(signal_cn_tc90522_s_mdb(3000, NULL) == -EINVAL);
	CHECK(signal_parse_tc90522_t_ber(zero, NULL, &bits) == -EINVAL);
	CHECK(signal_parse_tc90522_t_ber(zero, &errors, NULL) == -EINVAL);
	CHECK(signal_parse_tc90522_s_ber(zero, NULL, &bits) == -EINVAL);
	CHECK(signal_parse_tc90522_s_ber(zero, &errors, NULL) == -EINVAL);
	CHECK(signal_parse_cxd2856er_isdbc_ber(ber, 0, NULL, &bits) == -EINVAL);
	CHECK(signal_parse_cxd2856er_isdbc_ber(ber, 0, &errors, NULL) == -EINVAL);
}

int main(void)
{
	test_isdbt_cxd_formula();
	test_isdbc_cxd_formula();
	test_tc90522_t_formula();
	test_tc90522_s_formula();
	test_isdbs_table();
	test_error_ppb();
	test_tc90522_ber_parse();
	test_cxd2856er_isdbc_ber_parse();
	test_null_outputs();

	printf("max |integer - reference| (mdB): isdbt=%d isdbc=%d tc90522_t=%d tc90522_s=%d\n",
	       max_diff_isdbt, max_diff_isdbc, max_diff_t, max_diff_s);

	if (failures) {
		fprintf(stderr, "signal_stat_test: %d failure(s)\n", failures);
		return 1;
	}

	printf("signal_stat_test: all checks passed\n");
	return 0;
}
