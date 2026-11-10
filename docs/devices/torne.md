# Sony CECH-ZD1 J (torne)

## 対応範囲

CECH-ZD1 J は USB `054c:04b2` の ISDB-T チューナー1基です。公開実装の基板資料では、USB ブリッジに Fujitsu MB86H47A、チューナー / 復調器に DiBcom DiB8096 (DiB8000 / DiB0090) を使用しています。外部チューナーファームウェアは不要です。

内蔵カードスロットは MB86H47A の UART を通じて PC/SC リーダーとして利用できます。Windows は DriverHost_PX4 の CardServer とアプリケーションローカルの `WinSCard.dll`、macOS は `pcsc/` の IFD ハンドラーから CardServer、Linux は `/dev/px4card/<ID>` と同 IFD ハンドラーを通ります。TVTest では torne の BonDriver とカードリーダーを個別に選べます。

CECH-ZD1 J は地上デジタル専用です。BS / CS の受信とカード利用には対応しません。

対応状況の `△` は、コード経路が追加済みで実機品質の確認が残っている状態です。実機の確認は macOS の試験 (後述) までで、Windows と Linux は自動試験とビルドの確認に限られます。

## 制御、カード UART、ストリーム

MB86H47A のメモリー読み書きは USB vendor control request `0xbc` で行い、DiB8096 へは内蔵 I2C master を通じてアクセスします。初期化はブリッジ起動、復調器 reset、DiB8096 初期化の順です。選局時は TS bridge を停止して設定し直し、受信中なら選局後にストリームを再開します。

受信 TS は bulk IN endpoint `0x81` の192-byte単位で届きます。各単位の先頭4-byte timestampを除き、188-byte MPEG-TS packetを共通ストリームバッファーへ渡します。endpoint 値は `torne_drv` のコードと `coax` の公開資料を参照しており、実機の endpoint descriptor の直接取得は未完了です。

統計は C/N を利用できます。Windows / macOS Host は共通 C/N 値を 0.001 dB 単位で返し、Linux の ptx `GET_CNR` は既存の torne 実装に合わせて 0.01 dB 単位です。Linux の信号強度は demodulator AGC 表からの推定値で、校正済み dBm ではありません。

カード UART は MB86H47A の `0xfea000`–`0xfea03c` のレジスターを使います。機種固有の reset、baud rate、FIFO の読み書きは [`torne_card.c`](../../driver/common/torne/torne_card.c) に置き、レジスター read/write、ロック、待機を callback に分離しています。ATR 完了待ち、解析、T=1 の I/R/S ブロック、再送、APDU 全体の絶対期限、セッション破棄は全機種共通の [`smart_card.cpp`](../../host/core/smart_card.cpp) が担当します。torne 固有層にはカード UART のハードウェア操作だけを置いています。

ホスト側はカード UART 用 mutex をレジスター1回の read/write ごとに取り、USB 制御転送 mutex も転送ごとに解放します。ATR / T=1 のポーリングと reset の待機中はどちらも保持せず、選局用 `mutex_` や TS の bulk 受信、framer、ストリームバッファーをカード通信で止めません。Linux も `operation_lock` とカード UART mutex を分離し、USB 制御転送を1回ずつ直列化します。待機はユーザー空間の PC/SC 層で行います。torne のカード VCC は UART が制御し、チューナーと共有する基板電源参照はありません。

## OS ごとの利用

- macOS: `DriverHost_PX4` の libusb 経路で受信し、`pcsc/` の IFD ハンドラーが同じ CardServer に接続します。DriverHost_PX4 の起動後、PC/SC アプリケーションから `Sony CECH-ZD1 J Smart Card Reader` を選べます。
- Windows: 同梱の `Torne.inf` を使って WinUSB を割り当て、`BonDriver_Torne.dll` と `BonDriver_Torne.ini` を配置します。
- Linux: `px4_drv` の kernel module が `/dev/tornevideoN` と `/dev/px4card/<ID>` を公開します。映像デバイスは `px4video*` と同じ ptx ioctl / read API です。例: `recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts`。

## 実装元と検証

Linux 用 C 制御コアは [mihalinn/torne_drv](https://github.com/mihalinn/torne_drv) (`fc662902a3e4ad56566b00cae4a0ead922e3dd02`) から移植しました。ブリッジと DiB8096 のプロトコル資料は [Till0196/coax](https://github.com/Till0196/coax) (`6aaa1899991c875ba0482fa9098adc6c83b01fd8`) を参照しています。

自動試験は vendor control の値・index・big-endian データ、分割 USB input の TS framing、模擬ブリッジ起動、TS start/stop、カード UART の reset / baud rate / FIFO と lock-release を確認します。共通 SmartCard の模擬試験では ATR 完了待ち、T=1 I/R/S ブロック、NAD、再送、絶対期限、抜去後の再初期化を確認します。自動試験は実機の RF lock、カード応答、TS との同時負荷を証明しません。

### macOS の実機試験 (2026-11)

Apple Silicon Mac に CECH-ZD1 J を1台接続し、地上波 T21 のアンテナ入力と B-CAS カードを挿した状態で、専用の `PX4_RUNTIME_DIR` から起動した DriverHost_PX4 を使いました。カードの抜き差しは試験の対象外です。

| 試験 | 条件 | 結果 |
|---|---|---|
| 選局 (T21) | DriverHost_PX4 の `Tune timing` | 周波数設定 3118 ms、ロック 3121 ms、最初の TS 3147 ms。C/N 約 22.76 dB |
| 60秒受信 (T21) | `px4rec` で録画 | 688,682 パケットで同期エラー・TEI・CC 欠落はすべて0、17.27 Mbps |
| 選局と Open/Close の反復 | `px4rec -d 2` を T21 / T28 で交互に100回 | 100回すべて成功 |
| 30分連続受信 (T21) | `px4rec` で録画し、DriverHost_PX4 の5秒ごとの統計を記録 | 20,666,154 パケットで同期エラー・TEI・CC 欠落はすべて0。統計382行すべてで同じ。平均 17.27 Mbps |
| カード単独 | ATR 取得後、初期設定 APDU を200回 | ATR 13 バイト、T=1。成功200回、失敗0回、最大応答 57.4 ms、平均 53.2 ms |
| 受信と APDU の並行 | `px4rec -d 300 T 21` の受信中に 200 ms 間隔で APDU を290秒 | APDU 成功1117回、失敗0回、最大応答 66.0 ms。TS 3,444,266 パケットで同期エラー・TEI・CC 欠落はすべて0、17.27 Mbps、パケット間隔の最大 35 ms |
| リアルタイム復号 | T21 を120秒受信し、libaribb25 が torne のカードを使って復号 | 1,377,578 パケットでスクランブル残り・同期エラー・TEI・CC 欠落はすべて0。ECM の APDU 61回で失敗0 |

DriverHost_PX4 は各試験の後に SIGINT で正常終了しました。T28 の TS 品質の個別計測、カードを使いながらの長時間受信、抜き差し後の再利用、Windows と Linux の実機受信、Windows の TVTest での復号、Linux の PC/SC 経路は未確認です。
