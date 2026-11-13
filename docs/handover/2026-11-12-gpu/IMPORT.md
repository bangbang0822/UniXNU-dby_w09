# GPU handover, 2026-11-12

Source archive: `GPU交接-2026-11-12.zip` on the author's desktop, 117 MB. The same content also
exists as the extracted directory `GPU交接-2026-11-12/`. This is the second revision of the
package; the 2026-11-10 revision is not kept here, because the package's own entry document says
the first revision failed an independent review and lists the six items this one fixes.

Read [`00-先读我.md`](00-先读我.md) first -- it is the package's entry document and it is kept
verbatim, in the language it was written in.

## What is here

| Path | Content |
| --- | --- |
| `00-先读我.md` | The package's entry document: what was verified, how to use it, what it cannot do |
| `01-校验/` | `MANIFEST.json` (size and sha256 for every file of the full package), `MANIFEST.sha256`, `verify-package.py`, `args-check.py`, `args-diff.py`, `make-s151-excerpt.py`, `make-stage179-toml.py` |
| `06-工具/` | `decode-thisboot.py`, `read-payload-args.py`, `llvm-objcopy-linux.py`, `brew-shim.sh`, `flash-preload-edl.ps1`, `read-window-8192-keep-edl.ps1` |
| `07-证据/` | `gpu-实测摘要.md`, the S130/S131 GPU row dumps, the S132 root-mount panic, `s151-关键页摘录.txt`, `lun0-partitions.txt` |
| `TOUCH-PLAN.md` | The touch plan that sat at the root of the package's source tree |
| `board-stage179.toml` | `board.toml` as of stage 179, the configuration behind the S151 evidence |

## What was left behind

| Path | Size | Why |
| --- | --- | --- |
| `02-UEFI/` | 8.0 MB | Firmware build output for this unit; the firmware tree is `uefi-dby_w09`, mirrored separately |
| `03-payload/` | 208 MB | Built payload images with the unit's volume UUID, chip unique ID and Data seed baked in |
| `04-GPU插件/` | 48.9 MB | Built plug-in binaries; their sources are `05-源码` and the tree here |
| `05-源码/` | 507 files | A snapshot of this repository, 332 files of which are already byte-identical here |

Nothing was dropped after arriving, so this directory has no separately recorded hashes; the
package's own `01-校验/MANIFEST.json` describes the full archive if it ever has to be checked
against the original.
