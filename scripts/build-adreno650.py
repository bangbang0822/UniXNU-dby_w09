#!/usr/bin/env python3
# =============================================================================
# ORCHESTRATOR ACTION REQUIRED
#
# This script is complete on its own, but six edits live outside its scope and
# the archive does nothing until they land. Each one names the file, the exact
# anchor text, and the replacement. Anchors are quoted text, not line numbers:
# board.toml is being edited by other work in parallel (a `build-cpufreq.py`
# archive entry appeared between two reads of it on 2026-11-03), so match the
# quoted text and do not trust a number.
#
# --- (1) feeds.toml: the GPU feed pin is not enough under schema_version 2 ----
# 2026-11-05, measured: the pin IS in feeds.toml (Adreno740, kind = "driver",
# FFT-infra/Adreno650 @ 70771a7), and the tree still had no
# `{tree}/drivers/Adreno740`. Cause: under schema_version 2 `unixnu tree` keeps only
# the components board.toml selects (scripts/unixnu/components.py:102 and :111-117),
# so a [[source]] that is not in `[components] include` is dropped. Selecting the
# catalog Adreno740 component instead is the wrong shape: its dependencies pull
# Sm8550Platform and its build is `build-arm-driver.py Adreno740`.
# So the recipe falls back to the core checkout's own drivers/Adreno740
# (DRIVER_CANDIDATES below, the same idiom the kits already use). Wired-remote
# alternative, for a machine with network: `./unixnu fetch Adreno740` plus a
# components entry that adds no build recipe. `name` must be Adreno740 (the catalog
# identity) even though the git repository is Adreno650 -- the same shape as the
# reference board's feeds.toml:32-36.
#
#   anchor (feeds.toml:20-24, the array that ends the file):
#     [[source]]
#     name = "ShengUfs"
#     kind = "driver"
#     git = "https://github.com/FFT-infra/NativeUfs-alioth"
#     commit = "ab5405905a1892da03a9a53e0fa3dcd255d04055"
#
#   replacement: that block unchanged, then:
#
#     # The GPU driver this image compiles in, named for the component that
#     # consumes it (Adreno740 in components.toml; the silicon is this board's
#     # A650). Pinned to the per-client address-space line 70771a7, which is what
#     # the reference board runs. fft@bef707a is its sibling and still carries the
#     # static client-window table (slot0 512-1536 MiB, slot1 1536-1706.66 MiB)
#     # whose OutOfMemory(Device) is the README bug: this panel is 1.58x the
#     # reference's pixels, so that is the branch this board must not use.
#     [[source]]
#     name = "Adreno740"
#     kind = "driver"
#     git = "https://github.com/FFT-infra/Adreno650"
#     commit = "70771a71795d7e1a78afdb82badd7943adb0a3a0"
#
# --- (2) board.toml: three kit components, and the comment that denies them ---
# The three kits are `build_stage = "embedded", build = []` in the core catalog,
# so selecting them is the only thing that materialises kits/<name> (alioth
# board.toml:20-21 says exactly this). The comment above `[components]` currently
# states the opposite and must change with the array or it will mislead the next
# reader.
#
#   anchor (the block that introduces [components]):
#     # 目录规范 ID（components.toml）。Adreno740/AdrenoMetal 不在这里：SM8250 的 A650
#     # 没有任何目录组件覆盖（soc:configs/sm8250.config 明写 # CONFIG_XNU_NATIVE_ADRENO is not set），
#     # 是未移植项，不是省略。ShengHidInput/ShengNovatekTouch/QcomGeniSpi 也还没接（输入是
#     # porting.toml 里记录的下一个卡点）。QcomXnuUsbDebug 先在本地分支验证，不进这条链。
#     [components]
#     include = ["ArmDTPlatform", "ArmCpu", "SoftwareAES", "ShengUfs", "VirtioMmioBlock", "RamBlock", "GopFramebuffer", "XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "VerifiedRoot"]
#
#   replacement:
#     # 目录规范 ID（components.toml）。ArmSmmu4K / AdrenoPM4 / AdrenoFirmware 是
#     # kind = "kit"、build_stage = "embedded"、build = [] 的三个组件：选中它们才会把
#     # kits/<name> 的源码物化进构建树，scripts/build-adreno650.py 再从那里编译
#     # （参考板 board.toml:20-25 的做法）。Adreno740 本体由 feeds.toml 的
#     # [[source]] 提供，不在 include 里；AdrenoMetal 仍未移植。
#     # ShengHidInput/ShengNovatekTouch/QcomGeniSpi 也还没接（输入是 porting.toml 里
#     # 记录的下一个卡点）。QcomXnuUsbDebug 先在本地分支验证，不进这条链。
#     [components]
#     include = ["ArmDTPlatform", "ArmCpu", "SoftwareAES", "ShengUfs", "VirtioMmioBlock", "RamBlock", "GopFramebuffer", "XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "VerifiedRoot", "ArmSmmu4K", "AdrenoPM4", "AdrenoFirmware"]
#
# --- (3) board.toml: run the recipe, last in [kernel] archives ----------------
# It goes after the display recipe for the reference's reason (alioth
# board.toml:128-130): the display/record steps run first and this one must not
# be able to rewrite what they read. Order inside the list is also the order
# kernel.py:146 executes, and the archive has to exist before
# build-xnu-4k-baseline.sh:648 `test -f` runs.
#
#   anchor (the three lines that end [kernel] archives today):
#     "seed-mobile-graphics-record.py",
#     "build-gop-legacy-display-linux.py",
#   ]
#
#   replacement:
#     "seed-mobile-graphics-record.py",
#     "build-gop-legacy-display-linux.py",
#     # GPU（A650）：产 {tree}/build/adreno740/libAdreno740.a。名字是
#     # libAdreno740 而不是 libAdreno650，因为 build-xnu-4k-baseline.sh:647 把路径
#     # 写死成 build/adreno740/libAdreno740.a 并 force_load，不读
#     # CONFIG_XNU_NATIVE_ADRENO_ARCHIVE。见 scripts/build-adreno650.py 的文件头。
#     "build-adreno650.py",
#   ]
#
# --- (4) configs/dby_w09_defconfig: the two symbols ---------------------------
# CONFIG_XNU_NATIVE_ADRENO=y is what makes build-xnu-4k-baseline.sh:644 enter the
# block that force-loads the archive. CONFIG_XNU_NATIVE_ADRENO_ARCHIVE has no
# linker reader, but scripts/unixnu/kernel.py:165-167 requires every
# CONFIG_*_ARCHIVE path to exist, so writing it wrong still fails the build --
# write it to the same file the shell script hard-codes.
#
#   anchor (configs/dby_w09_defconfig:144-145):
#     CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y
#     # CONFIG_XNU_NATIVE_ADRENO is not set
#
#   replacement:
#     CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y
#     # GPU（A650 / SM8250 kona）。驱动由 scripts/build-adreno650.py 编成
#     # {tree}/build/adreno740/libAdreno740.a；build-xnu-4k-baseline.sh:644-650 在
#     # 这个开关为 1 时 force_load 那个固定路径（ARCHIVE 变量只被
#     # scripts/unixnu/kernel.py:165-167 当作"文件必须存在"的检查读过，链接器不读它）。
#     # force_load 是必需的：没有任何符号引用这份驱动，普通归档会被整份丢弃。
#     CONFIG_XNU_NATIVE_ADRENO=y
#     CONFIG_XNU_NATIVE_ADRENO_ARCHIVE="{tree}/build/adreno740/libAdreno740.a"
#
# --- (5) board.toml: kernel arguments, and the 1000-byte ceiling -------------
# The command line is 996 bytes today (46 arguments, measured by parsing and
# joining [payload] kernel_args exactly as scripts/unixnu/payload.py:34-37 does),
# so 4 bytes are free. Never add a GPU argument without removing one.
#
# G0 (this round, link-only) needs NO argument: with no `pipa-gpu` on the command
# line native_gpu_probe() returns at its own guard, so adding the archive cannot
# change a boot. Do not add pipa-gpu until the G-1 read is scheduled.
#
#   G1 (mode 1, the SMMU ID2 read): add "pipa-gpu=1".
#     +10 and one separator = +11 -> 1007, over the limit by 7.
#     Drop "dby-touch-dump=1" (16 + 1) -> 990. Its one-shot 8208-byte frame dump
#     has already been captured and analysed; the stage it belonged to is done.
#
#   G6 (mode 12, publish + accelerated Metal): add all four, +56 with their four
#   separators = +60 -> 1056.
#     "pipa-gpu=12" (11)  "sheng-gpu-addon=1" (17)
#     "sheng-fb-gpu=1" (14)  "sheng-vsync=60" (14)
#     Drop three (50 + 3 separators = 53) -> 1003, still 3 over:
#       "dby-touch-dump=1"      (16) one-shot dump, evidence recorded
#       "dby-touch-se=1"        (14) SE/QUP pin read; stage 8 never touches SE
#       "armcpu-diagnostics=1"  (20) diagnostic rows only
#     Drop a fourth, and choose knowingly:
#       "boot-diag-resume=3600" (21) -> 981. Consequence: BootProgress.c:535 keeps
#         its default and the diagnostic page takes the panel back after ~10 s of
#         no new UI content, which is normal for the Always-On display, so the
#         tablet alternates between the system image and our page.
#       "sheng-disable-jobs=com.apple.identityservicesd,..." (108) -> 947, which
#         also buys room for the rest of the GPU work. Consequence:
#         identityservicesd/locationd/CommCenter/bluetoothd start again on a
#         tablet with no radios and no accounts, which is what that argument was
#         added to prevent.
#     RECOMMENDED: boot-diag-resume for the first mode-11 round, because the
#     panel ping-pong is recoverable by a later image while a hung daemon at
#     startup is not, and sheng-disable-jobs can go in the round after that.
#
#   G1..G6 also want `pipa-gpu-perf` <= 6, for reason (a) in this file's header.
#
# --- (6) package/DbyGpuFirmware.h: put it beside DbyGpuRegs.h -----------------
# This repository had no DbyGpuFirmware.h; the file was generated at
# package/DbyGpuFirmware.h and it `#include "DbyGpuRegs.h"`. The DbyGpu group
# lives at package/DbyGpu/ (staged in work/tracks/gpu/repo), so either move the
# generated header into package/DbyGpu/ and delete that directory's nullptr
# placeholder, or keep the group flat. Do not keep both copies: the guard is
# DBY_GPU_FIRMWARE_H in each, so the second include silently wins or is skipped,
# and a reviewer cannot tell which pins the build saw. Regenerate with
#   python3 scripts/gen-gpu-firmware-header.py \
#       --source <reference>/package/GpuFirmwareA650.c \
#       --out package/DbyGpu/DbyGpuFirmware.h      (or package/DbyGpuFirmware.h)
#
# --- NOT an edit, but record it ----------------------------------------------
# porting.toml's gpu area should carry: the pin above, the three kits, this
# recipe, the two CONFIGs, the pipa-gpu gate, and the fact that G-1 (read the GPU
# SMMU ID2 bits[11:8], require >= 36) has not been measured. SPEC §6 lists G0 as
# host-only and that is all this change claims.
# =============================================================================
"""Build the DBY-W09 GPU driver archive at {tree}/build/adreno740/libAdreno740.a.

G0 only: this produces an archive that links, not a driver that runs. Nothing in
this script touches the device, and the archive it produces cannot execute until
the payload's command line carries `pipa-gpu=<mode>` (see the gate below).

WHY THE ARCHIVE IS CALLED libAdreno740.a EVEN THOUGH THE SILICON IS AN A650
  scripts/build-xnu-4k-baseline.sh:644-650 is the only thing that consumes this
  build, and it does:
      if [ "${XNU_NATIVE_ADRENO:-0}" = 1 ]; then
        ADRENO_ARCHIVE=$ROOT/build/adreno740/libAdreno740.a
        test -f "$ADRENO_ARCHIVE"
        KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES -Wl,-force_load,$ADRENO_ARCHIVE"
  The path and the name are hard-coded; CONFIG_XNU_NATIVE_ADRENO_ARCHIVE is not
  read there (scripts/unixnu/kernel.py:165-167 only checks that the file it names
  exists). So an archive at any other path, or under any other name, links
  nothing and reports success -- hence OUT_DIR and ARCHIVE_NAME are constants
  here, not options, and the build report repeats the line they come from.
  force_load matters for the same reason: no symbol references this driver, so a
  plain archive member would be discarded by the linker.

WHAT THIS PORTS, AND FROM WHERE
  scripts/build-adreno650.py of the reference board
  (FFT-infra/UniXNU-alioth, scripts/build-adreno650.py:26-109), adjusted for this
  board: the driver sources come from the pinned Adreno650 fork as
  drivers/Adreno740 (catalog identity, per that fork's AGENTS.md), the three kits
  from kits/<name> as materialised by the component selections, and no board
  probe is compiled (this board has none; see "deliberately not ported").

THE GATE THAT KEEPS IT FROM RUNNING: `pipa-gpu=<n>`
  package/GpuProbe.cpp:1893-1922 is the only entry point, and its first act is:
      unsigned mode = 0;
      const boolean_t parsed = PE_parse_boot_argn("pipa-gpu", &mode, sizeof mode);
      ...
      if (gMode || !parsed || !mode) return;
  so the probe returns before it maps, reads or writes a single register when the
  argument is absent, when it fails to parse, or when it is 0. `gMode` is a
  file-scope unsigned initialised to 0, so a second call is inert too. The driver
  objects in this archive are a library: nothing in them calls the probe, and the
  probe is not in this archive. Link-only really is link-only.
  Verified against the reference board's source, not inferred.

THE HARD GATE BEHIND THE ARGUMENT: the GPU SMMU's ID2 bits[11:8]
  The first read of the bring-up (SPEC §2, milestone G-1) must be the GPU SMMU
  ID2 register, because its input size decides whether user clients can exist at
  all. drivers/Adreno740/NativeAdrenoSmmuMemory.cpp:13-18 of the pinned line:
      unsigned NativeAdrenoSmmuInputBits(uint32_t id2)
      {
          static const uint8_t bits[5]={32,36,40,42,44};
          const unsigned code=(id2>>8)&15;
          return code<5?bits[code]:48;
      }
  and package/GpuProbe.cpp:1285-1289 records what a low value costs:
      // The context's own GPU address size, not a constant. Publish derives every
      // user client's VA window as (1 << input_bits) - GpuClientVaStart and
      // refuses the client when that is zero, so a hardcoded value below 33
      // rejects every non-kernel connection with kIOReturnNoResources at the
      // scope step (measured 2026-10-31: [gpu windows] mount fail stage 3 st
      // e00002be, three processes refused).
  Publication itself is NativeAdrenoPublish.cpp:146-147:
      static uint64_t clientWindowBytes(uint32_t input_bits)
      {return input_bits>32 && input_bits<=48?(UINT64_C(1)<<input_bits)-GpuClientVaStart:0;}
  So: input_bits <= 32 -> window 0 -> every non-kernel client refused. Do not
  spend a boot cycle on the ladder before that one read says >= 36.

THIS BOARD'S TWO DEVIATIONS FROM THE REFERENCE (both are, or will be, failures)
  (a) NO PERFORMANCE LEVEL 7. The reference's mode >= 8 default is
          unsigned perf = 7;   // 670 MHz, the top level   (GpuProbe.cpp:535)
      and the fork's kGpuMHz[8] = {0,305,400,441,490,525,587,670} is the
      kona-v2 (SM8250-AC) table. This board is a plain SM8250, whose top level is
      6 (587 MHz); there is no level 7 to select. Anything that passes
      `pipa-gpu-perf` must pass <= 6, and the first mode-8 round must not rely on
      the default. Source: work/tracks/gpu/SPEC.md §5, table row 3 -- unverified
      on this unit, which is why it stays a boot argument rather than a constant.
  (b) 1600x2560 PANEL. The reference board is 1080x2400, so this panel is
      1.58x its pixels. That does not change one line of driver code -- the GPU
      owns no scanout here, frames still leave through the GOP CPU copy
      (offline-legacy-phase=4 is a code-path selector, not geometry) -- but it
      raises client VA pressure exactly where the fft line died: its static
      kClientWindows table ends at 1706.66 MiB while the render server peaked at
      1727 MiB. This is why the pin must be the 70771a7 per-client address-space
      line (window = (1 << input_bits) - GpuClientVaStart per connection, one
      page table per process, TTBR0 switched by the CP) and not fft@bef707a.
      If the pin is ever moved back to fft, this archive still builds and the
      board still boots -- it just cannot serve a client on this panel.

DELIBERATELY NOT PORTED, and why
  * package/GpuProbe.cpp and package/A650Gmu.c (the mode ladder and the GMU cold
    start). They are the reference board's board code, not driver code, and this
    board has no equivalent yet. G0 is the wiring milestone; the probe is what G1
    ports. Consequence: this archive has no caller, which is exactly what makes
    G0 safe to link.
  * package/GpuFirmwareA650.c. Replaced by the generated package/
    DbyGpuFirmware.h (same symbol names, so a ported probe links unchanged).
  * NativeAdrenoAccelerator.cpp and the ioaccelerator-base-abi.json binding
    (scripts/build-adreno650.py:111-168 of the reference). That is mode 12, it
    binds three IOAcceleratorES symbols to addresses recorded from the reference
    kernel, and this board has neither the recorded ABI file nor IOGPUFamily in
    its component selection. Adding it at G0 would put unproven absolute
    addresses into a force-loaded archive.
  * NativeAdrenoService.cpp, NativeAdrenoGmuService/Platform/Config (the A740
    service personality), NativeAdrenoGpu.cpp, NativeAdrenoPwrup.cpp,
    NativeAdrenoFirmware*.cpp -- the same exclusions the reference recipe makes,
    for the same reasons.
  * The device tree. Nothing is added: all hardware facts are probe constants
    (chip id 0x06050002, GPU 0x3d00000, GMU 0x3d6a000, GPUCC 0x3d90000, SMMU
    0x3da0000), and the reference board's dts/alioth.dts:35 says "Leave the GPU
    disabled".

WHAT THIS SCRIPT DOES NOT PROVE
  It does not link a kernel, does not run llvm-nm against a kernel image, and
  does not touch the device. It proves the archive exists, has the required
  symbols, and has no unresolved driver or kit symbol. The kernel link is the
  next authority (see the "likely to fail" list in the reply that accompanies
  this file).

Usage:
  python3 scripts/build-adreno650.py                    # run from the assembled tree
  python3 scripts/build-adreno650.py --tree <tree>      # or name it
  UNIXNU_TREE=<tree> python3 scripts/build-adreno650.py # what `unixnu` sets
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCH = 'DEVELOPMENT_ARM64_BCM2837'

# build-xnu-4k-baseline.sh:647. Not a preference: the name and the directory are
# both hard-coded there, so this is the only path that links.
OUT_DIR = Path('build/adreno740')
ARCHIVE_NAME = 'libAdreno740.a'
# The kits keep their own plain archive so both consumers can share one copy
# (work/tracks/gpu-r3-wiring/REPORT.md Q1, option ii).
KITS_OUT_DIR = Path('build/adreno-kits')
KITS_ARCHIVE_NAME = 'libAdrenoKits.a'

# The C and C++ compile commands the kernel build itself recorded, so the driver
# sees the exact headers, defines, target and ABI flags this kernel was built
# with. bsd_init.o is what the reference recipe uses; osfmk/startup.o is this
# board's own preference (scripts/build-dby-touch.py:49). Either is the same
# command line for practical purposes; the first that exists wins and the report
# records which one was used.
C_RECORDS = ('bsd/DEVELOPMENT/bsd_init.o.json', 'osfmk/DEVELOPMENT/startup.o.json')
CXX_RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

# kits/<id>/<file>. The three kit components are selected in board.toml, which is
# what materialises these directories (kind = "kit", build_stage = "embedded",
# build = [] in the core components.toml). ArmSmmu4K's file list is a glob
# because the kit owns more than one translation unit.
KITS = {
    'ArmSmmu4K': ('ArmSmmu4K*.c',),
    'AdrenoPM4': ('AdrenoPM4.c',),
    'AdrenoFirmware': ('AdrenoFirmware.c', 'AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c'),
}
# Required whether or not the glob finds more: a kit that compiled to nothing is
# the failure this catches.
KIT_REQUIRED = {'ArmSmmu4K': 1, 'AdrenoPM4': 1, 'AdrenoFirmware': 2}

# drivers/Adreno740, exactly the reference recipe's set
# (scripts/build-adreno650.py:46-50 of the A650 fork's board), in that order.
DRIVER_SOURCES = (
    'NativeAdrenoGpuContext', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
    'NativeAdrenoMemory', 'NativeAdrenoCpu', 'NativeAdrenoVm', 'NativeAdrenoRing',
    'NativeAdrenoSubmit', 'NativeAdrenoTimer', 'NativeAdrenoUserClient',
    'NativeAdrenoPublish', 'NativeAdrenoCpInit', 'NativeAdrenoIrq',
    'NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory',
)

# Warning demotions the driver needs under -Werror, from the reference recipe
# (its :85-97) and this board's own scripts/build-dby-touch.py:56-61. They are
# not cosmetic: xnu's OSMetaClass / KALLOC_TYPE_DEFINE macros trip
# -Wmissing-designated-field-initializers, -Wc99-designator, -Wreorder-init-list
# and -Wunnecessary-virtual-specifier in every driver that uses them, and
# clang-24 split the designated-initializer half into its own flag.
WARNINGS = (
    '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter',
    '-Wno-incompatible-sysroot',
    '-Wno-c99-designator',
    '-Wno-missing-field-initializers',
    '-Wno-missing-designated-field-initializers',
    '-Wno-reorder-init-list',
    '-Wno-deprecated-enum-enum-conversion',
    '-Wno-unnecessary-virtual-specifier',
)

# Symbols every object set above must define. The first group is the pinned
# fork's extern "C" surface -- these are what a ported probe calls, and all of
# them were read out of NativeAdrenoGpuContext.cpp:251-297 and
# NativeAdrenoGmuContext.cpp:103-129 at 70771a7. The second group is checked by
# prefix and count instead of by name, because the kit revisions are pinned by
# components.toml, not by this repository, and a renamed helper inside a kit is
# not a failure of this recipe.
REQUIRED_SYMBOLS = (
    '_NativeAdrenoGpuContextBringup',
    '_NativeAdrenoGpuContextBringupRoutes',
    '_NativeAdrenoGpuContextBuffers',
    '_NativeAdrenoGpuContextInputBits',   # the ID2 input size the publish gate reads
    '_NativeAdrenoGpuContextVm',
    '_NativeAdrenoGpuContextObject',
    '_NativeAdrenoGmuContextBringup',
    '_NativeAdrenoGmuContextBringupRoutes',
    '_NativeAdrenoGmuContextPayloads',
    '_native_adreno_publish',
    '_native_adreno_publish_stats',
    '_native_adreno_published_service',
    '_native_adreno_fences',
    '_native_adreno_new_user_client',
    '_native_adreno_set_restart',
    '_ArmSmmu4KLivePrepare',              # NativeAdrenoSmmuMemory.cpp:33 calls it
)
REQUIRED_PREFIXES = ('_ArmSmmu4K', '_AdrenoPM4')

# A symbol the archive leaves undefined is a kernel-link failure unless it is the
# kernel's own or the board package's. The board package provides weak stand-ins
# for the whole native_adreno_* / native_gpu_* counter surface
# (package/BootProgressStubs.c, package/LinkStubs.c), so those names are looked
# up there instead of being assumed. Anything matching DRIVER_OWNED that is still
# undefined is this recipe's failure.
DRIVER_OWNED = re.compile(r'NativeAdreno|native_adreno')
# Kit symbols the driver references: since the kits moved to their own plain archive
# (work/tracks/gpu-r3-wiring/REPORT.md Q1, option ii) these are expected to be
# undefined here and resolved by build/adreno-kits/libAdrenoKits.a on the ld line.
KIT_OWNED = re.compile(r'ArmSmmu4K|AdrenoPM4|AdrenoHfi|AdrenoFirmware')


def die(message):
    raise SystemExit('build-adreno650: ' + message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_key(path, tree):
    """How a source is named in the report.

    Tree-relative when it lives in the tree. The fallback driver comes from the core
    checkout outside the tree (DRIVER_CANDIDATES), and a path that cannot be made
    relative is still recorded in full rather than dropped from the report."""
    try:
        return str(path.relative_to(tree))
    except ValueError:
        return str(path)


def read_recipe(path):
    """Load one of the kernel's recorded compile commands.

    The records are JSON objects with a trailing comma in some trees, which is
    what .strip().rstrip(',') handles."""
    return json.loads(Path(path).read_text().strip().rstrip(','))


def command_from(recipe, source, obj, extra_includes):
    """Rebuild the recorded command for one source, with this recipe's flags.

    -W / -MD / -MP / -MF / -MJ are dropped (they describe the record's own
    dependency bookkeeping, and -Weverything would re-introduce what the
    demotions remove); -I entries that no longer exist are dropped because
    -Werror turns -Wmissing-include-dirs into a hard error on a pruned tree."""
    args = recipe['arguments']
    command = [args[0]]
    skipped = []
    index = 1
    while index < len(args):
        value = args[index]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            index += 1
        elif value in ('-MF', '-MT', '-MJ'):
            index += 1
        elif value == '-Xclang' and index + 1 < len(args) and args[index + 1].startswith('-W'):
            index += 1
        elif value == '-I' and index + 1 < len(args):
            directory = args[index + 1]
            if Path(directory).is_dir():
                command += ['-I', directory]
            else:
                skipped.append(directory)
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-Weverything'):
            command.append(value)
        index += 1
    command += list(WARNINGS)
    for include in extra_includes:
        if include.is_dir():
            command += ['-I', str(include)]
        else:
            skipped.append(str(include))
    return command, recipe['directory'], skipped


def find_tool(name, compiler, fallbacks):
    """A binutil beside the compiler first (the tree's LLVM is the one that
    produced the objects), then the same LLVM from PATH, then the host wrappers.

    The order matters and used to be wrong (found 2026-11-03): the kernel's LLVM
    directory ships clang but no llvm-nm/llvm-ar, so the old order fell through to
    /usr/bin/nm -- binutils on this host, which exits 0 on a Mach-O archive after
    printing "file format not recognized" to stderr only, so nm_symbols() saw a
    complete archive as having no symbols at all and the recipe reported all 16
    required definitions absent."""
    candidates = [Path(compiler).parent / name]
    from_path = shutil.which(name)
    if from_path:
        candidates.append(Path(from_path))
    candidates += [Path(item) for item in fallbacks]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    die(f'missing {name}: it ships with the LLVM the kernel build uses')


# Where the pinned driver line can be found. `unixnu tree` keeps only selected
# components under schema_version 2 (scripts/unixnu/components.py:102, :111-117), so a
# feeds.toml [[source]] that is not in `[components] include` never reaches the tree. The
# board's own checkout carries the identical line, and it is reachable through the
# package symlink even when unixnu runs this script from inside the tree.
def driver_candidates(tree):
    candidates = [('tree', tree / 'drivers/Adreno740')]
    package = board_package(tree)
    if package is not None:
        device = package.resolve().parent
        candidates.append((f'core checkout beside {device.name}',
                           device.parent / 'UniXNU' / 'drivers' / 'Adreno740'))
    candidates.append(('core checkout beside this script',
                       ROOT.parent / 'UniXNU' / 'drivers' / 'Adreno740'))
    return candidates


def find_driver(tree):
    for origin, path in driver_candidates(tree):
        if path.is_dir():
            return path.resolve(), origin, path
    return None, None, None


def collect_kits(tree, notes):
    """The four kit source sets, in build order (they can be archived on their own)."""
    kits = tree / 'kits'
    sources = []
    for kit, patterns in KITS.items():
        directory = kits / kit
        if not directory.is_dir():
            die(f'{directory} is missing: select {kit} in board.toml [components] '
                f'include. See ORCHESTRATOR ACTION REQUIRED (2).')
        found = []
        for pattern in patterns:
            found += sorted(directory.glob(pattern))
        found = [path for path in found if path.is_file()]
        if len(found) < KIT_REQUIRED[kit]:
            die(f'{directory} has {len(found)} of the required '
                f'{KIT_REQUIRED[kit]} sources ({", ".join(patterns)})')
        notes.append(f'kit {kit}: {", ".join(path.name for path in found)}')
        sources += found
    return sources


def collect_driver(tree, notes):
    """The pinned driver's translation units, in build order."""
    driver, driver_origin, driver_path = find_driver(tree)
    if driver is None:
        tried = ', '.join(f'{origin}: {path}' for origin, path in driver_candidates(tree))
        die(f'no drivers/Adreno740 anywhere this recipe can see ({tried}): feeds.toml '
            f'has no Adreno740 [[source]], `unixnu fetch` has not run, and the core '
            f'checkout is not beside this repository. See ORCHESTRATOR ACTION '
            f'REQUIRED (1) at the top of this file.')
    if driver_origin != 'tree':
        notes.append(f'driver from the {driver_origin} ({driver}): the assembled tree '
                     f'has no drivers/Adreno740 link')
    missing = [name for name in DRIVER_SOURCES
               if not (driver / (name + '.cpp')).is_file()]
    if missing:
        die(f'{driver} is missing {", ".join(missing)}.cpp: the Adreno740 feed '
            f'pin is not the 70771a7 line this recipe targets (that line has all '
            f'{len(DRIVER_SOURCES)}), or it is a stale checkout.')
    sources = [driver / (name + '.cpp') for name in DRIVER_SOURCES]
    notes.append(f'driver {driver}: {len(DRIVER_SOURCES)} sources ({driver_origin})')
    return sources


def board_package(tree):
    """Where this board's package is: kits/<kit> in the assembled tree, package/ in
    the repository itself (the same lookup scripts/build-dby-touch.py:135-142 uses)."""
    for candidate in (tree / 'kits' / board_kit(tree), tree / 'package', ROOT / 'package'):
        if candidate and candidate.is_dir():
            return candidate
    return None


def board_kit(tree):
    """[board] kit from board.toml, so the package directory is read not guessed."""
    manifest = tree / 'board.toml'
    if manifest.is_file():
        match = re.search(r'^\s*kit\s*=\s*"([^"]+)"', manifest.read_text(), re.M)
        if match:
            return match.group(1)
    return 'dby_w09'


def stub_names(package):
    """Every native_adreno_* / native_gpu_* name the board package mentions.

    Used only to explain an undefined symbol, never to excuse one this driver
    owns: package/BootProgressStubs.c and package/LinkStubs.c define weak
    stand-ins for the whole counter surface, and BootLog.c weak-imports them."""
    names = set()
    if package is None:
        return names
    for path in list(package.rglob('*.c')) + list(package.rglob('*.cpp')):
        try:
            names.update(re.findall(r'\b(native_(?:adreno|gpu)_\w+)\b', path.read_text(errors='replace')))
        except OSError:
            continue
    return names


def nm_symbols(nm, archive):
    """(defined, undefined) sets from `nm -gP`."""
    result = subprocess.run([nm, '-gP', str(archive)], capture_output=True, text=True)
    if result.returncode:
        die(f'{nm} failed on {archive}:\n{result.stderr[-1500:]}')
    if not result.stdout.strip():
        die(f'{nm} printed no symbols for {archive}, so every required definition '
            f'would look absent. That is a tool problem, not a source problem: '
            f'binutils nm exits 0 on Mach-O and this archive is Mach-O arm64 '
            f'(llvm-objdump -h confirms it). Give find_tool() an llvm-nm.')
    defined, undefined = set(), set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        if fields[1] == 'U':
            undefined.add(fields[0])
        else:
            defined.add(fields[0])
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='assembled kernel tree; defaults to $UNIXNU_TREE or this '
                             'script\'s own tree root (what `unixnu` uses when it runs an '
                             'archive recipe)')
    parser.add_argument('--kits-only', action='store_true',
                        help='compile only the four kits into build/adreno-kits/' +
                             KITS_ARCHIVE_NAME + ' (the plain archive both consumers ' +
                             'resolve their kit symbols from)')
    args = parser.parse_args()

    # `unixnu` copies scripts/ into <tree>/scripts and runs the recipe there, so the
    # script's own parent-of-parent is the tree on the normal path. --tree covers the
    # "this repository against a tree somewhere else" case.
    tree = (args.tree or ROOT).resolve()
    records = tree / 'build/xnu/ios-obj' / ARCH
    if not records.is_dir():
        die(f'{tree} has no build/xnu/ios-obj/{ARCH}: point --tree at the assembled '
            f'tree (this recipe compiles with the commands the kernel build recorded)')

    c_record = next((records / name for name in C_RECORDS if (records / name).is_file()), None)
    cxx_record = records / CXX_RECORD
    if c_record is None:
        die(f'no C compile record under {records}: tried {", ".join(C_RECORDS)}')
    if not cxx_record.is_file():
        die(f'{cxx_record} is missing: build the kernel once before this archive')
    c_recipe = read_recipe(c_record)
    cxx_recipe = read_recipe(cxx_record)

    notes = []
    if args.kits_only:
        sources = collect_kits(tree, notes)
        out = tree / KITS_OUT_DIR
        archive = out / KITS_ARCHIVE_NAME
    else:
        sources = collect_driver(tree, notes)
        # The archive path is not configurable. build-xnu-4k-baseline.sh:647 hard-codes
        # it and force-loads exactly it, so any other location is a silent no-op.
        out = tree / OUT_DIR
        archive = out / ARCHIVE_NAME
    out.mkdir(parents=True, exist_ok=True)

    package = board_package(tree)
    includes = [tree / 'kits' / kit for kit in KITS]
    includes += [tree / '.cache/xnu-work/osfmk', tree / 'drivers/Adreno740']
    if package is not None:
        includes.append(package)
        if (package / 'DbyGpu').is_dir():
            includes.append(package / 'DbyGpu')

    objects, skipped_all = [], []
    for source in sources:
        recipe = c_recipe if source.suffix == '.c' else cxx_recipe
        obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
        command, directory, skipped = command_from(recipe, source, obj, includes)
        skipped_all += skipped
        (out / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            print(result.stdout[-1500:], file=sys.stderr, end='')
            die(f'{source}:\n{result.stderr[-2500:]}')
        objects.append(obj)
        print(f'  compiled {source.name}')

    # xcrun ar and llvm-ar write the same archive; the tree's LLVM is preferred so the
    # object format and the archiver come from one toolchain.
    ar = find_tool('llvm-ar', cxx_recipe['arguments'][0], ['/usr/bin/ar'])
    archive.unlink(missing_ok=True)
    subprocess.run([ar, 'rcs', str(archive), *[str(obj) for obj in objects]], check=True)

    if args.kits_only:
        # The kit archive is finished here: the checks below are the driver's own
        # (published entry points, native_* ownership), and none of them apply.
        print(f'PASS: {len(objects)} kit objects archived into {archive}')
        print(f'      {archive.stat().st_size} bytes, sha256 {sha256(archive)}')
        for note in notes:
            print(f'      {note}')
        return

    nm = find_tool('llvm-nm', cxx_recipe['arguments'][0], ['/usr/bin/nm'])
    defined, undefined = nm_symbols(nm, archive)

    # 1. A driver or kit symbol left undefined is this recipe's failure, not the
    #    kernel's: every one of them is supposed to be in this archive.
    unresolved = sorted(name for name in undefined - defined if DRIVER_OWNED.search(name))
    if unresolved:
        die('driver symbols used but not built here: ' + ', '.join(unresolved))
    kit_unresolved = sorted(name for name in undefined - defined if KIT_OWNED.search(name))
    notes.append(f'{len(kit_unresolved)} kit symbols resolve from {KITS_ARCHIVE_NAME} '
                 f'(not built here)')

    # 2. The board package's weak stand-ins cover the counter surface; anything else
    #    with the native_ prefix is a link error waiting for the kernel build.
    stubs = stub_names(package)
    native_undefined = sorted(name for name in undefined - defined
                              if re.match(r'_native_(?:adreno|gpu)_', name))
    unowned = [name for name in native_undefined if name[1:] not in stubs]
    # The GOP hand-off the published device offers frames through. Nothing in this
    # repository greps for these two, so if the display component does not define
    # them the kernel link fails on exactly this pair; naming them here turns that
    # into a message instead of a linker error nobody localised.
    gop_undefined = sorted(name for name in undefined - defined
                           if name.startswith('_native_gop_handoff'))

    # 3. The published entry points must exist by name; the kits by prefix.
    absent = [name for name in REQUIRED_SYMBOLS
              if name not in defined and DRIVER_OWNED.search(name)]
    for name in REQUIRED_SYMBOLS:
        if name not in defined and KIT_OWNED.search(name):
            notes.append(f'{name} comes from {KITS_ARCHIVE_NAME}')
    if absent:
        die('required definitions absent: ' + ', '.join(absent))

    report = {
        'stage': 'G0',
        'tree': str(tree),
        'archive': str(archive),
        'archive_name': archive.name,
        'archive_note': 'build-xnu-4k-baseline.sh:647 hard-codes this path for '
                        '-Wl,-force_load and does not read CONFIG_XNU_NATIVE_ADRENO_ARCHIVE',
        'archive_bytes': archive.stat().st_size,
        'archive_sha256': sha256(archive),
        'compile_records': {'c': str(c_record.relative_to(tree)),
                            'cxx': str(cxx_record.relative_to(tree))},
        'sources': {source_key(path, tree): sha256(path) for path in sources},
        'objects': {obj.name: obj.stat().st_size for obj in objects},
        'source_count': len(sources),
        'required_symbols': list(REQUIRED_SYMBOLS),
        'defined_symbol_count': len(defined),
        'undefined_symbols': sorted(undefined - defined),
        'undefined_native_left_to_kernel_or_package': native_undefined,
        'undefined_native_without_a_board_stub': unowned,
        'undefined_gop_handoff': gop_undefined,
        'skipped_include_dirs': sorted(set(skipped_all)),
        'notes': notes,
        'board_package': str(package) if package is not None else None,
        'probe_compiled': False,
        'hardware_execution': False,
        'gpu_boot_argument_required': 'pipa-gpu=<mode>',
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')

    print(f'PASS: {len(objects)} A650 objects archived into {archive}')
    print(f'      {report["archive_bytes"]} bytes, sha256 {report["archive_sha256"]}')
    print(f'      report {out / "build-report.json"}')
    if unowned:
        print('WARN: these native_* symbols are neither in the archive nor in a board '
              'stub; the kernel link will fail unless package/LinkStubs.c or '
              'package/BootProgressStubs.c gains them: ' + ', '.join(unowned))
    if gop_undefined:
        print('WARN: the published device offers frames through the GOP hand-off and '
              'this archive references it, but it is undefined here: '
              + ', '.join(gop_undefined) + '. The display component '
              '(drivers/GopFramebuffer, built by build-gop-legacy-display-linux.py) is '
              'the only candidate provider; confirm with llvm-nm before the kernel '
              'link, because a force-loaded archive turns this into a hard error.')
    print('G0: linkable only. Nothing in this driver runs until the payload command '
          'line carries pipa-gpu=<mode>; the reference probe (package/GpuProbe.cpp:1921) '
          'returns on `if (gMode || !parsed || !mode)`, and this board has not ported '
          'that probe yet, so at G0 there is not even a caller.')
    print('NEXT GATE (G-1): read GPU SMMU ID2 bits[11:8] and require >= 36 before '
          'any mode above 1 -- NativeAdrenoSmmuMemory.cpp:13-18 decodes '
          '{32,36,40,42,44} else 48, and input_bits <= 32 makes every non-kernel '
          'client fail with e00002be.')
    print('REMINDER: this board is a plain SM8250, not SM8250-AC: there is no '
          'performance level 7, so any pipa-gpu-perf must be <= 6.')


if __name__ == '__main__':
    main()
