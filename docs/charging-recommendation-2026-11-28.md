# DBY-W09 充电 / USB-PD 路线建议

- 日期：2026-11-28
- 适用范围：UniXNU-dby_w09（SM8250 / PM8150B，iPadOS 17.7.11 21H461）
- 依据：本仓四份充电文档、同机 Linux `dby-chg`（pin `3d82fb7d`）、UniXNU 组件目录与本板 `DbyBattery` 代码
- 性质：建议与执行顺序，不是充电控制授权；任何充电写或刷机仍需 owner 逐次批准

## 结论先行

1. **路线不变，目标收窄。** 继续标准 USB-PD 固定 PDO；本轮真正的交付物是「5 V 持续充电基线 + 只读 PD/Type-C 遥测」，22.5 W 是输入上限而不是本轮目标。
2. **在 sink capability 被运行时证据确认前，可辩护的上限是 13.5 W。** 公开板级记录只给出 stock sink 5 V/2 A、9 V/1.5 A，见 `docs/charging-usb-pd-spec-2026-11-26.md:28` 与 `:62`。
3. **Linux 的 9 V 行为只能当参考，不能直接移植。** 它请求 9 V/3 A、27 W 合约上限、默认 2.5 A 输入，但那是 Linux 驱动自己的策略与实机验收；UniXNU 既没有 owner diagram，也没有对应回退路径。
4. **第一个代码动作不是改 ICL，而是补只读证据。** 端口类型第二来源（FSA9685，I2C `0x26`/寄存器 `0x08`）与 PD 合约字段必须先能读到、且 unknown 与 0 可区分。
5. **写集合不扩大。** `dby-battery=2` 现有的 ICL 写与饥饿回退（`package/DbyBattery.cpp:326` 的阶梯）保持；不把阶梯数值抬高，不引进 Linux 的 PD 强制写序列。

## 一、三方现状对照

| 维度 | 本仓规格 | 本板 UniXNU 代码 | 同机 Linux `dby-chg` |
| --- | --- | --- | --- |
| PD 决策 | 标准 PD 固定 PDO，输入 ≤ 22.5 W（`docs/charging-usb-pd-spec-2026-11-26.md:5`、`:12`） | 无 PD 状态机；`DbyBattery` 只有 APSD/AICL/ICL 与 Type-C sink 字节 | 固定 PD 9 V 已在一台设备验证（`docs/status.md:77`） |
| 功率上限 | 未证 sink 表前 13.5 W（`:62`） | 阶梯最高 2.3 A @ 5 V DCP（`package/DbyBattery.cpp:326`） | `pd9v_max_ma` 默认 2500 mA，范围 1500–3000（`dby-chg-core.h:129`–`131`） |
| 端口类型来源 | 本板 APSD 被 `usb-thd-bc-12` 关掉 BC1.2，类型来自 FSA9685（`docs/charging-spec-2026-11-24.md:64`） | 未驱动 FSA9685；只有 APSD result 字节 | Type-C/PD PHY 归 `qcom_pmic_tcpm`，充电器经 supplier `USB_TYPE` 分类（`dby-chg.c:299`、`:313`） |
| 9 V 合法路径 | 未实现；禁止直接写 HVDCP 档（`docs/charging-spec-2026-11-24.md:397`） | 无 | `1344=02` + `1342=01` + `1366=01` + `1370` 目标和（README Fixed PD 表） |
| 回退 | 契约丢失/超时/过热回 5 V 或固件自动（`docs/charging-usb-pd-spec-2026-11-26.md:42`） | ICL 饥饿回退到 SDP 500（`docs/charging-spec-2026-11-24.md:319`–`324`） | 关闭序列 `1344=00`→`1342=00`→`1366=02`→`1370=1E`（README） |
| 低电量 | 未定义 | 无 | <3.5 V 时 9 V 限 1 A，≥3.6 V 释放（`dby-chg-core.h:139`–`141`） |

补充事实：

- 本仓现有基线只有电脑口：500 mA SDP，或 DCP 标签但净放电，且没有 PDO/RDO/TCPM 行（`docs/charging-baseline-2026-11-26.md:49`–`51`）。
- Linux 记录里 9 V 请求在 0%/2.89 V 时于 PS_RDY 前硬复位，31% 时 0.27 s 到 SNK_READY（README `PD transition and low battery guard`）。这条直接支持「低电量禁止请求高压」。
- Linux 的 `1370=1E` 在自动模式下并不约束输入，实测 ICL 状态仍约 2.2 A（README `Fixed PD power ceiling`）——说明「写一个限流值」不等于「限流生效」。
- UniXNU 公共 `QcomDwc3` 的硬件是 PM8550B/eUSB2/QMP，OS ABI 是 iPadOS 27，验证设备是 sheng，且依赖 UCSI charger channel（`../UniXNU/components.toml:526`），不是本板可替换件。
- 固定 core 的 `drivers/` 没有 Type-C/TCPM/PD 组件（目录只有 13 个驱动），所以「直接用 core 的 PD policy engine」目前不成立。

## 二、差距与风险

1. **PD owner 未定**：attach → PD PHY（PM8150B `0x17`）→ policy → SMB5 ICL 的链路没有任何一段在 UniXNU 里被证明；现有 Type-C 状态字节（`0x06`）只够判断 sink 状态，不够读合约。
2. **sink capability 未证**：stock 表 5 V/2 A、9 V/1.5 A 来自公开板级记录；如果运行时 sink 表就是它，最高 13.5 W。请求 22.5 W 需要先证明存在 9 V/2.5 A 或 10 V/2.25 A 的有效 sink PDO。
3. **端口类型缺第二个来源**：PMIC 拿不到 BC1.2 类型，FSA9685 未驱动；只靠 APSD result 会把 DCP 当 PD，或把 PD 源当 unknown（`docs/charging-baseline-2026-11-26.md:51`）。
4. **Linux 写集合不可移植**：`0x1344/0x1342/0x1366/0x1370` 的写序列是 Linux 驱动自证的回退与门槛，不是通用契约；UniXNU 照抄会把「驱动自证」当成「硬件保证」。
5. **空电升压风险**：Linux 实测 0% 请求 9 V/3 A 会在 PS_RDY 前硬复位；本板更应先限 5 V、再限电流。
6. **构建前置未绿**：冷构建尚未通过，任何充电改动都不能进入刷机轮次；先看 `docs/reviews/cold-build-failure-log.md`。

## 三、建议执行顺序

### P0A — 只读取证（host / 现有镜像）

- 扩展 `[charger]` 行：只在来源可证时增加 `pd=`、`cc=`、`contract_mv`、`contract_ma`、`rdo`、`pdo` 字段；来源不明写 `unknown`，不得用 0 冒充。
- 读 FSA9685：I2C `0x26`，只读 `0x01`（ID）、`0x08`（`DEVICE_TYPE_1`：bit6 DCP、bit5 CDP、bit2 SDP）、`0x1B`（VBUS_VALID）。地址与位定义见 `docs/charging-spec-2026-11-24.md:346`–`361`。
- 从本板 DTBO/逐 QUP 探测确认 FSA9685 挂在哪条 I2C 总线（Linux 参考机是 `i2c@78b8000`，本板未证）。
- 产出 owner diagram：attach → PD PHY → policy → SMB5 ICL，并写明回退路径。

**Gate：** 有 owner diagram、有可审查的寄存器/消息接口、有 5 V 回退；缺一项就不写任何 PD 控制寄存器。

### P0B — 5 V 持续充电基线（device）

- 用原装 22.5 W 充电器 + 外部 USB-PD 仪表；记录 VBUS、输入电流、电池净电流、SOC、温度、suspend、enable、故障，idle 与亮屏各 ≥10 分钟。
- 判据：持续正 IBAT 且 SOC 上升；拔插恢复；电脑 SDP 不回归。

**Gate：** 5 V 下可持续充电，且日志里能区分「端口类型 / 输入限流 / 电池净电流」三个量。

### P1 — 只读 PD 合约

- 仅在 P0A/P0B 通过后：读 Source_Capabilities、sink capability、选中的 PDO/RDO、合约电压电流，与外部仪表对齐。
- 判据：合约字段与仪表一致；读取失败与 0 可区分；充电行为不变。

### P2 — 受限固定 PDO 请求

- 仅当 P1 通过且 sink capability 经运行时证据确认：请求 `min(已验证 sink PDO, 线缆/源限制, 热限, 22.5 W)` 的固定 PDO。
- 明确禁止：3 A/27 W 请求、低电量（<3.5 V）请求 9 V、PPS/AVS、华为 SCP/FCP。
- ICL 上限取「合约电流、设备验证上限、热限」三者最小值；热降参考 Linux 的 250 mA/floor 1.5 A（`dby-chg-core.h:129`–`133`），但数值必须在本板重测后再定。

**Gate：** RDO 合法、合约与仪表吻合、输入不超 22.5 W、无持续负 IBAT、无保护故障；任一失败回 5 V/自动。

### P3 — 稳定性

- 冷启动、热插拔、不同 SOC、亮屏负载、拒绝/超时、VBUS collapse、过热、低电启动；保存源码、kernel/payload SHA-256、适配器/线缆型号、PD 日志与仪表读数。

## 四、具体落点

- `package/DbyBattery.cpp:326`：阶梯数值不动；新增字段只能来自已证来源，且必须区分 unknown/0。
- `board.toml:720`：`dby-battery=2` 保持；本建议不新增 boot arg。
- 新增只读 FSA9685 探测（建议独立脚本或最小驱动，只读 `0x01/0x08/0x1B`），先 host/现有镜像验证总线，再谈是否进包。
- PD 合约遥测的落点取决于 P0A 的 owner 结论：若 core 无组件，先在板级 package 做最小只读适配；若确定要共享，另开 core 组件提案，不把单板实现塞进 core。
- 每步更新 `porting.toml` power 区 findings/evidence（现有 next_action 已指向 `docs/pd-upstream-development-2026-11-28.md`）。

## 五、明确不做

- 不写 Linux 的 PD 强制序列：`0x1344`（FORCE_9V）、`0x1342`（override）、`0x1366`（options）、`0x1370`（9 V 目标）。
- 不移植 Linux 的 watchdog 喂狗 `0x1643` 与 enable `0x1042` 写。
- 不把 ICL 阶梯从 2.3 A 抬到 2.5 A，不以寄存器值或充电图标判定成功。
- 不把 APSD `DCP` 标签当 USB-PD 合约。
- 不做华为 SCP/FCP、PPS/AVS、40 W 请求；22.5 W 只在实测输入功率下声明。
- 不在 sink capability、owner、温度保护任一未知时启用 PD 控制。

## 六、待决策

1. **PD owner 归属**：板级最小只读适配，还是向 core 提通用 Type-C/PD 组件？（core 当前无 TCPM，`QcomDwc3` 不适用）
2. **首个功率目标**：先只验 5 V，还是直接把 13.5 W（9 V/1.5 A）作为 P2 上限？
3. **仪表**：P1 的 gate 需要能记录 VBUS/电流的外部 USB-PD 仪表；是否采购/借用。
4. **FSA9685 总线**：DTBO 取证优先，还是逐 QUP 只读探测？

## 七、证据索引

- 本仓规格：`docs/charging-usb-pd-spec-2026-11-26.md:5`、`:28`、`:36`、`:62`；`docs/charging-spec-2026-11-24.md:38`、`:64`、`:346`、`:397`；`docs/charging-baseline-2026-11-26.md:49`；`docs/pd-upstream-development-2026-11-28.md:37`。
- 本板代码：`package/DbyBattery.cpp:326`（ICL 阶梯）、`board.toml:720`（`dby-battery=2`）。
- UniXNU 组件：`../UniXNU/components.toml:526`（`QcomDwc3` 的 UCSI/硬件/OS 范围）。
- Linux 驱动（pin `3d82fb7d`）：
  - `drivers/dby-chg/dby-chg-core.h` 常量 `:123`–`147`（ICL/allow/override/options、9 V 默认 2500、热降 250、floor 1500、低电 3.5/3.6 V、1 A）
  - `drivers/dby-chg/dby-chg.c:299`、`:313`（supplier `USB_TYPE` 与 PD 分类）、`:338`（目标钳制）、`:639`（低电守护）
  - `drivers/dby-chg/README.md` 的 `Hardware ownership`、`Fixed PD`、`Fixed PD power ceiling and thermal derating`、`PD transition and low battery guard` 小节
  - `docs/status.md:77`（固定 PD 9 V 与 22.5 W 上限、长期风险未知）
- 公开板级记录：`youzjuzi/dby-w09-kernel@fc1fc8d` 的 `dts/provenance.md`（stock sink PDO 5 V/2 A、9 V/1.5 A）
