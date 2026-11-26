# Cold macOS build review

## What changes

The packaging workflow now starts from a clean output tree by building an intermediate kernel
without the dual-policy root verifier. That kernel becomes the reference image for the HID and
legacy-display binding steps. The workflow then restores the shipping board configuration and
builds the dual-policy inputs from the checksum-pinned iPadOS 17.7.11 / 21H461 IPSW. The display
builder emits only three temporary callback stubs while no candidate exists; the shipping build
replaces them with the audited backend. The bootstrap image is retained as build evidence and is
never packaged or flashed.

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
that observation.

## Checks without the tablet

Before accepting a run, parse the workflow and TOML files, run `python3 check-repository.py`, and
inspect the macOS run's bootstrap and shipping kernel outputs, the Data UUID recorded in the final
kernel configuration and restored source defconfig, candidate hashes, VerifiedRoot
`archive.json` (`verified_root_policy_count` must be 2), and package manifest. Keep the run logs and
one-day build-input artifacts with the resulting hashes. The repository check and a successful
macOS workflow run are still pending for this change.

## What to observe on the tablet

No image from the bootstrap configuration is eligible for a device. Before a later shipping-image
flash, review the exact source diff and hashes, run the pre-flash regression gate, and record the
rollback payload and its hash. On device, the first decisive storage observation is the VerifiedRoot
row reporting two policies and a published root; the kernel must then reach `launchd`. A failure
before root publication points to the policy/archive path; a published root followed by a userspace
stall is a separate checkpoint. Flash only after the owner approves that reviewed shipping image.
