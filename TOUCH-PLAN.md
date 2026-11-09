# Touch bring-up: remaining work

Status on 2026-11-01. Everything in §1 was measured on the device; §2 is what is left; §3 is the
order to do it in. Each task names its own pass criterion so a step is either done or not.

## 1. What is already proven

| Step | Evidence |
| --- | --- |
| GCC vote + SE root clock | `clk=1`, `rok=1`, vote/branch wiring test changed the CBCR as expected |
| GENI/SPI engine | `hw0=0x201029e4` (32-bit words, 16-word FIFO), `se=FIFO-USABLE` |
| Loopback | `lp=a55ac33c`: the four bytes sent came back unchanged |
| Pads | `GPIO_CFG=0x84` after the mux (function 1 = qup14, drive 2) |
| Chip identity | N9Z: `0A 00 01 23 65 03`, vendor trim table row 20 |
| Whole frame | 8208 bytes read at 9.6 MHz (128-byte pieces, CS held between them) |
| Frame is live | two frames 50 ms apart differ (`ch=3` idle, `ch=6..7` while touched) |
| Frame layout, coarse | first 4 KiB sparse (report area), 4096..8207 nearly full (capacitance image) |

Measured offsets inside the frame (frame byte 0 = received byte 2):

| Offset | Behaviour |
| --- | --- |
| 10..13 | constant `14 04 06 18` across boots |
| 65 | toggles `0x04`/`0x14` between frames - a counter or buffer tag |
| 1019 | increments by one between frames - the frame counter |
| 4096..8207 | 4096 bytes, ~100% non-fill: capacitive image, 2048 int16 nodes |

## 2. Decisions taken from the two research reports

`work/tracks/route2/REPORT.md` and `work/tracks/official-touch/REPORT.md` agree on these:

1. **The six-byte contact record is not this board's protocol.** The vendor stack for this board
   (Huawei THP + N9Z, `support_vendor_ic_type=5`) copies the whole frame to user space and lets the
   HAL parse it; there is no in-kernel record parser to copy.
2. **Contacts come from the int16 matrix, not from records.** The official component computes
   centroids from a 40x60 int16 matrix (`NvtTouchCore.c`), and its output domain 30480x20320 is the
   HID publish domain. Reuse `NvtTouchCore` for matrix to contacts; do not port the whole
   `ShengNovatekTouch` (its identify rejects our chip: `id[3]==0x32` vs our `0x23`).
3. **Geometry comes from the device tree, not the chip.** `huawei_tp.dtsi` gives finger 1600x2560
   with magnification 8, i.e. 12800x20480, and pen 12800x20480. The `0x2FE78` read is wrong: that
   address is the THP command mailbox, and reading it moves the chip's page pointer.
4. **The frame reader stays ours.** The official one is page-explicit and reads a different length;
   ours matches the vendor's own `tx[0]=0x20` transfer byte for byte.

## 3. Tasks, in order

### T1 - frame to a file (unblocks everything else)

Every remaining question is "what is at offset N", and answering it by flashing costs ~15 minutes
per question. Write one frame to the UFS log window and read it back over EDL.

Design constraints, all of which the first attempt got wrong:

- `native_ufs_log_write(offset, blocks, data)` takes an **offset inside the window**, not an LBA.
- It copies `blocks * 4096` bytes: a 8208-byte frame needs a **zero-padded 3-block buffer**.
- The window can be opened once. Touching it from the touch driver must not break the log writer:
  the open path has to hand back the geometry instead of failing when the log writer already owns it.
- The dump runs **after** the two-frame comparison, never between the two frames.
- The destination must sit outside the log writer's own slots/messages for the first minutes.

Pass: the row shows `dp=1@<offset>`, EDL reads those 3 blocks back, and the first two bytes are the
same `FF FF` the driver reports; the log writer keeps working (`sheng-log=1` still archives).
Rollback: drop `dby-touch-dump=1`; nothing else changes.

### T2 - matrix base and geometry (host side, no flashing)

With a frame in hand, find the int16 matrix: slide a window over frame offsets, score each candidate
by `x_num * y_num * 2 == length` and by neighbouring-node smoothness, and cross-check the aspect
against 1600x2560 (ratio 0.625).

Pass: exactly one candidate offset survives both tests, and its node count is consistent with the
measured 4096-byte full region (2048 nodes) or explains a larger region.

### T3 - contacts from the matrix

Feed the candidate matrix to the official `NvtTouchCore` (it has a host-side test), scaled to the
node grid found in T2. Compare idle and pressed frames: idle must give zero contacts, a pressed frame
one contact whose centroid moves with the finger.

Pass: centroid tracks the finger across three positions, and the reported axis range matches
12800x20480 within a few percent.

### T4 - publish the snapshot

Map contacts to `native_touch_seq/count/xs/ys/ids` (the contract in `DbyTouchProtocol.h`), with the
firmware-side values unchanged while no finger is down.

Pass: the diagnostics row shows a stable count and coordinates that follow the finger.

### T5 - HID

Wire `ShengHidInput` per `https://unixnu.com/zh/porting/input` (`native_touch_set_sink`,
`sheng-touch-hid=1`), which is a separate track from T1-T4.

Pass: a touch produces a HID report (key-reports counter moves).

### T6 - swipe-up gesture

Only after T4. Up-swipe = y decreasing monotonically across at least N reports by more than a
threshold, with x within a band. Thresholds come from T3's measured resolution, not from guesses.

Pass: a deliberate up-swipe is recognised once and a slow drag is not.

## 4. Rules for this track

- Every code change is reviewed by an independent agent before it is built and flashed; the review
  request carries the measured evidence, the code as it stands, and the questions to answer.
- A measurement that cannot distinguish "no data" from "wrong offset" is not a measurement: every
  field published on the diagnostics row must have a defined meaning for its failure case.
- Hardware values are read, never guessed; the source goes next to the value.
