# Problems and fixes: the 2026-11-15 hardware session

This file keeps the session's order of events. `porting.toml` keeps what was measured and which row measured
it; this file keeps the sequence, because the sequence is what says which row answered which question.

The session started with an image that had never run on this tablet and ended with a boot that kept
SpringBoard and backboardd alive for 23 minutes and 246 processes, a panel that presented iPadOS frames, and
three subsystems (storage, touch, display) each narrowed to one measured question. It cost a flash per
question, and two of the dead ends looked exactly like a kernel fault.

## The eleven, in the order they appeared

| # | symptom | what it was | closed by |
|---|---|---|---|
| 1 | a console that enumerated and read zero bytes | the reader never raised DTR | `22a7176` |
| 2 | the log replay printed nothing | it opened a window the writer already held, and would have read this boot's own header | `f7cf971` |
| 3 | the panic text existed only on the panel | nothing wrote it to the log window | `65548fc` |
| 4 | `bsd_init` waited in `IOFindBSDRoot`, two boots in a row | the assembled tree carried the link-only trust stub, policy count 0 | `3794464`, `29e673e` |
| 5 | every exec logged `vm_shared_region_start_address() failed` | the gate reads `gpu-addon`, the line said `sheng-gpu-addon` | `29d73bc` |
| 6 | panic `AMFI: No PMGR?` 46 s in, every boot | the rename to `pmgr=2` stopped the stand-in, which reads `sheng-pmgr` | `be6ce91` |
| 7 | APFS `disk1: device is not ready`, panel `D st=7` | the store still held the previous seed's Data overlay | `c832300` |
| 8 | the pre-flash gate was red on its own bookkeeping | pinned arguments the line had renamed or dropped | `538cf71`, then the `gpu-addon` row below |
| 9 | the gate could not run on the Windows host at all | `read_text()` used the locale code page, and `mcopy` is a build-host package | this push |
| 10 | touch: a read that looked stuck, then looked healthy, and a frame that never carries a contact | five separate answers, none of them the transport | `f9a60d2` .. `4922886` |
| 11 | white horizontal lines instead of the lock screen | not yet known: the surface or the copy | `2c8a354`, `2d5b334`, `e699e01` |

## 1. A live console that read zero bytes

`1209:316d` enumerated 12 s after the reboot and every capture came back empty, which reads as "the kernel
stopped printing". It was the reader. The gadget's `pump()` refuses to queue an IN transfer while `d->dtr` is
0, and `dtr` is set only by the host's SET_CONTROL_LINE_STATE; the reader never raised it. Two smaller facts
made it worse: this device refuses `SetCommState`, so .NET's `SerialPort` cannot open it at all, and Windows
keeps the friendly name the device started with (COM6) while the device key's PortName and SERIALCOMM both say
COM20.

The fix is to open the PortName directly, raise DTR and RTS with `EscapeCommFunction`, and wait for the port
instead of failing at once. `docs/flash-runbook-2026-11-15.md` states the requirement (`22a7176`); the readers
themselves live outside this repository, which is why the record points at that section rather than at a file.

What this re-read: every zero-byte capture before that date measured the reader, not the kernel.

## 2. The replay that printed nothing

`sheng-log-replay=1` was supposed to print the previous run's log. It printed nothing because it waited six
seconds and then called `native_ufs_log_open()`, which refuses a window that is already open - and the writer
has held it since its first round. Even a successful open would have read this boot's own header and message
area, because the writer rewrites both immediately and then every `LogPeriodMs`.

`f7cf971` moved the snapshot into the writer's start-up, right after the window opens and before the first
write: the previous header, its message area, and its newest text slot. The slot is the part that matters -
`compose()` is where `[panic]` and the panic text land, and the message area never carries it. The replay
thread now only sleeps for the console and prints the snapshot with a `[prev]` tag; it opens nothing and
writes nothing.

## 3. A panic text that only the panel could see

The panic message reached the screen and nowhere else, and the screen is the one place this device cannot be
read: the console stops answering the moment the panic parks the kernel, the screen overlaps the text with the
diagnostic page, and EDL does not answer at all.

`65548fc` stores the text in the last block of the log window - one block, outside the slot ring, so writing it
cannot make the writer's own snapshot inconsistent - and the replay prints it as
`[prev] --- panic text ---`. The panic screen resolves `boot_log_panic_text` by symbol name from the kernel it
is patched into (`-DPANIC_FLUSH`), so a kernel without the export is left alone.

This is the change that made problem 6 readable.

## 4. Two boots that waited in `IOFindBSDRoot`

The symptom was a boot with no kprintf output after about 46 s and PC samples in `_machine_idle`, which reads
like a kernel hang. It was a build input: the assembled tree's `libVerifiedRootProbe.a` was the link-only stub
(`verified_root_policy_count` 0, all-zero anchor), so every root window was refused and `bsd_init` waited for a
device that was never going to appear.

`3794464` made the refusal say which check failed - stage, policy count, window bytes, and every policy's image
size - instead of one summary line. The real two-policy bundle (manifests 47,481,664 and 25,805,344 bytes) was
still on disk under the old board slug; swapping it in grew the kernel to 126 MB and got the first boot to
launchd. `29e673e` recorded it and pointed README's carried-input row at the copy that is the real one, because
a stub and a real bundle are both "the trust archive" until you read `archive.json`.

## 5. Every exec failing in `vm_shared_region_start_address()`

The gate is `native_gpu_addon_prepare()`, which reads `gpu-addon` (`kern_exec.c:7661`). The board had passed
`sheng-gpu-addon=1` since 2026-11-07, and upstream `47ec6a5` dropped the `sheng-` prefix from every boot
argument, so the function returned on its first line: no addon install, none of the five union mounts, no
cryptex OS view, no gestalt cache drop, no LaunchServices store drop. The boot showed exactly that - `[cryptex
os] stage=0`, every exec logging the shared-region failure, and no process after launchd.

`29d73bc` renamed the argument to `gpu-addon=1` and dropped the temporary `sheng-data-reset=1`. `[cryptex os]
stage=5 status=0` afterwards. The addon blob itself is still missing - its producer needs `xcrun` and no copy
survives - but the cryptex OS view needs the gate, not the blob.

## 6. `AMFI: No PMGR?`, 46 s in, on every boot

The first panic the new log path made readable. `sheng_pmgr_run()` opens with
`PE_parse_boot_argn("sheng-pmgr", ...)` and returns when it is absent (`ShengPlatformPower.cpp:71`). The
2026-11-14 rename to `pmgr=2` - needed because `scripts/unixnu/payload.py` refuses a native-UFS-root payload
unless exactly one `pmgr=2` is present - therefore stopped the stand-in from running at all, and the comment
that recorded the rename claimed the argument had no reader. It has one.

`be6ce91` passes both: `pmgr=2` for the payload gate, `sheng-pmgr=2` for the component. 53 arguments, 940 of
the 1000-byte gate. The boot that followed ran 23 minutes and reached 246 processes. The proper fix is the
component reading `pmgr` itself, with the pin that implies.

## 7. `disk1: device is not ready`, panel `D st=7`

`st=7` is `persist::Open::Kept`: the UFS store still held the previous seed's Data, so the overlay stayed
closed and every APFS write was answered `device is not ready`.

`c832300` added the documented install-and-boot half - `sheng-data-reset=1`, which makes
`NativeRamBlock.cpp:82` discard the old pages instead of applying them - for exactly one boot. The row went to
`D st=3` (Fresh) and then `st=2` (Resumed). The argument came out again in `29d73bc` once the store matched the
seed; a permanent reset would throw the overlay away on every boot.

## 8. A gate that was red for its own reasons

The pre-flash gate read red before it could say anything about the artifact: `CRITICAL_ARGS` still required
`sheng-pmgr=2` and `sheng-data-reset=1`, and the accepted-argument list still carried those two plus
`sheng-disable-jobs`. None of the three can be on the line today, and none of the three was a regression.

`538cf71` moved the accepted list the way `-no_slto_panic` moved on 2026-11-09, and said why in the baseline's
note. It could not fix the fourth name because the rename came later that day: `29d73bc` renamed
`sheng-gpu-addon=1` to `gpu-addon=1` at 12:31, and the gate kept pinning the old spelling. The `gpu-addon` row
is now pinned under the name the kernel reads, and the baseline note records that the addon itself is still
absent - which is what the carrier row reports, not the argument row.

## 9. A gate that could not run on the Windows host

Two independent reasons, both found by running it there for the first time:

* `read_text()` without an encoding answers the locale code page, cp936 here, and the baseline carries arrows
  and em dashes, so the gate died with `UnicodeDecodeError` before its first check. Every read in
  `check-repository.py` passes `encoding="utf-8"`; the gate's did not. Three reads and the baseline write now
  do.
* the ADT extraction shells out to `mcopy`, which is a build-host package. Without it the gate raised
  `FileNotFoundError` and printed a traceback, which hides which check could not run; it now reports the FAIL
  row with `mtools` as the reason, and still compares bytes when the tool is there.

Same tree, both hosts: 92 checks / 88 pass / 4 fail on the Windows host, 94 / 90 / 4 on the build host - the two
extra rows are the ADT comparison that needs `mcopy`. The four that fail are the carrier rows (trust cache,
addon, seed, ADT), which describe the accepted 2026-11-08 boot and are expected to differ on a tree that ships
a new seed and trust bundle. Re-recording them needs a boot that works.

## 10. Touch: three readings of the same silence

The first reading was wrong. `s5=0xFFFFFFFE` on the panel was read as a stalled transfer; `f9a60d2` printed the
read ladder instead and showed all 65 chunks of the 8208-byte frame completing and the thread starting the next
frame immediately. `0xFFFFFFFE` means "started, not finished" for a thread that reads frames forever.

The second reading was a measurement. `a1c644b` fingerprinted the scanned payload every 64 publishes, and the
rows were byte-identical with a finger held on the panel and with it idle (`nz=4677 sum=688f4116
w1000=14000000`): the `0x20` request returns one static page, not the report page.

The third reading came from the vendor source (`df6bcba`): `thp_irq_thread` is the only caller of
`ops->get_frame`, and `novatek_thp.c`'s `get_frame` is a single 8208-byte full-duplex transfer whose first byte
is `0x20`. This driver polled the same command on a 25 ms timer with no interrupt, in 65 chunked transfers.
Three changes followed, and all three were negative: `eebd1d9` gated the loop on pin 39 (`skip=0` - the line is
low from boot and stays low), `c62af0a` cut the chunking gaps from 65 to 8 per frame (fingerprint unchanged),
and `47b3067` made the firmware picker say why it kept the default (the first bytes its oeminfo scan reads are
all `0xff`, i.e. an erased partition; `W2599Z2700` is what the accepted build used too).

So the chip holds its interrupt low and answers every read with the same page no matter how the transaction is
cut. What is left is the vendor's enable/resume sequence and its host commands, compared line by line against
this driver - not the transport, the HID path or the pin/power bring-up, all of which are measured healthy.

## 11. White horizontal lines, and a reading not yet taken

After the OS took the panel the lock screen became white horizontal lines, while the counters said 44 full
frames were copied (`SWAP calls=44 accepted=1 reason=0`, `COPY calls=44 status=00000000 bytes=16384000
retired=44`). Two halves have to be told apart: a surface the renderer never drew, or a copy that lands
elsewhere.

`2c8a354` added the row that separates them - surface format, source stride, and three pixels from the
compositor's surface and from the scanout at the same offsets. `2d5b334` fixed its cadence: this boot takes a
copy about every 30 s, so one row in 32 printed nothing inside the console window; the first eight copies are
printed, then one in 64.

`e699e01` ruled out a pixel-format mismatch by reading the path rather than by another boot.
`native_legacy_copy_layout` only validates and copies nothing; `native_legacy_copy_pixels` moves 32-bit words
unchanged; `nativeBgra` accepts exactly `0x42475241` ("ARGB", no planes) and `0x26424741` ("&BGA", at most one
plane); the garbled boot's swap reported `0x42475241`; and layer0 geometry is equal rather than tolerated
(1600 x 2560, stride 0x1900, allocation 0xfa0000 = 1600 x 2560 x 4). An R/B swap shifts hues and cannot turn a
picture white.

The reading itself is still missing. The build that prints the row was flashed and the tablet came back to
fastboot instead of booting, so the row has not been read. That is where the next session starts.

## Still open at the end of the session

* the `copy=` row of problem 11, from a boot that stays up;
* the GPU addon blob (`payload.json` `gpu_addon_sha256` is null; its producer needs `xcrun`);
* audio: the four TFA9874 amplifiers enumerate and answer, but no sound has been played;
* touch: the vendor enable/resume and host-command comparison;
* the four carrier rows of the gate, re-recorded after a boot that works;
* Bluetooth serial is still disabled in `board.toml` and has to be re-enabled at the end of this work.

## Three things this session taught

1. A zero-byte console is a measurement of the reader until the reader is proven. Two sessions concluded the
   kernel had stopped printing when it had not.
2. Renaming a boot argument is a fault surface. `47ec6a5` dropped a prefix from every argument, and two
   different renames (`gpu-addon`, `pmgr`) each cost a boot; a rename is only safe after `grep` for the reader,
   in the kernel and in the component, not only in this repository.
3. The image is the payload plus its build inputs. The trust bundle, the seed and the addon are not build
   details: a link-only stub among them looked exactly like a kernel hang, and cost two boots.
