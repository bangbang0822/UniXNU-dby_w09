# Handover, 2026-11-16 evening

One day, 17 hours, 30 commits. This is what is true at the end of it, what is on the tablet, and
what the first hour of the next session should be.

## What works on the tablet now

Payload **`79d5dcb8`** (`touch-restored.img` on the build host, `/root/payloads/`):

- display: home screen, no white lines;
- touch: works, ~32 Hz, **Y axis inverted** (moving up moves the pointer down);
- CPUs: **all eight online** (`SMP req=1 run=7 … c1..c7=step7`);
- buttons: poller runs but reads `pon=04 g6=01` unchanged, so no key event;
- speaker: no sound, but the path is further along than it looked (see below).

Other payloads kept on the build host: `btnraw.img` (hang, see below), `ymirror.img` (touch
direction fixed, peek-first reads, 51 Hz - the best touch state measured), `smp-btn.img`,
`a794378e.img`, `15d4b505.img`, `repo-clean.img`, `touch-restored.img`, `peek-first.img`,
`gap2.img`, `gap2s.img`.

## The three findings that matter

**1. The tablet was running on one core, and the reason was a name.** `board.toml` passed
`sheng-smp=1`; `ArmCpu` reads `smp` (`ArmCpu.cpp:561`), and nothing in the tree parsed
`sheng-smp`. Renamed, and the seven secondaries came up on the next boot. This is also the best
candidate for the touch lag the owner feels.

**2. Touch image selection no longer depends on oeminfo, but the probe does not work yet.** The
selection runs a full enable round per candidate and asks whether the chip announces frames; the
round is part of the test because a chip that was just uploaded answers nothing
(`DbyButtons(hostcmd): bt=1 idle=1 active=1`). With the right image named it proves and selects it.
Two limits measured: a second round in one cold boot does not come up, and a boot with the probe
enabled selected the right image and then had **no touch at all** (cause unknown). The probe is
therefore off by default (`dby-touch-probe=1`), and the default path is the one that works.

**3. The speaker path is up to the last hop.** Four TFA9874 amps answer and are initialised
(`slots=4`), the APR ladder reaches `step=13` (ASM run) with `status=0` on every step from 8, and
`dby_smartpa_play()` returns 4. What is not proven is that real audio reaches the DSP and that the
amp's unmute/route is right; no app has played a sound on the tablet yet.

## Open items, in the order they should be taken

1. **CI for the GPU addon** is blocked on GitHub's macOS runners, not on code: run 9 was
   `cancelled` at 15 minutes and run 10 spent 20 minutes `queued` then failed with an empty log.
   Eight fixes landed today (exec bit, gitee core clone + pin, seed diagnostic, glslang, Adreno
   source resolution in three scripts, the one link that fixes all three, bison/flex). The next
   dispatch needs a runner that actually starts.
2. **Y axis**: the mirror was measured and stashed (`git stash list`, `touch: y-mirror, peek-first
   skip, button poll1 log`) - it works, and it was never committed because the user asked to go
   back to the previous touch build.
3. **Touch latency**: 32 Hz with the 12 ms gap; the two-rate pacing reached 51 Hz and is stashed
   with the same work. The peek-first read (skip the 8 KiB read when the header did not move) is in
   the same stash and is the bigger lever.
4. **The hang at 18:30**: payload `f2bb4d7a` (button raw log every 5 s) stopped enumerating USB
   with the screen lit. The previous payload booted fine afterwards. Recorded in `porting.toml`;
   the suspicion is the button thread's `kprintf`, which is its first console write.
5. **Volume keys** are not on PM8150 `PON RT_STS` bit 0/1 nor on PMIC GPIO6 `RT_STS`: both bytes
   stayed `04`/`01` over 10 000 polls with a key held. The vendor DT that names the real pad is not
   in this repository.
6. **Speaker**: play something on the tablet and read the console; that is the one measurement that
   separates "no audio data" from "amp never unmuted".

## The rule that paid for itself

Every change went through a review sub-agent before it was flashed, and the reviews caught: a touch
probe that read before the start-scanning pair (would have rejected every image silently), a
selection loop that left a different image on the chip than the rows named, a missing execute bit in
a script the runner calls, a workflow that checked out the wrong core line, and a record line that
quoted a 25 ms cadence the code no longer had. Two of those would have cost a flash cycle each.

## Where the code is

`gitee.com/uni-xnu-dby-w09/UniXNU-dby_w09` `main` at `a2f7249`; the core line is
`gitee.com/uni-xnu-dby-w09/UniXNU` `dby-w09` at `f28ea59`, pinned by the workflow. The GPU addon
workflow pins that commit; the seed it needs is cached on the fork after run 4.
