# PX-S1UD V2.0 / PX-Q1UD

PX-S1UD V2.0 is a one-channel terrestrial tuner. PX-Q1UD supports four simultaneous terrestrial channels. Both are USB 2.0 devices without an internal B-CAS reader. Their Siano RIO tuner path differs from PX-S1UR, which uses the ITE/IT930x path already present in px4_drv.

The Linux USB driver table identifies `3275:0080` as a Siano RIO device. The official PLEX download page lists Windows drivers for both models and a Linux download for PX-S1UD. PLEX does not publish the USB ID or PX-Q1UD's internal USB topology in its product specifications.

## OS paths

- **Windows:** DriverHost_PX4 exposes one ISDB-T receiver for each Siano RIO USB function. PX-S1UD V2.0 uses one function. PX-Q1UD's four-function layout is reported by third-party device inventories; the topology still requires confirmation on PX-Q1UD hardware.
- **macOS:** DriverHost_PX4 uses the same Siano protocol path as Windows. The bundled `isdbt_rio.inp` firmware is accompanied by `LICENCE.siano`.
- **Linux:** px4_drv binds `3275:0080` and creates one terrestrial `/dev/px4video*` receiver for each Siano RIO USB function. The Linux upstream `smsusb` driver matches the same USB ID and publishes a DVB frontend, so each USB function can be owned by only one of the drivers. Unbind the device from `smsusb` before binding it to px4_drv; doing so can interrupt any recording that currently uses the Siano device. The driver requests `isdbt_rio.inp` through the kernel firmware loader, so install that file in the system firmware search path first.

Windows requires the included WinUSB INF to be installed for `3275:0080`. The official BDA driver and the WinUSB driver cannot own the same USB function at once. No card reader is exposed for either product, so descrambling requires a separate card reader and the applicable software.

## Firmware

The Windows/macOS host expects `isdbt_rio.inp` next to DriverHost_PX4, unless `SianoFirmware` in `DriverHost_PX4.ini` names another path. Linux asks the kernel firmware loader for the same filename, normally from `/lib/firmware`. The bundled binary matches the copy in the upstream Linux firmware repository (SHA-256 `054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818`). Its redistribution terms are included in `LICENCE.siano`.

## Signal reporting

On Windows and macOS, `BonDriver_PX4::GetSignalLevel()` reports Siano's demodulator SNR as C/N in dB. `GET_STATISTICS_EX_RES` begins with a 32-bit `request_result`, followed by `sms_isdbt_stats_ex`; `is_demod_locked` is at offset 12 and signed `SNR` (integer dB) at offset 20 within that structure. The host converts SNR to millidecibels by multiplying by 1000, then the existing BonDriver interface converts it back to dB. This is not calibrated RF input power in dBm. The Linux upstream Siano DVB frontend uses the same SNR field and publishes it in millidecibels.

## ISDB-C and J.83B

Siano RIO support in this repository is limited to terrestrial ISDB-T. The public Linux `smsdvb` frontend registers only `SYS_DVBT` and `SYS_ISDBT`, and the firmware named in this repository (`isdbt_rio.inp`) is the ISDB-T image. No public Siano firmware, mode, or register document describes DVB-C, ISDB-C, or ITU-T J.83 Annex B reception. PX-S1UD V2.0 and PX-Q1UD therefore have no ISDB-C or J.83B path in any OS build.

A user report says PX-S1UD receives SKY PerfecTV! Premium Hikari (J.83B). The report includes no firmware name, mode, channel, or TS log, and this repository has not reproduced it. Until a capture shows lock and valid TS on a J.83B signal, the report is not treated as evidence of support.

## Verification status

The portable protocol codec has automated tests. The macOS DriverHost builds with the Siano backend and CTest passes. The Windows v145 x86/x64 `build.ps1` and hardware-independent tests pass. On Windows hardware, 60-second captures on 13ch, 15ch, and 22ch had zero CC, TEI, and sync errors and average bitrates within 0.2% of the official BDA driver. In a four-function concurrent run, all four channels (13ch, 15ch, 22ch, 28ch) produced TS. A separate function-level check found that USB function `&0&1` fails to produce `BDA_DATA` on 28ch under WinUSB although the official BDA driver receives 28ch on that function; the other three WinUSB functions received 28ch. The WinUSB host now reports a tune failure when no TS data arrives, and this function-specific reception gap remains unresolved. Linux Siano reception, 30-minute captures, hotplug, and driver ownership switching still require hardware verification. Until the remaining checks pass, PX-S1UD V2.0 and PX-Q1UD remain experimental.

## References

- [PLEX PX-S1UD V2.0 specifications](https://plex-net.co.jp/itemreco/px-s1ud/)
- [PLEX PX-Q1UD specifications](https://plex-net.co.jp/item/tv-tuner/px-q1ud/)
- [PLEX driver downloads](https://plex-net.co.jp/download/)
- [Linux upstream Siano USB driver](https://github.com/torvalds/linux/blob/master/drivers/media/usb/siano/smsusb.c)
- [Linux upstream Siano DVB statistics](https://github.com/torvalds/linux/blob/master/drivers/media/common/siano/smsdvb-main.c#L2880-L2932)
- [Siano extended statistics structure](https://github.com/torvalds/linux/blob/master/drivers/media/common/siano/smscoreapi.h#L3468-L3479)
- [Linux upstream Siano firmware names](https://codebrowser.dev/linux/linux/drivers/media/common/siano/smscoreapi.h.html)
- [Upstream `isdbt_rio.inp`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/isdbt_rio.inp)
- [GPL Siano RIO userspace reference implementation](https://github.com/Khronos31/siano-userland)
- [Third-party USB inventory reference for PX-Q1UD's four Siano functions](https://github.com/tsukumijima/ISDBScanner/blob/master/isdb_scanner/tuner.py)
