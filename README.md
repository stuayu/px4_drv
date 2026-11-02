# px4_drv - Unofficial Linux / Windows (WinUSB) / macOS driver for PLEX PX4/PX5/PX-MLT series / e-Better DTV series ISDB-T/S/C receivers

PLEX や e-Better から発売された各種 ISDB-T/S/C チューナー向けの chardev 版非公式 Linux ドライバ / Windows (WinUSB) ドライバ / macOS 版 DriverHost_PX4 です。  
PLEX 社の [Webサイト](http://plex-net.co.jp) にて配布されている公式ドライバとは**別物**です。

## 構成と OS 境界

| パス | 役割 | 利用 OS |
|---|---|---|
| `driver/common/` | IT930x、復調器、チューナー、TS、固定小数点の信号計算など、OS 非依存の C コード | Linux カーネル / WinUSB / macOS |
| `driver/linux/` | USB 登録、カーネル内受信スレッド、chardev、ioctl、Linux の電源・寿命管理 | Linux |
| `host/common/`, `host/core/`, `host/devices/` | 共通の C++ デバイス、受信機、制御・配信サーバー | WinUSB / macOS |
| `winusb/` | WinUSB 転送、BonDriver、WinSCard、INF、Windows ビルドと配布 | Windows |
| `macos/` | libusb / POSIX 転送、Unix socket、`px4rec`、macOS ビルド | macOS |
| `fwtool/`, `tools/` | ファームウェア補助、診断・TS 解析ツール | ツールごとに異なる |
| `tests/`, `winusb/tests/`, `macos/tests/` | 共通プロトコル、状態遷移、OS 別ビルドの自動試験 | 対象ごとに異なる |
| `docs/` | 検証記録、機種別の手順、設計計画 | 全 OS |

`driver/common/` はハードウェア制御の共有部です。USB API、カーネル chardev、BonDriver、socket は OS ごとの境界に残し、受信機の生成は `host/core/device_factory.cpp` に集約しています。ファイル移動と段階別の完了状況は [再構成計画](docs/restructure_plan.md)を参照してください。

## 対応状況

次表の `○` はドライバ経路が実装済み、`△` は実装または実機確認に未完了条件がある状態、`—` はその OS の実装がない状態です。INI に製品名があるだけでは受信経路の実装と判定しません。

| 機種群 | Linux | Windows (WinUSB) | macOS | 未完了条件 |
|---|:---:|:---:|:---:|---|
| PLEX PX4 系 | ○ | ○ | ○ | 製品ごとの機能差は各設定を参照 |
| PLEX PX-MLT 系 / DTV02A-4TS-P / DTV02A-5TS-P | ○ | ○ | ○ | ISDB-C / J.83B は [ケーブルテレビ (ISDB-C) / スカパー！プレミアムサービス光 (J.83B) の受信](#ケーブルテレビ-isdb-c--スカパープレミアムサービス光-j83b-の受信) を参照 |
| ISDB2056 / ISDBT2071 | ○ | ○ | ○ | 製品・ロットごとの検証範囲は別紙を参照 |
| MyGica T230C 系 (USB `0572:c68a` は Linux の ID 表で T230C2) | △ | △ | △ | ISDB-C のみ (実験的)。Linux と macOS は実機未確認。J.83B の対応を示す根拠なし |
| Siano RIO (PX-S1UD V2.0 / PX-Q1UD 系) | ○ | ○ | ○ | firmware・製品リビジョン別の確認範囲は [Siano の別紙](docs/siano_tuners.md)を参照 |

T230C 系は、3 OS とも受信経路を持ちますが、Linux と Windows の実機確認が残っています。

## 機種別資料

- [ISDB-C と J.83 Annex B の選局の仕様](docs/isdbc_tuning.md)
- [T230C / T230C2 の Linux 参照手順と方式の制限](docs/t230c_linux_reference.md)
- [Siano RIO と対応チューナー](docs/siano_tuners.md)

## 診断ログ

WinUSB / macOS の DriverHost_PX4 は `LogLevel` (`error`, `warn`, `info`, `debug`, `trace`)、`LogCategories` (`usb`, `i2c`, `demod`, `tuner`, `ts`, `card`, `auth`, `server`, `device`)、`LogFormat` (`text`, `jsonl`) を設定できます。TS 統計は `LogStatsInterval` を秒数で指定すると有効になり、USB 転送履歴は `LogUsbHistory=true` または `LogLevel=trace` で保持し、転送失敗時に直近の履歴を出力します。

```ini
[DriverHost_PX4]
LogLevel=debug
LogCategories=usb,i2c,demod,tuner,ts,card,auth,server,device
LogFormat=jsonl
LogStatsInterval=10
LogUsbHistory=true
```

Windows では DriverHost_PX4 のログをデバッガー出力 (`OutputDebugString`) へ出力するため、DebugView などで記録します。macOS では DriverHost_PX4 の標準エラーへ出力します。Linux 版はカーネルの `dev_err` / `dev_warn` / `dev_info` / dynamic debug を使います。周期統計は既定で無効です。ログ整形と出力は呼び出し元スレッドで同期実行されるため、受信品質を測るときはログ有効状態も記録してください。

USB 履歴には各転送の先頭 16 byte が含まれる場合があります。カード通信やサービス情報を含む可能性があるため、共有前に payload を確認・マスクしてください。統計出力は既定で無効です。受信中に USB payload の trace を有効にした測定は、出力負荷を含む条件として記録してください。

### 選局から最初の TS までの時間

DriverHost_PX4 の `Tune timing` 行には、周波数設定完了 (`set_ms`)、ロック確認 (`lock_ms`)、最初の TS packet (`first_ts_ms`) の経過時間が入ります。ログを複数回分まとめるときは次のツールで system / 周波数ごとの件数、中央値、p95、最大値を集計できます。p95 は nearest-rank 方式です。

```sh
python3 tools/tune_latency.py DriverHost_PX4.log
python3 tools/tune_latency.py --json DriverHost_PX4.log
```

このツールは保存済みログだけを読み、チューナーへの接続や選局は行いません。最適化前後を比べる際は、同じ機種・周波数・信号経路・受信負荷で記録したログを使ってください。

`BonDriver_PX4::GetSignalLevel()` は既存互換のため C/N を dB 単位で返します。校正済み RF 入力電力 (dBm) として解釈しないでください。C/N 換算は WinUSB / macOS のホスト受信機で共通 C 関数を使います。Linux の `PTX_GET_CNR` は PT1/PT3 互換の値を返す既存の ioctl で、全機種共通の統計 API とは異なります。CXD2856ER ISDB-C では valid BER を記録します。PER の分母が確定していない機種は raw count のまま記録し、他チップの BER / PER と統計 API への接続は未完了です。

## このフォークについて

本家 [nns779/px4_drv](https://github.com/nns779/px4_drv) は 2021 年以降メンテナンスされておらず、残念ながら [nns779](https://github.com/nns779) 氏自身もネット上から失踪されています。  
このフォークは、各々のフォークに散逸していた有用な変更を一つにまとめ、継続的にメンテナンスしていくことを目的としています。

### 変更点 (WinUSB 版)

動作確認は Windows 10 / Windows 11 (x64) で行っています。

#### 新デバイス対応・機能追加

- PX-Q3PE5 に対応し、inf ファイルを追加
- [hendecarows 氏のフォーク](https://github.com/hendecarows/px4_drv) での更新を取り込み、DTV02A-1T1S-U (ロット番号 2309 以降を含む) / DTV03A-1TU / PX-M1UR / PX-S1UR に対応 (v0.5.0 以降 / DTV03A-1TU のみ v0.5.2 以降)
- PX-MLT5PE のリブランド品である DTV02A-5TS-P (USB Product ID: 0x924e) に対応し、inf ファイルを追加 (v0.6.1 以降)
- 各機種の内蔵 B-CAS カードリーダーを利用するためのドライバを追加 (v0.6.0 以降)
  - 同梱の `WinSCard.dll` を利用するアプリケーションの実行ファイルと同じフォルダに配置すると、内蔵カードリーダーを優先して利用できるようになる
  - Windows が認識している外付けカードリーダーも引き続き利用できる
- PX-MLT 系チューナー (PX-MLT5PE / PX-MLT8PE / DTV02A-4TS-P / DTV02A-5TS-P) において、ケーブルテレビ (トランスモジュレーション方式: ISDB-C) とスカパー！プレミアムサービス光 (J.83 Annex B) の受信に対応 (v0.7.0 以降)
  - [n0atl 氏のフォーク](https://github.com/n0atl/px4_drv) と [aoiro-labo 氏のフォーク](https://github.com/aoiro-labo/px4_drv) で進められていた対応を基にしている
  - 受信方法は [ケーブルテレビ (ISDB-C) / スカパー！プレミアムサービス光 (J.83B) の受信](#ケーブルテレビ-isdb-c--スカパープレミアムサービス光-j83b-の受信) を参照のこと
- Siano RIO 系の PX-S1UD V2.0 / PX-Q1UD に対応し、inf ファイルを追加 (実験的)
- MyGica T230C (USB `0572:c68a`) の ISDB-C 受信に対応し、inf ファイルを追加 (実験的)
- DriverHost_PX4 に、ログレベル・カテゴリ・JSON Lines 形式の出力、TS 統計、選局から最初の TS までの時間の内訳、USB 転送履歴を追加

#### 設定ファイル・ドライバ構成ファイルの改善

- inf ファイルをより分かりやすい名前に変更
- inf ファイルに自己署名を行い、自己署名証明書のインストールだけで正式なドライバーと認識されるように
  - 以前は署名がないためインストール時にドライバー署名の強制の無効化が必要だったが、事前に自己署名証明書を適切な証明書ストアにインストールしておけばこの作業が不要になる
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1577466040/104-108
- 自己署名証明書のインストール・アンインストールスクリプトを追加
  - 拡張子が .jse となっているが、これは PowerShell スクリプトにダブルクリックで実行させるための JScript コードを先頭の行に加えたもの
- inf ファイルを ARM 版 Windows でもインストールできるようにする
  - 実機がないので試せていないけど、おそらくインストールできるはず
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1625673548/762
- 地上波の ChSet に物理 53ch ～ 62ch の定義を追加
  - 物理 53ch ～ 62ch は地上波の割り当て周波数から削除されているが、現在も ”イッツコムch10” など、一部ケーブルテレビの自主放送の割り当て周波数として使われている
- BS/CS の ChSet に2022年3月開局の BS 新チャンネル（BS松竹東急・BSJapanext・BSよしもと）の定義を追加
- BS/CS の ChSet で、BS23/TS1 と BS23/TS3 の TSID が入れ替わっていたのを修正
- `BonDriver_PX-MLT.ini` に、LNB 給電を有効にするための `LNBPower` の設定を追記
- inf ファイルを改善し、チューナーをタスクトレイの「ハードウェアの安全な取り外し」に表示しないようにする (v0.4.3 以降)
  - ref: https://mevius.5ch.net/test/read.cgi/avi/1666581918/207
- BS/CS の ChSet に2024年10月～2025年1月に行われた BS トランスポンダ再編後の物理チャンネル情報を反映 (v0.5.2 以降)

#### ドライバの品質改善

- エラー発生時の MessageBox を表示しない設定を追加
  - BonDriver の ini 内の `DisplayErrorMessage` を 1 に設定すると今まで通り MessageBox が表示される
- バージョン情報が DLL のプロパティに表示されないのを修正
- https://github.com/tsukumijima/px4_drv/pull/2 をマージし、PX4 系チューナーで地上波の受信機を開く順番によってドロップが発生することがある問題を修正
- PX4 系チューナーを複数台接続したとき、使っていない方のチューナーの基板にも電源が入ったままになる問題を修正 (v0.6.0 以降)
  - ref: https://github.com/tsukumijima/px4_drv/issues/36
- 受信機の割り当て処理の不具合により、空いている受信機があっても `no receivers available` のエラーで選局できなくなることがある問題を修正 (v0.6.0 以降)
  - ref: https://github.com/tsukumijima/px4_drv/issues/38
- チャンネル切り替え、USB の切断・再接続、内蔵カードリーダーとの同時利用に関する安定性を改善 (v0.6.0 以降)
- BonDriver の ini の節に `=` を含まない行があると、設定の読み込みが無限ループに陥る問題を修正 (v0.7.0 以降)
- コピー型の `GetTsStream()` で、呼び出し元のバッファを越えて TS を書き込むことがある問題を修正 (v0.7.0 以降)
- DriverHost_PX4 のログファイルの末尾に、前回起動時のログが残る問題を修正 (v0.7.0 以降)
- PX-M1UR などの USB バスパワーで動作するチューナーで、内蔵カードリーダーを使っている間に未使用の受信回路へ給電され、同じ PC に接続した別のチューナーの受信品質が下がることがある問題を修正 (v0.7.1 以降)

#### ビルド・ドキュメント

- ビルドとパッケージングを全自動で行うスクリプトを追加
- README（このページ）に WinUSB 版のインストール方法などを追記
  - Visual Studio 2022 が入っていれば、build.ps1 を実行するだけで全自動でビルドからパッケージングまで行える
- fwtool: PX-W3PE5 / PX-Q3PE4 / PX-Q3PE5 / PX-W3U4 / PX-Q3U4 / PX-MLT5PE / PX-MLT8PE の2021年版公式 Windows ドライバからのファームウェア抽出に対応 (v0.6.0 以降)
  - 既定の抽出済みファームウェアで問題なく動作することが知られているため、特段実用性はない

### 変更点 (macOS 版)

- Apple Silicon (ARM) および Intel Mac 向けのユーザー空間デーモン `DriverHost_PX4` と、録画コマンド `px4rec` を追加
  - libusb を用いた USB デバイスアクセスと、UNIX ドメインソケットによる IPC を実装
  - Windows WinUSB 版と `host/` の受信機・サーバー実装を共有します。利用できる機種は上の対応状況表で確認してください
- `px4rec C` による ISDB-C の選局と、TSMF の TS 番号の指定に対応 (実験的)

### 変更点 (Linux 版)

動作確認は Ubuntu 22.04 LTS (x64) で行っています。

#### 新デバイス対応・機能追加

- [techmadot 氏のフォーク](https://github.com/techmadot/px4_drv) の更新を取り込み、PX-M1UR / PX-S1UR に対応
- [otya 氏のフォーク](https://github.com/otya128/px4_drv) での更新を取り込み、BS/CS の選局で相対 TS 番号に加えて TSID も指定できるように
- [hendecarows 氏のフォーク](https://github.com/hendecarows/px4_drv) での更新を取り込み、DTV03A-1TU に対応 (v0.5.4 以降)
- チップ構成が一部変更された、ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U に対応 (v0.4.5 以降)
- PX-MLT5PE のリブランド品である DTV02A-5TS-P (USB Product ID: 0x924e) に対応 (v0.6.0 以降)
- 各機種の内蔵 B-CAS カードリーダーを利用するためのドライバを追加 (v0.7.0 以降)
  - 別途 `px4-drv-pcsc` ドライバをインストールすることで、pcscd から内蔵カードリーダーを認識・利用できるようになる
  - pcscd が既定で認識している外付けカードリーダーも引き続き利用できる
-  PX-MLT 系チューナー (PX-MLT5PE / PX-MLT8PE / DTV02A-4TS-P / DTV02A-5TS-P) において、WinUSB 版と同じくケーブルテレビ (トランスモジュレーション方式: ISDB-C) とスカパー！プレミアムサービス光 (J.83 Annex B) の受信に対応 (v0.7.0 以降)
  - 受信方法は [ケーブルテレビ (ISDB-C) / スカパー！プレミアムサービス光 (J.83B) の受信](#ケーブルテレビ-isdb-c--スカパープレミアムサービス光-j83b-の受信) を参照のこと
- Siano RIO 系の PX-S1UD V2.0 / PX-Q1UD と、MyGica T230C2 の ISDB-C 受信を `/dev/px4video*` として追加 (実験的、カーネルビルドのみ確認)
- ソースコードを OS 非依存の `driver/common/` と Linux 固有の `driver/linux/` に分け、WinUSB 版・macOS 版とチップ制御のコードを共有

#### ドライバの品質改善

- [otya 氏のフォーク](https://github.com/otya128/px4_drv) での更新を取り込み、安定性と互換性を改善
- [kznrluk 氏のフォーク](https://github.com/kznrluk/px4_drv) の更新を取り込み、Linux カーネル 6.4 系以降の API 変更に対応
- https://github.com/tsukumijima/px4_drv/pull/3 をマージし、`ctrl_timeout` をモジュールパラメーターに追加 (v0.4.1 以降)
- https://github.com/tsukumijima/px4_drv/pull/6 をマージし、Linux カーネル 6.8 系以降の API 変更に対応 (v0.4.4 以降)
- DTV02A-4TS-P / PX-M1UR / PX-S1UR のデバイスファイルのパーミッションが 644 になっていたのを、他の機種と同じ 664 に修正 (v0.4.5 以降)
- ロット番号 2309 以降の DTV02A-1T1S-U で、復調器 (TC90522) の初期化と終了の処理が抜けていたのを修正 (v0.5.0 以降)
- Digibest 製チューナーに常に USB 電源を供給するようにし、USB の省電力機能によってチューナーの動作が不安定になる問題を改善 (v0.5.1 以降)
- https://github.com/tsukumijima/px4_drv/pull/20 をマージし、UBSAN を有効にしたカーネルで、dmesg に誤検知の警告が出る問題を修正 (v0.5.3 以降)
- 新しい GCC を使う Oracle Linux 9 などでビルドできない問題を修正 (v0.5.4 以降)
- https://github.com/tsukumijima/px4_drv/pull/33 をマージし、IBT (Indirect Branch Tracking) 対応に伴う `objtool` の検査強化により、Linux 6.15 系などでモジュールをビルド・ロードできない問題に対応 (v0.5.5 以降)
  - 当初は保守的に Linux 6.15.4 以降でのみ `module_init()` と `module_exit()` を使っていたが、これらは古いカーネルでも利用できるため、v0.6.0 以降ではカーネル版による分岐を削除して統一した
- https://github.com/tsukumijima/px4_drv/pull/37 をマージし、Linux カーネル 6.19 系以降でビルドできない問題に対応 (v0.5.6 以降)
- https://github.com/tsukumijima/px4_drv/pull/40 をマージし、モジュールパラメーター `multi_device_power_control_mode` の値の範囲の検査が誤っていたのを修正 (v0.6.0 以降)
- PX4 系チューナーを複数台接続したとき、使っていない方のチューナーの基板にも電源が入ったままになる問題を修正 (v0.6.0 以降)
  - ref: https://github.com/tsukumijima/px4_drv/issues/36
- チャンネル切り替え、USB の切断・再接続に関する安定性を改善 (v0.6.0 以降)
- IT930x の GPIO の設定の書き込みに一度失敗すると、USB を抜き差しするまで同じ設定をやり直せなくなる問題を修正 (v0.7.0 以降)
- PX-S1UR / DTV03A-1TU で、チューナーを開く処理が途中で失敗したときに、基板の電源が入ったまま残る問題を修正 (v0.7.0 以降)
- Linux 4.15 (Ubuntu 18.04 の初期カーネル) 以降でビルドできるように修正 (v0.7.0 以降)
- WinUSB 版と同じく、PX-S1UR を ISDB2056 系の共通処理に統合し、設計の異なる DTV03A-1TU (ISDBT2071) を単独の実装に分離 (v0.7.0 以降)
- 少なくとも PX-W3U4 で、チューナーを使っていない間にカードを抜いても、挿入済みのまま検出され続ける問題を修正 (v0.7.1 以降)
- PX-M1UR などの USB バスパワーで動作するチューナーで、内蔵カードリーダーを使っている間に未使用の受信回路へ給電され、同じ PC に接続した別のチューナーの受信品質が下がることがある問題を修正 (v0.7.1 以降)
- 内蔵カードリーダーのカード検出が一時的に失敗すると、録画中の復号がその録画の終わりまで再開しなくなることがある問題を修正 (v0.7.1 以降)
- pcscd の起動時や USB の接続直後に、内蔵カードリーダーが認識されないことがある問題を修正 (v0.7.1 以降)

#### ビルド・ドキュメント

- Debian パッケージ (.deb) の作成とインストールに対応
- DKMS でのインストール時にファームウェアを自動でインストールするように変更
- README (このページ) に Debian パッケージからのインストール方法などを追記
- https://github.com/tsukumijima/px4_drv/pull/27 をマージし、新しいカーネルをインストールしたときに、DKMS の後処理が使用中のドライバをアンロードしてしまう問題を修正 (v0.5.3 以降)
- https://github.com/tsukumijima/px4_drv/pull/29 をマージし、Ubuntu 24.04 で DKMS の後処理が `99-px4video.rules` と `it930x-firmware.bin` を削除してしまう問題を修正 (v0.5.4 以降)

## 対応デバイス

- PLEX

	- PX-W3U4
	- PX-Q3U4
	- PX-W3PE4
	- PX-Q3PE4
	- PX-W3PE5
	- PX-Q3PE5
	- PX-MLT5PE
	- PX-MLT8PE
    - PX-M1UR
    - PX-S1UR
    - PX-S1UD V2.0 / PX-Q1UD (実験的)

- MyGica

	- T230C 系 (USB `0572:c68a` の個体は T230C2、ISDB-C のみ・実験的)

- e-Better

	- DTV02-1T1S-U (実験的)
	- DTV02A-1T1S-U
	  - チップ構成が一部変更された、ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U にも対応しています。  
	  手元の実機では問題なく動作していますが、長期間の動作テストは行えていないため、未知の不具合があるかもしれません。
	- DTV02A-4TS-P
	- DTV02A-5TS-P (PX-MLT5PE のリブランド品)
	- DTV03A-1TU (実験的)
	  - チップ構成が大幅に変更された、ロット番号 2021-11 以降の個体のみ対応しています。

> [!NOTE]  
> 2021 年以降メンテナンスされていない [nns779/px4_drv](https://github.com/nns779/px4_drv) と異なり、新規に下記チューナーのサポートを追加しています。
> 
> - PLEX PX-M1UR
> - PLEX PX-S1UR
> - PLEX PX-S1UD V2.0 / PX-Q1UD (Siano RIO、実験的)
> - e-Better DTV02A-5TS-P (PX-MLT5PE のリブランド品)
> - e-Better DTV02A-1T1S-U / Digibest ISDB2056 (Windows 版ドライバを新規追加)
> - e-Better DTV02A-1T1S-U (ロット番号 2309 以降) / Digibest ISDB2056N
> - e-Better DTV03A-1TU / Digibest ISDBT2071 (ロット番号 2021-11 以降)

> [!WARNING]  
> PX-M1UR または DTV02(A)-1T1S-U で CATV（周波数変換パススルー）の ISDB-T C13ch ~ C24ch を受信するには、[専用の recpt1 フォーク (hendecarows/recpt1)](https://github.com/hendecarows/recpt1) が必要となります。  
> [stz2012/recpt1](https://github.com/stz2012/recpt1) や [recisdb](https://github.com/kazuki0824/recisdb-rs) では、当該機種にて C13ch ~ C62ch を正常に選局できないことが報告されています（[詳細はこちら](https://github.com/tsukumijima/px4_drv/issues/16)）。  
> ただし、C13ch ~ C24ch 以外のチャンネルであれば、stz2012/recpt1 や recisdb でも問題なく選局・受信が可能です。

PX-S1UD V2.0 / PX-Q1UD は Siano RIO 系の別プロトコルです。Windows/macOS DriverHost と Linux px4_drv の `/dev/px4video*` 経路、ファームウェア、検証状況は [Siano チューナーの説明](docs/siano_tuners.md)を参照してください。Linux 標準 `smsusb` ドライバーも同じ USB ID を扱うため、同じ USB function はどちらか一方のドライバーへ割り当てます。

## インストール (Windows)

Windows (WinUSB) 版のドライバは、OS にチューナーを認識させるための inf ファイルと、px4_drv 専用の BonDriver、ドライバの実体でチューナー操作を司る DriverHost_PX4 から構成されています。

**ビルド済みのアーカイブは [tsukumijima/DTV-Builds](https://github.com/tsukumijima/DTV-Builds) からダウンロードできます。**  
または、winusb フォルダにある build.ps1 を実行して、ご自分でビルドしたものを使うこともできます (Visual Studio 2022 が必要です) 。

### 1. 自己署名証明書のインストール

Driver フォルダには、各機種ごとのドライバのインストールファイル (.inf) が配置されています。  
ドライバをインストールする前に cert-install.jse を実行し、自己署名証明書をインストールしてください。

自己署名証明書をインストールして信頼することで、自己署名証明書を使用して署名されたドライバも、（自己署名証明書をアンインストールするまでは）通常の署名付きドライバと同様に信頼されるようになります。

cert-install.jse が Smart App Control や関連付けの変更などにより実行できない場合は、管理者権限の Windows Terminal または PowerShell を開き、ビルド済みアーカイブ内の Driver フォルダに移動した上で、下記コマンドを実行してください。

```powershell
certutil.exe -addstore Root ".\px4_drv_winusb.cer"
certutil.exe -addstore TrustedPublisher ".\px4_drv_winusb.cer"
```

### 2. ドライバのインストール

自己署名証明書をインストールしたあとは、公式ドライバと同様の手順でインストールできると思います（公式ドライバは事前にアンインストールしてください）。

具体的には、デバイスマネージャーからチューナーデバイスを選択し、右クリックメニューの [ドライバーの更新] → [コンピューターを参照してドライバーを検索] から、inf ファイルが入っている Driver フォルダを指定してください。[ドライバーが正常に更新されました] と表示されていれば OK です。

### 3. BonDriver の配置

チューナーの機種に応じた BonDriver を配置します。

- PX4/PX5 シリーズの機種: `BonDriver_PX4`
- PX-MLT シリーズの機種・DTV02A-4TS-P・DTV02A-5TS-P: `BonDriver_PX-MLT`
- DTV02A-1T1S-U: `BonDriver_ISDB2056`
- DTV02A-1T1S-U (ロット番号 2309 以降): `BonDriver_ISDB2056N`
- DTV03A-1TU: `BonDriver_ISDBT2071`
- PX-M1UR: `BonDriver_PX-M1UR`
- PX-S1UR: `BonDriver_PX-S1UR`
- PX-S1UD V2.0: `BonDriver_PX-S1UD` (実験的)
- PX-Q1UD: `BonDriver_PX-Q1UD` (実験的、4チューナーの並列受信は実機未確認)
- MyGica T230C (0572:C68A): `BonDriver_T230C` (実験的、ISDB-C のみ)

お使いの PC に合うビット数のフォルダの中のファイルを**すべて**選択し、TVTest や EDCB などのソフトウェアが指定する BonDriver の配置フォルダにコピーします。  
BonDriver と同じフォルダに DriverHost_PX4.exe / DriverHost_PX4.ini / it930x-firmware.bin があることを確認してください。Siano RIO のパッケージには `isdbt_rio.inp` と `LICENCE.siano` も含まれます。T230C の Si2168 / Si2141 のファームウェアは再配布できないため、[T230C の説明](docs/t230c.md)に従って配置してください。

使用にあたり、特段 ini ファイルの設定変更などは必要ありません。ソフトウェアごとにチャンネルスキャンを行えばそのまま視聴できます。  
なお、BonDriver の ini ファイル内の `DisplayErrorMessage` を 1 に設定すると、オリジナル同様にエラー発生時にメッセージボックスを表示します。

### 4. 内蔵 B-CAS カードリーダーの利用

**v0.6.0 以降、winUSB 版 px4_drv 専用の WinSCard.dll を TVTest.exe や EpgDataCap_Bon.exe と同じフォルダに配置することで、内蔵カードリーダーが利用できるようになりました！**

PX-S1UD V2.0、PX-Q1UD、MyGica T230C は内蔵カードリーダーを搭載していないため、B-CAS カードには外付けカードリーダーが必要です。

機種別の BonDriver フォルダに同梱されている `WinSCard.dll` を、TVTest や EDCB など、B-CAS カードを利用するアプリケーションの実行ファイルと同じフォルダにコピーしてください。

> [!NOTE]  
> **Windows が認識している外付けカードリーダーと内蔵カードリーダーは併用できます。**  
> カードリーダーの一覧には内蔵カードリーダー、外付けカードリーダーの順で表示されます。  

> [!NOTE]  
> **リモートデスクトップ接続中に、チューナーを接続した PC 側の外付けカードリーダーが見つからなくなる場合があります。**  
> RDP のスマートカードリダイレクトが有効なセッションでは、Windows 標準の WinSCard API が接続元 PC 側のカードリーダーを参照するためです。  
> [Windows 標準の対処方法](https://learn.microsoft.com/ja-jp/azure/virtual-desktop/redirection-configure-smart-cards)として、リモート デスクトップ接続の「オプションの表示」→「ローカル リソース」→「詳細」からスマートカードのチェックを外すか、接続先 PC の `gpedit.msc` で「コンピューターの構成」→「管理用テンプレート」→「Windows コンポーネント」→「リモート デスクトップ サービス」→「リモート デスクトップ セッション ホスト」→「デバイスとリソースのリダイレクト」→「スマート カード デバイスのリダイレクトを許可しない」を有効にしてください。  
> 内蔵カードリーダーへの通信は、`DriverHost_PX4.exe` が接続先 PC の WinUSB デバイスと直接行うため、RDP のスマートカードリダイレクトを経由しません。  
> ただし、`WinSCard.dll` を配置していても、外付けカードリーダーを選んだ処理は Windows 標準の WinSCard API へ引き渡されます。この場合は RDP の設定に従い、接続元 PC 側のカードリーダーだけが外付けカードリーダーとして表示されることがあります。

> [!WARNING]  
> **アプリケーション側のビット数 (32bit/64bit) と `WinSCard.dll` のビット数を合わせる必要があります。**  
> また、すでに別実装の `WinSCard.dll` を配置している場合は、元の DLL を退避してから入れ替えてください。  
> 同じフォルダで2つの `WinSCard.dll` を併用することはできません。

> [!TIP]  
> `WinSCard.dll` の追加・退避後、TVTest ではカードリーダーエラーが発生することがありますが、これは過去使っていたカードリーダーにアクセスできなくなったことによるエラーですので、異常ではありません。  
> **「設定」→「TSプロセッサー」→「デフォルトフィルター」のセレクトボックスで利用するカードリーダー名を選択し、TVTest を再起動すると、そのカードリーダーに挿入された B-CAS カードを使ってスクランブル解除を行えます。**  

> [!TIP]  
> **PC に複数のチューナーを接続している場合、TVTest では複数の内蔵カードリーダーを認識します。** B-CAS カードを挿入したチューナーの内蔵カードリーダーを選択してください。  
> **内蔵カードリーダーを使うために、PC に接続されているすべてのチューナーに B-CAS カードを挿入する必要はありません。** 内蔵カードリーダーは、受信中チューナーと異なるデバイスでも利用できます。

> [!NOTE]  
> `WinSCard.dll` は必要に応じて同じフォルダの `DriverHost_PX4.exe` を起動し、内蔵カードリーダーとの通信を仲介します。  
> 複数の TVTest や EDCB プロセスが同じカードへ接続した場合も、APDU と PC/SC トランザクションはカード単位で直列に処理されます。

## インストール (Linux)

### 1. ファームウェアの抽出とインストール (手動インストール時のみ)

> [!IMPORTANT]  
> **px4_drv を Debian パッケージや DKMS を使用してインストールする際、ファームウェアは自動的にインストールされます。**  
> **Debian パッケージや DKMS を使用してインストールを行う場合は、この手順をスキップしてください。**

事前に unzip, gcc, make がインストールされている必要があります。

	$ cd fwtool
	$ make
	$ wget http://plex-net.co.jp/plex/pxw3u4/pxw3u4_BDA_ver1x64.zip -O pxw3u4_BDA_ver1x64.zip
	$ unzip -oj pxw3u4_BDA_ver1x64.zip pxw3u4_BDA_ver1x64/PXW3U4.sys && rm pxw3u4_BDA_ver1x64.zip
	$ ./fwtool PXW3U4.sys it930x-firmware.bin && rm PXW3U4.sys
	$ sudo mkdir -p /lib/firmware && sudo cp it930x-firmware.bin /lib/firmware/
	$ cd ../

または、抽出済みのファームウェアを利用することもできます。

	$ sudo mkdir -p /lib/firmware && sudo cp ./etc/it930x-firmware.bin /lib/firmware/

### 2. ドライバのインストール

px4_drv は、Linux 4.15 (Ubuntu 18.04 の初期カーネル) 以降のカーネルでビルドできます。  
一部の Linux ディストリビューションでは、別途 udev のインストールが必要になる場合があります。
	
#### Debian パッケージを使用してインストール (強く推奨)

Debian パッケージを使用してインストールすると依存パッケージも自動インストールされるほか、DKMS のソースコード管理も透過的に行われます。  
Ubuntu / Debian 環境では Debian パッケージを使用してインストールすることを強く推奨します。

	$ wget https://github.com/tsukumijima/px4_drv/releases/download/v0.7.1/px4-drv-dkms_0.7.1_all.deb
	$ sudo apt install -y ./px4-drv-dkms_0.7.1_all.deb

上記コマンドで、px4_drv の Debian パッケージをインストールできます。

> [!TIP]  
> 手動で Debian パッケージを生成することもできます。  
> `./build_deb.sh` を実行すると、`./build_deb.sh` の一つ上層のディレクトリに `px4-drv-dkms_0.7.1_all.deb` という名前の Debian パッケージが生成されます。  
> ```
> $ ./build_deb.sh
> $ sudo apt install -y ../px4-drv-dkms_0.7.1_all.deb
> ```
> 上記コマンドで、生成した px4_drv の Debian パッケージをインストールできます。

#### DKMS を使用してインストールする

gcc, make, カーネルソース/ヘッダ, dkms がインストールされている必要があります。

	$ sudo cp -a ./ /usr/src/px4_drv-0.7.1
	$ sudo dkms add px4_drv/0.7.1
	$ sudo dkms install px4_drv/0.7.1

#### DKMS を使用せずにインストールする

事前に gcc, make, カーネルソース/ヘッダがインストールされている必要があります。

> [!WARNING]  
> DKMS を使用せずにインストールした場合、**カーネルのアップデート時にドライバが自動で再ビルドされないため、アップデート後に再度インストールを行う必要があります。**  
> **可能な限り DKMS を使用してインストールすることをおすすめします。**

	$ cd driver
	$ make
	$ sudo make install
	$ cd ../

### 3. 確認

#### 3.1 カーネルモジュールのロードの確認

**下記のコマンドを実行し、`px4_drv` から始まる行が表示されれば、カーネルモジュールが正常にロードされています。**

	$ lsmod | grep -e ^px4_drv
	px4_drv                81920  0

**何も表示されない場合は、下記のコマンドでカーネルモジュールを明示的にロードしてから、再度上記のコマンドを実行して確認を行ってください。**

	$ sudo modprobe px4_drv

それでもカーネルモジュールが正常にロードされない場合は、インストールから再度やり直してください。

> [!IMPORTANT]  
> **px4_drv や Linux カーネルの更新後、px4_drv は自動再ロードされない場合があります。**  
> **px4_drv や Linux カーネルを更新した際は、可能な限りすぐに PC を再起動することを強く推奨します。**   
> すぐに再起動ができない状況では、必ず `sudo modprobe px4_drv` を実行してください。  
> (当然ですが、録画予約のある時間帯に px4_drv や Linux カーネルを更新するべきではありません。)

#### 3.2 デバイスファイルの確認

インストールに成功し、カーネルモジュールがロードされた状態でデバイスが接続されると、`/dev/` 以下にデバイスファイルが作成されます。  
下記のようなコマンドで確認できます。

##### PLEX PX-W3U4/W3PE4/W3PE5 を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video1  /dev/px4video2  /dev/px4video3

チューナーは、`px4video0` から ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、SとTが2つずつ交互に割り当てられます。

##### PLEX PX-Q3U4/Q3PE4/Q3PE5 を接続した場合

	$ ls /dev/px4video*
	/dev/px4video0  /dev/px4video2  /dev/px4video4  /dev/px4video6
	/dev/px4video1  /dev/px4video3  /dev/px4video5  /dev/px4video7

チューナーは、`px4video0` から ISDB-S, ISDB-S, ISDB-T, ISDB-T, ISDB-S, ISDB-S, ISDB-T, ISDB-T というように、S と T が2つずつ交互に割り当てられます。

##### PLEX PX-MLT5PE / e-Better DTV02A-5TS-P を接続した場合

	$ ls /dev/pxmlt5video*
	/dev/pxmlt5video0  /dev/pxmlt5video2  /dev/pxmlt5video4
	/dev/pxmlt5video1  /dev/pxmlt5video3

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-MLT8PE を接続した場合

	$ ls /dev/pxmlt8video*
	/dev/pxmlt8video0  /dev/pxmlt8video3  /dev/pxmlt8video6
	/dev/pxmlt8video1  /dev/pxmlt8video4  /dev/pxmlt8video7
	/dev/pxmlt8video2  /dev/pxmlt8video5

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-M1UR を接続した場合

	$ ls /dev/pxm1urvideo*
	/dev/pxm1urvideo0

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

##### PLEX PX-S1UR を接続した場合

	$ ls /dev/pxs1urvideo*
	/dev/pxs1urvideo0

すべてのチューナーにおいて、ISDB-T のみ受信可能です。

##### e-Better DTV03A-1TU を接続した場合

	$ ls /dev/isdbt2071video*
	/dev/isdbt2071video0

すべてのチューナーにおいて、ISDB-T のみ受信可能です。

##### e-Better DTV02-1T1S-U/DTV02A-1T1S-U を接続した場合

	$ ls /dev/isdb2056video*
	/dev/isdb2056video0

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

> [!NOTE]  
> ロット番号 2309 以降の DTV02A-1T1S-U を接続した場合でも、デバイスファイル名は `/dev/isdb2056video*` となります。  
> `/dev/isdb2056nvideo*` ではないので注意してください。

##### e-Better DTV02A-4TS-P を接続した場合

	$ ls /dev/isdb6014video*
	/dev/isdb6014video0  /dev/isdb6014video2
	/dev/isdb6014video1  /dev/isdb6014video3

すべてのチューナーにおいて、ISDB-T と ISDB-S のどちらも受信可能です。

### 4. 内蔵 B-CAS カードリーダーの利用

**v0.7.0 以降、pcscd 用ドライバ (`px4-drv-pcsc`) を別途インストールすることで、内蔵カードリーダーを外付けカードリーダー同様に利用できるようになりました！**  
内蔵 B-CAS カードリーダーを使わない場合は、この手順は不要です。

> [!NOTE]  
> Mirakurun などを Docker 環境で稼働させている場合は、pcscd をホストとコンテナのどちらで動かすかによって、ドライバのインストール先が異なります。  
> 先に [Docker 環境からカードリーダーにアクセスする場合](#docker-環境からカードリーダーにアクセスする場合) をお読みください。 

#### pcscd 用ドライバ (px4-drv-pcsc) のインストール

Releases から、CPU アーキテクチャに合った Debian パッケージ (amd64 か arm64) をダウンロードし、pcscd と一緒にインストールします。  
Ubuntu 20.04 以降と、同じ世代以降の Debian 系ディストリビューションで動作します。

	$ wget https://github.com/tsukumijima/px4_drv/releases/download/v0.7.1/px4-drv-pcsc_0.7.1_amd64.deb
	$ sudo apt install -y ./px4-drv-pcsc_0.7.1_amd64.deb pcscd
	$ sudo udevadm trigger --subsystem-match=px4card
	$ sudo udevadm settle
	$ sudo systemctl restart pcscd

`udevadm` の2行は、すでに接続しているチューナーのカードリーダーを、新しくインストールした pcscd から開けるようにするためのものです。

> [!IMPORTANT]  
> 基本的に、異なるバージョンの px4_drv と px4-drv-pcsc を混在させて利用することはできません。  
> px4_drv と px4-drv-pcsc の更新後は、必ず pcscd を再起動してください。

> [!IMPORTANT]  
> **PX4 系・PX-MLT 系チューナーでは、カードの抜き差しを検出するため、pcscd が内蔵カードリーダーを認識している間は、チューナーを使っていなくてもチューナー本体に給電されます。**  
> Ubuntu や Debian 標準の pcscd は、カードリーダーを使用するアプリケーションの終了から約1分で自動停止するため、給電もあわせて停止します。
> 
> 一方、 **Docker コンテナ内などで pcscd を常駐させている場合は、pcscd の動作中は給電が継続します。**  
> pcscd を Docker コンテナで運用する際のチューナーへの常時給電を避けたい方は、pcscd をホスト側で実行してカードリーダー利用終了後に自動停止できるようにするか、内蔵カードリーダーが不要であれば px4-drv-pcsc 自体をコンテナ上から削除してください。
> 
> USB バスパワーで動作する DTV02A-1T1S-U / PX-M1UR / DTV03A-1TU などでは、受信回路に給電されるのはチューナーを使っている間だけです (DTV03A-1TU は実機での動作を確認できていません)。

> [!TIP]  
> ソースからビルドしてインストールすることもできます。  
> C++17 に対応したコンパイラー、make 、pkg-config 、pcsc-lite の開発用ヘッダー (Debian / Ubuntu では `libpcsclite-dev`) が必要です。  
> ```
> $ make -C pcsc
> $ sudo make -C pcsc install
> ```

#### 内蔵カードリーダーの認識を確認

B-CAS カードを挿した状態で、`pcsc-tools` の `pcsc_scan` を実行します。

	$ sudo apt install -y pcsc-tools
	$ pcsc_scan

次のように、内蔵カードリーダーの名前の下に `Card inserted` と、末尾に `B-CAS Card` の判定が表示されれば利用可能です。  
確認が完了したら、Ctrl+C で `pcsc_scan` を終了します。

```
 Reader 0: PLEX PX-W3U4 Smart Card Reader [ID: CD65353E4231AA77] (000001711009061) 00 00
  Event number: 0
  Card state: Card inserted, 
  ATR: 3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
(中略)
Possibly identified card (using /usr/share/pcsc/smartcard_list.txt):
3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99
	Japanese Chijou Digital B-CAS Card (pay TV)
```

> [!TIP]  
> リーダー名の `[ID: ...]` は、Windows 版のリーダー名の `(ID: ...)` と同じ値になるよう設計されています。  
> Windows と Linux の両方で、同じチューナー機器を共通の ID で識別できます。

#### 内蔵カードリーダーが認識されない場合

- **カードリーダーが表示されない:** `ls /dev/px4card/` で、カードリーダーのデバイスファイルが存在するかを確認してください。  
  デバイスファイルが存在しない場合は、インストールした px4_drv が内蔵カードリーダーに対応したバージョンかを確認してください。  
  デバイスファイルが存在する場合は、以下のコマンドを再度実行してください。  
  ```
  $ sudo udevadm trigger --subsystem-match=px4card
  $ sudo udevadm settle
  $ sudo systemctl restart pcscd
  ```
- **`Card state` が `Card inserted` にならない:** B-CAS カードの向き (裏表) と、奥まで挿し込まれているかを確認してください。
- **リーダー名に `[ID: ...]` が入っていない:** カーネルモジュールを読み込み直したときなどに発生します。pcscd を再起動することで解消されます。

それでもカードリーダーが表示されない場合は、`journalctl -u pcscd` で pcscd のログを確認してください。  
デバイスファイルのオープンに失敗した際には `open(...) failed` と、その理由が出力されます。

#### root 以外のサービスから使う場合 (Ubuntu 24.04 / Debian 13 以降)

Ubuntu 24.04 / Debian 13 以降の pcscd は、接続してきたプログラムを polkit で検証し、ログインしていないユーザーからの接続を拒否します。  
このため、Mirakurun / EDCB を一般ユーザー権限 (root 以外) の systemd サービスとして稼働させている場合、カードリーダーにアクセスできなくなります。

> [!NOTE]  
> この問題は内蔵カードリーダー固有の問題ではなく、外付けカードリーダーでも同様に発生します。  
> カードリーダーへのアクセスが拒否された場合、pcscd のログに `is NOT authorized for action: access_pcsc` が出力されます。
> なお、Ubuntu 22.04 / Debian 12 以前の pcscd は当該チェックを行わないため、この設定を記述する必要はありません。

カードリーダーへのアクセスが拒否される場合には、次の polkit ルールを `/etc/polkit-1/rules.d/50-pcscd-recorder.rules` に配置し、  
systemd サービスの起動ユーザー (この例では `mirakurun`) がカードリーダーにアクセスできるように設定してください。

```js
polkit.addRule(function(action, subject) {
    if ((action.id == "org.debian.pcsc-lite.access_pcsc" ||
         action.id == "org.debian.pcsc-lite.access_card") &&
        subject.user == "mirakurun") {
        return polkit.Result.YES;
    }
});
```

> [!NOTE]  
> recisdb などの受信ソフトウェアは、カードリーダーのデバイスファイルには直接アクセスせず、pcscd 経由で間接的にアクセスします。  
> チューナーデバイスの `video` グループと異なり、recisdb を実行するユーザーを `pcscd` グループに追加する必要はありません。

#### Docker 環境からカードリーダーにアクセスする場合

Docker 環境からカードリーダーにアクセスするには、ホストの pcscd を使うか、コンテナ内で pcscd を立ち上げるかの2つの方法があります。

- **コンテナ内で pcscd を動かす (Mirakurun の既定の構成):** コンテナのイメージにドライバを追加してビルドし直します
- **ホストの pcscd を使う:** イメージはそのままで済みますが、ホストとコンテナの pcsc-lite のバージョンが異なる場合は接続できないことがあります

> [!WARNING]  
> 事前に px4_drv のカーネルモジュールがホスト側にインストールされている必要があります。  
> ホストとコンテナの両方で pcscd を起動するとカードリーダーへのアクセスが競合し、どちらか一方が正常に動作しなくなってしまいます。

##### コンテナ内で pcscd を動かす

Mirakurun の公式イメージには pcscd 用ドライバが入っていないため、事前に px4-drv-pcsc の Debian パッケージを追加したイメージを作成します。  
起動用の `docker-compose.yml` は Mirakurun 公式のものをベースに、`image` だけをこのイメージに変更してください。  

```dockerfile
# arm64 環境では "amd64" を "arm64" に変更してください
FROM chinachu/mirakurun:latest
ADD https://github.com/tsukumijima/px4_drv/releases/download/v0.7.1/px4-drv-pcsc_0.7.1_amd64.deb /tmp/
RUN dpkg -i /tmp/px4-drv-pcsc_0.7.1_amd64.deb && rm /tmp/px4-drv-pcsc_0.7.1_amd64.deb
```

完了したら、ホストの pcscd を停止します。

	$ sudo systemctl disable --now pcscd.socket pcscd.service

> [!NOTE]  
> 全てのチューナー機器が取り外されると、`/dev/px4card` はディレクトリごと消滅します。  
> `/dev/px4card` だけをコンテナへマウントしてしまうと、USB を抜き差しした後にデバイスファイルがコンテナから見えなくなってしまう可能性があります。  
> 公式の Docker Compose に従い、`/dev` 全体をマウントしてください。

##### ホストの pcscd を使う

事前に pcscd 用ドライバをホストマシンに [インストール](#pcscd-用ドライバ-px4-drv-pcsc-のインストール) しておいてください。

その後、コンテナ内では pcscd を起動させず、代わりにホスト上の pcscd のソケットディレクトリをコンテナ内にマウントします。  
Mirakurun の場合は、環境変数 `DISABLE_PCSCD=1` を設定した上で、`/var/run/pcscd:/var/run/pcscd` をコンテナ内にマウントしてください。

> [!WARNING]  
> Mirakurun 公式の `docker-compose.yml` のようにソケットファイル (`pcscd.comm`) だけをマウントすると、ホストの pcscd が再起動した際にコンテナから接続できなくなります。  
> 必ず `/var/run/pcscd` ディレクトリごとコンテナへマウントしてください。  
> また、ホストとコンテナの pcsc-lite のバージョンが大きく異なると接続できないことがあります。その場合は、コンテナ内で pcscd を動かす構成を検討してください。

## インストール (macOS)

macOS 版は、ユーザー空間デーモン `DriverHost_PX4` として動作します。  
デーモンが起動すると、UNIX ドメインソケット (`/tmp/px4_ctrl_pipe.sock`, `/tmp/px4_data_pipe.sock`) 経由でチューナーの制御・TS データの取得が可能になります。

あわせて、デーモンのクライアントとして機能する `px4rec` コマンドも同時にビルドされます。  
`px4rec` は録画した TS データを stdout または指定ファイルに出力するシンプルなコマンドラインツールで、Linux の `recpt1` に相当する役割を担います。

受信ソケットの予期しない切断、出力の書き込み、B25 処理または受信停止が失敗した場合、`px4rec` はエラーを標準エラー出力へ記録し、非ゼロの終了コードを返します。指定時間の終了や終了シグナルによる停止は、後処理が成功すれば終了コード 0 です。
`DriverHost_PX4` は、USB の TS 受信転送がすべて異常終了して受信を続けられなくなった場合、受信済みのデータを送り終えてからデータソケットを閉じます。録画クライアントは無受信が続く状態ではなく切断として検知でき、`px4rec` は上記の予期しない切断として非ゼロで終了します。一部の転送だけが異常終了した場合は、残りの転送で受信を続けます。PX-W3U4 や PX-MLT5PE など、複数のチューナーが1つの USB 受信を共有する機種で異常終了した後は、全チューナーの受信を閉じるまで追加の受信開始が失敗し、`px4rec` は非ゼロで終了します。
`-i`、`-d` とチャンネル番号には整数を指定します。未知のオプションや余分な引数はエラーになり、`-` で始まる出力ファイル名は `./-name` のように指定できます。

`DriverHost_PX4` は、macOS のドライバから USB デバイスを切り離して取得するため、**root 権限 (`sudo`) で起動する必要があります**。
デーモンが作成するソケットは全ユーザーが接続できるため、`px4rec` や Mirakurun は一般ユーザーのままで利用できます。

`px4rec` は、制御ソケットに接続できない場合に、同じディレクトリに配置された `DriverHost_PX4` を自動起動してから再接続を試みます。
ただし、自動起動されるデーモンは `px4rec` と同じユーザーで動くため、`px4rec` を一般ユーザーで実行している場合はチューナーを取得できません。**デーモンはあらかじめ `sudo` または launchd で起動しておいてください。**

> [!IMPORTANT]
> **Linux の `/dev/px4video*` との互換性について**
>
> Linux ドライバは `ioctl()` によって制御するキャラクタデバイスを提供しますが、macOS ではユーザー空間からカーネル拡張なしに `/dev/*` デバイスノードを作成することはできません。  
> このため、`recpt1` や `recisdb` などの既存 Linux バイナリをそのまま macOS で使用することはできません。
>
> 代わりに、同梱の `px4rec` コマンドを `recpt1` の代わりに使用するか、`mirakurun` の `command:` 設定に `px4rec` を指定することで、同等の機能を利用できます。

### 1. 前提条件のインストール

[Homebrew](https://brew.sh/) がインストールされている必要があります。

	$ xcode-select --install            # Xcode コマンドラインツール (未インストールの場合)
	$ brew install cmake libusb

### 2. ソースコードの取得とビルド

リリースには、Apple Silicon 用 (`px4_drv_macos_arm64.tar.gz`) と Intel Mac 用 (`px4_drv_macos_x86_64.tar.gz`) のビルド済みアーカイブも添付されます。アーカイブの実行ファイルは Homebrew の libusb を利用するため、使う場合も `brew install libusb` が必要です。ソースからビルドする場合は次の手順に従ってください。

	$ git clone https://github.com/stuayu/px4_drv.git
	$ cd px4_drv
	$ cmake -S macos -B macos/build -DCMAKE_BUILD_TYPE=Release
	$ cmake --build macos/build

以降のコマンドは、すべてリポジトリのルート (`px4_drv/`) で実行します。

ビルドが成功すると `macos/build/` に次のファイルが生成されます。

| ファイル | 内容 |
| --- | --- |
| `DriverHost_PX4` | デーモン本体 |
| `px4rec` | 録画コマンド |
| `DriverHost_PX4.ini` | 設定ファイル (ビルド時に自動でコピーされる) |
| `isdbt_rio.inp` | Siano RIO 用ファームウェア (ビルド時に自動でコピーされる) |
| `LICENCE.siano` | Siano RIO 用ファームウェアの再配布条件 |

### 3. ファームウェアの配置

`it930x-firmware.bin` を `macos/build/` に置きます。ファームウェアは自動ではコピーされません。
リポジトリに同梱している抽出済みのファームウェアを使う場合は、次のコマンドだけで完了です。

	$ cp etc/it930x-firmware.bin macos/build/

公式ドライバから自分で抽出する場合は、同梱ファームウェアをコピーする代わりに次を実行します。

	$ make -C fwtool
	$ curl -L -o pxw3u4_BDA_ver1x64.zip http://plex-net.co.jp/plex/pxw3u4/pxw3u4_BDA_ver1x64.zip
	$ unzip -oj pxw3u4_BDA_ver1x64.zip pxw3u4_BDA_ver1x64/PXW3U4.sys && rm pxw3u4_BDA_ver1x64.zip
	$ ./fwtool/fwtool PXW3U4.sys macos/build/it930x-firmware.bin && rm PXW3U4.sys

### 4. 動作確認

インストールする前に、ビルドしたものが動くことを確認します。チューナーを接続し、`sudo` を付けてデーモンを起動します。

	$ sudo ./macos/build/DriverHost_PX4

`DriverHost_PX4 started. (resident)` と表示され、`libusb_claim_interface() failed` が出ていなければ、チューナーを取得できています。
デーモンを動かしたまま別のターミナルを開き、地上波の物理チャンネル (下の例では 27) を 10 秒録画します。

	$ cd px4_drv
	$ ./macos/build/px4rec -d 10 T 27 test.ts
	$ ls -l test.ts

`test.ts` が 20 MB 前後のサイズで作られていれば受信できています。チャンネル番号は、お住まいの地域で受信できる物理チャンネルに置き換えてください。確認が済んだら、デーモンを起動したターミナルで `Ctrl-C` を押して終了します。

### 5. デーモンの起動

常用する場合は、下の「[システムへインストールして launchd で起動](#システムへインストールして-launchd-で起動)」の手順で登録してください。Mac の起動時に root 権限で自動的に起動します。

`DriverHost_PX4` は root 権限で起動します。一般ユーザーで起動してもプロセスは動きますが、チューナーを取得できず、ログに `libusb_claim_interface() failed` と表示されます。

`px4rec` 実行時に `DriverHost_PX4` が起動していなければ、同じディレクトリにある `DriverHost_PX4` が自動起動されますが、`px4rec` と同じユーザーで動きます。`px4rec` を `sudo` で実行した場合に限り、自動起動されたデーモンがチューナーを取得でき、`px4rec` の終了後も常駐し続けます。

#### 手動起動

デーモンを手動で起動する場合は、`sudo` を付けて実行します。バックグラウンドで起動するとパスワードを入力できないため、先に `sudo -v` で認証を済ませておきます。

	$ sudo -v
	$ sudo ./macos/build/DriverHost_PX4 &

標準出力と標準エラー出力をログへ保存する場合は、次のように起動します。

	$ sudo nohup ./macos/build/DriverHost_PX4 \
	    > /tmp/DriverHost_PX4.log 2>&1 &

`DriverHost_PX4.ini`、`isdbt_rio.inp`、`LICENCE.siano` はビルド時に `macos/build/` へ自動でコピーされます。`it930x-firmware.bin` は実行ファイルと同じ `macos/build/` に手動で配置します。実行時のカレントディレクトリは実行ファイルのディレクトリへ変更されるため、別のディレクトリから絶対パスで起動しても、この配置関係は維持されます。

#### 起動オプション

| オプション | 説明 |
| --- | --- |
| `-t <秒>`, `--idle-timeout=<秒>` | 接続がない状態がこの秒数続いたときに終了する (既定値: `0` = 終了せず常駐) |
| `-r`, `--resident` | クライアントが接続していなくても終了せず常駐する (`--idle-timeout=0` と同じ。既定の動作) |
| `-h`, `--help` | 使い方を表示する |

既定では、クライアントが接続していなくてもデーモンは終了せず常駐します。

使っていないときはデーモンを終了させたい場合は、無接続のまま待つ秒数を指定します。次の例では、最後のクライアントが切断されてから 300 秒後に終了します。

	$ sudo ./macos/build/DriverHost_PX4 --idle-timeout=300 &

デーモンは `SIGINT` または `SIGTERM` で終了処理を開始し、受信の停止とソケットの削除を行ってから終了します。`kill -9` で停止すると、この後始末を行いません。

	$ sudo kill $(pgrep -f macos/build/DriverHost_PX4)

#### デーモンの動作

デーモンは次の UNIX ドメインソケットを作成します。

| 用途 | ソケット |
| --- | --- |
| チューナー制御 | `/tmp/px4_ctrl_pipe.sock` |
| TS データ取得 | `/tmp/px4_data_pipe.sock` |

ソケットのパーミッションは `0666` で、この Mac にログインできるユーザーであれば誰でもチューナーを利用できます。

既定では、接続の有無にかかわらずデーモンは終了しません。`--idle-timeout` に 1 以上の秒数を指定した場合に限り、最後のクライアントが切断されてから 1 秒間隔で接続数を確認し、無接続のまま指定した秒数が経過したときに終了します。制御接続またはデータ接続が 1 つでも残っている間は終了しません。複数の `px4rec` や Mirakurun の録画処理から同時に接続できます。

同じデーモンを複数起動することはできません。2つ目のプロセスはロックファイル `/tmp/px4_drv_host.lock` で既存プロセスを検出し、終了します。

制御・データ・カードソケットとロックファイルの配置先は `PX4_RUNTIME_DIR` で変更できます。
指定先のディレクトリはデーモン起動前に作成してください。未指定時は従来どおり `/tmp` を使用します。
recisdb-proxy から接続する場合は、`px4daemon:` の `@` に同じディレクトリの `px4_ctrl_pipe.sock` を指定し、
`PX4_CARD_SOCKET` に `px4_card_pipe.sock` のパスを設定してください。
`px4rec` も同じ `PX4_RUNTIME_DIR` を参照するので、デーモンと同じ値を設定して実行します。

#### システムへインストールして launchd で起動

ビルドした一式を root 所有のディレクトリへインストールし、LaunchDaemon として登録します。Mac の起動時に root 権限で起動し、ログインしていなくても動作します。異常終了したときは launchd が再起動します。

root で動かす実行ファイルを一般ユーザーが書き換えられる場所 (ホームディレクトリ以下の `macos/build/` など) に置いたまま登録すると、そのユーザーが root 権限を得られてしまいます。必ず root 所有のディレクトリへコピーしてください。

**1. ファイルのインストール**

	$ sudo mkdir -p /usr/local/px4_drv
	$ sudo cp macos/build/DriverHost_PX4 macos/build/px4rec macos/build/DriverHost_PX4.ini macos/build/isdbt_rio.inp macos/build/LICENCE.siano macos/build/it930x-firmware.bin /usr/local/px4_drv/
	$ sudo chown -R root:wheel /usr/local/px4_drv
	$ sudo chmod 755 /usr/local/px4_drv /usr/local/px4_drv/DriverHost_PX4 /usr/local/px4_drv/px4rec
	$ sudo chmod 644 /usr/local/px4_drv/DriverHost_PX4.ini /usr/local/px4_drv/isdbt_rio.inp /usr/local/px4_drv/LICENCE.siano /usr/local/px4_drv/it930x-firmware.bin

**2. LaunchDaemon の作成**

次のコマンドをそのまま貼り付けると、`/Library/LaunchDaemons/io.px4.DriverHost_PX4.plist` が作成されます。

	$ sudo tee /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist > /dev/null <<-'EOF'
	<?xml version="1.0" encoding="UTF-8"?>
	<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
	  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
	<plist version="1.0">
	<dict>
	  <key>Label</key>
	  <string>io.px4.DriverHost_PX4</string>
	  <key>ProgramArguments</key>
	  <array>
	    <string>/usr/local/px4_drv/DriverHost_PX4</string>
	  </array>
	  <key>WorkingDirectory</key>
	  <string>/usr/local/px4_drv</string>
	  <key>RunAtLoad</key>
	  <true/>
	  <key>KeepAlive</key>
	  <dict>
	    <key>SuccessfulExit</key>
	    <false/>
	  </dict>
	  <key>ThrottleInterval</key>
	  <integer>30</integer>
	  <key>StandardOutPath</key>
	  <string>/var/log/DriverHost_PX4.log</string>
	  <key>StandardErrorPath</key>
	  <string>/var/log/DriverHost_PX4.error.log</string>
	</dict>
	</plist>
	EOF

LaunchDaemon の plist は、所有者が root、パーミッションが `644` でなければ読み込まれません。

	$ sudo chown root:wheel /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist
	$ sudo chmod 644 /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist
	$ plutil -lint /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist

**3. 登録と起動**

手動で起動したデーモンが残っている場合は、先に停止します。

	$ sudo pkill DriverHost_PX4
	$ sudo launchctl bootstrap system /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist

**4. 確認**

	$ sudo launchctl print system/io.px4.DriverHost_PX4 | grep -E 'state|pid|last exit'
	$ ls -l /tmp/px4_ctrl_pipe.sock /tmp/px4_data_pipe.sock
	$ cat /var/log/DriverHost_PX4.log /var/log/DriverHost_PX4.error.log
	$ /usr/local/px4_drv/px4rec -d 10 T 27 test.ts

`state = running` と表示され、ソケットが 2 つ存在し、`px4rec` で録画できれば完了です。以降は Mac を再起動しても自動的に起動します。

デーモンは既定で常駐するため、起動オプションは不要です。`KeepAlive` の `SuccessfulExit` を `false` にしているので、launchd が再起動するのは異常終了したときだけです。`kill` による正常終了や、すでに別の `DriverHost_PX4` が動いていて二重起動を検出した場合 (どちらも終了コード 0) は再起動しません。`KeepAlive` を単に `true` にすると、手動起動したデーモンが残っている間、launchd が起動と終了を繰り返します。`ThrottleInterval` は、ファームウェアの配置漏れなどで起動に失敗し続けたときの再起動間隔 (秒) です。

- plist を書き換えたときは、いったん `bootout` してから `bootstrap` し直します
- デーモンだけを再起動するときは `sudo launchctl kickstart -k system/io.px4.DriverHost_PX4` を使います
- ビルドし直したときは、「1. ファイルのインストール」のコマンドをもう一度実行してから再起動します
- Mirakurun の `command:` には `/usr/local/px4_drv/px4rec` を指定します

停止と登録解除:

	$ sudo launchctl bootout system/io.px4.DriverHost_PX4

録画中に停止すると TS が途中で終了します。録画処理を先に終了し、クライアントが切断されたことを確認してから停止します。
#### tmux で起動

ターミナルを閉じても動作させ、ログを確認したい場合は tmux を使用します。

	$ tmux new-session -d -s px4-driver \
	    'sudo ./macos/build/DriverHost_PX4 2>&1 | tee /tmp/DriverHost_PX4.log'
	$ tmux attach -t px4-driver   # パスワードを求められていたら入力する

デタッチは `Ctrl-b` の後に `d` です。セッションとデーモンの状態は次で確認できます。

	$ tmux has-session -t px4-driver
	$ ls -l /tmp/px4_ctrl_pipe.sock /tmp/px4_data_pipe.sock

デーモンは既定で常駐するため、クライアントが切断されてもソケットは残ります。`--idle-timeout` を指定して起動した場合に限り、無接続が続くとデーモンが終了してソケットが消えます。

#### 起動確認とトラブルシューティング

ソケットだけでなく、プロセスとログを確認します。

	$ pgrep -af DriverHost_PX4
	$ ls -l /tmp/px4_ctrl_pipe.sock /tmp/px4_data_pipe.sock
	$ tail -f /tmp/DriverHost_PX4.log

ソケットが存在してもプロセスが動いていなければ、前回の異常終了で残ったソケットの可能性があります。プロセスが存在しないことを確認してから、対象ファイルだけを削除し、再起動します。

	$ sudo rm -f /tmp/px4_ctrl_pipe.sock /tmp/px4_data_pipe.sock /tmp/px4_drv_host.lock
	$ sudo ./macos/build/DriverHost_PX4

起動時に `DriverHost_PX4.ini` または `it930x-firmware.bin` を読み込めない場合は、配置とファイル名を確認します。USB デバイスを認識できない場合は、USB 接続、対応機種、ログのエラーを確認します。

ログに `libusb_claim_interface() failed: LIBUSB_ERROR_ACCESS` や「USB デバイスを取得する権限がありません」と表示される場合は、デーモンを root 権限で起動していません。いったん停止し、`sudo` を付けて起動し直してください。`could not open lock file` と表示される場合は、別のユーザー (root) で起動したデーモンがすでに動いています。

### 6. 録画 (px4rec)

`px4rec [options] T|S|C <channel> [output|-]` で TS データを録画できます。macOS 版の ISDB-C は実験的な対応です。選局と TSMF の TS の選択は WinUSB 版と共通の処理を使いますが、macOS での長時間受信は未検証です。

> [!NOTE]
> 接続されているチューナーが対応していない系統を指定した場合は `OPEN failed` で終了します。  
> また、アンテナが未接続・無信号・受信条件不一致の場合は `TUNE failed` で終了します。

	# 地上波 (ISDB-T) 27ch を 60 秒録画してファイルに保存
	$ ./macos/build/px4rec -d 60 T 27 output.ts

	# 衛星受信対応チューナーで、BS トランスポンダ 3 を stdout に出力 (パイプで他ツールへ渡す場合)
	$ ./macos/build/px4rec S 3 - | ffplay -i -

	# B-CAS カードで B25 デスクランブルを行いながら録画 (--b25 オプション)
	$ ./macos/build/px4rec --b25 -d 60 T 13 output_decoded.ts

	# CATV C13 を 60 秒録画 (ISDB-C、変調方式は復調器が判別)
	$ ./macos/build/px4rec -d 60 C 13 cable.ts

	# 中心周波数を直接指定し、補正値を加える
	$ ./macos/build/px4rec --freq-khz 473000 --freq-offset -1 C 13 relay.ts

	# TSMF で多重された C52 の2本目の TS (TS1) を録画
	$ ./macos/build/px4rec --tsmf 1 C 52 stream.ts

オプション一覧:

| オプション | 説明 |
| --- | --- |
| `-i N` | 使用するチューナーのインデックス (0 始まり、デフォルト: -1 = 最初の空きチューナー) |
| `-d N` | 録画時間を秒で指定 (省略すると SIGTERM/Ctrl+C まで継続) |
| `--b25` | B-CAS カードを使って ARIB STD-B25 のデスクランブル処理を行う (macOS の PC/SC フレームワーク経由) |
| `--freq-khz kHz` | C 専用。中心周波数を直接指定 (1〜4294967295 kHz)。指定時はチャンネル番号からの周波数計算が不要 |
| `--freq-offset kHz` | C 専用。中心周波数への補正値 (符号付き32bit整数、既定 0)。補正後は1〜4294967295 kHzが必要 |
| `--tsmf 0-14` | C 専用。TSMF のヘッダーに載っている有効な TS のうち、0 から数えて何番目を受信するか (既定 0) |
| `--help` / `-h` | usage を表示 |
| `T` / `S` / `C` | ISDB-T (地上波) / ISDB-S (衛星) / ISDB-C (CATV トラモジ) |
| チャンネル | T: 物理チャンネル番号 13–62 / S: トランスポンダ番号 0–11 (BS), 12–23 (CS110) / C: CATV Cチャンネル番号 13–63 |
| 出力先 | ファイルパス、または `-` (標準出力、省略時も標準出力) |

ISDB-C の C13〜C22 は物理チャンネル3〜12、C23〜C63 は物理チャンネル22〜62に対応します。中心周波数は `93000 + 物理チャンネル × 6000` kHzで、物理チャンネル12だけ2000 kHzを加算します。UHF/VHF帯の再送信などでは `--freq-khz` で中心周波数を指定できます。直接指定時も Cチャンネル番号13〜63が必要です。

C 専用オプションを T/S と併用した場合と、値が範囲外の場合は、選局前にエラーになります。TSMF を使わない搬送波では `--tsmf 0` (既定) だけが受信でき、TS1 以降の指定は選局に失敗します。TSMF のヘッダーで分割 TLV と示された TS は、TLV を取り出して出力するため、TS 用の復号処理である `--b25` とは併用できません。

> [!NOTE]
> `--b25` を使用する場合は、macOS に接続された PC/SC 対応 IC カードリーダーに B-CAS カードを挿入してください。  
> macOS の PC/SC フレームワーク (`PCSC.framework`) を使用するため、追加のドライバやライブラリは不要です。

### 7. mirakurun との連携

`mirakurun` の設定ファイル (`tuners.yml`) に以下のように記述することで、mirakurun 経由で EPGStation や Chinachu から利用できます。

`DriverHost_PX4` はあらかじめ launchd (または `sudo`) で起動しておき、`command:` には `px4rec` の絶対パスを指定してください。
Mirakurun を一般ユーザーで動かしている場合、`px4rec` からの自動起動ではチューナーを取得できません。

```yaml
- name: PX4-T1
  types:
    - GR
  command: /path/to/px4rec T <ch>

- name: PX4-S1
  types:
    - BS
    - CS
  command: /path/to/px4rec -i 1 S <ch>
```

### 8. 確認

デーモンが起動すると、UNIX ドメインソケットが作成されます。

	$ ls /tmp/px4_ctrl_pipe.sock /tmp/px4_data_pipe.sock
	/tmp/px4_ctrl_pipe.sock  /tmp/px4_data_pipe.sock

ソケットファイルが存在していれば、デーモンは起動しています。チューナーを取得できているかは、ログに `libusb_claim_interface() failed` が出ていないことで確認します。

## アンインストール (macOS)

### 1. デーモンの停止

launchd に登録している場合は、先に登録を解除します。

	$ sudo launchctl bootout system/io.px4.DriverHost_PX4
	$ sudo rm /Library/LaunchDaemons/io.px4.DriverHost_PX4.plist

実行中の `DriverHost_PX4` プロセスを停止します。

	$ sudo pkill DriverHost_PX4

### 2. ファイルの削除

	$ rm -rf macos/build
	$ sudo rm -rf /usr/local/px4_drv   # launchd 用にコピーした場合

## アンインストール (Windows)

### 1. ドライバのアンインストール

デバイスマネージャーからチューナーデバイス（ PLEX PX-W3PE5 ISDB-T/S Receiver Device (WinUSB) のような名前）を選択します。  
その後、右クリックメニュー → [デバイスのアンインストール] から、[このデバイスのドライバー ソフトウェアを削除します] にチェックを入れて、ドライバをアンインストールしてください。

### 2. 自己署名証明書のアンインストール

ドライバのインストール時にインストールした自己署名証明書をアンインストールするには、cert-uninstall.jse を実行します。アンインストールするとドライバが信頼されなくなってしまうので、十分注意してください。

## アンインストール (Linux)

### 1. ドライバのアンインストール

#### Debian パッケージを使用してインストールした場合

	$ sudo apt purge px4-drv-dkms

#### DKMS を使用してインストールした場合

	$ sudo dkms remove px4_drv/0.7.1 --all
	$ sudo rm -rf /usr/src/px4_drv-0.7.1

#### DKMS を使用せずにインストールした場合

内蔵カードリーダーを pcscd で使っている場合は、pcscd がデバイスファイルを開いたままでモジュールを外せないため、先に pcscd を止めます。

	$ sudo systemctl stop pcscd.socket pcscd.service
	$ sudo modprobe -r px4_drv
	$ cd driver
	$ sudo make uninstall
	$ cd ../

### 2. ファームウェアのアンインストール

> [!NOTE]  
> **px4_drv を Debian パッケージや DKMS を使用してインストールした場合、ファームウェアは自動的にアンインストールされます。**  
> Debian パッケージや DKMS を使用してインストールを行った場合は、この手順はスキップしてください。

	$ sudo rm /lib/firmware/it930x-firmware.bin

## 受信方法

### Windows

チューナーの機種に応じた BonDriver とその設定ファイルを配置し、TVTest や EDCB などの BonDriver に対応したソフトウェアで使用することで、TS データを受信することが可能です。

BonDriver は専用のものが必要になるため、公式 (Jacky版) BonDriver や radi-sh 氏版 BonDriver_BDA と併用することはできません。

### macOS

`DriverHost_PX4` デーモンを起動した状態で、または `px4rec` に同梱された自動起動機能を通じて、UNIX ドメインソケット (`/tmp/px4_ctrl_pipe.sock`, `/tmp/px4_data_pipe.sock`) を経由し、Windows WinUSB 版と同一のコマンドプロトコルで通信することで、TS データを受信することが可能です。

macOS のソケットはバイトストリームのため、コマンドは受信単位ではなくコマンド種別と要素数から求めた長さで区切られます。コマンド構造体は macOS 側の ABI を使用し、`wchar_t` を含む構造体のバイト配置は Windows と異なります。

### Linux

recpt1 や [BonDriverProxy_Linux](https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux) 等の PT シリーズ用 chardev ドライバに対応したソフトウェアを使用することで、TS データを受信することが可能です。  
recpt1 は、PLEX 社より配布されているものを使用する必要はありません。

BonDriverProxy_Linux と、PLEX PX-MLT5PE や e-Better DTV02A-1T1S-U などのデバイスファイル1つで ISDB-T と ISDB-S のどちらも受信可能なチューナーを組み合わせて使用する場合は、BonDriver として BonDriverProxy_Linux に同梱されている BonDriver_LinuxPT の代わりに、[BonDriver_LinuxPTX](https://github.com/nns779/BonDriver_LinuxPTX) を使用してください。

## ケーブルテレビ (ISDB-C) / スカパー！プレミアムサービス光 (J.83B) の受信

**v0.7.0 以降、PX-MLT 系チューナー (PX-MLT5PE / PX-MLT8PE / DTV02A-4TS-P / DTV02A-5TS-P) なら、地上波や BS/CS に加え、ケーブルテレビ (ISDB-C) とスカパー！プレミアムサービス光 (J.83 Annex B) を受信できるようになりました！**

- ISDB-C (J.83 Annex C): ケーブルテレビ局が地上波・地上波 (区域外再送信)・BS・専門チャンネルをトランスモジュレーション方式で再送信する放送
  - 実装にあたり、実際に PX-MLT5PE とケーブルテレビ（イッツコム）の環境で動作確認を行っています。
- J.83 Annex B: フレッツ・テレビなどの光回線経由で送られる「スカパー！プレミアムサービス光」の放送
  - スカパー！プレミアムサービス光は、ユーザーの環境でチャンネルスキャン・受信が正常に行えたと報告を受けています。

### Windows

`BonDriver_PX-MLT.dll` は、既定では従来通り、地上波・BS/CS のみが受信対象として設定されています。  
ケーブルテレビを受信するには、`BonDriver_PX-MLT.ini` → `[BonDriver]` → `System` に `ISDB-C` を、  
スカパー！プレミアムサービス光を受信するには `J.83B` を明示的に追加してください (例: `System="ISDB-T,ISDB-S,ISDB-C"`)。

```ini
[BonDriver]
Name="PX-MLT"
; ケーブルテレビ (ISDB-C) を受信する際は 2つ目、スカパー！プレミアムサービス光 (J.83B) を受信する際は
; 3つ目の System の行の先頭の ; を外し、1つ目の行の先頭に ; を付ける
System="ISDB-T,ISDB-S"
;System="ISDB-T,ISDB-S,ISDB-C"
;System="ISDB-T,ISDB-S,J.83B"
...
```

> [!TIP]  
> EDCB (EpgDataCap_Bon) などの「全チューニング空間の全チャンネルを順に選局して一括でチャンネルスキャンを行う」受信ソフトウェアでは、実際には全て受信できないチューニング空間であっても、チャンネルスキャン時に毎回選局されます。  
> ケーブルテレビのチャンネル定義は約450行、スカパー！プレミアムサービス光は約110行あり、受信できない環境では1チャンネルごとに受信を待ってから次へ進む形となるため、既定でこれらのチューニング空間を有効にすると、チャンネルスキャンに非常に時間が掛かります。  
> こうした理由により、アンテナで地上波と BS/CS だけを受信している大半のユーザーのチャンネルスキャンまで遅くなってしまうことから、ISDB-C / J.83B は既定では無効化されています。

> [!NOTE]  
> ケーブルテレビでは、1つの物理チャンネルで複数の放送をまとめて送出していることがあります (TSMF 方式)。  
> チャンネル定義にある「C52Ch/TS1」のような行は、こうした TSMF 方式で送出されているチャンネルの、2本目以降の TS を受信するためのものです。  
> 当該物理チャンネルに該当する TS がなければ、チャンネルスキャン時に自動的に除外されます。

> [!NOTE]  
> BS4K が再送信されているケーブルテレビにおいて 4K 放送を受信するには、別途 [BonDriver_dantto4k](https://github.com/nekohkr/dantto4k) と組み合わせてご利用ください。

### Linux

[recisdb](https://github.com/kazuki0824/recisdb-rs) を使い、次のようにチャンネルを指定して ISDB-C / J.83B を受信できます。  

- `ISDBC-C52` (ケーブルテレビ: C52)、`ISDBC-20` (ケーブルテレビ: UHF 20ch)
- `ISDBC-C52_1` (ケーブルテレビ: C52 の2本目の TS (TSMF))
- `J83B-H001` 〜 `J83B-H105`、`J83B-374950` (スカパー！プレミアムサービス光: 番号か中心周波数 (kHz) で指定)

受信ソフトウェアを開発する方向けの ioctl 仕様は、[ISDB-C と J.83 Annex B の選局の仕様](docs/isdbc_tuning.md) に記載されています。

### macOS

`px4rec` の `C` でケーブルテレビ (ISDB-C) を選局できます。TSMF で多重された2本目以降の TS は `--tsmf` で指定します。指定方法は [録画 (px4rec)](#6-録画-px4rec) を参照してください。J.83B の選局は `px4rec` では未対応です。

### MyGica T230C

MyGica T230C は ISDB-C だけを受信できます (実験的)。チャンネル定義の形式は PX-MLT 系と同じで、Windows では `BonDriver_T230C` を使います。Si2168 の公開ドライバに ISDB-C (J.83 Annex C) の設定がないため、DVB-C 6 MHz の設定で受信しています。詳細は [T230C の説明](docs/t230c.md) を参照してください。

> [!WARNING]  
> 2026/10/29 現在、recisdb の ISDB-C / J.83B サポートは [kazuki0824/recisdb-rs#202](https://github.com/kazuki0824/recisdb-rs/pull/202) にて開発中です。  
> recisdb の新バージョンがリリースされるまでは、こちらのブランチを自ビルドした上でご利用ください。

## LNB電源の出力

### PLEX PX-W3U4/Q3U4/W3PE4/Q3PE4

出力なしと 15V の出力のみに対応しています。デフォルトでは LNB 電源の出力を行いません。  
LNB 電源の出力を行うには、recpt1 を実行する際のパラメータに `--lnb 15` を追加してください。

Windows では、BonDriver_PX4-S.ini に記載の `LNBPower=0` を `LNBPower=1` に変更してください。

### PLEX PX-W3PE5/Q3PE5

出力なしと 15V の出力のみに対応しているものと思われます。

### PLEX PX-MLT5PE/MLT8PE

対応していないものとされていましたが、5ch の有志により、正しく LNB 電源を出力できることが確認されています ([参考](https://mevius.5ch.net/test/read.cgi/avi/1648542476/267-288))。

### PLEX PX-M1UR / e-Better DTV02-1T1S-U/DTV02A-1T1S-U

対応しておりません。

### e-Better DTV02A-4TS-P

対応していると思われます。

### e-Better DTV02A-5TS-P

ハードウェア上 PX-MLT5PE と同一機種であることから、LNB 電源の動作も PX-MLT5PE と共通です。

## 備考

### 内蔵カードリーダーやリモコンについて

WinUSB 版と Linux 版のどちらでも、各種対応デバイスに内蔵されているカードリーダーを利用できます (Linux 版は pcsc-lite 用の [IFD ハンドラー](pcsc/README.md) の導入が必要です)。リモコンの操作には対応していません。

### e-Better DTV02-1T1S-U について

e-Better DTV02-1T1S-U は、個体によりデバイスからの応答が無くなることのある不具合が各所にて多数報告されています。そのため、このドライバでは「実験的な対応」とさせていただいております。  
上記の不具合はこの非公式ドライバでも完全には解消できないと思われますので、その点は予めご了承ください。

### e-Better DTV02A-1T1S-U について

e-Better DTV02A-1T1S-U は、DTV02-1T1S-U に存在した上記の不具合がハードウェアレベルで修正されています。そのため、このドライバでは「正式な対応」とさせていただいております。

## 技術情報

### デバイスの構成

PX-W3PE4/Q3PE4/MLT5PE/MLT8PE, e-Better DTV02A-4TS-P/DTV02A-5TS-P は、電源の供給を PCIe スロットから受け、データのやり取りを USB を介して行います。  
PX-W3PE5/Q3PE5 は、PX-W3PE4/Q3PE4 相当の基板に PCIe→USB ブリッジチップを追加し、USB ケーブルを不要とした構造となっています。  
PX-Q3U4/Q3PE4 は、PX-W3U4/W3PE4 相当のデバイスが USB ハブを介して2つぶら下がる構造となっています。

- PX-W3U4/W3PE4

	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Toshiba TC90522XBG
	- Terrestrial Tuner: RafaelMicro R850 (x2)
	- Satellite Tuner: RafaelMicro RT710 (x2)

- PX-Q3U4/Q3PE4

	- USB Bridge: ITE IT9305E (x2)
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (x2)
	- Terrestrial Tuner: RafaelMicro R850 (x4)
	- Satellite Tuner: RafaelMicro RT710 (x4)

- PX-W3PE5

	- PCIe to USB Bridge: ASIX MCS9990CV-AA
	- USB Bridge: ITE IT9305E
	- ISDB-T/S Demodulator: Toshiba TC90522XBG
	- Terrestrial Tuner: RafaelMicro R850 (x2)
	- Satellite Tuner: RafaelMicro RT710 (x2)

- PX-Q3PE5

	- PCIe to USB Bridge: ASIX MCS9990CV-AA
	- USB Bridge: ITE IT9305E (x2)
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (x2)
	- Terrestrial Tuner: RafaelMicro R850 (x4)
	- Satellite Tuner: RafaelMicro RT710 (x4)

Demodulator として CXD2856ER が搭載されたデバイスは、ISDB-T/S だけでなく、ISDB-C / J.83 Annex B の受信にも対応しています。  
PX-MLT8PE は、同一基板上に PX-MLT5PE 相当のデバイスと、3チャンネル分のチューナーを持つデバイスが実装されている構造となっています。

- PX-MLT5PE/MLT8PE5 / DTV02A-5TS-P

	- USB Bridge: ITE IT9305E
	- ISDB-T/S/C Demodulator: Sony CXD2856ER (x5)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x5)

- PX-MLT8PE3

	- USB Bridge: ITE IT9305E
	- ISDB-T/S/C Demodulator: Sony CXD2856ER (x3)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x3)

DTV02-1T1S-U/DTV02A-1T1S-U は、ISDB-T 側の TS シリアル出力を ISDB-S 側と共有しています。そのため、同時に受信できるチャンネル数は1チャンネルのみです。

- DTV02-1T1S-U/DTV02A-1T1S-U (ロット番号 2309 以前: Digibest ISDB2056)

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90532XBG
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

ロット番号 2309 (2023年9月) 以降の DTV02A-1T1S-U では、ISDB-T/S Demodulator IC が TC90532XBG (ISDB-T×1TS + ISDB-S×1TS) から TC90522XBG (ISDB-T×2TS + ISDB-S×2TS) に変更されています。（[詳細](https://web.archive.org/web/20130513083035/http://www.semicon.toshiba.co.jp/product/new_products/assp/1275558_37644.html)）  
この変更に伴い、内部名称が ISDB2056 から ISDB2056N に変更されています。

- DTV02A-1T1S-U (ロット番号 2309 以降: Digibest ISDB2056N)

	- USB Bridge: ITE IT9303FN
	- ISDB-T/S Demodulator: Toshiba TC90522XBG (TC90532XBG -> TC90522XBG)
	- Terrestrial Tuner: RafaelMicro R850
	- Satellite Tuner: RafaelMicro RT710

DTV02A-4TS-P は、PX-MLT5PE から1チャンネル分のチューナーを削減した構造となっています。

- DTV02A-4TS-P (Digibest ISDB6014)

	- USB Bridge: ITE IT9305E
	- ISDB-T/S/C Demodulator: Sony CXD2856ER (x4)
	- Terrestrial/Satellite Tuner: Sony CXD2858ER (x4)

### TS Aggregation の設定

sync_byte をデバイス側で書き換え、ホスト側でその値を元にそれぞれのチューナーの TS データを振り分けるモードを使用しています。
