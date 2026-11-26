/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 相机阶段 2：把 SM8250 的相机域（CAMCC / GDSC / CCI 块）按级开起来并读回状态。写成寄存器回调上的可移植代码，
 * 结构照母本 PaganiCamera 的 CamBringup.h，顺序和数值由主机模型检查（Test/bringup.c）。
 * 母本的第一条规矩也是这里的第一条规矩：没开时钟的块一读就挂总线。dby-cam=N 决定走到哪一级：
 *
 *   级 1  步 101 勘察：GCC 的两条相机分支、四个脚的 CTL 和 IO。只读，一个寄存器都不写。
 *   级 2  步 102 开 GCC 的 camera_ahb 和 camera_xo（[K] Android 的 probe 一直开着的那两条），两条都起来才碰
 *         CAMCC，只读：第一次读可能挂住总线，所以上一行日志之后先等一拍再读。读到全 1 或硬件控制位就停。
 *   级 3  步 201 投 mmcx、mxc 到 0x180（母本记的 Android 开机投的 TURBO 档，绝不投 mx），CAMCC 的 sleep_clk
 *         和 core_ahb、cpas_ahb、cci_0_clk，titan GDSC 解除 collapse，三条 RCG 切 XO；逐个确认状态之后，
 *         才读 CCI_HW_VERSION。这一行是本板相机域真的活着的第一个证据。
 *   级 4  步 301 投 cam_vio（cmd-db 的 ldoc5，1800 mV）并把两条开关轨（GPIO 48/26/49）拉高；步 302 MCLK0 的
 *         RCG 切 XO（19.2 MHz，不开 PLL）并开分支、把 94 号脚设成 cam_mclk；步 303 复位脚先低后高放开传感器；
 *         步 304 CCI 复位、装节拍、全掩全清中断、设读 FIFO 阈值；步 305 在 master 0 / queue 0 上对
 *         0x10 / 0x20 / 0x34 各发一笔"读 0x0000 两字节"，谁 ACK 就打印读回来的值。三个都 NACK 是结果
 *         （传感器没上电或没这颗料），不是崩溃——所以那一步返回 DbcamErrNoSensor 而不是让整条阶梯假装成功。
 *
 * 和母本的一处结构差别写在这里而不是藏起来：母本的 guard_on 在第一次危险访问之前持住 UFS 日志写者
 * （pagani_panel_probe_quiet 的一位）。本仓的 BootProgress 家族没有这个写者门（全包 grep 只有不相干的
 * quiet 变量），所以这里只保留"上一行日志之后等一拍"的落盘语义，不假造一个没人看的位。
 *
 * 每步有步号，失败时 b->step 是负的步号，b->error 是错误码，并写一行日志。这里不映射内存，只经 DbcamHw
 * 回调访问；所有写都过 dbcam_write_allowed。 */
#ifndef DBY_CAM_BRINGUP_H
#define DBY_CAM_BRINGUP_H
#include <stdint.h>
#include "DbyCamBoard.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 每个窗口映射的字节数，内核据此映射，模型据此检查每次访问。顺序必须和 DbyCamWin 一致。 */
static const uint32_t kDbcamWinBytes[DbcamWinCount] = {DBCAM_GCC_BYTES, DBCAM_TLMM_SOUTH_BYTES, DBCAM_TLMM_NORTH_BYTES,
                                                       DBCAM_TLMM_WEST_BYTES, DBCAM_CAMCC_BYTES, DBCAM_CCI_BYTES};
static const uint64_t kDbcamWinBase[DbcamWinCount] = {DBCAM_GCC_BASE, DBCAM_TLMM_SOUTH_BASE, DBCAM_TLMM_NORTH_BASE,
                                                      DBCAM_TLMM_WEST_BASE, DBCAM_CAMCC_BASE, DBCAM_CCI0_BASE};

enum DbcamStep {
    DbcamStepSurvey = 101, DbcamStepCamcc = 102, DbcamStepDomain = 201,
    DbcamStepRail = 301, DbcamStepClock = 302, DbcamStepReset = 303, DbcamStepCciInit = 304, DbcamStepScan = 305,
};
enum {DbcamLevelMax = 4};
enum DbcamError {
    DbcamErrNone = 0, DbcamErrArgument = -22, DbcamErrLevel = 34,
    DbcamErrGcc = 1,             /* GCC 分支没起来：CAMCC 没读 */
    DbcamErrCamccValue,          /* CAMCC 读到不可能的值 */
    DbcamErrVote,                /* ARC 投票失败 */
    DbcamErrGdsc,                /* GDSC 不是预期状态或没上电 */
    DbcamErrRcg,                 /* RCG 更新没完成 */
    DbcamErrBranch,              /* 分支没起来：CCI 没读 */
    DbcamErrCciVersion,          /* 版本读到全 0 或全 1 */
    DbcamErrRail,                /* 轨道投票失败：没上电不碰 CCI */
    DbcamErrCciReset,            /* CCI 复位没回 ACK */
    DbcamErrCciTimeout,          /* 队列启动后没有 RD_DONE */
    DbcamErrCciError,            /* RD_DONE 之外还置了错误位 */
    DbcamErrNoSensor,            /* 三个地址全 NACK：这是结果，不是崩溃 */
};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned win, uint32_t offset);
    void (*write)(void *ctx, unsigned win, uint32_t offset, uint32_t value);   /* 带屏障 */
    void (*udelay)(void *ctx, unsigned us);
    void (*msleep)(void *ctx, unsigned ms);
    /* ARC：cmd-db 资源名、要的 vlvl，返回实际投的 hlvl。0 或负数。 */
    int (*arc)(void *ctx, const char *resource, uint32_t vlvl, uint32_t *hlvl);
    /* 稳压器：cmd-db 资源名（VRM）与要的毫伏，返回 0 或负数。级 4 只投 cam_vio。 */
    int (*rail)(void *ctx, const char *resource, uint32_t millivolts);
    void (*log)(void *ctx, const char *line);
} DbcamHw;

typedef struct {
    const DbcamHw *hw;
    unsigned level;
    int step, error;
    uint32_t gcc[2];
    uint32_t pin_ctl[4], pin_io[4];       /* 只读，级 4 才写回 */
    uint32_t gdscr, cfg_gdscr, cci_cbcr, cci_rcg, mclk_cbcr, mclk_rcg;
    uint32_t cci_version;
    uint32_t refused;           /* 闸门拒绝的写 */
    uint32_t mm_hlvl, mxc_hlvl;
    uint32_t rail_rc;           /* cam_vio 那笔投票的返回码 */
    uint32_t scan_ack;          /* 扫到几个地址应答（bit n = kDbcamScanAddr[n]） */
    uint32_t scan_id[DbcamScanCount];   /* 每个地址读回来的两字节，0 表示没 ACK */
    uint8_t pins_read;
} DbcamBringup;

/* 级的换算：1..4 是级，4 以上截成 DbcamLevelMax（4），0 是关。
 * 注意调用侧的默认值不是 0 —— DbyCamera.cpp 里缺席即 4（命令行没有余量再加参数）。 */
unsigned dbcam_level(unsigned value);
/* 跑到 b->level 这一级，失败就停下。返回 0、DbcamError 或负的 errno。hw 在 b 里。 */
int dbcam_run(DbcamBringup *b);
/* 闸门。 */
int dbcam_write_allowed(unsigned win, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
