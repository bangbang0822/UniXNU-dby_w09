# Audit 2026-11-13 - disposition record

Every finding of the 2026-11-13 review, with where it ended up. The batch files it is built from live
outside this repository; this record is the in-tree evidence that each one was read.

Two states are mechanical: a finding whose audited sentence no longer appears in the file was fixed by
changing that code, and one whose file is gone moved out with its component. A finding whose sentence is
still present was fixed *around*, with the audited text kept as the explanation of why the code is what it
is - the note says which.

## Counts

| severity | findings | audited text gone | addressed with the text kept | moved out |
|---|---|---|---|---|
| blocker | 11 | 9 | 1 | 1 |
| major | 65 | 46 | 6 | 13 |
| minor | 270 | 175 | 53 | 42 |
| nit | 248 | 177 | 23 | 48 |

## The findings whose audited text is still in the file

### `drivers/Adreno740/NativeAdrenoBoard.h` - fixed in code: both parsers validate every entry before storing

Last change this session: e5eb95a docs(gpu): point the board parser contract at the pass that enforces it

- [minor] lines 25-26: The documented invariant "never writes on failure" is false: NativeAdrenoBoard.cpp writes `stream_ids[index]` inside the per-entry loop (line 24) before a later entry can fail (lin

### `drivers/Adreno740/NativeAdrenoIrq.cpp` - documented: PROVENANCE paragraph on the mask

Last change this session: 38cbd03 fix(gpu): say where the A740 register tables really come from

- [minor] lines 64-65: kCpErrors is the mask that decides whether a CP_HW interrupt becomes a fatal CP error, but its value has no source. The file's own preamble (lines 5-9) says every register offset a

### `drivers/Adreno740/NativeAdrenoPublish.cpp` - fixed: static_assert ties the three marker capacities; unread mirror described

Last change this session: 2be3502 docs(gpu): correct two claims about the plugin verdict path

- [minor] lines 967-975: A failed attach() (attach_state 2) or a failed start() after a successful attach (detach, attach_state 3) is only stored in native_adreno_metal_diag.attach_status; the function con
- [minor] lines 912-928: The marker-stream writer's destination is a runtime-selected pointer: line may be report2 or report (char[64] each) or native_adreno_client_text[i].text (char[64], line 923), but t
- [nit] lines 931-937: native_adreno_metal_marks is described as the mirror the boot log reads, but nothing reads it: BootLog.c calls native_adreno_metal_marks_dump() (declared here at line 52, used at p

### `drivers/Adreno740/NativeAdrenoPwrup.cpp` - fixed: alignment refused; file is not in this board's archive (recorded)

Last change this session: 46ee0cc fix(gpu): refuse an unaligned preemption-list buffer

- [nit] lines 40-44: The packed image is written through a uint32_t* cast of a uint8_t* buffer 16 bytes in, but neither the function nor the header states an alignment requirement for out ('Output is t

### `drivers/Adreno740/NativeAdrenoPwrup.h` - documented: contract moved onto pack(), exclusion noted

Last change this session: 3ebd7bf docs(gpu): say the preemption list is not compiled into this board

- [nit] lines 25-27: The comment block that documents pack()'s contract ('regs/values are parallel arrays ... *written receives the length on success') sits directly under requiredBytes() and immediate

### `drivers/Adreno740/NativeAdrenoSubmit.cpp` - fixed/documented: the stage comment now matches the handler

Last change this session: 050aba6 docs(gpu): name the snapshot gate instead of a line that moved

- [major] lines 1086: recover() replays each surviving job through useVm(). With table switching in mode 1 - the fallback whenever the CP SMMU aperture check fails (NativeAdrenoPublish.cpp:177, :1144) o
- [minor] lines 85: The stall-stage comment documents stage 2 ('translations dropped'), but the handler only ever assigns 1 (line 629) and then 3 (line 637), so native_adreno_unstick[8+2] can never be

### `drivers/Adreno740/NativeAdrenoTimer.cpp` - fixed: k-prefixed constants with their sources

Last change this session: 98bb8da chore(gpu): k-prefix the timer constants and tag their two sources

- [nit] lines 13-19: The file-scope constants are not k-prefixed as the style requires (TimerGmuLow, TimerGmuHigh, TimerCpLow, TimerCpHigh, TimerRetries), and NativeAdrenoSubmit.cpp:9-11 does the same 

### `drivers/Adreno740/NativeAdrenoUserClient.cpp` - fixed: Tally after validation, fences above UINT32_MAX refused

Last change this session: 50ade4c fix(gpu): make four A740 checks able to fail

- [major] lines 215: The definition of native_adreno_client_stats has eight uint32 per row ([5][8]), but its only consumer declares and reads six per row: package/BootLog.c:149 declares [5][6] and prin
- [minor] lines 539-541: dispatch() builds its Tally with tally_result=kIOReturnSuccess (line 536-537) and only assigns the real status at the end (line 657); the three early returns for an unknown method 
- [minor] lines 398, 750: The ABI fence fields are 64-bit (NativeAdrenoAbiSubmit.wait_fence, NativeAdrenoAbiWaitFence.fence) but both wait paths cast them down to uint32 with no range check: `if(!request->f
- [minor] lines 258-264: When an allocation fails and its release() also fails, the entry is marked used and unreleasable but it was never published: clear() had already set handle=0 and nothing was insert

### `drivers/AdrenoMetal/CacheExport.h` - fixed: sizes above UINT32_MAX refused

Last change this session: 163dfe2 fix(metal): a half-row blit of a one-row frame, and a pack size that cannot be announced

- [minor] lines 134-135: build() narrows both sizes to 32 bits with no bound and still returns true. A stream of 2^32 bytes or more gets a header whose announced stream and compressed sizes are truncated; 

### `drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm` - fixed/documented: publish/release handshake, refusals logged, dead experiment explained, nullptr

Last change this session: 727b235 docs(gpu): say what the disabled early-completion experiment is

- [minor] lines 242-243, 2048: noteWaitCaller() writes WaitCaller.thread[24] and symbol[28] with no lock at all, while native_bridge_metal_telemetry() reads them under gCommitLock. The lock therefore excludes no
- [minor] lines 1826-1839: installAnswers() ignores the return value of class_addMethod(). When _MTLDevice already implements an answer selector the method is not installed, so the base implementation keeps 
- [nit] lines 1861-1862: The 536-byte _MTLDevice instance size is a hardcoded ABI value whose only recorded source is the OS build string in the comment above; unlike the size checks in the ABI headers it 

### `drivers/AdrenoMetal/OfflineMetalFenceWire.h` - fixed: continuation indent

Last change this session: feba4c1 chore(gpu): indent the fence enum the way the rest of the header is

- [nit] lines 5-6: Style: the enum continuation line is indented with one space, where the repository rule is four; OfflineMetalDepthStencilWire.h:5-6 has the same shape. No behavioural effect.

### `drivers/AdrenoMetal/bridge/graphics_cache_loader.cpp` - fixed: static_asserts on the cache header and null checks

Last change this session: b90727a build(gpu): include cstddef for the cache header assertions

- [minor] lines 63-64, 83-84: Both rewritten entry points resolve the real driver function through realDevice.load() and call it without a null check. A vkGetDeviceProcAddr lookup is the one thing here that can
- [nit] lines 59-61: The negative control hardcodes the VkPipelineCacheHeaderVersionOne layout: byte 16 is the first pipelineCacheUUID byte and the 32-byte minimum is the whole header, with no _Static_

### `drivers/AdrenoMetal/bridge/import_count_loader.cpp` - fixed: null check on the resolved entry point

Last change this session: 3b21e04 fix(metal): check what the loaders resolved before calling it, and let two checks fail

- [minor] lines 41-42: The trampoline stores the result of realDevice(device,"vkAllocateMemory") in a local and calls it on the next line without checking for null; the same file rejects a missing loader

### `drivers/AdrenoMetal/bridge/priming_cache_ios.cpp` - see the commits touching this file

Last change this session: c23e9b5 fix(gpu): finish what the independent review found unfinished

- [major] lines 26, 56: The cold producer path is unsatisfiable. When the first-consumer marker is absent, line 26 requires prefix+".archive" NOT to exist (stop(3) otherwise), but line 56 then calls summa

### `drivers/AdrenoMetal/bridge/priming_cache_loader.cpp` - fixed: null check on the resolved entry point

Last change this session: 3b21e04 fix(metal): check what the loaders resolved before calling it, and let two checks fail

- [minor] lines 50-51: primeCompute resolves the real driver entry point through realDevice.load()(device,"vkCreateComputePipelines") and calls the result immediately with no null check, although the bas

### `drivers/AdrenoMetal/bridge/selector_coverage_host.mm` - fixed: non-zero exit on GAPS

Last change this session: 3b21e04 fix(metal): check what the loaders resolved before calling it, and let two checks fail

- [minor] lines 70-72: The tool prints a GAPS verdict for selectors the target references and the frontend does not answer, but main then returns 0, so the exit status reports success on a real coverage 

### `drivers/AdrenoMetal/bridge/surface_import_host.mm` - see the commits touching this file

Last change this session: 9f647b8 fix(metal): give the surface-import instrument the hook the frontend uses

- [blocker] lines 23-25: The instrumentation hooks the wrong no-copy import. The wrapper is defined for nmb_buffer_new_no_copy, but the frontend never calls that function: every IOSurface import goes throu

### `drivers/ShengUfs/GccUfs.h` - documented: [LINUX] citation level, storage limits record the gap

Last change this session: fc4ca7b docs(storage): record the 2026-10-17 clock readback both comments cite

- [minor] lines 2-10: The header names its sources but does not cite them as file:line: eleven register offsets are attributed to 'the public Linux gcc-sm8550.c clock driver', and the three windows to '

### `drivers/ShengUfs/NativeUfs.cpp` - fixed/documented: GPT entry size refused, rename reference handed over, layout from the header

Last change this session: 4f8b5c6 chore(storage): use the shared transport layout in the diagnostic path

- [minor] lines 1333-1342: native_ufs_part_find reads a GPT entry out of a 4096-byte one-block buffer (`uint8_t *buffer = static_cast<uint8_t *> (IOMalloc (4096))`, line 1322) but only refreshes that buffer 
- [minor] lines 1802-1806, 2926-2935: The whole-userdata rename thread loads the file-scope pointer gWholeUserdataDevice and only afterwards takes its own reference, while NativeUfs::stop clears that pointer, terminate

### `drivers/ShengUfs/NativeUfsBsdRootCore.cpp` - documented: the real bound is the UINT32_MAX refusal

Last change this session: b3d9e39 fix(storage): size the GPT entry buffer from the bound that fills it

- [minor] lines 172-178 (also 290-291, 357): Two overflow guards in ParseUserdataPartition can never fire: every accepted entry's last LBA is already rejected above UINT32_MAX at line 140, so selected_last - selected_first is

### `drivers/ShengUfs/RootCommands.h` - documented: window is a snapshot, lease recorded as owed

Last change this session: 4311623 chore(storage): give the six guard-less ShengUfs headers a guard

- [minor] lines 331, 334 (window state written at 359-378): A write is admitted against the open window before gHost.lock is taken, and the window variables that hostInWindow() reads (gRootOpen/gRootFirst/gRootBlocks, gStoreOpen/gLogOpen an

### `drivers/ShengUfs/RootDiagnostics.h` - fixed: include guards on all six headers

Last change this session: 4311623 chore(storage): give the six guard-less ShengUfs headers a guard

- [nit] lines 1-4 (same gap in RootCommands.h, RootQueue.h, RootTransportLayout.h, TransferPoll.h, SmmuTakeover.h): The header starts with the SPDX line and a comment but has no include guard at all - neither #pragma once nor an #ifndef with an _H suffix, against the repository style. Including 

### `drivers/ShengUfs/RootLayout.h` - documented: lbaBytes is the driver's unit; comparison owed

Last change this session: eff642f docs(storage): say what lbaBytes is, and what it is not

- [minor] lines 14, 170 (also 1, 123, 207): lbaBytes is documented as the 'Logical block size reported by the device', but findRootLayout assigns the literal 4096 instead of the value READ CAPACITY returns (RootCommands.h:22

### `drivers/ShengUfs/SmmuTakeover.h` - fixed: reuse requires SCTLR == 0 and the CBAR shape

Last change this session: 4311623 chore(storage): give the six guard-less ShengUfs headers a guard

- [minor] lines 195-198, 249-251: The planner and the configurator disagree on when a reused context bank is usable. planUfsSmmuBank only rejects a bank whose SCTLR.M is set, so a bank with M clear and any other bi

### `drivers/ShengUfs/tests/NativeUfsBsdRootCore.cpp` - fixed: duplicate entry inside the declared range

Last change this session: 99b742c fix(storage): keep the duplicate entry inside the declared geometry

- [nit] lines 319-325: The duplicate-userdata fixture puts the second 'userdata' entry at 507..516 (kUserdataLast + 1 .. kUserdataLast + 10) while setting the header's last usable LBA to 510 on line 323,

### `drivers/ShengUfs/tests/RootCommands.cpp` - fixed: runner added; DMA base source named

Last change this session: 7d71b64 docs(storage): name the source of the fixture-s DMA base

- [minor] lines 136 (also tests/SmmuTakeover.cpp:9 and tests/TransferPoll.cpp:23): Every other test program in this directory has a runner, but these three have none: check_root_path_host.py builds only RootLayout/RootWindowRead/RootDiagnostics/RootPublication, c
- [nit] lines 65 (expectation derived at 179): The fixture's physical DMA base is hardcoded with no source, although every register expectation in the file is derived from it: it is passed to ufs_test_reset here and re-checked 

### `drivers/ShengUfs/tests/RootLayout.cpp` - documented: the fixture's geometry cites its record

Last change this session: 757d013 chore(storage): take the window size from the header in both fixtures

- [minor] lines 134-135 (provenance claim at 140): The optional existing-image branch rebuilds a synthetic GPT around hardcoded userdata endpoints with no source written next to them; the only provenance statement is the printf tex

### `drivers/ShengUfs/tests/WholeUserdataRoot.cpp` - documented: alioth record cited

Last change this session: 53cd4d2 docs(storage): cite the alioth record behind the whole-userdata fixture

- [minor] lines 11-13: The userdata GPT endpoints and capacity are hardcoded with no source next to them. They are not this board's partition (board.toml:46-47 records userdata first_lba 0x29B600, last_l

### `drivers/ShengUfs/tests/check_legacy_descriptors.py` - fixed: verdict from the run, UBSan fatal

Last change this session: bd6f6e1 fix(storage): derive the descriptor report from the run, not a literal

- [nit] lines 81-90: The report is written with a hardcoded 'passed':True. Because the compare binary is built with -fsanitize=address,undefined and run without halt_on_error, a recovered UBSan diagnos

### `drivers/ShengUfs/tests/run-whole-userdata-disk.sh` - fixed: UBSAN_OPTIONS=halt_on_error=1

Last change this session: bde6980 fix(storage): halt the disk runner on a sanitizer report

- [nit] lines 21-24: The sanitized binary enables UBSan but is run without UBSAN_OPTIONS=halt_on_error=1, so a recovered UB diagnostic does not change the exit status; the script's only pass signal is 

### `drivers/ShengUfs/tests/run-whole-userdata-root.sh` - fixed: UBSAN_OPTIONS=halt_on_error=1

Last change this session: fa74396 fix(storage): halt the root runner on a sanitizer report

- [nit] lines 21-24: Same as the disk script: -fsanitize=undefined is enabled but the run does not set UBSAN_OPTIONS=halt_on_error=1, so a UB diagnostic is printed and the script still exits 0.

### `hal/ShengAudioPlugIn.c` - fixed/documented: mapping size checked, dead branch gone, two races documented

Last change this session: 0102372 fix(audio): check the control mapping reaches the trace block

- [minor] lines 563-572: GetZeroTimeStamp consumes the service's anchor update as a seqlock but takes no barrier: the seq read, the anchor read and the re-read of out_seq are unordered volatile loads, so o
- [minor] lines 167,174,628: gVolume is written by SetPropertyData on the HAL's property thread (line 174) and read on the audio IO thread by gain() inside DoIOOperation (line 628) as a plain, non-atomic float
- [nit] lines 45-50,145,650: The size IOConnectMapMemory64 returns is stored and then discarded, so nothing checks that the control mapping actually reaches SHENG_AUDIO_TRACE_OFFSET + sizeof(ShengAudioTrace) (

### `package/DbyAudio.cpp` - fixed: ABI static_asserts, kind mask, stream gated on the level

Last change this session: 2935171 docs(audio): describe the row field the way it prints

- [major] lines 757-762: The channel-down path cancels the three timers and records StChannel but never clears busyMask, and the channel-up path (736-737: apr_pending_init + gAudio.step = 0) restarts the w
- [minor] lines 401, 432-435, 464-465: The 4096-byte control page is written and then read back through hardcoded byte offsets (SHENG_AUDIO_DIAG_OFFSET 2048 for ShengAudioDiag, SHENG_AUDIO_TRACE_OFFSET 2560 for ShengAud

### `package/DbySmartPA.cpp` - fixed/documented: partition bounds, cancel driven to completion, pins recorded

Last change this session: 3657e88 chore(audio): use the GENI registers the shared header defines

- [minor] lines 529-536: The SMEM partition table's own offset/size cells are used to build a pointer into the 2 MiB mapping without any check against that mapping: surveyPart is handed s + peerAt/4 with p
- [minor] lines 248-258: The timeout path calls geni_i2c_timeout() exactly once. Per GeniI2c.h:93-95 that call only sends CANCEL and leaves the engine in GeniCancelling; it must be re-entered (poll/timeout
- [minor] lines 74: The two I2C pin numbers are hardcoded with no source written next to them, which the repository rule forbids (the reset pin at :73 at least names the dts property gpio_reset, and b

### `package/DbyTorch/DbyTorchSpmi.c` - fixed: whole-object reset, timeouts counted

Last change this session: 8c391fa fix(torch): record that the SPMI shape ladder was exhausted

- [minor] lines 198-204: dbt_spmi_discover() re-initialises only io/ee/apid/owner/rform/wform; slots, ppid_hits, the counters, silent, closed and rw_mapped keep whatever a previous session left. A second d

### `package/DbyTorch/DbyTorchSpmi.h` - fixed: the exhausted state is reachable

Last change this session: 55ef8c6 fix(power): count the timeouts the arbiter actually saw

- [minor] lines 47: The timeouts counter is never incremented: every DBT_TIMEOUT return in DbyTorchSpmi.c (lines 78, 90, 101 and 144) returns without touching it, so the field the header presents next
- [nit] lines 48: The comment documents a ladder state that no code path produces: laddered() sets -2 (DbyTorchSpmi.c:80) and, when all three shapes fail, returns DBT_ERROR at line 94 leaving the fi

### `package/DbyTorch/Test/check.h` - fixed: <string.h> included

Last change this session: b50c1c6 fix(power): include string.h where the SEQ macro needs it

- [minor] lines 19: The SEQ macro calls strcmp() but check.h includes only <stdio.h> and <stdlib.h>; it compiles only because both includers happen to include <string.h> first (Test/model.h:12, Test/i

### `package/DbyTorch/Test/model.h` - fixed/documented: bounded copy, timer quantum provenance

Last change this session: e03e788 docs: say what the safety-timer step is sourced from

- [minor] lines 102-104: The safety-timer quantum (10000 us per count) is the one hardware fact in the model with no [V]/[D] citation, although the header states everything the model knows about the hardwa

### `package/DbyTouch/DbyTouchBoard.h` - fixed: one IC-family paragraph, TODO convention dropped

Last change this session: e98f1d7 docs(input): drop the TODO-token convention nothing follows

- [minor] lines 71-75: Two adjacent comments describe the same mask and contradict each other, and the value follows the newer one. Lines 71-72 say the mask covers only DIV[4:0] | SRC_SEL[10:8] | HW_CLK_
- [nit] lines 421-432: The IC-family paragraph is a corrupted paste: the sentence 'IC family: Novatek NT36xxx "9Z" (ic_type 5), not Goodix. Evidence: ... See work/tracks/touchprot/out/PROTOCOL-SPEC.md.' 

### `package/DbyTouch/DbyTouchProtocol.c` - fixed: trim table bounded by sizeof, unused identify removed, cadence in frames

Last change this session: 0eedab6 docs(input): state the log cadence in the units the board runs

- [minor] lines 42-72: The 23x6 version/trim table and its matching loop are a second, hand-copied instance of the same table already pinned in this repository at package/NvtProtocol.c:53-77, and the cop
- [minor] lines 178-196: The two chip registers and the two boot/idle bytes are bare literals with no source next to them, although this repository already carries the sourced form of all of them: DbyTouch
- [nit] lines 213-225: The comment claims the placeholder has been replaced ('the earlier "UNPROBED" placeholder was written before the vendor source settled what the chip is'), but the function body sti

### `package/GeniI2c.h` - documented: [LINUX] citation level; shared-file move recorded

Last change this session: 16961b3 docs(audio): state the provenance level of the GENI map and the PRM ids

- [minor] lines 14-56: The whole serial-engine register map, the M_IRQ bit set, GENI_PROTO_I2C 3 and the opcode/packing values are hardcoded with no source tag anywhere in the header or in GeniI2c.c, and
- [minor] lines 2-6: This is a board-independent Qualcomm GENI serial-engine I2C master: no address, clock, pin or partition of the DBY-W09 appears anywhere in GeniI2c.h/.c (the slave address is a call

### `package/LegacyDisplayProbe.cpp` - fixed/documented: report guards, offset map

Last change this session: 39fa8a8 docs(display): repoint two citations at the lines they mean

- [minor] lines 144, 268: Struct offsets are read as bare integers from opaque objects - `bytes+2624` for the swap request's surface slots, `bytes+5472/5480/5820/5824/252` for the display object, vtable slo
- [minor] lines 373 (also 208, 365): Three of the four report functions write out[0] before looking at cap, while native_legacy_properties_now guards with `if(!out || !cap)return 0;` on line 277. A caller that passes 

### `package/LinkStubs.c` - fixed: tabs, trailing space, two citations, stray comment terminator

Last change this session: 9eb5969 chore(repo): normalise LinkStubs.c and fix its two citations

- [nit] lines 43 (also 44, 60-62, 167-168, 189, 196, 205): The file indents with tab characters on 16 lines where this repository's style is four spaces, and line 70 ends with a trailing space.

### `package/ShengAudioAbi.h` - fixed: three _Static_asserts on the page layout

Last change this session: 787fbe8 fix(audio): pin the control page-s three blocks to their windows

- [minor] lines 49, 81: The kernel/userland control-page layout fixes ShengAudioDiag at offset 2048 and ShengAudioTrace at 2560, but nothing asserts that ShengAudioControl still fits before 2048, that She

### `package/ShengAudioReach.h` - documented: PRM ids and the shared-file move

Last change this session: 16961b3 docs(audio): state the provenance level of the GENI map and the PRM ids

- [minor] lines 2-6: This is the sheng board's audio graph, not this board's: the header says it is 'for the sheng speaker and microphone paths' and the topology it encodes (secondary MI2S speakers on 

### `package/ShengCs35l43.c` - documented: the key-guarded word

Last change this session: 7153366 docs(audio): say where the QRTR domain ids and the 0x2084 word come from

- [minor] lines 57, 71: The power-up and power-down sequences write the raw register 0x2084 with the magic values 0x000f1aa0 and 0x000f1aa3 inside the CS35 test-key sequence, but 0x2084 is not one of the 

### `package/ShengCs35l43.h` - documented: the file describes another sheng unit (audio limits)

Last change this session: (no change this session)

- [minor] lines 2-5: This is the sheng board's amplifier driver, not this unit's: the header describes six CS35L43s on QUP1 SE5 and QUP2 SE0 at I2C addresses 0x40-0x42, while the repository's own audio

### `package/ShengGpr.h` - documented: shared codec, move recorded

Last change this session: (no change this session)

- [minor] lines 2-5: A generic GPR packet codec: it names no dby_w09 resource, and every constant in it is the protocol's (24-byte header, GPR_VERSION 0, APM port 1, 512-byte packet limit). It is not o

### `package/ShengPlatformPower.cpp` - documented: provider offset named, reindented, component move recorded

Last change this session: 3372c58 chore(power): reindent the PMGR bridge with four-space steps

- [minor] lines 1-2: This file is byte-for-byte identical (sha256 cf9abdff...) to UniXNU-alioth/package/ShengPlatformPower.cpp, the Xiaomi Redmi K40, another SM8250 board; it contains no board-specific
- [nit] lines 36-44: The file indents with two spaces throughout, while the repository style requires four; the other package C++ files (for example ShengAudioService.cpp) use four. This is a style dev

### `package/ShengQrtr.c` - documented: domain ids and their provenance

Last change this session: 7153366 docs(audio): say where the QRTR domain ids and the 0x2084 word come from

- [minor] lines 23-26: The protection-domain instance numbers 74 (msm/adsp/*), 76 (msm/cdsp/root_pd) and 180 (msm/modem/root_pd) are hardcoded with no citation, and package/tests/qrtr_host.c:118-119 repe

### `package/TouchProbe.cpp` - documented: reference copy marked, citations and naming

Last change this session: 037b6c4 docs(input): point the ruler citations at the function, not a line

- [minor] lines 460-472, 391-413: The seqlock publication is a plain read-modify-write of a volatile counter, and publishRuler has two producers: touchThread's decoders and swipeThread's `swipeStroke` (lines 1054 a

### `scripts/build-gpu-smmu-probe.py` - fixed: docstring names the script that builds it

Last change this session: 0eded0c docs(tools): point the GPU SMMU probe docstring at the script that builds it

- [major] lines 24-32: The docstring says the defconfig force-loads this archive and that package/BootProgressStubs.c carries no stubs for the probe. Neither is true in this tree, so the silent-absence c
- [nit] lines 20-21: The docstring names scripts/build-cpufreq.py 'in this repository', but no such file exists in scripts/ (the CPU-frequency probe is compiled by scripts/build-gop-legacy-display.py:3

### `scripts/build-storage-bindings.py` - fixed: weak references bound; archive-stem citation

Last change this session: f4a8ea0 docs(tools): cite the block that re-reads the archive stem

- [minor] lines 80, 114: Nothing requires the driver object to import an original storage symbol. In the audit branch (the qualified path the docstring describes first) an empty `imports` - a wrong or stub

### `scripts/build-xnu-4k-baseline.sh` - fixed: USB-debug switch validated, Data UUID in the stamp

Last change this session: dac9e14 fix(tools): validate XNU_QCOM_USB_DEBUG like the switches beside it

- [nit] lines 352, 360, 767: XNU_QCOM_USB_DEBUG is the only build switch in the file taken from the environment without the 0/1 validation every neighbour has (compare XNU_SHENG_PMGR at 338-342 and XNU_BOOT_PR

### `scripts/check-commit-messages.py` - fixed: unreadable record is not 'no areas'; usage range

Last change this session: e8999a9 docs(tools): document the range the commit checker actually reads

- [minor] lines 48-53: read_areas() treats an unreadable or unparsable porting.toml exactly like a record with no areas. With --strict-scopes the empty fallback makes every scope look unknown and reports

### `scripts/check-dby_w09.py` - see the commits touching this file

Last change this session: 7ec9068 build(repo): give the scripts their SPDX line

- [major] lines 90, 104, 107: The vendor-input check is computed and recorded but never contributes to the exit status. `missing` only goes into the result JSON (line 95) and a printed count (line 104), while l

### `scripts/patch-xnu-entry-probe.py` - fixed: banner buffer bound, MAX_MEM no longer required

Last change this session: b47997c chore(tools): drop MAX_MEM from the panic screen and its patcher

- [nit] lines 225: The comment that documents where the banner buffer comes from cites a path that exists nowhere in this repository or in the UniXNU checkout: there is no kits/sheng directory (this 

### `scripts/xnu-panic-screen.c` - fixed: claim before clear, MAX_MEM dropped

Last change this session: b47997c chore(tools): drop MAX_MEM from the panic screen and its patcher

- [minor] lines 248-253, 296-297: The comment promises that a second panic "must not overwrite" the first screen, but the marker it relies on is only stored at the very end of the render (after format(), the frame 
- [nit] lines 292: `(void)MAX_MEM;` is the only use of MAX_MEM in the file and does nothing at run time, yet the patcher still resolves `_max_mem`, refuses a kernel without it (scripts/patch-xnu-entr

