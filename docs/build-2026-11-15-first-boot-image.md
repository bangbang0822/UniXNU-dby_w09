# First boot image for the dby_w09 port - 2026-11-15

The chain that produced it, the artifacts it produced, and the two defects it exposed. Nothing here has run
on the tablet: `hardware_tested` is false in every report below, and that is the next step, not a completed
one.

## Inputs

| input | where it came from | how it was checked |
|---|---|---|
| IPSW members | `./unixnu ipsw fetch` - the profile asks for `KernelCache`, `DeviceTree`, `StaticTrustCache`, `Cryptex1,SystemTrustCache`, and the fetch reads the zip directory and each member with HTTP range requests, so no complete restore image is downloaded | each member against `[ipsw.sha256]` |
| trust bundle | `scripts/prepare-trust-inputs.py --ipsw-dir <out/ipsw/21H461> --write-cache`, which takes the two fetched IM4P members, extracts the version-2 modules and assembles the bundle | the bundle hash is `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7`, which is `board.toml`'s `original_trust_cache_sha256` |
| Data seed | the macOS runner (`bangbang0822/dby-w09-data-seed`, workflow `Data seed (macOS)`, run `43610415507` on `macos-26`): fetch the OS volume, check its UUID and `sbin/launchd`, build the fixture, grow it to 8 GiB, mark Setup done, pack to DBRDISK1 and upload the artifact | `data-final.dbr` 1,261,728 bytes packed, `1039dd44d89d2e60ce8e122e5b14e59715c00b9240849de59fccc7e2a6437c5b`; volume UUID `2138CDAC-F86A-4DED-9EF1-09F27363DB2A`, now `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` |
| platform seed | `--unique-chip-id` on the payload step, an ephemeral value that is not recorded here | per-image, per the README table |

## Artifacts

| artifact | size | sha256 |
|---|---|---|
| `kernel/kernel.macho` | 53,284,864 | `322eaafff81a41c5df394a659da3bba655a5a9d98b020ac4d665611c09b89215` |
| `payload/payload.img` | 218,103,808 | `5520b0abbe151dd5e38615fdc0538985ed09ff026733515f6ec44f14a6167f08` |
| `boot/boot.img` | 220,028,928 | `3f436907049fa288ad53c97f7a0f5bcb7abb3b90f5ce433a236a0708c9f06ef7` |

The payload report (`payload/payload.json`) records the command line it baked, the trust cache and the Data
seed: `kernel_sha256 322eaafff81a41c5df394a659da3bba655a5a9d98b020ac4d665611c09b89215`, `payload_kernel_sha256 0bd2ce983d41f66f7b61c98e7b5c7867eb63440e0ff0c33859c8a685cbbf9ddb`, `adt_sha256 fb3be63b245e6425a05c47d3abfdb18a9e796a26ce14e25ac31ffd9e4e8fc93e`, `trust_cache_sha256 7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7`, `data_ramdisk_sha256 1039dd44d89d2e60ce8e122e5b14e59715c00b9240849de59fccc7e2a6437c5b`, `payload_sha256 5520b0abbe151dd5e38615fdc0538985ed09ff026733515f6ec44f14a6167f08`. The boot report adds
`fd_sha256 fa45488b6c07cdc999e4031eea1e0405b6b7d1a5a9458389027e2272bc82c841`, `dtb_sha256 0a92b1fa197dc744e13f006945ab6ae8f2ef2fed7ef0e62b800c89783a589ba9`, `shim_bytes 192`, `header_version 2`,
`kernel 1,179,625 bytes`, `ramdisk 218,103,808 bytes`.

## Two defects the chain exposed

1. **`unixnu bootimg` never passed the dtb to mkbootimg.** A UEFI device config that asks for
   `header_version = 2` also names `[boot_image] dtb`, and mkbootimg refuses a v2 header with an empty dtb -
   but the step only forwarded the flags in `MKBOOTIMG_FLAGS`, which has no `dtb`. Every such board failed
   with `DTB image must not be empty` no matter which tree `--dtb` pointed at; the core fix resolves the
   config's dtb against the UEFI checkout and passes `--dtb` (and `--dtb_offset`) for any header above 1
   (UniXNU `491efd7`).
2. **The board still passed `sheng-pmgr=2`.** Upstream commit `47ec6a5` dropped the `sheng-` prefix from
   every boot argument and `scripts/unixnu/payload.py` refuses a native-UFS-root payload unless the command
   line carries exactly one `pmgr=2`. Nothing in this tree reads the argument - the kernel calls
   `sheng_pmgr_run()` from the sheng-platform-power patch and the component publishes none - so the marker
   was renamed (this repository, `614673f`). The other twelve `sheng-` arguments stay: this board's own
   drivers parse them (`CpuFreqProbe.cpp`, `DbyTouch.cpp`, `KeyStoreProbe.cpp`, `BootLog.c`, `LinkStubs.c`,
   `Adreno740`, `GopFramebuffer`).

## Next

Flash it and read the console. `fastboot boot out/dby-w09-boot.img` boots without writing a partition;
`fastboot flash boot` writes it and needs an unlocked bootloader. Rows worth capturing, in the order they
would answer open questions:

* `[pa] pins before: ...` - the smart-PA pin measurement, which is the only thing still standing between
  `kSclGpio = 119` / `kSdaGpio = 120` and a measured value (`porting.toml`, `audio` area).
* `offline scratch Data mount` - the seed's UUID has to match the one compiled in, or the boot panics with
  `offline scratch Data mount failed: 2`.
* the `pmgr` stand-in row, the UFS root rows and the AudioReach handshake - each is a limit with a stated
  observation.
