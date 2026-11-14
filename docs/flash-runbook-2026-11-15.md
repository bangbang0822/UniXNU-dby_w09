# Flashing and first-boot runbook - dby_w09

The image and what to do with it. Everything below is a host-side action; nothing here has been run
against the tablet yet, so treat the sequence as the plan it is rather than a record.

## What to flash

`out/dby-w09-boot.img`, 220,028,928 bytes, sha256
`3f436907049fa288ad53c97f7a0f5bcb7abb3b90f5ce433a236a0708c9f06ef7`, header version 2, kernel 1,179,625
bytes, ramdisk 218,103,808 bytes. Its contents were read back with `scripts/check-boot-image.py`, which matched
the kernel, the trust cache, the packed Data seed and all 51 boot arguments against the build reports
(`docs/build-2026-11-15-first-boot-image.md`).

Before flashing, confirm the image on disk is the one that was checked:

```sh
sha256sum out/dby-w09-boot.img      # Linux
Get-FileHash out\dby-w09-boot.img -Algorithm SHA256   # Windows
```

## Prerequisites

| item | why |
|---|---|
| `fastboot`/`adb` (Android platform-tools) or `edl` (Qualcomm 9008) | nothing on the build host provided either; the build machine had no `adb`, `fastboot` or `edl` and no visible USB device |
| an unlocked bootloader | `fastboot flash boot` is refused on a locked Huawei unit; `fastboot oem device-info` reports the state |
| the tablet in **fastboot**: power off, then volume-down + power | the bootloader's own fastboot mode, not the OS's |
| a serial capture | `serial=3` is in the baked command line; the console is where every row below appears |

For the 9008 route the unit must be in EDL instead; `edl r boot boot.img` reads and `edl w boot <file>`
writes. That path does not need an unlocked bootloader but does need the Qualcomm driver.

## Sequence

1. Non-destructive first: `fastboot boot out/dby-w09-boot.img` loads the image without writing a partition.
   If anything in the packaging is wrong, a power cycle restores the previous state.
2. Capture the console from the first row. The boot arguments already ask for `-v`, `debug=0x14e` and
   `-enable_kprintf_spam`, and `panic_screen = true` keeps a panic on screen instead of rebooting into a loop.
3. Only after a clean boot, `fastboot flash boot out/dby-w09-boot.img` and `fastboot reboot`.

## Rows worth capturing, and what each settles

| row | settles |
|---|---|
| `[pa] pins before: scl119=0x... sda120=0x... qup3=N` | the smart-PA pin ids. `func1` on both pins and their presence in the `qup3` count is the measurement `porting.toml`'s audio limit is waiting for; a pair the bootloader did not mux shows up as a count that excludes them |
| `offline scratch Data mount` | whether the packed seed's volume UUID matches `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` (`2138CDAC-F86A-4DED-9EF1-09F27363DB2A`). A mismatch panics with `offline scratch Data mount failed: 2` |
| the PMGR stand-in row | that `pmgr=2` reached the kernel and the linked `sheng_pmgr_run` ran; its absence is the AMFI `No PMGR` panic instead |
| UFS root rows | that the whole-userdata window opened with the block size the device answered |
| AudioReach/GPR rows | whether the DSP handshake and the playback ladder get past `dby-audio=2` |

If a row disagrees with the record, that is a finding, not a problem with the image: the record says what each
one was expected to be and why.

## If it does not boot

* **Stuck in the bootloader**: the shim or the firmware volume did not load. Re-check `fd_sha256`
  (`fa45488b6c07cdc999e4031eea1e0405b6b7d1a5a9458389027e2272bc82c841`) against the firmware build, and that
  `fastboot boot` reports no write error.
* **Panic on the panic screen**: the text names the subsystem. The AMFI `No PMGR` panic means `pmgr=2` did not
  reach the kernel; `offline scratch Data mount failed: 2` means the seed UUID does not match the compiled one,
  which needs `unixnu defconfig` plus a kernel rebuild with the seed's UUID.
* **Boots but the panel stays dark**: `offline-legacy-*` rows describe where the display handoff stopped; the
  display area's limits list what each stage expects.

## After the run

Whatever the console shows, the record moves: `porting.toml`'s affected areas take the observation, and the
limits that were waiting on a boot either close or gain a narrower question. `hardware_tested` in
`out/dby-w09-boot.json` is `false` until that happens.
