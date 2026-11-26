/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * 结构、日志写法和"先落盘再碰危险块"的规则沿用母本 PaganiCamera 的 CamBringup.c，窗口和偏移换成本板的事实
 * （DbyCamBoard.h 标了每一条的来源）。级 4 的每一步都在这一个文件里，没有藏到别处：
 *   301 轨道 → 302 MCLK → 303 复位 → 304 CCI 初始化 → 305 扫地址读 ID。
 * 顺序不能换：没上电的传感器不会应答，没时钟的 CCI 不会走完一笔交易，没放开复位的 sensor 连 ID 都不给。 */
#include "DbyCamBringup.h"
#include "DbyCamText.h"

enum {
    kBranchPolls = 200, kBranchStepUs = 10,
    kGdscPolls = 500, kGdscStepUs = 10,
    kRcgPolls = 1000, kRcgStepUs = 10,
    /* 上一行日志由 BootLog 同步写完；这一拍只覆盖慢总线，母本用它换掉的是它那边的 UFS 写者门，本仓没有那个门。 */
    kSettleMs = 50,
    kCoreAhbDelayUs = 10,
    /* 轨道与传感器的拍子：开关轨开完等 3 ms 让负载开关的输出建立；复位低 2 ms、放开后 5 ms 是常见 sensor 的
     * 上电时序上限（[推断]，取宽一点，多等不花钱）。 */
    kRailSettleMs = 3, kResetLowMs = 2, kResetBootMs = 5,
    /* 一笔 CCI 交易最多等 20 ms：100 kHz 下读两字节连地址加开销不到 1 ms，留 20 倍余量。 */
    kCciPolls = 2000, kCciStepUs = 10,
};

/* 引脚表：窗口、脚号、要写进 CTL 的功能/驱动/上下拉。前四个是级 1 勘察要打印的，后三个是级 4 的开关轨。 */
typedef struct {
    unsigned win;
    uint32_t gpio;
    uint8_t func, drv, pull;
    const char *name;
} DbcamPin;
static const DbcamPin kPins[] = {
    {DbcamWinTlmmNorth, DbcamPinMclk0, DbcamFuncCamMclk, DbcamDrv8mA, 0, "mclk0"},
    {DbcamWinTlmmNorth, DbcamPinScl0, DbcamFuncCciI2c, DbcamDrv2mA, DBCAM_TLMM_PULL_UP, "scl0"},
    {DbcamWinTlmmNorth, DbcamPinSda0, DbcamFuncCciI2c, DbcamDrv2mA, DBCAM_TLMM_PULL_UP, "sda0"},
    {DbcamWinTlmmNorth, DbcamPinReset, DbcamFuncGpio, DbcamDrv8mA, 0, "reset"},
    {DbcamWinTlmmSouth, DbcamPinDvddSw, DbcamFuncGpio, DbcamDrv2mA, 0, "dvddsw"},
    {DbcamWinTlmmSouth, DbcamPinAvdd, DbcamFuncGpio, DbcamDrv2mA, 0, "avdd"},
    {DbcamWinTlmmSouth, DbcamPinDvdd, DbcamFuncGpio, DbcamDrv2mA, 0, "dvdd"},
};
enum {kPinCount = (int)(sizeof kPins / sizeof kPins[0]), kSurveyPins = 4};
enum {kPinMclk, kPinScl, kPinSda, kPinReset, kPinRailDvddSw, kPinRailAvdd, kPinRailDvdd};
enum {kRailPins = 3};

static const uint32_t kGccBranches[2] = {DbcamGccCameraAhb, DbcamGccCameraXo};
static const uint32_t kAlwaysOn[2] = {DbcamCcSleepClk, DbcamCcCoreAhb};        /* kona 没有 drv_ahb / drv_xo / gdsc_clk 的号 */
static const uint32_t kRcgs[3] = {DbcamCcSlowAhbRcg, DbcamCcFastAhbRcg, DbcamCcCciRcg};
static const uint32_t kPolled[2] = {DbcamCcCpasAhb, DbcamCcCci0};              /* 母本的 top_ahb / top_fast_ahb 在 kona 是 cpas_ahb */

static int cbcr_on(uint32_t value) {return (value & DBCAM_CBCR_ENABLE) != 0;}
static uint32_t pin_ctl_off(unsigned i) {return kPins[i].gpio * DbcamPinStride;}

/* TLMM 的闸门：只放行表里这七个脚的 CTL 与 IO，而且 CTL 的功能/驱动/上下拉必须正是表里那一组。
 * CTL 是读改写，所以值里会带别的位（中断配置等），这里只核对本驱动动的那三段。 */
static int tlmm_allowed(unsigned win, uint32_t off, uint32_t value)
{
    for (int i = 0; i < kPinCount; ++i) {
        if (kPins[i].win != win) continue;
        const uint32_t ctl = pin_ctl_off((unsigned)i);
        if (off == ctl) {
            if ((value & DBCAM_TLMM_FUNC_MASK) != ((uint32_t)kPins[i].func << DBCAM_TLMM_FUNC_SHIFT)) return 0;
            if ((value & DBCAM_TLMM_DRV_MASK) != ((uint32_t)kPins[i].drv << DBCAM_TLMM_DRV_SHIFT)) return 0;
            return (value & DBCAM_TLMM_PULL_MASK) == kPins[i].pull;
        }
        if (off == ctl + 4) return (value & ~DBCAM_TLMM_IO_OUT) == 0;    /* 只许动输出电平那一位 */
    }
    return 0;
}

/* CCI 的闸门：级 ≤3 只读版本；级 4 只放行初始化那几笔和队列的五个寄存器，值也逐个核对。 */
static int cci_allowed(uint32_t off, uint32_t value)
{
    switch (off) {
    case DbcamCciReset: return value == DbcamCciResetAll || value == 1;
    case DbcamCciSclCtl: return value == DbcamCciSclCtl100k;
    case DbcamCciSdaCtl0: return value == DbcamCciSdaCtl0_100k;
    case DbcamCciSdaCtl1: return value == DbcamCciSdaCtl1_100k;
    case DbcamCciSdaCtl2: return value == DbcamCciSdaCtl2_100k;
    case DbcamCciMiscCtl: return value == DbcamCciMiscCtl100k;
    case DbcamCciIrqMask0: case DbcamCciIrqClear0: return value == (uint32_t)DbcamCciIrqMaskAll;
    case DbcamCciIrqMask1: case DbcamCciIrqClear1: return value == (uint32_t)DbcamCciIrqMask1All;
    case DbcamCciIrqGlobalClear: return value == 1;
    case DbcamCciRdThreshold: return value == (uint32_t)DbcamCciRdThresholdValue;
    case DbcamCciHalt: return value == (uint32_t)DbcamCciHaltM0;
    case DbcamCciLoadData: return 1;                 /* 队列字：命令字与数据都在这里，装什么由下面的交易决定 */
    case DbcamCciExecWords: return 1;
    case DbcamCciQueueStart: return value == 1;
    default: return 0;                               /* 只读的寄存器一个都不写 */
    }
}

int dbcam_write_allowed(unsigned win, uint32_t offset, uint32_t value)
{
    switch (win) {
    case DbcamWinGcc:
        return (offset == DbcamGccCameraAhb || offset == DbcamGccCameraXo) && cbcr_on(value);
    case DbcamWinTlmmSouth: case DbcamWinTlmmNorth: case DbcamWinTlmmWest:
        return tlmm_allowed(win, offset, value);
    case DbcamWinCamcc:
        switch (offset) {
        case DbcamCcGdscr: return !(value & (DBCAM_GDSC_SW_COLLAPSE | DBCAM_GDSC_HW_CONTROL));     /* 只开，不关 */
        case DbcamCcSleepClk: case DbcamCcCoreAhb: case DbcamCcCpasAhb: case DbcamCcCci0: case DbcamCcMclkCbcr:
            return cbcr_on(value);
        case DbcamCcSlowAhbRcg: case DbcamCcFastAhbRcg: case DbcamCcCciRcg: case DbcamCcMclkRcg:
            return (value & DBCAM_RCG_UPDATE) != 0;
        case DbcamCcSlowAhbRcg + DbcamRcgCfg: case DbcamCcFastAhbRcg + DbcamRcgCfg:
        case DbcamCcCciRcg + DbcamRcgCfg: case DbcamCcMclkRcg + DbcamRcgCfg:
            return value == DBCAM_RCG_CFG_XO;
        default:
            return 0;
        }
    case DbcamWinCci:
        return cci_allowed(offset, value);
    default:
        return 0;
    }
}

/* ---- 访问 ---- */
static uint32_t rd(DbcamBringup *b, unsigned win, uint32_t off) {return b->hw->read(b->hw->ctx, win, off);}
static void wr(DbcamBringup *b, unsigned win, uint32_t off, uint32_t value)
{
    if (!dbcam_write_allowed(win, off, value)) {
        b->refused++;
        return;
    }
    b->hw->write(b->hw->ctx, win, off, value);
}
static void line(DbcamBringup *b, const char *text) {if (b->hw->log) b->hw->log(b->hw->ctx, text);}

static int done(DbcamBringup *b, int step)
{
    b->step = step;
    return 0;
}
static int failed(DbcamBringup *b, int step, int error, const char *what)
{
    char text[160];
    DbcamText t;
    b->step = -step;
    b->error = error;
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] FAIL");
    dbcam_text_kd(&t, "step", step);
    dbcam_text_kd(&t, "err", error);
    dbcam_text_char(&t, ' ');
    dbcam_text_str(&t, what);
    line(b, text);
    return error;
}

/* ---- 步 101 ---- */
static int survey(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] survey gcc");
    for (unsigned i = 0; i < 2; ++i) {
        b->gcc[i] = rd(b, DbcamWinGcc, kGccBranches[i]);
        dbcam_text_kx(&t, i == 0 ? "ahb" : "xo", b->gcc[i]);
    }
    line(b, text);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] pins");
    for (unsigned i = 0; i < (unsigned)kSurveyPins; ++i) {
        b->pin_ctl[i] = rd(b, kPins[i].win, pin_ctl_off(i));
        b->pin_io[i] = rd(b, kPins[i].win, pin_ctl_off(i) + 4);
        dbcam_text_char(&t, ' ');
        dbcam_text_str(&t, kPins[i].name);
        dbcam_text_char(&t, '=');
        dbcam_text_hex(&t, b->pin_ctl[i] & 0x3ffu);
        dbcam_text_char(&t, '/');
        dbcam_text_udec(&t, b->pin_io[i] & 3u);
    }
    b->pins_read = 1;
    line(b, text);
    line(b, "[cam] rail cam_vio=ldoc5 1800mV; switches are gpio 48/26/49");
    return done(b, DbcamStepSurvey);
}

/* ---- 步 102 ---- */
static uint32_t branch_off(uint32_t value) {return (value & DBCAM_CBCR_OFF) && !(value & DBCAM_CBCR_HW_CTL);}

static int camcc_read(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t now = rd(b, DbcamWinGcc, kGccBranches[i]);
        if (!cbcr_on(now)) wr(b, DbcamWinGcc, kGccBranches[i], now | DBCAM_CBCR_ENABLE);
    }
    uint32_t off = 3;
    for (unsigned n = 0; n < kBranchPolls && off; ++n) {
        off = 0;
        for (unsigned i = 0; i < 2; ++i) {
            b->gcc[i] = rd(b, DbcamWinGcc, kGccBranches[i]);
            if (branch_off(b->gcc[i]) || !cbcr_on(b->gcc[i])) off |= 1u << i;
        }
        if (off) b->hw->udelay(b->hw->ctx, kBranchStepUs);
    }
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] gcc");
    dbcam_text_kx(&t, "ahb", b->gcc[0]);
    dbcam_text_kx(&t, "xo", b->gcc[1]);
    dbcam_text_kx(&t, "off", off);
    line(b, text);
    /* 分支没起来时绝不读 CAMCC：读没时钟的块会把总线挂住 */
    if (off) return failed(b, DbcamStepCamcc, DbcamErrGcc, "a GCC branch did not start: CAMCC was not read");
    b->hw->msleep(b->hw->ctx, kSettleMs);
    b->gdscr = rd(b, DbcamWinCamcc, DbcamCcGdscr);
    b->cfg_gdscr = rd(b, DbcamWinCamcc, DbcamCcCfgGdscr);
    b->cci_cbcr = rd(b, DbcamWinCamcc, DbcamCcCci0);
    b->cci_rcg = rd(b, DbcamWinCamcc, DbcamCcCciRcg + DbcamRcgCfg);
    b->mclk_cbcr = rd(b, DbcamWinCamcc, DbcamCcMclkCbcr);
    b->mclk_rcg = rd(b, DbcamWinCamcc, DbcamCcMclkRcg + DbcamRcgCfg);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] camcc");
    dbcam_text_kx(&t, "gdscr", b->gdscr);
    dbcam_text_kx(&t, "cfg", b->cfg_gdscr);
    dbcam_text_kx(&t, "cci", b->cci_cbcr);
    dbcam_text_kx(&t, "ccircg", b->cci_rcg);
    dbcam_text_kx(&t, "mclk", b->mclk_cbcr);
    dbcam_text_kx(&t, "mclkrcg", b->mclk_rcg);
    line(b, text);
    if (b->gdscr == 0xffffffffu || b->cci_cbcr == 0xffffffffu || b->mclk_cbcr == 0xffffffffu)
        return failed(b, DbcamStepCamcc, DbcamErrCamccValue, "CAMCC reads all ones");
    if (b->gdscr & DBCAM_GDSC_HW_CONTROL) return failed(b, DbcamStepCamcc, DbcamErrGdsc, "the GDSC is under hardware control");
    return done(b, DbcamStepCamcc);
}

/* ---- 引脚写入 ---- */
/* 只换功能和驱动/上下拉，不碰输出使能与电平：给 mclk 和 cci 这些走复用功能的脚用。 */
static void pin_mux(DbcamBringup *b, unsigned idx)
{
    const DbcamPin *p = &kPins[idx];
    const uint32_t off = pin_ctl_off(idx);
    const uint32_t ctl = rd(b, p->win, off);
    wr(b, p->win, off, (ctl & ~((uint32_t)DBCAM_TLMM_FUNC_MASK | DBCAM_TLMM_DRV_MASK | DBCAM_TLMM_PULL_MASK)) |
                       ((uint32_t)p->func << DBCAM_TLMM_FUNC_SHIFT) |
                       ((uint32_t)p->drv << DBCAM_TLMM_DRV_SHIFT) | (uint32_t)p->pull);
}
/* 写成 GPIO 输出并给电平：给复位和三条开关轨用。 */
static void pin_output(DbcamBringup *b, unsigned idx, int level)
{
    pin_mux(b, idx);
    const DbcamPin *p = &kPins[idx];
    const uint32_t off = pin_ctl_off(idx);
    const uint32_t ctl = rd(b, p->win, off);
    wr(b, p->win, off, ctl | DBCAM_TLMM_OE);
    /* IO 寄存器的 bit 0 是输入状态（只读），bit 1 才是输出电平；整字写回会把只读位一起写进去，闸门也不放行。 */
    wr(b, p->win, off + 4, level ? DBCAM_TLMM_IO_OUT : 0);
}

/* ---- 步 201 ---- */
/* [K] clk-rcg2.c:724 的 enable 与 :105 的 update：先置 ROOT_EN + UPDATE 等更新完，写 CFG，再置 UPDATE 等更新完。
 * 分频字段 0 才是"不分频"（clk-rcg.c:334 rate /= pre_div + 1），所以 XO 直通的 CFG 只有 bit 20。 */
static int rcg_wait(DbcamBringup *b, uint32_t rcg)
{
    for (unsigned n = 0; n < kRcgPolls; ++n) {
        if (!(rd(b, DbcamWinCamcc, rcg) & DBCAM_RCG_UPDATE)) return 0;
        b->hw->udelay(b->hw->ctx, kRcgStepUs);
    }
    return -1;
}

static int rcg_configure(DbcamBringup *b, uint32_t rcg, uint32_t cfg)
{
    wr(b, DbcamWinCamcc, rcg, rd(b, DbcamWinCamcc, rcg) | DBCAM_RCG_CMD_ROOT_EN | DBCAM_RCG_UPDATE);
    if (rcg_wait(b, rcg)) return -1;
    wr(b, DbcamWinCamcc, rcg + DbcamRcgCfg, cfg);
    wr(b, DbcamWinCamcc, rcg, rd(b, DbcamWinCamcc, rcg) | DBCAM_RCG_UPDATE);
    return rcg_wait(b, rcg);
}

static int rcg_to_xo(DbcamBringup *b, uint32_t rcg)
{
    const uint32_t cfg = rd(b, DbcamWinCamcc, rcg + DbcamRcgCfg);
    /* 已经是 XO 直通（软件持有、源 0、模式 0、不分频）就什么都不写 */
    if ((cfg & DBCAM_RCG_SW_OWNED) && !(cfg & (DBCAM_RCG_SRC_MASK | DBCAM_RCG_MODE_MASK | DBCAM_RCG_DIV_MASK))) return 0;
    return rcg_configure(b, rcg, DBCAM_RCG_CFG_XO);
}

static int branch_on(DbcamBringup *b, uint32_t cbcr)
{
    const uint32_t now = rd(b, DbcamWinCamcc, cbcr);
    if (!cbcr_on(now)) wr(b, DbcamWinCamcc, cbcr, now | DBCAM_CBCR_ENABLE);
    for (unsigned n = 0; n < kBranchPolls; ++n) {
        const uint32_t v = rd(b, DbcamWinCamcc, cbcr);
        if (cbcr_on(v) && !branch_off(v)) return 0;
        b->hw->udelay(b->hw->ctx, kBranchStepUs);
    }
    return -1;
}

static int gdsc_on(uint32_t gdscr, uint32_t cfg) {return (gdscr & DBCAM_GDSC_PWR_ON) || (cfg & DBCAM_GDSC_CFG_UP_DONE);}

/* 碰 CCI 之前逐个看：GDSC 上电，要跑的分支都在动。这是这个阶段最要紧的一条：未上电访问会挂总线。 */
static int cci_ready(DbcamBringup *b)
{
    if (!gdsc_on(rd(b, DbcamWinCamcc, DbcamCcGdscr), rd(b, DbcamWinCamcc, DbcamCcCfgGdscr))) return 0;
    if (!cbcr_on(rd(b, DbcamWinCamcc, DbcamCcCoreAhb))) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t v = rd(b, DbcamWinCamcc, kPolled[i]);
        if (!cbcr_on(v) || branch_off(v)) return 0;
    }
    return 1;
}

static int domain_on(DbcamBringup *b)
{
    const DbcamHw *hw = b->hw;
    char text[190];
    DbcamText t;
    int rc = hw->arc(hw->ctx, DBCAM_ARC_MM, DbcamArcLevel, &b->mm_hlvl);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] arc");
    dbcam_text_kd(&t, "mm", b->mm_hlvl);
    dbcam_text_kd(&t, "rc", rc);
    line(b, text);
    if (rc) return failed(b, DbcamStepDomain, DbcamErrVote, "ARC vote");
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t now = rd(b, DbcamWinCamcc, kAlwaysOn[i]);
        if (!cbcr_on(now)) wr(b, DbcamWinCamcc, kAlwaysOn[i], now | DBCAM_CBCR_ENABLE);
    }
    const uint32_t gdscr = rd(b, DbcamWinCamcc, DbcamCcGdscr);
    if (gdscr & DBCAM_GDSC_HW_CONTROL) return failed(b, DbcamStepDomain, DbcamErrGdsc, "the GDSC is under hardware control");
    if (!gdsc_on(gdscr, rd(b, DbcamWinCamcc, DbcamCcCfgGdscr))) {
        wr(b, DbcamWinCamcc, DbcamCcGdscr, gdscr & ~DBCAM_GDSC_SW_COLLAPSE);
        hw->udelay(hw->ctx, 1);                 /* 8 个 XO 周期之后再看状态 */
        unsigned n = 0;
        for (; n < kGdscPolls; ++n) {
            if (gdsc_on(rd(b, DbcamWinCamcc, DbcamCcGdscr), rd(b, DbcamWinCamcc, DbcamCcCfgGdscr))) break;
            hw->udelay(hw->ctx, kGdscStepUs);
        }
        if (n == kGdscPolls) return failed(b, DbcamStepDomain, DbcamErrGdsc, "the GDSC did not power up: CCI was not read");
        hw->udelay(hw->ctx, 1);
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (rcg_to_xo(b, kRcgs[i])) return failed(b, DbcamStepDomain, DbcamErrRcg, "an RCG did not update: CCI was not read");
    }
    const uint32_t core = rd(b, DbcamWinCamcc, DbcamCcCoreAhb);
    if (!cbcr_on(core)) wr(b, DbcamWinCamcc, DbcamCcCoreAhb, core | DBCAM_CBCR_ENABLE);
    hw->udelay(hw->ctx, kCoreAhbDelayUs);
    for (unsigned i = 0; i < 2; ++i) {
        if (branch_on(b, kPolled[i])) return failed(b, DbcamStepDomain, DbcamErrBranch, "a branch did not start: CCI was not read");
    }
    if (!cci_ready(b)) return failed(b, DbcamStepDomain, DbcamErrBranch, "state check failed: CCI was not read");
    b->cci_version = rd(b, DbcamWinCci, DbcamCciVersion);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] domain on");
    dbcam_text_kx(&t, "gdscr", rd(b, DbcamWinCamcc, DbcamCcGdscr));
    dbcam_text_kx(&t, "cci", rd(b, DbcamWinCamcc, DbcamCcCci0));
    dbcam_text_kx(&t, "cpas", rd(b, DbcamWinCamcc, DbcamCcCpasAhb));
    dbcam_text_kx(&t, "cci_version", b->cci_version);
    line(b, text);
    if (!b->cci_version || b->cci_version == 0xffffffffu) return failed(b, DbcamStepDomain, DbcamErrCciVersion, "CCI version is not plausible");
    return done(b, DbcamStepDomain);
}

/* ---- 步 301：轨道 ---- */
static int rails_on(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    b->rail_rc = (uint32_t)b->hw->rail(b->hw->ctx, DBCAM_RAIL_VIO, DbcamRailVioMv);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] rail");
    dbcam_text_char(&t, ' ');
    dbcam_text_str(&t, DBCAM_RAIL_VIO);
    dbcam_text_kd(&t, "mv", DbcamRailVioMv);
    dbcam_text_kd(&t, "rc", (int64_t)b->rail_rc);
    line(b, text);
    if (b->rail_rc) return failed(b, DbcamStepRail, DbcamErrRail, "cam_vio vote failed: the sensor is not powered");
    /* 三条开关轨（[D] gpio-custom1 / gpio-custom-vana-ref / gpio-vdig，都是高有效） */
    for (unsigned i = 0; i < (unsigned)kRailPins; ++i) pin_output(b, (unsigned)(kPinRailDvddSw + i), 1);
    b->hw->msleep(b->hw->ctx, kRailSettleMs);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] switch");
    for (unsigned i = 0; i < (unsigned)kRailPins; ++i) {
        const unsigned idx = (unsigned)(kPinRailDvddSw + i);
        dbcam_text_char(&t, ' ');
        dbcam_text_str(&t, kPins[idx].name);
        dbcam_text_char(&t, '=');
        dbcam_text_hex(&t, rd(b, kPins[idx].win, pin_ctl_off(idx)) & 0x3ffu);
        dbcam_text_char(&t, '/');
        dbcam_text_udec(&t, rd(b, kPins[idx].win, pin_ctl_off(idx) + 4) & 3u);
    }
    line(b, text);
    return done(b, DbcamStepRail);
}

/* ---- 步 302：MCLK ---- */
/* MCLK0 的 RCG 在 CAMCC（0x5000），父时钟表里 XO 是 0（[K] camcc-kona.c:100）。24 MHz 要走 CAM_CC_PLL2 的
 * out_early（camcc-kona.c:1125 F(24000000, P_CAM_CC_PLL2_OUT_EARLY, 10, 1, 4)），这一级不开 PLL，就用 XO 19.2 MHz：
 * 传感器只要有时钟就能应答 ID 读，模式相关的时序等出画面那一级再按 PLL 配。 */
static int clock_on(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    if (rcg_configure(b, DbcamCcMclkRcg, DBCAM_RCG_CFG_XO))
        return failed(b, DbcamStepClock, DbcamErrRcg, "the MCLK0 RCG did not update");
    if (branch_on(b, DbcamCcMclkCbcr)) return failed(b, DbcamStepClock, DbcamErrBranch, "the MCLK0 branch did not start");
    pin_mux(b, (unsigned)kPinMclk);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] mclk");
    dbcam_text_kx(&t, "rcg", rd(b, DbcamWinCamcc, DbcamCcMclkRcg + DbcamRcgCfg));
    dbcam_text_kx(&t, "cmd", rd(b, DbcamWinCamcc, DbcamCcMclkRcg));
    dbcam_text_kx(&t, "cbcr", rd(b, DbcamWinCamcc, DbcamCcMclkCbcr));
    dbcam_text_kx(&t, "pin", rd(b, kPins[kPinMclk].win, pin_ctl_off(kPinMclk)) & 0x3ffu);
    line(b, text);
    return done(b, DbcamStepClock);
}

/* ---- 步 303：复位 ---- */
static int sensor_reset(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    pin_output(b, (unsigned)kPinReset, 0);
    b->hw->msleep(b->hw->ctx, kResetLowMs);
    pin_output(b, (unsigned)kPinReset, 1);
    b->hw->msleep(b->hw->ctx, kResetBootMs);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] reset released");
    dbcam_text_kx(&t, "ctl", rd(b, kPins[kPinReset].win, pin_ctl_off(kPinReset)) & 0x3ffu);
    dbcam_text_kx(&t, "io", rd(b, kPins[kPinReset].win, pin_ctl_off(kPinReset) + 4) & 3u);
    line(b, text);
    return done(b, DbcamStepReset);
}

/* ---- 步 304：CCI 初始化 ---- */
static int cci_init(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    /* CCI 的两条腿先按 [D] cci0_active 配上：功能 cci_i2c、上拉、2 mA。 */
    pin_mux(b, (unsigned)kPinScl);
    pin_mux(b, (unsigned)kPinSda);
    /* [K] cam_cci_soc.c:39-46：全掩码复位（整块），再写 1 触发；完成位在 STATUS0 的 bit 24。 */
    wr(b, DbcamWinCci, DbcamCciReset, DbcamCciResetAll);
    wr(b, DbcamWinCci, DbcamCciReset, 1);
    uint32_t status = 0;
    unsigned n = 0;
    for (; n < kCciPolls; ++n) {
        status = rd(b, DbcamWinCci, DbcamCciIrqStatus0);
        if (status & DBCAM_CCI_IRQ_RESET_DONE) break;
        b->hw->udelay(b->hw->ctx, kCciStepUs);
    }
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] cci reset");
    dbcam_text_kx(&t, "st", status);
    dbcam_text_kd(&t, "poll", (int64_t)n);
    dbcam_text_kx(&t, "scl", rd(b, kPins[kPinScl].win, pin_ctl_off(kPinScl)) & 0x3ffu);
    dbcam_text_kx(&t, "sda", rd(b, kPins[kPinSda].win, pin_ctl_off(kPinSda)) & 0x3ffu);
    line(b, text);
    if (n == kCciPolls) return failed(b, DbcamStepCciInit, DbcamErrCciReset, "CCI reset never acknowledged: no transaction sent");
    /* 节拍（[K] cam_cci_core.c:614-628 的装法），然后全掩全清、全局清一次、读 FIFO 阈值（cam_cci_soc.c:176-186）。 */
    wr(b, DbcamWinCci, DbcamCciSclCtl, DbcamCciSclCtl100k);
    wr(b, DbcamWinCci, DbcamCciSdaCtl0, DbcamCciSdaCtl0_100k);
    wr(b, DbcamWinCci, DbcamCciSdaCtl1, DbcamCciSdaCtl1_100k);
    wr(b, DbcamWinCci, DbcamCciSdaCtl2, DbcamCciSdaCtl2_100k);
    wr(b, DbcamWinCci, DbcamCciMiscCtl, DbcamCciMiscCtl100k);
    wr(b, DbcamWinCci, DbcamCciIrqMask0, DbcamCciIrqMaskAll);
    wr(b, DbcamWinCci, DbcamCciIrqClear0, DbcamCciIrqMaskAll);
    wr(b, DbcamWinCci, DbcamCciIrqMask1, DbcamCciIrqMask1All);
    wr(b, DbcamWinCci, DbcamCciIrqClear1, DbcamCciIrqMask1All);
    wr(b, DbcamWinCci, DbcamCciIrqGlobalClear, 1);
    wr(b, DbcamWinCci, DbcamCciRdThreshold, DbcamCciRdThresholdValue);
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] cci timing");
    dbcam_text_kx(&t, "scl", rd(b, DbcamWinCci, DbcamCciSclCtl));
    dbcam_text_kx(&t, "sda0", rd(b, DbcamWinCci, DbcamCciSdaCtl0));
    dbcam_text_kx(&t, "sda1", rd(b, DbcamWinCci, DbcamCciSdaCtl1));
    dbcam_text_kx(&t, "sda2", rd(b, DbcamWinCci, DbcamCciSdaCtl2));
    dbcam_text_kx(&t, "misc", rd(b, DbcamWinCci, DbcamCciMiscCtl));
    line(b, text);
    return done(b, DbcamStepCciInit);
}

/* ---- 步 305：CCI 交易 ---- */
static uint32_t set_param_word(uint32_t sid, uint32_t retries)
{
    return (uint32_t)DbcamCciCmdSetParam | (sid << DbcamCciSidShift) | (retries << DbcamCciRetryShift);
}
static uint32_t write_nostop_word(uint32_t addr, unsigned addr_type)
{
    uint32_t word = (uint32_t)DbcamCciCmdWriteNoStop | ((uint32_t)addr_type << 4);
    for (unsigned i = 0; i < addr_type; ++i)
        word |= ((addr >> (i * 8)) & 0xffu) << ((addr_type - i) * 8);
    return word;
}

/* 一笔注册读：SET_PARAM → LOCK → WRITE_DISABLE_P(寄存器地址) → READ → UNLOCK，装进 queue 0，启动，轮询状态。
 * [K] cam_cci_core.c:972-1021 逐字对应。返回 0 表示读到，-1 超时，-2 是 NACK/错误，-3 是 FIFO 里的字数不够。
 * 读回来的第一个字节是 CCI 回显的从地址，从第二个字节起才是寄存器内容（cam_cci_core.c:1070-1079）。 */
static int cci_read(DbcamBringup *b, uint32_t sid, uint32_t reg, uint8_t *data, unsigned bytes)
{
    wr(b, DbcamWinCci, DbcamCciIrqClear0, DbcamCciIrqMaskAll);
    wr(b, DbcamWinCci, DbcamCciIrqGlobalClear, 1);
    wr(b, DbcamWinCci, DbcamCciLoadData, set_param_word(sid, DbcamCciRetries));
    wr(b, DbcamWinCci, DbcamCciLoadData, (uint32_t)DbcamCciCmdLock);
    wr(b, DbcamWinCci, DbcamCciLoadData, write_nostop_word(reg, DbcamCciAddrTypeWord));
    wr(b, DbcamWinCci, DbcamCciLoadData, (uint32_t)DbcamCciCmdRead | (bytes << 4));
    wr(b, DbcamWinCci, DbcamCciLoadData, (uint32_t)DbcamCciCmdUnlock);
    wr(b, DbcamWinCci, DbcamCciExecWords, rd(b, DbcamWinCci, DbcamCciCurWords));
    b->hw->udelay(b->hw->ctx, 1);
    wr(b, DbcamWinCci, DbcamCciQueueStart, 1);
    uint32_t status = 0;
    unsigned n = 0;
    for (; n < kCciPolls; ++n) {
        status = rd(b, DbcamWinCci, DbcamCciIrqStatus0);
        if (status & (DBCAM_CCI_IRQ_RD_DONE | DBCAM_CCI_IRQ_ERROR)) break;
        b->hw->udelay(b->hw->ctx, kCciStepUs);
    }
    if (n == kCciPolls) return -1;
    if (status & DBCAM_CCI_IRQ_ERROR) return -2;
    const uint32_t level = rd(b, DbcamWinCci, DbcamCciReadLevel);
    uint8_t raw[8];
    unsigned got = 0;
    for (uint32_t w = 0; w < level && got < sizeof raw; ++w) {
        const uint32_t word = rd(b, DbcamWinCci, DbcamCciReadData);
        for (unsigned i = 0; i < 4 && got < sizeof raw; ++i) raw[got++] = (uint8_t)(word >> (i * 8));
    }
    if (got < bytes + 1u) return -3;
    for (unsigned i = 0; i < bytes; ++i) data[i] = raw[i + 1];
    return 0;
}

static int scan(DbcamBringup *b)
{
    char text[190];
    DbcamText t;
    for (unsigned i = 0; i < (unsigned)DbcamScanCount; ++i) {
        uint8_t data[2] = {0, 0};
        const uint32_t sid = kDbcamScanAddr[i];
        const int rc = cci_read(b, sid, DbcamScanReg, data, DbcamCciScanBytes);
        b->scan_id[i] = rc ? 0 : (((uint32_t)data[0] << 8) | data[1]);
        if (!rc) b->scan_ack |= 1u << i;
        dbcam_text_start(&t, text, sizeof text);
        dbcam_text_str(&t, "[cam] scan");
        dbcam_text_kx(&t, "addr", sid);
        if (rc) {
            dbcam_text_str(&t, rc == -2 ? " nack" : rc == -1 ? " timeout" : " short");
            dbcam_text_kd(&t, "rc", rc);
        } else {
            dbcam_text_kx(&t, "id", b->scan_id[i]);
        }
        line(b, text);
        /* 一笔交易结束把状态清干净再下一笔，不然下一笔会立刻看到上一笔的 RD_DONE */
        wr(b, DbcamWinCci, DbcamCciIrqClear0, DbcamCciIrqMaskAll);
        wr(b, DbcamWinCci, DbcamCciIrqGlobalClear, 1);
    }
    if (!b->scan_ack)
        return failed(b, DbcamStepScan, DbcamErrNoSensor, "no sensor answered 0x10/0x20/0x34: rails, MCLK or the part itself");
    return done(b, DbcamStepScan);
}

unsigned dbcam_level(unsigned value)
{
    if (!value) return 0;
    return value > DbcamLevelMax ? DbcamLevelMax : value;
}

int dbcam_run(DbcamBringup *b)
{
    if (!b || !b->hw || !b->hw->read || !b->hw->udelay || !b->hw->msleep || !b->hw->arc || !b->hw->rail)
        return DbcamErrArgument;
    if (!b->level) return 0;
    int rc = survey(b);
    if (!rc && b->level >= 2) rc = camcc_read(b);
    if (!rc && b->level >= 3) rc = domain_on(b);
    if (!rc && b->level >= 4) rc = rails_on(b);
    if (!rc && b->level >= 4) rc = clock_on(b);
    if (!rc && b->level >= 4) rc = sensor_reset(b);
    if (!rc && b->level >= 4) rc = cci_init(b);
    if (!rc && b->level >= 4) rc = scan(b);
    return rc;
}
