# Display research merge, 2026-11-19: why the panel keeps the boot logo

Status: research only. No code changed by this document except the read-only probe described in §7,
which is written but not yet reviewed/built/flashed. The tablet was in EDL (charging) while this was
written.

## 1. The observation that has to be explained

The last captured long boot (payload `9d9b615e`, 54 arguments, 971 bytes, no `sheng-dpu-scanout`,
`sheng-fb-gpu=0`, `sheng-data-reset=1`) ran 2990 s, kept a working userspace, and copied 220 full
frames into the firmware framebuffer; the panel never left the boot logo.

Window rows of that boot, read from the next boot's `[prev]` (tools/console-prev337.log:190-1424):

```
SWAP calls=220 accepted=1 reason=0 COPY calls=220 status=00000000 bytes=16384000 retired=220
[scanout prepare] prepare calls=442 failures=0 ...
C0 pid=56 calls=1016 fails=236
s6#1..s6#23 -> 0 / e00002d5, [5820]=[5824] 0..a
[display] layer0 src_stride=0x1900 src 1600x2560 format=0x42475241
[gpu clients] backboardd s0 4416M, mediaserverd s1, WidgetRenderer_Defa s2, SpringBoard s3, PhotosPosterProvide s4
[metal askers] backboardd:1 mediaserverd:1 WidgetRenderer_Defa:1 SpringBoard:1 PhotosPosterProvide:1
A files=6 wr=6 mount=5/0   [cryptex os] stage=5
backboardd p56 n49 r13 w36
```

So the compositor, the Metal clients, the IOMFB user client, the swap retirement and the CPU copy
all worked for 50 minutes. Whatever keeps the logo is downstream of memory.

## 2. Four research lines, 2026-11-19

1. kona DPU register map and flip semantics (mainline `dpu_6_0_sm8250.h`, `dpu_hw_ctl.c`,
   `dpu_hw_sspp.c`, `dpu_encoder_phys_vid.c`; same-SoC gts7xlwifi and alioth sources).
2. UEFI/PHYFB: which framebuffer the firmware reports and what DarwinBoot does with it
   (`uefi-dby_w09`, platform `dby_w09Pkg` and `Silicon/Silicium` copies).
3. backboardd/IOMFB failure shapes across UniXNU device records.
4. DBY-W09 on Linux (two independent ports) and mainline msm splash handling.

### 2.1 kona DPU (offsets relative to the MDP node at 0xae01000)

| item | value | consequence |
|---|---|---|
| CTL0..5 | 0x1000 + 0x200n | `kCtlFlush` must be **0x1018**, not 0x15018 |
| VIG0..3 | 0x4000, 0x6000, 0x8000, 0xa000 | `kVig={0x4000,0x6000}` is correct |
| SSPP SRC_ADDR / YSTRIDE0 | +0x14 / +0x24 | correct; YSTRIDE packs pitch0 in the low half |
| CTL FLUSH value | `0x20003` for VIG0+VIG1 | bit17 is the CTL commit bit |
| flush consumed | read FLUSH back, 0 = latched | Linux waits 50 ms |
| LM0..5 | 0x44000.. | not used by any code here yet |
| DSPP / INTF / PP | 0x54000 / 0x6a000.. / 0x70000.. | not used |

The board's vendor DT states the same numbers from 0xae00000 (`sde-off` 0x1000, `ctl-off` 0x2000,
`sspp-off` 0x5000/0x7000); the 0x1000 difference is the node offset, not a second layout. Reading the
DT offsets as node-relative was this session's one wrong turn and is recorded here so it is not
repeated.

`sde-secure-sid-mask=0x4000821` is the display SMMU stream-id classification (secure CB, vmid 0xa),
not a CPU MMIO firewall: the AP can write the DPU. The alioth hang attributed to the secure world
came from writing 0x15018, an address with no register (`UniXNU-alioth/package/DpuProbe.cpp:53-55`).

### 2.2 UEFI / PHYFB

* `MemoryMapLib.c:38`: `{"Display Reserved", 0x9C000000, 0x02400000, ..., Reserv, WRITE_THROUGH_XN}`.
* The platform `SimpleFbDxe` keeps a pool-allocated logical canvas and publishes GOP over it; the
  physical 0x9C000000 is handed to the OS through the private `DBY_PHYFB` protocol (PhysBase,
  PanelW/H 1600x2560, PitchBytes 6400, Rotation 90, LogicalW/H).
* `DarwinBoot.c:652-699`: read GOP video, look up PHYFB (50 x 20 ms), then override
  `Video[0]=PhysBase, Video[2]=Pitch, Video[3..4]=PanelW/H`. On a miss it leaves the GOP canvas and
  prints `no PHYFB protocol; video stays on GOP logical canvas`.
* The firmware never programs MDSS/DSI: `USE_CUSTOM_DISPLAY_DRIVER = 0` in `dby_w09.dsc:36`, and the
  vendor `DisplayDxe` is not in the FV. The panel pipeline is whatever ABL left running.

Consequence for this port: the PHYFB-hit case is provable from the kernel rows — `[swapok] ... base=9c000000`
plus `NativeLegacyDisplay.cpp:746-749` requiring `map.phys == video.v_baseAddr`. Both captured boots
pass that, so the copy wrote the real framebuffer, not the canvas. The PHYFB-miss case is the *warm
reboot* failure already in porting.toml:146/:196 (no driver bind, thousands of `verify_swap failed`),
not this boot.

### 2.3 backboardd

* Round 336 and round 1443 both show backboardd running, IOMFB client C0, GPU clients and retired
  swaps. `backboardd=0` in earlier notes was counted in the live serial section, where window rows
  never appear (porting.toml:341/:344 records the same mistake twice already).
* `IOMFB: default_fb_surface: No Default FB Surface found` is non-fatal and appears on working ports
  (opi5plus, e5); selector 3 returns e00002bc and is not a prerequisite.
* Missing Metal does not stop backboardd; it degrades. Here the accelerator is in the image and
  working (5 clients, `[gpu objects]`, zero fence timeouts), so the clang-24 ICE on
  `NativeAdrenoService.cpp:95` is a user-space service gap, not a kernel accelerator gap.

### 2.4 DBY-W09 on Linux

* Panel is a Novatek NT36523 TDDI, 1600x2560, two DSI links, C-PHY, 3 lanes, pixel clock 584849 kHz;
  both ports program **video mode** (`MIPI_DSI_MODE_VIDEO`). A video-mode panel holds its last frame
  when the stream stops and does not need TE.
* `cont_splash_region@9c000000` (0x2300000 in the stock DTB, 0x02400000 in MemoryMapLib) is the
  bootloader splash and the region Linux hands to `simple-framebuffer`; Linux then reprograms
  VIG/LM/CTL/DSI to its own buffers and frees the region (`_sde_kms_free_splash_region`).
* No mainline splash-handoff code exists; both ports re-initialise the display controller.

## 3. Ruled out

| claim | why it is out |
|---|---|
| userspace stops before backboardd | round 1443 window: backboardd p56, 220 retired swaps, 5 GPU clients |
| the panel does not scan 0x9c000000 | 2026-11-16 `copy=2` boot reached the desktop; porting.toml:342 same shape |
| XNU copies into the GOP pool canvas | `[swapok] base=9c000000` and the mapping check; the canvas case is the warm-reboot boot with no copies at all |
| command-mode panel needs TE | two Linux ports say video mode |
| cache not cleaned | `kIOMapInhibitCache`; `dst` readback changes |
| secure world blocks DPU stores | SID classification only; the hang came from a register-less address |
| `default_fb_surface` blocks the UI | non-fatal on opi5plus/e5 |
| missing Metal keeps backboardd away | contradicted by opi5plus and by this boot's clients |
| stride/geometry wrong | 6400 == PitchBytes == width*4; `[display]` row agrees |
| the touch reset kills the panel | `dbyTouchResetAllowed()` (DbyTouch.cpp:2239-2269) leaves gpio98 alone unless `dby-touch-reset=1`; this payload does not pass it |

## 4. What is still open

1. **The DPU stopped fetching** (pipe disabled, or pointed elsewhere).
2. **The DSI/INTF stopped consuming** (video-mode panel holds the last frame).
3. **The ABL scans a different frame**: the 36 MiB region holds two 16.4 MiB frames, and nothing
   recorded which base ABL programmed.

All three are decided by reading the live pipe: `SRC0_ADDR` of VIG0/VIG1 and CTL0's
PIPE/FETCH_PIPE/INTF active words, behind the dispcc/GDSCR power gate.

## 5. The probe (written, under review, not built)

`package/LegacyDisplayProbe.cpp:496-504` adds `dby_dpu_probe()`, opt-in `dby-dpu=1`, one shot,
read-only; the snapshot it calls is `:121-383`. It first reads dispcc `mdss_ahb_clk`/`mdss_mdp_clk` CBCRs and `mdss_core_gdsc` GDSCR; if
either gate says off it does not map 0xae01000 at all, because the 2026-11-15 read of that block
aborted the kernel. Otherwise it prints four `[dby-dpu]` rows: VIG0/VIG1 size/xy/src0/src1/ystride/
format/unpack/multirect, CTL0/CTL1 flush/top/intf/fetch/pipe/layer, and the console base/geometry.

`package/BootProgress.c:108-112,1304-1308` calls it once from the monitor thread under the same
log-backed conditions as the two GPU probes. `board.toml` gains `dby-dpu=1` (971 + 10 = 981 of 1000)
and restores `sheng-fb-gpu=0`, which is what both flashed payloads carry in their ESP.

Judgement table:

| reading | verdict |
|---|---|
| gdscr bit31 clear or CBCR bit1 set | block gated; probe stops before the MDP |
| vig.src0 == 9c000000 and ctl.pipe/fetch != 0 | DPU still fetches -> the fault is DSI/INTF/panel |
| vig.src0 == 0 or ctl.pipe/fetch == 0 | DPU stopped -> power/clock/INTF takeover |
| vig.src0 != 9c000000 | the controller scans another buffer (frame 2 of the splash, or a stale pipe) |
| vig.mr != 0 | firmware left the pipe in multirect; a flip must rewrite RECT_1 |
| size/xy != 1600x2560 | geometry inherited from the firmware; a flip must set SRC_SIZE/SRC_XY/OUT_* |

## 5a. The probe ran (payload d53f28b1, kernel 1268c4b2)

Six rows, one boot:

```
[dby-dpu] gdscr=f822f000 ahb=00000001 mdp=00000221 clock=1 power=1
[dby-dpu] vig0 size=0a000320 xy=00000000 src0=9c000000 src1=00000000 ystride=00001900 fmt=000237ff unpack=03020001 mr=00000000
[dby-dpu] vig1 size=0a000320 xy=00000320 src0=9c000000 src1=00000000 ystride=00001900 fmt=000237ff unpack=03020001 mr=00000000
[dby-dpu] ctl0 flush=00000000 top=00000000 intf=00000006 fetch=00000000 pipe=00000000 layer=00000000
[dby-dpu] ctl1 flush=00000000 top=00001f00 intf=00000000 fetch=00000000 pipe=00000000 layer=00000000
[dby-dpu] console=0 base=9c000000 w=1600 h=2560 row=6400 depth=32
```

What it settles: the controller is powered and clocked; both VIG pipes fetch 0x9c000000 with the
6400-byte row the copy uses; the pair is an 800+800 split of one 1600x2560 buffer (VIG1 src_x=0x320),
not multirect; CTL0 marks INTF1/INTF2 (DSI0/DSI1) active; the console base agrees. The "controller
stopped" and "it scans another frame" readings are therefore out.

What is left is that SRC0_ADDR is an IOVA. If the display apps_smmu context bank translates
0x9c000000 to another physical page, the panel keeps the boot loader frame while the copy writes
physics. The =1 round only ever programmed the unsecure stream bank (0x820) and only after finding it
bypassed; the secure stream (0x821, vmid 0xa) is untouched by this port. The next probe reads both
banks (read-only) and prints SCTLR/TTBR0/TTBR1/TCR/FSR, which decides "translated" versus "bypass";
a bypass verdict moves the whole question to the DSI/panel side.

## 5b. The SMMU read (payload 312675b0, kernel 4f061913)

```
[dby-dpu] smmu idr0=4c017e57 idr1=6000004a groups=87 banks=74 pages=128
[dby-dpu] smmu stream=820 smr=84020820 s2cr=00000004 type=0 bank=4 cbar=0001f000(stage1) sctlr=000000e0 ttbr0=00000000 ttbr1=00000000 tcr=00000000 tcr2=40014065 mair=00000000 fsr=00000400
[dby-dpu] smmu stream=821 not mapped
```

Decoded: stream 0x820 matches its SMR entry and routes to context bank 4 with S2CR type 0; the bank's
CBAR says it is set up for stage-1, but **SCTLR.M is 0**, so stage-1 translation is off and TTBR0/TTBR1/
TCR/MAIR are all zero. FSR has only the FORMAT field set (not a fault). Stream 0x821 (the secure one,
vmid 0xa) is not in the SMR table at all, so the controller does not fetch through it.

Verdict: the VIG's `src0=9c000000` is used as a **physical** address. The DPU reads exactly the memory
the CPU copy writes. The SMMU hypothesis is out, and with it every remaining "wrong address" reading.
What is left is the DSI/panel side: the branches that turn the MDP output into a DSI stream, or the
panel's own state.

Next read: the DSI clock branches in the same dispcc block (mainline dispcc-sm8250.c: pclk0 0x2004,
pclk1 0x2008, byte0 0x2028, byte0_intf 0x202c, byte1 0x2030, byte1_intf 0x2034, esc0 0x2038,
esc1 0x203c), which the MDP gates above say nothing about. A gated branch here means the panel gets no
stream however correct the fetch is; all branches on means the DSI controller is running and the panel
itself (ESD/PSR/init) is what has to be explained.

## 5c. DSI clocks and INTF state (payloads 8cd2d757 and c0ffd172)

```
[dby-dpu] dsi pclk0=00000001 pclk1=00000001 byte0=00000001 byte0i=00000001 byte1=00000001 byte1i=00000001 esc0=00000001 esc1=00000001
[dby-dpu] intf dsi0_en=00000001 st=00000001 hsync=035e0014 cmd=00000000 dsi1_en=00000000 st=00000001 hsync=035e0014 cmd=00000000
```

All eight DSI branches in dispcc read bit0 set and bit1 clear: the pixel, PHY-byte, INTF-byte and
escape clocks of both links are running. The INTF row is the DPU 6.0 form - `INTF_STATUS` bit0 (the
running bit) is 1 on both interfaces, `INTF_HSYNC_CTL` is non-zero (video timing was programmed) and
the command-mode trigger is 0. So the DPU fetches the copy target with the right stride, the CTLs bind
INTF1/INTF2, and both INTFs are generating video timing.

Everything from the fetch to the INTF output is therefore healthy. The remaining link is the DSI
host/PHY and the panel itself; the next read is the 7nm PHY/PLL status for both links (CMN_PHY_STATUS
0x140 bit0 REFGEN, CMN_CTRL_0 0x24 bit5 bias, CMN_CLK_CFG1 0x14 source mux, PLL_COMMON_STATUS_ONE
0x1b0 bit0 lock).

## 5d. Timeline samples and the boot page (payload 3093eb03, kernel c438ec7e)

Four samples, t=4/5/10/20 s, all identical:

```
[dby-dpu] t=4  at=2  gdscr=f822f000 intf0_st=00000001 ctl0_intf=00000006 dsi0_ctrl=00000077 dsi0_lock=00000051
[dby-dpu] t=5  at=5  same
[dby-dpu] t=10 at=10 same
[dby-dpu] t=20 at=20 same
```

No transient gate: from the first sample (t=4, the earliest the log gate allows) the domain is powered, the
INTF timing engine runs, CTL0 binds INTF1/2, DSI0 is enabled and its PLL is locked. The domain never drops
inside the measured window.

The boot page is exonerated by two rows that had not been read together before:

```
OBS pulse-worker=2 anchor=0 bind=3 result=0 depth=32 base=fffffff14cdf0000 (0=bound 1=invalid 2=UI owned)
GOP actual mapping virtual=fffffff29899b000 physical=9c000000 bytes=16384000
```

`boot_progress_bind` passed every argument check (result 0), and `bp_clear` (package/BootProgressDisplay.h:13)
writes the WHOLE frame to zero before the first text row. So when the page binds, the firmware's Apple logo
is erased from memory and replaced by black plus text rows. A panel that still shows the Apple logo therefore
froze BEFORE the page's first paint, i.e. at the UEFI/kernel handoff (t ~ 0.1-0.2 s), not because the page
failed. The 0-copy boots look like a pure Apple logo for the same reason: the memory already holds the black
page, and the panel shows the frozen camera of the frame it latched.

That is also why the whole register chain reads healthy in a frozen boot: the freeze is upstream of it in time,
not broken in configuration.

## 6. Next, in order

1. Review the probe (PASS/FAIL), build kernel, repack payload, flash `preload`.
2. Read the `[dby-dpu]` rows on the first boot.
3. Only then: either the DSI/INTF track (if the DPU is healthy) or the DPU-flip track
   (`kCtlFlush=0x1018`, flush `0x20003`, REC T_1/geometry) if the pipe is stale.

This document owes porting.toml a display-area evidence entry (the 220-copy, backboardd-alive boot
and the four research conclusions) with the next commit.
