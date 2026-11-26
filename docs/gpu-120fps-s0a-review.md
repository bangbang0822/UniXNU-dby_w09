# S0a 实现与评审稿（v2）—— 观测三行 + 帧预算/漏帧账（纯仪表）

- 对应规格：`docs/gpu-120fps-spec-2026-11-24.md` §5 行语法 / §6-S0a、S0b
- 性质：**不改任何控制流**；只新增计数器、时间戳与五行控制台输出
- 状态：**v2，已按第一轮两份评审（3+1 BLOCKER）修改；待第二轮复核与构建**
- 第一轮评审：`.research/gpu-120/reviews/s0a-code.md`（代码）、`s0a-design.md`（测量设计）

---

## 1. 改了什么（机制）

测量一个问题：**"这一帧从被内核接手到真的出现在面板上用了多久；如果它没出现，属于哪一类丢帧"**。

| 计数器 | 位置 | 记录 |
| --- | --- | --- |
| `native_legacy_visible[272]`（uint32） | `NativeLegacyDisplay.cpp` | 可见帧计数（总数 + 按路径）、128 个间隔环、128 个预算环、最长值、乱序次数、起点被覆盖数、无起点数 |
| `native_legacy_visible_sum[2]`（uint64） | 同上 | 间隔总和、预算总和（避开 uint32 在 71.6 分钟处回绕） |
| `native_adreno_submit_drain[5]` | `NativeAdrenoSubmit.cpp` | 页表切换排空：次数、总时长、其中 fence 等待数/超时数、最长一次 |
| `native_adreno_lock_wait[4]` | `NativeAdrenoPublish.cpp` | `dispatch` 前的**锁争用**：次数、总时长、最长、未等待次数 |
| `native_adreno_call_hold[20]` / `_max[20]` | 同上 | **每个方法持锁时长**（总和/最长）：`wait` 是受害者，`hold` 是原因 |

**时间戳**：`noteFrameVisible(unsigned kind)` 在**五处**调用（无参数）；
预算起点是**校验器接受该 swap 的时刻**（`package/LegacyDisplayProbe.cpp` 的 `native_legacy_swap_accepted_us`），不是驱动取帧的时刻。
且 **start 在调用 `finishTransfer()` 之前快照并清零**——因为 `finishTransfer()` 末尾会进 `kickSwapQueue()`→`kickOnce()`，
那里会给下一帧写新的 start（第一轮 MAJOR 1）。

五处：直扫 latch（`latchTick` 的 `finishTransfer(request,true)` 之后）、空白帧、CPU copy、
GPU 提前退休成功分支、GPU 非提前退休分支。**`flip()` 处不记**（只是写寄存器，面板还没取走）。

## 2. 它能弄坏什么（共享状态逐条）

| 共享状态 | 还有谁用 | 结论 |
| --- | --- | --- |
| `native_legacy_direct[]` | `BootLog.c:868-872`、`BootProgress.c:32` | 未改；仅读 `[4]`（最长 latch）用于 `panel=` |
| `native_adreno_fence_wait[]` | 驱动写、BootLog 读 | 只做差（排空内/外），不改语义；并发下差值可能失真，已注释 |
| `clock.idle` / 等待路径 | `Publish.cpp:674` | 未改（S1 才动） |
| `g.lock` | 多处 | 只读本次调用的 `lockClock[LockWaited]/[LockHeld]`，在 `runTimed` 返回后立即读；不改持锁范围 |
| `finishGpuSwap()` | GPU 完成线程 | 成功分支才清零 start 并计数；失败分支不动 start（不偷下一帧） |
| `useVm()` / `forgetVm()` | 每次提交 / VM 销毁 | 只在 `waitAll` 前后取时间与计数快照，返回值不变 |
| BootLog | `[gpu ibs]` 之后 | 五个 `if (&symbol)` 块；**全部先测地址再解引用**（第一轮 BLOCKER） |

## 3. 不上机怎么查

| 检查 | 命令 | 期望 |
| --- | --- | --- |
| 编译 | `./unixnu build kernel -j 16 -D /root/dev-qud --uefi /root/uefi-alioth` | 返回 0 |
| 产物 | `sha256sum kernel.macho` | 记入本轮 evidence |
| 仓库自检 | `python3 check-repository.py` | 通过 |
| 回归门 | `python3 check-regression.py` | `fail=0`；`kernel_args_bytes` 不变 |
| 符号 | `llvm-nm kernel.macho \| grep -E 'native_legacy_visible\|native_adreno_submit_drain\|native_adreno_lock_wait\|native_adreno_call_hold'` | 四个都在 |
| 类型/边界 | 手工核 | 见 §6 第 10 条 |

**本轮不做主机侧行为测试**：没有控制流、没有状态机。

## 4. 上机看什么（每行一个判据）

| 行 | 通过 | 失败的含义 |
| --- | --- | --- |
| `[frame]` | `n>0`；`win` 与 `fps` 自洽（`fps≈n*1e6/win`）；`panel` ≈16667（60 Hz）；动画时 `p50≈16667` | `fps` 远高于面板 → 间隔环记的不是呈现；`panel≈8333` → 面板其实已在 120 Hz（S4 前提要重写） |
| `[frame budget]` | `n` 有值、`avg` 与 `[gpu methods] 5` **同量级**（不是小于一个数量级） | `avg` 只有 latch 量级（~8.5 ms）→ 起点太晚，预算测的是 latch 而不是提交（第一轮 BLOCKER 的形态） |
| `[frame loss]` | `not_shown` 接近 0；`blank` 与 `[swapok] r5` 一致 | `not_shown` 大 → 真丢帧；`not_shown==acc` → `shown` 没计数（路径漏了） |
| `[gpu submit]` | `p0..p5` 有值；`drain` 与 `hold` 都有值 | `drain` 大而 `hold` 小 → 排空在别的上下文；`hold` 大而 `drain` 小 → 锁被别的方法占着 |
| `[gpu ttbr]` | `mode=1`、`ttbr_writes>0`、`cpchk=0` | `mode=2` → 本板走 CP 换表，S2 前提要重写 |

**S2 的分叉判据（第一轮设计评审 BLOCKER 3 的修正）**：比较 **`drain` 与 `hold`(method 5)**，而不是
`drain` 与 `lock`（后者是受害者，且排空的持有者就是它自己）。若 `drain/p5 ≥ 80 %` 且 `hold ≈ p5` ⇒ 排空即持锁原因，走 S2-A；
若 `hold` 里有别的（或 `hold ≪ p5`）⇒ 走 S2-B。

## 5. 回退

flash 前归档（规格 §3.3 四步）；回退 = flash 上一份 payload。S0a 只加观测，回退后行为与现在一致。

## 6. 已知限制（写清楚，避免下一轮误读）

1. 间隔/预算环是 lock-free：`visibleLastTick` 原子交换，数组普通递增；两写者并发时可能丢一个样本。**不当精确统计用。**
2. `[frame]` 只在动画时有意义；行里带 `win`/`n`/`fps`，静止桌面读成低帧率而不是丢帧。
3. `budget` 的起点是"内核接手 swap"，不是用户态提交；真实端到端更长，本值是**下界**。
4. `drain` 只覆盖 `useVm`/`forgetVm` 的 `waitAll`；`p5` 只累加成功提交，`drain` 包含 `forgetVm`（不在任何提交里），
   所以 **`drain` 与 `p5` 的分母不同，只能比计数不能只比均值**。
5. `p2` 含 write-combine 审计与 `reassertWriteCombined`（是我们自己的开销），`p4` 含每次提交的四个 `peekWords`
   + 第二次 `describe()`（也是我们的取证开销）。S0a 只记录，不裁剪。
6. `[frame loss]` 的 `not_shown = acc − shown`（`shown` 含 blank），与 `[frame]`/`r5` 必须互证；
   用 `flips` 作分母会把 copy/blank 全算成丢帧（第一轮 MAJOR 3）。
7. `[frame]` 的百分比分母是**窗口内样本数**（≤128），不是生命周期计数（第一轮 MAJOR 2）。
8. `noteFrameVisible` 在 latch 临界区与完成线程里调用：无锁、无阻塞、无长耗时。
9. `[frame]` 在 50 ms give-up 分支也会计数（帧最终仍退休）；`replaced` 表示起点被下一帧覆盖。
10. 数组大小：`visible[272]`、`visible_sum[2]`(u64)、`drain[5]`、`lock_wait[4]`、`call_hold[20]`、`call_hold_max[20]`、
    `submit_phase[7]`(u64)、`cp_image[22]`、`table_switches[8]`、`cp_aperture[4]`；BootLog 侧同尺寸、全部 `weak_import`。

## 7. 第一轮评审的处置

| 评审项 | 处置 |
| --- | --- |
| 代码 BLOCKER：`if (native_legacy_visible[0])` 解引用弱符号 | 改为 `if (&native_legacy_visible && native_legacy_visible[0])`；`[frame loss]` 的 `||` 右侧同样改为测地址 |
| 代码 MAJOR 1：budget 起点被下一帧偷走 | 起点改为**校验器的接受时刻**（`native_legacy_swap_accepted_us`），`noteFrameVisible(unsigned kind)` 不再带起点，取帧处不打时间戳 |
| 代码 MAJOR 2：百分比分母用生命周期计数 | 分母改为窗口内 `have`，并新增 `win`/`fps` |
| 代码 MAJOR 3：`not_shown = acc − flips` 伪造丢帧 | 改为 `acc − shown`（`shown` 含全部路径），并保留 `flips` 仅作参考 |
| 代码 MAJOR 4：`back=` 打错计数器 | 删除该字段，改为 `backint=`（乱序间隔数）；`directToBoot` 的计数留给 S3a |
| 设计 BLOCKER 1：budget 起点在 dispatch 之后 | 见 MAJOR 1 的修正：起点是 `kickOnce` 取到 swap 的时刻，**早于** method 5 的返回；`[frame budget]` 因而含排队等待 |
| 设计 BLOCKER 2：`not_shown` 伪造 | 见 MAJOR 3 |
| 设计 BLOCKER 3：S2 分叉不可判 | 新增 `native_adreno_call_hold[]` 与 `hold=`/`wait=` 字段；判据改为 `drain` vs `hold` |
| 设计 MAJOR：60 Hz 不诚实、缺 `win=` | 行里加 `T=8333us panel=%uus`，`b9` 恢复为 120 Hz 预算语义并加 `win`/`fps` |
| 设计 MAJOR：`p5` 内容不止 useVm+ring | 在 §6 第 4/5 条写明；不裁剪 |
| MINOR（µs 回绕、双名、缺 guard 等） | 求和使用 `native_legacy_visible_sum[]`(u64)；`[12]`=nostamp、`[13]`=backwards；所有读点加地址 guard |

**过程问题**：工作树里 `BootLog.c` 还带着另一条线（wireless）的 `native_dbypcie_census` 块与 `board.toml`/`configs`/`BootProgress.c`/`SmmuBypass.h`
的改动。S0a 的 commit **只包含**本节四个文件里属于 S0a 的 hunk，其余不得混入。

---

## 8. 第二轮复核与构建结果

第二轮复核：`.research/gpu-120/reviews/s0a-round2.md`，结论 **C1/C2/C3/C4/C5/D2/D3 FIXED，D1 未修好，D4/D5 部分修好**，
并指出 1 个 MAJOR 回归（`[frame loss]` 丢了 `late`/`miss`）与 5 个 MINOR。第二轮之后又改了三处：

| 轮二问题 | 最终处置 |
| --- | --- |
| **D1（未修好）**：`[frame budget]` 的起点仍在 `kickOnce`，即 method 5 返回之后，因此只测 pick→latch ≈8.5 ms，**不可与 42 ms 比较** | 起点改为**校验器接受该 swap 的时刻**：`package/LegacyDisplayProbe.cpp` 在 `native_legacy_swap_validation_observed` 的 accept 分支写 `volatile uint64_t native_legacy_swap_accepted_us`（cntpct/cntfrq，与串口时基同源），`noteFrameVisible` 读它算 budget。这是内核对该帧的**第一次接触**，因此预算**包含**整个提交路径。`kickOnce` 里不再打任何时间戳 |
| D4：列名仍是 `b9`、缺 `budget120` | 改回 `budget120=`；并加 `T=8333us panel=%uus`（`panel` 取 `native_legacy_direct[4]` 的最长 latch，~16.7 ms 即 60 Hz，**不读任何新寄存器**） |
| D5：`drain` 与 `p5` 分母不同 | 行里同时打印两者的计数（`drain=…/1398`、`n=…`），并在 §6 第 4 条写明"只能比计数" |
| 回归 MAJOR：`[frame loss]` 少了 `late`/`miss` | 加回（窗口内计数），并保留 `blank`；`replaced` 改为不再打印 |
| MINOR：`have==0` 时打 `win=0ms` | `[frame]` 的门加 `&& native_legacy_visible[5]`（没有间隔样本就不打这一行） |
| MINOR：`[frame]` 行宽 375 > 340 | `row[420]` |
| MINOR：`visible[10]` 无写者 | 注释标明 `[10] unused（保持偏移固定）`；预算总和改用 `native_legacy_visible_sum[1]`（u64） |

### 8.1 构建结果（2026-11-24 13:5x）

| 项 | 值 |
| --- | --- |
| 命令 | `/root/core-pub: UNIXNU_UEFI=/root/uefi-alioth ./unixnu build kernel -j 16 -D /root/dev-qud --uefi /root/uefi-alioth` |
| 返回码 | **0** |
| `kernel.macho` | **sha256 `585981037e85b8f5c4fd77196a4d4ef4898c242a3ca577d12de41d0a0bd29439`**（126,611,456 B） |
| 五个源文件（树内 hash 前缀） | `BootLog.c` d0154675a52d、`LegacyDisplayProbe.cpp` 179761bc4e89、`NativeLegacyDisplay.cpp` 0ec6d3cd6a1f、`NativeAdrenoSubmit.cpp` d61041bef011、`NativeAdrenoPublish.cpp` 31349511e2fc |
| 参数表 | 树内已还原为 **committed 版 55 参数 / 983 字节**（无 `dby-pcie`、无 `sheng-vsync`），S0a 不动参数 |
| 符号 | `native_legacy_visible`、`native_legacy_visible_sum`、`native_legacy_swap_accepted_us`、`native_adreno_submit_drain`、`native_adreno_lock_wait`、`native_adreno_call_hold`(+`_max`) 全部在镜像里（`llvm-nm` 逐个 grep 命中；对照 `native_adreno_submit_phase`/`native_legacy_direct` 同样命中） |
| 行格式串 | `[frame] win=`、`[frame budget] n=`、`[frame loss] acc=`、`[gpu submit] n=`、`[gpu ttbr] mode=` 各 1 条在镜像字符串表里 |
| 首轮编译错误 | 两处，已修：`weak_import` 不能标在定义上（`extern "C"` 块内写成定义）→ 改为 `extern "C" volatile … __attribute__((weak_import));`；随之的 `-Wpointer-bool-conversion` 一并消失 |

### 8.3 仓库检查

| 检查 | 结果 |
| --- | --- |
| `scripts/check-repository.py` | 除一条**属于另一条工作线**的失败外全 PASS。失败项在 **untracked 的 wireless 文档**里：它引用了厂商 `core.c` 中超出该文件行数的位置，不由本步修改。本步自己早先的三处越界引用（`gpu-8250-approach-survey` 把 gts7xlwifi 同名文件的行号用在本板只有 624 行的 `NativeAdrenoRing.cpp` 上）已改为本板真实行号 `544-606`/`551-553`/`588-598` |
| `scripts/check-regression.py`（内核轮，无 payload） | 报 `payload.json missing`——该门需要一份已构建的 payload，符合预期 |
| `scripts/check-regression.py`（payload 轮，见 8.4） | **红，4 项，全部与 S0a 无关**（输入身份 3 项 + ADT 长度 1 项），`kernel_args_bytes=995` 未变，`checks=93 pass=89 fail=4` |

### 8.4 payload 构建与归档（2026-11-24 22:33）

**原则**：相对当前已刷的 payload（`be60dd08`），只换内核，其余输入与参数一字不改。

| 项 | 已刷 `be60dd08` | S0a `3ca73bbf` | 结论 |
| --- | --- | --- | --- |
| `kernel_sha256` | `8221dbf7…` | **`58598103…`（S0a）** | **唯一差异** |
| `trust_cache_sha256` | `1dfe0f75…` | `1dfe0f75…` | 同 |
| `data_ramdisk_sha256` | `3b509f95…`（`/root/seed/data-final-prefs.dbr`） | `3b509f95…` | 同 |
| `gpu_addon_sha256` | `8d6571b3…` | `8d6571b3…` | 同 |
| `kernel_args` | 56 / 995 B（含 `dby-pcie=10`） | 56 / 995 B | 同（S0a 不动参数） |

- 产物：`payload.img` 218,103,808 B，**sha256 `3ca73bbf040a31cecf60e6c9df29e130ae78bd5f37fb36013c0f9e65a6ede38f`**，
  已归档为 **`/root/payloads/3ca73bbf.img`**（规格 §3.3 第 1 步）；
- 回退物：`/root/payloads/be60dd08.img` 已存在（当前刷机态），即回退目标；
- `payload.json` 里 `hardware_tested=false`——S0a 还没上机，符合事实；
- **没有 flash**。

### 8.5 回归门为红的真实原因（需要 owner 决策）

`scripts/check-regression.py` 对这份 payload 报 4 项 FAIL，**没有一项由 S0a 引起**：

| FAIL | 已刷 payload 的值 | 基线钉的值 | 是谁动的 |
| --- | --- | --- | --- |
| `trust_cache_sha256` | `1dfe0f75…` | `400d5667…` | 刷机态与基线已经不一致 |
| `gpu_addon_sha256` | `8d6571b3…` | `a267725c…` | addon 由 macOS 产物更新过 |
| `data_ramdisk_sha256` | `3b509f95…` | `423d4000…` | Data seed 换成了 `data-final-prefs.dbr` |
| ADT 与已验收启动的差异 | 15020 B | 14736 B | `/chosen` 内容随输入变 |

也就是说：**当前已刷的那份 payload 本身就不是基线接受的那一份**，这条红在 S0a 之前就存在。
按规格 §3.2 与 §8 第 9 条，`--write-baseline` 在 DoD 未满足前**禁止**使用，所以本步没有重钉基线。

**三条可走的路（需要 owner 选）**：

1. **接受这条红并刷 S0a**：理由是与已刷态只差内核，S0a 没有引入任何一项失败；
   代价是"门是红的"这件事要被记录，且以后每次 flash 都要重复这个判断；
2. **先让门变绿再刷**：把 payload 的三份输入换回基线钉的那三份——但那会让刷机态与现在不同（多了一个变量），
   而且旧 seed 是否还带着当前的数据状态未知；
3. **暂缓，由 wireless 那条线先解决它的输入钉选**（addon/seed/trust 变更是它那一轮带来的），再统一重录基线。

我的建议是 **1**，且把这条红作为 `limits` 记进 `porting.toml` 的 `diagnostics`；但这需要你点名批准才 flash。

### 8.6 下一步（未做）

1. **等 owner 批准后**用 `/root/payloads/3ca73bbf.img` 刷机（`fastboot flash preload`），保留 `be60dd08` 为回退；
2. 上机后按 §4 的判据读五行；`[frame budget] avg` 若仍在 8.5 ms 量级而不是 42 ms 量级，
   说明 `accepted_us` 没被写到或被后一帧覆盖，先查 `[frame loss] not_shown` 与 `nostamp`；
3. 只有五行互证通过，才进入 S1（`clock.idle`）。
