/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* DBY-W09（华为 MatePad 11，SM8250/kona）相机阶段 2 的硬件事实，对应母本 PaganiCameraBoard.h（一加 13T，SM8750）。
 * 这里只有数字和它们的来源，不含任何 GPL 或厂商代码。来源标记：
 *   [D]    本机 stock 设备树片段（DTBO）与原厂 dtsi：CCI0 的引脚和节拍、MCLK/RESET 脚、cam_vio 的稳压器标签
 *   [K]    原厂内核源码的时钟表：drivers/clk/qcom/camcc-kona.c、gcc-kona.c 的 cmd_rcgr / enable_reg / 位
 *   [U]    主线 Linux 的事实：i2c-qcom-cci.c（CCI 寄存器和队列命令）、gdsc.c、clk-rcg2.c、clk-branch.c、pinctrl-msm.c
 *   [T]    本仓已验收的 package/DbyTorch/DbyTorchRegs.h（同一颗 SoC 的 SPMI v5 仲裁器、TLMM 窗口、VRM 寄存器）
 *   [R]    本仓已验收的 package/QcomRpmh.h（cmd-db 查找与主动请求的寄存器写法）
 *   [推断] 写明了依据，上机才算数
 *
 * 和母本不同的三处是本板的事实，不是我改的路线：
 *   1 kona 没有 cambistmclkcc 块，MCLK0 的 RCG 和分支就在 CAMCC 里（0x5000 / 0x5018），所以只有四个窗口。
 *   2 kona 的 CAMCC 没有 cam_cc_top_ahb、cam_cc_top_fast_ahb、cam_cc_drv_ahb、cam_cc_drv_xo 这四条，也没有
 *     相机 BIST AHB 分支；能对上母本名单的是 cpas_ahb（0xc0f4）、core_ahb（0xc150）、sleep_clk（0xc188）。
 *     cam_cc_gdsc_clk 的 enable_reg 没测到，级 3 不碰它。
 *   3 titan_top_gdsc 的父时钟是 gcc_camera_ahb（[D] clocks = <&clock_gcc 11>），所以级 2 不开 GCC 分支就
 *     绝不碰 CAMCC 这条规矩在本板更硬。
 *
 * 级 4（轨道、复位放开、MCLK、CCI 交易、扫传感器）原来记了"还缺三样"，现在两样已经在内核源码里查到、一样要上机扫：
 *   a 轨道：cam_vio 就是 cmd-db 里的 "ldoc5"（[D] kona-regulators.dtsi:660 的 qcom,resource-name），电压与使能
 *     两个寄存器偏移在 [K] qcom-rpmh-regulator.c:35-36；另外三条开关轨是 GPIO 48/26/49，不经 PMIC。
 *   b 引脚：三段基址、位宽和功能序号都按 [K] pinctrl-kona.c / pinctrl-msm.c 核过（见下面引脚表）。
 *   c 传感器从地址与 ID：只能上机扫（0x10 / 0x20 / 0x34），扫到就打印读回来的两字节，扫不到也是答案。
 * 所以级 4 发的是"上电 → MCLK 19.2 MHz → 放开复位 → CCI 读 ID"这条真实链路，扫不到传感器不假装成功。 */
#ifndef DBY_CAM_BOARD_H
#define DBY_CAM_BOARD_H
#include <stdint.h>

/* 窗口。TLMM 在 kona 按段编址（[K] pinctrl-kona.c:20-25）：地址 = 0x0f000000 + 段偏移 + 0x1000 × 脚号，
 * IO 寄存器在其后 +4。本板相机用到的脚跨三段，所以一个窗口盖不住（之前只有一个 0x900000 的窗口，那是 NORTH 段
 * 的偏移，漏了 0x0f000000 这个基址，也没算 SOUTH/WEST）：
 *   SOUTH 26（后摄 AVDD 开关）、48（后摄 DVDD 开关）、49（后摄 DVDD）
 *   NORTH 93（后摄复位）、94（MCLK0）、101/102（CCI0 SCL/SDA）
 *   WEST  153（前摄 VDIG 开关）、154（AFVDD 开关，级 4 不用）
 * 每个窗口只盖到名单里最大的那个脚，别的一段一个字节都不读。 */
#define DBCAM_GCC_BASE 0x00100000ull
#define DBCAM_GCC_BYTES 0xc0000u          /* [K] 最大的相机分支 enable_reg 是 0xb040，窗口要盖到它 */
#define DBCAM_TLMM_SOUTH_BASE 0x0f500000ull   /* 0x0f000000 + SOUTH 0x00500000；脚 49 → +0x31000 */
#define DBCAM_TLMM_SOUTH_BYTES 0x32000u
#define DBCAM_TLMM_NORTH_BASE 0x0f900000ull   /* 0x0f000000 + NORTH 0x00900000；脚 102 → +0x66000 */
#define DBCAM_TLMM_NORTH_BYTES 0x67000u
#define DBCAM_TLMM_WEST_BASE 0x0f100000ull    /* 0x0f000000 + WEST 0x00100000；脚 153 → +0x99000 */
#define DBCAM_TLMM_WEST_BYTES 0x9a000u
#define DBCAM_CAMCC_BASE 0x0ad00000ull    /* [D] clock-controller@ad00000 */
#define DBCAM_CAMCC_BYTES 0x20000u
#define DBCAM_CCI0_BASE 0x0ac4f000ull     /* [D] cam_cci0 */
#define DBCAM_CCI_BYTES 0x1000u

/* ARC 走 apps RSC 和这颗机子的 cmd-db，两个地址都是本仓已经在用的：AdspProbe.cpp:57 与 AudioProbe.cpp:784。 */
#define DBCAM_RSC_DRV 0x18220000ull
#define DBCAM_RSC_TCS_BLOCK 0xd00u
#define DBCAM_RSC_BYTES 0x10000u
#define DBCAM_CMD_DB 0x80860000ull
#define DBCAM_CMD_DB_BYTES 0x20000u

enum DbcamWin {DbcamWinGcc, DbcamWinTlmmSouth, DbcamWinTlmmNorth, DbcamWinTlmmWest, DbcamWinCamcc, DbcamWinCci,
               DbcamWinCount};

/* CBCR 和 RCG 的位 [U]。 */
#define DBCAM_CBCR_ENABLE 0x1u
#define DBCAM_CBCR_HW_CTL 0x2u
#define DBCAM_CBCR_OFF 0x80000000u
#define DBCAM_RCG_UPDATE 0x1u
#define DBCAM_RCG_SRC_MASK 0x700u            /* CFG [10:8] */
#define DBCAM_RCG_DIV_MASK 0x1fu             /* CFG [4:0] */
#define DBCAM_RCG_MODE_MASK 0x3000u
/* [K] clk-rcg.c:334 calc_rate()：rate /= pre_div + 1。所以 CFG 的分频字段 0 才是"不分频"，1 是二分频。
 * 之前这里写 0x1 并注成"不分频"，那会把 XO 再砍一半（9.6 MHz），CCI 和 MCLK 都会跟着错，改成 0。
 * [K] clk-rcg2.c:540：带 HW_CLK_CTRL_MODE 的 RCG（camcc 这几条都是）配置时 CFG 的 bit 20 由软件置 1。 */
#define DBCAM_RCG_SW_OWNED 0x100000u
#define DBCAM_RCG_CFG_XO DBCAM_RCG_SW_OWNED  /* parent_map 里 XO 是 0，分频 0，模式 0 */
/* [K] clk-rcg2.c:724 clk_rcg2_enable()：先写 CMD_ROOT_EN 再写 CMD_UPDATE 才算把这条 RCG 打开。 */
#define DBCAM_RCG_CMD_ROOT_EN 0x2u

/* GCC：[K] gcc-kona.c 里两条相机分支的 enable_reg，值都带 BRANCH_HALT_DELAY；camera_xo 是 CLK_IS_CRITICAL，
 * Android 的 probe 一直开着它们，XNU 没跑那段 probe，所以要自己确认。本板没有相机 BIST AHB 分支。 */
enum {
    DbcamGccCameraAhb = 0xb008,
    DbcamGccCameraXo = 0xb040,
};

/* CAMCC：[K] camcc-kona.c，相对 0xad00000。 */
enum {
    DbcamCcGdscr = 0xc144,                  /* [D] qcom,gdsc@ad0c144；+4 是 CFG_GDSCR（[D] support-cfg-gdscr） */
    DbcamCcCfgGdscr = 0xc148,
    DbcamCcSlowAhbRcg = 0x7058,             /* cam_cc_slow_ahb_clk_src：core_ahb 的父 */
    DbcamCcFastAhbRcg = 0x703c,             /* cam_cc_fast_ahb_clk_src */
    DbcamCcCciRcg = 0xc0bc,                 /* cam_cc_cci_0_clk_src：安卓分到 37.5 MHz，这一阶段不开 PLL，用 XO */
    DbcamCcCoreAhb = 0xc150,
    DbcamCcCpasAhb = 0xc0f4,
    DbcamCcSleepClk = 0xc188,
    DbcamCcCci0 = 0xc0d4,                   /* cam_cc_cci_0_clk */
    DbcamCcMclkRcg = 0x5000,                /* cam_cc_mclk0_clk_src， kona 在 CAMCC 里，不在 cambistmclkcc */
    DbcamCcMclkCbcr = 0x5018,
    DbcamRcgCfg = 4,
};

/* GDSC 的位 [U]；有 CFG_GDSCR 时状态也在那里。 */
#define DBCAM_GDSC_SW_COLLAPSE 0x1u
#define DBCAM_GDSC_HW_CONTROL 0x2u
#define DBCAM_GDSC_PWR_ON 0x80000000u
#define DBCAM_GDSC_CFG_UP_DONE 0x10000u

/* 引脚 [D]。后摄：94 cam_mclk、101/102 cci SCL/SDA、93 复位、48 DVDD 开关、26 AVDD 开关、49 DVDD。
 * [K] pinctrl-kona.c:26 的 PINGROUP 把 funcs[0] 固定成 msm_mux_gpio，f1 是 funcs[1]：
 * gpio94 的 f1 是 cam_mclk、gpio101/102 的 f1 是 cci_i2c → 这三个写 mux 1；93/26/48/49 的 f1 都不是它们要的功能
 * （93 是 qup_l6，26 是 qup8，48/49 是 qup16），它们走 gpio 功能 → mux 0。
 * TLMM 位 [K] pinctrl-kona.c:49-56：[1:0] 上下拉（bit0 上拉、bit1 下拉），[4:2] 功能（3 位），[8:6] 驱动，[9] 输出使能；
 * IO 寄存器 bit 1 是输出电平。驱动档 [U] pinctrl-msm.c：0=2mA 1=4mA 2=6mA 3=8mA。 */
enum {
    DbcamPinDvddSw = 48, DbcamPinAvdd = 26, DbcamPinDvdd = 49,      /* SOUTH */
    DbcamPinReset = 93, DbcamPinMclk0 = 94,                         /* NORTH */
    DbcamPinScl0 = 101, DbcamPinSda0 = 102,                         /* NORTH，[D] 注释是 "CLK, DATA" */
    DbcamPinVdigFront = 153,                                        /* WEST，级 4 只记录不用 */
    DbcamPinStride = 0x1000,
};
#define DBCAM_TLMM_PULL_MASK 0x3u
#define DBCAM_TLMM_PULL_UP 0x1u
#define DBCAM_TLMM_PULL_DOWN 0x2u
#define DBCAM_TLMM_FUNC_SHIFT 2
#define DBCAM_TLMM_FUNC_MASK (0x7u << 2)
#define DBCAM_TLMM_DRV_SHIFT 6
#define DBCAM_TLMM_DRV_MASK (0x7u << 6)
#define DBCAM_TLMM_OE 0x200u
#define DBCAM_TLMM_IO_OUT 0x2u
enum {DbcamDrv2mA = 0, DbcamDrv8mA = 3};
enum {DbcamFuncGpio = 0, DbcamFuncCamMclk = 1, DbcamFuncCciI2c = 1};

/* 轨道 [D]。只有 I/O 轨走 PMIC：cam_vio = pm8150a_l5，1.8 V。三条开关轨是 GPIO 直接使能的负载开关
 * （[D] gpio-req-tbl-label CAM_DVDD_SW / CAM_REAR_AVDD / CAM_REAR_DVDD → 48 / 26 / 49），不需要 PMIC 资源。
 * [D] kona-regulators.dtsi:660 rpmh-regulator-ldoc5 的 qcom,resource-name 就是 cmd-db 里的名字（之前记成"没记录"，
 * 其实是这两个文件里一直写着）；[K] qcom-rpmh-regulator.c:35-36,161：电压寄存器 +0x0 收 mV，使能 +0x4 收 1。 */
#define DBCAM_RAIL_VIO "ldoc5"
enum {DbcamRailVioMv = 1800, DbcamRailVoltageOff = 0x0, DbcamRailEnableOff = 0x4};

/* ARC [T] [R]：0x180 = RPMH_REGULATOR_LEVEL_TURBO（[K] qcom,rpmh-regulator-levels.h:26）。
 * 本板只有一条相机要投的域资源：mmcx.lvl（[K] kona-regulators.dtsi:503，PM8150A S4+S5 = VDD_MMCX）。
 * 11-10 那次上机日志 [cam] arc mm=5 mxc=0 rc=-2 里的 mxc.lvl 是从别的平台抄来的名字，本板 cmd-db 里没有它
 * （[K] 全树 grep "mxc.lvl" 零命中，只有 mmcx.lvl），查不到就以 QcomRpmhMissing 中止了整条阶梯 —— 已删。
 * MMCX 的父轨是 mx.lvl（[K] :504 parent-supply = VDD_MX_MMCX_SUPPLY_LEVEL，PM8150A S3，:6-8），但本板
 * 绝不主动投它：GPU 那条路径把它长期投在表顶，父轨本就够高，再去抬只会和它抢同一个 TCS。 */
#define DBCAM_ARC_MM "mmcx.lvl"
enum {DbcamArcLevel = 0x180};

/* CCI [K]（techpack/camera/drivers/cam_sensor_module/cam_cci/cam_cci_hwreg.h 与本板 stock 的 msm_cci 表一致）。
 * 只用 master 0、queue 0（queue 0 深 128 字，queue 1 深 32 字）。级 ≤3 只读 CCI_HW_VERSION，不发交易。 */
enum {
    DbcamCciVersion = 0x000,
    DbcamCciReset = 0x004, DbcamCciResetAll = 0x0f73f3f7, DbcamCciResetM0 = 0x3f1,
    DbcamCciQueueStart = 0x008,
    DbcamCciHalt = 0x034, DbcamCciHaltM0 = 1,
    DbcamCciSclCtl = 0x100, DbcamCciSdaCtl0 = 0x104, DbcamCciSdaCtl1 = 0x108, DbcamCciSdaCtl2 = 0x10c, DbcamCciMiscCtl = 0x110,
    DbcamCciReadData = 0x118, DbcamCciReadLevel = 0x11c,
    DbcamCciRdThreshold = 0x120, DbcamCciRdThresholdValue = 0x30,
    DbcamCciExecWords = 0x300, DbcamCciCurWords = 0x304, DbcamCciReportStatus = 0x30c, DbcamCciLoadData = 0x310,
    DbcamCciIrqGlobalClear = 0xc00, DbcamCciIrqMask0 = 0xc04, DbcamCciIrqClear0 = 0xc08, DbcamCciIrqStatus0 = 0xc0c,
    DbcamCciIrqMask1 = 0xc10, DbcamCciIrqClear1 = 0xc14, DbcamCciIrqStatus1 = 0xc18,
};
/* [K] cam_cci_hwreg.h：全掩、全清、全局清一次，再把读 FIFO 阈值设成 0x30 —— 和 cam_cci_soc.c:176-186 一样。 */
enum {DbcamCciIrqMaskAll = 0x7fff7ff7, DbcamCciIrqMask1All = 0x00110000};
#define DBCAM_CCI_IRQ_RD_DONE 0x1u
#define DBCAM_CCI_IRQ_REPORT 0x10u
#define DBCAM_CCI_IRQ_RESET_DONE 0x1000000u
#define DBCAM_CCI_IRQ_HALT_ACK 0x2000000u
#define DBCAM_CCI_IRQ_NACK_M0Q0 0x8000000u
#define DBCAM_CCI_IRQ_ERROR 0x18000ee6u      /* master 0 的 NACK、错误命令、溢出（含 NACK_M0Q0） */
#define DBCAM_CCI_IRQ_ENABLE (DBCAM_CCI_IRQ_RD_DONE | DBCAM_CCI_IRQ_REPORT | DBCAM_CCI_IRQ_RESET_DONE | \
                              DBCAM_CCI_IRQ_HALT_ACK | DBCAM_CCI_IRQ_ERROR)
enum {DbcamCciCmdSetParam = 1, DbcamCciCmdLock = 6, DbcamCciCmdUnlock = 7, DbcamCciCmdReport = 8,
      DbcamCciCmdWrite = 9, DbcamCciCmdRead = 10, DbcamCciCmdWriteNoStop = 11,
      DbcamCciReportIrq = 1 << 8, DbcamCciRetries = 3, DbcamCciRetryShift = 16, DbcamCciSidShift = 4};
/* [K] cam_cci_core.c:972-1012 的读交易装字法（master 0 / queue 0）：
 *   SET_PARAM = cmd | sid << 4 | retries << 16 | id_map << 18
 *   WRITE_DISABLE_P = cmd | addr_type << 4 | addr[7:0] << 16 | addr[15:8] << 8   （addr_type = 2）
 *   READ = cmd | num_byte << 4
 *   LOCK / UNLOCK 只有命令字。之后 EXEC_WORD_CNT = CUR_WORD_CNT，QUEUE_START = 1 << (master*2 + queue)。 */
enum {DbcamCciAddrTypeWord = 2, DbcamCciAddrTypeByte = 1, DbcamCciScanBytes = 2};
/* 上机扫描的 7 位从地址。设备树没有 reg，工厂包里的 sensormodule 是加密的，所以扫最常用的三个：
 * Sony/OV 系 0x10、0x20 与 Samsung 系 0x34（[推断]，扫到谁都打印出来，扫不到也是答案）。 */
enum {DbcamScanCount = 3};
static const uint8_t kDbcamScanAddr[DbcamScanCount] = {0x10, 0x20, 0x34};
/* 读 ID 的寄存器地址。0x0000 是绝大多数 sensor 的 chip-id 高位（Sony 在 0x0016、Samsung 在 0x0000），
 * 这一级不判 ID 对不对，只判有没有 ACK、读回来是什么。 */
enum {DbcamScanReg = 0x0000};

/* [D] kona 设备树 i2c_freq_100Khz 的节拍数（37.5 MHz 下算的）。级 3 把 CCI 的 RCG 切到 XO 19.2 MHz，
 * 节拍数不变，总线约 50 kHz：所有最小时间只变长，不违反 I²C。 */
typedef struct {uint16_t thigh, tlow, tsu_sto, tsu_sta, thd_dat, thd_sta, tbuf; uint8_t scl_stretch, trdhld, tsp;} DbcamCciTiming;
static const DbcamCciTiming kDbcamCciSlow = {201, 174, 204, 231, 22, 162, 227, 0, 6, 3};

/* CCI 时序寄存器怎么装节拍数 [K]：本机内核 drivers/media/platform/msm/camera_v2/sensor/cci/msm_cci.c:131-144，
 * 逐条对照过。之前这里只有节拍，没有打包规则，所以级 4 写不出 SCL_CTL：
 *   SCL_CTL   = thigh   << 16 | tlow
 *   SDA_CTL_0 = tsu_sto << 16 | tsu_sta
 *   SDA_CTL_1 = thd_dat << 16 | thd_sta
 *   SDA_CTL_2 = tbuf
 *   MISC_CTL  = scl_stretch << 8 | trdhld << 4 | tsp
 * 下面五个值由 kDbcamCciSlow 直接算出。节拍是按 37.5 MHz 排的，级 3 把 CCI 的 RCG 切到 XO 19.2 MHz 之后
 * 时钟变慢、总线约 50 kHz，所有最小时间只变长，不违反 I²C，所以节拍数不必改。 */
enum {
    DbcamCciSclCtl100k = (201 << 16) | 174,      /* 0x00c900ae thigh 201 / tlow 174 */
    DbcamCciSdaCtl0_100k = (204 << 16) | 231,    /* 0x00cc00e7 tsu_sto 204 / tsu_sta 231 */
    DbcamCciSdaCtl1_100k = (22 << 16) | 162,     /* 0x001600a2 thd_dat 22 / thd_sta 162 */
    DbcamCciSdaCtl2_100k = 227,                  /* 0x000000e3 tbuf 227 */
    DbcamCciMiscCtl100k = (0 << 8) | (6 << 4) | 3, /* 0x00000063 stretch 关 / trdhld 6 / tsp 3 */
};

#endif
