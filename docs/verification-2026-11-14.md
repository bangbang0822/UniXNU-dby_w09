# Verification record - 2026-11-14 remediation pass

What was run, what it printed, and what it does not cover. Everything here ran in this repository at the
commit the audit disposition record names; the batch files the audit came from are outside the tree.

## Kernel

`./unixnu build kernel -j 20 -D <device tree>` against the core branch `dby-w09`, after every code change of
this pass, most recently: `RC=0`, `kernel.macho` sha256
`7f3e9d95a192f8160bf72a02d6eceaccd10c66aac277e2fdf1348fa059981df7` (53,231,616 bytes). Changes to comments,
`static_assert`s and whitespace were checked by rebuilding and comparing the hash: it is unchanged, which is
the evidence that they changed nothing at run time.

## Host gates

| gate | verdict | covers |
|---|---|---|
| `scripts/check-repository.py` | all checks passed | layout, tracked artefacts, claims and the ABI records |
| `scripts/check-commit-messages.py` | all commit subjects follow the convention | type/area/subject rules over the range |
| `scripts/check-adsp-host.py` | PASS qcom adsp cores: 4184 checks | MDT parsing, SMEM, SMP2P, GLINK, QRTR, APR, AudioReach |
| `scripts/check-apr-host.py` | PASS apr: core handshake, AFE v1/v2 parameters, TDM ladder, ADM route, ASM stream, answers, decoder | APR packet layer and the audio ladder |
| `scripts/check-audioreach-host.py` | PASS audioreach: 182 checks | AudioReach graph and GPR |
| `scripts/check-dby-torch-host.py` | PASS DbyTorch host checks | torch core, SPMI ladder, iOS rules, boot arguments |
| `scripts/check-gpr-host.py` | PASS GPR: 37 checks | GPR codec |
| `scripts/check-modem-fw-host.py` | PASS modem firmware reader (FAT32): 112889 checks, 164318 disk reads | firmware-image reader |
| `scripts/check-nvt-host.py` | PASS: N9Z protocol layer against the vendor driver's own register map and reset sequence | Novatek touch protocol |
| `scripts/check-nvt-trim-tables.py` (also run by the gate above) | PASS trim tables: both copies carry the same 23 rows and masks | the two copies of the N9Z version/trim table cannot drift apart unnoticed |
| `scripts/check-nvt-trim-tables.py` (also run by the gate above) | PASS trim tables: both copies carry the same 23 rows and masks | the two copies of the N9Z version/trim table cannot drift apart unnoticed |
| `scripts/check-qrtr-host.py` | PASS qrtr and service locator: 41 checks | QRTR and the locator |
| `scripts/check-smp-topology.py --tree <built tree>` | dby_w09.dts declares 8 unixnu,cluster-type node(s); cluster-type is backed by the cluster patch and MAX_CPU_CLUSTERS >= 2 | the device tree's cluster description against the patched kernel |

## ShengUfs host suites

| suite | verdict | covers |
|---|---|---|
| `drivers/ShengUfs/tests/run-native-ufs-bsd-root-core.sh` | PASS: NativeUfs BSD-root core host and ASan/UBSan | GPT CRC/uniqueness/overlap, HFSX, range limits, write protection, sync failure |
| `drivers/ShengUfs/tests/RootCommands.cpp` | PASS: actual command/queue path and whole-userdata root window; malformed DMA responses, bounds, short reads, sense, quarantine, queue refusal and full-width DMA | the command path and both window routes |
| `drivers/ShengUfs/tests/RootWindowRead.cpp` | PASS: window geometry, 512/4K/128K boundaries, tail, no unverified copy, partial progress, concurrent snapshot lock, completion outside lock | the shared window reader |
| `drivers/ShengUfs/tests/SmmuTakeover.cpp` | PASS: SMMU geometry/conflicts, configurable stream IDs, VA64 high-DMA regression, full-range width limits, ordered configuration, reuse/repair and blocked-write refusal | the SMMU planner and configurator |
| `drivers/ShengUfs/tests/TransferPoll.cpp` | PASS: unaccepted, delayed, immediate, failed, stuck and fatal UFS completions | the completion state machine |
| `drivers/ShengUfs/tests/check_legacy_descriptors.py --output <dir>` | PASS 14 legacy/current descriptor comparisons | the historical builder prefixes against the current command path |

## Component pins

The board selects eighteen components (`board.toml`'s `[components].include`) and the published core's
`components.toml` carries all eighteen: no id is missing, and each entry names a repository. Seven of the pins
are gitlinks - commits, not branch tips - so `git ls-remote` does not advertise them; that is the shape of a
pinned component, not a broken pin. Two were confirmed against the tree they build from: the catalogue's
`RamBlock` revision `4be2d7eb05ef` and the core tree's gitlink for `drivers/RamBlock` are the same commit, and
`drivers/DarwinBoot`'s gitlink (`5243c68ec332`) matches its checkout. The other five were not verified
individually here; what a fresh clone would have to do is fetch the component and check the object exists.

The core branch published at `uni-xnu-dby-w09/UniXNU` is `dby-w09` = `113070122ab4`, which is what this tree
built against (the WSL clone's `origin/dby-w09` ref was stale and made it look one commit behind; `git
ls-remote` against the remote answers `1130701...`).

## What is not covered

* **No hardware run.** Every change that alters a decision at run time is listed in `porting.toml`'s limits
  with what a boot would have to show: the window lease needs start/stop driven against I/O, the block-size
  refusal needs a drive that answers something other than 4096, and the `dby-apr=1/2` stream gate needs one
  boot at each level.
* **Objective-C, Objective-C++ and the iOS HAL do not build on this host** (IOKit, Metal, the audio server
  plug-in). Those files were reviewed, and the ones whose claims changed say so in their comments.
* **`scripts/check-regression.py` needs a built payload** (`out/<image>/payload/payload.json`), which this
  working tree does not have; it did not run here.
* **`drivers/ShengUfs/tests/check_arm64_root_commands.py` fails in this environment** (`power query` returns
  0xe00002ca). It fails identically with and without this pass's changes - checked by stashing the one file it
  depends on and re-running - so it is an environment limitation, not a regression.
* **`build-adreno650.py`'s accelerator and pwrup objects are not in this board's archive**, so correctness
  fixes in them do not move this kernel; the header says so.
