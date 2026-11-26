# GPU 120 Hz 规格评审记录（v1 → v2）

- 日期：2026-11-24
- 被评审对象：`docs/gpu-120fps-spec-2026-11-24.md` **v1**（draft for review）
- 评审方式：三个互不重叠的评审轴，各自独立读代码/读日志，产出 `[BLOCKER|MAJOR|MINOR]` + `file:line`
- 评审原文：
  - `.research/gpu-120/reviews/a-facts.md`（事实核对，26 KB）
  - `.research/gpu-120/reviews/b-process.md`（流程与仓库规则，28 KB）
  - `.research/gpu-120/reviews/c-technical.md`（对抗性技术评审，44 KB）
- 结论：**v1 不可实施**。14 个 BLOCKER，其中 3 个会让"按规格实施后刷机前的门是红的"或"编译不过"。
- 处置：v2 已重写，逐条见 §3；第二轮复核见 §5。

---

## 1. 评审统计

| 评审轴 | BLOCKER | MAJOR | MINOR | 关注点 |
| --- | ---: | ---: | ---: | --- |
| A 事实核对 | 4 | 9 | 6 | 每个 `path:line`、符号、字节账、行名是否与代码一致 |
| B 流程与规则 | 4 | 6 | 6 | `AGENTS.md` 四问、一次一变量、回退、门、记录、提交、跨线冲突 |
| C 对抗性技术 | 6 | 7 | 5 | 延迟链能否闭合、判据是否自证、顺序是否会把测量带偏 |
| 合计（去重前） | **14** | 22 | 17 | — |

三个轴**独立地**发现了同一个致命项（参数预算），说明它不是笔误而是流程缺陷：
规格复制了一个会变动的数字，而不是引用门的输出。

---

## 2. 最关键的五个发现

1. **每帧预算从未被加总一次**（C）。`dispatch 42.26 ms + latch 8.51 ms ≈ 50.8 ms`，
   对 8.333 ms 预算差 6 倍；而 v1 把最关键的"去排空"（S2）设成"有条件开启"，把只能回收 ~14 % 的
   S1 当成主线。**规格的目标与手段错配。**
2. **42.26 ms 发生在全局锁内**（C，`NativeAdrenoPublish.cpp:712-725` 实证）。
   锁被持有整个排空期 ⇒ 7 个客户端串行 ⇒ 系统级上限约 23.7 次提交/s。
   修法只有两类：mode 2（CP 在 ring 内换表）或把排空移出锁；v1 的"把 2 s 上限改成 20 ms"
   会把慢成功变成快失败，并让 §7 的判据**空洞通过**。
3. **参数预算算错**（A+B+C 独立发现）。工作树实测 **56 参数 / 995 字节**（含未提交的 `dby-pcie=10`），
   HEAD 是 55 / 983；`sheng-vsync=120` 含空格 16 字节 ⇒ 1011 > 1000，门直接红。
4. **`[frame]` 只统计直扫路径**（A）。`latchRequest` 仅在 `NativeLegacyDisplay.cpp:536` 赋值，
   copy / GPU / 空白三条路径都走 `finishTransfer`，帧计时环会系统性漏计。
5. **S1 的分支在本板恒为真**（A+C）。SPI 300 attach 返回 0 但从不投递，于是
   "有 IRQ 就 `assert_wait_timeout`"这条永远被选中——那正是 payload `164b4a52` 冻结 UI 的形态；
   而 `clock.idle` 的返回值在 `NativeAdrenoRing::wait` 里还意味着"fence 超时"。

---

## 3. 逐条处置

记号：**采纳**（v2 已改）/ **部分采纳**（改了但保留差异，说明理由）/ **不采纳**（说明理由）。

### 3.1 A 组（事实）

| # | 发现 | 严重度 | 处置 |
| --- | --- | --- | --- |
| A1 | 参数预算用了 HEAD 的数，工作树 995 | BLOCKER | **采纳**：§3.4 改为"只认门的输出"，不复制数字；新增 S5a（腾位）与 S5b（加参数）两步 |
| A2 | `[frame]` 放 `latchTick` 只统计直扫 | BLOCKER | **采纳**：§5 改为四条路径共用的 `noteFrameVisible(kind)`，并列出四个调用点 |
| A3 | `static bool dbyGpuIdle(void*)` 类型不符 | BLOCKER | **采纳**：改为 `static int dbyGpuIdle(void *cookie)`，并注明 `NativeAdrenoRing.h:38-43` 是 `int (*idle)(void*)` |
| A4 | S1 的 IRQ 分支恒为真 | BLOCKER | **采纳**：S1 **完全不用 `assert_wait_timeout`**，只做纯自旋 + 退回原 100 µs；正常路径一律 `return 0` |
| A5 | `submit_phase[6]` 是次数不是时长 | MAJOR | **采纳**：行改为 `p0..p5` + `n=`；样例值更正 |
| A6 | `drains` 无数据来源 | MAJOR | **采纳**：新增 `native_adreno_submit_drain_ticks`；`ttbr_writes` 明确取 `table_switches[1]` |
| A7 | S2a 引 `:582` 不在排空路径 | MAJOR | **采纳**：v1 的 S2a 整段撤销（见 C3）；不再引用 `:582` |
| A8 | 自动回退只在 mode 2/publish/无在飞工作时发生 | MAJOR | **采纳**：在 S2-A 明确写出三个不触发回退的条件 |
| A9 | `[panel]` 无生产者且与 `[dpu]` 重复 | MAJOR | **采纳**：删除 `[panel]` 行；面板速率改由 latch 上界判定（N10） |
| A10 | `[coherence] slept/spun` 永不打印 | MAJOR | **采纳**：从验收判据中删除，并在 §5 注明"永不打印，不得作判据" |
| A11 | `pclk` 无可读来源 | MAJOR | **采纳**：删除该字段 |
| A12 | 漏了 `sheng-gpu-ttbr` 强制开关 | MAJOR | **采纳**：附录 A 注明该开关存在（且它要 15 字节预算） |
| A13 | `[frame]` 样例把 latch 均值当 p50 | MAJOR | **采纳**：样例改为 `p50≈16667 µs`、`n≈60` |
| A14 | 120.03 Hz 算错 | MINOR | **采纳**：改为 119.9998 Hz |
| A15 | 附录 A 的 `direct[9..13]` 行号错 | MINOR | **采纳**：改为上游 `:56-61` 并注明上游文件 |
| A16 | `mode=` 与 `mode_final=` 同源 | MINOR | **采纳**：`[gpu ttbr]` 改为 `mode=` + `req=`，并要求新增只读"请求模式"槽 |
| A17 | `sw=` 无定义 | MINOR | **采纳**：改名为 `ttbr_writes` 并给出取数方式 |
| A18 | 四处 `clock.idle` 只有 `:4833` 进发布路径 | MINOR | **采纳**：S1 只改 `:4833`，其余三处注明是 probe 本地时钟 |
| A19 | `native_legacy_frame[]` 重名 | MINOR | **采纳**：改名 `native_legacy_visible[]` |
| A20 | 漏帧阈值三处不一致 | MINOR | **采纳**：§5 统一为 `T=8333.33 µs`、`late (T,1.5T]`、`miss >1.5T`；DoD 用 8340 µs |

A 组另有 4 项"磁盘上无法验证"（面板帧计数寄存器是否存在、S4 固件侧断言、S6 的 Turnip/管线缓存、
LPDDR5 44 GB/s），v2 的处理：前两项**已从规格的判据里移除或标 `[待测]`**；
S6 的预热收益标 `[待测]`；DRAM 带宽标 `[推算]` 并在 §2 表里注明规格来源待补。

### 3.2 B 组（流程）

| # | 发现 | 严重度 | 处置 |
| --- | --- | --- | --- |
| B1 | 四问的第 2、3 问九阶段全缺 | BLOCKER | **采纳**：§6 每阶段固定九行，新增"共享状态（还有谁用）"与"不上机的检查"；§7 增"主机端绿灯"列 |
| B2 | S5 预算算错 12 字节 | BLOCKER | **采纳**：同 A1；另在 §3.4 写明"两条线不得同时飞同一个加参数改动" |
| B3 | 回退物没有生产者 | BLOCKER | **采纳**：新增 §3.3 的四步归档流程，且规定"未归档不许 fastboot" |
| B4 | 评审/批准边界缺失 | BLOCKER | **采纳**：§3.2 写明评审落点、FAIL 拦截范围（构建/flash/status/合并）、每次 flash 都需 owner 批准、S4 另需固件仓评审 |
| B5 | 单变量在 S0/S2a/S3 被破坏 | MAJOR | **采纳**：S0 → S0a/S0b；S2a 整段撤销；S3 → S3a（仪表）/S3b（门） |
| B6 | S4 自相矛盾（vtotal+vfp vs 先只改 vtotal） | MAJOR | **采纳**：S4 不再声称"先只改 vtotal"，改为"先解释 233 行差、读回归档再写"，并保留 I4 为阻塞 |
| B7 | 回归门后果未处理 | MAJOR | **采纳**：§6-S6 增"门的三种后果与处置"；新增约定"新参数只进 `CRITICAL_ARGS`，不进 `baseline.args`；`--write-baseline` 在 DoD 未满足前禁止"（§8 第 9 条） |
| B8 | 跨线冲突无程序 | MAJOR | **采纳**：§3.4 四条程序（先跑门、只认输出、不得同飞、腾位须证无读者） |
| B9 | 记录义务没按阶段落到 area | MAJOR | **采纳**：§6 每阶段"记录"行 + §11 区归属表（含 `network`/`diagnostics` 的既有错配） |
| B10 | 范围与"不碰其他工作线"矛盾、固件仓指错 | MAJOR | **采纳**：§1 增正面声明；§1 非目标改为"不新增/不修改代码与参数（S5a 腾位除外）"；§3.5 指明 `uefi-dby_w09` 而非 `drivers/ShengUfs` |
| B11 | 缺"线上实际进度"与"测量推翻规格" | MINOR | **采纳**：新增 §13 进度表与 §3.8 条款 |
| B12 | S7 表缺固定字段 | MINOR | **采纳**：S7 补齐九行 |
| B13 | DoD 哈希与 evidence 要求不一致 | MINOR | **采纳**：§8 第 7 条要求"每条各成一条 evidence + 三份哈希" |
| B14 | 需求编号 R1–R18 撞名 | MINOR | **采纳**：改 N1–N20，并在 §4 注明与准则无关 |
| B15 | R12 误指门能查"真读者" | MINOR | **采纳**：N13 改为"人工核 + 门只核预算与不丢失"，并引用 `board.toml:349` 反例 |
| B16 | 提交草案无逐阶段映射、S4 外仓 | MINOR | **采纳**：§11 增逐阶段主题表 + S4 外仓说明 |

### 3.3 C 组（技术）

| # | 发现 | 严重度 | 处置 |
| --- | --- | --- | --- |
| C1 | 延迟链闭合不了；S1 只能回收 ~2.8 %（按 C 的口径） | BLOCKER | **部分采纳**：加总本身采纳（新增 `[frame budget]`，DoD 用它）；但 **C 的 53.8 ms 是重复计数**（fence 段已含在 dispatch 内），v2 用 50.8 ms 并在附录 B 写明。S1 的回收上限按 v2 口径是 ~14 %（7.19 / 50.8），仍不足以达标，因此 S2 升为主线 |
| C2 | S1 验收量 method 5 在锁内计时，S1 动不了；idle 在排空路径只会更慢/更失败 | BLOCKER | **采纳**：S1 验收改用 `[gpu submit] fence` 段而非 method 5；S1 只改 `:4833`，排空路径的 clock 明确不动 |
| C3 | 42.26 ms 是锁内 mode-1 排空；23.7 次/s 上限；唯一能修它的 S2b 被关掉；S2a 会让判据空洞通过 | BLOCKER | **采纳**：S2 升为主线；`S2-C`（v1 的上限削减）**明确判否**；S2 给出 A/B 两条路与"两条都不可行则下调目标"的出口 |
| C4 | S4 垂直时序差 233 行，照规格写会得到 ≈130.8 Hz | BLOCKER | **采纳**：§2 增"不自洽"行与 `[推算]` 标注；S4 改为"先解释、读回、再写 `vtotal = vactive_fw + vbp + vsync + vfp`"，I4 升为阻塞 |
| C5 | S5 预算与 `board.toml` 实测矛盾 | BLOCKER | **采纳**：同 A1/B2 |
| C6 | DoD 的 `[panel] fps` 无生产者且违反约束 8 | BLOCKER | **采纳**：删除 `[panel]`，N10 改用 latch 上界；I6 标"已绕开" |
| C7 | `p5` 合成三件事、漏锁等待 | MAJOR | **采纳**：拆为 `p0..p5 + drain + fence + lock`；`lock=` 取既有 `lockTimed` 计时 |
| C8 | `missed` 判据测的不是丢帧，漏掉最大一类 `accepted−flips` | MAJOR | **采纳**：N9 改为三类（`not_shown` / `late` / `miss`），阈值重定义 |
| C9 | 顺序：S3 落在 60 Hz 上会系统性偏高，门自己可能成为上限 | MAJOR | **采纳**：§7 增"S3b 的 late/miss 在 S4 后重算"的说明；S3b 本身验收不含数值阈值 |
| C10 | DoD 的"无 CP 重启/无 VM 毒化"无行可评 | MAJOR | **部分采纳**：DoD 改指 `[gpu recovery]`/`vmfail`，并新增 I8 要求 S0a 先确认这两行是否真的打印；若不打，另立一步 |
| C11 | R5 是自证指标；R6 被 S1 自己违反且 hostsim 的 `IODelay` 是空桩 | MAJOR | **采纳**：N5 改为"相对当前 1.60 ms 改善 ≥5×"的前后对比；N6 的验证手段改为"静态评审 + 上机 5 分钟 soak"，并注明 hostsim `IODelay` 空桩（`XnuMemoryFixture.h:142`）不能检出阻塞 |
| C12 | 42 ms 的备择假设（锁争用 vs 排空）规格区分不了 | MAJOR | **采纳**：`[gpu submit] lock=` 即为区分手段；S0a 的验收加"`lock=` 与 method 5 之差 ≈ drain+fence" |
| C13 | 120 fps 必需的五个量一个都没测 | MAJOR | **部分采纳**：`not_shown`（accepted−flips）与合成器提交节奏（`[kick]`）纳入 S0b；swap→vblank、队列深度、GPU 忙时、渲染线程 CPU 列为"后续可选扩展"，不进 DoD（避免范围膨胀），在 §12 记为 I9 |
| C14 | 7.19/4.49 分母混用 | MINOR | **采纳**：附录 B 同时给出 calls 与 outer waits 两个分母 |
| C15 | `p6=20` 是计数 | MINOR | **采纳**：同 A5 |
| C16 | `switches=`/`drains=` 字段名不一致 | MINOR | **采纳**：统一为 `ttbr_writes`、`drain` |
| C17 | 8.333–8.400 阈值冲突 | MINOR | **采纳**：§5 重定义阈值，DoD 用 8340 µs |
| C18 | I1 被列为阻塞但按 S4 前提它必然已在固件 | MINOR | **采纳**：I1 标"很可能已满足，只需确认并归档" |

---

## 4. 未采纳与保留的差异

| 项 | 评审意见 | 本规格立场 |
| --- | --- | --- |
| C1 的 53.8 ms | 要求把 fence 段与 dispatch 相加 | **不采纳该加法**：fence 段在 `dispatch` 内部（`NativeAdrenoPublish.cpp:674` 的等待发生在锁内），相加即重复计数。DoD 的 `[frame budget]` 会给出正确加总 |
| C13 的五个量 | 要求全部纳入 | **部分采纳**：只纳入 `not_shown` 与提交节奏（零成本、直接影响 120 判据）；其余四个列为 I9 可选，避免把规格扩成诊断工程 |
| B5 的 S2a 拆分 | 要求把"上限"与"错误码"拆两步 | **不采纳拆分，改为整段撤销**：C3 已证明该改动会制造失败并让判据空洞通过，拆开只会多花一次 flash |
| A 组"无法验证"的 S4 固件断言 | 要求可验证 | **保留为 `[待测]`**：固件仓不在本机，无法在规格阶段验证；I4 已把它变成 S4 的前置 |

---

## 5. 第二轮复核（已完成，2026-11-24）

复核结论：**11 FIXED / 3 PARTIAL / 0 NOT FIXED**，并指出 6 处新矛盾。
原文：`.research/gpu-120/reviews/round2-verify.md`。

### 5.1 三个 PARTIAL

| 编号 | 复核意见 | 处置（已落进规格） |
| --- | --- | --- |
| A2 | `:534` 是 pre-latch（可见性在 `:269` 关闭）；默认 `sheng-early-retire=1` 的 GPU 帧走 `:633-639`，**不经过** `:643` | `[frame]` 改为**五处**时间戳：latch `:269`、空白 `:482`、copy `:626`、GPU 提前退休 `:633-639`、GPU 非提前退休 `:643`；并明确 `:534` 不是可见点 |
| B3 | S6 回退要的"上一份 addon"仍无生产者 | §3.3 增 `/root/addons/<sha8>.bin` 归档步骤；I5 从"风险"升为 **S6 的阻塞项**（本机没有该文件就不许开工） |
| C2 | `:4833` 的 clock 对象会一路进入 `Ring::wait(:622)`，排空也会变，v2 的"drain 不变"不成立 | S1 验收改为"fence 与 drain **都**下降；但次数类计数（`drains`、`ttbr_writes`）必须**完全不变**"——用计数而非时长判"有没有改到控制流" |

### 5.2 六处新矛盾

| 编号 | 矛盾 | 处置 |
| --- | --- | --- |
| 1 | `lockWait` 不是常驻数组，而是 `AGXDeviceUserClient::run()` 的局部 `lockClock`（`Publish.cpp:751`），`:758` 消费一次即失效 | S0a 要求在 `:751-763` 新增 `native_adreno_lock_wait_ticks[2]` 累加器；规格里"常驻数组"的说法已删 |
| 2 | S1 在排空路径上的效果与"drain 不变"冲突 | 同 C2 |
| 3 | S2-B 只把排空移出锁，`waitAll` 时长不变，却要求 `drain < 1 ms` | S2-B 验收改为看 `lock=`/锁持有时间，明确"**不看** drain" |
| 4 | `[frame] p50=16667` 与附录 B 的 19.7 fps、以及 118 flips 的同一窗口冲突 | `[frame]` 只在连续动画窗口（`n ≥ 50/s`）判定；附录 B 注明 19.7 fps 是**上限推算**，不等于实测帧率 |
| 5 | 把"fence 段"当成 method 5 的组成 → 重复计数 | `[gpu submit]` 删除 `fence=`；附录 B 改为"两个不同计数器、不可相加/相减"；S0a 新增 `Submit::wait` 的**调用者标签**（drain / display / client） |
| 6 | "`lock=` 与 method 5 之差 ≈ drain+fence"的判据不成立 | S0a 判据改为：`lock=` < 10 % 且 `drain` 占 `p5` 主要部分 ⇒ 锁内排空；`lock=` 很大 ⇒ 锁争用（两者修法相反） |

### 5.3 复核确认的正面事实

- 字节预算：HEAD 983 B / 工作树 995 B / `sheng-vsync=120` 16 B，与门的常量
  （`ARG_LIMIT_BYTES=1000`、`" ".join`）一致——v2 的参考值正确；
- `useVm` 每帧都会进入，但 `waitAll` 只在 `table_mode_==1 && vm != table_vm_ && !drained`
  时执行（`:221-230`）——v2 已写对，可直接实施；
- 四个 `noteFrameVisible` 候选位置都真实存在（`482/534/626/643`），只是 `:534` 的语义要纠正、
  且需要补 `:633-639`。

### 5.4 第三轮（待执行，范围只有一条）

第三轮只核一件事：**五处 `noteFrameVisible` 的时间戳与 `[frame budget]` 的起点必须成对**
（预算 = 提交时刻 → 可见时刻），且五处之间不重复计数。其余 14 个 BLOCKER 已闭环。

---

## 6. 说明

- 本记录只登记"评审说了什么、规格怎么处置"；技术事实的最终依据仍是代码与日志，
  每条处置都能在 v2 与三个评审文件之间对上；
- 三个评审都由独立子代理完成，互不可见，因此"三个轴独立发现同一问题"是可信度信号；
- v1 保留在 git 历史里（未提交，本文件与 v2 一起入库时会形成对照）；
- 本记录**不代替** owner 批准：任何一次 flash 仍需 owner 在 `[[area.evidence]]` 里点名。
