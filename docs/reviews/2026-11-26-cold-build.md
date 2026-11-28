# Cold macOS build review

## What changes

The packaging workflow now starts from a clean output tree by building an intermediate kernel
in a separate output directory with the dual-policy root verifier enabled. That kernel becomes the
reference image for the HID and legacy-display binding steps. The workflow first builds the
dual-policy inputs from the checksum-pinned iPadOS 17.7.11 / 21H461 IPSW, then copies those verified
inputs into the intermediate tree. The display
builder emits only three temporary callback stubs while no candidate exists; the shipping build
replaces them with the audited backend. The bootstrap image is retained as build evidence and is
never packaged or flashed. The pinned dby-w09 core lacks the SystemOS policy signer, so the workflow
fetches that official UniXNU helper from a fixed upstream commit, checks its SHA-256, and places it
in the assembled tree before signing the SystemOS volume with the same per-run owner key as OS.
The bootstrap defconfig matches the shipping authenticated-root options. The owner key and signed
OS/SystemOS policy inputs are copied from the verified shipping tree before the bootstrap build;
the Data seed UUID is bound through the normal runtime preparation path.
The first-pass display stub archive writes the build report the kernel gate requires, binding the
exact archive hash to the generated stub source hash and marking it as first-pass-only.

## What can break

The bootstrap build uses the audited UFS, VerifiedRoot, Virtio, and boot-progress archives. An
earlier attempt disabled those root inputs and failed at link with unresolved VerifiedRoot and RPMh
symbols, so the workflow now carries the owner key and signed policies into the separate tree and
keeps the complete archive plan. These generated inputs stay in the ephemeral runner workspace; the
owner private key is not uploaded or committed. The display preflight checks its temporary archive
and generated source against the report; it does not treat the stubs as an initialized or presented
display backend. The shipping tree rebuilds this archive after the candidate kernel has been copied
in.
The final `release` invocation selects `dby_w09_defconfig`, uses the saved Data seed, GPU addon and
matching owner trust bundle, and runs the full archive plan. A failed restore or missing candidate
must fail the job before packaging. On a cache miss with `init_data=true`, runtime preparation
binds the generated seed UUID into the bootstrap `.config`; the package step applies that UUID to
the shipping defconfig for the build and restores the tracked defconfig on exit. The core tree
assembler preserves `build/` while refreshing its module links and copied scripts, so the candidate
and generated policy inputs survive the subsequent `tree` and `release` preparations. The display
ABI record carries the previously recorded QEMU checkpoint hash; this workflow does not remeasure
that observation. The new signer validates the exact SystemOS size, UUID and image hash, then
verifies every block through the same VerifiedRoot C library. A missing source, hash mismatch,
or policy verification failure stops before the kernel build. Its download is pinned to commit
`e0ee2ad9a2654309480c8b0cc725d0dc09186cd4` and checked against SHA-256
`a29a653da6812ed092bb548eb37c645b34677c57326047e19369d3bc46e7d700`.

## Checks without the tablet

Before accepting a run, parse the workflow and TOML files, run `python3 check-repository.py`, and
inspect the macOS run's bootstrap and shipping kernel outputs, the Data UUID recorded in the final
kernel configuration and restored source defconfig, candidate hashes, VerifiedRoot
`archive.json` (`verified_root_policy_count` must be 2), and package manifest. Keep the run logs and
one-day build-input artifacts with the resulting hashes. Run `45822828154` on `macos-15` reached
the IPSW extraction and verified the OS and SystemOS image hashes, then stopped because the pinned
core did not contain `build-owner-systemos-policy.py`; it produced no kernel or payload. The pinned
helper is now supplied from the reviewed upstream commit above. The local repository check passes
all 472 checks after the latest runner record was added. Corrected runs `45829828436`,
`45832346714`, `45834952200`, `45838105298`, `45840910709`, `45843837616`, `45848510902`, and
`45851668299` ended before any workflow step started because GitHub did not assign a hosted runner;
each job has an empty runner name and no step output. Runs `45848510902` and `45851668299` each
waited about 15 minutes before ending as failure. The Actions status page reported operational
during the earlier checks. No kernel or payload was
produced by these attempts; a hosted runner is still required to continue. Standalone fallback
run `45851278112` (`macos-14`) was cancelled within seconds before a runner was assigned or any
workflow step ran; it also produced no kernel or payload.

On 2026-11-27, run `45871772311` (`macos-15`) reached the runner but stopped at the preserved-seed
gate because the per-device Data cache was absent. The supported first-install input is
`init_data=true`. Runs `45873226889`, `45875423398`, and `45879601288` then waited about 15 minutes
without runner allocation; runs `45875337853` and `45879572617` (`macos-14`) were cancelled before a
runner or workflow step appeared. Run `45881593042` (`macos-15`, `init_data=true`) passed checkout,
tool installation, source verification, and root-input preparation, then failed while
`prepare-xnu-source.py` queried the GitHub API: HTTP 403, rate limit exceeded. The bootstrap kernel
was not produced and no Data seed was cached. The script already accepts `GITHUB_TOKEN`; the
packaging workflow now passes the built-in read-only token to its bootstrap step, matching the core
macOS workflow. Run `45888888658` (`macos-15`, `init_data=true`, board SHA
`39035014cd7b4298be3bfbb4a8384d31ae330e18`) was assigned a hosted runner and passed checkout,
tools, platform seed, shipping-tree preparation and signed root inputs. It reached the bootstrap
archive plan, confirming the authenticated source fetch passed the previous rate-limit point. The
first compile-record link reported missing board archive symbols and the build continued as designed;
the actual stopping error was `ArmDTPlatform` compilation: Apple Clang on this runner rejected
`-Wno-unnecessary-virtual-specifier` under `-Werror`. No bootstrap kernel or payload was produced.

## macos-15 compiler compatibility review

The core commit `6fde121ca52667a0f3c3bbf55cf2ccd1efb218a2` probes each optional warning
suppression with the compiler named by the XNU compile record. Follow-up core commit
`973f86d6bb7e336e255204b0cf5171b759de1c03` also passes the recorded target to each probe,
because cross Clang may have no default target. It retains supported flags and omits only flags
that the compiler reports as unknown; any other probe failure stops the build. The board workflow
pins the follow-up commit. The change affects the warning policy of all components built by
`build-arm-driver.py`, so a previously suppressed source warning may surface on an older compiler
and stop under `-Werror`; it does not change driver source, kernel arguments or hardware state.
Host checks: WSL `python3 -m py_compile scripts/build-arm-driver.py` and `git diff --check` passed in
the core checkout; the board's repository and record checks passed before dispatch. The next macOS
run must show `ArmDTPlatform` compiling and must identify the next archive or link failure, or produce
the bootstrap and shipping kernel hashes. There is no tablet observation for this build-only change.

## Failed-bootstrap Data seed retention review

Run `45888888658` produced a validated first-install seed in the bootstrap runtime directory but
failed before copying it to the shipping runtime directory. `actions/cache/save` then reported that
its configured path did not exist. Its one-day installation-seed artifact `15151879542` was saved
locally under Git-ignored `.cache/unit-recovery/run-458888/`: `data.dbr` is 1,073,280 bytes with
SHA-256 `0721c8788b7834289d163c0cf7fdccda88a1cc706d4b5514a98e9c74161957a6` and Data
volume UUID `730D857D-9E75-46EF-9355-A814634DDBE5`. The hash matches `inputs.json` in that artifact.

The workflow now copies a generated bootstrap seed and its runtime input report to the configured
cache path even when a later bootstrap archive fails. It verifies the seed against the report before
copying, and refuses an existing shipping seed with different bytes. This touches only the retained
build input; it does not change the seed generator or kernel configuration. A corrupt or incomplete
seed could otherwise poison later runs, so missing reports are skipped and hash mismatches fail.
The actual YAML step was executed on the saved artifact in WSL: the valid seed was cached with its
report, while a one-byte modification was rejected without creating a cache entry. The next CI run
must show a cache save after any post-runtime failure and the following run must restore the same
seed SHA-256. No tablet observation applies to this retention change.

## VerifiedRoot compile-record replay review

Run `46026133847` used board revision `c42bc99` and core `6fde121` on macOS 15. It passed
`ArmDTPlatform`, the previous compiler-warning blocker. The first XNU compile-record link lacked
board archive symbols as expected and continued, but `build-verified-root-probe.py` then failed on
`-Wmissing-include-dirs`: its replayed `bsd_init.o.json` command named the absent bootstrap-tree
`build/xnu/ios-obj/EXPORT_HDRS/libsa` directory. The workflow had created that directory only in
the shipping tree. No bootstrap kernel or payload was produced; the run did produce an installation
seed artifact, but its older workflow did not save the seed to the cache path after failure.

Core `086d5372ca70f9c2cc4f1f1ec302dd48ddbc30e6` now drops a replayed `-I` directory only
when it is absent and resolves inside this generated `EXPORT_HDRS` subtree. It also replaces XNU's
replayed warning switches with `-Wall -Wextra -Werror -Wno-unused-parameter`, matching the other
standalone driver probe. The workflow pins that core revision and removes the ineffective
shipping-tree directory placeholder. The signed OS/SystemOS policy hashes, manifest checks,
trusted-key check, source inputs and archive output remain in place.

Risk: a VerifiedRoot source could begin depending on a generated header that the replay omits, or
the deliberate warning policy could surface a different source error. In either case compilation
must stop; a missing include outside `EXPORT_HDRS` is not suppressed. WSL `py_compile`, `git diff
--check`, and a host check of the include boundary (known generated path skipped, real include
kept, path escaping through `..` rejected) passed. The next clean macOS run must show the
VerifiedRoot archive compile and its dual-policy metadata, then report the next blocker or both
kernel and payload hashes. This is a build-only change; there is no tablet observation or eligible
flash image from the failed run.

## Bootstrap UEFI checkout review

Run `46055634701` built the ARM64/iOS 17 VerifiedRoot probe and reached archive 17,
`build-boot-progress.py`. The bootstrap step did not set `UNIXNU_UEFI`, so the script
tried a default `uefi-sheng` directory next to the assembled tree and could not find
`Font.h`. The job produced a GPU addon and cached the generated Data seed. It stopped
before the final bootstrap link or payload packaging.

The bootstrap step now receives the same absolute UEFI checkout path as the shipping step
and checks the font before starting the build. The firmware checkout is pinned to
`423e222159dfa159d788769554007455d34e6193`, the revision used by the failed run. The check
could expose an incomplete firmware checkout sooner. It does not change the font, boot
arguments, firmware behavior or driver code. The font exists in the pinned source revision
and the local firmware checkout. Next, macOS CI must pass the font check and build archive 17.
Tablet acceptance remains the shipping boot checkpoint below, with the panic-screen text
available if a panic occurs.

## A650 archive compiler review

Run `46095445433` restored the same Data seed as #67, SHA-256
`5b06ed89f78f9cd1414ea2401088760b6ea389fba82d8d5570c59466fa91fe64`, and
passed archive 17 (BootProgress). Archive 24 stopped at a compiler option rejected
by Apple Clang. The remaining bootstrap archives, final link and shipping build did not run.

The board archive recipes that supplied this option unconditionally now probe it with the
compiler and target from the original compile record. The probe retains the option on a
compiler that accepts it. It drops the option only if Clang reports it as unknown and the
same probe without the option succeeds. Real compile warnings remain subject to the original
warning policy. Shared users of this check include display, PCIe, touch and torch. Each can still
stop on a real warning in its source or headers. The WSL check uses real Clang for an
unsupported flag and an invalid target. CI must build past the A650 archive and complete the
bootstrap link. Any later tablet run must use a reviewed shipping image and show the original
boot checkpoints and responsive display and input.

## Linux continuation

On 2026-11-27, the WSL continuation used the saved bootstrap tree and the Linux `llvm-objcopy`
shim documented in the GPU handover. The DbyTorch archive completed at 524,888 bytes with SHA-256
`c09522a7f6a6811e77dea62e8028f39c7fd7e6be33b29ac912c914529b91c307`. The display first-pass
archive and its generated-source report also passed the kernel's hash gate; the archive SHA-256 is
`cbad7ce2997c671e2e13cbc0de25fc043fda988b75aff12fbb08af5965922f6f`. The final link then stopped
with undefined VerifiedRoot and RPMh symbols because that attempted bootstrap config omitted the
signed root inputs and boot-progress providers. It produced no `kernel.macho` or payload. The
reviewed workflow now carries the verified policies into the bootstrap tree and uses the same
authenticated-root config as shipping; that path still needs a macOS runner to confirm it.

## Earlier installation seed recovery

Before its one-day expiry, installation-seed artifact `15107593055` from run `45796042471` was
saved under the Git-ignored `.cache/unit-recovery/run-457960/`. Its `data.dbr` is 1,073,280 bytes,
SHA-256 `d044042c1f2eb7f721396da1ab7cddebcd5e547e88ce7a5df5e14409e4cc2c4f`; the accompanying
manifest records Data volume UUID `B415F419-BD11-4332-A026-86DA4C1B2D2A`. This does not match the
tracked defconfig UUID `2138CDAC-F86A-4DED-9EF1-09F27363DB2A`. This is not a build blocker: the
kernel-build runtime reads the seed's APFS UUID and binds it into the temporary bootstrap config;
the workflow then carries that value into the shipping defconfig for packaging and restores the
tracked file. The seed is therefore a valid build input when restored from this workflow's
platform-seed cache. No Apple restore-image member, Data seed, or owner private key was added to Git.

## What to observe on the tablet

No image from the bootstrap configuration is eligible for a device. Before a later shipping-image
flash, review the exact source diff and hashes, run the pre-flash regression gate, and record the
rollback payload and its hash. On device, the first decisive storage observation is the VerifiedRoot
row reporting two policies and a published root; the kernel must then reach `launchd`. A failure
before root publication points to the policy/archive path; a published root followed by a userspace
stall is a separate checkpoint. Flash only after the owner approves that reviewed shipping image.
