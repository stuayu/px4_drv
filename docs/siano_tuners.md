# PX-S1UD V2.0 / PX-Q1UD

PX-S1UD V2.0 は地上デジタル1チャンネル、PX-Q1UD は地上デジタル4チャンネル同時受信のチューナーです。どちらも USB 2.0 機器で、内蔵 B-CAS カードリーダーはありません。チューナー部は Siano RIO で、px4_drv が従来から扱う ITE / IT930x 経路の PX-S1UR とは別のプロトコルです。

Linux の USB ドライバーの ID 表では、`3275:0080` を Siano RIO 機器として扱っています。PLEX の公式ダウンロードページは両機種の Windows ドライバーと PX-S1UD の Linux 版を掲載していますが、製品仕様には USB ID と PX-Q1UD の内部 USB 構成の記載がありません。

## OS ごとの経路

- **Windows:** DriverHost_PX4 は Siano RIO の USB function ごとに ISDB-T の受信機を1つ公開します。PX-S1UD V2.0 は1 function です。PX-Q1UD の4 function 構成は第三者のデバイス一覧に基づく情報で、PX-Q1UD 実機での構成確認が残っています。
- **macOS:** DriverHost_PX4 は Windows と同じ Siano プロトコルの経路を使います。同梱の `isdbt_rio.inp` ファームウェアには `LICENCE.siano` を添付しています。
- **Linux:** px4_drv は `3275:0080` に結び付き、Siano RIO の USB function ごとに地上デジタルの `/dev/px4video*` を1つ作ります。Linux 標準の `smsusb` ドライバーも同じ USB ID に一致して DVB frontend を公開するため、1つの USB function を所有できるのはどちらか一方のドライバーだけです。px4_drv へ結び付ける前に `smsusb` から unbind します。unbind すると、その Siano 機器を使っている録画が中断されます。ドライバーはカーネルのファームウェアローダーで `isdbt_rio.inp` を要求するため、先にシステムのファームウェア検索パスへ配置してください。

Windows では、同梱の WinUSB 用 INF を `3275:0080` へ導入します。公式 BDA ドライバーと WinUSB ドライバーは同じ USB function を同時に所有できません。どちらの製品もカードリーダーを公開しないため、復号には別のカードリーダーと対応するソフトウェアが必要です。

## ファームウェア

Windows / macOS のホストは、`DriverHost_PX4.ini` の `SianoFirmware` で別のパスを指定しない限り、DriverHost_PX4 と同じ場所の `isdbt_rio.inp` を読みます。Linux はカーネルのファームウェアローダーへ同じファイル名を要求し、通常は `/lib/firmware` から読み込まれます。同梱のバイナリは上流の Linux firmware リポジトリのファイルと一致します (SHA-256 `054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818`)。再配布の条件は `LICENCE.siano` にあります。

## 信号値

Windows と macOS では、`BonDriver_PX4::GetSignalLevel()` が Siano の復調器 SNR を C/N (dB) として返します。`GET_STATISTICS_EX_RES` は32ビットの `request_result` で始まり、その後に `sms_isdbt_stats_ex` が続きます。この構造体の中で `is_demod_locked` はオフセット12、符号付きの `SNR` (整数 dB) はオフセット20です。ホストは SNR を1000倍してミリデシベルへ換算し、既存の BonDriver インターフェースが dB へ戻します。校正済みの RF 入力電力 (dBm) ではありません。Linux 上流の Siano DVB frontend も同じ SNR フィールドを使い、ミリデシベルで公開しています。

## ISDB-C と J.83B

このリポジトリの Siano RIO 対応は地上デジタル (ISDB-T) に限られます。Linux 公開版の `smsdvb` frontend が登録する方式は `SYS_DVBT` と `SYS_ISDBT` だけで、このリポジトリが使うファームウェア (`isdbt_rio.inp`) は ISDB-T 用のイメージです。DVB-C、ISDB-C、ITU-T J.83 Annex B の受信を記載した Siano の公開ファームウェア、モード、レジスターの資料は見つかっていません。このため PX-S1UD V2.0 と PX-Q1UD には、どの OS のビルドにも ISDB-C と J.83B の経路がありません。

PX-S1UD でスカパー！プレミアムサービス光の J.83B を受信できたという報告がありましたが、技術的な一次資料と TS の記録は見つかっていません。上記の Siano の公開ソースに J.83B のモードはなく、報告にもファームウェア名、モード、チャンネル、TS のログがありません。J.83B の信号でロックと正常な TS を示す記録が得られるまで、Siano の J.83B は実装の対象外です。

## 検証状況

移植可能なプロトコル処理には自動試験があります。macOS の DriverHost は Siano の経路を含めてビルドでき、CTest も通過します。Windows の v145 x86 / x64 の `build.ps1` とハードウェア非依存の試験も通過します。

Windows 実機では、13ch・15ch・22ch の60秒録画で CC・TEI・同期エラーがすべて0で、平均ビットレートは公式 BDA ドライバーとの差が0.2%以内でした。4 function の同時受信では、4チャンネル (13ch・15ch・22ch・28ch) のすべてで TS を得ました。

一方、function ごとの確認では、USB function `&0&1` が WinUSB で 28ch の `BDA_DATA` を出さず、公式 BDA ドライバーでは同じ function で 28ch を受信できました。他の3 function は WinUSB でも 28ch を受信できました。WinUSB のホストは TS が届かない場合に選局失敗を返すようにしましたが、この function 固有の受信差は未解決です。

Linux での Siano の受信、30分録画、ホットプラグ、ドライバーの所有権の切り替えは実機での確認が残っています。残りの確認が済むまで、PX-S1UD V2.0 と PX-Q1UD は実験的な対応です。

## 参考資料

- [PLEX PX-S1UD V2.0 の仕様](https://plex-net.co.jp/itemreco/px-s1ud/)
- [PLEX PX-Q1UD の仕様](https://plex-net.co.jp/item/tv-tuner/px-q1ud/)
- [PLEX ドライバーのダウンロード](https://plex-net.co.jp/download/)
- [Linux 上流の Siano USB ドライバー](https://github.com/torvalds/linux/blob/master/drivers/media/usb/siano/smsusb.c)
- [Linux 上流の Siano DVB 統計](https://github.com/torvalds/linux/blob/master/drivers/media/common/siano/smsdvb-main.c#L2880-L2932)
- [Siano 拡張統計の構造体](https://github.com/torvalds/linux/blob/master/drivers/media/common/siano/smscoreapi.h#L3468-L3479)
- [Linux 上流の Siano ファームウェア名](https://codebrowser.dev/linux/linux/drivers/media/common/siano/smscoreapi.h.html)
- [上流の `isdbt_rio.inp`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/isdbt_rio.inp)
- [GPL の Siano RIO ユーザー空間参照実装](https://github.com/Khronos31/siano-userland)
- [PX-Q1UD の4つの Siano function を示す第三者の USB 一覧](https://github.com/tsukumijima/ISDBScanner/blob/master/isdb_scanner/tuner.py)
