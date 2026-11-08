# Sony CECH-ZD1 J (torne)

## 対応範囲

CECH-ZD1 J は USB `054c:04b2` の ISDB-T チューナー1基です。公開実装の基板資料では、USB ブリッジに Fujitsu MB86H47A、チューナー / 復調器に DiBcom DiB8096 (DiB8000 / DiB0090) を使用しています。外部チューナーファームウェアは不要です。

この移植は受信経路を対象にします。内蔵 B-CAS カードスロットは統合していないため、復号には別のカードリーダーが必要です。

対応状況の `△` は、コード経路が追加済みで実機品質の確認が残っている状態です。現時点の確認は模擬 USB と macOS のビルドに限られます。

## 制御とストリーム

MB86H47A のメモリー読み書きは USB vendor control request `0xbc` で行い、DiB8096 へは内蔵 I2C master を通じてアクセスします。初期化はブリッジ起動、復調器 reset、DiB8096 初期化の順です。選局時は TS bridge を停止して設定し直し、受信中なら選局後にストリームを再開します。

受信 TS は bulk IN endpoint `0x81` の192-byte単位で届きます。各単位の先頭4-byte timestampを除き、188-byte MPEG-TS packetを共通ストリームバッファーへ渡します。endpoint値は `torne_drv` のコードと `coax` の公開資料を参照しており、この Mac での endpoint descriptor 直接取得は未完了です。

統計は C/N を利用できます。Windows / macOS Host は共通 C/N 値を 0.001 dB 単位で返し、Linux の ptx `GET_CNR` は既存の torne 実装に合わせて 0.01 dB 単位です。Linux の信号強度は demodulator AGC 表からの推定値で、校正済み dBm ではありません。

## OS ごとの利用

- macOS: `DriverHost_PX4` の libusb 経路で認識し、`px4rec` の通常の録画コマンドから ISDB-T の周波数を指定します。
- Windows: 同梱の `Torne.inf` を使って WinUSB を割り当て、`BonDriver_Torne.dll` と `BonDriver_Torne.ini` を配置します。
- Linux: `px4_drv` の kernel module が `/dev/tornevideoN` を公開します。これは `px4video*` と同じ ptx ioctl / read API です。例: `recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts`。

## 実装元と検証

Linux 用 C 制御コアは [mihalinn/torne_drv](https://github.com/mihalinn/torne_drv) (`fc662902a3e4ad56566b00cae4a0ead922e3dd02`) から移植しました。ブリッジと DiB8096 のプロトコル資料は [Till0196/coax](https://github.com/Till0196/coax) (`6aaa1899991c875ba0482fa9098adc6c83b01fd8`) を参照しています。

自動試験は vendor control の値・index・big-endian データ、分割 USB input の TS framing、模擬ブリッジ起動、TS start/stop を確認します。これらはチューナーの RF lock や実放送 TS を証明しません。Mac の実機選局とTS品質、Windows x86/x64、Linux kernel build は未確認です。
