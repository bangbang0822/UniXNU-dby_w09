# Touch handover, 2026-11-04

Source archive: `DBY-W09-触控子系统-v3-2026-11-04.zip` on the author's desktop, 3.8 MB. Read
[`README.md`](README.md) first -- it is the package's own entry document, kept verbatim, and it
says which of the four notes to read depending on what you are doing.

## What is here

| Path | Content |
| --- | --- |
| `README.md` | The package's entry document: what is in it, where the state is recorded |
| `04-整合说明.md` | How to integrate the touch driver: the real stage ladder, `dby-touch-se=1`, order of flashes |
| `05-进度汇报.md` | Progress report: what was verified on hardware, the known limits, next steps |
| `02-配置片段/` | The kernel arguments, the defconfig excerpt, the touch call sites in `BootProgress.c`, and `reproduce-build.sh` |
| `03-证据/` | The log extract behind the multi-touch acceptance run of S39 |
| `06-工具/` | `decode-thisboot.py`, `read-bootlog.ps1`, `emulate-multitouch.py`, `sim-multitouch-global.py`, `precision-grid.py` |
| `SHA256SUMS.original.txt` | The package's own checksum file, kept as it arrived |

## What was left behind

| Path | Size | Why |
| --- | --- | --- |
| `01-源码/` | 6 files | The touch sources as of `dby-touch/2026-11-03b`. Older than the tree here: this package's `DbyTouchFw.h` is 1,192,808 B with the three images inlined, while this repository generates them from `.cache/firmware/novatek` and keeps a 3,356 B header |
| `03-证据/多点验收-s39/bootlog-window.img` | 33,554,432 B | A raw boot-log window, not text. `06-工具/decode-thisboot.py` and `read-bootlog.ps1` are what read it back, so the dump is reproducible from a running unit |

The image is the one file this package had that was dropped after arriving here. Its sha256, so
the omission can be verified against the archive:

```
1B79068A6A9CE9F70AD02AA328E9CB5D317BAAA7EF09167BDAAAA5F05BA6974F
```

`SHA256SUMS.original.txt` still lists the `01-源码` files, which are deliberately not here; that is
where the two checksum files differ. `SHA256SUMS.txt` covers what is committed.
