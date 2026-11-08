# リポジトリ再構成と品質向上の計画

この文書は、Linux 版・WinUSB 版・macOS 版の共通化、ログ機能、信号品質の計算、ISDB-C / J.83B 対応、受信開始までの時間の最適化、README の改定をまとめて進めるための計画です。
AGENTS.md の「構造的な修正に広い書き換えが必要な場合は、先に理由、維持する契約、移行方法、試験範囲を文書化する」に従い、実装の前に置きます。

## 2026-11-01 の再開後の進捗

### 既存ブランチで確認済み

- 基準固定、Linux ビルド修正、共通層・Linux 層・ホスト層への移動、factory 統合と機種定義照合試験、制御・配信サーバーの共有までの段階を完了しています。
- 各段階の当時のビルド結果は引き継ぎメモに記録されています。今回の変更を含む Windows / Linux ビルド結果としては扱っていません。

### 今回の作業ツリーに統合した範囲

- WinUSB / macOS 用のログレベル、カテゴリ、text / JSONL 出力設定と USB 転送履歴を統合しました。TS 統計と選局から最初の TS までの時間内訳を `StreamBuffer` へ追加し、出力は受信ロックの外で行います。USB 履歴の slot は atomic reservation で書き込みを直列化し、競合した記録を待たずに捨てます。並行 writer の整合性試験も追加しました。
- C/N の固定小数点換算を共通 C 層へ追加し、PX-MLT、PX4、ISDB2056、ISDBT2071 のホスト `ReadStat(CNR)` から利用するようにしました。Linux では PX-MLT の既存 CNR ioctl も共通換算を使います。CXD2856ER の ISDB-C ロック確認では、valid BER を ppb に換算して記録し、FEC 読み取りエラーを TS ロック失敗として返さない回帰試験も追加しました。
- ISDB-C / J.83B のロック確認は 20 ms 間隔で呼ばれるため、ロック中にも各回 FEC を読んでいた条件では模擬 I2C 要求が 50 回で 350 件でした。FEC と状態ログを 25 回ごとに制限し、同じ試験で 206 件を確認します。これは模擬バスの確認で、実機の選局時間や USB 占有時間を測った値ではありません。
- ISDB-C の自動 QAM 再試行で最初の試行の未完了タイミングが残る経路を `stream_buffer_test` で再現しました。修正前は再試行中の packet に2行の時間ログを付け、試行開始時に計測状態を破棄した後は1行だけになります。
- USB 履歴は固定長の原子スロットへ保存し、同じ slot への同時書き込みは atomic flag で直列化します。slot が使用中の場合は記録を待たず、そのイベントを落とします。周期統計は既定で無効です。有効時の整形と出力は呼び出し元スレッドで同期実行されるため、非同期ログ配送は未実装で、受信中のログ有効条件を含めた実測が残っています。
- `tools/tune_latency.py` は text / JSONL の `Tune timing` 行から、system と周波数ごとに選局設定・ロック・最初の TS packet の中央値、p95、最大値を集計します。混在ログと複数周波数のユニット試験、CLI の JSON 出力を確認しました。今回の再開作業では実機ログを取得していません。
- macOS の全 CMake ターゲットと CTest 21 件、Windows の x86 / x64 全ソリューション再ビルドと6種類のハードウェア非依存テストが成功しています。ログ履歴と選局計測の最終変更後も Windows `DriverHost_PX4` x86 / x64 と診断テストを再ビルドしました。Linux 6.8 のカーネルモジュールは隔離ツリーでビルドし、ロードしていません。これらは実機受信の証明ではありません。
- README に層構成、現在の OS 別機種状況、ログ設定、別紙の索引を追加し、Claude 用レビュー / 調査 skill を追加しました。

### まだ満たしていない要件

- 対応機種の OS 差は残っています。T230C 系 (`0572:c68a`, Linux の参照名では T230C2) の Linux 経路は追加しましたが、Linux 実機確認は未実施です。
- BER / PER は CXD2856ER ISDB-C の valid BER と Si2168 の参照根拠までです。TC90522 等のチップ読み出し、全方式での有効条件、統一公開統計 API は未完了です。CXD2856ER packet-error counter は denominator の意味が未確定なため raw count と valid bit の記録に留めます。誤り訂正設定の読み戻し、理論ビットレート、校正済み dBm 信号強度も未完了です。TS から測る入力ビットレートは診断ログへ出します。
- 受信開始時間は共通ホスト層のログで段階計測できますが、各機種の比較測定は未実施です。
- 今回の変更後に Linux 6.8 カーネルモジュールを隔離した一時ツリーでビルドし、Windows x86/x64 の全ソリューションを Visual Studio v145 で再ビルドし、macOS 全 CMake ターゲットと CTest 21 件を実行しました。各 OS の結果はビルド / 自動試験の確認で、実機受信を確認した範囲には含みません。

## 用語

- 共通層: OS に依存しない C のチップ制御とプロトコル処理。Linux カーネルモジュールとユーザー空間ホストの両方からコンパイルされる。
- ホスト層: WinUSB 版と macOS 版の DriverHost_PX4 が共有する C++ の device class、receiver、server。
- OS 層: USB 転送、スレッド、時刻、ファイル、ログ出力先、配布物など、OS ごとに書き分ける部分。
- 検証環境: Apple Silicon Mac に対象機種を接続し、CATV 回線、BS/CS IF、B-CAS カードを用意した構成。

## 1. 現状

### 1.1 ディレクトリ

| 場所 | 中身 | 問題 |
|---|---|---|
| `driver/` | チップ制御 (it930x, tc90522, r850, rt710, cxd2856er, cxd2858er, siano_*) と Linux カーネル固有部 (driver_module, ptx_chrdev, px4_usb, *_device.c) が同居 | 共通層と Linux 層の境界がファイル名でしか分からない |
| `winusb/src/DriverHost_PX4/` | WinUSB 固有部 (itedtv_bus_winusb.c, notify_icon, misc_win) と、macOS も使うホスト層が同居 | macOS 版が WinUSB 版のフォルダーを参照してビルドしている |
| `macos/src/` | libusb 実装、POSIX 互換層、サーバーの macOS 版、px4rec | サーバーは WinUSB 版と別実装で、修正が片方にしか入らない危険がある |
| `include/` | Linux 版のユーザー API (ioctl) | 名前から用途が分からない |
| `fwtool/`, `winusb/src/fwtool/` | ファームウェア抽出ツールが2か所 | 重複 |
| `macos/tests/`, `winusb/tests/` | OS ごとに試験が分散 | 共通層の試験が macOS 側にだけある |

### 1.2 OS ごとのデバイス対応

| 機種 (コード上の系統) | Linux | WinUSB | macOS | 差 |
|---|---|---|---|---|
| PX-W3U4 / Q3U4 / W3PE4 / Q3PE4 / W3PE5 / Q3PE5 (PX4) | ○ | ○ | ○ | なし |
| PX-MLT5PE / MLT8PE / ISDB6014 (PXMLT) | ○ | ○ | ○ | なし |
| DTV02-1T1S-U、ISDB2056N、PX-M1UR、PX-S1UR (ISDB2056) | ○ (M1UR/S1UR は別 device) | ○ (model 分岐) | ○ (model 分岐) | 実装構造の差 |
| ISDBT2071 | ○ | ○ | ○ | なし |
| MyGica T230C 系 (Si2168) | △ (経路実装済み、実機未確認) | △ (コードあり、実機未確認) | △ (コードあり、実機未確認) | 3 OS とも実機未確認。`0572:c68a` は T230C2 |
| PX-S1UD / Q1UD (Siano) | ○ | ○ | ○ | なし |

「OS 間で利用できるデバイスに差があってはならない」という要件に対し、T230C の各 OS での実機検証が残っています。

### 1.3 その他の不足

- ログ: `msg_printf` と `dev_print` に ERR / WARN / INFO / DBG があるが、出力先と粒度が OS ごとに違い、受信統計や USB / I2C の履歴を記録する仕組みがない。
- 信号品質: 公開している統計は `SIGNAL_STRENGTH` と `CNR` の2種類だけで、機種によっては未実装や近似式のまま。BER / PER、誤り訂正の有効化状態、ビットレートは公開していない。
- ISDB-C / J.83B: CXD2856ER の ISDB-C と J.83B 実験経路はあるが、CXD2858ER、Si2168 (T230C) 経路での対応状況が揃っていない。
- 受信開始までの時間: 固定の待機時間 (例: ロック後 350 ms) が実測に基づいていない。

## 2. 目標の構成

```
common/                     共通層 (C、カーネルとユーザー空間の両方でコンパイル)
  include/                  i2c_comm.h、itedtv_bus.h、px4_log.h、platform 抽象 (compat.h)
  bridge/                   it930x、siano_wire_core、siano_protocol_core
  demod/                    tc90522、cxd2856er、cxd2858er、si2168
  tuner/                    r850、rt710、si2141
  ts/                       ts_sync、ts_stats (欠落・TEI・ビットレート計測)
  signal/                   C/N・信号強度・BER の換算表と計算 (チップごと)
linux/                      Linux カーネルモジュール (Kbuild、driver_module、ptx_chrdev、px4_usb、*_device.c)
  include/                  ユーザー API (旧 include/)
  dkms/, debian/            配布
host/                       WinUSB 版と macOS 版が共有する C++ ホスト層
  devices/                  px4、pxmlt、isdb2056、isdbt2071、t230c、siano
  core/                     receiver_base、receiver_manager、ringbuffer、device_definition_set、card
  server/                   ctrl / stream / card server の共通部
windows/                    WinUSB 固有 (itedtv_bus_winusb、misc_win、notify_icon、BonDriver、WinSCard、pkg、build.ps1)
macos/                      macOS 固有 (itedtv_bus_libusb、misc_posix、launchd、px4rec、CMake)
config/                     デバイス定義の単一の元データと、そこから生成する各 OS の INI / ChSet
tools/                      fwtool、TS 解析、受信開始時間の計測
tests/                      共通層とホスト層の試験 (OS 固有の試験は各 OS 配下)
docs/                       文書。機種固有の別紙は docs/devices/
.claude/skills/             このリポジトリ用のレビューと不具合調査の skill
```

### 2.1 維持する契約

- Linux: デバイスファイル名 (`/dev/px4video*`、`/dev/pxmlt5video*` など)、ioctl 番号と構造体、モジュール名 `px4_drv`、DKMS のパッケージ名。
- WinUSB / macOS: 名前付きパイプ・Unix socket の名前と構造体、BonDriver の DLL 名と INI のキー、WinSCard の公開関数、ChSet の形式。
- 既存 INI のキーは削除せず、移行が必要な場合は旧キーも読む。
- 版番号は `update_version.sh` の生成先を移動後のパスへ合わせて維持する。

### 2.2 移行方法

1. ファイル移動だけの段階を独立させ、中身の変更と混ぜない。`git mv` で履歴を保ち、各ビルドファイル (Kbuild、CMake、vcxproj、build.ps1、dkms.conf、CI) のパスを同時に直す。
2. 移動後に3 OS のビルドが移動前と同じ結果になることを確認してから、次の段階に進む。
3. 重複実装の統合 (macOS と WinUSB のサーバー、fwtool の2か所など) は移動の後に、差分を確認しながら1つずつ行う。

## 3. 段階

各段階は「実装 → ビルドと自動試験 → コードレビュー → 実機確認」の順に進めます。
実装は範囲を区切って Haiku 5.5 のサブエージェントへ渡し、レビューと統合は主エージェントが行います。

### 段階 0: 基準の記録

- 現在の作業ツリーを基準として固定する (後述の確認事項 1)。
- 3 OS のビルド結果、自動試験の結果、検証環境での受信結果 (対象機種の TS 計数、C/N、受信開始までの時間) を記録する。
- 受信開始までの時間を集計する道具 (`tools/tune_latency.py`) を先に作り、以降の段階の比較基準にする。

### 段階 1: ファイル移動

- 2 章の構成へ `git mv` だけで移動し、ビルドファイルと CI のパスを直す。
- 確認: macOS のローカルビルドと CTest、GitHub Actions の Linux ビルドと WinUSB ビルド (確認事項 2)。

### 段階 2: 共通層とホスト層の統合

- macOS 版と WinUSB 版のサーバー実装の差分を洗い出し、共通部を `host/server/` へまとめる。OS 固有部は socket / named pipe の薄い層に残す。
- platform 抽象 (`common/include/compat.h`) に、mutex、sleep、時刻、ログ、メモリ確保の名前をそろえる。Linux カーネル、Windows、POSIX の実装は各 OS 層に置く。
- fwtool の重複を解消する。

### 段階 3: デバイス対応の OS 差の解消

- デバイス定義を `config/devices` に一本化し、Linux の USB ID 表、WinUSB と macOS の INI を生成または照合する試験を作る。生成物と元データが食い違うとビルドを失敗させる。
- T230C の Linux 対応: Si2168 / Si2141 の制御を共通層の C へ移し、Linux から使えるようにした。カーネル 6.8 でビルドを確認。実機確認が残る。

### 段階 4: ログ機能

- 共通の `px4_log` API: レベル (ERROR / WARN / INFO / DEBUG / TRACE)、カテゴリ (usb、i2c、demod、tuner、ts、card、auth、server)、`key=value` 形式。カーネルでは `dev_*` / dynamic debug、Windows と macOS では INI の `LogLevel` / `LogCategories` と環境変数で切り替える。
- AI による調査用の出力: JSON Lines 形式の任意出力、失敗時に直前の USB / I2C 転送を所定件数だけ書き出す履歴、選局ごとの時間の内訳 (周波数設定、ロック、最初の TS packet)、受信中の定期統計 (packet 数、continuity counter の欠落、TEI、同期外れ、空受信時間、ビットレート)。
- 長時間受信への影響を避けるため、TRACE と履歴は既定で無効にし、受信スレッドではロックを取らない ring buffer へ書く。

### 段階 5: 信号品質と誤り訂正

- チップごとに、データシートと公開ドライバーから C/N、信号強度 (AGC から dBm への換算)、BER (誤り訂正の前後)、PER を実装し、換算表を `common/signal/` へ置く。
- 誤り訂正の設定 (Reed-Solomon、BCH / LDPC、ISDB-C の FEC monitor) が有効になっていることを初期化時に読み戻して記録する。
- ビットレートは2種類を出す。TMCC や変調パラメーターから計算する理論値と、受信した TS から測る実測値。
- 公開 API の拡張: `StatType` に BER、PER、ビットレート、信号強度 (dBm) を追加する。BonDriver の既存関数 (`GetSignalLevel`) の意味は変えず、追加分は新しい命令で返す。Linux は ioctl を追加する。

### 段階 6: ISDB-C / J.83B

- 各チップがハードウェアで扱える方式を一覧にし、対応表を文書化する。
- CXD2856ER: ISDB-C 64 / 256QAM と J.83B を、CXD2856ER を使う機種で共通の経路にする。既存コードには両方式があるが、CXD2856ER に限定したメーカー資料で J.83B を確認できておらず、J.83B の実機受信も未確認。
- Si2168 (T230C): Si2168-D60 仕様書の Description は J.83 Annex B を記載している。共通層の Annex B 選局と、WinUSB・macOS・Linux の J.83B 経路を追加した (実機未確認)。
- 検証環境は ISDB-C だけで、J.83B の信号は無い。J.83B は実装と自動試験までとし、実機確認は未実施と明記する。

#### 2026-11-08 の公開資料照合

- Si2168-D60 の短縮仕様書 (Skyworks) は、Features と Description で ITU-T J.83 Annex A/B/C の復調を記載している。Linux `si2168` 公開ドライバーは Annex A だけを実装している。Annex B の配信方式 0x10、シンボルレートのプロパティ 0x1602、0x98 の状態要求は TBS linux_media (commit dd0e720e) の値を使い、ExperimentalISDBC の下で J.83B を有効にした。ISDB-C は従来どおり実験的のまま。
- CXD2856ER は ISDB-C を使う公開機器設計資料と CXD2856 公開 driver code がある。J.83B は現在の共通実装に Sony 系 demod/tuner 設定がある一方、参照する J.83B の設定資料が CXD2856ER の完全一致資料ではない。対象機器の実機もないため、実装の存在と silicon capability / 受信確認を分けて記録する。

### 段階 7: 受信開始までの時間の最適化

- 段階 0 の計測道具で、機種ごとに選局から最初の TS packet までの内訳を測る。
- 固定待機を、状態の読み戻しによる待機と、実測した上限に置き換える。

### 段階 8: Claude 用の skill

- `.claude/skills/px4-code-review/`: AGENTS.md の原則 (受信結果の維持、ロック範囲、状態破棄、エラーの境界、実測値) を確認項目にしたレビュー手順。
- `.claude/skills/px4-bug-investigation/`: ログの集め方、TS 解析の道具、受信開始時間の計測、層の切り分け (USB / 復調 / TS / カード) の手順。
- どちらも段階 4 と 7 の道具を呼び出す形にする。

### 段階 9: README と機種別の別紙

- README は対応機種の表 (3 OS で同じ)、導入方法、共通の使い方、文書の索引へ絞る。
- 機種別の別紙を `docs/devices/` に置き、シーケンス図 (Mermaid)、使える機能、設定キー、使い方、確認範囲を書く。T230C と Siano から同じ形式でまとめる。

## 4. 試験範囲

| 段階 | 自動試験 | 実機確認 (検証環境) | 実機確認できない範囲 |
|---|---|---|---|
| 1, 2 | macOS CTest、CI の Linux / WinUSB ビルド | 対象機種の受信 | Windows 実機、Linux 実機 |
| 3 | デバイス定義の照合試験 | — | T230C の各 OS 実機 |
| 4 | ログ形式と ring buffer の試験 | 90 秒受信で欠落が増えないこと | — |
| 5 | 換算表の境界値試験 | 対象機種の C/N・BER | 他の機種 |
| 6 | 復調パラメーターの試験 | ISDB-C | J.83B 全般 |
| 7 | — | 受信開始時間の前後比較 | 他の機種 |

## 5. 確認事項

1. 作業ツリーには未コミットの変更が 134 件ある。ファイル移動の前に、これを基準として固定する方法を決める必要がある。
2. この Mac では Linux カーネルモジュールと Windows 版をビルドできない。GitHub Actions で確かめるには、作業ブランチの push が必要になる。
3. T230C は Linux 経路を追加したが、3 OS とも実機が未確認。
