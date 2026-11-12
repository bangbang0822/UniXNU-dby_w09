# UniXNU on the Huawei MatePad 11 (dby-w09)

Device repository for the **DBY-W09** (Huawei MatePad 11, Qualcomm SM8250 / kona). It carries
the board description, the board device tree and XNU patch order, the kernel defconfig, the board
package and build scripts, the firmware selection, the partition layout and the porting record.
Shared XNU, driver and kit implementations come from [UniXNU](https://github.com/UniXNU/UniXNU);
the SM8250 hardware description and the UFS driver come from the two feeds pinned in
`feeds.toml`.

Memory and storage are not fixed on this model. MatePad 11 units ship with 6 or 8 GiB of RAM and
with 128 or 256 GiB of UFS, so both are per-unit inputs rather than board constants, and this
record was measured on one 6 GiB / 128 GiB unit.

Target software: iPadOS 17.7.11 (build 21H461) with the donor identity `iPad7,1` / `j120ap`, the
same verified combination as the official [sheng](https://github.com/UniXNU/UniXNU-sheng) port.
Chip ID `0x8011`, board ID `12`.

- [Documentation](https://unixnu.com) · [Porting](https://unixnu.com/porting/) ·
  [Building](https://unixnu.com/building) · [Device record](porting.toml)

## Values that belong to the unit, not to the board

Everything below was measured on the record's own tablet. Another unit needs its own reading of
each one before its first boot; copying these numbers is the failure this section exists to
prevent. The list is the whole of it: nothing else in the repository differs between two DBY-W09
units, and a value that does not appear here is a board fact rather than a unit fact.

| Value | Where it is written | Read it from |
| --- | --- | --- |
| DRAM size | `board.toml` `[payload] dram_size`, and the `memory@80000000` cell in `dts/dby_w09.dts` | The unit itself. The two must be equal or the ADT generator refuses the pair, and `dram_size` is the value the payload hands DarwinBoot. |
| `userdata` LBA range | `board.toml` `[userdata]` `first_lba` / `last_lba` | A full backup of that unit's GPT (`gpt_main0.bin`, LUN 0). Never the numbers here: they describe one 128 GiB tablet's table. |
| Data volume UUID | `configs/dby_w09_defconfig` `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` | The APFS volume UUID inside the Data seed you build or reuse for that unit. A seed grown from an existing one keeps its UUID; a fresh fixture does not, and then the kernel must be rebuilt. |
| Platform seed | `--unique-chip-id` at payload build time, or `UNIXNU_UNIQUE_CHIP_ID` | Per-image, not per-unit-persistent: it seeds the device tree the payload carries. Never commit it. |
| Touch firmware project | `package/DbyTouch/DbyTouchFw.h` picks among `W2599Z1100` / `1300` / `2700` at run time | The unit's own `oeminfo` partition. The driver scans it and selects; the default is this record's panel. The three images themselves come from the unit's stock package (see `proprietary-files.txt`). |

Two of these interact: the Data seed's volume UUID is compiled into the kernel, so replacing the
seed without rebuilding the kernel produces `offline scratch Data mount failed: 2`.

Opposite direction: a kernel argument list is measured against the 1000-byte command line and each
argument has one reader; `board.toml`'s comments name it next to every entry, so a unit does not
change the arguments, the board does.


## Layout

```text
board.toml                  schema 2: board, SoC feed, OS profile, [components], firmware, payload
feeds.toml                  this board's git pins: the sm8250 SoC feed and the ShengUfs driver
unixnu.lock                 resolved git sources, written by `unixnu fetch`
dts/dby_w09.dts             board device tree; the SoC feed supplies dts/sm8250.dtsi
patches/xnu/series          board XNU patch order
configs/dby_w09_defconfig   kernel configuration (owner Data volume UUID is compiled in)
configs/dby_w09_abiaudit_defconfig
                            variant used to record the storage ABI bindings
package/                    board C/C++ sources built into the kernel (boot progress, probes, policy)
scripts/                    board build recipes, ADT generator, XNU patchers, checks
proprietary-files.txt       Apple inputs and their hashes; never committed
local.toml.example          machine-specific source overrides (copy to the ignored local.toml)
porting.toml                porting record: areas, statuses and evidence
```

`package/` appears in the assembled tree as `kits/dby_w09`.

## Verify the record

This repository is meant to be readable and checkable without a build host and without the
tablet. `scripts/check-repository.py` needs only Python 3.11 and git, and it fails when the
record and the tree stop agreeing:

```sh
python3 scripts/check-repository.py            # every check, one line each
python3 scripts/check-repository.py --verbose  # plus what it skipped and why
```

It verifies that `board.toml` and `porting.toml` parse and agree on the device identity,
that every path either file names exists, that every tracked code file is claimed by a
record area and every claim matches a file, that each patch named by a series resolves
(series entries that live in the assembled tree are reported, not silently passed), that
the two copies the repository keeps of the same patch are still byte-identical, that no
committed file names a build host path, and that no generated or private artefact is
tracked. Exit status is 0 only when all of it holds.

The same two checks run before a push, if you install the hooks once:

```sh
git config core.hooksPath .githooks
```

That also checks every commit subject against the convention in `AGENTS.md`, which is the
one place the rule is written down.

The upstream record validator is a separate, stronger check and needs a UniXNU checkout:

```sh
python3 scripts/porting-record.py check --device ../UniXNU-dby_w09
```

## Contributing

`main` is a protected branch: changes arrive through a pull request, and merging is the
review step. Two ways to propose one:

- **inside this repository**, if you have write access: push a branch and open a pull
  request against `main`;
- **from a fork**, if you do not: fork, push a branch, open a pull request.

Install the hooks once per clone so the checks run before anything leaves your machine:

```sh
git config core.hooksPath .githooks
```

The checks are `scripts/check-repository.py` (does the repository still describe itself)
and `scripts/check-commit-messages.py` (does every subject follow the convention in
`AGENTS.md`). Both are also runnable by hand and need no build host.
## Prerequisites

1. **UniXNU** checkout plus an arm64 Apple toolchain (clang/lld, `xcrun` shims) on the build host.
   `unixnu` needs Python 3.11 or newer (`tomllib`).
2. **UEFI firmware**: a Mu-Silicium checkout exposing `Platforms/Huawei/dby_w09Pkg/dby_w09.dsc`,
   passed with `--uefi` (or `UNIXNU_UEFI`). `scripts/build-boot-progress.py` needs it for the
   panic-screen glyph table, so it is a build input of the kernel step, not only of the firmware.
3. **iPadOS restore image** `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, from the URL in
   `board.toml [ipsw]`; `./unixnu ipsw fetch` extracts and verifies the members against
   `[ipsw.sha256]`. Apple content never enters Git.
4. **A scratch Data volume seed** for the writable `/private/var`. It must be produced on macOS
   (`prepare-owner-data-fixture.py` uses `hdiutil`); see
   [userdata](https://unixnu.com/guide/userdata). Its APFS volume UUID has to equal
   `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` in the defconfig, otherwise the kernel refuses the mount
   (`offline scratch Data mount failed: 2`). Growing an existing seed keeps the UUID; a fresh
   fixture does not and therefore needs a kernel rebuild.
5. **Trust cache**: the unmodified original static + System bundle, built by
   `scripts/build-original-system-trust.py` and passed with `--trust-cache`.

## Build

```sh
cd UniXNU
./unixnu defconfig dby_w09_defconfig -D ../UniXNU-dby_w09 --uefi ../uefi-sheng
./unixnu fetch    -D ../UniXNU-dby_w09
./unixnu build kernel  -D ../UniXNU-dby_w09 --jobs 4
./unixnu build kernel  -D ../UniXNU-dby_w09 --jobs 4
./unixnu build payload -D ../UniXNU-dby_w09 --jobs 4 \
  --unique-chip-id <platform seed for the device tree> \
  --trust-cache build/original-static-system-trust/original-static-system.bundle \
  --data-ramdisk <data-final.dbr>
```

Two things about that sequence are load-bearing:

- **`defconfig` is not optional.** The kernel build reads only `out/dby-w09/.config`, so editing
  `configs/dby_w09_defconfig` has no effect until `defconfig` regenerates it. A defconfig-only
  change looks like it built and then fails at runtime.
- **`build kernel` runs twice.** `scripts/build-platform-power.py` links a no-op `sheng_pmgr_run`
  stub on its first pass and prints `Build the kernel again to link the bridge.`; only the second
  pass compiles the real bridge. The payload step refuses a kernel without `_sheng_pmgr_run`
  (`AMFI: No PMGR?!?`), so a single pass produces an image that cannot boot.

Kernel arguments reach the device through the payload's ESP `unixnu.toml`, so a
`[payload] kernel_args` change only needs a payload rebuild.

- **The output directory comes from `[board] name`, not from a flag.** `--out DIR` re-roots the tree but the
  sub-path is always `<slug>/ipados-<os>-<build>/<board id>`, so renaming the board moves everything under it
  and leaves the previous tree where it was. The slug is `dby-w09` because the upstream device catalog rejects
  an underscore, while the board id inside the path stays `dby_w09`.

### A fresh output tree needs three inputs no builder here can make

Those are build-tree artifacts, not repository files: they embed the donor Apple kernelcache, so they can never
be committed and a clean tree cannot pass the gates without them.

| Input | Who requires it | What it is |
| --- | --- | --- |
| `tree/build/xnu-prelink/*-candidate.macho` and `.json` | `scripts/build-sheng-hid-input.py`, `scripts/build-gop-legacy-display.py` | this board's previous prelinked kernel (`out/<slug>/…/kernel`), which the HID and display ABI audits compare instruction identity against. `scripts/build-platform-power.py` documents the same reference and has a no-op fallback; the other two have none, so on a new tree they abort with `No reference candidate available`. |
| `tree/build/display-analysis/mobile-graphics-legacy-abi.json` | `scripts/audit-mobile-graphics-legacy.py` | the recorded runtime observation the audit falls back to when the live `gdb-stop.json` is absent, keyed by `runtime_metadata_checkpoint_sha256`. |
| `tree/build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a` | `scripts/unixnu/kernel.py` (`XNU_VERIFIED_ROOT_ARCHIVE`) | the archive whose only producer, `scripts/build-cpu-desktop-kernel.py`, needs macOS `hdiutil`. The guide's rule for an archive no builder makes is to supply it in the build tree. |

## Flash

```sh
fastboot flash boot    <firmware image>    # packed by the firmware tree's own script
fastboot flash preload <payload.img>       # ~208 MiB
```

`preload` carries the payload because `cache` is 100 MiB and the prelinked kernel is 119 MiB.
Never re-image `userdata`: on this device it holds the iPadOS root volume the kernel mounts.

`unixnu bootimg` packs this board's image correctly but `bootimg.verify()` reads the v0/v1 header
fields, so any `header_version >= 3` board rejects its own output; the firmware image is packed by
the firmware tree instead (see Deviations).

## Status

Measured on hardware, 2026-11-05 (prelinked kernel `60f3dbac…`, payload `68059cca…`,
firmware tree `WQSQW/uefi-dby_w09@423e222`). See
`porting.toml` for the per-area record and evidence.

| Area | State |
| --- | --- |
| UEFI firmware, panel, keys, USB at boot | boots on hardware |
| Kernel boot to launchd | reaches user space; original iPadOS binaries exec against the unmodified AMFI |
| Root and data volumes | native UFS root, OS and SystemOS windows mounted, packed Data seed with a persistent overlay |
| Display | XNU presents scanout: the iPadOS lock screen is on the panel |
| Input | `DbyTouch` (accepted from a stack verified on this same panel model) reports points on the lock screen; the Home and volume keys reach XNU |
| CPU | eight cores up as two clusters (`unixnu,cluster-type` E on cpu0-3, P on cpu4-7, `MAX_CPU_CLUSTERS 2`, `__ARM_AMP__`), EPSS frequency votes issued by `package/CpuFreqProbe.cpp` |
| GPU | not ported; the current frames are CPU-rendered scanout |
| Audio, network, camera | not started |

## Firmware boot message

Earlier builds printed `Error: Image at … start failed: Not Found` before UniXNU started.
That was Mu's `MsBootPolicy.efi`: a BDS boot option (`UefiApplicationEntryPoint`) whose entry
point returns the boot-policy status, because Silicium's `MsBootPolicyLibGetBootSequence`
returns `EFI_UNSUPPORTED`, so Mu falls back to the hardcoded HDD → USB → PXE scan and reports
`EFI_NOT_FOUND`. It is a boot-option retry, not a UniXNU failure. Wiring `UniXNUDxe` so the
platform BDS loads `UniXNU.efi` from the firmware volume by GUID (`4e07b0b`), keeping only the
official silent `SecurityLockAuditLib` instance (`bfe74bc`) and pinning `PcdDebugPrintErrorLevel`
to the errors-only value (`042a4e4`) took it off the boot path; the panel now shows the boot
menu without a log band over it (`423e222`).

## Deviations

Recorded so that a reviewer knows what is not yet publishable, rather than discovering it:

- **The firmware FD is not byte-reproducible, and that is not a defect of this port.** The
  firmware embeds `__TIME__` and lld writes a fresh random CodeView GUID into every PDB
  reference, so no FD hash can be pinned. The equivalent proof was run per FFS: building
  `423e222` through `unixnu build firmware` and comparing the volume against the FD extracted
  from the flashed `boot` partition gives 106 of 107 FFS entries byte-identical, the outlier
  being `PcdDx_.ffs`, 8 bytes of PDB GUID after the `RSDS` signature.
- The Mu workflow `.github/workflows/active_devices.yml` inherited from the firmware upstream
  still builds all platforms and fails here; it is not this board's entry point. The board
  entry is `./unixnu build firmware --uefi <uefi checkout> --edk2 <fetched mu_basecore>
  -D ../UniXNU-dby_w09 --jobs 4` with `scripts/check-dby_w09.py` as preflight.
- Storage ABI bindings are consumed from a recorded static table
  (`scripts/build-storage-bindings.py`); the live audit that would regenerate it on this board is
  not yet wired.
- The verified-root policy and probe inputs come from a macOS-only producer; the tree carries
  recorded copies rather than a reproducible build.
- `porting.toml` is validated by `scripts/porting-record.py` from a current UniXNU checkout, and
  `.github/workflows/porting-record.yml` calls the reusable workflow from `UniXNU/UniXNU`.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). Apple restore-image content, derived volume images and
owner signing keys are not part of this repository.
