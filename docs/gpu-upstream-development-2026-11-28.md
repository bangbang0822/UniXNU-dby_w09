# DBY-W09 GPU development against current UniXNU

Snapshot: 2026-11-28. Device tree: `6992b639`; local core: `973f86d6`; public UniXNU `main`: `e7bddb9a`. This is a development plan, not a device qualification. Keep the last booted payload and its SHA-256 before any new flash.

## Decision and source boundary

The Metal → AdrenoMetal → metal2vulkan → Turnip → Darwin KMD → A650 route remains appropriate. The public catalog's `Adreno740` implements A740, not A650; it does not supply a drop-in A650 KMD. The [GPU porting guide](https://unixnu.com/porting/gpu) explicitly requires a matching backend for another GPU generation. [j716f](https://unixnu.com/devices/j716f) reports an A650 KMD and NativeAdrenoMetal/Turnip rendering its iPadOS 17.7.11 desktop at 60 FPS. That is evidence for the architecture, not a measurement on DBY-W09.

The frontend has moved substantially. Public UniXNU pins `UniXNU/metal2vulkan` runtime v2; this board's core pins the older `steelbrain/metal2vulkan` revision plus local bridge patches. Public UniXNU also records the Adreno kernel ABI in the kernel artifact and rejects a mismatched addon. The [current AdrenoMetal page](https://unixnu.com/drivers/adreno-metal) says the old `metal-flow` switch and v1 counters are unavailable. The [general GPU guide](https://unixnu.com/porting/gpu) still describes those old defaults. Use the selected source and build manifest, not a boot argument copied from either page, to determine behavior.

## Baseline on this tablet

- `docs/gpu-8250-approach-survey-2026-11-21.md` compares the A650 forks; `docs/gpu-120fps-spec-2026-11-24.md` records a roughly 60 Hz panel, 42.26 ms mean submit, about 23% fence timeouts, no working vsync and a null `clock.idle`. Those are historical observations; bind any new measurement to its running payload hash.
- `drivers/Adreno740/` and `drivers/AdrenoMetal/` are board-vendored forks (92 and 71 tracked files). `scripts/build-adreno650.py` builds A650 code into the legacy `libAdreno740.a` path. The path name is a build contract, not the GPU generation.
- `feeds.toml` already pins shared SM8250 support, including `kits/Sm8250Platform`; `package/Sm8250Platform/` keeps another copy of the A650 GMU operations. Compare exact revisions and move identical shared code only after a stable build.
- `porting.toml` marks `gpu` implemented based on the recorded bring-up. The current S0a/S0b instrumentation still needs a device run tied to the payload hash.

## Execution sequence

### G0. Rebuild a fixed baseline

Use one pinned core, component set, OS profile, Data seed and owner trust bundle. Run `unixnu defconfig`, `status --json`, `tree`, kernel, runtime, payload and the repository/record/regression checks. Save the kernel, addon, payload, Data seed and source revisions with SHA-256. The current cold build is **not complete**: run `46026133847` reached `ArmDTPlatform` but stopped when `build-verified-root-probe.py` reused a compile-record include directory absent after the bootstrap link. Resolve that build blocker before changing GPU behavior. No bootstrap image is flashable.

### G1. Establish a 60 Hz correctness baseline

On a reviewed shipping image, prove a bounded known-image render with pixel readback or CRC, successful fence completion, zero SMMU/GPU faults and a presented frame. Capture one fixed touch-active scene and one idle scene for 60 seconds each. Join submission, fence, callback and display-latch timings by frame or command ID; keep CPU and GPU clock domains separate until calibrated. Record panel scan interval, frame p50/p95/p99, submit and fence wait distributions, timeout count, reset count and warm/cold shader-cache identity. The known-image and desktop observations are separate acceptance gates.

Do not replay the former `clock.idle` sleep experiment as a default: the board's interrupt-delivery evidence and prior freeze require a bounded, reviewed fallback. The S0a/S0b instrumentation in the 120 Hz spec has not yet been device-qualified against the current source hashes.

### G2. Audit the new frontend in isolation

On a separate branch, resolve public UniXNU, AdrenoMetal, the translator, Turnip patches and A650 KMD as one versioned set. Compare `NativeAdrenoABI.h`, user-client request sizes, IOSurface imports, cache modes and owner trust packaging. Run the upstream runtime-interface check, arm64/iOS 17 bridge type check, host frontend checks and a clean addon build. Reject a mixed KMD/frontend ABI; do not copy only the new translator pin into the old bridge. A changed runtime also changes shader-cache identity and diagnostic schema.

Only after host checks pass, compare old and new on the same tablet scene, panel mode, battery range and temperature range. One runtime change per flash; a wrong pixel, client crash, fault or regression returns to the saved payload.

### G3. Put reusable code in its owner

Compare the board's A650 GMU files byte-for-byte with the pinned `UniXNU-sm8250-common` kit. Keep board firmware choices, votes, addresses and boot arguments here; place SoC operations and reusable KMD/frontend changes in their feed or component, pinned from `feeds.toml`. Preserve runtime class, symbol, bundle and protocol names. Rebuild and review the archive's contents after each move.

### G4. Revisit 120 Hz

Start only after G1 is stable. Prove a 120 Hz panel mode from this device or its official firmware, then use the existing v2 120 Hz spec's one-variable stages. A 60 Hz panel cannot validate a 120 FPS acceptance claim. Require 8.333 ms scan periods and complete frame-path timing before optimizing a particular stage.

## Pre-flash review and record

For each code change, write the mechanism, shared-state breakage, host checks, and the exact tablet row/panel outcome that decides success or failure. Run `check-repository.py`, `porting-record.py check`, and `check-regression.py` against the built payload. Update the `gpu` area in `porting.toml` with the same push. Never flash an unapproved change or write GPU logs to `userdata`.

References: [GPU adaptation](https://unixnu.com/porting/gpu), [AdrenoMetal](https://unixnu.com/drivers/adreno-metal), [component catalog](https://unixnu.com/reference/components), [j716f record](https://unixnu.com/devices/j716f), [shared SM8250 support](https://github.com/FFT-infra/UniXNU-sm8250-common).
