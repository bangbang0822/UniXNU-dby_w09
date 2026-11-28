# Codex 交接：DBY-W09 UniXNU 构建推进（2026-11-29）

- 交接对象：接手本仓库的 Codex 主 Agent
- 交接人：本会话（DSH 主 Agent）
- 交接时分支：`codex/rebuild-snapshot-r2-sync`，HEAD `0bb91d7`
- 仓库路径：`C:\Users\BangBang\Documents\CodexZone`
- 依据：本会话提交、GitHub Actions run、仓库文件实际内容；未读取、未写入任何凭据

## 0. 交接范围与阅读顺序

本文只交接三件事：**构建链当前状态、本会话改了什么、下一步按什么顺序推进**。充电/PD 线单独放在第 7 节，属于“建议已写、代码未动”的状态。

建议阅读顺序：第 1 节现状 → 第 3 节阻塞因果 → 第 8 节下一步；需要复现时看第 9 节命令。

## 1. 现状快照

- 工作树：干净。
- 远端：GitHub `bangbang0822/UniXNU-dby_w09` 已同步到 `0bb91d7`；Gitee `origin` 仍停在 `4d1f4b4`，**落后 8 个提交**（本会话未推 Gitee）。
- 正在跑的构建：run `46167693966`（sha `0bb91d7`），交接时处于 `verified root inputs` 步骤；结论必须以该 run 的最终状态为准，不得沿用本文快照。
- 构建链已推进到的位置：bootstrap kernel 的**无归档链接**已通过；当前在验证 bootstrap tree 里**带归档的最终链接**能否通过（冷启动 stub 覆盖修复）。
- 记录门（Device documentation）已稳定通过：`46155961458`、`46156182637` 以及之后的 `46161572082`、`46167685580` 均 success。
- 尚未发生的事实：shipping kernel 链接从未成功；payload/package 从未在绿色构建上跑过。

## 2. 本会话提交清单

| SHA | 提交 | 作用 |
| --- | --- | --- |
| `3d220f7` | fix(ci): check porting record with pinned board core catalog | 记录门改为显式 checkout 板级 core `bangbang0822/UniXNU-core-dby-w09@f28ea590d6e0f4a912f256490c49300921a39032` 并直接跑 `scripts/porting-record.py check`，替代体内仍 checkout `UniXNU/UniXNU@main` 的复用 workflow |
| `be6de02` | docs: record missing Data seed cache on dispatched run | 记录 `46156006362` 因分支无 seed 缓存失败 |
| `548b003` | fix(build): gate full owner chain during bootstrap | bootstrap 门控改为环境级，覆盖完整 owner 链 12 个开关；删除过窄且自阻塞的旧块 |
| `bf3f46b` | docs: record bootstrap gate evidence | 新增 `docs/reviews/2026-11-28-bootstrap-gate.md` |
| `1029e5e` | docs: record independent review of bootstrap gate | 写入只读审核结论 |
| `5e1354f` | fix(build): defer sibling archive imports in camera gate | camera 归档不再要求 bootstrap kernel 定义兄弟归档提供的 5 个 RPMh 符号 |
| `682ff04` | docs: recommend charging and USB-PD execution order | 新增 `docs/charging-recommendation-2026-11-28.md` |
| `0bb91d7` | fix(build): cover cold first-pass archive symbols | 冷启动 `FirstPassStub` 覆盖 `native_legacy_visible(_sum)` 与 5 个 RPMh 符号 |

仍相关的前序提交：`cd2d818`（首版 bootstrap 门控，已被 `548b003` 取代）、`4d1f4b4`（记录门错误 core pin，已被 `3d220f7` 取代）、`e0733bb` + `114f549`（shipping kernel 步骤与失败记录）。

## 3. 构建阻塞的因果链（按发生顺序）

1. **记录门版本错配**（`46153746882`、`46154194890`）：复用 workflow 即使引用被 pin，体内仍 checkout 公共 main catalog，导致 `unknown component`。修复：`3d220f7` 显式 checkout 板级 core 并本地调用 checker。结果：`46155961458` success。
2. **seed 缓存分支隔离**（`46156006362`）：新分支没有可恢复的 `data.dbr`，工作流在 `require the preserved Data seed` 停止。处置：用 `init_data=true` 首装路径派发；该轮把 seed 缓存保存到了本分支（cache id `11245356338`），后续可复用。
3. **bootstrap 门控自阻塞且过窄**（`46156104185` 取消）：`cd2d818` 把 `XNU_OWNER_ROOT_TRUST` 降为 0，但 bootstrap defconfig 仍选 `CONFIG_XNU_OWNER_CACHE_MOUNT=y`，脚本依赖守卫会直接 exit；且 mount/manifest 等开关在更晚位置才读取，旧门控覆盖不到。修复：`548b003` 把门控提前到任何 provider 开关读取之前，并清零完整 owner 链。
4. **bootstrap 链接首次通过**（`46156668323`）：`548b003` 生效，bootstrap 脚本 15:35:06Z–15:39:36Z 无 undefined 输出。失败前移到 archive-29 `build-dby-camera.py`：它要求 kernel 镜像定义 5 个 RPMh 符号，而这 5 个由兄弟归档（含 `AdspProbe` 的 legacy 归档）在最终链接提供。修复：`5e1354f` 把 `SIBLING_ARCHIVE_IMPORTS` 从 kernel 符号门中豁免并记入报告。
5. **冷启动 stub 覆盖不足**（`46161581196`）：archive-29 通过；bootstrap tree 的最终链接失败，undefined 完整列表只有 6 个：`_native_legacy_visible_sum`（`BootLog.o`）、`_qcom_cmd_db_find`、`_qcom_rpmh_active_request`、`_qcom_rpmh_finish`、`_qcom_rpmh_level_index`、`_qcom_rsc_layout`（`libDbyCamera.a`、`libDbyPcie.a`）。原因：archive-31 legacy display 无 `build/xnu-prelink` candidate，按设计退化为 `FirstPassStub`，而 stub 只定义 3 个 legacy 入口。修复：`0bb91d7` 给 stub 增加两个 weak 计数器定义，并用 kernel C record 编译真实 `QcomRpmh.c` 进首轮归档。

这条链的核心事实：**冷启动是先链接（无归档）、再跑归档配方、最后带归档再链接一次**；因此任何“provider 在归档里”的引用，首轮链接必须由 stub 覆盖，或者延后到最终链接。

## 4. CI run 台账

| run | workflow | sha | 结论 | 首个阻塞 |
| --- | --- | --- | --- | --- |
| `46153746882` | Device documentation | `cd2d818` | fail | 记录门 `unknown component QcomXnuUsbDebug` |
| `46154194890` | Device documentation | `4d1f4b4` | fail | 记录门 `unknown component ShengCs35l43` |
| `46155961458` | Device documentation | `3d220f7` | success | — |
| `46156182637` | Device documentation | `be6de02` | success | — |
| `46156006362` | package | `3d220f7` | fail | 分支无 seed 缓存 |
| `46156104185` | package | `3d220f7` | cancelled | 静态发现 `cd2d818` 自阻塞，取消避免空跑 |
| `46156668323` | package | `548b003` | fail | bootstrap 链接通过；archive-29 camera 符号门 |
| `46161581196` | package | `682ff04` | fail | 最终链接缺 6 个冷 stub 符号 |
| `46167693966` | package | `0bb91d7` | 交接时 in progress | 以最终状态为准 |
| `46157410059` / `46161572082` / `46167685580` | Device documentation | 对应 sha | success | — |

Run 直接链接：`https://github.com/bangbang0822/UniXNU-dby_w09/actions/runs/<id>`。

Seed 缓存：key `data-seed-v1-c0ea247cb462f7d6-ipados-17.7.11-21H461`，cache id `11245356338`，ref 本分支，创建于 `2026-11-28T15:40:56Z`。`46161581196`、`46167693966` 均恢复成功。

可用的确定性 seed 备源（已实测哈希，未接入工作流）：仓库 `bangbang0822/dby-w09-data-seed` 的 release `data-seed-base-2026-11-18`，资产 `data-final.dbr`，大小 1,261,728 字节，sha256 `1039dd44d89d2e60ce8e122e5b14e59715c00b9240849de59fccc7e2a6437c5b`，与 `porting.toml` 记录一致。

## 5. 未决缺陷与风险

- **shipping 链接未验证**：历史上 undefined 集合为 `_native_legacy_visible`、`_native_legacy_visible_sum`、`_qcom_cmd_db_find`、`_qcom_rpmh_*`、`_qcom_rsc_layout`。`0bb91d7` 只修了 bootstrap tree 的最终链接；shipping tree 是否复用真实 legacy 归档，要看 `46167693966` 后续步骤。
- **审核非阻塞项**（只读审核已确认无阻塞缺陷）：
  1. `provided_symbols` 是手写声明，未用 `llvm-nm` 实测，名字漂移不会让构建失败但会让报告失真。
  2. `build-dby-camera.py` 注释把“最后一次带归档 link”写成“shipping kernel”，首次 shipping pass 同样会 deferred。
  3. `SIBLING_ARCHIVE_IMPORTS` 与 `EXTRA_IMPORTS` 重复五个名字，建议由同一常量派生。
  4. 冷分支缺 `QcomRpmh.c` 时只 print 不 fail（已加 `qcom_rpmh_compiled` 字段，但未改成硬失败）。
  5. 既有缺口：`native_legacy_display_probe` / `_late_probe` 的生产者在本仓之外，cold 树里由谁解析需在构建树上用 `llvm-nm` 确认。
- **seed 供应单点**：目前依赖分支缓存；release 资产回退未落地，工作流在缓存缺失时仍会停或重建 fixture。
- **Gitee 镜像落后** 8 个提交。
- **不可声称**：任何“shipping 已通过”“22.5 W 已实现”的说法都还没有证据。

## 6. 本机环境与工具坑

- 仓库：`C:\Users\BangBang\Documents\CodexZone`；板级 core（公共 main 形态）：`C:\Users\BangBang\Documents\UniXNU`。
- Windows git 原缺失，现已装 PortableGit `2.56.1`：`C:\Tools\PortableGit\cmd\git.exe`，写入用户级 PATH。当前 DSH 宿主进程的环境块是旧的，直接在命令里调 `git` 可能仍找不到；在单条命令内前置 PATH 或使用 WSL。
- WSL：Ubuntu-24.04，git `2.43.0`，python `3.12`。本会话的仓库操作走 WSL：
  `wsl.exe -e bash -lc "cd /mnt/... && git ..."`（注意：不要把这条 WSL 路径写进提交文件，`check-repository.py` 的 host-path 规则会命中其中的用户目录段）。
- GitHub CLI：`C:\Program Files\GitHub CLI\gh.exe`，`2.106.0`，已登录 `bangbang0822`。推送凭据通过一次性 credential helper + 环境变量传入，不落盘、不打印。
- `check-repository.py` 坑：Windows Python 找不到 git 时会回退 `rglob`，把 gitignore 的 `.cache` 误报成 4 项失败；git 可达时 491 项全过。提交前必须以“git 可达”的方式跑。
- pre-push 钩子会跑 491 项检查并校验 `github/<branch>..HEAD` 的 commit 标题，失败即拒绝推送。
- `package.yml` 只有 `workflow_dispatch`，push 不会触发；`init_data` 默认 false，缓存缺失时会在 seed 步骤停。
- Actions cache 按分支隔离；cache 命中后步骤 6/7 在 1 秒内完成。
- 记录门复用 checker 的固定路径：需要在临时目录放 `bangbang0822/UniXNU-core-dby-w09@f28ea590…` 的 `scripts/porting-record.py` 与 `components.toml`；本会话用 `C:\Users\BangBang\AppData\Local\Temp\core-verify-f28\run-gate.sh <base-sha>` 预演。
- 提交文件的 host-path 规则：单反斜杠的 `C:\...` 当前可通过；`C:/...`、`/Users/...`、`/home/...` 会被拦。写文档时避开后三者。

## 7. 充电/PD 线交接

状态：**只有建议文档，没有代码改动、没有刷机、没有功率结论**。

- 规格：`docs/charging-usb-pd-spec-2026-11-26.md`（标准 PD 固定 PDO，输入 ≤22.5 W）、`docs/charging-spec-2026-11-24.md`（SMB5/APSD/FSA9685 与 ICL 阶梯）、`docs/charging-baseline-2026-11-26.md`（CDC 基线，只有 SDP 500 mA 与 DCP 标签净放电）、`docs/charging-huawei-private-protocol-prep-2026-11-28.md`（私有协议未实现，先只读）。
- 本板事实：PM8150B SMB5 提供电池遥测与 ICL；端口类型不由 APSD 决定（`usb-thd-bc-12` 关掉 BC1.2），而来自 FSA9685（I2C `0x26`，类型寄存器 `0x08`）；`package/DbyBattery.cpp:326` 的阶梯为 500/1200/1500/2300 mA，最高档 2.3 A 不是 PD 档。
- 同机 Linux `dby-chg`（pin `3d82fb7d`）：Type-C/PD PHY 归 `qcom_pmic_tcpm`，充电器通过 supplier `USB_TYPE` 分类；固定 9 V 路径需要 `0x1344=02 + 0x1342=01 + 0x1366=01 + 0x1370` 目标；`pd9v_max_ma` 默认 2500、范围 1500–3000；热降 250 mA、floor 1500 mA；<3.5 V 限 1 A、≥3.6 V 释放；实机 9 V/1.5 A 15 分钟 81→88%；0%/2.89 V 请求 9 V/3 A 会在 PS_RDY 前硬复位。
- UniXNU 现状：无 PD 状态机；core 的 `drivers/` 没有 TCPM/PD 组件；`QcomDwc3` 面向 PM8550B/eUSB2/iPadOS 27/sheng，不是本板替换件。
- 建议文档结论：先做「5 V 持续充电基线 + 只读 PD/Type-C 遥测」；sink capability 未证前可辩护上限 13.5 W（stock 5 V/2 A、9 V/1.5 A）；不移植 Linux 私有写序列；不提高 ICL 阶梯；不做 SCP/PPS/40 W；unknown 与 0 必须区分。
- 待决策：PD owner 归属（板级最小只读适配 vs 向 core 提通用组件）、首个功率目标（先 5 V，还是直接按 13.5 W 设计 P2）、外部 USB-PD 仪表来源、FSA9685 总线取证方式（DTBO 还是逐 QUP 只读探测）。

## 8. 下一步顺序（接手后按此推进）

1. 读 `46167693966` 的最终结论（`gh run view 46167693966 --json status,conclusion,jobs`，失败再 `--log-failed`）。若仍在跑，等它结束，不要并发派新 run。
2. 若最终链接仍失败：只取新 run 的 undefined 原文，不沿用本文第 5 节的旧集合；按“冷 stub 覆盖 vs 归档注入”二分定位。
3. 若最终链接通过：确认 workflow 继续复制 prelink candidate、跑 shipping 的 `defconfig/tree/build kernel`，重点看 shipping link 的 `final kernel archives` 与 undefined。
4. 构建绿后记录：kernel/payload/addon/seed 的 SHA-256、run id、archive 报告；更新 `docs/reviews/cold-build-failure-log.md` 与 `porting.toml` 对应 area。
5. 再处理非阻塞项（优先 `llvm-nm` 校验 `provided_symbols`、camera 注释与常量派生）。
6. Gitee `origin` 同步 8 个提交（需 Gitee 凭据，当前未配置）。
7. PD 线等 owner 决策后再动代码；动之前先补只读遥测，不写 PD 控制寄存器。

## 9. 验证命令

```powershell
# 仓库检查（git 可达；WSL 方式）
wsl.exe -e bash -lc "cd <repo-wsl-path> && python3 scripts/check-repository.py --quiet"

# 脚本语法
wsl.exe -e bash -lc "cd <repo-wsl-path> && bash -n scripts/build-xnu-4k-baseline.sh"
wsl.exe -e bash -lc "cd <repo-wsl-path> && python3 -m py_compile scripts/build-dby-camera.py scripts/build-gop-legacy-display.py"

# 记录门预演（临时 checker，参数为上一推送 sha）
wsl.exe -e bash C:\Users\BangBang\AppData\Local\Temp\core-verify-f28\run-gate.sh <base-sha>

# CI 取证
& 'C:\Program Files\GitHub CLI\gh.exe' run view <run-id> --repo bangbang0822/UniXNU-dby_w09 --json status,conclusion,jobs
& 'C:\Program Files\GitHub CLI\gh.exe' run view <run-id> --repo bangbang0822/UniXNU-dby_w09 --log-failed
```

推送走已验证的一次性 helper（脚本在临时目录，凭据只经环境变量，不写文件）。

## 10. 禁止与合规

- 不添加任何 SoC/板级 provider 的 weak stub 来“让链接通过”；冷启动 stub 是既有机制，只覆盖它声明的符号。
- 不把 Linux 的 PD 控制写序列（`0x1344/0x1342/0x1366/0x1370`）搬进 UniXNU。
- 不提高 ICL 阶梯、不请求 3 A/27 W、不宣称 22.5 W。
- 不读取、不写入、不转交任何凭据；聊天中出现过的 token 应视为已泄露并撤销。
- 不臆造 run 结果或日志；结论必须能指向 run id 或 `文件:行号`。
- 不直接推主干；用 `codex/<意图>` 分支，conventional commit，合并前跑仓库检查。

## 11. 附：关键文件与行号索引

- bootstrap 门控：`scripts/build-xnu-4k-baseline.sh:92`（`XNU_BOOTSTRAP=1` 的整链清零）；归档列表仅在 `:604` 的 `BOOTSTRAP=0` 分支组装。
- camera 豁免：`scripts/build-dby-camera.py:73`（`SIBLING_ARCHIVE_IMPORTS`）；报告字段在 `:196` 附近。
- 冷启动 stub：`scripts/build-gop-legacy-display.py:48`（weak 计数器）、`:56`（编译 `QcomRpmh.c`）；stub 分支在 `:96` 退出。
- 记录门：`.github/workflows/porting-record.yml`（显式 checkout + checker 调用）。
- 记录内容：`docs/reviews/cold-build-failure-log.md`（本会话四行）、`docs/reviews/2026-11-28-bootstrap-gate.md`、`docs/charging-recommendation-2026-11-28.md`。
- 记录字段：`porting.toml` 的 configuration（记录门与 bootstrap 门控）、power（camera 冷启动门控）、display（legacy 冷启动 stub）。
