# GPU / 显示 / 触摸 实测摘要（作者机器）

这些行来自作者机器上一次**全绿**启动的面板页（`evidence/edl-log-s151`，image
`payload-stage179-w0fix.img`，sha256 `193831b7ef732ee22df7955981516511bc7ac7876fc24034fcef0670510db293`）。

**原始页就在本包里**：`07-证据/s151-关键页摘录.txt`。它不是抽样——本文引用的每一行都能在那份摘录里
原样找到（摘录保留的是判定行所在的那 9 页：slot 0/1/10/47/69/186/285/441/443）。
摘录用 `01-校验/make-s151-excerpt.py` 从 444 页的完整解码里切出来，未改一个字。

## 1. 启动成功的行

```
[gpu latch] irq=100000 last=100000 n=2 cpi=0 prot=0 fault=0 lcpi=0 lprot=0 st3=cccccccc pf=cccccccc
[gpu zap] st=0 step=30 sec=0 tr=1 ld=0
[gop] arg=1 step=7
[fb] scanout=9c000000 1600x2560 rb=6400 gop=9c000000 match=1
[metal gpu7] backboard s34 e0 v4/4 t27bd241c7 a256/0
[metal gpu7] PF 1/0 r0 v1/0 t38 a38
[metal gpu7] PF 6/0 r0 v4/0 t30 a31
SWAP  calls=21 accepted=21 rejected=0 | gpu offers=19 done=19 failed=0 abandoned=0 fallback=0 last=0
TX c=2 1068,2505 w=6634 n=8 hid=22096/1
TOUCH HID started=1 drv=1 key-reports=0 touch-reports=116 res=0 | pad dots=0
```

## 2. 每一行的意思

| 行 | 含义 | 判据 | 在摘录的哪一页 |
|---|---|---|---|
| `[gpu zap] st=0 step=30 tr=1` | GPU 退出安全模式（PAS 13 走 qcom_scm，用本机自己的 `a650_zap.elf`） | `st=0` 且 `tr=1` | slot 10 |
| `[gpu latch] … fault=0 … pf=cccccccc` | 没有 UCHE/总线故障 | `cpi=0 prot=0 fault=0` | slot 10 |
| `[gop] arg=1 step=7` | GOP presenter 建好（`sheng-fb-gpu=1` 被读到） | `arg=1` | slot 0 |
| `[fb] … match=1` | 呈现目标 = 面板扫描缓冲 | `match=1` | slot 0 |
| `[metal gpu7] backboard … a256/0` | Metal 客户端（backboard = 合成器）在渲染 | `/0` 是错误数，必须 0 | slot 285 |
| `[metal gpu7] PF 1/0 r0 v1/0` | 帧呈现 | `PF` 的 `/0` 必须 0 | slot 47 |
| `[metal gpu7] PF 6/0 r0 v4/0` | 一次提交 6 帧、0 错误（`v4/0` = 4 次成功、0 次失败） | 同上 | slot 69 |
| `SWAP … rejected=0 \| gpu offers=19 done=19 failed=0` | 扫描输出真的被 GPU 接走（不是 CPU 拷贝兜底） | `rejected=0` 且 `done==offers` | slot 186 |
| `TX c=2 1068,2505 …` | 触摸 IC 的一帧真实坐标（手指在屏上时才有） | `c=` 后面是触点数 | slot 441 |
| `TOUCH HID … touch-reports=116 res=0` | 触摸进内核并发布成 HID 报告 | `drv=1` 且 `touch-reports` 在涨 | slot 443 |

> `touch-reports` 是逐步涨的：slot 0 是 `0`、`res=-1`（开机时没人碰），中途 `16`，slot 443 是 `116`、`res=0`。

## 3. 怎么从你自己的机器上读出来

```powershell
# 1) 设备进 EDL，读回整段日志窗口（只读，不写设备）
powershell -File 06-工具\read-window-8192-keep-edl.ps1 -Dir evidence\my-boot

# 2) 解出本次启动的页
python 06-工具\decode-thisboot.py evidence\my-boot\bootlog-window.img --page all > evidence\my-boot\decoded-all.txt

# 3) 找行
Select-String -Path evidence\my-boot\decoded-all.txt -Pattern '\[gop\]|\[fb\]|\[metal gpu7\]|TOUCH HID|SWAP'
```

## 4. 失败时的第一分工

| 你看到的 | 和作者机器的差异 | 先查 |
|---|---|---|
| 完全没有 `[gpu zap]` | GPU 那段没跑到 | `dby-gpu-power=26` 在不在 |
| `[gpu zap]` 有、`[metal gpu7]` 没有 | Metal 客户端没起来 | `sheng-gpu-metal=1`、插件是否装上（`DARWINBOOT: addon` 那行） |
| 有 `[metal gpu7]`、没有 `[gop]` | 显示交接没跑 | `sheng-fb-gpu=1` 与那 9 条 `offline-legacy-*`（`00-先读我.md` §5.1） |
| `[gop] arg=0` | `sheng-fb-gpu=1` 丢了 | 加回 |
| `[fb] match=0` | 呈现目标不是面板扫描缓冲 | 该机显示基址 |
| `SWAP … rejected>0` 或 `fallback>0` | GPU 没接住，退回 CPU 拷贝 | `sheng-fb-gpu=1`、插件、`[metal gpu7]` 的错误计数 |
| 有 `[fb]`、面板仍是菜单 | 面板被 UEFI 占着 | 先做「点屏幕看菜单是否响应」那个五秒判断（`00-先读我.md` §6.1） |
