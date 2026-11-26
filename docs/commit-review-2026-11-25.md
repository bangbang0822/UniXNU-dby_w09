# 提交前审核包（2026-11-25，v2 —— 按第一轮审计的 6 个 BLOCKER 修订）

目的：把本轮**属于我的**改动提交到 gitee，且不把另一条工作线（wireless/PCIe）的东西带上车。
v1 审计结论为 FAIL（`.research/charging/reviews/precommit.md`），本版逐条处置；审计 PASS 后才 commit + push。

## 1. 要提交的三个 commit

| # | 主题 | 文件 | 说明 |
| --- | --- | --- | --- |
| 1 | `feat(display): instrument the frame path from submission to the glass` | `drivers/GopFramebuffer/NativeLegacyDisplay.cpp`、`package/LegacyDisplayProbe.cpp`、`drivers/Adreno740/NativeAdrenoSubmit.cpp`、`drivers/Adreno740/NativeAdrenoPublish.cpp`、`package/BootLog.c`（**S0a 变体**） | 120 Hz 规格的 S0a：只加观测，不改控制流 |
| 2 | `fix(power): judge the input-current ladder by the measured cell current` | **`package/DbyBattery.cpp` 一个文件** | 充电 P1（IBAT 窗口判据）+ 驱动侧 `[charger]` 观测行；只写 ICL、只读 FG。**不再改 `BootLog.c`**（见 §3） |
| 3 | `docs: the 120 Hz and charging specifications, reviews and surveys` | `docs/charging-spec-2026-11-24.md`、`docs/gpu-120fps-plan-2026-11-24.md`、`docs/gpu-120fps-spec-2026-11-24.md`、`docs/gpu-120fps-spec-review-2026-11-24.md`、**`docs/gpu-120fps-s0a-review.md`**、`docs/gpu-8250-approach-survey-2026-11-21.md`、`docs/commit-review-2026-11-25.md` | 七份文档（第三份是审核包本身，随代码一起入库） |

## 2. 明确不提交

| 路径 | 原因 |
| --- | --- |
| `board.toml`、`configs/dby_w09_defconfig`、`package/BootProgress.c`、`package/SmmuBypass.h`、`porting.toml` | 另一条工作线的未提交改动（`porting.toml` 的 531 行是那条线的记录） |
| `package/DbyPcie.cpp`、`package/DbyPciePhySeq.h`、`scripts/build-dby-pcie.py`、`docs/wireless-*` | 那条线的新文件与文档 |
| `*.before-*` 等备份 | 本地备份 |

**记录义务的例外**：本轮不改 `porting.toml`，因为该文件同时被另一条线修改；本轮的 evidence 写在
`docs/charging-spec-2026-11-24.md` §12 与 `docs/gpu-120fps-s0a-review.md` §8，commit body 里明说这一点。

## 3. `BootLog.c` 只需一个变体

v1 审计发现两处问题：变体里残留了另一条线 `if (&native_dbypcie_census)` 的**闭括号**（不编译），
以及**两个生产者打同一个 `[charger]` 标签但字段集不同**。处置：

1. 移除工作树里 `BootLog.c` 的 `[charger]` 行与 extern，`native_battery_charger` 数组与 `portCode()` 一并从
   `DbyBattery.cpp` 删除（它们只喂那一行）——**`[charger]` 的唯一生产者是驱动侧 kprintf（每 10 个周期一行）**；
2. 于是 commit 2 只动 `DbyBattery.cpp`，commit 1 的 `BootLog.c` 变体 = 工作树去掉另一条线的两段（extern 与 row）；
3. 变体用**括号配平**校验：HEAD 206/206、变体 218/218、工作树 220/220（工作树多的是那条线的 extern+row）。

文件：`.research/staging/BootLog.s0a.c`（用于 commit 1）、`BootLog.HEAD.c`（对照）。

## 4. 本轮重新构建（代码必须等于产物）

v1 的 `aa6a2fce` 是改动前的树构建的；按审计修完（删数组/`portCode`、修注释、恢复换行）后**重新构建**：

| 项 | 值 |
| --- | --- |
| kernel | `ab27a09a42f3793c0dd20dc7cfcbc9fe15252448f7cb91a7cd53ae47c5333920` |
| payload | `bd2e2f27…`（归档 `/root/payloads/bd2e2f27.img`），218,103,808 B |
| 参数 | 56 / 995 B 未变（`dby-battery=2` 已在） |
| 镜像校验 | `[charger] port=` 字符串 ×1（只剩驱动那一个生产者） |

## 5. 评审请逐条核对（PASS/FAIL + 证据）

1. **隔离**：`BootLog.s0a.c` 与 `BootLog.HEAD.c` 的差只含 S0a 的 extern 与五行；`native_dbypcie_census`、`[dby-pcie]`、
   `native_battery_charger`、`[charger]` 四个串**都不出现**；两文件括号数相等（218/218 vs 206/206）。
2. **无越界**：三个 commit 的文件清单不含 §2 的任何路径。
3. **无泄漏**：改动文件里没有 `/root/`、`/mnt/c/`、私钥、`.img`/`.dbr`/`.ipsw` 字样。
4. **约定**：三个主题过 `scripts/check-commit-messages.py`（含 `--strict-scopes`）；`display`=`porting.toml:317`、
   `power`=`:742`。
5. **文档与代码一致**：`docs/gpu-120fps-s0a-review.md` 是否已改为 `noteFrameVisible(unsigned kind)` + 校验器时间戳；
   `docs/charging-spec` §5 是否已改成"驱动 kprintf、每 10 个 2 s 周期、字段 = port apsd res il cfg iclmax applied want
   flt susp rt stage en ma step stv ev"；§12.05 是否明确 `aa6a2fce` 已刷、`bd2e2f27` 未刷。
6. **`DbyBattery.cpp` 范围**：每个 hunk 属于 P1（饥饿窗口 + ceiling 只降 + 拔线复位）或 `[charger]` 观测行；
   不得再有 `native_battery_charger` / `portCode` / `kBatteryUnknown` 残留。
7. **可构建**：kernel `ab27a09a…` 就是从这份工作树构建的；`git stash` 后三个 commit 的内容应与之一致。
8. **其它**：按显式路径 `git add`（工作树有 60 个未跟踪的 `.before-*` 与另一条线文件，绝不 `git add -A`）。

## 6. 推送

`gitee` = `https://gitee.com/uni-xnu-dby-w09/UniXNU-dby_w09.git`，本地与 `gitee/main` 均为 `a0a455b`。
push 后 `git log --oneline -4` 与 `git status --short` 留证。
