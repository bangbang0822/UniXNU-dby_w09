# DBY-W09 USB-PD 充电接入规格

- 修订日期：2026-11-26
- 设备：Huawei MatePad 11 DBY-W09，SM8250 / kona；目标系统 iPadOS 17.7.11（21H461）
- 决策：按用户确认，采用标准 USB Power Delivery（USB-PD）路径；不实现或调研华为私有充电协议。
- 设备额定输入：华为官方资料列出 10 V / 2.25 A（22.5 W）。22.5 W 是输入功率上限，不是承诺的持续充电功率。
- 当前状态：有 SMB5 电池遥测与 ICL 限流代码；未发现 UniXNU 内的 PD 协商状态机。没有针对 PD 的代码修改、构建或刷机。
- 基线测量与串口记录见 [`charging-baseline-2026-11-26.md`](charging-baseline-2026-11-26.md)。

## 1. 范围与功率目标

通过 USB-C 的标准 USB-PD 固定 PDO 协商为设备供电。设备端输入功率限制为：

```text
P_input <= min(22.5 W, 本机已验证的输入限值, 对端 PDO 与线缆允许的功率)
```

目标是让设备按其已证实的接收能力使用固定 PDO，最多到 22.5 W。不能只凭额定值推断当前固件、PD 控制器、线缆或充电器组合能达到 22.5 W。若实际有效的 sink capability 只允许更低功率，就按更低值协商；不超报能力，不请求超出接收端限制的 PDO/RDO。

第一阶段仅覆盖固定电压 PDO 和 5 V 回退。PPS/AVS、华为 SCP/FCP、QC 等均不在本规格范围内。充电器必须确实提供兼容的 USB-PD source PDO；充电器铭牌上的“40 W”等最大值不能证明它提供 USB-PD，也不能证明设备会吸收该功率。

## 2. 已知实现与证据

| 项目 | 现状 | 证据与限制 |
| --- | --- | --- |
| 设备输入额定值 | 10 V / 2.25 A（22.5 W） | [华为 DBY-W09 设备信息](https://consumer.huawei.com/cn/support/content/zh-cn15905569/)；厂商额定值，不是当前镜像实测 |
| 本机 Type-C/充电器 | PM8150B SMB5 与 PMIC Type-C/TCPM 出现在同机公开板级记录中 | [dby-w09-kernel DTS 来源](https://github.com/youzjuzi/dby-w09-kernel/blob/fc1fc8d509f9457ae96d4ef20cb7ca720c43c4fd/dts/provenance.md)；需要把记录中的控制器与本机运行时 DT/镜像逐项对应 |
| stock sink capability | 记录有 5 V / 2 A、9 V / 1.5 A 固定 PDO | 公开 DTS 来源记录；若该表就是本机运行时有效的 Sink_Capabilities，则最高仅 13.5 W。要超过此值，必须先确认实际 PD 栈允许的本机 sink capability |
| 同机 Linux 实现 | 报告过固定 PD 9 V / 3 A 合约能力；默认输入限流为 2.5 A（22.5 W） | [dby-w09-linux 状态](https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/docs/status.md) 与 [dby_chg 说明](https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/drivers/dby-chg/README.md)。Linux 记录可作同机行为参考，不证明 UniXNU 的 PD 路径已存在或 22.5 W 已在 UniXNU 上实测 |
| 当前 UniXNU 电池服务 | `DbyBattery` 读取 SMB5/fuel-gauge 状态、Type-C sink 状态并调 SMB5 ICL；源类型判定和 ICL 限制不等于 PD 合约协商 | [`package/DbyBattery.cpp`](../package/DbyBattery.cpp)。当前没有 PD source-capabilities、选定 PDO/RDO、合约电压/电流的已验证遥测 |
| 当前 ICL 阶梯 | `dby-battery=2` 的最高档是 2.3 A DCP；阶梯由 APSD/AICL 与 SMB5 限值驱动，不含 PD PDO/RDO 选择 | [`package/DbyBattery.cpp`](../package/DbyBattery.cpp) 的 `kIclLadder`。不能把 2.3 A DCP 档直接当作 9 V PD 档；22.5 W 输入需经 PD 合约确认并按 9 V/2.5 A 或其他合规 PDO 限幅 |
| 设备基线 | 电脑 SDP 口曾测得 ICL 500 mA；新 USB-C 电脑口捕获显示 APSD DCP、AICL 约 1.4 A 和负电池电流，且大部分遥测字段读失败 | [`charging-baseline-2026-11-26.md`](charging-baseline-2026-11-26.md)。新捕获没有 VBUS 或 PDO/RDO 数据，不能当作 PD 充电测试 |

## 3. 设计约束

- **PD 栈先确认。** 查明本机 Type-C/PD PHY、PMIC block、IRQ、sink capabilities 的来源，以及目标镜像中谁拥有 PD policy engine。仅靠 SMB5 ICL 写入不能发起 PD 协商。
- **使用标准 PD 路径。** 若 SoC/PMIC feed 或 UniXNU component 已有适用的 PD policy engine，优先使用它；共享的 Type-C/PD PHY 与协议实现不能复制进仅适用于单板的驱动。板级仓库只保留本板所需配置和安全限值。
- **组件需匹配硬件与系统。** UniXNU 目录目前可见的 `QcomDwc3` 组件依赖 UCSI charger channel，但其硬件范围包含 PM8550B/eUSB2 与 QMP USB3，OS ABI 是 iPadOS 27，验证设备为 sheng；这不是本板 PM8150B / SM8250 / iPadOS 17.7.11 的可直接替换件。当前 `feeds.toml` 未选入 PM8150B PD policy engine。
- **先读取再控制。** 先记录 attach、source PDO、sink capability、当前合约电压/电流、输入电流、VBAT、IBAT、SOC、温度和故障状态；未知或过期值需明确标为 unknown。
- **按可验证边界选择 PDO。** 选择满足本机已公布/验证 sink capability 且不超过 22.5 W 的固定 PDO；不请求 PPS/AVS。任何 sink capability 修改需先有本机源代码或运行时证据，并单独审查。
- **限制实际输入。** PD 合约功率、SMB5 ICL 和电池侧净电流是不同量。控制器必须将输入限流压在当前已验证上限内，并读取回显/AICL 与 collapse/UV；不得用合约额定功率冒充实际功率。
- **保留保护和回退。** 过温/故障、通信超时、读数失效、VBUS collapse/UV、合约丢失时，回到已验证的 5 V 或固件自动策略。回退机制没有本机依据时，不写寄存器。
- **不损坏现有门控。** `dby-battery=1` 只读路径和 `=2` 现有限流行为必须保持可回退；新增 PD 控制不得在设备身份、有效温度/健康状态及 PD 状态未知时启用。当前启动参数预算只有少量空余，不随意增加参数。
- **硬件验证受控。** 每次只改变一个变量；保存上一份成功启动镜像及 SHA-256。按仓库评审规则，未经评审的改动不刷机。

## 4. 阶段与完成条件

### P0 — 找到现有 PD policy engine

核对本机 runtime DT/DTBO、同机 Linux 的 Type-C/PD PHY 与 TCPM 代码、UniXNU/SM8250 feed 可用组件及其归属。明确 stock sink capabilities 与 Linux 9 V / 3 A 行为差异。

**通过条件：** 有明确的本机控制器身份、寄存器/消息接口、PD 栈 owner，以及可审查的 Source_Capabilities / Request / Accept / PS_RDY 状态链。若缺少其中任一项，不写 PD 控制寄存器。

### P1 — 只读 PD 遥测

使用已确认的接口记录 attach、对端 PDO、设备 sink capability、选中的 PDO/RDO、合约电压与电流和 SMB5 输入状态。至少覆盖无电缆、普通 USB、固定 PD 充电器及拔插恢复。

**通过条件：** PD 合约字段与外部 USB-PD 测量仪读数一致；读取失败可与 0 区分；不会改变现有充电行为。

### P2 — 22.5 W 限幅策略

仅在 P0/P1 通过且 sink capability 经确认后，令标准 PD policy engine 选择不超过 22.5 W 的固定 PDO；SMB5 ICL 限制不得超过 PD 合约电流、设备上限和已验证安全上限中的最小值。若有效 sink PDO 仅为 9 V / 1.5 A，则当前可验证上限为 13.5 W，不能仅为达到 22.5 W 而提高 PDO。

**通过条件：** RDO 合法、合约与仪器吻合、输入实测不超 22.5 W、没有持续负 IBAT、保护故障或异常温升；掉线/拒绝/超时回到 5 V。

### P3 — 稳定性

覆盖冷启动、热插拔、不同 SOC、亮屏负载、限流、拒绝、硬复位和异常传感器读数；保持普通 USB 主机口与现有电池上报工作。

**完成条件：** 保存源码、kernel/payload SHA-256、适配器/线缆型号、PD 日志、仪器测量和设备观察；只在测得输入功率后声明具体瓦数。

## 5. 验收与停止条件

| 项目 | 验收 |
| --- | --- |
| 协议 | 标准 USB-PD 固定 PDO；不要求也不启用华为私有协议、PPS 或 AVS |
| 输入上限 | 实际输入不超过 22.5 W，且不超过本机 sink capability、线缆与安全限值 |
| 回退 | 失败、过热、超时或拔线后回到已验证的 5 V/自动模式 |
| 电池侧 | 温度、VBAT、IBAT 符号和单位有来源；无持续净放电或异常升温 |
| 兼容 | 电脑 SDP 与默认启动模式不回归；现有 `=1`/`=2` 行为保持 |

若 sink capability、控制器 owner、合约状态或温度保护无法确认，停在只读阶段。现有 2.3 A DCP ICL 梯子不构成 PD 实现；不得仅把阶梯值从 2.3 A 改到 2.5 A。发现实际输入超过 22.5 W、保护故障、异常温升或无法回退时，停止升级并恢复上一份已知可用镜像。

## 6. 参考

- [UniXNU 功耗适配指南](https://unixnu.com/porting/power)
- [UniXNU 组件目录](https://unixnu.com/reference/components)
- [UniXNU `QcomDwc3` 组件条目](https://github.com/UniXNU/UniXNU/blob/main/components.toml#L515-L530) — UCSI charger-channel 依赖与硬件/OS 范围需与本板核对
- [华为 DBY-W09 设备额定值](https://consumer.huawei.com/cn/support/content/zh-cn15905569/)
- [同机板级 DTS 来源记录](https://github.com/youzjuzi/dby-w09-kernel/blob/fc1fc8d509f9457ae96d4ef20cb7ca720c43c4fd/dts/provenance.md)
- [同机 Linux PD 状态](https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/docs/status.md)
- [同机 Linux `dby_chg` 说明](https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/drivers/dby-chg/README.md)
- [`package/DbyBattery.cpp`](../package/DbyBattery.cpp) 与 [`porting.toml`](../porting.toml)
