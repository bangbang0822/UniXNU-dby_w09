# Handover packages

Two handover packages arrived as archives outside this repository. They are kept here as they
arrived -- the same file names, the same directory names -- minus their binaries.

The source trees they carry are not repeated: the tree is what this repository is. What is kept is
the part that is not reconstructible from a clean checkout: how the work was verified, which
observations support the record, and the tools that read them back.

| Directory | Package | Date | Kept | Omitted |
| --- | --- | --- | --- | --- |
| [`2026-11-12-gpu/`](2026-11-12-gpu/IMPORT.md) | `GPU交接-2026-11-12.zip`, 117 MB | 2026-11-12 | the handover document, the verifier scripts, the tools, the evidence | `02-UEFI`, `03-payload`, `04-GPU插件`, `05-源码` |
| [`2026-11-04-touch/`](2026-11-04-touch/IMPORT.md) | `DBY-W09-触控子系统-v3-2026-11-04.zip`, 3.8 MB | 2026-11-04 | the integration notes, the progress report, the configuration fragments, the tools, the evidence minus one image | `01-源码`, and one 32 MiB boot-log image |

Why the binaries stay out: the images in these packages carry this unit's own APFS volume UUID,
chip unique ID and Data seed baked in -- the three inputs `proprietary-files.txt` says never enter
Git, and the reason a handover package is not a distributable image. `05-源码` is the fourth
omission and a different case: it is a snapshot of this repository, and this repository is the
copy that has moved on.

Each package directory has an `IMPORT.md` naming exactly what was left behind and the sha256 of
anything that was dropped after arriving; `SHA256SUMS.txt` in the same directory covers what is
committed here.

The GPU package was issued twice. The 2026-11-10 revision is not kept: by the package's own
account its first version failed an independent review, and the 2026-11-12 revision fixes the six
items that review named.
