# MyGica T230C (USB 0572:C68A) Linux 参照資料

対象: MyGica/Geniatech T230C 系 USB DVB-T/T2/C スティック。FX2 系ブリッジ + Si2168 (デモジュレータ) + Si2141 (チューナ, Si2157 ドライバで扱う)。
目的: Linux カーネル実装を読み、WinUSB (Windows) / libusb (macOS) のユーザモードドライバへ移植するための仕様メモ。

## 0. 前提と読み方

- 参照元はすべて `torvalds/linux` の master を 2026-10-31 時点で取得したもの。行番号はそのスナップショットに基づく。
- 取得元: `drivers/media/usb/dvb-usb-v2/dvbsky.c`, `dvb_usb.h`, `dvb_usb_core.c`, `dvb_usb_urb.c`, `usb_urb.c`, `drivers/media/dvb-frontends/si2168.c`, `si2168_priv.h`, `si2168.h`, `drivers/media/tuners/si2157.c`, `si2157_priv.h`, `si2157.h`, `include/media/dvb-usb-ids.h`。
- ファームウェアは `osmc/dvb-firmware-osmc` から取得し、サイズと構造のみ解析。バイナリは本書に含めていない。
- 「推定」と書いた箇所は、カーネルのコードや既知の情報からの推測であり、Silicon Labs の API 文書で確認していない。
- 重要な発見 (先に記載):
  1. **PID の対応**: カーネルの定義では `USB_PID_MYGICA_T230C` は `0xc689`、`USB_PID_MYGICA_T230C2` は `0xc68a` (`include/media/dvb-usb-ids.h:307-308`)。VID は `USB_VID_CONEXANT = 0x0572` (`dvb-usb-ids.h:38`)。ご指定の `0x0572:0xC68A` は カーネル上の **T230C2** に当たる。T230C2 だけ `SI2168_TS_CLK_MANUAL` が有効になり、TS モードが異なる (後述 2.2)。
  2. **FX2 ファームウェアのダウンロードはない**: `mygica_t230c_props` に `firmware` / `download_firmware` が無く、`identify_state` は常に `WARM` を返す (`dvbsky.c:600`)。FX2 のファームは工場出荷時の状態 (EEPROM 等) で動いている前提 (推定)。
  3. **J.83 Annex C は Si2168 ドライバで拒否される**: `si2168_set_frontend` は `SYS_DVBC_ANNEX_A` / `SYS_DVBT` / `SYS_DVBT2` 以外に `-EINVAL` を返す (`si2168.c:262-264`)。delivery system 一覧にも Annex C は無い (`si2168.c:637`)。
  4. **DVB-C の QAM 指定は書き込まれていない**: 変調方式 (`c->modulation`) を Si2168 に送るコードは無い。QAM 選択はファームウェア任せ (推定)。
  5. **roll-off 設定は無い**: DVB-C の roll-off を指定する書き込みは見当たらない。

---

## 1. USB ブリッジ・プロトコル (dvbsky.c)

### 1.1 転送の基本構造

T230C は「ベンダーコマンド」ではなく、**バルク転送で独自コマンドを送る**方式。`mygica_t230c_props` (`dvbsky.c:733-757`) の設定:

| 項目 | 値 | 参照 |
|---|---|---|
| コマンド送信 (OUT) | bulk endpoint **0x01** | `dvbsky.c:739` `generic_bulk_ctrl_endpoint = 0x01` |
| 応答受信 (IN) | bulk endpoint **0x81** | `dvbsky.c:740` |
| 応答前ディレイ | `DVBSKY_MSG_DELAY` = **0** ms (コメントアウトの 2000 は無効) | `dvbsky.c:15`, `:741` |
| TS ストリーム | bulk IN **0x82**, URB 数 **8**, 1 URB あたり **4096** バイト | `dvbsky.c:754` `DVB_USB_STREAM_BULK(0x82, 8, 4096)` |
| バッファ (ホスト側) | `DVBSKY_BUF_LEN` = 64 (ibuf / obuf) | `dvbsky.c:16`, `:25-26` |

送信・受信の実体は `dvb_usb_v2_generic_io` (`dvb_usb_urb.c` 相当, 行 11-58):
- 送信: `usb_bulk_msg(EP 0x01 OUT, wbuf, wlen, timeout=2000 ms)`。`actual_length != wlen` ならエラー。
- 応答: `rlen != 0` のときのみ、`usleep_range(delay, delay+20000)` の後、`usb_bulk_msg(EP 0x81 IN, rbuf, rlen, timeout=2000 ms)`。
- 応答長は要求した `rlen` 固定。デバイスが短く返す場合の扱いは、カーネルでは明示的に検査していない (推定: 常に rlen 分返る前提)。

ストリーム側 (`usb_urb.c:136-160`): `usb_fill_bulk_urb` を `usb_rcvbulkpipe(EP 0x82)`、バッファ長 `buffersize` (= 4096) で、`count` (= 8) 個の URB を確保して submit する。

### 1.2 全コマンド一覧 (1 バイト目 = コマンドコード)

すべて `dvbsky_usb_generic_rw` (`dvbsky.c:39-57`) を経由する (`wlen`/`rlen` は指定どおり)。`dvbsky_stream_ctrl` のみ書き込み専用。

| コマンド | 送信バイト列 (wlen) | 応答長 (rlen) | 用途 | 参照 |
|---|---|---|---|---|
| GPIO 書き込み | `0x0e, gport, value` (3) | 1 | GPIO 制御 (identify_state で使用) | `dvbsky.c:86-96` |
| I2C 書き込み | `0x08, addr, len, data[len]` (len+3) | 1 | 単独 I2C write | `dvbsky.c:135-141` |
| I2C 読み出し | `0x09, 0x00, len, addr` (4) | len+1 | 単独 I2C read。応答 `[0]` は捨て `[1..len]` がデータ | `dvbsky.c:126-133` |
| I2C 書き込み後読み出し | `0x09, wlen, rlen, addr, wdata[wlen]` (wlen+4) | rlen+1 | repeated start 相当。応答 `[1..rlen]` がデータ | `dvbsky.c:152-160` |
| ストリーム制御 (前置) | `0x37, 0x00, 0x00` (3) | (write only) | FX2 FIFO 関係。停止時もこれのみ送る | `dvbsky.c:62, 67-68` |
| ストリーム制御 (開始) | 上記の後、20 ms 待って `0x36, 0x03, 0x00` (3) | (write only) | 開始時のみ送る | `dvbsky.c:63, 69-73` |
| RC 読み出し (RC5) | `0x10` (1) | 2 | リモコン (任意。`get_rc_config` で有効) | `dvbsky.c:185-188` |
| MAC 読み出し | `0x1e, 0x00` (2) + I2C 0x51 | 6 | T230C では使われない | `dvbsky.c:232-255` |
| CI 制御 (0x70/0x71) | — | — | T230C では未使用 | `dvbsky.c:345-374` |

補足:
- 「応答長 1」の I2C write や GPIO も、応答 1 バイトを読むことが必須。読まないとデバイスのバッファに応答が残る可能性がある (推定)。
- ストリーム制御は `dvbsky_streaming_ctrl` (`dvbsky.c:78-83`) から `onoff` に応じて呼ばれる。`onoff != 0` のとき `0x37` → 20 ms → `0x36`、`onoff == 0` のとき `0x37` のみ。
- `dvbsky_usb_read_status` のフック (ロック立ち上がり時に `dvbsky_stream_ctrl(d, 1)` を再送、`dvbsky.c:257-272`) は **T230C では設定されていない** (`dvbsky.c:532-577` でフックなし)。T230C の attach 関数だけがそう。

### 1.3 I2C ラッパの形式と制限 (dvbsky_i2c_xfer, `dvbsky.c:99-165`)

- メッセージ数は最大 2 (`dvbsky.c:109-114`)。
- `msg.addr` は **7 bit アドレス** (例: Si2168 は `0x64`、USB コマンドのバイトもそのまま `0x64`)。Linux の `i2c_msg.addr` は 7 bit 表現なので、コマンドに 8 bit (0xC8) を入れてはいけない。
- 1 メッセージの長さ上限 **60 バイト**:
  - 単独: `msg[0].len > 60` でエラー (`dvbsky.c:117`)。
  - 書き込み後読み出し: 両方とも `> 60` でエラー (`dvbsky.c:144`)。
- 単独 I2C read の応答は `len + 1` バイト (先頭 1 バイトは捨てる、`dvbsky.c:131-133`)。

### 1.4 GPIO / リセット / LNB / 電源

- `identify_state` (non-T230A 経路、`dvbsky.c:591-598`) で GPIO 操作を行う。`dvbsky_identify_state` は `WARM` を返すので、以降ファーム DL はない。
  - 送信順 (`dvbsky_gpio_ctrl(d, gport, value)` → `{0x0e, gport, value}`):
    1. `0x04 = 1`
    2. sleep 20 ms
    3. `0x83 = 0`
    4. `0xc0 = 1`
    5. sleep 100 ms
    6. `0x83 = 1`
    7. `0xc0 = 0`
    8. sleep 50 ms
- `dvbsky_usb_set_voltage` / `dvbsky_usb_ci_set_voltage` (`dvbsky.c:216-230`, `:329-343`) は T230C では設定されていない。LNB 制御は T230C 経路で無し (地上波・ケーブル機のため)。
- T230A 用の別シーケンス (`0x87`, `0x86`, `0x80`) もあるが、T230C には無関係 (`dvbsky.c:582-589`)。

### 1.5 TS エンドポイント・URB 設定

- TS は bulk IN **0x82**。`usb_rcvbulkpipe` で読む (`usb_urb.c:150-152`)。
- URB 8 本 × 4096 バイトを基本とする。移植では同程度の並列度 (8 本) を確保する案が妥当 (推定。カーネルのデフォルト値をそのまま採用)。
- TS は Si2168 からパラレル (`SI2168_TS_PARALLEL`) で FX2 に入り、FX2 の FIFO から bulk で流れる、という構成と推定 (`dvbsky.c:20`, `:548` と `dvbsky_streaming_ctrl` の FIFO 関連コメント `dvbsky.c:266`)。

### 1.6 ファームウェアと EEPROM

- `mygica_t230c_props` に `.firmware` / `.download_firmware` がない。`dvb_usbv2_probe` は `identify_state` が `COLD` を返したときだけ DL を試みる (`dvb_usb_core.c:946-974`)。T230C は常に `WARM` (`dvbsky.c:600`)。
- よって FX2 のファームは既に動作中、またはデバイス内で保持されている前提 (推定)。ユーザモードでも FX2 への DL は不要と考えられる。ただし検証は未実施。

---

## 2. 初期化順序 (attach シーケンス)

### 2.1 I2C アドレス

| デバイス | I2C アドレス (7bit) | 接続先 | 参照 |
|---|---|---|---|
| Si2168 | **0x64** | USB ブリッジの I2C アダプタ直下 | `dvbsky.c:552`, `si2168.h:33` |
| Si2141/Si2157 (チューナ) | **0x60** | Si2168 の I2C マルチプレクサ経由 (ゲート制御あり) | `dvbsky.c:566-569` |
| SP2 (CI) | 0x40 | T230C では **使わない** | `dvbsky.c:485`, 参照のみ |

### 2.2 T230C の attach (`dvbsky_mygica_t230c_attach`, `dvbsky.c:532-577`)

1. Si2168 の設定 (`si2168_config`):
   - `ts_mode = SI2168_TS_PARALLEL` (`0x06`) (`dvbsky.c:543`)
   - **PID が 0xC68A (T230C2)、0xC69A (T230C2_LITE)、0x689A (T230A) の場合のみ** `ts_mode |= SI2168_TS_CLK_MANUAL` (`0x20`) (`dvbsky.c:544-547`)
     - 結果: 0xC68A → `ts_mode = 0x26`、0xC689 → `ts_mode = 0x06`
   - `ts_clock_inv = 1` (`dvbsky.c:548`) — すべての PID で固定
   - `ts_clock_gapped` は設定なし (= 0)。`ts_clock_gapped` を使うのは T330 のみ (`dvbsky.c:509`)
   - `spectral_inversion` は設定なし (= 0)
2. Si2168 プローブ: `dvb_module_probe("si2168", NULL, &d->i2c_adap, 0x64, &si2168_config)` (`dvbsky.c:550-552`)。
3. Si2157 の設定:
   - `if_port`: `0x00` (`dvbsky.c:565`)。ただし `USB_PID_MYGICA_T230` (0xC688) のみ `if_port = 1` (`dvbsky.c:558-559`)。
   - 名前 `"si2141"` で probe (`dvbsky.c:566-569`)。アドレス `0x60`、アダプタは Si2168 のマルチプレクサ (`i2c_adapter`)。
4. 失敗時は demod を release して終了 (`dvbsky.c:571-574`)。
5. T230C は `sp2` (CI) を attach しない。`read_status` / `set_voltage` のフックもない。

### 2.3 USB 側の起動順 (dvb_usb_core)

1. `identify_state` (GPIO シーケンス、約 170 ms、`dvbsky.c:591-598`) → `WARM`。
2. `init` = `dvbsky_init` で `last_lock = 0` のみ (`dvbsky.c:603-608`)。
3. frontend attach (上記)。
4. ストリーミング開始は `streaming_ctrl` (`0x37` → 20 ms → `0x36`)。

### 2.4 si2168_config / si2157_config の値 (T230C, PID 0xC68A の場合)

| 項目 | Si2168 | Si2157 (Si2141) |
|---|---|---|
| I2C アドレス | 0x64 | 0x60 |
| ts_mode | 0x26 (= CLK_MANUAL 0x20 | PARALLEL 0x06) | — |
| ts_clock_inv | 1 | — |
| ts_clock_gapped | 0 | — |
| spectral_inversion | 0 | (inversion = 0) |
| if_port | — | 0 |
| firmware 名 (probe で決定) | `dvb-demod-si2168-d60-01.fw` (chip ID D60 のとき) | (後述 4.2) |

PID 0xC689 (`USB_PID_MYGICA_T230C`) の場合は ts_mode が `0x06` になるだけで、他は同じ。

---

## 3. Si2168 (デモジュレータ, si2168.c)

### 3.1 共通: コマンド実行 (`si2168_cmd_execute`, `si2168.c:22-83`)

- 送信: `i2c_master_send(client, args, wlen)`。
- `rlen != 0` のとき、`i2c_master_recv` を繰り返し、`args[0] & 0x80` (ready) が立つまでポーリング。間隔 2.5-3.5 ms、タイムアウト `TIMEOUT` = **140 ms** (`si2168.c:43`)。
- `args[0] & 0x40` でエラー (`-EREMOTEIO`) (`si2168.c:66-69`)。
- `rlen == 0` のときは送信のみで戻る。
- 応答バイト列 (`args`) の最大長は `SI2168_ARGLEN` = **30** (`si2168_priv.h:46`)。

### 3.2 プローブ (`si2168_probe`, `si2168.c:677-776`)

1. `C0 12 00 0C 00 0D 16 00 00 00 00 00 00` (13 バイト、rlen 0) — カーネル側コメントは "Initialize" (`si2168.c:696-697`, `:422-423`)。
2. `C0 06 01 0F 00 20 20 01` (8 バイト、rlen 1) — "Power up" (`si2168.c:703-704`)。
3. `02` (rlen 13) — chip 情報。`chip_id = args[1]<<24 | args[2]<<16 | args[3]<<8 | args[4]` (`si2168.c:709-715`)。
   - 定義: `SI2168_CHIP_ID_D60 = 'D'<<24 | 68<<16 | '6'<<8 | '0'` (`si2168_priv.h:32`)。他に A20/A30/B40 (`:29-31`)。
   - D60 → `SI2168_D60_FIRMWARE = "dvb-demod-si2168-d60-01.fw"` (`si2168_priv.h:20`)。
4. I2C マルチプレクサ作成 (`si2168.c:741-751`)。

### 3.3 I2C ゲート

- open: `C0 0D 01` (`si2168.c:607`)。
- close: `C0 0D 00` (`si2168.c:625`)。
- マルチプレクサの select/deselect で、チューナのアクセス前後に呼ばれる。

### 3.4 ファームウェアのダウンロード・起動 (`si2168_init`, `si2168.c:410-539`)

- 初回 (cold) の順序:
  1. `C0 12 00 0C 00 0D 16 00 00 00 00 00 00` (rlen 0)
  2. `C0 06 01 0F 00 20 20 01` (rlen 1, power up)
  3. ファーム DL (下記 5 章)
  4. `01 01` (rlen 1) — カーネルにコメントなし。ファーム起動/リブート相当と推定 (推定)
  5. `11` (rlen 10) — ファーム版数。`version = (args[9]+'@')<<24 | (args[6]-'0')<<16 | (args[7]-'0')<<8 | args[8]` (`si2168.c:501-508`)
  6. `si2168_ts_bus_ctrl(fe, 1)` (TS モード設定、3.6 節)
- warm 再開 (`dev->warm` のとき): `C0 06 08 0F 00 20 21 01` (rlen 1)、`udelay(100)`、`85` (rlen 1) (`si2168.c:430-441`)。
- `si2168_get_tune_settings` は `min_delay_ms = 900` (`si2168.c:595`)。チューン後 900 ms 待つ必要がある。

### 3.5 DVB-C の set_frontend (`si2168_set_frontend`, `si2168.c:232-408`)

順序は次のとおり。**まずチューナ (`fe->ops.tuner_ops.set_params`) を呼ぶ (`si2168.c:288-292`)**、その後にデモジュレータへ送る。

| # | 送信バイト列 | rlen | 内容 / 参照 |
|---|---|---|---|
| 0 | (チューナ側: Si2157 の set_params) | — | 4.3 節 |
| 1 | `88 02 02 02 02` | 5 | カーネル側コメントなし (`si2168.c:294`) |
| 2 | `89 21 06 11 89 F0` (DVB-C) | 3 | "that has no big effect" (`si2168.c:302-303`)。T: `89 21 06 11 FF 98`、T2: `89 21 06 11 89 20` (`:301`, `:305`) |
| 3 | `51 03` | 12 | カーネル側コメントなし (`si2168.c:322`) |
| 4 | `12 08 04` | 3 | カーネル側コメントなし (`si2168.c:327`) |
| 5 | `14 00 0C 10 12 00` | 4 | プロパティ 0x100C = 0x0012 (`:332`) |
| 6 | `14 00 06 10 24 00` | 4 | プロパティ 0x1006 = 0x0024 (`:337`) |
| 7 | `14 00 07 10 00 24` | 4 | プロパティ 0x1007 = 0x2400 (`:342`) |
| 8 | `14 00 0A 10 <args4> <args5>` | 4 | **delivery system と帯域**。`args4 = 0x30 | bw` (DVB-C)。`args5 |= 1` は spectral inversion 時のみ (`:347-353`) |
| 9 | `14 00 02 11 <sr_lo> <sr_hi>` (DVB-C のみ) | 4 | **シンボルレート (ksym/s)**、LE16 (`:356-363`) |
| 10 | `14 00 0F 10 10 00` | 4 | プロパティ 0x100F = 0x0010 (`:365`) |
| 11 | `14 00 09 10 E3 08` (+ inv 補正) | 4 | `args5 |= ts_clock_inv ? 0x00 : 0x10` (`:370-374`)。T230C (inv=1) では `E3 08` |
| 12 | `14 00 08 10 D7 05` (+ inv 補正) | 4 | `args5 |= ts_clock_inv ? 0x00 : 0x10` (`:376-380`)。T230C では `D7 05` |
| 13 | `14 00 01 12 00 00` | 4 | プロパティ 0x1201 = 0 (`:382`) |
| 14 | `14 00 01 03 0C 00` | 4 | プロパティ 0x0301 = 0x000C (`:387`) |
| 15 | `85` | 1 | カーネル側コメントなし (`:392`) |
| 16 | TS バス有効化 (3.6 節) | — | `si2168_ts_bus_ctrl(fe, 1)` (`:400`) |

DVB-T は `delivery_system = 0x20`、DVB-T2 は `0x70` (`si2168.c:254-260`)。
DVB-T2 では PLP 選択 `52 <plp> <flag>` (rlen 1) を追加 (`:312-318`)。

#### 帯域コード (`si2168.c:267-285`)

`bandwidth_hz <= 6000000` → `0x06`。**6 MHz は到達可能**。
（2 MHz→0x02, 5 MHz→0x05, 7 MHz→0x07, 8 MHz→0x08, 上記以外→0x0F）

#### delivery system コード

| 方式 | 値 | 参照 |
|---|---|---|
| DVB-T | 0x20 | `si2168.c:254` |
| DVB-C Annex A | **0x30** | `si2168.c:257` |
| DVB-T2 | 0x70 | `si2168.c:260` |

#### DVB-C の各値 (例: 6 MHz, 5274 ksym/s)

- プロパティ 0x100A の値 = `0x0036` (bw=6, delivery=0x30, spectral=0)。
- シンボルレート: `5274000 / 1000 = 5274 = 0x149A` → `14 00 02 11 9A 14`。
- 範囲は `symbol_rate_min = 1,000,000`, `symbol_rate_max = 7,200,000` (`si2168.c:643-644`)。**5.274 Msym/s は範囲内**。LE16 (ksym/s) なので 65535 ksym/s まで表現可能だが、ops の上限は 7.2 Msym/s。
- 変調方式 (QPSK/16/32/64/128/256QAM): **書き込みコードなし**。`c->modulation` は参照されない (`si2168.c:241-245` のデバッグ出力のみ)。QAM はファーム任せ (推定)。
- ロールオフ: 書き込みなし。
- Annex A/C の切替: **なし**。Annex C は `si2168.c:262-264` で拒否される。

### 3.6 TS バス設定 (`si2168_ts_bus_ctrl`, `si2168.c:85-114`)

1. `ts_mode & SI2168_TS_CLK_MANUAL` のとき: `14 00 0D 10 E8 03` (rlen 4) — プロパティ 0x100D = 0x03E8 (= 1000)。単位は不明 (パッチ説明では 10 MHz と書かれているが、コード上は 1000 という値のみ)。
2. `14 00 01 10 <args4> 00` (rlen 4) — プロパティ 0x1001。
   - 初期値 `args4 = 0x10`。`CLK_MANUAL` 時は `args4 = 0x20` (代入)。
   - acquire 時は `args4 |= ts_mode`。解放 (tristate) 時は `args4 |= 0x00`。
   - `ts_clock_gapped` 時は `args4 |= 0x40`。
   - T230C (0xC689): 値 `0x0016`。 T230C2 (0xC68A): 値 `0x0026`。

### 3.7 ステータス・統計 (`si2168_read_status`, `si2168.c:116-230`)

- ロック状態取得: DVB-C `90 01` (rlen 9)、DVB-T `A0 01` (rlen 13)、DVB-T2 `50 01` (rlen 14)。
- ステータス判定 (`args[2] >> 1 & 3`): `0x01` → SIGNAL|CARRIER、`0x03` → SIGNAL|CARRIER|VITERBI|SYNC|**LOCK** (`si2168.c:151-159`)。
- CNR (DVB-C 含む、LOCK 時のみ): `args[3] * 1000 / 4` (dB×1000、単位 0.25 dB) (`si2168.c:163-170`)。
- BER: `82 00` (rlen 3)。`args[1]` = 指数 (0-8 にクランプ)、`args[2]` = 仮数。`post_bit_error += args[2] × 10^(8−args[1])`、`post_bit_count += 10^8` (`si2168.c:176-204`)。
- UCB: `84 01` (rlen 3)。`args[1] | args[2] << 8` (LE16)、0xFFFF は 0 扱い (`si2168.c:207-224`)。
- 注: 既定 (SYNC 後) でのみ UCB を読む。

### 3.8 スリープ (`si2168_sleep`, `si2168.c:561-590`)

1. `ts_bus_ctrl(fe, 0)` (TS tristate)。
2. version が B 4.0-11 以降なら `warm = false` (`si2168.c:578`)。D60 の扱いは本コードからは不明。
3. `13` (rlen 0)。

### 3.9 その他の制約

- 周波数範囲: 48-870 MHz、ステップ 62.5 kHz (`si2168.c:640-642`)。
- `FE_CAN_QAM_AUTO` 等の能力フラグは立っている (`si2168.c:645-663`)。実際のオートQAM動作は未検証。

---

## 4. Si2141 (チューナ, si2157.c)

### 4.1 コマンド実行 (`si2157_cmd_execute`, `si2157.c:17-77`)

- 応答待ち: `TIMEOUT` = **80 ms** (`si2157.c:38`)、ready = `args[0] & 0x80`、ポーリングは 間隔なし (ループ)。
- `args[0] & 0x40` でエラー `-EAGAIN` (`si2157.c:64-66`)。
- `ARGLEN` = 30 (`si2157_priv.h:62`)。

### 4.2 チップ識別とファーム選択 (`si2157_find_and_load_firmware`, `si2157.c:140-214`)

- `02` (rlen 13) で `part_id = args[2]`、`rom_id = args[12]` (`si2157.c:157-166`)。
- テーブル (`si2157.c:79-90`): Si2141 ROM 0x60 → `dvb_driver_si2141_rom60.fw` (代替: `dvb-tuner-si2141-a10-01.fw`)、ROM 0x61 → `dvb_driver_si2141_rom61.fw` (代替同じ)。
- 名前は `si2157_priv.h:84-85`, `:80` で定義。
- ROM ID がテーブルに無い場合は `fw_name = NULL`。そのときは代替名 (A10) を試す。
- Si2141 は `required = false`。どちらも無い場合は ROM ファームで続行 (`si2157.c:204-208`)。

### 4.3 初期化 (`si2157_init`, `si2157.c:216-325`)

1. xtal trim 確認: `15 00 02 04` (rlen 4)。`xtal_trim < 16` かつ成功なら warm 扱いで飛ぶ (`si2157.c:228-236`)。
2. パワーアップ (Si2141): `C0 00 0D 0E 00 01 01 01 01 03` (rlen 1)。`-EAGAIN` は許容 (`si2157.c:247`, `:255`)。
3. Si2141 のみ wake-up: `C0 08 01 02 00 00 01` (wlen 7。rlen は直前のコマンドから引き継がれた値のままで、`cmd.rlen` を明示していない) (`si2157.c:258-265`)。
4. ファーム DL (4.2)。
5. `01 01` (rlen 1) — "reboot the tuner with new firmware?" とコメント (`si2157.c:272-278`)。
6. `11` (rlen 10) — ファーム版数表示 (`si2157.c:280-289`)。
7. 状態フラグ有効化 (`si2157.c:291-311`): `14 00 01 05 01 00`、`14 00 01 06 01 00`、`14 00 01 07 01 00` (rlen 1)。
8. 統計ポーリング (1000 ms 後開始、2000 ms 周期) (`si2157.c:317`, `:991`)。

### 4.4 DVB-C 6 MHz の set_params (`si2157_set_params`, `si2157.c:442-567`)

| # | 送信 | 内容 |
|---|---|---|
| 1 | `14 00 03 07 <args4> <args5>` (rlen 4) | プロパティ 0x0703。`args4 = delivery | bw`。DVB-C annex A/C は `0x30` (`si2157.c:494`)。6 MHz は `bw = 6` (`:465-467`)。inversion フラグ時のみ `args5 = 1` (`:509-510`) |
| 2 | `14 00 02 07 <if_port> 00` (rlen 4) | プロパティ 0x0702 (`si2157.c:519-522`)。`if_port` (T230C は 0) |
| 3 | `14 00 06 07 <lo> <hi>` (rlen 4) | プロパティ 0x0706 = IF 周波数 (kHz)。DVB-C 既定 `if_frequency = 5000000` → 5000 kHz (`si2157.c:451`, `:530-541`)。前回と同じなら省略 |
| 4 | `41 00 <f0> <f1> <f2> <f3>` (rlen 1) | 周波数 Hz (LE32) (`si2157.c:544-553`) |
| 5 | (`si2157_tune_wait(client, 1)`) | ステータス読み。`0x81` で完了、`0x04` (デジタル lock) で lock 確認 (`tuner_lock_debug` 時のみ)、`(status & 0xC0) == 0x80` で成功 (`si2157.c:355-440`) |

- `TUN_TIMEOUT` = 40 ms、`DIG_TIMEOUT` = 30 ms (`si2157.c:357-358`)。
- 「IF 周波数」は **5 MHz** (`si2157.c:451`, `:1020`)。Si2168 側にも IF 指定コマンドは無く、Si2168 が 5 MHz IF を前提とするかどうかは、コードからは断定できない (推定: 5 MHz IF を前提)。

### 4.5 AGC

- kernel には、チューナ側の DTV AGC ソース設定として `0x0702` を書くコードがある (`si2157.c:517-522`)。SI2146 のみ `DLIF_AGC_3DB` (`0x0001`) を書き、他は `0x0000` + `if_port`。Si2141 では `if_port` の値 (T230C は 0) を書く。
- 名前から「AGC ソース」とあるが、Si2141 の AGC の詳細 (速度、閾値) は設定していない。

### 4.6 RF 強度 (`si2157_get_rf_strength`, `si2157.c:916-950`)

- `42 00` (rlen 12)。`args[3]` を dBm 相当 (`s8`)。`rssi = (args[3] + 100) を 0-80 にクランプ → ×0xFFFF/80` (`si2157.c:926-941`)。

### 4.7 Si2141 の IF 出力

- Si2141 → Si2168 の IF 出力は、チューナ側の IF 周波数 5 MHz (デジタル)。Si2168 側の IF 受けの設定は kernel に無い (推定: ファーム既定)。

---

## 5. ファームウェアの形式と読み込み

### 5.1 サイズと構造 (取得物を解析、バイナリは非掲載)

| ファイル | サイズ | %17 | チャンク数 | 形式 | 先頭チャンク (長さ) |
|---|---|---|---|---|---|
| `dvb-demod-si2168-d60-01.fw` (Si2168) | 3349 バイト | 0 | 197 | **17 バイト形式** (new) | len=8 |
| `dvb-tuner-si2141-a10-01.fw` (Si2141) | 1666 バイト | 0 | 98 | **17 バイト形式** | len=8 |

- 解析スクリプト: `/private/tmp/claude-501/-Users-ayumu-prog/c48993bc-9eb0-45c8-8eed-304c6a3ef86f/scratchpad/analyze_fw.py`
- 両ファイルとも先頭バイトは `0x08` (> 5) で、Si2168 の「新形式」判定 (`si2168.c:462`) に該当する。
- Si2168 D60 ファイルのチャンク長 (`len` バイト) の分布は 2〜16 で、`SI2168_ARGLEN` = 30 を超えるものはない。Si2141 も同様に 8〜16。
- 第 1 バイト以外の `args` の中身はバイナリのため、ここでは記載しない。

### 5.2 17 バイト形式の読み込み

- 1 チャンク = 17 バイト。`[0]` = 有効長 `len`、`[1..len]` = コマンド引数、`[len+1..16]` = パディング。
- Si2168 (`si2168.c:462-475`): `(fw->size % 17 == 0) && (fw->data[0] > 5)` で新形式。各チャンクを `cmd_init(&fw->data[pos+1], len, 1)` で送り、応答 1 バイトを ready まで待つ。`len > SI2168_ARGLEN` でエラー。
- Si2141 (`si2157.c:105-132`): `fw->size % 17 != 0` でエラー。新形式のみ対応。`len > SI2157_ARGLEN` でエラー。各チャンクで rlen = 1 のポーリング。
- 旧形式 (8 バイト単位) は Si2168 のみ (`si2168.c:476-483`)。T230C のファームは新形式なので使わない。

### 5.3 ファイル名の探し方

- Si2168: `dvb-demod-si2168-d60-01.fw` (D60 の場合、`si2168_priv.h:20`)。
- Si2141: 第 1 候補 `dvb_driver_si2141_rom60.fw` (ROM 0x60)、第 2 候補 `dvb-tuner-si2141-a10-01.fw` (`si2157_priv.h:80, :84`)。osmc のファイルは後者に該当。
- ユーザモード移植では、同名ファイルを読んで上記形式で送るだけでよい。`request_firmware` 相当は `firmware_request_nowarn` 相当の探索 (Windows/mac では、アプリ同梱ファイルを読む)。

---

## 6. 既知の品質問題と推定原因

検索で見つけた情報は断片 (検索結果のスニペット) で、本文は未確認。各リンクの内容は要確認。

1. **T230C と T230C2 の混同 (PID 差)**
   - 症状: デバイスは列挙されるが、Linux で正しく機能しない。
   - 推定原因: カーネルは PID で `SI2168_TS_CLK_MANUAL` の有無を変えている (`dvbsky.c:544-547`)。0xC68A と 0xC689 で ts_mode が違う (0x26 / 0x06)。PID 判定を誤ると TS クロック設定が違い、ロックしない・ストリームエラーになる可能性。
   - 参照: <https://patchwork.kernel.org/patch/10997259> (2019, v2 対応の説明, 検索結果の要約)、<https://scrapbox.io/rokoucha/2022-03-07> (日本語、ロック失敗の報告、dvbv5-zap でのリトライ回避策)、<https://discourse.osmc.tv/t/dvb-t2-geniatech-mygica-t230-dont-work/77221/17> (T230C2 と D60 ファーム名への言及)。

2. **TS クロックのフラグ不一致 (ts_clock_inv / ts_clock_gapped)**
   - カーネルは `ts_clock_inv = 1` 固定 (`dvbsky.c:548`)、`ts_clock_gapped` は未設定。
   - 推定原因: 他機種では `ts_clock_gapped` が必要なものがあり (T330 は設定, `dvbsky.c:509`)、T230C で合わないとストリームのエラー、部分的な破損の原因になりうる。検証は未実施。
   - 参照: <https://lkml.iu.edu/hypermail/linux/kernel/1702.2/00746.html> (2017 の T230C 初版パッチ: `ts_clock_inv = 1`、parallel)、<https://lkml.iu.edu/hypermail/linux/kernel/1702.1/05106.html>。

3. **ロック時の FX2 FIFO 再同期の欠如**
   - T230C では `dvbsky_usb_read_status` フック未設定 (`dvbsky.c:532-577`)。他機種では「ロック立ち上がりで `0x37`/`0x36` を再送」(`dvbsky.c:266-268`)。
   - 推定原因: ロック直後にストリームが同期しない場合、ストリームエラーの候補。T230C で必要かは未確認。

4. **DVB-C の変調・ロールオフ・Annex の制御不足**
   - QAM 指定が無い、roll-off 指定が無い、Annex C は拒否される (3.5 節)。
   - 推定原因: 日本のケーブル (J.83 Annex C 想定) での非ロックは、この欠落で説明できる可能性。ただし、Annex A の実機での動作もカーネル上は確認できていない。

5. **スペクトラム反転**
   - `c->inversion` は参照されず、`spectral_inversion` は T230C で 0 固定 (`si2168.c:349-350`, `dvbsky.c:548` 周辺)。
   - 推定原因: 反転が必要な受信環境で非ロックになりうる。

6. **ハードウェアの個体差**
   - 参照: <https://tvheadend.org/boards/4/topics/26380> (T230C 個体のフロントエンドの不具合の疑い、アンテナ入力でロックせず)。
   - ドライバ以前に、ハードの不良の可能性がある。

7. **Si2141 ファームの代替名処理**
   - ROM ID が 0x60/0x61 以外の場合は、A10 の代替ファームを試す。どちらも無いと ROM ファームで続行 (`si2157.c:204-208`)。実機の ROM ID は未確認。
   - 参照: <https://lore.kernel.org/patchwork/patch/904819/mbox/> (検索結果の表題のみ確認。本文は未確認)。

8. **発見の注意**
   - 検索の結果、T230C 向けの DVB-C 固有の検証結果は見つからなかった。Linux でのパッチの最終的な upstream 状況も未確定。

---

## 7. 魔法定数の一覧 (ファイル:行)

### dvbsky.c

| 値 | 意味 | 行 |
|---|---|---|
| `DVBSKY_MSG_DELAY 0` | 応答前待ち (ms) | 15 |
| `DVBSKY_BUF_LEN 64` | ホスト側バッファ | 16 |
| `0x0e` | GPIO 書き込み | 91 |
| `0x08` | I2C 書き込み | 136 |
| `0x09` | I2C 読み出し / 書き込み後読み出し | 126, 152 |
| I2C 長さ上限 `60` | 単独 / 書き込み後読み出し | 117, 144 |
| I2C メッセージ数上限 `2` | | 109 |
| `0x37, 0, 0` | ストリーム前置 | 62 |
| `0x36, 3, 0` | ストリーム開始 | 63 |
| `msleep(20)` | ストリーム開始の待ち | 70 |
| `0x10` | RC 読み出し | 185 |
| `rc->interval = 300` | RC ポーリング間隔 | 209 |
| `0x80` | LNB 制御 GPIO (T230C 未使用) | 227 |
| `0x1e, 0x00` / `0x51` | MAC 読み出し (T230C 未使用) | 235, 239 |
| `0x70` / `0x71` | CI (T230C 未使用) | 355, 359 |
| `GPIO 0x04=1 / 0x83=0 / 0xc0=1 / 0x83=1 / 0xc0=0` | identify_state (非 T230A) | 591-597 |
| `msleep(20), (100), (50)` | identify_state の待ち | 592, 595, 598 |
| `GPIO 0x87, 0x86, 0x80` | T230A 用 | 583-588 |
| `SI2168_TS_PARALLEL` | ts_mode 基本 | 543 |
| PID `USB_PID_MYGICA_T230C2` (0xC68A) 等で `CLK_MANUAL` | | 544-547 |
| `ts_clock_inv = 1` | | 548 |
| `si2168 I2C 0x64` | | 552 |
| `if_port = 0` (T230C) / `1` (T230) | | 558-565 |
| `si2157 I2C 0x60` | | 566 |
| `WARM` (identify_state 戻り値) | | 600 |
| `DVB_USB_STREAM_BULK(0x82, 8, 4096)` | TS 設定 | 754 |
| `generic_bulk_ctrl_endpoint 0x01 / response 0x81` | | 739-740 |
| `0x0572` (DVBSky 系 VID, ids 上は CONEXANT) | | 760-766 |

### dvb-usb-ids.h

| 値 | 意味 | 行 |
|---|---|---|
| `USB_VID_CONEXANT 0x0572` | VID | 38 |
| `USB_PID_MYGICA_T230 0xc688` | | 305 |
| `USB_PID_MYGICA_T230A 0x689a` | | 306 |
| `USB_PID_MYGICA_T230C 0xc689` | | 307 |
| `USB_PID_MYGICA_T230C2 0xc68a` | **ご指定の PID** | 308 |
| `USB_PID_MYGICA_T230C2_LITE 0xc69a` | | 309 |
| `USB_PID_MYGICA_T230C_LITE 0xc699` | | 310 |

### dvb_usb.h / dvb_usb_urb.c / usb_urb.c

| 値 | 意味 | 参照 |
|---|---|---|
| `WARM 0` / `COLD 1` | | dvb_usb.h:238-239 |
| `usb_bulk_msg` タイムアウト `2000` ms | | dvb_usb_urb.c 相当 26, 47 |
| 応答前ディレイ `usleep_range(d, d+20000)` | | dvb_usb_urb.c 相当 41-43 |
| `usb_rcvbulkpipe` (IN) | | usb_urb.c:150-152 |

### si2168.c / si2168_priv.h / si2168.h

| 値 | 意味 | 行 |
|---|---|---|
| `TIMEOUT 140` ms | cmd 待ち | si2168.c:43 |
| ready = `args[0] & 0x80`, error = `& 0x40` | | si2168.c:55, 66 |
| poll `usleep_range(2500,3500)` | | si2168.c:58 |
| `SI2168_ARGLEN 30` | | si2168_priv.h:46 |
| `SI2168_TS_PARALLEL 0x06` | | si2168.h:39 |
| `SI2168_TS_SERIAL 0x03` | | si2168.h:40 |
| `SI2168_TS_TRISTATE 0x00` | | si2168.h:41 |
| `SI2168_TS_CLK_MANUAL 0x20` | | si2168.h:42 |
| I2C アドレス 0x64 (コメント) | | si2168.h:33 |
| `\x14\x00\x0d\x10\xe8\x03` | CLK_MANUAL 時 0x100D | si2168.c:96 |
| `\x14\x00\x01\x10\x10\x00` | TS モード 0x1001 | si2168.c:102 |
| `0x40` (gapped) | | si2168.c:110 |
| `\xa0\x01` / `\x90\x01` / `\x50\x01` | ステータス DVB-T/C/T2 | si2168.c:134, 137, 140 |
| `\x82\x00` | BER | si2168.c:177 |
| `\x84\x01` | UCB | si2168.c:208 |
| `0xffff` (UCB 無効) | | si2168.c:217 |
| `0x20` / `0x30` / `0x70` | delivery system | si2168.c:254, 257, 260 |
| 帯域コード `0x02,0x05,0x06,0x07,0x08,0x09,0x0a,0x0f` | | si2168.c:267-285 |
| `\x88\x02\x02\x02\x02` | | si2168.c:294 |
| `\x89\x21\x06\x11\xff\x98` (T) / `\x89\xf0` (C) / `\x89\x20` (T2) | | si2168.c:301, 303, 305 |
| `\x52` (PLP) | | si2168.c:312 |
| `\x51\x03` | | si2168.c:322 |
| `\x12\x08\x04` | | si2168.c:327 |
| `\x14\x00\x0c\x10\x12\x00` | 0x100C | si2168.c:332 |
| `\x14\x00\x06\x10\x24\x00` | 0x1006 | si2168.c:337 |
| `\x14\x00\x07\x10\x00\x24` | 0x1007 | si2168.c:342 |
| `\x14\x00\x0a\x10\x00\x00` | 0x100A (delivery, bw) | si2168.c:347 |
| `\x14\x00\x02\x11` | 0x1102 (symbol rate) | si2168.c:357 |
| `\x14\x00\x0f\x10\x10\x00` | 0x100F | si2168.c:365 |
| `\x14\x00\x09\x10\xe3\x08` | 0x1009 (ts_clock_inv 0x00 / 0x10) | si2168.c:370-371 |
| `\x14\x00\x08\x10\xd7\x05` | 0x1008 | si2168.c:376-377 |
| `\x14\x00\x01\x12\x00\x00` | 0x1201 | si2168.c:382 |
| `\x14\x00\x01\x03\x0c\x00` | 0x0301 | si2168.c:387 |
| `\x85` | | si2168.c:392, 436 |
| `\x01\x01` | (ファーム起動, 推定) | si2168.c:496 |
| `\x11` | ファーム版数 (rlen 10) | si2168.c:502 |
| `\xc0\x12\x00\x0c\x00\x0d\x16\x00...` (13 B) | Initialize | si2168.c:422, 696 |
| `\xc0\x06\x01\x0f\x00\x20\x20\x01` | Power up | si2168.c:445, 703 |
| `\xc0\x06\x08\x0f\x00\x20\x21\x01` | warm 再開 | si2168.c:430 |
| `udelay(100)` | | si2168.c:435 |
| `%17 == 0` かつ `data[0] > 5` (新形式判定) | | si2168.c:462 |
| `%8` (旧形式) | | si2168.c:476 |
| `\x02` (rlen 13) | chip ID | si2168.c:709 |
| `SI2168_CHIP_ID_D60 'D'<<24 | 68<<16 | '6'<<8 | '0'` | | si2168_priv.h:32 |
| `\xc0\x0d\x01` / `\xc0\x0d\x00` | I2C ゲート open / close | si2168.c:607, 625 |
| `min_delay_ms = 900` | | si2168.c:595 |
| 周波数 48-870 MHz, step 62500 | | si2168.c:640-642 |
| symbol rate 1,000,000-7,200,000 | | si2168.c:643-644 |
| `\x13` | sleep | si2168.c:581 |
| `B 4.0-11` (version 比較) | | si2168.c:578 |
| `\xc0\x0d\x01` | | si2168.c:607 |

### si2157.c / si2157_priv.h / si2157.h

| 値 | 意味 | 行 |
|---|---|---|
| `TIMEOUT 80` ms | | si2157.c:38 |
| ready `& 0x80`, error `& 0x40` (-EAGAIN) | | si2157.c:50, 64 |
| テーブル `{SI2141, 0x60, ..., rom60, A10}` | | si2157.c:80-81 |
| `%17 != 0` で無効 | | si2157.c:106 |
| `\x02` (rlen 13), `part_id = args[2]`, `rom_id = args[12]` | | si2157.c:158-166 |
| `\x15\x00\x02\x04` | xtal trim (< 16 で warm) | si2157.c:228, 235 |
| `\xc0\x05\x01\x00\x00\x0b\x00\x00\x01` | SI2146 power up | si2157.c:243 |
| `\xc0\x00\x0d\x0e\x00\x01\x01\x01\x01\x03` | SI2141 power up | si2157.c:247 |
| `\xc0\x00\x0c\x00\x00\x01\x01\x01\x01\x01\x01\x02\x00\x00\x01` | 他 power up | si2157.c:250 |
| `\xc0\x08\x01\x02\x00\x00\x01` | SI2141 wake-up | si2157.c:260 |
| `\x01\x01` | reboot | si2157.c:273 |
| `\x11` (rlen 10) | ファーム版数 | si2157.c:281 |
| `\x14\x00\x01\x05\x01\x00` / `06` / `07` | 状態フラグ | si2157.c:292, 299, 306 |
| `TUN_TIMEOUT 40`, `DIG_TIMEOUT 30`, `ANALOG_TIMEOUT 150` | | si2157.c:357-359 |
| lock mask デジタル `0x04`、アナログ `0x02` | | si2157.c:368, 370 |
| 完了 `(status & 0x81) == 0x81`、成功 `(status & 0xC0) == 0x80` | | si2157.c:391, 428 |
| `if_frequency = 5000000` | DVB-C/T の IF (Hz) | si2157.c:451, 1020 |
| `bw` 6 MHz `bw = 6` | | si2157.c:465-467 |
| delivery `0x30` (DVB-C annex A/C) | | si2157.c:494 |
| `\x14\x00\x03\x07` | 0x0703 | si2157.c:507 |
| `\x14\x00\x02\x07` | 0x0702 (if_port / AGC) | si2157.c:519-521 |
| `\x14\x00\x06\x07` | 0x0706 (IF kHz) | si2157.c:531 |
| `\x41\x00` | 周波数設定 (LE32, Hz) | si2157.c:544 |
| `\x42\x00` (rlen 12) | RSSI / 統計 | si2157.c:926, 981 |
| `strength + 100`, clamp `0..80`, `×0xFFFF/80` | | si2157.c:939-941 |
| `\x16\x00` | standby | si2157.c:342 |
| `schedule_delayed_work` 1000 ms / 2000 ms | | si2157.c:317, 991 |
| id テーブル `"si2141" → SI2141` | | si2157.c:1102 |
| `SI2141 = 41` | part ID | si2157_priv.h:45 |
| `SI2157_ARGLEN 30` | | si2157_priv.h:62 |
| `SI2141_A10_FIRMWARE "dvb-tuner-si2141-a10-01.fw"` | | si2157_priv.h:80 |
| `SI2141_60_FIRMWARE "dvb_driver_si2141_rom60.fw"` | | si2157_priv.h:84 |
| `if_port` の説明 (pins 9+11 / 12+13) | | si2157.h:25-27 |
| I2C アドレス 0x60 (コメント) | | si2157.h:30 |

---

## 8. 移植時の要点 (要約)

- 通信は **bulk 0x01 (OUT) / 0x81 (IN)** のコマンドプロトコル。タイムアウトは 2000 ms。
- TS は **bulk 0x82**。転送単位 4096、8 本を並行。
- T230C (0xC68A) は `ts_mode = 0x26`、`ts_clock_inv = 1`、Si2168 DL 不要 (FX2 DL なし)。
- Si2168 ファームは 17 バイト形式 (3349 B, 197 チャンク)。Si2141 ファームも 17 バイト形式 (1666 B, 98 チャンク)。
- DVB-C 6 MHz / 5274 ksym/s は kernel のコード上で到達可能。QAM 指定・ロールオフ・Annex C は kernel のコードに無い。
- J.83 Annex C (ケーブル, 日本) は kernel のコードでは到達不能。そのまま移植しても Annex C のチューニングは成立しない。ユーザモード版でも、Si2168 の API 文書 (非掲載) で Annex 切替の有無を確認する必要がある。
