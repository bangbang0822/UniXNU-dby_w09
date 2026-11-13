// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// DbyCamera.h 的内核侧：四个寄存器窗口 + apps RSC + cmd-db 的映射、一条一次性线程、ARC 投票和日志。
// 阶梯、白名单和每一步读到什么全在 DbyCamBringup.c，这里不含判断。窗口映射、线程和参数读取的写法照本仓已经
// 验收的 DbyTorch.cpp：kIOMapInhibitCache、每个窗口带上尺寸、越界的读回全 1（阶梯里"读到全 1 就停"那条正好接住）。
// ARC 走 package/QcomRpmh.h：主动 TCS 的写法照 AdspProbe.cpp:259 的 vote()，只是这里不等 done 中断——本板相机
// 不需要 SPI 5 的第二读者，投完等一拍收尾就够了。主动 TCS 用 0：本仓唯一占着主动 TCS 的是 AdspProbe 的 1，
// 母本把 0 记给 Wi-Fi 的 bring-up，这台板没有那条路径；AudioProbe.cpp:790 的 micRail 名义上用 2，但它那份的
// RSC 和 cmd-db 基址还是 SM8550 的（0x17a20000 / 0x81c60000；本板 0x18220000 / 0x80860000，kona.dtsi:972），
// 还没落到这台机子的地址上。
// 每一行都走 kprintf：本板 COM4 镜像 kprintf 行，没有 IOLog 行（DbyTorch.cpp 文件头同一条）。
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "DbyCamera.h"
#include "DbyCamBoard.h"
#include "DbyCamBringup.h"
#include "DbyCamText.h"
#include "QcomRpmh.h"
}

extern "C" {
volatile int32_t dby_camera_status[16];
}

namespace {
enum : unsigned {
    kArcTcs = 0,
    kVoteSettleMs = 2,            // AudioProbe.cpp:790 同一段用的拍子：请求写完等 2 ms 再收尾
};

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    uint32_t size;
};
Window gWin[DbcamWinCount];
Window gRsc;
uint8_t *gCmdDb;
unsigned gLevel;                  // 参数换算出来的级，线程起手时用，避免共享状态

void trace(const char *line) {kprintf("%s\n", line);}

unsigned argument(const char *name, unsigned fallback)
{
    unsigned value = fallback;
    return PE_parse_boot_argn(name, &value, sizeof value) ? value : fallback;
}

bool mapWindow(Window &w, uint64_t base, uint32_t size)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = w.va ? size : 0;
    return w.va != nullptr;
}
void unmapWindow(Window &w)
{
    w.va = nullptr;
    w.size = 0;
    OSSafeReleaseNULL(w.map);
}

uint32_t rd32(const Window &w, uint32_t at)
{
    if (!w.va || at + 4u > w.size) return 0xffffffffu;
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.va + at);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
void wr32(Window &w, uint32_t at, uint32_t value)
{
    if (!w.va || at + 4u > w.size) return;
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- 阶梯的回调（DbyCamBringup.h） ----
uint32_t winRead(void *, unsigned win, uint32_t offset)
{
    return win < DbcamWinCount ? rd32(gWin[win], offset) : 0xffffffffu;
}
void winWrite(void *, unsigned win, uint32_t offset, uint32_t value)
{
    if (win < DbcamWinCount) wr32(gWin[win], offset, value);
}
void delayUs(void *, unsigned us) {IODelay(us);}
void sleepMs(void *, unsigned ms) {IOSleep(ms);}
void logLine(void *, const char *text) {trace(text);}

// ---- 一条 ARC 主动投票 ----
// 第一次用时才映射 RSC、复制 cmd-db（128 KiB 的副本只在线程活着时存在，不进内核 __DATA，和 AdspProbe.cpp:604
// 那个 CmdDbCopy 一个办法）。找不到资源就照实返回负数，阶梯把它记成一次投票失败，不去动 CAMCC。
bool voteReady()
{
    if (!gRsc.va && !mapWindow(gRsc, DBCAM_RSC_DRV, DBCAM_RSC_BYTES)) return false;
    if (!gCmdDb) {
        gCmdDb = static_cast<uint8_t *>(IOMallocData(DBCAM_CMD_DB_BYTES));
        if (!gCmdDb) return false;
        Window db;
        if (!mapWindow(db, DBCAM_CMD_DB, DBCAM_CMD_DB_BYTES)) {
            IOFreeData(gCmdDb, DBCAM_CMD_DB_BYTES);
            gCmdDb = nullptr;
            return false;
        }
        for (uint32_t i = 0; i < DBCAM_CMD_DB_BYTES; i += 4) {
            const uint32_t word = rd32(db, i);
            gCmdDb[i] = (uint8_t)word;
            gCmdDb[i + 1] = (uint8_t)(word >> 8);
            gCmdDb[i + 2] = (uint8_t)(word >> 16);
            gCmdDb[i + 3] = (uint8_t)(word >> 24);
        }
        unmapWindow(db);
    }
    return gCmdDb != nullptr;
}

bool sameName(const char *a, const char *b)
{
    unsigned i = 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}

int arcVote(void *, const char *resource, uint32_t vlvl, uint32_t *hlvl)
{
    if (!voteReady()) return QcomRpmhNoDb;
    QcomRpmhResource res;
    const int found = qcom_cmd_db_find(gCmdDb, DBCAM_CMD_DB_BYTES, resource, &res);
    if (found) return found;
    uint32_t index = 0;
    const int level = qcom_rpmh_level_index(&res, vlvl, &index);
    if (level) return level;
    dby_camera_status[11] |= sameName(resource, DBCAM_ARC_MM) ? 1 : 2;    // bit 0 mmcx，bit 1 mxc
    dby_camera_status[10] = (int32_t)rd32(gRsc, 0);
    const QcomRscLayout layout = qcom_rsc_layout((uint32_t)dby_camera_status[10]);
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&layout, kArcTcs, &res.address, &index, 1, writes, 16);
    if (!n) return QcomRpmhArgument;
    for (unsigned i = 0; i < n; ++i) wr32(gRsc, DBCAM_RSC_TCS_BLOCK + writes[i].offset, writes[i].value);
    IOSleep(kVoteSettleMs);
    const unsigned m = qcom_rpmh_finish(&layout, kArcTcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr32(gRsc, DBCAM_RSC_TCS_BLOCK + writes[i].offset, writes[i].value);
    *hlvl = index;
    return 0;
}

// ---- 一条稳压器投票 ----
// 同一个 active-only TCS，两条命令：电压（+0x0，毫伏）和使能（+0x4，1）。这是 [K] qcom-rpmh-regulator.c:161 与 :218
// 的两笔，本板唯一要走 PMIC 的相机轨就是 cam_vio（pm8150a_l5 → cmd-db 里的 "ldoc5"）。
int railVote(void *, const char *resource, uint32_t millivolts)
{
    if (!voteReady()) return QcomRpmhNoDb;
    QcomRpmhResource res;
    const int found = qcom_cmd_db_find(gCmdDb, DBCAM_CMD_DB_BYTES, resource, &res);
    if (found) return found;
    dby_camera_status[12] = (int32_t)res.address;
    if (!dby_camera_status[10]) dby_camera_status[10] = (int32_t)rd32(gRsc, 0);
    const QcomRscLayout layout = qcom_rsc_layout((uint32_t)dby_camera_status[10]);
    const uint32_t addr[2] = {res.address + DbcamRailVoltageOff, res.address + DbcamRailEnableOff};
    const uint32_t value[2] = {millivolts, 1};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&layout, kArcTcs, addr, value, 2, writes, 16);
    if (!n) return QcomRpmhArgument;
    for (unsigned i = 0; i < n; ++i) wr32(gRsc, DBCAM_RSC_TCS_BLOCK + writes[i].offset, writes[i].value);
    IOSleep(kVoteSettleMs);
    const unsigned m = qcom_rpmh_finish(&layout, kArcTcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr32(gRsc, DBCAM_RSC_TCS_BLOCK + writes[i].offset, writes[i].value);
    return 0;
}

void cleanup()
{
    if (gCmdDb) {
        IOFreeData(gCmdDb, DBCAM_CMD_DB_BYTES);
        gCmdDb = nullptr;
    }
    unmapWindow(gRsc);
    for (unsigned i = DbcamWinCount; i-- > 0;) unmapWindow(gWin[i]);
}

void cameraThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "dby-cam");
    for (unsigned i = 0; i < DbcamWinCount; ++i) {
        if (mapWindow(gWin[i], kDbcamWinBase[i], kDbcamWinBytes[i])) continue;
        cleanup();
        dby_camera_status[0] = 0;
        dby_camera_status[2] = -5;
        trace("[cam] FAIL windows not mapped: camera bring-up off");
        return;
    }
    const DbcamHw hw = {nullptr, winRead, winWrite, delayUs, sleepMs, arcVote, railVote, logLine};
    DbcamBringup bringup = {};
    bringup.hw = &hw;
    bringup.level = gLevel;
    const int rc = dbcam_run(&bringup);
    dby_camera_status[1] = bringup.step;
    dby_camera_status[2] = rc ? rc : bringup.error;
    dby_camera_status[5] = (int32_t)bringup.refused;
    dby_camera_status[6] = (int32_t)bringup.cci_version;
    dby_camera_status[7] = (int32_t)bringup.gdscr;
    dby_camera_status[8] = (int32_t)bringup.mm_hlvl;
    dby_camera_status[9] = (int32_t)bringup.mxc_hlvl;
    dby_camera_status[13] = (int32_t)bringup.rail_rc;
    dby_camera_status[14] = (int32_t)bringup.scan_ack;
    dby_camera_status[15] = (int32_t)bringup.scan_id[0];
    dby_camera_status[0] = 2;
    char text[160];
    DbcamText t;
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] done");
    dbcam_text_kd(&t, "step", bringup.step);
    dbcam_text_kd(&t, "rc", rc);
    dbcam_text_kd(&t, "refused", (int64_t)bringup.refused);
    trace(text);
    cleanup();
}
}  // namespace

extern "C" void dby_camera_start(void)
{
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    // 不新增启动参数：命令行是 50 条 998 字节、离 payload.py:35 的 1000 只剩 2，
    // 而这台板子要跑的本来就是满阶（MCLK 24 MHz、放开复位、CCI 读 0x10/0x20/0x34 找 sensor）。
    // 所以默认值就是 DbcamLevelMax，参数缺席 = 全跑。dby-cam 只保留「往下调」这一个用途：
    // 某次刷写想看第 3 级停在哪儿时才传，传 0 则整段不动（映射、投票、写寄存器都不发生）。
    const unsigned want = argument("dby-cam", DbcamLevelMax);
    if (!want) return;                      // 显式传 0：不映射、不投、不写、不起线程
    const unsigned level = dbcam_level(want);
    dby_camera_status[3] = (int32_t)want;
    dby_camera_status[4] = (int32_t)level;
    gLevel = level;
    char text[160];
    DbcamText t;
    dbcam_text_start(&t, text, sizeof text);
    dbcam_text_str(&t, "[cam] start");
    dbcam_text_kd(&t, "want", (int64_t)want);
    dbcam_text_kd(&t, "level", (int64_t)level);
    trace(text);
    if (want > DbcamLevelMax)
        trace("[cam] level clamped: this board stops at 4 (rails, MCLK, reset, CCI read of 0x10/0x20/0x34)");
    dby_camera_status[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(cameraThread, nullptr, &thread) != KERN_SUCCESS) {
        dby_camera_status[0] = 0;
        dby_camera_status[2] = -6;
        trace("[cam] FAIL thread: camera bring-up off");
        return;
    }
    thread_deallocate(thread);
}
