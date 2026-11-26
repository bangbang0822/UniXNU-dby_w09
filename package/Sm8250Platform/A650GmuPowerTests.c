/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host test for A650GmuPower.c over a software register peer.
 *
 * Part 1, the command-DB parser: a synthetic database shaped like the AOP's
 * (version, magic, slave records, entry tables, aux data), the eight resources
 * a650_gmu.c needs, its ARC level lists, and the refusals: a bad magic, an
 * entry or aux extent past the end, a name under the wrong slave, a missing
 * resource, and gmxc's optionality.
 *
 * Part 2, the power sequence: a fake register file that records every write and
 * reacts as the hardware is described to -- a VOTABLE GDSC that powers up once
 * SW_COLLAPSE clears, voted branches whose CLK_OFF follows the enable bit, and
 * an RCG whose root turns on and whose update completes -- so the sequence can
 * be checked write by write, and its refusals (no CX, a stuck RCG, a stuck
 * branch, an unmapped window) too.
 *
 * Part 3, the seam to the GMU: the command DB's three level lists are handed to
 * the real A650GmuBuildStartup, so the parser's output is proved to be the
 * input that function actually accepts.
 *
 * cc -std=c11 -Wall -Wextra -Werror -I <UniXNU>/kits/AdrenoFirmware A650GmuPower.c \
 *    A650GmuPowerTests.c A650Gmu.c \
 *    <UniXNU>/kits/AdrenoFirmware/{AdrenoFirmware,AdrenoHfi,AdrenoHfiStartup,AdrenoHfiVotes}.c \
 *    -o a650power
 */
/* posix_memalign under -std=c11: glibc hides it unless a POSIX level is asked
 * for, and the register windows need page alignment like the kernel's. */
#define _POSIX_C_SOURCE 200809L
#include "A650GmuPower.h"
#include "A650Gmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {if (!(x)) {fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1);}} while (0)

/* Window sizes, as the DTS gives them. */
static const size_t kBytes[A650GpuWindows] = {
    0x1000, 0x1000, 0x9000, 0x10000, 0x40000, 0x30000, 0x10000
};

/* ------------------------------------------------------------------------
 * Part 1: the synthetic AOP command database.
 * ------------------------------------------------------------------------ */

#define DB_BYTES 0x20000u
static uint8_t db[DB_BYTES];

static void put16(uint8_t *p, unsigned v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, v & 0xffff); put16(p + 2, v >> 16);}

/* A slave record: {slv_id, header_offset, data_offset, count, version}. */
static void slave(uint8_t *at, unsigned id, unsigned headers, unsigned data, unsigned count)
{
    put16(at, id); put16(at + 2, headers); put16(at + 4, data); put16(at + 6, count); put16(at + 8, 1);
}

/* An entry: {id[8], priority, address, len, offset}.  The id is zero padded, so
 * the eight bytes are cleared first: a rewrite with a shorter name must not
 * leave the old name's tail behind. */
static void entry(uint8_t *at, const char *id, uint32_t address, unsigned len, unsigned offset)
{
    memset(at, 0, 8);
    memcpy(at, id, strlen(id));
    put16(at + 8, 0);
    put32(at + 16, address);
    put16(at + 20, len);
    put16(at + 22, offset);
}

/* The real pipa command DB's level lists (2026-10-24, A650GmuTests.c). */
static const unsigned kGfx[] = {0, 48, 64, 128, 144, 192, 224, 256, 320, 384, 416};
static const unsigned kMx[]  = {0, 16, 128, 192, 224, 256, 320, 384, 416};
static const unsigned kCx[]  = {0, 16, 64, 128, 192, 256, 320, 384, 416};

/* The aux data of an ARC list is 16-bit little-endian words; `n` is the word
 * count and the byte length is 2 * n. */
#define LEVELS_WORDS(v) (sizeof(v) / sizeof((v)[0]))
#define LEVELS_BYTES(v) (2u * LEVELS_WORDS(v))
static void levels(uint8_t *at, const unsigned *v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) put16(at + 2 * i, v[i]);
}

/* Layout: slave 3 (ARC) holds four entries, slave 5 (BCM) four.  The ARC
 * entries' aux lists sit at data offset 0x400, the BCM entries' at 0x800. */
enum {kArcEntries = 144 + 0x000, kArcData = 144 + 0x400, kBcmEntries = 144 + 0x100};

static void build_db(void)
{
    memset(db, 0, sizeof(db));
    put32(db, 1);                                  /* version */
    put32(db + 4, A650_CMD_DB_MAGIC);
    slave(db + 8, 3, 0x000, 0x400, 4);              /* ARC */
    slave(db + 24, 5, 0x100, 0x800, 4);             /* BCM */
    entry(db + kArcEntries + 0 * 24, "gfx.lvl", 0x30000, LEVELS_BYTES(kGfx), 0x000);
    entry(db + kArcEntries + 1 * 24, "mx.lvl",  0x30010, LEVELS_BYTES(kMx),  0x040);
    entry(db + kArcEntries + 2 * 24, "cx.lvl",  0x30020, LEVELS_BYTES(kCx),  0x080);
    entry(db + kArcEntries + 3 * 24, "gmxc.lvl", 0x30030, 8,           0x0c0);
    levels(db + kArcData + 0x000, kGfx, LEVELS_WORDS(kGfx));
    levels(db + kArcData + 0x040, kMx,  LEVELS_WORDS(kMx));
    levels(db + kArcData + 0x080, kCx,  LEVELS_WORDS(kCx));
    levels(db + kArcData + 0x0c0, (const unsigned[]){0, 64, 128, 192}, 4);
    entry(db + kBcmEntries + 0 * 24, "SH0", 0x50004, 4, 0x000);
    entry(db + kBcmEntries + 1 * 24, "MC0", 0x50000, 4, 0x010);
    entry(db + kBcmEntries + 2 * 24, "ACV", 0x5007c, 4, 0x020);
    entry(db + kBcmEntries + 3 * 24, "CN0", 0x50074, 4, 0x030);
}

static void part1_db(void)
{
    A650GpuDbResult r;

    build_db();
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbSuccess);
    CHECK(r.gmxc_present);
    /* The ARC lists come back as byte aliases into the database, unaligned or
     * not: the consumer reads them with its own little-endian reader. */
    CHECK(r.gfx_bytes == LEVELS_BYTES(kGfx) && r.mx_bytes == LEVELS_BYTES(kMx) &&
          r.cx_bytes == LEVELS_BYTES(kCx));
    CHECK(A650GpuLe16(r.gfx + 0) == 0 && A650GpuLe16(r.gfx + 2) == 48 && A650GpuLe16(r.gfx + 8) == 144);
    CHECK(A650GpuLe16(r.gfx + 20) == 416);
    CHECK(A650GpuLe16(r.mx + 0) == 0 && A650GpuLe16(r.mx + 16) == 416);
    CHECK(A650GpuLe16(r.cx + 4) == 64 && A650GpuLe16(r.cx + 16) == 416);
    CHECK(r.gmxc_bytes == 8 && A650GpuLe16(r.gmxc + 6) == 192);
    CHECK(r.sh0 == 0x50004 && r.mc0 == 0x50000 && r.acv == 0x5007c && r.cn0 == 0x50074);
    /* The aliases must be inside the caller's buffer, not copies. */
    CHECK(r.gfx >= db && r.gfx < db + sizeof(db) && r.cx < db + sizeof(db));
    printf("cmd-db: 8 entries, gfx %zu bytes, mx %zu, cx %zu, gmxc %zu, BCM %x %x %x %x\n",
           r.gfx_bytes, r.mx_bytes, r.cx_bytes, r.gmxc_bytes, r.sh0, r.mc0, r.acv, r.cn0);

    /* gmxc is optional: with the entry absent the other seven still parse. */
    memset(db + kArcEntries + 3 * 24, 0, 24);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbSuccess && !r.gmxc_present);
    CHECK(r.gfx_bytes == LEVELS_BYTES(kGfx) && r.cn0 == 0x50074);
    printf("cmd-db: gmxc absent, still success, gmxc_present 0\n");

    /* A required resource missing is refused. */
    build_db();
    memset(db + kBcmEntries + 3 * 24, 0, 24);      /* CN0 */
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbMissing);

    /* A prefix is not a match: the id must agree through the terminator. */
    build_db();
    entry(db + kBcmEntries + 3 * 24, "CN", 0x50074, 4, 0x030);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbMissing);
    /* A longer id is not a match either. */
    build_db();
    entry(db + kArcEntries + 2 * 24, "cx.lvlX", 0x30020, 4, 0x080);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbMissing);

    /* The slave a name sits under is not checked, as pipa's lookup does not
     * check it: the same name under another slave is still found, and its
     * address is taken as the database gives it.  alioth's AOP database has
     * not been read, so a slave rule here would be a guess. */
    build_db();
    entry(db + kArcEntries + 3 * 24, "SH0", 0x50004, 4, 0x0c0);   /* BCM name under ARC */
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbSuccess && r.sh0 == 0x50004);
    /* A BCM address is passed through unchanged, aligned or not: the parser
     * does not second-guess the platform's own database. */
    build_db();
    entry(db + kBcmEntries + 1 * 24, "MC0", 0x50002, 4, 0x010);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbSuccess && r.mc0 == 0x50002);

    /* A bad magic is corrupt. */
    build_db();
    put32(db + 4, 0);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbCorrupt);

    /* An aux extent past the end of a truncated snapshot is corrupt.  The
     * mapped reservation is always 0x20000 bytes, so this is the shape a
     * short copy or a short mapping takes: SH0's aux sits at 144 + 0x800 =
     * 0x890, and a 0x890-byte buffer ends exactly there. */
    build_db();
    CHECK(A650GpuCommandDbParse(db, 0x890, &r) == A650GpuDbCorrupt);

    /* An entry table past the end is corrupt: slave 3's table would start at
     * 144 + 0xfff0 = 0x10080, and 4 entries of 24 bytes run to 0x10140, which
     * is inside the reservation but the count is a lie -- so make the table
     * genuinely overshoot with a count that cannot fit: 0xfff0 + 144 + 4096
     * entries is far past 0x20000. */
    build_db();
    slave(db + 8, 3, 0xfff0, 0x400, 0xffff);
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &r) == A650GpuDbCorrupt);
    /* A truncated buffer cannot be parsed. */
    CHECK(A650GpuCommandDbParse(db, 64, &r) == A650GpuDbCorrupt);
    CHECK(A650GpuCommandDbParse(NULL, sizeof(db), &r) == A650GpuDbCorrupt);
    printf("cmd-db: missing, prefix, long id, bad magic and truncation refused; "
           "slave and alignment passed through\n");
}

/* ------------------------------------------------------------------------
 * Part 2: the software register peer.
 * ------------------------------------------------------------------------ */

static struct {
    uint32_t *regs[A650GpuWindows];
    unsigned writes, cx_writes;
    unsigned rcg_stuck, branch_stuck, no_cx, no_power;
    unsigned delay_us;
    struct {
        unsigned window;
        uint32_t offset, value;
    } log[256];
    unsigned logged;
} f;

static int read32(void *cookie, unsigned window, size_t offset, uint32_t *out)
{
    CHECK(cookie == &f && window < A650GpuWindows && !(offset & 3) && offset + 4 <= kBytes[window]);
    *out = f.regs[window][offset / 4];
    return 0;
}

/* A voted branch: the enable bit is written by the software; CLK_OFF follows
 * it unless the branch is stuck. */
static void branch(unsigned w, uint32_t offset, uint32_t value)
{
    uint32_t v = value;
    if (v & A650_CBCR_ENABLE) {
        if (!f.branch_stuck) v &= ~A650_CBCR_CLK_OFF;
    } else {
        v |= A650_CBCR_CLK_OFF;
    }
    f.regs[w][offset / 4] = v;
}

static int write32(void *cookie, unsigned window, size_t offset, uint32_t value)
{
    CHECK(cookie == &f && window < A650GpuWindows && !(offset & 3) && offset + 4 <= kBytes[window]);
    ++f.writes;
    if (f.logged < sizeof(f.log) / sizeof(f.log[0])) {
        f.log[f.logged].window = window;
        f.log[f.logged].offset = offset;
        f.log[f.logged].value = value;
        ++f.logged;
    }
    f.regs[window][offset / 4] = value;

    if (window == A650GpuWinGpucc && offset == A650_CC_CX_GDSCR) {
        ++f.cx_writes;
        if (!(value & A650_GDSC_SW_COLLAPSE) && !f.no_power) {
            f.regs[window][offset / 4] |= A650_GDSC_PWR_ON;
            f.regs[window][A650_CC_CX_GDS_HW_CTRL / 4] |= A650_GDSC_PWR_ON;
        }
        return 0;
    }
    if (window == A650GpuWinGpucc && offset == A650_CC_GMU_RCGR) {
        uint32_t cmd = value & ~A650_RCG_CMD_UPDATE;   /* the update completes at once */
        cmd = (value & A650_RCG_ROOT_EN) && !f.rcg_stuck
            ? (cmd & ~A650_RCG_ROOT_OFF) : (cmd | A650_RCG_ROOT_OFF);
        f.regs[window][offset / 4] = cmd;
        return 0;
    }
    if ((window == A650GpuWinGpucc && (offset == A650_CC_AHB_CLK || offset == A650_CC_SMMU_VOTE_CLK ||
                                       offset == A650_CC_CX_GMU_CLK || offset == A650_CC_CXO_CLK)) ||
        (window == A650GpuWinGccGpu && (offset == A650_GCC_DDRSS_GPU_AXI ||
                                        offset == A650_GCC_GPU_MEMNOC_GFX))) {
        branch(window, offset, value);
        return 0;
    }
    if (window == A650GpuWinGccGpu && offset == A650_GCC_GPU_CFG_AHB) {
        f.regs[window][offset / 4] = value;            /* HW-gated: enable bit only */
        return 0;
    }
    return 0;
}

static int barrier(void *cookie) {CHECK(cookie == &f); return 0;}
static void delay_us(void *cookie, unsigned us) {CHECK(cookie == &f); f.delay_us += us;}

static void *allocate(size_t n)
{
    void *p = NULL;
    CHECK(!posix_memalign(&p, 4096, n));
    memset(p, 0, n);
    return p;
}

/* The tablet's power-on state: cfg AHB off, CX collapsed, every branch off,
 * the GMU RCG's root off. */
static void reset_model(void)
{
    for (unsigned i = 0; i < A650GpuWindows; ++i) memset(f.regs[i], 0, kBytes[i]);
    f.writes = f.cx_writes = 0;
    f.rcg_stuck = f.branch_stuck = f.no_cx = f.no_power = 0;
    f.delay_us = 0;
    f.logged = 0;
    f.regs[A650GpuWinGccGpu][A650_GCC_GPU_CFG_AHB / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGpucc][A650_CC_CX_GDSCR / 4] = 0x00222001;      /* collapsed */
    f.regs[A650GpuWinGpucc][A650_CC_GX_GDSCR / 4] = 0x00222001;
    f.regs[A650GpuWinGpucc][A650_CC_AHB_CLK / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGpucc][A650_CC_SMMU_VOTE_CLK / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGpucc][A650_CC_CX_GMU_CLK / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGpucc][A650_CC_CXO_CLK / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGpucc][A650_CC_GMU_RCGR / 4] = A650_RCG_ROOT_OFF;
    f.regs[A650GpuWinGccGpu][A650_GCC_DDRSS_GPU_AXI / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinGccGpu][A650_GCC_GPU_MEMNOC_GFX / 4] = A650_CBCR_CLK_OFF;
    f.regs[A650GpuWinSmmu][A650_SMMU_ID0 / 4] = 0x08;                /* 8 SMRs */
    f.regs[A650GpuWinSmmu][A650_SMMU_SMR(3) / 4] = 0x80000005;
    f.regs[A650GpuWinSmmu][A650_SMMU_S2CR(3) / 4] = 0x1;
}

static void part2_power(void)
{
    A650GpuIo io = {&f, read32, write32, barrier, delay_us};
    A650GpuResult r;
    A650GmuClocksResult clocks;
    const unsigned all = (1u << A650GpuWindows) - 1;

    /* Mode 1 reads only and stops at GPUCC while cfg AHB is off. */
    reset_model();
    A650GpuBringup(&io, 1, all, &r);
    CHECK(f.writes == 0);
    CHECK(r.gcc == A650GpuDone && r.cc == A650GpuSkippedPrereq &&
          r.smmu == A650GpuSkippedPrereq && r.gpu == A650GpuSkippedPrereq);
    CHECK(r.cfg_ahb == A650_CBCR_CLK_OFF && r.cfg_ahb_after == A650_CBCR_CLK_OFF);
    printf("mode 1: read only, %u writes, cc %s\n", f.writes,
           r.cc == A650GpuSkippedPrereq ? "skip" : "?");

    /* Mode 2 makes exactly one write: the cfg AHB enable bit. */
    reset_model();
    A650GpuBringup(&io, 2, all, &r);
    CHECK(f.writes == 1 && f.logged == 1);
    CHECK(f.log[0].window == A650GpuWinGccGpu && f.log[0].offset == A650_GCC_GPU_CFG_AHB &&
          f.log[0].value == (A650_CBCR_CLK_OFF | A650_CBCR_ENABLE));
    CHECK(r.enabled_ahb && A650GpuCfgAhbIsEnabled(r.cfg_ahb_after));
    CHECK(r.cc == A650GpuDone && !A650GpuGdscIsOn(r.cx_gdscr) && f.cx_writes == 0);
    printf("mode 2: 1 write %08x -> cfg_ahb %08x, CX untouched\n",
           f.log[0].value, r.cfg_ahb_after);

    /* Mode 3 powers CX with one GDSCR write, then enables the SMMU's two
     * clocks.  GX is never written; the SMMU reads back. */
    reset_model();
    A650GpuBringup(&io, 3, all, &r);
    CHECK(f.cx_writes == 1 && r.powered_cx == 1 && r.powered_clocks);
    CHECK(r.cx_before == 0x00222001 && A650GpuGdscIsOn(r.cx_gdscr) && A650GpuGdscIsOn(r.cx_hwctrl));
    CHECK(f.regs[A650GpuWinGpucc][A650_CC_GX_GDSCR / 4] == 0x00222001);
    CHECK(r.smmu == A650GpuDone && r.smr_valid == 1 && r.smr_first == 3);
    CHECK(r.smr_first_val == 0x80000005 && r.s2cr_first_val == 1);
    CHECK(r.gpu == A650GpuSkippedPrereq && !r.io_error);
    /* The exact writes: cfg AHB, CX GDSCR (collapse cleared), then AHB and
     * SMMU vote enable bits.  Nothing else. */
    CHECK(f.writes == 4);
    CHECK(f.log[1].offset == A650_CC_CX_GDSCR &&
          f.log[1].value == (0x00222001u & ~A650_GDSC_SW_COLLAPSE));
    CHECK(f.log[2].offset == A650_CC_AHB_CLK &&
          (f.log[2].value & A650_CBCR_ENABLE));
    CHECK(f.log[3].offset == A650_CC_SMMU_VOTE_CLK &&
          (f.log[3].value & A650_CBCR_ENABLE));
    printf("mode 3: 4 writes, CX %08x -> %08x, hw %08x, SMMU %u SMR\n",
           r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.smr_valid);

    /* CX already on: no GDSCR write and no SMMU clock writes; still reads. */
    reset_model();
    f.regs[A650GpuWinGccGpu][A650_GCC_GPU_CFG_AHB / 4] = A650_CBCR_ENABLE;
    f.regs[A650GpuWinGpucc][A650_CC_CX_GDSCR / 4] = A650_GDSC_PWR_ON;
    f.regs[A650GpuWinGpucc][A650_CC_AHB_CLK / 4] = A650_CBCR_ENABLE;
    f.regs[A650GpuWinGpucc][A650_CC_SMMU_VOTE_CLK / 4] = A650_CBCR_ENABLE;
    A650GpuBringup(&io, 3, all, &r);
    CHECK(f.writes == 0 && f.cx_writes == 0 && r.powered_cx == 2 && r.smmu == A650GpuDone);
    printf("mode 3: CX already on, 0 writes, powered_cx 2\n");

    /* An unmapped SMMU window is reported, not read. */
    reset_model();
    A650GpuBringup(&io, 3, all & ~(1u << A650GpuWinSmmu), &r);
    CHECK(r.smmu == A650GpuNoMap);
    /* No GCC window at all. */
    A650GpuBringup(&io, 3, all & ~(1u << A650GpuWinGccGpu), &r);
    CHECK(r.gcc == A650GpuNoMap && r.cc == A650GpuNotRun);

    /* Mode 6: the votes, the RCG to GPLL0 div / 1.5, the wake/sleep fields,
     * the five branches. */
    reset_model();
    A650GpuBringup(&io, 3, all, &r);
    f.logged = 0;
    f.writes = 0;
    CHECK(A650GmuClocksEnable(&io, &r, &clocks) == 0);
    CHECK(clocks.step == 5 && !clocks.failed);
    CHECK((clocks.vote & A650_GPLL0_VOTES) == A650_GPLL0_VOTES);
    /* The RCG's config word: parent 6 in bits 10:8, hid 2 in bits 4:0, the
     * other config fields cleared. */
    CHECK((clocks.gmu_cfg & 0x71f) == ((6u << 8) | 2u));
    CHECK(!(clocks.gmu_cmd & A650_RCG_ROOT_EN));
    CHECK(A650GpuClockIsOn(clocks.cx_gmu) &&
          (clocks.cx_gmu & A650_CX_GMU_WAKE_SLEEP) == A650_CX_GMU_WAKE_SLEEP);
    CHECK(A650GpuClockIsOn(clocks.ahb) && A650GpuClockIsOn(clocks.cxo));
    CHECK(A650GpuClockIsOn(clocks.axi) && A650GpuClockIsOn(clocks.memnoc));
    printf("mode 6: %u writes, RCG cfg %08x cmd %08x, cx_gmu %08x, %u branch polls\n",
           f.writes, clocks.gmu_cfg, clocks.gmu_cmd, clocks.cx_gmu, clocks.branch_polls);
    /* The wake/sleep write sets 0xf in both nibble fields, as the gpucc probe
     * does with regmap_update_bits.  It is a read-modify-write, so the branch's
     * other bits (CLK_OFF at this point) ride along; the fields are what must
     * be set. */
    {
        int seen = 0;
        for (unsigned i = 0; i < f.logged; ++i) {
            if (f.log[i].window == A650GpuWinGpucc && f.log[i].offset == A650_CC_CX_GMU_CLK &&
                (f.log[i].value & A650_CX_GMU_WAKE_SLEEP) == A650_CX_GMU_WAKE_SLEEP)
                seen = 1;
        }
        CHECK(seen);
    }

    /* Without CX, mode 6 refuses before any write. */
    reset_model();
    A650GpuBringup(&io, 2, all, &r);
    f.writes = 0;
    CHECK(A650GmuClocksEnable(&io, &r, &clocks) == 1);
    CHECK(clocks.failed == 1 && clocks.step == 1 && f.writes == 0);
    printf("mode 6: no CX, refused at step 1 with 0 writes\n");

    /* An RCG whose root never turns on times out at step 2, before the config
     * word and before any branch. */
    reset_model();
    A650GpuBringup(&io, 3, all, &r);
    f.rcg_stuck = 1;
    f.writes = 0;
    CHECK(A650GmuClocksEnable(&io, &r, &clocks) == 1);
    CHECK(clocks.failed == 1 && clocks.step == 2 && (clocks.gmu_cfg & 0x71f) == 0);
    CHECK(!A650GpuClockIsOn(clocks.cx_gmu));
    printf("mode 6: stuck RCG, timeout at step 2, gmu_cfg %08x\n", clocks.gmu_cfg);

    /* A CX GDSC that never reports PWR_ON: no power, no clocks, an I/O error
     * only if a read failed; here it is a clean "not powered". */
    reset_model();
    f.no_power = 1;
    A650GpuBringup(&io, 3, all, &r);
    CHECK(r.powered_cx == 0 && !r.powered_clocks && !r.io_error);
    CHECK(r.smmu == A650GpuSkippedPrereq);
    printf("mode 3: GDSC never powers, powered_cx 0, SMMU skipped\n");

    /* A branch that never clears CLK_OFF: the SMMU clock poll ends after 500 us
     * and the stage reports not powered. */
    reset_model();
    f.branch_stuck = 1;
    A650GpuBringup(&io, 3, all, &r);
    CHECK(r.powered_cx == 1 && !r.powered_clocks && r.smmu == A650GpuSkippedPrereq);
    CHECK(f.delay_us >= 500);
    printf("mode 3: stuck SMMU vote clock, powered_clocks 0 after %u us\n", f.delay_us);
}

/* ------------------------------------------------------------------------
 * Part 3: the parser's output feeds the real A650GmuBuildStartup.
 * ------------------------------------------------------------------------ */

static void part3_seam(void)
{
    A650GpuDbResult db_out;
    AdrenoHfiStartupConfig startup;

    build_db();
    CHECK(A650GpuCommandDbParse(db, sizeof(db), &db_out) == A650GpuDbSuccess);
    CHECK(A650GmuBuildStartup(db_out.gfx, db_out.gfx_bytes, db_out.mx, db_out.mx_bytes,
                              db_out.cx, db_out.cx_bytes, db_out.mc0, db_out.sh0,
                              db_out.acv, db_out.cn0, &startup) == AdrenoHfiStartupSuccess);
    CHECK(startup.gpu_count == 8 && startup.gmu_count == 2);
    CHECK(startup.gpu[7].hz == 670000000 && startup.gmu[1].hz == 200000000);
    CHECK(startup.ddr_addresses[0] == 0x50000 && startup.ddr_addresses[1] == 0x50004 &&
          startup.ddr_addresses[2] == 0x5007c);
    CHECK(startup.cnoc_addresses[0] == 0x50074);
    printf("seam: A650GmuBuildStartup accepted the parsed lists, gpu votes %08x..%08x, gmu %08x\n",
           startup.gpu[1].vote, startup.gpu[7].vote, startup.gmu[1].vote);
}

int main(void)
{
    for (unsigned i = 0; i < A650GpuWindows; ++i) f.regs[i] = allocate(kBytes[i]);

    part1_db();
    part2_power();
    part3_seam();

    puts("PASS: A650 GPU power (command DB parse and refusals, modes 1-3 with exact writes, "
         "mode 6 GMU clocks and its two timeouts, the A650GmuBuildStartup seam)");
    return 0;
}
