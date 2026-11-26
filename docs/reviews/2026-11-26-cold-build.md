# Cold macOS build review

## What changes

The packaging workflow now starts from a clean output tree by building an intermediate kernel
without the dual-policy root verifier. That kernel becomes the reference image for the HID and
legacy-display binding steps. The workflow then restores the shipping board configuration and
builds the dual-policy inputs from the checksum-pinned iPadOS 17.7.11 / 21H461 IPSW. The display
builder emits only three temporary callback stubs while no candidate exists; the shipping build
replaces them with the audited backend. The bootstrap image is retained as build evidence and is
never packaged or flashed. The pinned dby-w09 core lacks the SystemOS policy signer, so the workflow
fetches that official UniXNU helper from a fixed upstream commit, checks its SHA-256, and places it
in the assembled tree before signing the SystemOS volume with the same per-run owner key as OS.

## What can break

The temporary config disables the verified-root probe, root audit/trust, cache mount, boot manifest,
Data mount, and SystemOS union, and points NativeVirtioBlock at the plain archive. This keeps the
temporary kernel within the core build script's rule that Data mount requires the authenticated
owner boot manifest. The workflow removes exactly
the three verified-root archive recipes for that pass and restores `board.toml` with an exit trap.
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
all 469 checks after the recovery record was added. Corrected runs `45829828436`, `45832346714`,
`45834952200`, `45838105298`, `45840910709`, `45843837616`, and `45848510902` ended before any
workflow step started because GitHub did not assign a hosted runner; each job has an empty runner
name and no step output. Run `45848510902` waited about 15 minutes before ending as failure. The
Actions status page reported operational during the earlier checks. No kernel or payload was
produced by these attempts; a hosted runner is still required to continue.

Before its one-day expiry, installation-seed artifact `15107593055` from run `45796042471` was
saved under the Git-ignored `.cache/unit-recovery/run-457960/`. Its `data.dbr` is 1,073,280 bytes,
SHA-256 `d044042c1f2eb7f721396da1ab7cddebcd5e547e88ce7a5df5e14409e4cc2c4f`; the accompanying
manifest records Data volume UUID `B415F419-BD11-4332-A026-86DA4C1B2D2A`. This does not match the
tracked defconfig UUID `2138CDAC-F86A-4DED-9EF1-09F27363DB2A`, so the recovered seed is preserved
for recovery only and is not an input to the current shipping build. No Apple restore-image member,
Data seed, or owner private key was added to Git.

## What to observe on the tablet

No image from the bootstrap configuration is eligible for a device. Before a later shipping-image
flash, review the exact source diff and hashes, run the pre-flash regression gate, and record the
rollback payload and its hash. On device, the first decisive storage observation is the VerifiedRoot
row reporting two policies and a published root; the kernel must then reach `launchd`. A failure
before root publication points to the policy/archive path; a published root followed by a userspace
stall is a separate checkpoint. Flash only after the owner approves that reviewed shipping image.
