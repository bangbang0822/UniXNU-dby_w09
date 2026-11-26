# GPU direct scanout on dby_w09: design and integration plan

Status: 2026-11-18, revision 2. Revision 1 was reviewed and failed on six points; this file
supersedes it and every one of those points is addressed below. Nothing here has been applied yet.

## 1. What is blocked, and by what

`drivers/GopFramebuffer/NativeLegacyDisplay.cpp:153` gates the direct path:

```cpp
if (mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state
   || !(native_ufs_dpu_state>>31) || !primary) return;
```

`package/LinkStubs.c:48-53` states the situation itself:

> "GopFramebuffer's compositor-scanout path imports the UFS DPU window as weak_import… NativeUfs
> defines both; **this board selects ShengUfs, so neither exists here**."

`native_ufs_dpu_map()` returns 0 and `native_ufs_dpu_state` stays 0, so `>>31` is never set and the
path returns immediately. Enabling the argument alone cannot work.

**Why a strong definition would replace the stub**: the weak/strong rule alone is not the guarantee —
a `weak_import` reference does not pull an archive member in. The guarantee is
`scripts/build-xnu-4k-baseline.sh:633`'s `-Wl,-force_load,$UFS_ARCHIVE`: `libShengUfs.a` is force
loaded, so its strong definition wins and the stub is dropped. Anyone changing that link line breaks
this silently.

## 2. The implementation that exists

`/root/core-pub/out/feeds/NativeUfs-alioth-556351a5/DpuWindow.h` (216 lines) is a stage-1
translation for the display's apps_smmu context bank, written for SM8250. Its own header states the
per-board values are parameters, not constants.

| Parameter | sheng reference | This board | State |
| --- | --- | --- | --- |
| display streams | `0x1c00` / `0x1c02` | `0x820` / `0x821` per `DpuWindow.h:10` | **unverified** — see §6 |
| boot scanout IOVA | `0xb8000000` | `0x9c000000` (our PHYFB) | verified from this board's log |
| apps_smmu base | — | `0x15000000` | verified, see below |

The apps_smmu base: `NativeUfs.cpp:275` maps `0x15000000ULL` with length `0xd0000`. Revision 1 cited
`:273`, which is a `return` statement. A stronger witness is `:415` `smmuBypassStream`, which runs
every boot and panics on failure, plus the 2026-10-17 hardware row at `:402-409` describing
apps_smmu's `sCR0` and the missing UFS host stream match.

**Dependencies, corrected**: `DpuWindow.h` needs `read32`/`write32` (`ShengUfs/NativeUfs.cpp:178`,
not `:139`), `IOBufferMemoryDescriptor`, `IOLock`, `IODelay`, `kernel_task`, `bzero` and
`PE_parse_boot_argn` — all already included by `ShengUfs/NativeUfs.cpp:30-37`. The `dc civac` cache
helper and `dpuClean` are **inside** the header (`:46-51`); no external DC helper is needed.
`ShengUfs/NativeUfs.cpp:139` is a different function and is not the relevant one.

## 3. The four edits

Revision 1 said three. The fourth is the one that makes the fallback claim true.

### 3.1 Add the header

Copy `DpuWindow.h` into `tree/drivers/ShengUfs/`. It is self-contained given §2's dependencies.

### 3.2 Publish the globals, the forwarder and the entry point in `ShengUfs/NativeUfs.cpp`

The header requires three globals at **file scope, outside every namespace** — inside an anonymous
namespace they get internal linkage and the display driver's `weak_import` never resolves:

```cpp
extern "C" {
volatile uint32_t native_ufs_dpu_smmu[24] = {};
volatile uint32_t native_ufs_dpu_state = 0;
volatile uint32_t native_ufs_dpu_check[8] = {};
}
```

Then, **after `write32` exists (`ShengUfs/NativeUfs.cpp:178`), at file scope — not inside the
anonymous namespace** — `#include "DpuWindow.h"`. Revision 2 said "inside the anonymous namespace",
which both contradicted §3.2's own linkage rule and had no target: this tree's `read32`/`write32` are
file-scope `static` (`:139` / `:178`) outside every namespace, so an include "inside" one has nothing
to attach to. The globals, the forwarder and the exported entry point must all be at file scope for
the same reason — the display driver imports them by name, and internal linkage would hide them.

Note also that `DpuWindow.h` itself defines `native_ufs_dpu_map` (`:182`) as `extern "C"`. If the
copy that lands in this tree kept internal linkage, the `LinkStubs.c` weak stub would win and
`:198` would return 0 forever — the same silent failure this plan exists to remove.

Then — this is what revision 1 omitted — add the **forwarder** that the header does
not provide. `DpuWindow.h` defines only `dpuWindowBuildImpl` (`:200`); the name `dpuWindowBuild` does
not exist in it. Alioth supplies the forwarder at `NativeUfs.cpp:1045`, and its exported entry point
at `:1236`:

```cpp
static int dpuWindowBuild(uintptr_t smmu) { return dpuWindowBuildImpl(smmu); }   // forwarder, :1045 pattern
...
extern "C" int native_ufs_dpu_window_build(uintptr_t smmu) { return dpuWindowBuild(smmu); }
```

Both must sit outside every namespace, for the same linkage reason as the globals.

### 3.3 Call it before the display starts

`package/LegacyDisplayProbe.cpp` currently has no declaration of this symbol. Add one next to the
other weak externs, then call it in the `else` branch that actually starts the display — **inside
`:570`'s `else`, before `:571`** — not merely "after `:527`". `:527` is where `pendingLegacyDisplay`
is read out; `:531-569` is the surface wait and nub setup, and the `phase == 3` branch at `:563-569`
never calls `start` at all, so a call placed there would run on boots that never start the display.
`:573`'s `display->start(nub)` is the target (it reaches `startDirect` at `:317`). Guard it so a
build without `ShengUfs` does not call NULL:

```cpp
extern "C" int native_ufs_dpu_window_build(uintptr_t) __attribute__((weak_import));
...
if (&native_ufs_dpu_window_build) native_ufs_dpu_window_build(kAppsSmmuBase);
```

`kAppsSmmuBase` should be a named constant rather than a literal; `ShengUfs` already carries
`0x15000000` in five places, and alioth exports `native_ufs_smmu_base()` (`:1239`) for exactly this.

### 3.4 Fix the boot IOVA and stride — in **both** places, not one

`drivers/GopFramebuffer/NativeLegacyDisplay.cpp:150` currently reads:

```cpp
kBootIova = 0xb8000000U, kBootStride = 0x2fa0
```

Those are **sheng's** values. This board's GOP mapping is `0x9c000000` and its row is `6400`
(`0x1900`). The derivation matters and was missing: the boot log's `[swapok]` row reports
`video=1600x2560 row=6400 depth=32 base=9c000000`, and `1600 × 4 = 6400`, so `row`/`stride` is
**bytes per line** — the same unit VIG's `SRC_STRIDE` takes, which is why `0x1900` is the right
literal and not a pixel count. `directToBoot()` at `:219-223` points both VIG pipes at `kBootIova`
with `kBootStride`, so with sheng's numbers the tear-down path would aim the pipes at
`0xb8000000` with a 12200-byte row while the copy path writes `0x9c000000`. Correcting these two
constants is a prerequisite, not a refinement.

**The half revision 2 missed, and the one that matters more.** Once stage-1 is on for this context
bank, the VIG fetches through translation, so the address the DPU emits is an **IOVA**, not a
physical address. `dpuEnable` receives the boot IOVA as a parameter and is expected to map it
identity-wise in the window. The exported entry point
`native_ufs_dpu_window_build(uintptr_t smmu)` **takes only the SMMU base — no IOVA at all**; the
boot IOVA inside the header comes from `PE_parse_boot_argn("native-dpu-boot-iova", …)` whose default
is `0x9c000000` per `DpuWindow.h:210`. So:

- If that default is what this header revision really carries, the identity mapping works and the
  display driver's two constants must match it.
- If it carries sheng's `0xb8000000` instead, then the moment `=1` enables stage-1 the DPU fetches
  `0x9c000000` against a table whose identity range starts elsewhere, and the panel faults
  immediately.

**This must be settled by reading the header text before any flash**, and the answer recorded next to
the constant. A one-line `native-dpu-boot-iova=0x9c000000` in `kernel_args` is the cheap hedge if the
default turns out to be sheng's, since `dpuWindowBuildImpl` parses it.

**SoC constants versus board values** (a distinction revision 2 blurred): `kVig{0x4000,0x6000}`,
`kCtlFlush=0x15018`, the `+0x14` / `+0x24` register offsets, the `0x3` flush value and the 16-bit
stride mask are **SM8250 DPU 9.2 SoC facts** and should not be touched. Only `kBootIova` and
`kBootStride` are board values, and only they changed.

## 4. Risks, including the one revision 1 omitted

**R1 — the first `flip()` may take the system down, and the earlier attribution for that crash was
retracted by the record itself.** Revision 2 wrote "reading VIG 0/1 aborts the kernel" as fact. The
record does not support that: `porting.toml:393` attributes the abort to a VIG read, but `:400`
records the ablation that **withdrew the pipe read and the kernel still died at the same point at
69 s**, with `x0=0x9c000000`, `esr=0x96000010` (a synchronous external abort) and `pc`/`lr` inside
`startVsync()`'s lambda — so the fault is on the **vsync path**, not proven to be the copy path.
Revision 3 therefore states only what survives: the record explicitly withdraws the VIG-read
attribution, and the risk that the first `flip()` can abort rather than return an error stands on
its own because `flip()` writes VIG registers and **there is no MDP clock or power-domain code
anywhere in this fork** (verified: all occurrences in `GopFramebuffer` are logical IOPM calls and
comments). The unknown is whether the vsync-path abort and the flip path are the same fault; the
first `=2` boot will tell.

**R2 — a partial write is reported as a failure.** `native_ufs_dpu_map` returns 0 from inside its
loop after earlier L3 entries were already written, and a failing `dpuTlbFlush` also returns 0
**after** the tables were replaced. Revision 1 called "map failures > 0" a rejection; it must read
"a half-written window is possible", and recovery means **explicitly disabling the window**, not
retrying it and not letting the next key press reuse the same slot — reusing the pick would rewrite
over a window the DPU may already be walking. Note this path belongs to the per-surface mapping
(`=2`); the `=1` round never reaches it.

**R5 — ordering against the UFS SMMU takeover.** `NativeUfs.cpp:415`'s `smmuBypassStream` runs on
every boot and panics on failure, i.e. the UFS driver takes over apps_smmu's stream table. If the
display's late probe builds the window **before** that takeover, the takeover can reset the table
underneath it while `native_ufs_dpu_state` still reads installed. The call must therefore be placed
where the UFS SMMU configuration has already happened, and the `=1` round's `state` row is the check:
`0x8000000x` means the window survived to the point of reading, `0` means it was never built.

**R3 — unverified stream numbers** (see §6). A wrong stream fails safely: the SMR match misses and
the code refuses with `0xdead0001` rather than mapping anything.

**R4 — a wrong IOVA after §3.4 is fixed.** If `0x9c000000` is right but the stride is not, the
picture tears instead of failing.

## 5. Safeguards that do exist

| Safeguard | Where |
| --- | --- |
| Nothing happens without the argument | `dpuWindowBuildImpl`: `if (dpu < 2 && !scanout) return 0;` |
| Idempotent | `if (native_ufs_dpu_state >> 31) return 1;` |
| Refusal leaves the bank as found | header contract; verified in the refusal paths this review checked |
| The copy path is kept | `NativeLegacyDisplay` retains it; direct scanout takes over only when the state bit is set |
| A build without `ShengUfs` still links and does nothing | the `weak_import` + `if (&symbol)` guard of §3.3 |

## 6. What is not verified here

- **`0x820` / `0x821`**: three local cross-checks are unavailable. `dts/dby_w09.dts` carries only
  ufs/ufsphy/CPU/memory nodes; the pinned `sm8250.dtsi` carries only `gpu_smmu@3da0000`; the
  `k40-kernel` reference tree that `DpuWindow.h:10` cites does not exist on this host. Treat the
  stream numbers as **unverified** and fault-safe (`0xdead0001` refusal), not as known-good.
- **The nine counters cannot be read on the current build.** `native_legacy_direct[9]` and
  `native_ufs_dpu_state` are written but **never read** by any translation unit that compiles: the
  only console row from `drivers/GopFramebuffer` is `DbyDisplay: copy=` (`:503`). Revision 1 §5
  described four outcomes that no flash could distinguish. **A `BootLog.c` row is a prerequisite for
  the measurement**, not an afterthought: gate it on `native_ufs_dpu_state` or on the argument, and
  print `direct[0..8]`, `state` and `check[0..7]`.

## 7. Order of work

The step split is deliberate: `NativeLegacyDisplay.cpp:153` requires `mode >= 2` **and**
`state>>31`, while `DpuWindow.h:207` builds the window for any non-zero `scanout`. So
`sheng-dpu-scanout=1` installs the SMMU window **without flipping** — a free check of the table
build — and `=2` then enables the flip. One variable per flash.

1. **Correct §3.4's two constants first** (`kBootIova`, `kBootStride`). Without this the flip path
   aims at the wrong address and there is no fallback.
2. Apply §3.1–3.3, rebuild the kernel, and verify the symbols rather than assume: `nm -m` on
   `build/sheng-ufs/libShengUfs.a` and on `kernel.macho` must both show `native_ufs_dpu_map` and
   `native_ufs_dpu_window_build` as **non-weak external** definitions inside `NativeUfs.o`, not as
   the `LinkStubs.o` stub.
3. Add the `BootLog.c` row of §6, rebuild, so the next flash can be read at all.
4. Payload with `sheng-dpu-scanout=1` (54 args / 971 bytes + 20 = **991** of the 1000-byte gate;
   `payload.py:35` counts `' '.join` without a NUL). **This round has two possible outcomes and both
   are informative:**
   - `state` reads `0x8000000x` **and the picture is unchanged** → stage-1 is on for the display
     bank, the formulas are right, and the table is sound. Proceed to `=2`.
   - `state` reads `0` with no `dpuEnable` having run → `=1` was read-only on this header (the
     host harness's `scanout=0` case shows this shape), so nothing was exercised and the round
     proved nothing about the table. Settle `DpuWindow.h:207` against the header text before
     concluding anything.

   **Be aware `=1` is not free.** When `dpuEnable` does run it sets the context bank's SCTLR bit 0,
   i.e. it enables stage-1 on a **live display stream**. The failure signature is specific: the
   panel freezes on its last frame (or goes black/white) while the console stays alive,
   `DbyDisplay: copy=` keeps printing every 32 frames and `[swapok]` keeps appearing — CPU still
   drawing, glass not updating. If the table half-wrote or the boot IOVA is not identity-mapped,
   that is exactly what happens. Distinguish it from the `:400` sync-abort signature (which dies)
   and from `0xdead0001` (refused, picture should be unchanged).
5. Only then `sheng-dpu-scanout=2`, watching for R1's abort.

Revert: `sheng-dpu-scanout` is an argument, and the previous payloads `12e6349f` and `a47c82ec` are
retained. Nothing in this plan is irreversible at the payload level; §3.4 is the only change that
touches the display driver and it is two constants with values quoted from this board's own log.

## 8. What this does not provide

GPU composition or Metal: `sheng-gpu-metal=1` still needs the AdrenoMetal payload plugin, which
`board.toml:716-718` records as blocked by the clang-24 ICE at `NativeAdrenoService.cpp:95`. The
target for this work is "swaps retire by direct scanout with no stripes" — gts7xlwifi's sentence on
the same SoC — not a frame-rate number.

**Also rejected**: a minimal fake `native_ufs_dpu_map` that only sets the state bit. With the bit set
and no page tables, the DPU would fetch `0x10000000+` against an empty table and fault. And swapping
`ShengUfs` for `NativeUfs` would mean redoing the root mount, the dual stream and the storage
bindings — weeks, not hours. The 216-line header has no smaller equivalent.
