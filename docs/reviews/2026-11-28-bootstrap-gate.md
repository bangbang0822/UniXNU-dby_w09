# Bootstrap provider gate and record gate, 2026-11-28

Scope: branch `codex/rebuild-snapshot-r2-sync`, commits `3d220f7`, `be6de02` and `548b003`. This note records why the bootstrap link kept failing, what changed in the board tree, and which evidence supports each claim. It does not claim a hosted package run has passed.

## What was blocked

| Run | Stop point | Cause |
| --- | --- | --- |
| [46153746882](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46153746882) | `porting record: device: unknown component QcomXnuUsbDebug` | The reusable record workflow was pinned to core `086d5372`, whose catalog lacks the module. |
| [46154194890](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46154194890) | `porting record: device: unknown component ShengCs35l43` | The workflow reference moved, but the reusable body still checked out `UniXNU/UniXNU@main`, so the checker read the public main catalog. |
| [46143394163](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46143394163) | Bootstrap link and, separately, shipping link | The bootstrap pass links before the archive recipes and left `_native_verified_block_volume_device`, `_native_verified_root_probe`, `_qcom_usb_debug_control_*`, `_sheng_pmgr_run` undefined; the shipping pass then left the legacy display, CmdDB, RPMh and RSC providers undefined. |
| [46156006362](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46156006362) | `require the preserved Data seed` | No `data.dbr` cache is restorable on this branch; the seed cache belongs to `codex/rebuild-snapshot-r2`. |
| [46156104185](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46156104185) | Cancelled before the build step | The gate added in `cd2d818` was incomplete: it was placed before the owner switches were read and covered only part of the chain that references verified-root providers. |

## What changed

### 1. Record gate (commit `3d220f7`)

`.github/workflows/porting-record.yml` no longer calls the reusable workflow. It checks out the device repository at `device` and the board core at `tools`, pins `bangbang0822/UniXNU-core-dby-w09@f28ea590d6e0f4a912f256490c49300921a39032`, and runs:

```
python3 tools/scripts/porting-record.py check --device device --base "$BASE"
```

The pinned catalog contains all 23 selected components and their dependencies; the local resolution against that catalog returned no unknown component. `porting.toml` claims the workflow path under `configuration`, so a later edit to the gate asks for a record change instead of running silently.

### 2. Bootstrap provider gate (commit `548b003`)

`scripts/build-xnu-4k-baseline.sh:92` now zeroes the whole owner chain for `XNU_BOOTSTRAP=1` before any provider switch is read: `XNU_VERIFIED_ROOT_PROBE`, `XNU_OWNER_ROOT_AUDIT`, `XNU_OWNER_ROOT_TRUST`, `XNU_OWNER_CACHE_MOUNT`, `XNU_OWNER_CACHE_STARTUP`, `XNU_OWNER_BOOT_MANIFEST`, `XNU_OWNER_DATA_MOUNT`, `XNU_OWNER_CRYPTEX_FILES`, `XNU_OWNER_UNION_SYSTEMOS`, `XNU_SHENG_PMGR`, `XNU_QCOM_USB_DEBUG` and `XNU_NATIVE_LEGACY_DISPLAY`. The shipping pass (`XNU_BOOTSTRAP=0`) keeps every value the board selected.

The gate has to be environment level, for two reasons that were both measured by reading the script:

- The archive list is assembled only when `XNU_BOOTSTRAP=0` (`scripts/build-xnu-4k-baseline.sh:604`), so a bootstrap link has none of the archives whose symbols the provider call sites need.
- The switches are read at different points. `XNU_OWNER_ROOT_TRUST` is read near `:303`, while `XNU_OWNER_CACHE_MOUNT` and the mount/manifest switches are read later, after `:405`. A shell-level assignment placed at the first read cannot affect the later ones. Worse, the earlier commit lowered the trust value while the bootstrap defconfig still selects `CONFIG_XNU_OWNER_CACHE_MOUNT=y`, and the dependency guard at `scripts/build-xnu-4k-baseline.sh:411` exits on that pair before the link.

### 3. Failure log (commit `be6de02` and `548b003`)

`docs/reviews/cold-build-failure-log.md` gained rows for runs `46156006362` and `46156104185` so the next run does not repeat either dispatch mistake.

## Verification

- `python3 scripts/check-repository.py` reports `all 491 checks passed`. Run it where `git` is reachable; a Windows Python without `git` falls back to walking the working tree and reports the ignored `.cache` files as failures.
- `bash -n scripts/build-xnu-4k-baseline.sh` passes.
- The record gate was replayed locally against the previous push with `scripts/porting-record.py check --device <device> --base be6de02` and printed `dby-w09: 16 structured areas checked`.
- Device documentation runs [46155961458](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46155961458) (commit `3d220f7`) and [46156182637](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46156182637) (commit `be6de02`) finished `success`, which is the hosted confirmation that the record gate reads the matching catalog.
- Package run [46156668323](https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/46156668323) was dispatched on `548b003` with `init_data=true`; its result is recorded in the failure log when it finishes, not here.

## Still open

- The shipping link has never succeeded. Its remaining undefined set (`_native_legacy_visible`, `_native_legacy_visible_sum`, `_qcom_cmd_db_find`, `_qcom_rpmh_active_request`, `_qcom_rpmh_finish`, `_qcom_rpmh_level_index`, `_qcom_rsc_layout`) points at archive injection for the legacy display and RPMh recipes, not at the bootstrap gate.
- No weak stub was added for any SoC or board provider, and none should be.
- The bootstrap kernel is also the prelink reference candidate; changing what it compiles is safe only while the archive audits that consume it accept the reduced symbol set. That has not yet been proven by a hosted run.

## Independent read-only review

A separate read-only review of `cd2d818` and `3d220f7` reached the same two blocking conclusions, and made no repository change (`git status --porcelain` was empty):

- In `cd2d818` the bootstrap block did apply only when `XNU_BOOTSTRAP=1` and was not overwritten later, but it lowered the trust switch while the bootstrap defconfig still selects `CONFIG_XNU_OWNER_CACHE_MOUNT=y`. In the current script those reads are `scripts/build-xnu-4k-baseline.sh:303` and `:405`, and the conflicting dependency guard is `:411`, so a fresh bootstrap pass exited before compiling anything.
- `cd2d818` gated six switches only. The owner cache, manifest and Data chain still reached `kern_exec.o` and `IOKitBSDInit.cpo`, whose call sites reference the verified-root providers that the bootstrap link does not have.
- `3d220f7` satisfies the gate's ROOT, glob and base rules (`scripts/porting-record.py:14` resolves ROOT to the `tools` checkout, and the checker's `fnmatchcase` matches the literal `porting.toml` path), so no defect was found in the record gate itself.
- The reviewer's recommendation was the same whole-chain environment gate that `548b003` implements; a narrow fix that only preserves the trust value would let the build proceed but still leave owner-chain symbols unresolved at the bootstrap link.

## Follow-up: run 46156668323

- The bootstrap link passed for the first time on `548b003`: the bootstrap script ran 15:35:06Z–15:39:36Z and produced no undefined-symbol output, and the job proceeded to the archive recipes.
- The job then failed in archive-29 `build-dby-camera.py` with `<kernel> does not define 5 import(s): _qcom_cmd_db_find, _qcom_rpmh_active_request, _qcom_rpmh_finish, _qcom_rpmh_level_index, _qcom_rsc_layout`.
- Cause: those five are supplied by the audio probe archive at the shipping link, while a bootstrap kernel links before every archive and therefore cannot define them. The camera recipe's kernel-symbol gate now exempts sibling-archive imports and records the deferral (`scripts/build-dby-camera.py:73`); the shipping link remains the proof.
- The same run saved the Data seed cache on this branch (`11245356338`), so later dispatches can restore it instead of rebuilding the fixture.
