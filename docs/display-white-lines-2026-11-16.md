# White horizontal lines: cause and fix, 2026-11-16

The panel showed white horizontal lines instead of the lock screen for most of 2026-11-15, while the
display counters said full frames were being copied (`SWAP calls=44 accepted=1 reason=0`, `COPY calls=44
status=00000000 bytes=16384000 retired=44`). The lines were not a copy problem and not a pixel-format
problem: they were the panel being reset and never re-initialised, and the reset came from the touch
driver.

This file records the bisect that found it, the fix, and the checks that decide whether a later change
brought it back. The general method is
`DBY-W09-触摸包-检测与修复指南-2026-11-03.md` §1 (symptom taxonomy) and §5 (the touch stage ladder); this
file is that method applied to this board and its outcome.

## 1. What the symptom was, in the guide's terms

The guide splits "花屏" into eleven faults with different causes. This one is its S1 with its ranked cause
2: *the panel was reset and never initialised*. The symptom that separates it from a stride fault (S2) is
that the picture is not sheared or displaced - there is no picture at all, just repeating white lines, and
it appears **after** the display backend has already reached its last stage (`DISPLAY stage=10`).

## 2. The bisect

One variable per boot, payload rebuilt each time, no kernel change: only `board.toml [payload] kernel_args`
moved.

| boot | argument | result |
| --- | --- | --- |
| 1 | `dby-touch=8` (full ladder: clocks, pins, reset pulse, firmware upload, frame reads, HID) | white horizontal lines once the OS took the panel |
| 2 | `dby-touch=1` (read-only pin census, returns before any mapping) | **no white lines**; the iPadOS home screen came up |

Payload 2 is `959c642d4eb033be4fa9333664a60fe44b9f1b27e2534b100043ac057008fb3c`, and its console carries
`DbyTouch: stage=1 ic=novatek transport=spi compatible=huawei,thp` followed by the pin census rows and no
`DbyTouch(s5)` reads. The display rows of that boot show the copy landing (`DbyDisplay: copy=2 fmt=42475241
stride=6400 src=ff000000 ... dst=ff000000 ...`), i.e. the path was working the whole time - what changed
was the panel's state, not the pipeline.

## 3. The mechanism

`dby-touch=8` walks the whole ladder. Its stage 6 is the reset pulse: `dbyTouchChipEnable()` drives GPIO 98
low for a few milliseconds and back high (`package/DbyTouch/DbyTouch.cpp`, `dbyTouchResetDrive` /
`dbyTouchResetRelease`), and stage 2 has already muxed that pin (`applyPinConfig`'s plan). On this panel
the touch controller and the display sit in one TDDI die and the reset line is shared: driving it resets
the panel's own state as well. Nothing in the kernel re-initialises DSI afterwards, so the panel stays in
its uninitialised state and paints white lines regardless of what the framebuffer holds - which is why the
counters looked healthy and why the first copy's source pixels (`0xff000000`, opaque black) were copied
correctly while the panel showed nothing of it.

Two other explanations were ruled out earlier and stay ruled out:

* **Pixel format.** `nativeBgra` accepts `0x42475241` ("ARGB", no planes) and `0x26424741` ("&BGA", at most
  one plane); the garbled boot reported the accepted one, and the copy moves 32-bit words unchanged. An
  R/B swap shifts hues, it cannot turn a picture white.
* **The display pipes pointing at the wrong memory.** With `sheng-dpu-scanout` off nothing in this driver
  writes the DPU, so the pipes still fetch what the firmware left, and the firmware fetched the GOP
  framebuffer. Reading the pipes directly is not possible without their clocks: it aborts the kernel
  (recorded in `porting.toml`'s display area).

## 4. The fix

Short term, and what the tablet is running: **do not run the reset pulse**. `dby-touch=1` keeps the panel
alive and costs touch, which does not work yet anyway (the chip announces no new frames - see the input
area's evidence row for the THP header reading).

The durable fix is the guide's §8 patch 2, applied to this driver:

1. `applyPinConfig`'s plan keeps only the IRQ pin; GPIO 98 is not muxed at stage 2.
2. `dbyTouchChipEnable()` starts with a gate: without `dby-touch-reset=1` it returns
   `DBY_TOUCH_ERR_IO` and logs that the pad was left alone.
3. `dbyTouchResetRelease()` takes the same gate, so a boot that never drove the pin never drives it back.

With that, a boot that wants touch says so (`dby-touch=8 dby-touch-reset=1`) and a boot that does not
never touches the panel's reset line. If the reset pulse turns out to be required for the N9Z - it is a
flashless part, so the host may have to reset it before the firmware upload - the next question is not
whether to drive GPIO 98 but *when*: after the OS owns the panel, a reset is invisible to the user only if
the panel is re-initialised afterwards, and nothing here re-initialises DSI.

## 5. How to check that a later change did not bring it back

Run these in order; each is cheap and each has a photo or a console row behind it.

1. Boot with the touch argument under test and watch the panel from the firmware logo to the home screen.
   White lines that start when the OS takes the panel = the fault is back; the panel was reset.
2. `DbyTouch: stage=` tells you which rungs ran. A boot at stage 1 must never show the fault; a boot at
   stage 6 or above may.
3. The console's copy rows (`DbyDisplay: copy=N ... dst=...`) say whether the framebuffer is being written;
   if they show the copy landing while the panel shows lines, the panel - not the copy - is the fault.
4. `DISPLAY stage=10` with a picture on the panel is the pass condition; a panel showing lines at stage 10
   is this fault or a scanout fault, and the guide's S2/S5 branches separate them.

## 6. What is still open

* Which rung between 2 and 8 breaks the display is still being bisected (`dby-touch=2` was flashed after
  the stage-1 boot that worked). The reset pulse is the prime suspect and the only rung that changes the
  panel's own state.
* Touch itself: the N9Z announces one frame and then nothing (`thp=2049 new=1 same=2048`), so the driver
  never has a report to publish. That is recorded in the input area and is a separate problem from this
  one.
* The AMFI `No PMGR?` panic is intermittent (one boot in three), so it is not yet a closed item either.
