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

## Verification status

The portable protocol codec has automated tests. The Linux kernel module has not yet been built against Linux kernel headers in this macOS workspace, and Linux Siano reception has not been tested on hardware. The macOS DriverHost builds with the Siano backend. The new Windows INF is present; Windows catalog verification has passed in a remote staging tree, but the Windows build could not proceed because that host lacks the repository's required v143 toolset. Long-run capture, hotplug, TS continuity under concurrent Q1UD reception, and driver ownership switching still require hardware verification. Until those checks pass, PX-S1UD V2.0 and PX-Q1UD remain experimental.

## References

- [PLEX PX-S1UD V2.0 specifications](https://plex-net.co.jp/itemreco/px-s1ud/)
- [PLEX PX-Q1UD specifications](https://plex-net.co.jp/item/tv-tuner/px-q1ud/)
- [PLEX driver downloads](https://plex-net.co.jp/download/)
- [Linux upstream Siano USB driver](https://github.com/torvalds/linux/blob/master/drivers/media/usb/siano/smsusb.c)
- [Linux upstream Siano firmware names](https://codebrowser.dev/linux/linux/drivers/media/common/siano/smscoreapi.h.html)
- [Upstream `isdbt_rio.inp`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/isdbt_rio.inp)
- [GPL Siano RIO userspace reference implementation](https://github.com/Khronos31/siano-userland)
- [Third-party USB inventory reference for PX-Q1UD's four Siano functions](https://github.com/tsukumijima/ISDBScanner/blob/master/isdb_scanner/tuner.py)
