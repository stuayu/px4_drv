# Sony CECH-ZD1 J (torne)

## 対応範囲

CECH-ZD1 J は USB `054c:04b2` の ISDB-T チューナー1基です。公開実装の基板資料では、USB ブリッジに Fujitsu MB86H47A、チューナー / 復調器に DiBcom DiB8096 (DiB8000 / DiB0090) を使用しています。外部チューナーファームウェアは不要です。

内蔵カードスロットは MB86H47A の UART を通じて PC/SC リーダーとして利用できます。Windows は DriverHost_PX4 の CardServer とアプリケーションローカルの `WinSCard.dll`、macOS は `pcsc/` の IFD ハンドラーから CardServer、Linux は `/dev/px4card/<ID>` と同 IFD ハンドラーを通ります。TVTest では torne の BonDriver とカードリーダーを個別に選べます。

CECH-ZD1 J は地上デジタル専用です。BS / CS の受信とカード利用には対応しません。

対応状況の `△` は、コード経路が追加済みで実機品質の確認が残っている状態です。現時点の確認は模擬 USB と macOS のビルドに限られます。

## 制御、カード UART、ストリーム

MB86H47A のメモリー読み書きは USB vendor control request `0xbc` で行い、DiB8096 へは内蔵 I2C master を通じてアクセスします。初期化はブリッジ起動、復調器 reset、DiB8096 初期化の順です。選局時は TS bridge を停止して設定し直し、受信中なら選局後にストリームを再開します。

受信 TS は bulk IN endpoint `0x81` の192-byte単位で届きます。各単位の先頭4-byte timestampを除き、188-byte MPEG-TS packetを共通ストリームバッファーへ渡します。endpoint値は `torne_drv` のコードと `coax` の公開資料を参照しており、この Mac での endpoint descriptor 直接取得は未完了です。

統計は C/N を利用できます。Windows / macOS Host は共通 C/N 値を 0.001 dB 単位で返し、Linux の ptx `GET_CNR` は既存の torne 実装に合わせて 0.01 dB 単位です。Linux の信号強度は demodulator AGC 表からの推定値で、校正済み dBm ではありません。

カード UART は MB86H47A の `0xfea000`–`0xfea03c` のレジスターを使います。機種固有の reset、baud rate、FIFO の読み書きは [`torne_card.c`](../../driver/common/torne/torne_card.c) に置き、レジスター read/write、ロック、待機を callback に分離しています。ATR 完了待ち、解析、T=1 の I/R/S ブロック、再送、APDU 全体の絶対期限、セッション破棄は全機種共通の [`smart_card.cpp`](../../host/core/smart_card.cpp) が担当します。torne 固有層にはカード UART のハードウェア操作だけを置いています。

ホスト側はカード UART 用 mutex をレジスター1回の read/write ごとに取り、USB 制御転送 mutex も転送ごとに解放します。ATR / T=1 のポーリングと reset の待機中はどちらも保持せず、選局用 `mutex_` や TS の bulk 受信、framer、ストリームバッファーをカード通信で止めません。Linux も `operation_lock` とカード UART mutex を分離し、USB 制御転送を1回ずつ直列化します。待機はユーザー空間の PC/SC 層で行います。torne のカード VCC は UART が制御し、チューナーと共有する基板電源参照はありません。

## OS ごとの利用

- macOS: `DriverHost_PX4` の libusb 経路で受信し、`pcsc/` の IFD ハンドラーが同じ CardServer に接続します。DriverHost_PX4 の起動後、PC/SC アプリケーションから `Sony CECH-ZD1 J Smart Card Reader` を選べます。
- Windows: 同梱の `Torne.inf` を使って WinUSB を割り当て、`BonDriver_Torne.dll` と `BonDriver_Torne.ini` を配置します。
- Linux: `px4_drv` の kernel module が `/dev/tornevideoN` と `/dev/px4card/<ID>` を公開します。映像デバイスは `px4video*` と同じ ptx ioctl / read API です。例: `recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts`。

## 実装元と検証

Linux 用 C 制御コアは [mihalinn/torne_drv](https://github.com/mihalinn/torne_drv) (`fc662902a3e4ad56566b00cae4a0ead922e3dd02`) から移植しました。ブリッジと DiB8096 のプロトコル資料は [Till0196/coax](https://github.com/Till0196/coax) (`6aaa1899991c875ba0482fa9098adc6c83b01fd8`) を参照しています。

自動試験は vendor control の値・index・big-endian データ、分割 USB input の TS framing、模擬ブリッジ起動、TS start/stop、カード UART の reset / baud rate / FIFO と lock-release を確認します。共通 SmartCard の模擬試験では ATR 完了待ち、T=1 I/R/S ブロック、NAD、再送、絶対期限、抜去後の再初期化を確認します。自動試験は実機の RF lock、カード応答、TS との同時負荷を証明しません。最新の実機条件と数値は [`torne_card_results.md`](../../torne_card_results.md) を参照してください。
