# SM8250（kona / Adreno 650）GPU 实现方式：上游全量普查与本板比对

- 文档版本：2026-11-21，第一版
- 范围：`_core_devices.toml` 中 `soc = "SM8250"` 的 **47 台设备**（合并 fork 后 **22 个基座实现**）
- 对照对象：本仓库（dby-w09，华为 MatePad 11）
- 取证材料：`.research/gpu-survey/{a-catalog,b-alioth,c-gts7xlwifi,d-core-vs-fork}.md`，以及
  `.research/gpu-survey/pages/`（2026-11-21 抓取的 47 个设备记录页原件）

本文是一份**说明**，不是提交说明：它解释这块 SoC 上 GPU 是怎么被实现的、别人做到哪一步、
我们的做法与它们的差别在哪、以及差别里哪些是结构性的、哪些只是还没做。

---

## 0. 怎么读，以及证据分级

三个词在这份文档里有严格含义，混用会得出错误结论：

| 说法 | 含义 |
| --- | --- |
| **实现了** | 代码在仓库里，能编译、能链接、能在引导时跑到某一步 |
| **真机验证** | 有该板自己的引导日志/计数为证，且记录里写明"device-verified" |
| **像素级证明** | 有截图、回读、CRC 或 swaptest 输出，能证明面板上那帧确实是 GPU 写出来的 |

截至 2026-11-21，**没有任何一台 SM8250 设备有第三类证据**（包括我们）。所以本文明说的"能跑"，
一律指前两类。

引用格式：`路径:行` 指该仓库检出的当前内容；`.research/gpu-survey/*.md` 里的行号指那些报告本身。

---

## 1. 一句话结论

> 8250 的 GPU 没有"官方组件"可用，22 个基座各自用五种私有方式把同一份 Adreno 650 驱动搬进设备仓；
> 其中三台真机跑到了"GPU 合成桌面"，但没有一台有像素级证明；
> **本板的驱动与 gts7xlwifi 是同一份代码的两个分支（92 个文件里 46 个字节相同），
> 真正的差距不在驱动，而在三处板级接线：中断不可达、`clock.idle` 为空、故障隔离缺失。**

---

## 2. 背景：为什么这块 SoC 的 GPU 是"没有组件的"

### 2.1 目录里有什么，缺什么

`_core_components.toml` 与 GPU 相关的条目只有四个：

| 组件 | hardware（原文） | 对 8250 可用？ |
| --- | --- | --- |
| `Adreno740` | `["Adreno 740", "SM8550"]`（`_core_components.toml:88`） | **否**。依赖 `Sm8550Platform`，且代际是 7xx |
| `AdrenoMetal` | `["Turnip Darwin backend", "kernel GPU user-client ABI"]`（`:110`） | 是（用户态侧，代际无关） |
| `AdrenoPM4` | `["Adreno packet type 4 and 7", "A740 command layouts"]`（`:368`） | 部分（packet 编码通用） |
| `AdrenoFirmware` | `["Adreno A6xx/A7xx HFI"]`（`:387`） | 是（HFI 协议通用） |

**没有 `Adreno650` 这个组件**，也没有 `Sm8250Platform`。alioth 的记录把话说得很直白：
"`Adreno650` 是设备分支，基座目录收录它只是因为工具没有设备本地目录"（`a-catalog.md:51`）。
cmi 的记录更干脆：选 `Adreno740` "would misreport the hardware range"（`a-catalog.md:110`）。

结果就是：**每一台 8250 板子都必须自己解决"驱动从哪来"**。

### 2.2 一个共享的"历史遗留"：归档名

所有实现最终都产出/链接一个名字叫 `build/adreno740/libAdreno740.a` 的归档，
而里面的代码是 A6xx。这不是笔误，是 `build-xnu-4k-baseline.sh` 把名字写死了
（`b-alioth.md:374`、`a-catalog.md:159`）。对照本文后面所有"构建"一栏时要记住：
**看到 `libAdreno740.a` 不代表用了 Adreno 740 的代码**。本板的目录名 `drivers/Adreno740`
是同一件事的另一种表现。

### 2.3 两代代码，一份源

`Adreno740` 不是新实现，而是 `Adreno650` 向前 rebase 的产物：

- alioth 的提交历史自己写着 `f385f6a feeds: bump Adreno740 pin to rebased Adreno650 tip`
  （`d-core-vs-fork.md:25`）；
- 两棵树里的类名完全相同：`class AGXAccelerator`（`NativeAdrenoAccelerator.cpp:18`）；
- A650 树里带着 A740 的代码（`buildA740()`、`NativeAdrenoA740HwInit.h`），
  gts7xlwifi 的 `drivers/Adreno650/README.md:1` 标题就是 `# Adreno740`；
- 最硬的证据：**我们的** `drivers/Adreno740` 里自己写着 "This board's A650"
  （`NativeAdrenoRing.cpp:134`），并给出 chip id `0x06050002`（`:391`）。

代际差异在驱动内部用运行时分支处理，核心是 fence 尾包的事件号：

```
generation_ == 6 ? CACHE_FLUSH_TS(0x04) | IRQ      // A6xx，也就是 8250/Adreno 650
                 : CACHE_CLEAN(0x31) | ENABLE | IRQ // A7XX
```
（`NativeAdrenoRing.cpp:318`；gts7xlwifi 同构：`NativeAdrenoRing.cpp:313-321`）

所以"8250 用 Adreno740 驱动"在代码层面只是**同一个驱动的第 6 代分支**，不是错配。

---

## 3. 全量清单：47 台设备，22 个基座

### 3.1 为什么 47 ≠ 47

同一台机器被不同人 fork 后各自建记录，代码是同一份。fork 地图（`a-catalog.md:133-152`）：

| 基座 | fork 数 | fork 名单 |
| --- | --- | --- |
| alioth（Redmi K40） | 6 | alioth, -bangbang0822, -fjcx555, -songchiyu, -taishang1024, -yang232138 |
| pipa（Xiaomi Pad 6） | 5 | pipa, -bxiidn, -esaijj, -fft-infra, -xvanturing |
| umi（Mi 10） | 5 | umi, -agxmx666, -jopangjovan, -mouya-q, -neowiseq |
| instantnoodlep（OnePlus 8 Pro） | 3 | -ye3912, -zhangp996, 本体 |
| kebab（OnePlus 8T） | 3 | -stevedang67889, -yecw, 本体 |
| munch（Redmi K40S） | 3 | -erodenine, -upcry, 本体 |
| apollo（Mi 10T） | 2 | -ehgemus, 本体 |
| cas（Mi 10 Ultra） | 2 | -pxl777806, 本体 |
| j716f（Lenovo Xiaoxin Pad Pro 2021） | 2 | -emptyghost, 本体 |
| lmipro / pdx203 / thyme | 各 2 | — |
| cmi, **dby-w09**, enuma, gts7xlwifi, lemonades, nio, pong, r8q, sm-t970, tb371fc | 各 1 | — |

另有 12 个基座的记录页是**空桩**（只有仓名）：`enuma-alghiffaryfa19`、`lemonades`、`lmipro*`、`nio`、
`pdx203*`、`pong-kmu1757`、`r8q`、`sm-t970`、`thyme*`。空桩不等于"没有 GPU"，只等于"没有可归因的声明"。
（`dagu`、`elish` 是 SM8250-**AC**，不在 47 台之内，引用"所有 kona 设备"时要注意。）

### 3.2 逐基座总表

"代码来源"= 驱动字节从哪来；"构建"= 谁把它编译/链接进去；"板级"= 设备仓自己拥有什么。

| 基座 | 代码来源 | 构建 / 链接 | 板级自有 | 记录里的状态 |
| --- | --- | --- | --- | --- |
| **alioth**(6) | 设备仓 **submodule**：`drivers/Adreno650@84f4b44c`、`drivers/AdrenoMetal@f3be55bc`；同时存在一份 vendored `reference/repo/Adreno650`，记录里两者互相"谁才是被编译的" | `CONFIG_XNU_NATIVE_ADRENO=y` → 硬名归档 `libAdreno740.a`，成内核链接的一部分；Metal 插件 `build-adreno-metal-plugin.py` → `make-gpu-addon.py --publish` | `GpuProbe.cpp`（2214 行，`pipa-gpu=1..12`）、`package/GpuFirmwareA650.c`、`package/pipa/A650Gmu.c`、`GpuPowerBoard.h`、addon 挂载补丁、自己的 `AdrenoMetal` 分支 | **device-verified**（6 fork 中 3 个）：桌面出现、`GPUROW24 METAL ok`；但同时写"known-image render is still outstanding"、"backboardd SIGBUS"、"Drawing is not confirmed" |
| **gts7xlwifi**(1) | **vendored fork**：108 文件，`SOURCE.txt:1` 钉 `FFT-infra/Adreno650 a6xx-on-7fa13d4 914d006a` | 同名脚本 → `libAdreno740.a`，`build-xnu-4k-baseline.sh:654-657` 强载；11 个导出符号审计（`build-adreno650.py:168-179`） | `GpuProbe.cpp`（1703 行）、MACF 沙盒授权（`opengl-or-metal`、`posterkit.provider`）、内嵌 addon、hangcheck/重启策略 | **device-verified**：backboardd/SpringBoard/widget/poster/Settings 在 GPU 上合成；1730 次提交 0 失败、1436/1438 次换页无 CPU 拷贝 |
| **j716f**(2) | **设备内构建**，无 feed pin：`scripts/build-adreno650.py` → `libAdreno650.a`，A650 patchid `06050002` | `CONFIG_XNU_NATIVE_ADRENO` + `pipa-gpu=12`；addon `NGPUADD2`（`build-j716f-gpu-addon.py`，带固件 SHA-256） | 整条 KMD 构建、DCVS 块（`pipa-gpu-dcvs=1`，305–670 MHz）、Turnip 磁盘 shader cache、背光、addon 打包器、`gpu.status` 计数面 | **device-verified**：记录称"渲染 iPadOS 60 FPS"、"数千次硬件 draw ~54 µs/次，零 GPU fault" |
| **pipa**(5) | **采纳上游 pipa 移植**：chip id、GMEM 几何、hwcg、`a650_protect` 表都取自上游；固件**外置**（`proprietary-files.txt`） | `CONFIG_XNU_NATIVE_ADRENO` → `libAdreno650.a`；GPU addon | 采纳来的 `GpuProbe.cpp`、固件抽取脚本 + 摘要清单、addon 名 | implemented，但"**No pipa GPU run in this fork**；GMU/SQE/zap 固件为外部文件" |
| **munch**(3) | **vendored fork** `reference/repo/Adreno650`（"the pipa fork's A650 driver"）+ `package/GpuFirmwareA650.c` | 同上归档；**加速器编译关闭**（`PIPA_GPU_ACCELERATOR=0`，没有 IOAccelerator 类） | `GpuFirmwareA650.c`、`GpuProbe.cpp`、模式阶梯 | implemented，止步 modes 1–6："this SMMU accepts no writes from EL1"，Metal 不可达 |
| **tb371fc**(1) | 仓内 A650 后端 + "Pipa 21 项补丁" + V129 分配策略；无组件、无 submodule | 内核携带 A650 后端 | 21 个补丁 + V129 连续分配策略 | implemented：V128 推进提交/完成；V129 用户观察滑动 +5–10 FPS；"未建立统一帧时间基准" |
| **apollo**(2) | **补丁系列**：`patches/xnu/0001`（`apollo_adreno`、`ApolloGPU`），来自 HakiOS | 不走 UniXNU 构建：`apollo_gpu=21 apollo_gpu_use=1` | 整个驱动即补丁；固件取自 `.cache/firmware/gpu` | UniXNU 侧 planned；另一 OS 路径能用 Mesa/freedreno 出 OpenGL，**Metal 不可用** |
| **cas**(2) | 无 | — | — | planned："第一版用户态走 CPU 呈现" |
| **cmi**(1) | 无 | — | — | **not-applicable**："no UniXNU GPU implementation targets this part" |
| **dby-w09**（我们） | 无组件；`IOGPUFamily` 只在 prelink 根里陪跑 | 板内脚本 → `libAdreno740.a`（见 §7） | 见 §7 | planned（记录页仍写"面板上一切都是 CPU 渲染"） |
| instantnoodlep / kebab / umi | 无（占位或 planned） | — | — | planned |
| 12 个空桩基座 | — | — | — | 无记录 |

### 3.3 进度分级

把上表按"真机走到哪"重排：

| 级别 | 设备 | 依据 |
| --- | --- | --- |
| **A. GPU 合成桌面（记录自称）** | gts7xlwifi、j716f、alioth（3/6 fork） | 前者：1436/1438 次换页无 CPU 拷贝；中者：60 FPS 声明；后者：桌面出现 + `METAL ok`，但同页写"Drawing is not confirmed" |
| **B. 提交/完成推进，未到桌面** | tb371fc、munch（modes 1–6）、pipa（未上机） | 各自记录的最后一句话 |
| **C. 有补丁/驱动的另一 OS 路径** | apollo | HakiOS + Mesa，无 Metal |
| **D. 未做** | dby-w09（记录口径）、cas、instantnoodlep、kebab、umi | planned |
| **E. 不适用** | cmi | 记录明确声明 |

注意 D 里的"dby-w09"是**记录口径**，与本板实际状态不符——见 §7.4：我们已有 GPU 提交、fence 完成、
DPU 直扫，只是记录还没更新（`porting.toml:528` 仍写 "No GPU code is implemented for this SoC."）。

---

## 4. 五种实现方式

### P1 设备仓 submodule pin —— alioth 家族

- 驱动不在设备仓内，而是 gitlink：`.gitmodules:7-9` 指向 `FFT-infra/Adreno650`，pin `84f4b44c`；
  `AdrenoMetal` 同理 pin `f3be55bc`。
- `feeds.toml` **不**钉 GPU 仓，只钉 SoC 仓（`FFT-infra/UniXNU-sm8250-common`）和两个非 GPU 驱动。
- 板级补偿：`package/GpuProbe.cpp` 2214 行把电源、SMMU、CP、zap、IRQ、发布全做了。
- **代价**：记录自相矛盾——同一份 `porting.toml` 里出现三个"被编译的 Adreno650 commit"
  （`porting.toml:16`、`:43`、`:360`），因为 submodule pin 与 vendored 副本同时在仓里。
  另外 `Sm8250Platform` 在目录里不存在，alioth 的 GMU 实现是 `package/pipa/A650Gmu.c` 私有复制
  ——**共享靠拷贝，不靠组件**。

### P2 vendored fork —— gts7xlwifi、munch、**本板**

- 整棵驱动进设备仓：gts7xlwifi 108 文件/15,585 行；本板 92 文件/14,605 行。
- 好处：能改内部（我们的 `NativeAdrenoSubmit.cpp:365-396` write-combine 审计环、`:399` 去掉
  `CP_WAIT_FOR_IDLE`，都是"只有 fork 才能做"的改动）。
- **代价**：没有升级路径，靠人工对比上游；三份 fork 会各自漂移。
- 应对：gts7xlwifi 用 `SOURCE.txt` 记录出处，并用 `T970-GPU-FAULT-FREEZE.md` 记录为什么偏离上游。

### P3 板内构建 + 采纳上游移植 —— j716f、pipa、tb371fc

- 驱动以"采纳的移植"存在，由设备仓脚本编译（j716f 的 `libAdreno650.a`），或干脆是 21 个补丁
  （tb371fc），或固件外置（pipa）。
- 好处：上手快，能贴着某台已跑通的板子抄参数（chip id、GMEM 几何、hwcg、保护表）。
- **代价**：出处不可追溯，固件常变成"外部输入"，升级无路径。

### P4 补丁系列 —— apollo

- 驱动是 `patches/xnu/0001` 里的一坨 XNU 补丁（`apollo_adreno`、`ApolloGPU`），由板自己的 prelink 流程应用。
- 只在 6xx 语义层面同构；不能与其他板共享。

### P5 不做 —— cmi 等

- cmi 的结论值得引用：GOP 帧缓冲输出不需要 GPU 组件，目录里也没有任何东西面向 Adreno 650。

### 4.1 五种方式的代价对照

| | P1 pin | P2 fork | P3 板内构建 | P4 补丁 | P5 不做 |
| --- | --- | --- | --- | --- | --- |
| 可复现性 | 中（pin 可复现，但记录曾说不清） | 高（整棵在仓里） | 低 | 低 | — |
| 升级路径 | **有** | 无 | 弱 | 弱 | — |
| 改驱动的自由度 | 无 | **高** | 中 | 高 | — |
| 知识归属 | 组件 | 设备仓（漂移风险） | 某台板子的私有做法 | 某台板子的私有做法 | — |
| 适合谁 | 想跟上游走的板 | 需要长期改内部的板 | 快速验证 | 依赖某 OS 补丁栈的板 | GOP 够用的板 |

本板选的是 P2，而且比 gts7xlwifi 更"偏"一步：**目录名与代码代际不符**（见 §2.3），
官方目录里 `Adreno740` 是 SM8550 专用（`_core_components.toml:88`）。

---

## 5. 一次提交的完整机制（三家共用同一套语义）

这一节是理解后文所有差异的基础。把"从 Metal 提交到面板出帧"拆成七段：

### 5.1 入口

Metal 通过 `IOAcceleratorES` 找到驱动：`NativeAdrenoAccelerator.cpp:2-4` 用
`MetalPluginName` 指向 bundle，客户端随后拿到 `NativeAdrenoUserClient::dispatch`（`:563`）
与 `submit`（`:428`）。

### 5.2 客户端交的是 IOVA，不是 PM4 字节

客户端交出的是 **IOVA 列表 + 描述**，驱动负责把它们包成 IB
（`NativeAdrenoUserClient.cpp:446-451`）。这点很关键：驱动并不理解 Metal 的指令流，
它只是把客户端准备好的命令装进 ring。

### 5.3 IB 封装与 fence 尾包

`NativeAdrenoRing::submitWords`（`NativeAdrenoRing.cpp:544-606`（本板 fork 的行号；gts7xlwifi 的同名文件行号更大））预留"prologue + fence"，
把客户端字**原样拷贝**，然后追加 fence；A6xx 的 framing 在源码注释里写明
（`NativeAdrenoRing.cpp:551-553`）：A6xx 既不要 context entry 也不要 BV fence，
一次 framed 提交只花两个 CCU invalidate 加一个普通 fence。

fence 尾包是 7 个字（`NativeAdrenoRing.cpp:309-323`）：

```
word0..1  CP_SCRATCH(2) 存 seqno
word2..   CP_EVENT_WRITE7 (opcode 0x46) 4 个参数
  words[1] = seqno
  words[3] = generation_==6 ? CACHE_FLUSH_TS|IRQ : CACHE_CLEAN|ENABLE|IRQ
  words[4..6] = (fence_iova_+4) 的 lo/hi 与 seqno      <- 时间戳落点
```

`fence_iova_+0` 是 CP 的 RPTR shadow，`+4` 才是 `msm_rbmemptrs.fence`，等待方读的就是 `+4`
（附带内存模型：先 `cache_.publish`，再 `barrier`，最后才写 `CP_RB_WPTR`，
`NativeAdrenoRing.cpp:588-598`）。

### 5.4 完成与唤醒：三个岔口

完成**不是中断写的**，是 GPU 自己把 seqno 写进 `fence_iova_+4`。中断只负责"把人叫醒"。
等待循环（本板 `NativeAdrenoPublish.cpp:656-686`）长这样：

```
读 fence 字 → 已到 return Complete
           → 未到：now_us - start >= timeout_us ? 超时
           → clock.idle 存在？调用它；返回非零 → 结束等待
           → 不存在：delay_for_interval(100 us) 继续轮询
```

三个岔口就是三种实现方式的全部差别：

| 岔口 | 有中断 | 无中断但有心跳式 spin | 都没有（我们） |
| --- | --- | --- | --- |
| 醒来手段 | `armcpu_spi_attach` → handler → `thread_wakeup` | `IODelay(20)` 或 `assert_wait_timeout(100 µs)` | `delay_for_interval(100 µs)` |
| 谁实现 | 板上 probe（不是驱动） | 板上 probe | 没人（驱动内建降级） |
| 代价 | 需要 GIC 组正确 | 每次 ≤100 µs 的定时器往返 | 同样，且**没有** spin 的快速路径 |

### 5.5 驱动编入的内存

三家的数字几乎相同（都以板级常量形式出现，不在驱动里）：

| 项 | 值 | 出处 |
| --- | --- | --- |
| SMMU 基址 | `0x3da0000`，窗口 `0x10000` | `GpuProbe.cpp:442`、`:326` |
| context bank / ASID | bank 0 / ASID 2（GMU 用 bank 1 / ASID 1） | `GpuProbe.cpp:687` |
| S2CR 槽位 | `{0,1,4,5}` | 同上 |
| stream id | `{0,1,0x400,0x401}` | 同上 |
| 用户态 VA 窗 | 从 `0x20000000` 起，chip id `0x06050002` | `GpuProbe.cpp:1453-1454` |
| SMMU 配置 | `coherent_walk=1, stall_on_fault=0, continue_after_fault=1` | `NativeAdrenoSmmuContext.cpp:15-17` |

一个真实的硬件坑（值得每个 8250 板记下）：alioth 的空闲 S2CR 槽读作 `0x000203ff`
（CBNDX 0xff + FAULT 类型），而 kit 的空槽判定要求整字等于 `0x00020000`；
把这种槽清成 0 会把 FAULT 变成 TRANSLATE，于是"又像被占用"。
alioth 的解法是把 FAULT 型空闲槽规整成 `0x00020000` 再 attach（`GpuProbe.cpp:865-873`）。

### 5.6 电源与固件

- **不是单纯时钟投票，也不是纯 GMU**：先 CX 域 + GPUCC/GMU 时钟上电，再通过 GMU 自己的 SMMU
  上下文拉起 GMU，最后向 GMU 投 GX 票。alioth 的模式阶梯 6→7→8 就是这三步。
- GX 票是三元素 `{1, perf_index, ddr}`（`A650Gmu.c:354-363`），`perf_index` 1..7 对应
  305–670 MHz，默认 7（670 MHz）。**三家的默认都是 7。**
- 固件：alioth/munch 编译进内核（`package/GpuFirmwareA650.c`），gts7xlwifi 用
  `scripts/gen-gpu-firmware-a650.py` 从平板自己的分区生成，pipa 外置。
- zap 打包有个真实坑：厂商 `a650_zap.mdt` 是 `[ELF 头 148 B][hash 6712 B]`，
  而程序头把 hash 指到 `0x1000`、`b02` 段指到 `0x3000`；直接拼接 8536 B 会让 `zapLoad` 失败（-6），
  alioth 最终内嵌的是按地址布局重排的 13964 B 镜像（`porting.toml:373`）。

### 5.7 显示交接

GPU 写出的帧有两条出口，可同时开：

| 出口 | 参数 | 机制 |
| --- | --- | --- |
| GPU 客户端接管帧 | `fb-gpu=1`（旧名 `sheng-fb-gpu`） | 帧被交给特权 GPU 客户端，swap 在 `native_legacy_gpu_complete` 时退休；CPU 拷贝被移除（`NativeGOPresenter.cpp:122` 返回 `kIOReturnUnsupported`） |
| DPU 直扫 IOSurface | `dpu-scanout=2`（旧名 `sheng-dpu-scanout`） | 显示控制器直接取合成器的 IOSurface（经 stage-1 窗口），CPU 完全不碰像素 |

本板用的是第二条（`sheng-dpu-scanout=2`）+ `dby-dpu=1`；gts7xlwifi 两条都开；
alioth 用 `fb-gpu=1` 加 `vsync=120`。

---

## 6. 三台走得最远的实现

### 6.1 alioth（P1，submodule pin）

- 入口：`IOAcceleratorES` → bundle（`NativeAdrenoAccelerator.cpp:2-4`）。
- 提交：客户端交 IOVA → `NativeAdrenoRing::submitWords` 装帧 → fence 尾包 → `CP_RB_WPTR`。
- 中断：SPI 300 → INTID 332，level（`armcpu_spi_attach(32+300,false,…)`，`GpuProbe.cpp:1431`）；
  handler 读 `RBBM_INT_0_STATUS`(0x201) 写 `RBBM_INT_CLEAR_CMD`(0x37)，锁 CP 错误，然后
  `thread_wakeup`；同一状态连续 100000 次则屏蔽（`GpuProbe.cpp:1405-1424`）。
- 等待：**板级提供 `naIdle`**（`GpuProbe.cpp:815-821`）：中断已 attach 时
  `assert_wait_timeout(…, 100 µs)`，否则 20 µs spin。所以它是"中断驱动 + 100 µs 上界"。
- 电源：`gxOn()` 向 GMU 投 `perf_index`（默认 7），GMU 冷启动在 `package/pipa/A650Gmu.c`。
- 记录里的硬数字：`GPUROW25 timeouts 97388 over 1897 submissions`（`porting.toml:393`）、
  `[fence wait] outer waits=169, 65142 us total, mean 385 us, longest 7787 us`（同处）、
  单次等待占 60 Hz 帧预算的 47%。
  > 注意：97388 这个数远大于提交数，很可能是**按轮询计**而不是按 fence 计；
  > 不同板的 `timeouts` 计数口径未必相同，跨板直接比较会失真。
- 未完成："known-image render is still outstanding"、"Drawing is not confirmed"、
  backboardd `SIGBUS`（`EXITCRASH pid 54 sig10 … ESR 0x82000007`）。

**它证明的事**：在同一份驱动上，只要中断真的送达、板级 `clock.idle` 真的实现，
等待就能从"100 µs 轮询"变成"中断唤醒"。

### 6.2 gts7xlwifi（P2，vendored fork，与我们同源）

- 出处：`SOURCE.txt:1` = `FFT-infra/Adreno650 a6xx-on-7fa13d4 914d006ab46c99dde801da9c3ff9b52f0777b27e`。
- 真机数字（periph15 引导日志）：
  - `5:1730/0/4313605` —— Submit 1730 次、失败 0、总计 4,313,605 µs（约 2.5 ms/次）；
  - `SWAP calls=1438 … COPY calls=2 … retired=1438` —— 1436/1438 次换页由 GPU 写，CPU 只兜了 2 次；
  - `[gpu methods] 6:2496` 等 —— 2496/5194 次 fence 等待超时（48%）；
  - 8 个客户端打开设备、0 次拒绝、峰值映射 1353 MiB。
- 中断：SPI 300 → INTID 332，level（`GpuProbe.cpp:1153`）；记录里 6.68M 次中断。
- 等待：`naIdle`（`GpuProbe.cpp:608-618`）——有中断时睡在事件上，否则 `IODelay(20)`。
- 未解决：mediaserverd `VK_ERROR_INITIALIZATION_FAILED`（0 MiB 映射）、锁存的 cb0 FSR 0x400
  一直不清、146 秒内 32 次 CP 重启、首次旋转时 SoC 静默复位。

**T970 故障冻结全案**（`drivers/Adreno650/T970-GPU-FAULT-FREEZE.md`，260 行）是这棵树里
最有移植价值的东西，因为它记录了一个完整失败链并给出了修复：

| 现象 | 打开"设置 > Wi-Fi/声音"或上滑后 10–15 s 整机 UI 冻结；之后 backboardd 在 Turnip 里崩（`tu_init_hw`，FAR 0）且回不来 |
| --- | --- |
| 取证 | 四次引导的 SMMU `FSR 0x402` / `0x80000402`、FAR（三个 >4 GiB）、FSYNR0（PLVL 1/2）、停住时的 IB1/IB2 |
| 解码 | `0x402` = TF(translation fault) + AArch64 format 位；`SS`(bit30) 从未置位 ⇒ 没有事务被 stall，是 GPU 拿到错误响应后 CP 停住（SQE pc 0xaa，IB1 有余字，fence 永不写） |
| 根因 1 | 板子从未安装 `native_adreno_set_restart`，`hangcheck` 只计数不恢复 ⇒ CP 永久停 |
| 根因 2 | 故障锁存在 bank 0 的 FSR 里，`observeCheap` 每 64 次 map/unmap 读一次，失败即 `fail()` 毒化该 VM |
| 结果 | 每个进程的 VM 依次被毒化（`[gpu vmfail] first st 11 stage 11`），UI 永久冻结 |
| 修复 | 板级钩子 `native_adreno_bank_fault_end(bank)`（null 默认）+ 证明失败最多重试 3 次 + `gpuBankFaultEnd`（记录 FSR/FAR/FSYNR0/ASID/uptime、必要时发 `CB_RESUME`、清 FSR）+ `native_adreno_set_restart(gpuRestart)` + `native_adreno_dram_read32` 让 stall dump 读到真 IB 字 |
| 附带 | `useVm` 返回 `kIOReturnBusy` 而不是 `kIOReturnTimeout`，避免"丢一次中断"被上报成 `VkDeviceLost` |

**对无中断的兄弟板意味着什么**：`gpuBankFaultEnd` 还挂在 **2 秒心跳**上（`GpuProbe.cpp:1631`），
hangcheck 靠 fence 进度判断而不是靠中断（`fenceProgress`/`hangCheck`），所以**即使 GPU 中断永远不来，
锁存的 bank fault 也能被清、CP 也能被重启**。代价是"每次卡帧付一次超时"，而不是"整机冻死"。

### 6.3 j716f（P3，唯一声称 60 FPS 的）

- 设备内构建：`scripts/build-adreno650.py` → `libAdreno650.a`，`CONFIG_XNU_NATIVE_ADRENO` 链接。
- 板级拥有 DCVS 块（`pipa-gpu-dcvs=1`，305–670 MHz，走 GMU HFI DVFS）、Turnip 磁盘 shader cache、
  addon 打包（`NGPUADD2`，带固件摘要）、`gpu.status` 计数面、背光。
- 声明：60 FPS、数千次 draw 约 54 µs/次、零 GPU fault / pagefault。
- 限制：为稳定性用 Turnip `sysmem,nocb` + 1× MSAA 覆盖；下一步才是 GMEM tile 渲染。
- **注意**：这是记录自述，仓库里没有像素级证明；它的 `-emptyghost` fork 连 GPU 固件都没有。

### 6.4 其余的实现与卡点（各一句）

- **pipa**：采纳上游参数 + 外置固件；两个 fork 都**没在真机上跑过 GPU**。
- **munch**：vendored fork，固件编进内核；卡在"这个 SMMU 不接受来自 EL1 的写"，
  GMU 上下文建不起来 ⇒ Metal 不可达；且 `PIPA_GPU_ACCELERATOR=0` 根本没编加速器。
- **tb371fc**：仓内 A650 后端 + 21 个 Pipa 补丁 + V129 显式连续分配；有提交/完成推进，
  用户观察滑动 +5–10 FPS，但没有统一帧时间基准。
- **apollo**：驱动是 XNU 补丁系列；只有 HakiOS + Mesa 路径能出 UI（OpenGL ES），**Metal 不可用**。
- **cmi**：明确声明不适用。

---

## 7. 本板实现详解

### 7.1 文件与构建

| 层 | 文件 / 脚本 | 规模 |
| --- | --- | --- |
| 驱动（vendored fork） | `drivers/Adreno740/**` | 92 文件 / 14,605 行 |
| 板级 probe | `package/DbyGpuPowerProbe.cpp` | 5,780 行 |
| 电源/GMU kit（板内副本） | `package/Sm8250Platform/A650Gmu*.c`、`package/DbyA650Gmu.c/h` | ~110 KB |
| 固件 | `package/DbyGpuFirmware.h`（由 `scripts/gen-gpu-firmware-header.py` 生成） | — |
| 构建 | `scripts/build-adreno650.py`（33.5 KB）、`build-adreno650-accelerator.py`、`build-gpu-power-probe.py`、`build-gpu-smmu-probe.py`、`make-gpu-addon.py` | — |

构建结果仍是硬名 `libAdreno740.a`，强载进内核；`kits/` 目录是空的（`.gitkeep`），
即我们**不 pin kit**，GMU 代码是板内私有副本——和 alioth 的 `package/pipa/A650Gmu.c` 一个性质。

### 7.2 板级 probe 的 26 个模式

`dby-gpu-power=<1..26>`（`DbyGpuPowerProbe.cpp:55`、`:5668`），absent 时只打一行 "skipped"。
阶梯按"上电 → GMU 上下文 → CP → zap → SMMU → 发布 → 加速器 → Metal"推进，
每一级都有独立的行与返回码（`gmu/pay/cpu/cold` 等四个返回码来自四个阶段，
`porting.toml:555` 把它们逐条解码过）。

### 7.3 GPU 相关引导参数（committed 版，55 个参数 / 983 字节）

| 参数 | 作用 |
| --- | --- |
| `dby-gpu-power=26` | probe 走到最高一级（发布 + 加速器 + Metal） |
| `gpu-addon=1` | 挂载 GPU addon 到 Data 卷 |
| `sheng-gpu-metal=1` | Metal 插件路径开关 |
| `dby-dpu=1` | 板级 DPU 直扫（GopFramebuffer 侧） |
| `sheng-dpu-scanout=2` | 启用直接扫描输出（DPU 取 IOSurface） |
| `offline-mobile-graphics-load=1` / `-client=1` | MobileGraphics 用户态链路 |
| `smp=1` | 多核（GPU 线程调度前提） |

工作树里另有 `dby-pcie=3`（另一个会话的无线工作），committed 版没有它。

### 7.4 实测数字，以及它们说明什么

以性能轮（`.research/console-gpuperf2-2026-11-20.log`）为准：

```
[metal askers]  backboardd:1 mediaserverd:1 WidgetRenderer_Defa:1 SpringBoard:1
                PhotosPosterProvide:1 CollectionsPoster:1 MobileSMS:1
[gpu ibs]       submissions=1753 ibs=4562 max=88 over16=45
[fence wait]    calls=3150 timeouts=726 last timed-out fence=1754 gpu wrote=1753
                submitted=1754 completed=1753 no-job=1693 refused fence=0
[fence wait]    outer waits=3103 us=22659129 pauses outside the lock=14150 longest=874685us
[gpu methods]   5:1752/0/74039183
[direct scanout] state=80000004 flips=118 surfaces=3 mapfail=0 fallback=0 boot=1
```

逐条解读：

- `submissions=1753` 与 `gpu wrote=1753` 对齐 ⇒ **GPU 把交给它的东西全执行完了**，不是驱动喂不进去；
- `[metal askers]` 有 7 个进程 ⇒ Metal 客户端侧是活的，不是"没人来问"；
- `flips=118`、`fallback=0` ⇒ 帧确实经 DPU 直扫出去，没有退回 CPU 拷贝；
- 但 `[gpu methods] 5` 是 **1752 次调用共 74,039,183 µs ≈ 42.3 ms/次**，而
  `[fence wait] us=22,659,129`（22.7 s）说明**大部分时间花在等 fence**；
- `timeouts=726 / calls=3150 = 23%`，最长单次 874 ms。

三家的同一行对比（不同轮次、不同等待预算，只能看量级）：

| | Submit 均值 | fence 超时 | 备注 |
| --- | --- | --- | --- |
| 我们 | ~42–44 ms | 23% | 无中断、无 `clock.idle` → 100 µs 轮询 |
| gts7xlwifi | ~2.5 ms | 48% | 有中断 + `naIdle`，靠超时-kick 推进 |
| alioth | 记录未给均值 | 按记录口径 97388 / 1897 | 有中断 + `naIdle`，单次等待占帧预算 47% |

> ⚠️ 这三列不能直接当"性能排名"：`Submit` 的 µs 是否包含 fence 等待、`timeouts` 是按 fence 还是
> 按轮询计数，三家的实现与计数口径都不同。能确定的是**我们的等待方式与另两家不同**。

### 7.5 死观测量与类型缺陷（都应当修）

| 符号 | 声明 | 写者 | 后果 |
| --- | --- | --- | --- |
| `native_gpu_perf_vote[2]` | `BootProgressStubs.c:234`（weak，`uint64_t`） | **没有** | `[clocks] gpu perf %u bw %u`（`BootLog.c:893`）永远是 0/0——不是"没投票"，是"没接线" |
| `native_gpu_idle_sleeps` / `_spins` | `BootProgressStubs.c:127-128`（weak，`uint64_t`） | **没有** | `[coherence]` 的 slept/spun 两列永远 0 |
| 类型不一致 | 桩是 `uint64_t`，驱动与读取端是 `uint32_t` | — | 若驱动未链入，读到的是 64 位对象的一半——"真 0"与"没接线"不可区分 |

本板记录已经承认前两项是"dead observation slots, not evidence"（`porting.toml:525`），
但没有修；第三项是新发现。

---

## 8. 逐文件差异：我们 vs gts7xlwifi

### 8.1 盘点

- 我们：92 文件 / 14,605 行；gts7xlwifi：108 文件 / 15,585 行；
- 我们的 92 个文件是它的子集；**46 个字节相同、46 个不同；我们没有任何它没有的文件**。

46 个不同的文件里，语义上有意义的分三类：

1. **它新、我们旧**：per-bank fault end + 重试、`vm_fail`/`asid_names`/`map_ring`、
   yield 型 fence（`RingYieldWords`）、trace/latency/perf counters 四个新文件；
2. **我们新、它旧**：`NativeAdrenoBoard.cpp` 的"先校验全部 DT 条目再落盘"（`Board.cpp:19-30`、
   `:44-65`）、write-combine 信任/审计环（`Submit.cpp:365-396`）、IB 里去掉 `CP_WAIT_FOR_IDLE`
   （`Submit.cpp:399-400`）；
3. **各自板级条件**：SMMU `coherent_walk` 的取值、chip id 常量、coherence 标志
   （`NativeAdrenoSmmuContext.cpp:4`、`Ring.cpp:391`、`Vm.cpp:13`）。

### 8.2 16 个"它独有"的文件，逐个评价

| 文件 | 它做什么 | 对我们有用吗 |
| --- | --- | --- |
| `NativeAdrenoPreempt.cpp/.h`（213/118 行）、`tests/test_preempt.cpp` | A6xx **没有硬件抢占**（我们自己的 `NativeAdrenoIrq.h:52` 写着 "A6xx has no preemption (no CP_SW)"），运行期只有 `setTableSwitching` 在用 | **不要移植** |
| `NativeGpuPerfCounters.h/.cpp`、`NativeGpuPerfCountersTest.cpp` | A7XX 的 19 计数器 BR 方案 | **不要移植**（A650 无此块） |
| `NativeGpuTrace.h/.inc`、`NativeGpuTraceStore.h`（49/70/32 行） | 65536 条记录环：submit / retire / present / display latch / 构建标识 / 线程身份；`gpu-trace=1` 开启 | **要**——这正是"43.9 ms 花在哪"的观测面 |
| `NativeGpuLatency.h`、`NativeGpuThreadTargets.h` | 分阶段延迟时间戳 + 线程角色标注 | **要**（与 trace 配套） |
| `T970-GPU-FAULT-FREEZE.md` | 整机冻结的完整取证与修复清单 | **要**（§6.2 已摘录） |
| `SOURCE.txt` | 一行出处：`FFT-infra/Adreno650 a6xx-on-7fa13d4 914d006a` | **要**（我们靠 `porting.toml` 记，更啰嗦） |
| `Info.plist`、`LICENSE`、`tests/HostKernelHooks.cpp` | 打包与宿主测试 | 参考 |

### 8.3 一个必须点明的反直觉结论

同一份代码，**gts7xlwifi 的 Submit 均值比我们低一个数量级**，而它的 fence 超时率（48%）比我们（23%）
还高。两者并不矛盾：它每次超时后走 kick/stall 路径快速返回（`noteStall`、`[gpu unstick]`），
我们是"每次轮询都付 100 µs 定时器往返"。**决定延迟的是等待策略，不是超时率。**

---

## 9. 差异归因：三条结构性 + 两条实现选择

### 9.1 结构性：中断不可达

- 现象：`armcpu_spi_attach` 读回 `0x0000a002`（bit0=0）⇒ SPI 300 在 **GIC group 0**；
  `ArmCpu.cpp:1056` 记录"secure 世界持有时 group-1 写入是 RAZ/WI"；`ArmCpu.cpp:762`
  只读 `icc_iar1_el1` ⇒ 线被拉起，但没人取。
- 计数证据：1502 次 fence 提交 → 2 次中断（`porting.toml:525`）。
- **这不是改一行 attach 能解决的**，要么让 secure 侧放行 SPI 300，要么承认"无中断"并按无中断设计。
- 另两家都不依赖中断单独成立（都有超时 + 降级 spin），所以"无中断"不等于"不能跑"。

### 9.2 结构性：等待策略缺位

- 我们传 `clock.idle = nullptr`（`DbyGpuPowerProbe.cpp:4485`、`:4527`、`:4613`、`:4833` 四处都保持 nullptr），
  驱动因此走 `NativeAdrenoPublish.cpp:674` 的 100 µs `delay_for_interval`。
- 我们**试过一次**：装了一个"前 4 次 poll spin 20 µs，然后睡 100 µs"的回调，
  结果 UI 冻结、内核仍活（payload `164b4a52`，`flips=1846` 后停住），源码改动被回退，
  记录为失败轮次（`porting.toml:525` 末尾）。
- 失败原因**不是"回调不该存在"**，而是那版在回调里做了不可调度/阻塞的等待。
  gts7xlwifi 的 `naIdle` 是"`assert_wait_timeout(event_t, …, 100 µs)` 或 `IODelay(20)`"，
  等待对象是 ISR 里的 `thread_wakeup`——这正是我们缺的语义。

### 9.3 结构性：故障隔离缺失

- `NativeAdrenoVm.cpp:324` 仍是 `observeCheap(); if(status) return fail(status);`
  ——一次锁存的 FSR 就毒化整个 VM，与 T970 修复前**完全一致**。
- 我们没有：`bank_fault_end` 钩子、3 次重试、`vm_fail`、`asid_names`、`map_ring`。
- 重启钩子只在 `dby-gpu-restart=1` 时安装（`DbyGpuPowerProbe.cpp:4847-4849`），
  而 `:4546` 自己写着"nothing in this repository ever called native_adreno_set_restart"
  ⇒ **默认情况下 CP 停住后本引导内不再恢复**。
- 这是"整机看起来卡死"的防护缺口，优先级应高于帧率。

### 9.4 实现选择：目录名与组件登记

`drivers/Adreno740` 装的是 A650 代码，目录名与代际不符；记录的 `components` 里
（`porting.toml:512`）列的是 `AdrenoFirmware/AdrenoPM4/ArmSmmu4K`，**没有登记任何 GPU 驱动组件**，
所以 `unixnu` 生成树时不会链接 `drivers/Adreno740`——记录里 2026-11-14 那一轮"三个 recipe 回退到
核心检出、造出另一份驱动线"的事故就是这么来的（`porting.toml:519`）。

### 9.5 实现选择：观测面自制

我们有约 25 行 GPU 控制台观测（`[gpu latch]`、`[gpu uche]`、`[gpu stall]`×4、`[gpu dropwait]`、
`[fence wait]`×2、`[gpu unstick]`、`[gpu recovery]`、`[gpu methods]`、`[gpu ibs]`、`[metal *]`），
比 gts7xlwifi 的 GPUROW 更密；但缺"分阶段延迟"和"记录环"这两类**时间序列**观测，
所以能看见"总耗时"，看不见"哪一段"。

---

## 10. 行动方案

按投入产出排序。每一项都给出"怎么验证"，因为本仓库的规则是**先评审再上机、一次只改一个变量**。

### 第 1 项：让 `clock.idle` 真的可用（最高 ROI）

- 目标语义（照 gts7xlwifi `GpuProbe.cpp:608-618`）：
  1. 中断已 attach：在 ISR 的 `thread_wakeup` 事件上 `assert_wait_timeout(100 µs)`；
  2. 未 attach：`IODelay(20)` spin；
  3. 两种都计入 `native_gpu_idle_sleeps/spins`（**顺便把这两个死观测量接活**）；
  4. 返回值语义：**返回非零表示"该结束了"，不是"睡了多久"**——这是上次冻结的教训之一。
- 前置：先在主机侧/静态证明该回调可从等待线程上下文调用（不可阻塞、不可持锁），
  评审通过后再上机。
- 验收：`[fence wait] outer waits` 均值与 `[gpu methods] 5` 均值同时下降；
  `[coherence]` 的 slept/spun 变成非零；`timeouts` 不升。

### 第 2 项：故障隔离（最高风险 ROI）

- 移植：`native_adreno_bank_fault_end` + 3 次重试（`NativeAdrenoVm.cpp:27-30`、`:101-111`、`:161`、`:490`）、
  `gpuBankFaultEnd`（清 FSR、必要时 `CB_RESUME`、记录 FSR/FAR/FSYNR0/ASID/uptime）、
  `native_adreno_set_restart(gpuRestart)`、`native_adreno_dram_read32`，
  以及把 fence 超时的返回值从 `kIOReturnTimeout`（我们的 `NativeAdrenoSubmit.cpp:582`）改成
  `kIOReturnBusy`（gts7xlwifi `NativeAdrenoSubmit.cpp:138`）——最后这一条是无中断板上最便宜的一条：
  它让"丢一次中断"不再被 Turnip 上报成 `VkDeviceLost`。
- 把 `dby-gpu-restart` 从"opt-in"改成默认开（或明确记录为什么不开）。
- 验收：人为触发一次 page fault 后，`[gpu recovery]` 显示重启、UI 存活；
  否则至少 `[gpu vmfail]` 能指到 ASID/进程名。

### 第 3 项：把 43.9 ms 拆开

- 两条路：移植 `NativeGpuTrace` + `NativeGpuLatency`（较重，但有 65536 条记录环）；
  或先用现有 `[gpu methods]`（按方法号）+ `[fence wait]`（按阶段）+ `[gpu ibs]` 交叉，做一次粗分解。
- 建议先做粗分解（零风险），再决定要不要移植 trace。

### 第 4 项：修死观测量与类型缺陷

- `native_gpu_perf_vote`、`native_gpu_idle_sleeps/spins`：要么接真值，要么从 `BootLog` 删掉；
- 统一 `uint64_t`/`uint32_t`；
- 顺手把 `[clocks] gpu perf/bw` 接到真实的 `perf_index`（我们现在固定 7，
  `DbyGpuPowerProbe.cpp:5904-5905`，接上后至少能证明"投票确实发了"）。

### 第 5 项：方向决策（需要 owner 拍板）

- 继续 P2（fork）：灵活，但要接受"无升级路径"，并学 gts7xlwifi 加 `SOURCE.txt` 式的出处记录；
- 收敛 P1（pin `Adreno650` + 把 `Sm8250Platform` 登记进目录）：
  一次性的目录改动，换来升级路径与可复现；代价是失去"改驱动内部"的自由（我们有三处改动要回推）。
- 不建议的中间态：继续 fork 但目录名与代际不符（当前状态），因为任何外部读者都会误读。

### 不建议做的

- 移植 `NativeAdrenoPreempt`：A6xx 无硬件抢占，运行期没有调用者；
- 移植 `NativeGpuPerfCounters`：A7XX 专用；
- 追 120 Hz：`porting.toml:521` 已经用算术否掉了（1600×2560×4 @120Hz 需 1.83 GiB/s，
  而本类 GOP 拷贝实测 88.6–95.7 ms/14.7 MB ≈ 153 MiB/s）。

---

## 11. 未决问题

1. **中断组**：SPI 300 能否从 group 0 迁到 group 1？需要谁配合（secure 固件？UEFI？）——目前只有"不可达"的结论，没有"怎样可达"的方案。
2. **`native_adreno_set_restart` 默认关的原因**：是没来得及，还是有过失败轮次？记录里只有"从来没人调用过"的注释。
3. **我们的 fork 与 `FFT-infra/Adreno650` 的关系**：我们取自 `UniXNU/Adreno740`（它本身是 rebase），
   gts7xlwifi 取自 `FFT-infra a6xx-on-7fa13d4`。两条分支的最近共同祖先在哪、后续怎么合，没有查过。
4. **`sheng-` 前缀的去留**：上游正在把 `sheng-*` 改成裸名（`fb-gpu`、`gpu-addon`、`vsync`），
   我们还全用旧名；某天驱动只解析新名时会静默失效（alioth 记录过同类 bug）。
5. **像素级证明**：三台都没有。要做的话，最小代价是 `screencapture`/`SCREENSHOT` 的一次落库，
   或让 DPU 扫出的帧做一次 CRC 与 GPU 侧写入的缓冲对比。

---

## 附录 A：证据边界

- 上游进度全部来自设备记录页与仓库内容，**没有一条来自本板真机**；
- 三家"能跑"都是记录自述；j716f 的 60 FPS、gts7xlwifi 的 1436/1438 换页、alioth 的已知图正确，
  均无截图/回读/CRC 落库；
- 本板的"无中断"由两条独立证据支撑（`armcpu_spi_attach` 读回、1502 提交 → 2 中断），
  仍是推断 + 计数，不是对 GIC 组的直接读取；
- 本板的"GPU 写出的帧"由 `gpu wrote=1753` 与 `[direct scanout] flips` 对齐支撑，
  但同样没有像素级证明；
- 核心仓 20 个 submodule 在本机是空目录（gitlink），凡"核心是什么"的结论以提交历史为准
  （`d-core-vs-fork.md:6-24` 已在报告开头声明）。

## 附录 B：术语

| 词 | 含义 |
| --- | --- |
| IB | Indirect Buffer，GPU 命令缓冲 |
| PM4 | Adreno 的 type-4/type-7 包格式 |
| CP | Command Processor，GPU 的命令前端 |
| GMU | Graphics Management Unit，负责电源/频率/部分固件 |
| GX / CX | GPU 执行域 / 核心电源域 |
| ZAP | Adreno 的安全固件（SECVID） |
| SMMU / stage-1 | 系统级 MMU；stage-1 是 GPU 的 IOVA 翻译 |
| S2CR | SMMU 的 stream-to-context-register（流与上下文绑定） |
| ASID | 地址空间标识（每个进程 VM 一个） |
| fence 字 | `msm_rbmemptrs.fence`，在 `fence_iova+4`，GPU 写 seqno |
| direct scanout | DPU 直接取合成器 IOSurface，不经 CPU 拷贝 |
| hangcheck | 靠 fence 进度（而非中断）判断 GPU 卡死的线程 |

## 附录 C：材料索引

| 材料 | 位置 |
| --- | --- |
| 设备记录页原件（47 个） | `.research/gpu-survey/pages/` |
| 目录级普查（含 URL 与抓取日期） | `.research/gpu-survey/a-catalog.md` |
| alioth 逐行机制 + 实现方式专章 | `.research/gpu-survey/b-alioth.md` |
| gts7xlwifi 逐行机制 + T970 故障全案 | `.research/gpu-survey/c-gts7xlwifi.md` |
| 核心谱系 + 逐文件差异 + 死观测量 | `.research/gpu-survey/d-core-vs-fork.md` |
| 本板 GPU 记录 | `porting.toml` 的 `[[area]] id = "gpu"`（`:510-556`） |
| 本板直扫计划 | `docs/gpu-direct-scanout-plan.md` |
| 性能轮日志 | `.research/console-gpuperf2-2026-11-20.log` |
| 触控/门限轮日志 | `.research/console-gate1000-2026-11-20.log` |
