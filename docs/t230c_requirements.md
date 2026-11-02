# MyGica T230C 対応の要件 (2026-10-30)

## 目的

MyGica (Geniatech) T230C を px4_drv の DriverHost_PX4 で扱えるようにし、日本の
ケーブルテレビ (ISDB-C、トランスモジュレーション) を安定して受信できるようにする。

- **Windows:** 現在は Geniatech のドライバー (サービス `CYDTV_SRV`、`oem3.inf`、2017-06-28 版 1.32.1748.0) しかなく、品質が悪い。
- **Linux:** カーネルの `dvbsky.c` + `si2168.c` + `si2157.c` で動くが、品質に問題がある。

px4_drv のユーザーモード実装 (Windows は WinUSB、macOS は libusb) に載せ、次を提供する。

- WinUSB 用の inf
- BonDriver_PX4 の ini / ChSet
- recisdb-proxy (px4daemon 経由) からの利用

## 対象機器

| 項目 | 値 |
|---|---|
| USB ID | VID 0x0572 / PID 0xC68A (シリアル例 161206) |
| ブリッジ | Cypress FX2 系。Windows のサービス名は `CYDTV_SRV` |
| 復調 IC | Silicon Labs Si2168 (D60) |
| チューナー IC | Silicon Labs Si2141 (A10) |
| ファームウェア | `dvb-demod-si2168-d60-01.fw` と `dvb-tuner-si2141-a10-01.fw` (https://github.com/osmc/dvb-firmware-osmc) |
| 検証環境 | 「検証用 Windows PC」: Windows 11 (26100)、Visual Studio Community 2026、SSH で操作 |

ファームウェアは再配布しない。利用者が上のリポジトリから取得して置き、ini でパスを指定する。

## 機能要件

### F1. DriverHost_PX4 のデバイスクラス

- `winusb/src/DriverHost_PX4/` に T230C 用クラスを追加し、`device_manager.cpp` に登録する。
  - USB 層は WinUSB と libusb の両方で動くこと。
- macOS ビルド (`macos/CMakeLists.txt`) にも含め、Mac に挿しても動くこと。
- 受信機は1つで、方式は ISDB-C のみ。
  - DVB-T/T2 は国内で使わないので対象外。ただし後で足せる構造にしておく。
  - Si2168 は ISDB-T/S に対応しない。

### F2. ISDB-C の選局

- **伝送パラメーター:** 6 MHz 帯域、シンボルレート 5.274 Msps、64QAM / 256QAM。
  - 日本の ISDB-C は ITU-T J.83 Annex C (ロールオフ 0.13) だが、Si2168 にロールオフや Annex の選択があるかは調査で確認する。
  - 無い場合は DVB-C (Annex A) 設定のまま受かるかを実機で確かめる。
- **周波数:** 既存の CATV チャンネル表を使う。
  - ChSet の周波数補正を含む C13〜C63 に加え、UHF 帯 U13〜U62 (473 MHz + 6 MHz 刻み) も含める。
- **QAM の自動判定:** 要求の modulation=0 で 64→256 の順に試してロックした方を使い、GET_PARAMS で返す。既存の PXMLT と同じ動作にする。
- **ロック判定:** TUNE は「TS ロック」で成功とする。タイムアウトは既存の TUNE 要求の値に従う。
- **測定値:** C/N (READ_STATS の CNR、ミリベル)、BER / パケットエラーを返す。返せない値は「未対応」として断る。0 を返して「信号なし」に見せない。

### F3. TSMF / TLV / 複数搬送波

- T230C は TSMF の TS を選ばず、搬送波の TS をそのまま流す。TS1 以降を指定した選局要求は、指定と異なる TS を返さないよう `-EINVAL` で拒否する。
  - そのため復調 IC の TS 出力で、NULL パケットと TSMF ヘッダー (PID 0x002F) を落とさないこと。
- これにより recisdb-proxy の各空間がそのまま使える。
  - 空間 3: CATV の通常 TS
  - 空間 4: CATV-TLV (4K)
  - 空間 5: CATV-8K (複数搬送波の合成。T230C 1台では受信機が足りないため、他の機器と合わせて使う)

### F4. ストリーム品質 (Linux / Windows 既存ドライバーより良いことの基準)

| 項目 | 基準 |
|---|---|
| 選局後の連続受信 | 30 分で CC エラー 0、TEI 0 (受信レベル十分の局。PX-MLT5PE と同じ局で比較) |
| 選局直後 | 最初の 1 秒の CC エラー 5 件以下 |
| 再選局 | 50 回連続で失敗 0、切り替え時間 (要求→最初のバイト) 1.5 秒以下 |
| USB 転送 | 転送単位とバッファー数は ini で変えられること (`XferPackets` / `MaxUrbs` / `ReceiverMaxPackets`) |
| 抜き差し | 動作中に抜いて挿し直しても DriverHost が落ちず、再び使えること |

Linux 版で報告されている不具合 (調査文書の 6 節) を、原因ごとに対策済みか確認する。

### F5. Windows パッケージ

- **inf:** `winusb/pkg/inf/T230C.inf` で WinUSB を割り当てる。既存の `PX-MLT5PE.inf` と同じ書式・署名手順で、x86 / amd64 / arm64 に対応する。
- **ini:** `winusb/pkg/DriverHost_PX4/DriverHost_PX4.ini` に DeviceDefinition を追加する。ファームウェアのパス、転送単位、`ExperimentalISDBC` 相当の設定を入れる。
- **BonDriver:** `winusb/pkg/BonDriver_PX4/BonDriver_T230C.ini` と `BonDriver_T230C-C.ChSet.txt`。
  - ChSet は空間を2つ持つ:
    - 空間1: CATV の通常 TS
    - 空間2: CATV-TLV (TSMF 相対ストリーム 0 + TLV 出力)
  - QAM は自動 (0)。
  - TVTest のチャンネルスキャンで、通常 TS と 4K TLV の両方が見つかること。
  - ChSet の書式は `BonDriver_PX4-C.ChSet.txt` に合わせ、Shift_JIS で保存する。
- **元のドライバーへの戻し方:** Geniatech ドライバーへ戻す手順 (`pnputil /delete-driver` など) を README に書く。

### F6. recisdb-proxy

- **px4daemon:** 既存のパス `px4daemon:any+qam=…@…` で T230C の受信機も選ばれる。そのため `+qam` を付けない自動判定でも動くこと。
- **チャンネルスキャン:**
  - 空間 3 で通常 TS、空間 4 で 4K TLV を自動で見分けて登録する (既存の仕組み)。
  - QAM は自動判定の結果をキャッシュする (既存の `qam_cache`)。
  - 追加の作業が要る場合だけ変更する。

## 非機能要件

- **ライセンス:** px4_drv は GPL-2.0。Linux カーネル (GPL-2.0) の si2168 / si2157 / dvbsky のコードを移植する場合は、出典と著作権表示をファイル先頭に残す。
- **既存機器への影響:**
  - PX-MLT / PX-W3U4 などの既存クラスの動作を変えない。
  - 共有ファイル (`receiver_base.*`、`device_manager.cpp`、ini) の変更は最小限にする。
- **テスト:**
  - USB とハードウェアなしで、コマンドの組み立てと応答の解釈を確かめる単体テストを `macos/tests/` に追加し、ctest に登録する。
  - Si2168 / Si2141 のコマンド列はカーネルのものと突き合わせる。
- **ドキュメント:** `docs/` に T230C の導入手順、制限、品質の測定結果を書く。日本語で、既存文書の文体に合わせる。

## 作業分担

| 担当 | 内容 |
|---|---|
| Haiku 5.5 (調査) | Linux の dvbsky.c / si2168.c / si2157.c を読み、`docs/t230c_linux_reference.md` にまとめる |
| Codex GPT-6-Luna medium (実装) | F1〜F3 と単体テスト、続いて F5 |
| 指揮役 (このセッション) | 要件の確定、差分レビュー、macOS ビルドと単体テスト、検証用 Windows PC でのビルドと実機確認 (F4)、F6 |

## 実機確認の手順 (検証用 Windows PC)

1. **ビルド:** `winusb/build.ps1` で DriverHost_PX4 と BonDriver_PX4 を作る (VS 2026)。
2. **ドライバーの差し替え:** `pnputil /add-driver T230C.inf /install` で WinUSB に切り替える。
   - 管理者権限が要る。
   - 元の Geniatech ドライバー (`oem3.inf`) は残しておく。
3. **起動と受信:** ファームウェアを置き、DriverHost_PX4 を起動して BonDriver_PX4 + TVTest か px4rec 相当で受信する。
4. **品質の測定:** F4 の項目を測り、結果を docs に書く。
