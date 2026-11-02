// SPDX-License-Identifier: GPL-2.0-only
/*
 * 信号品質の換算と誤り率の計算 (signal_stat.h)
 *
 * Linux カーネルモジュールと、WinUSB / macOS の DriverHost_PX4 の両方から使う。
 * 浮動小数点は使わず、すべて整数演算で求める (カーネルでは浮動小数点を使えないため)。
 *
 * 単位:
 *   C/N (CNR): mdB (0.001 dB)
 *   誤り率:    ppb (10^-9)。誤り数と総数の比を、10^9 を掛けて整数へ丸める。
 *
 * 戻り値の共通の意味:
 *   0        換算できた。
 *   -EAGAIN  その時点では値が無い (カウンターが無効、未ロック、誤り数や総数が 0 など)。
 *            呼び出し側は、これを機種ごとの既存の結果 (値 0 など) へ写す。
 *   -EINVAL  読んだ値が矛盾する (誤り数が総数を超える など)。
 */

#ifndef __SIGNAL_STAT_H__
#define __SIGNAL_STAT_H__

#ifdef __linux__
#include <linux/types.h>
#elif defined(__APPLE__)
#include "misc_posix.h"
#include <stdint.h>
typedef uint64_t u64;
typedef int64_t s64;
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#include <stdint.h>
/* misc_win.h は 32 bit までの型だけを定義するため、64 bit の型をここで補う。 */
typedef uint64_t u64;
typedef int64_t s64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ISDB-T (CXD2856ER) の C/N。raw は cxd2856er_read_cnr_raw_isdbt() の値。
 * 式: 10000 * log10(raw) - 9031 [mdB]。
 * 根拠: Linux mainline drivers/media/dvb-frontends/cxd2841er.c の
 *       cxd2841er_read_snr_i() と同じ式 (10000 * intlog10(reg) - 9031)。
 *       当該ファイルの読み取りは要約経由のため、式の括弧は実装の確認で補う必要がある。
 */
int signal_cn_isdbt_cxd2856er_mdb(u16 raw, s32 *mdb);

/*
 * ISDB-S (CXD2856ER) の C/N。raw は cxd2856er_read_cnr_raw_isdbs() の値。
 * 換算は表の引き当てで、表は raw の降順。一致しない raw は、raw が大きい側
 * (C/N が低い側) の行へ丸める。
 * 表の値: 旧 host/devices/cxd2856er_cn_table.hpp から移した 192 行。
 */
int signal_cn_isdbs_cxd2856er_mdb(u16 raw, s32 *mdb);

/*
 * ISDB-C (CXD2856ER、QAM) の C/N 推定。raw は cxd2856er_read_cnr_raw_isdbc() の値。
 * 式: -9.50 * ln(raw / 24320) [dB] = 95941 - 9500 * ln(raw) [mdB]。raw は 126 未満を 126 とする。
 * 根拠: Linux mainline drivers/media/dvb-frontends/cxd2841er.c の
 *       cxd2841er_read_snr_c() (16/64/256QAM 側、-95 * log(reg) + 95941)。
 */
int signal_cn_isdbc_cxd2856er_mdb(u16 raw, s32 *mdb);

/*
 * ISDB-T (TC90522) の C/N。cndat は tc90522_get_cndat_t() の 24 bit 値。
 * 式: P = 10 * log10(5505024 / cndat) [dB]、
 *     C/N = 0.024 P^4 - 1.6 P^3 + 39.8 P^2 + 549.1 P + 3096.5 [mdB]。
 * 根拠: Linux mainline drivers/media/dvb-frontends/tc90522.c の tc90522t_get_frontend() の
 *       コメント (cnr[mdB] の式と P の定義)。
 */
int signal_cn_tc90522_t_mdb(u32 cndat, s32 *mdb);

/*
 * ISDB-S (TC90522) の C/N。cn は tc90522_get_cn_s() の 16 bit 値。cn < 3000 は値なし。
 * 式: P = sqrt(cn - 3000) / 64、
 *     C/N = -1634.6 P^5 + 14341 P^4 - 50259 P^3 + 88977 P^2 - 89565 P + 58857 [mdB]。
 * 根拠: Linux mainline drivers/media/dvb-frontends/tc90522.c の tc90522s_get_frontend() のコメント。
 */
int signal_cn_tc90522_s_mdb(u16 cn, s32 *mdb);

/* 誤り数 errors と総数 total から、誤り率を ppb (10^-9 単位) で返す。 */
int signal_error_ppb(u64 errors, u64 total, s32 *ppb);

/*
 * TC90522 ISDB-T の誤り計数 (レジスター 0x9d、15 byte)。
 * 3 階層それぞれについて、誤り数 3 byte と、総パケット数 2 byte (x 204 x 8 bit) が並ぶ。
 * 総数が 0 の階層は合計へ加えない。
 * 根拠: Linux mainline tc90522.c の tc90522t_get_frontend() の 0x9d の読み方。
 *       カウンターが誤り訂正の前か後かは、メインラインでも確定していない。
 */
int signal_parse_tc90522_t_ber(const u8 *buf, u64 *errors, u64 *bits);

/*
 * TC90522 ISDB-S の誤り計数 (レジスター 0xeb、10 byte)。
 * 2 階層について、誤り数 3 byte と、総パケット数 2 byte (x 204 x 8 bit) が並ぶ。
 * 根拠: Linux mainline tc90522.c の tc90522s_get_frontend() の 0xeb の読み方。
 */
int signal_parse_tc90522_s_ber(const u8 *buf, u64 *errors, u64 *bits);

/*
 * CXD2856ER ISDB-C (QAM) の誤り計数。ber は bank 0x40 レジスター 0x62 (3 byte)、
 * period は bank 0x40 レジスター 0x60 (1 byte)。
 * 誤りビット数 = ber[0..2] の下位 22 bit、総ビット数 = (1 << period & 0x1f) * 204 * 8。
 * ber[0] の bit 7 が 0 のときは無効 (-EAGAIN)。period <= 11 で誤りが総数を超えるときも無効とする。
 * 根拠: Linux mainline cxd2841er.c の cxd2841er_read_ber_c()。
 *       J.83B では意味を確認していないため、呼び出し側で扱わない。
 */
int signal_parse_cxd2856er_isdbc_ber(const u8 *ber, u8 period, u64 *errors, u64 *bits);

#ifdef __cplusplus
}
#endif

#endif /* __SIGNAL_STAT_H__ */
