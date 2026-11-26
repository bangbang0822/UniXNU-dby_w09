# 充电（快充）—— 开发规格 spec

- 日期：2026-11-24
- 对象：dby_w09（华为 MatePad 11，SM8250 / kona，PM8150B + SMB5），iPadOS 17.7.11
- Status: **draft for review（未实现、未构建、未上机）**
- 依据：`package/DbyBattery.cpp`（105 KB，只读实现 + ICL 梯子）、`porting.toml` `area = "power"`、
  厂商设备树 `research/dby/kernel/dts__sm8250-huawei-dby-w09.dts`
- owner 已给的三条输入：**原装 40W 超级快充；现状"能充但很慢"；目标净电流 ≥ +1500 mA；
  写边界只允许 ICL / 充电使能**

---

## 1. 范围

让设备**插着原装 40W 充电器、系统正在运行**时，以明显快于现状的电流充电：
**连续 60 s 净充电电流 ≥ +1500 mA**（电量确实上升，而不是只在"慢慢充"）。

**本轮不包含**（明确写下，避免把沉默读成意图）：

1. **华为 SCP 10V 超级快充协议**：本地全部材料（厂商 DTS、`_dby.txt`、工作区）搜
   `sc8562 / sc8547 / hi6526 / hw_scp / direct_charge / super_charge / charge_pump` **零命中**，
   设备树里只有 PM8150B SMB5 + typec + vbus。缺协议实现与快充 IC 证据，本规格不做；
2. **HVDCP 9V/12V**：`package/DbyBattery.cpp:323-325` 明确写了 3000000 mA 档"只有在 reglit 谈成 9V/12V 后才合法"，
   而本轮写边界不含该协商；
3. 浮动电压、充电电流上限（FCC）、看门狗（bark/bite）、温控降流——**均不在允许的写集合内**；
4. 无线充电、反向充电、旁路供电（bypass）。

---

## 2. 事实基础（每条标 `[实测]`/`[代码]`/`[文档]`）

| 项 | 值 | 标注 | 来源 |
| --- | --- | --- | --- |
| 充电器 IC | PM8150B 内的 SMB5（`qcom,qpnp-smb5`），USID 2 | `[文档]` | 厂商 DTS `charger@1000` |
| 充电器中断 | `chg-state-change`、`usbin-plugin`、**`wdog-bark`** | `[文档]` | 同上 `interrupt-names` |
| 电量计 | gen4，`battery@4000`，设计容量 7,150,000 µAh = 7150 mAh | `[文档]` | 同上 `charge-full-design-microamp-hours` |
| Type-C | `pm8150b-typec`，sink 状态字节可读（`0x06`） | `[代码]` | `DbyBattery.cpp:215-221` |
| 厂商 ICL 表（Huawei 隔离分支） | **SDP 500 / FLOAT 1200 / CDP 1500 / DCP 2300 mA** | `[代码]` | `DbyBattery.cpp:311-332` |
| 900 mA 档为何没有 | 本板 DWC3 固定 HighSpeed，DT 未设 Rp → USB-2 主机口上限 500 mA | `[代码]` | `:318-323`、`package/QcomXnuUsbDebug/usb/Dwc3.c:533-534` |
| HVDCP 3000 mA 为何没有 | 需先谈成 9V/12V，本轮不做 | `[代码]` | `:323-325` |
| 现有能力 | `dby-battery=1` 只读；`dby-battery=2` **额外写 ICL**（梯子：idle→measure→ceiling→apply→settle→verify→hold，失败回 `HW_AUTO`） | `[代码]` | `:59-66`、`:503-520`、`:768-830` |
| 现有 ICL 上限逻辑 | 以 `ICL_MAX_STATUS` / `AICL_ICL_STATUS` 的较大者为 ceiling，再在表里取 ≤ ceiling 的最大档 | `[代码]` | `:775-787`、`:815-822` |
| 当前命令行 | `dby-battery=2` **已经在** `board.toml [payload] kernel_args` 里 | `[实测]` | `payload.json`（56 参数 / 995 B） |
| 实测净电流 | `ma=+422`（充着电、`chg=1`） | `[实测]` | `porting.toml:762`（payload `f2-payload-168`） |
| 电脑口实测 | `icl=500 want=500 ceiling=500 aicl0=500` → SDP | `[实测]` | `porting.toml:697` |
| 电池容量与时长 | 7150 mAh ÷ 422 mA ≈ **16.9 h**（0→100% 的量级，与"很慢"一致） | `[推算]` | 计算 |

**结论**：目标 1500 mA 的**唯一前提**是把输入从 SDP 500 抬到 DCP 2300。
但本规格起草后查到两条独立证据，说明**这件事不能靠"硬抬 ICL"完成**（见 §2.2）：

1. 端口类型在本板**不由 PMIC 的 APSD 决定**——厂商设备树设了 `usb-thd-bc-12`，
   `smblib_apsd_enable()` 故意清掉 `BC1P2_SRC_DETECT_BIT`，适配器类型改由 **FSA9685** 这颗开关芯片给出
   （`DbyBattery.cpp:287-291`）；
2. 2026-11-06 已经试过"自由梯子直接 arm DCP 2300"：PMIC 状态字节仍报 500，
   **IBAT 从 want=1200 时的 +548 mA 掉到 want=2300 时的 −861 mA**——输入塌陷，电量反而在掉
   （`DbyBattery.cpp:764-768`）。

所以 §2.2 才是本轮真正的入口。

### 2.2 为什么现在只到 500 mA（两条独立证据）

| 证据 | 内容 | 来源 |
| --- | --- | --- |
| **A：端口检测不在 PMIC** | `kona/DBY/common/huawei_charger.dtsi:147` 设 `usb-thd-bc-12` → `smblib_apsd_enable()` 清 `BC1P2_SRC_DETECT_BIT`；适配器类型从 **FSA9685** 来（`qpnp-smb5.c:3390-3414`）。本镜像**不驱动 FSA9685**，所以没有任何东西告诉 PMIC"这是 DCP"，ICL 停在 SDP 500 | `DbyBattery.cpp:287-291` |
| **B：硬抬已验证失败** | 自由梯子 arm 到 2300 mA 时 IBAT 由 +548 变 **−861 mA**（输入塌陷，VCL 把电流压到 0，电池倒贴系统负载） | `DbyBattery.cpp:764-768`（2026-11-06 实测） |
| **C：同机的另一个 Linux 移植** | 同一台 DBY-W09 的 Linux 移植把快充列为**未实现**，缺的正是 **SCP 直充框架 + FSA9685 + RT9759** 驱动；它的普通充电可用，并**定时喂 SMB5 看门狗** | `research/dby/cowave/docs__status.md:39-43`、`:78-81` |

**推论**：本板要越过 500 mA，必须**先把端口类型读出来**（FSA9685，I2C 只读），
再按读到的类型 arm ICL（这是允许的写）。这仍在本轮写集合内（只写 ICL），
但**新增一个依赖：FSA9685 的 I2C 地址与寄存器映射**，本仓库没有。

### 2.3 关键算术：1500 mA 净电流可达吗
| 项 | 数字 | 说明 |
| --- | --- | --- |
| DCP 档输入 | 2300 mA @ 5 V = **11.5 W** | 表内最高、本轮唯一能合法到达的档 |
| 系统功耗（亮屏中负载） | 约 3–6 W | `[推算]`，需 P0 实测 |
| 可进电池的功率 | 约 5.5–8.5 W | 11.5 − 系统 |
| 折算到电池侧（3.9 V） | 约 **+1400…+2200 mA** | 所以 1500 mA **可达，但余量薄** |
| 若仍在 SDP（500 mA @5V=2.5W） | 约 +0…+400 mA | 与实测 +422 mA 吻合 |

**结论**：目标 1500 mA 的**唯一前提**是把输入从 SDP 500 抬到 DCP 2300。
这条路不碰任何被禁止的寄存器——但有两个可能的绑定点：

1. **APSD 是否把 40W 充电器识别成 DCP**（若识别成 SDP/Unknown，ceiling 就停在 500，梯子永远不往上走）；
2. **FCC（充电电流上限）是否被固件限在小值**（若 FCC ≈ 1 A，输入再大也进不了电池，而写 FCC 不在允许集合内）。

P0 的读数就是来回答这两条；P1 之后若仍达不到，就**停在本规格**并请 owner 追加授权，而不是偷偷写。

---

## 3. 约束

1. **写集合**：只允许写 **USBIN 输入电流相关寄存器（ICL）与充电使能**；
   不写浮动电压、FCC、看门狗、typec/HVDCP、温控。任何超出集合的写都必须先改本规格并获得 owner 批准。
2. **一次一变量**：每阶段一次 flash；观测与机制分两次（P0 纯观测）。
3. **先评审后构建**：`.research/charging/reviews/<stage>.md`，FAIL 拦构建 + flash + status 推进；
   每次 flash 需 owner 点名批准。
4. **回退物**：flash 前归档当前 payload 到 `/root/payloads/<sha8>.img` 并记 sha256；
   回退 = 刷回该镜像。SMB5 的写全部有 `HW_AUTO` 回滚路径（`:718-730`），任何失败都必须走它。
5. **不读未上电的块、不在没有 ceiling 的情况下 arm**：现有代码的"先测后 arm、只 arm 表内值"的栅栏**保留**，
   本规格不得为了快而放宽它。
6. **参数预算**：本规格**不新增 boot arg**（`dby-battery=2` 已存在），预算不动。
7. **记录**：每阶段更新 `porting.toml` `area = "power"` 的 findings/evidence；达标前不改 `device-verified`。

---

## 4. 需求（N 前缀，与工程准则 R1–R10 无关）

| ID | 需求 | 判定方式 |
| --- | --- | --- |
| **N1** | 充电器状态必须可读且可归因：端口类型、ceiling、已 arm 的 ICL、FCC、stage | 新增 `[charger]` 行 |
| **N2** | 端口类型必须来自寄存器，不能按"插了线"猜 | `[charger] port=` 来自 APSD result |
| **N3** | 净电流必须用 FG 实测，不推算 | `[battery] ma=` 与 `[charger]` 同时打印 |
| **N4** | 只有"ceiling ≥ 档位"且"该档在厂商表内"时才 arm | 代码路径 + 行里 `want ≤ ceiling` |
| **N5** | arm 失败或硬件拒绝（COLLAPSE/UV）必须回 `HW_AUTO` 并计数 | `[charger] rollback=` |
| **N6** | DCP 识别成功时，必须能 arm 到 DCP 档（2300 mA） | `[charger] step=dcp` |
| **N7** | 达标判据：连续 60 s `ma ≥ +1500`，且电量百分比上升 | `[battery]` 前后各一次 |
| **N8** | 若 FCC 成为绑定点，必须**测出来并报告**，不得改写它 | `[charger] fcc=` 与结论行 |
| **N9** | 若 APSD 把墙充识别成 SDP，必须报告而不是硬抬 ICL | `[charger] port=sdp wall=1` |
| **N10** | 不得新增 boot arg；不得改变无 `dby-battery` 时的行为 | 参数表不变 + 缺省路径早退 |
| **N11** | 每阶段归档与记录齐全 | `/root/payloads/` + `porting.toml` |
| **N12** | 所有结论区分 `[实测]`/`[推算]` | 记录抽查 |

---

## 5. 行语法

现有 `[battery]` 行不变（`BootLog.c:1291`）：
```
[battery] st=3 pct=58 mv=3846 ma=+422 temp=0 fl=... usb=1 full=0 ...
```

新增一行（新增观测，不改行为）：
```
[charger] port=sdp apsd=11 res=1 il=0 cfg=-1 iclmax=-1 applied=-1 want=500 flt=-1 susp=0 rt=10 stage=7 en=1 ma=562 step=0 stv=0 ev=2
  (生产者：package/DbyBattery.cpp 的 kprintf，每 10 个 2 s 周期一行；不属于 BootLog 的一次性文本块)
```

| 字段 | 来源（Sample 成员，全部已在读） | 语义 |
| --- | --- | --- |
| `port` | `apsdName(s.apsd, s.apsr)` | sdp/cdp/dcp/ocp/float/hvdcp2/hvdcp3/unknown/not-done |
| `apsd` / `res` | `APSD_STATUS_REG` / `APSD_RESULT_STATUS` | 原始状态字节与结果字节 |
| `il` / `cfg` / `iclmax` / `applied` | `USBIN_CURRENT_LIMIT_CFG`、`ICL_MAX_STATUS`、已 arm 的值 | 输入电流的上限、回读、实测 |
| `want` | 梯子当前档（`kIclLadder[iclStep]`） | 想 arm 的值 |
| `flt` | `FLOAT_VOLTAGE_CFG` | 只读；被压低时快充也会被限 |
| `susp` / `rt` | `USBIN_CMD_IL` 的 suspend 位 / `USBIN_RT_STS` | 输入是否挂起、实时状态 |
| `stage` / `en` | `BATTERY_CHARGER_STATUS_1` / 使能镜像 | 充电阶段与使能 |
| `ma` | 电量计（FG） | **唯一的第三方判据**：正=进电池、负=在放电 |
| `step` / `stv` / `ev` | `iclStep` / `iclStarveTicks` / `iclStarveEvents` | 当前档位、饥饿窗口计数、累计降档/放弃次数 |

限流：每 10 个 2 s 周期一行（约 20 s），由驱动 kprintf 打印。
`fcc`（`CHARGING_CURRENT_LIMIT_CFG`）**没有进这一行**：偏移无法从本地材料核实（不动没把握的寄存器），
它是否成为绑定点由 `ma` 与 `step` 的对照间接判断。

---

## 6. 阶段

每阶段固定字段：**改动 / 行为变化 / 共享状态 / 不上机的检查 / 依赖 / 验收 / 记录 / 回退 / 风险**。

### P0 — 充电器普查（只读）

| 项 | 内容 |
| --- | --- |
| 改动 | `package/DbyBattery.cpp`：读 APSD_STATUS/RESULT、`ICL_MAX_STATUS`、`AICL_ICL_STATUS`、`USBIN_CURRENT_LIMIT_CFG`、`USBIN_CMD_IL`、`FLOAT_VOLTAGE_CFG`、`CHARGING_CURRENT_LIMIT_CFG`（只读）、`BATTERY_CHARGER_STATUS_1`、`USBIN_VOLTAGE_STATUS`；`BootLog.c` 加 `[charger]` 行。**不写任何寄存器** |
| 行为变化 | 无 |
| 共享状态 | 与现有 ICL 梯子共用同一组 SPMI 读；不新增映射；`native_battery_stats[]` 布局不动（新数据走独立数组） |
| 不上机的检查 | `build kernel` + `check-repository.py`；寄存器偏移与位定义逐条对照 `DbyBattery.cpp` 已有的 `smb5-reg.h` 引用行号；FCC 偏移若无法从本地材料核实，**该字段打印 `?` 而不是编一个** |
| 依赖 | 无 |
| 验收 | `[charger]` 出现；`port` 有名字；`ceiling`/`armed`/`cfg` 与 `[battery] ma=` 自洽（SDP 时 ma≈+0…+500，DCP 时 ma 明显更高） |
| 记录 | `power` 区 1 条 findings + 1 条 evidence |
| 回退 | 上一份 payload |
| 风险 | 低（只读）；唯一风险是 FCC 偏移猜错——所以要求"核不到就打 `?`" |

### P1 — 用 APSD 类型修正 ceiling/选档（只写 ICL）

> **⚠ 2026-11-24 修订：本节原方案已被 §2.2-B 推翻。** 原来的写法是"APSD 报 DCP 时即使 ceiling 报 500 也直接 arm 2300"，
> 而 2026-11-06 的实测正是这个做法：IBAT 从 +548 mA 掉到 **−861 mA**。所以 P1 现在改成**只按真实检测来源选档**，
> 并且**必须先有 §2.2-A 的 FSA9685 读数**（P0 负责），否则 P1 不启动。

| 项 | 内容 |
| --- | --- |
| 改动 | ①P0 的 FSA9685 读数给出 `port`；②`voteForCeiling()` 只在 `port==dcp` 且 ceiling ≥ 该档时才 arm（**保持现有的"不爬梯"栅栏**，`DbyBattery.cpp:764-768`）；③若 `port==dcp` 而 ceiling 仍报 500，**不 arm**，改为打印一行"port 与 ceiling 冲突"并把结论留给 P2 |
| 行为变化 | 是（写 ICL，但只在有真实检测依据时） |
| 共享状态 | 现有梯子状态机与 `HW_AUTO` 回滚；FSA9685 的 I2C 总线（读） |
| 不上机的检查 | 真值表：`port=dcp,ceiling=2300` → arm 2300；`port=dcp,ceiling=500` → 不 arm 且打冲突行；`port=sdp` → 500；`port=unknown` → 不动 |
| 依赖 | P0，且 **I6（FSA9685 映射）已解** |
| 验收 | `[charger] port=dcp step=dcp armed=2300` 且 `[battery] ma ≥ +1500`、`rollback=0`；若走冲突分支，必须打印 `conflict=1` |
| 记录 / 回退 / 风险 | 同前；风险从"中"降为"低"，因为不再在无依据时抬 ICL |

### P2 — 若墙充被识别成 SDP（**条件阶段**）

只有 P0/P1 显示 `port=sdp wall=1`（插着墙充却报 SDP）时才做。可选手段按风险排序：

1. **不做任何写**，只把结论记下来（`limits`：本板在 SMB5 的 APSD 下拿不到 DCP）；
2. 请求 owner 追加授权后 **rerun APSD**（`CMD_APSD_REG` 的 `APSD_RERUN_BIT`，现有代码**故意**从不写这一位，`:287-292`）；
3. 请求 owner 追加授权后 **HVDCP 9V**（是另一条独立的规格）。

### P3 — 达标验证或停在结论

| 项 | 内容 |
| --- | --- |
| 改动 | 无代码（除非 P1 需要调时序）；新增 `scripts/check-charging.py` 读串口日志判 N7 |
| 验收 | 连续 60 s `ma ≥ +1500` 且 `pct` 不再下降；若达不到，`[charger]` 必须指出绑定点（`port` / `ceiling` / `fcc`） |
| 记录 | 达标 → `power` 区 evidence（payload/kernel sha、ma 曲线）；不达标 → `limits` 写明绑定点 |
| 回退 | — |
| 风险 | 低（工具） |

---

## 7. 验收速查

| 阶段 | 必须出现的行 | 通过判定 | 主机端绿灯 | 失败的含义 |
| --- | --- | --- | --- | --- |
| P0 | `[charger]` | `port` 有名、`ceiling/armed/cfg` 自洽 | build=0 + hash；`check-repository.py` | 寄存器读不通 → 先修读 |
| P1 | `[charger]` `[battery]` | `step=dcp armed=2300`、`ma ≥ +1500`、`rollback=0` | 真值表 + build | `port!=dcp` → 走 P2；`ma` 不升 → 绑定点是 FCC（N8） |
| P2 | `[charger]` | 要么记 `limits`，要么拿到授权后 DCP 成功 | — | 无授权即停 |
| P3 | 脚本 | 60 s `ma ≥ +1500` | — | 按绑定点回对应阶段 |

## 8. 完成定义

1. 插原装 40W 充电器、亮屏使用时，连续 60 s `[battery] ma ≥ +1500`，`pct` 上升；
2. `[charger]` 显示 `port=dcp step=dcp armed=2300 rollback=0`，且 `cfg` 回读与 `armed` 一致；
3. 无新增 boot arg；`dby-battery` 缺省时行为与现在完全一致；
4. 写集合未越界（代码审查逐条核对：本轮只出现 ICL/使能相关写）；
5. `porting.toml` `power` 区更新，所有 `[预测]` 改 `[实测]`，未达成项进 `limits`；
6. payload/kernel sha256 与 60 s 电流记录各成一条 evidence。

## 9. 回退

| 阶段 | 回退物 | 回退后 |
| --- | --- | --- |
| P0 | 上一份 payload | 无变化（只读） |
| P1 | 上一份 payload；或运行时 `HW_AUTO` 回滚 | 回到 500 mA 级慢充 |
| P2 | 不写即回退 | 保持现状 |
| P3 | 删脚本 | 不影响映像 |

**立即回退判据**：输入 GVBS 掉压（UV）连续出现、`pct` 下降、SMB5 报 OV/OT、设备重启。

## 10. 风险

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 对 5V 源 arm 2300 mA 过抽 | VBUS 塌陷、掉电重启 | 只在 `port==dcp` 时启用；arm 后 1 tick 验证；COLLAPSE/UV 立即 `HW_AUTO` |
| APSD 报 DCP 但适配器实际只能给 1 A（非标线/劣质线） | 同上 | 电流不升即回滚并计数，不反复重试 |
| FCC 被固件限死 | 目标达不到 | 只报告不改写（N8），请 owner 决策 |
| 写错寄存器偏移（FCC 只读也一样） | 可能触发未知行为 | 核不到偏移就不读/打 `?`；写集合仅限已有代码验证过的 ICL 寄存器 |
| 看门狗在不喂的情况下 bite | 充电中断 | 本轮不写看门狗；P0 读 `wdog-bark` 状态并记录；若确认 bite，需要 owner 追加授权 |
| 温度和快充叠加 | 过热 | 本轮不做温控（不在写集合），P3 记录 `temp`；超过 45 ℃ 由 owner 决定是否停 |

## 11. 未决输入

| ID | 未决 | 阻塞 | 需要谁 |
| --- | --- | --- | --- |
| **I1** | `CHARGING_CURRENT_LIMIT_CFG`（FCC）的寄存器偏移与解码（厂商 `smb5-reg.h` 不在本仓库） | 只有 N8 的报告精度 | 厂商内核源码 / 已解包的 PMIC 寄存器表 |
| **I2** | 这台机器的 40W 适配器在 SMB5 的 APSD 下被识别成什么（DCP / SDP / Unknown） | P1 是否可行、是否要走 P2 | **P0 一次上机即可回答** |
| **I3** | APSD rerun 是否算"检测"还是"写配置" | P2-2 | owner 决定 |
| **I4** | FCC 当前值 | 目标能否达成 | P0 读到（若 I1 解决） |
| **I5** | 0→100% 现状耗时（owner 只说"特别慢"） | 只影响记录，不影响判据 | owner 可选补充 |
| **I6** | **FSA9685 的 I2C 地址与寄存器映射** | ~~阻塞~~ **已解决（见 §12.07）** | 华为自己的驱动 `switch_fsa9685.c` + `switch_chip.h`（GPL，LineageOS 镜像）与 DTS 给出了**地址 0x26、寄存器 0x08 = DEVICE_TYPE_1、bit5 CDP / bit6 DCP** |
| **I7** | **RT9759（电荷泵/直充 IC）是否在位**以及其映射 | 只有 SCP 10V 快充（本轮非目标） | 同上 |
| **I8** | SMB5 看门狗是否被固件使能（Linux 移植"定时喂狗"，我们不喂） | 若不喂会 bite，快充会被关断 | P0 读 `wdog-bark` 状态即可判断；使能位写入不在授权内 |

## 12. 进度

### 12.0 P0 实测（2026-11-25 19:4x，电脑 USB 口，不是 40W 充电器）

`[charger]` 行已工作（驱动 kprintf，每 10 个 2 s 周期一行）：

```
[charger] port=sdp apsd=1 res=1 il=0 cfg=-1 iclmax=-1 applied=-1 want=1500 flt=-1 susp=0 rt=10 stage=3 en=1 ma=-575
DbyBattery: icl apply step=2 name=cdp want=1500 ceiling=1500 apsd=sdp writes=5 ok=5 failed=0
DbyBattery: icl holding want=1500 (rt=0x10) ceiling=1500 ma=-666
DbyBattery: #52 ... aprd=0x01 step=2 good=2 ph=6 ceiling=1500 aicl0=1500 ib+=0 ib-=52
```

三条结论：

1. **端口类型读到了**：`res=0x01` → SDP（电脑口），`port=sdp`；
2. **梯子仍 arm 了 1500 mA**（`step=2 name=cdp`），因为 `ceiling` 读回的是它**自己写进去的 override 回声**（`aicl0=1500` 同理）——2026-11-06 的陷阱换了位置：不是没 ceiling，而是 ceiling 被自己骗了；
3. **净放电**：`ma` −490…−666 mA，`ib+=0`、`ib-=52`，每周期都为负。

**给 P1 的明确修法（仍在授权内）**：在 COLLAPSE/UV 之外，用**电量计实测电流**当第二判据——插电而 IBAT 连续 N 周期为负就降一档，降到 SDP 仍为负就回 `HW_AUTO` 并停。只写 ICL、只读 FG。

| 阶段 | 状态 | 决定它的读数 |
| --- | --- | --- |
| P0 | **已上机**（内核 `474b7f47c50fca5d…`，payload `84bca9c8…`） | `[charger]` 已出；SDP 下 arm CDP → IBAT 全负 |
| P1 | 待做（依据本条实测） | — |
| P2 | 条件阶段 | — |
| P3 | 未实现 | — |

### 12.05 P1 实现与评审（2026-11-25 20:0x）

`DbyBattery.cpp` 的输入电流梯子增加**第二个判据**：除了 PMIC 的 COLLAPSE/UV，再用**电量计实测 IBAT**。
只写 ICL、只读电量计，仍在写集合内。

**评审结论：FAIL → 已修**（原文 `.research/charging/reviews/p1-ibat.md`，2 BLOCKER / 3 MAJOR / 3 MINOR）：

| 评审项 | 处置 |
| --- | --- |
| B1 `s.ma == -1`（读失败）被当成"没在充" | 加 `has(s,kFieldI)` 门槛 |
| B2 taper/terminate/充电被禁/挂起/过冷过热被当成"没在充" | 加 `trying` 门槛（`en!=0 && !suspend && !cold && !hot && stage∉{Taper,Terminate}`） |
| M1 拔线不复位 | 拔线时清 `iclStarveTicks/iclCeiling/iclStep/iclGood/iclLocked`（换线重新判定） |
| M2 窗口从"插线"而非"arm"起算 | arm 成功分支清窗口 |
| M3 只降不升、lock 永不复位 | 由 M1 的拔线复位解决（同一个源内仍只降，避免回声上抬） |
| m1 饥饿分支抢占 refusal 判定 | 加 `!refused &&` |
| m2 ceiling 下落但票不变得不到 return | 改为无条件 `return` |
| m3 `[charger]` 行没有饥饿状态 | 行尾加 `step= stv= ev=` |

**产物**：`aa6a2fce…` 是 §12.06 那次上机用的镜像（**已刷**）；按 §12.05 的评审修完之后又重建了一份
（kernel `ab27a09a42f3793c…`、payload `bd2e2f27…`，归档 `/root/payloads/bd2e2f27.img`），**这一份尚未刷机**。
两版的参数都是 56 / 995 B，未变。

### 12.06 P1 上机结果（2026-11-25 19:5x，电脑 USB 口，**决定性**）

刷入 `aa6a2fce` 后，同一台设备、同一个 500 mA 电脑口：

```
DbyBattery: icl starved at 1500 mA (ma=29) for 15 cycles, re-voting 1200 (events=1)
DbyBattery: icl apply step=1 name=float want=1200 ceiling=1200 apsd=sdp ... failed=0
DbyBattery: icl holding want=1200 (rt=0x10) ceiling=1200 ma=31
DbyBattery: icl starved at 1200 mA (ma=31) for 15 cycles, re-voting 500 (events=2)
DbyBattery: icl apply step=0 name=sdp want=500 ceiling=500 apsd=sdp ... failed=0
DbyBattery: icl holding want=500 (rt=0x10) ceiling=500 ma=562
[charger] port=sdp apsd=11 res=1 want=500 ma=561 step=0 stv=0 ev=2
```

**结论（实测）**：
1. 判定链按设计工作：`1500 → 1200 → 500`，每次都是 15 个周期（30 s）窗口关闭后降一档，`ev=2` 记了两次；
2. **降到 SDP 500 mA 后，电池拿到 +562 mA** —— 这是 500 mA 源能给的最好结果
   （输入 500 mA ≈ 2.5 W，电池 0.56 A × 3.9 V ≈ 2.2 W，账对得上）；
3. **同一端口上，P0b 时是 +29 mA，现在是 +562 mA，约 19 倍**。之前"特别慢"的直接原因就是
   "在 SDP 口上 arm 了 1500"，不是充电器不行。

于是"电脑口能不能充"这一问已经闭环。**剩下的关键一问只有 40W 充电器**：它会报 DCP 吗？
如果报，梯子会（在同一窗口判据下）稳在更高档；如果仍报 SDP，那就走 §12.07 的 FSA9685。

### 12.07 I6 已解决：FSA9685 的地址与寄存器映射（2026-11-25 20:1x）

从华为自己的 GPL 驱动与设备树取得（`LineageOS/android_kernel_huawei_kiwi`）：

- 驱动：`drivers/usb/switch/switch_fsa9685.c`（`CONFIG_SWITCH_FSA9685`，`huawei_charger.c` 引用它）
- 寄存器表：`drivers/usb/switch/switch_chip.h`
- 设备树：`arch/arm/boot/dts/qcom/kiwi/huawei-usb.dtsi` → `fsa9685@26 { reg = <0x26>; }`

**I2C 地址 0x26，SMBus byte-data**。寄存器（头文件十进制 → 十六进制）：

| 寄存器 | 地址 | 用途 |
| --- | --- | --- |
| DEVICE_ID | 0x01 | 芯片识别 |
| CONTROL | 0x02 | bit0 INT_MASK、bit2 MANUAL_SW |
| INTERRUPT | 0x03 | bit0 ATTACH、bit1 DETACH、bit5 VBUS_CHANGE |
| INTERRUPT_MASK | 0x04 | — |
| ADC | 0x05 | ID 电阻 ADC |
| DETACH_CONTROL | 0x07 | 手动 detach |
| **DEVICE_TYPE_1** | **0x08** | **bit0 FC_USB、bit2 USB(SDP)、bit5 CDP、bit6 DCP、bit7 USBOTG** |
| DEVICE_TYPE_2 | 0x09 | bit2 JIG_UART |
| DEVICE_TYPE_3 | 0x0A | 附件（bit5/6/7、bit0 FM8） |
| MANUAL_SW_1/2 | 0x0B / 0x0C | 手动开关 |
| CONTROL_2 | 0x0E | DCD_TIMEOUT 等 |
| VBUS_STATUS | 0x1B | **bit1 VBUS_VALID** |
| DCD | 0x1F | DCD 配置 |

**这对 P2 意味着什么**：读 0x26 的 **0x08**，bit6=1 ⇒ DCP、bit5=1 ⇒ CDP、bit2=1 ⇒ SDP。
这是一个**纯读**操作（不写任何寄存器），因此完全在授权内；拿到类型后 P1 的梯子才有可靠依据去 arm 2300。
剩下的工程问题是**这颗芯片挂在哪条 I2C 总线上**（华为那台是 `i2c@78b8000`，本板需从自己的 DTBO 或逐 QUP 探测确认），
以及本板 `GeniI2c`（背光已在 se11 上用）能否直接复用到那条总线。

### 12.1 P0 的实现（2026-11-24 23:46）
| 项 | 值 |
| --- | --- |
| 改动文件 | `package/DbyBattery.cpp`（新增 `native_battery_charger[16]`、`portCode()`、在 `publish()` 里填充）、`package/BootLog.c`（新增 `[charger]` 行 + 弱 extern） |
| 读的东西 | 全部是**该文件本来就已读**的字节：APSD status/result、IL、ICL cfg 回读、ICL max、applied ICL、want、float、suspend、USBIN RT、stage、enable；**无新寄存器访问、无写** |
| 内核 | `kernel.macho` sha256 `ce344812c9aee057f16b400e6b20695782d1e92a51264d3d0715ebd7a7f79567` |
| payload | 218,103,808 B，sha256 `89f0a0582c0fe304fad98b9f863401d9a8b59dcb60f243913b43eaa3452ca30f`，归档 `/root/payloads/89f0a058.img` |
| 参数 | 56 / 995 B **未变**（`dby-battery=2` 本来就在），payload 三份输入与已刷态一致 |
| 镜像校验 | `native_battery_charger` 符号存在 ×1；`[charger] port=` 格式串存在 ×1 |
| 状态 | `aa6a2fce` 已刷并留下 §12.06 的实测；当前构建 `bd2e2f27` 尚未刷机 |

**上机协议（重要）**：本板只有一个 USB-C 口，串口控制台要占用它，40W 充电器也要占用它——**两者不能同时插**。
所以测量分三步：

1. 插 PC → `fastboot flash preload /root/payloads/89f0a058.img`；
2. **拔掉 PC，插 40W 充电器**，让系统跑 2–3 分钟（`[charger]`/`[battery]` 行按 2 s/60 s 节奏进内核日志缓冲）；
3. 再插回 PC 抓串口：`sheng-log-replay=1` 会把**上一次引导**的行以 `[prev]` 形式重放出来，从中读
   `[charger]` 与 `[battery]`。

**P0 的判据**：
- `port=` 若是 `sdp`/`unknown` 而充电器确实插着 → 证实 §2.2-A（PMIC 拿不到类型），P1 必须先解决 FSA9685；
- `port=dcp` 而 `iclmax`/`il` 仍是 500 → 端口类型有、上限没跟上，P1 的"按类型 arm"就有依据；
- `port=dcp` 且 `iclmax ≥ 2300` → 现在的 +422 mA 另有原因，回到 `[battery] ma` 与 `want` 对比。

---

## 附录：为什么不是"SMB5 直接上 9V/12V"

`DbyBattery.cpp:323-325` 的原因写得很清楚：HVDCP 的 3000000 mA 档"只有在 reglit 谈成 9 V 或 12 V 之后才合法，
而这次引导没有任何东西驱动那个协商；往 `0x1370` 写 60 就是向 5 V 源要 3 A"。
本轮写边界不含 HVDCP/typec，所以 2300 mA @ 5 V 是能合法到达的上限。

**但 2300 mA 也不是无条件可达**：本板 PMIC 的 BC1.2 检测被厂商关掉了（`usb-thd-bc-12`），
端口类型由 FSA9685 给出，而我们不驱动它；2026-11-06 硬 arm 2300 的实测结果是 IBAT 变负（−861 mA）。
所以真正的前置是**先把端口类型读出来**（FSA9685，只读 I2C），再按类型 arm。
这一步需要 FSA9685 的地址/寄存器映射（§11-I6），是本轮唯一的外部输入。
