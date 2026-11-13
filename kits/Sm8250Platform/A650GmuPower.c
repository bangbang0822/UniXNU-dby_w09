/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A650 CX power-on, GPU clock bring-up and AOP command-DB parser.  See
 * A650GmuPower.h for the register map and its sources.  This file holds no
 * board policy: the caller decides the mode, supplies the window mapping, and
 * runs the sequence in the order it needs. */
#include "A650GmuPower.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * AOP command database.
 *
 * Layout (cmd-db.c, Linux v6.6):
 *   0   u32 version
 *   4   u32 magic 0x0c0330db
 *   8   eight 16-byte slave records {u16 slv_id, u16 header_offset,
 *       u16 data_offset, u16 cnt, u16 version, 6 bytes reserved}
 *   136 u32 checksum, 140 u32 reserved
 *   144 data area
 * A slave's entries sit at 144 + header_offset, 24 bytes each:
 *   u8 id[8], u16 priority, u32 address, u16 len, u16 offset
 * The aux data sits at 144 + data_offset + offset.
 * The slave table ends at the first record whose slv_id is zero.
 * ------------------------------------------------------------------------- */

#define A650_CMD_DB_SLAVES      8u
#define A650_CMD_DB_SLAVE_BYTES 16u
#define A650_CMD_DB_ENTRY_BYTES 24u

/* The eight resources the GMU's votes need, in the order A650GmuBuildStartup
 * takes them: three ARC level lists, the GMU's optional gmxc list, and the
 * four BCM addresses. */
enum {
    A650DbGfxLvl, A650DbMxLvl, A650DbCxLvl, A650DbGmxcLvl,
    A650DbSh0, A650DbMc0, A650DbAcv, A650DbCn0, A650DbNames
};

static const char *const kA650DbNames[A650DbNames] = {
    "gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0"
};

/* An entry id is eight bytes, zero padded, so a name matches only when every
 * byte agrees through the name's terminator.  A prefix does not match. */
static int A650DbIdIs(const uint8_t *id, const char *name)
{
    unsigned k;
    for (k = 0; k < 8; ++k) {
        if (id[k] != (uint8_t)name[k]) return 0;
        if (!name[k]) return 1;
    }
    return 1;
}

/* at + len must stay inside bytes, without overflow. */
static int A650DbInBounds(size_t at, size_t len, size_t bytes)
{
    return at <= bytes && len <= bytes - at;
}

A650GpuDbStatus A650GpuCommandDbParse(const uint8_t *db, size_t bytes, A650GpuDbResult *out)
{
    A650GpuDbEntry found[A650DbNames];
    unsigned seen = 0, slave_index, name;

    if (!db || !out) return A650GpuDbCorrupt;
    if (bytes < A650_CMD_DB_DATA_OFFSET || A650GpuLe32(db + 4) != A650_CMD_DB_MAGIC)
        return A650GpuDbCorrupt;
    memset(found, 0, sizeof(found));

    for (slave_index = 0; slave_index < A650_CMD_DB_SLAVES; ++slave_index) {
        const uint8_t *record = db + 8 + A650_CMD_DB_SLAVE_BYTES * slave_index;
        uint32_t slave = A650GpuLe16(record);
        uint32_t headers = A650GpuLe16(record + 2);
        uint32_t data = A650GpuLe16(record + 4);
        uint32_t count = A650GpuLe16(record + 6);
        size_t entries_at = A650_CMD_DB_DATA_OFFSET + headers;
        uint32_t i;

        if (!slave) break;                     /* end of the slave table */
        if (!A650DbInBounds(entries_at, (size_t)count * A650_CMD_DB_ENTRY_BYTES, bytes))
            return A650GpuDbCorrupt;
        for (i = 0; i < count; ++i) {
            const uint8_t *entry = db + entries_at + A650_CMD_DB_ENTRY_BYTES * i;
            uint32_t address = A650GpuLe32(entry + 16);
            size_t len = A650GpuLe16(entry + 20);
            size_t aux_at = A650_CMD_DB_DATA_OFFSET + data + A650GpuLe16(entry + 22);

            /* Every slave is searched for every name, as pipa's
             * gpb_cmd_db_find does.  The database's own slave assignment is
             * not re-checked here: alioth's AOP database has not been read, so
             * a rule about which slave holds which name would be a guess, and
             * a wrong guess would reject a valid database.  The first match
             * wins; a duplicate under a later slave is not an error. */
            for (name = 0; name < A650DbNames; ++name) {
                if (seen & (1u << name)) continue;
                if (!A650DbIdIs(entry, kA650DbNames[name])) continue;
                if (!A650DbInBounds(aux_at, len, bytes)) return A650GpuDbCorrupt;
                found[name] = (A650GpuDbEntry){address, slave, db + aux_at, (unsigned)len};
                seen |= 1u << name;
                break;
            }
        }
    }

    /* gfx, mx, cx and the four BCM addresses are required; gmxc is optional,
     * exactly as a6xx_gmu.c treats it. */
    {
        const unsigned required = ((1u << A650DbNames) - 1u) & ~(1u << A650DbGmxcLvl);
        if ((seen & required) != required) return A650GpuDbMissing;
    }

    *out = (A650GpuDbResult){
        found[A650DbGfxLvl].aux,  found[A650DbGfxLvl].aux_bytes,
        found[A650DbMxLvl].aux,   found[A650DbMxLvl].aux_bytes,
        found[A650DbCxLvl].aux,   found[A650DbCxLvl].aux_bytes,
        found[A650DbGmxcLvl].aux, found[A650DbGmxcLvl].aux_bytes,
        found[A650DbSh0].address, found[A650DbMc0].address,
        found[A650DbAcv].address, found[A650DbCn0].address,
        (seen >> A650DbGmxcLvl) & 1u
    };
    return A650GpuDbSuccess;
}

/* -------------------------------------------------------------------------
 * Staged bring-up (pipa GpuProbe.cpp modes 1..3).
 * ------------------------------------------------------------------------- */

static int A650GpuRead(const A650GpuIo *io, A650GpuResult *r, unsigned window,
                       uint32_t offset, uint32_t *out)
{
    uint32_t value = 0;
    if (io->read32(io->cookie, window, offset, &value)) {
        r->io_error = 1;
        *out = 0;
        return -1;
    }
    *out = value;
    return 0;
}

static void A650GpuDelay(const A650GpuIo *io, unsigned us)
{
    if (io->delay_us) io->delay_us(io->cookie, us);
}

void A650GpuBringup(const A650GpuIo *io, unsigned mode, unsigned have_windows, A650GpuResult *r)
{
    unsigned n;

    if (!io || !r) return;
    memset(r, 0, sizeof(*r));
    r->mode = mode;
    if (!mode || !(have_windows & (1u << A650GpuWinGccGpu))) {
        r->gcc = mode ? A650GpuNoMap : A650GpuSkippedMode;
        return;
    }

    /* GCC: read the four GPU branches and the GPLL0 vote page. */
    if (A650GpuRead(io, r, A650GpuWinGccGpu, A650_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    A650GpuRead(io, r, A650GpuWinGccGpu, A650_GCC_GPU_MEMNOC_GFX, &r->memnoc);
    A650GpuRead(io, r, A650GpuWinGccGpu, A650_GCC_GPU_SNOC_DVM_GFX, &r->dvm);
    A650GpuRead(io, r, A650GpuWinGccGpu, A650_GCC_GPU_MISC, &r->misc);
    if (have_windows & (1u << A650GpuWinGccVote))
        A650GpuRead(io, r, A650GpuWinGccVote, A650_GCC_PLL_VOTE, &r->pll_vote);
    r->cfg_ahb_after = r->cfg_ahb;
    /* Mode 2's one write, the same the gcc-sm8250 probe makes.  The branch has
     * hardware clock gating, so CLK_OFF can read set on an enabled branch; the
     * enable bit is the test. */
    if (!A650GpuCfgAhbIsEnabled(r->cfg_ahb) && mode >= 2) {
        if (!io->write32(io->cookie, A650GpuWinGccGpu, A650_GCC_GPU_CFG_AHB,
                         r->cfg_ahb | A650_CBCR_ENABLE)) {
            r->enabled_ahb = 1;
            A650GpuRead(io, r, A650GpuWinGccGpu, A650_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        } else {
            r->io_error = 1;
        }
    }
    r->gcc = A650GpuDone;

    /* GPUCC needs gcc_gpu_cfg_ahb_clk on. */
    if (!(have_windows & (1u << A650GpuWinGpucc))) {
        r->cc = A650GpuNoMap;
    } else if (!A650GpuCfgAhbIsEnabled(r->cfg_ahb_after)) {
        r->cc = A650GpuSkippedPrereq;
    } else {
        A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_CX_GDSCR, &r->cx_gdscr);
        A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_GX_GDSCR, &r->gx_gdscr);
        A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_AHB_CLK, &r->cc_ahb);
        A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_SMMU_VOTE_CLK, &r->cc_smmu);
        A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_CX_GMU_CLK, &r->cc_gmu);
        r->cc = A650GpuDone;
    }

    /* Mode 3: gdsc_enable for gpu_cx_gdsc.  It is VOTABLE with gds_hw_ctrl, so
     * the sequence is gdsc_toggle_logic's: clear SW_COLLAPSE, wait 1 us, then
     * poll gds_hw_ctrl for PWR_ON up to STATUS_POLL_TIMEOUT_US.  GX is never
     * touched: the GMU owns it. */
    r->cx_before = r->cx_gdscr;
    r->cc_smmu_before = r->cc_smmu;
    if (mode >= 3 && r->cc == A650GpuDone) {
        int bad = 0;
        if (!A650GpuGdscIsOn(r->cx_gdscr)) {
            uint32_t status = 0;
            bad = io->write32(io->cookie, A650GpuWinGpucc, A650_CC_CX_GDSCR,
                              r->cx_gdscr & ~A650_GDSC_SW_COLLAPSE);
            if (!bad) {
                unsigned waited = 0;
                A650GpuDelay(io, 1);
                for (;;) {
                    if (A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_CX_GDS_HW_CTRL, &status)) {
                        bad = 1;
                        break;
                    }
                    if (A650GpuGdscIsOn(status) || waited >= A650_GDSC_POLL_US) break;
                    A650GpuDelay(io, 10);
                    waited += 10;
                }
                r->cx_hwctrl = status;
                A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_CX_GDSCR, &r->cx_gdscr);
                r->powered_cx = !bad && A650GpuGdscIsOn(status);
            }
        } else {
            r->powered_cx = 2;                 /* already on */
        }
        if (bad) r->io_error = 1;

        /* The clocks the adreno SMMU node lists: gpu_cc_ahb_clk and the HLOS
         * vote of the SMMU clock.  Both are voted branches: the enable bit is
         * the write, CLK_OFF the confirmation. */
        if (r->powered_cx) {
            uint32_t ahb = 0, vote = 0;
            int cbad;
            cbad = A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_AHB_CLK, &ahb) |
                   A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_SMMU_VOTE_CLK, &vote);
            if (!cbad && !(ahb & A650_CBCR_ENABLE))
                cbad |= io->write32(io->cookie, A650GpuWinGpucc, A650_CC_AHB_CLK,
                                    ahb | A650_CBCR_ENABLE);
            if (!cbad && !(vote & A650_CBCR_ENABLE))
                cbad |= io->write32(io->cookie, A650GpuWinGpucc, A650_CC_SMMU_VOTE_CLK,
                                    vote | A650_CBCR_ENABLE);
            {
                unsigned waited = 0;
                while (!cbad) {
                    if (A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_SMMU_VOTE_CLK, &r->cc_smmu)) {
                        cbad = 1;
                        break;
                    }
                    if (A650GpuClockIsOn(r->cc_smmu) || waited >= 500) break;
                    A650GpuDelay(io, 10);
                    waited += 10;
                }
            }
            A650GpuRead(io, r, A650GpuWinGpucc, A650_CC_AHB_CLK, &r->cc_ahb);
            r->powered_clocks = !cbad && A650GpuClockIsOn(r->cc_smmu);
            if (cbad) r->io_error = 1;
        }
    }

    /* SMMU global space 0, only with CX on and its vote clock running. */
    if (!(have_windows & (1u << A650GpuWinSmmu))) {
        r->smmu = A650GpuNoMap;
    } else if (r->cc != A650GpuDone || !A650GpuGdscIsOn(r->cx_gdscr) ||
               !A650GpuClockIsOn(r->cc_smmu)) {
        r->smmu = A650GpuSkippedPrereq;
    } else {
        A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_SCR0, &r->scr0);
        A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_ID0, &r->id0);
        A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_ID1, &r->id1);
        A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_ID2, &r->id2);
        A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_GFSR, &r->gfsr);
        n = A650GpuSmmuNumSmrs(r->id0);
        if (n > A650_SMMU_MAX_SMR) n = A650_SMMU_MAX_SMR;
        r->smr_first = 0xffffffffu;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t smr = 0;
            if (A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_SMR(i), &smr)) break;
            if (smr & (1u << 31)) {
                ++r->smr_valid;
                if (r->smr_first == 0xffffffffu) {
                    r->smr_first = i;
                    r->smr_first_val = smr;
                    A650GpuRead(io, r, A650GpuWinSmmu, A650_SMMU_S2CR(i), &r->s2cr_first_val);
                }
            }
        }
        r->smmu = A650GpuDone;
    }

    /* RBBM_STATUS is in the GX-gated space, so it needs both domains on. */
    if (!(have_windows & (1u << A650GpuWinGpu))) {
        r->gpu = A650GpuNoMap;
    } else if (r->cc != A650GpuDone || !A650GpuGdscIsOn(r->cx_gdscr) ||
               !A650GpuGdscIsOn(r->gx_gdscr)) {
        r->gpu = A650GpuSkippedPrereq;
    } else {
        A650GpuRead(io, r, A650GpuWinGpu, A650_RBBM_STATUS_BYTES, &r->rbbm_status);
        r->gpu = A650GpuDone;
    }
}

/* -------------------------------------------------------------------------
 * Mode 6: the GMU clocks a6xx_gmu_resume enables, at the GMU OPP's 200 MHz.
 * ------------------------------------------------------------------------- */

static int A650GpuSetBits(const A650GpuIo *io, A650GpuResult *r, unsigned window,
                          uint32_t offset, uint32_t bits)
{
    uint32_t value = 0;
    if (A650GpuRead(io, r, window, offset, &value)) return -1;
    if ((value & bits) == bits) return 0;
    return io->write32(io->cookie, window, offset, value | bits) ? -1 : 0;
}

/* clk_rcg2's update: force the root on, wait for ROOT_OFF to clear, write the
 * configuration word, set CMD_UPDATE, wait for it to self-clear, drop the root
 * force.  Returns 0 done, 1 timeout, -1 I/O error. */
static int A650GpuRcgSet(const A650GpuIo *io, A650GpuResult *r, uint32_t rcgr,
                         uint32_t sel, uint32_t hid, uint32_t *cfg_after,
                         unsigned *polls)
{
    uint32_t cmd = 0, cfg = 0;
    int timeout = 1;

    if (A650GpuRead(io, r, A650GpuWinGpucc, rcgr, &cmd) ||
        io->write32(io->cookie, A650GpuWinGpucc, rcgr, cmd | A650_RCG_ROOT_EN))
        return -1;
    for (unsigned i = 0; i < 500; ++i) {
        if (A650GpuRead(io, r, A650GpuWinGpucc, rcgr, &cmd)) return -1;
        if (!(cmd & A650_RCG_ROOT_OFF)) {
            timeout = 0;
            break;
        }
        A650GpuDelay(io, 1);
    }
    if (timeout) return 1;

    if (A650GpuRead(io, r, A650GpuWinGpucc, rcgr + 4, &cfg) ||
        io->write32(io->cookie, A650GpuWinGpucc, rcgr + 4,
                    (cfg & ~A650_RCG_CFG_CLEAR) | sel << 8 | hid))
        return -1;
    if (A650GpuRead(io, r, A650GpuWinGpucc, rcgr, &cmd) ||
        io->write32(io->cookie, A650GpuWinGpucc, rcgr, cmd | A650_RCG_CMD_UPDATE))
        return -1;

    timeout = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++*polls;
        if (A650GpuRead(io, r, A650GpuWinGpucc, rcgr, &cmd)) return -1;
        if (!(cmd & A650_RCG_CMD_UPDATE)) {
            timeout = 0;
            break;
        }
        A650GpuDelay(io, 1);
    }
    if (timeout) return 1;
    if (io->write32(io->cookie, A650GpuWinGpucc, rcgr, cmd & ~A650_RCG_ROOT_EN)) return -1;
    return A650GpuRead(io, r, A650GpuWinGpucc, rcgr + 4, cfg_after) ? -1 : 0;
}

int A650GmuClocksEnable(const A650GpuIo *io, const A650GpuResult *r, A650GmuClocksResult *o)
{
    /* The reads below need a result to record their I/O errors in.  The
     * caller's result is not modified: its stage fields are copied into a
     * local, whose io_error is folded into `o` at the end. */
    A650GpuResult local;
    int error = 0;

    if (!io || !r || !o) return -1;
    memset(o, 0, sizeof(*o));
    local = *r;

    o->step = 1;
    if (r->cc != A650GpuDone || !A650GpuGdscIsOn(r->cx_gdscr)) {
        o->failed = 1;
        return 1;
    }

    /* 1: the GPLL0 GPU votes the GMU's clocks hang from. */
    if (A650GpuSetBits(io, &local, A650GpuWinGccVote, A650_GCC_PLL_VOTE, A650_GPLL0_VOTES))
        goto io_error;
    A650GpuDelay(io, 10);

    /* 2: gpu_cc_gmu_clk_src to GPLL0 div (sel 6) / 1.5 (hid 2). */
    o->step = 2;
    error = A650GpuRcgSet(io, &local, A650_CC_GMU_RCGR, A650_RCG_SRC_SEL,
                          A650_RCG_SRC_HID, &o->gmu_cfg, &o->rcg_polls);
    if (error) {
        o->failed = error < 0 ? 2 : 1;
        goto out;
    }

    /* 3: the wake/sleep fields of gpu_cc_cx_gmu_clk, as the gpucc probe sets
     * them. */
    o->step = 3;
    if (A650GpuSetBits(io, &local, A650GpuWinGpucc, A650_CC_CX_GMU_CLK, A650_CX_GMU_WAKE_SLEEP))
        goto io_error;

    /* 4: the five branches, then gpu_cc_cx_gmu_clk's CLK_OFF clear. */
    o->step = 4;
    if (A650GpuSetBits(io, &local, A650GpuWinGpucc, A650_CC_AHB_CLK, A650_CBCR_ENABLE) ||
        A650GpuSetBits(io, &local, A650GpuWinGpucc, A650_CC_CX_GMU_CLK, A650_CBCR_ENABLE) ||
        A650GpuSetBits(io, &local, A650GpuWinGpucc, A650_CC_CXO_CLK, A650_CBCR_ENABLE) ||
        A650GpuSetBits(io, &local, A650GpuWinGccGpu, A650_GCC_DDRSS_GPU_AXI, A650_CBCR_ENABLE) ||
        A650GpuSetBits(io, &local, A650GpuWinGccGpu, A650_GCC_GPU_MEMNOC_GFX, A650_CBCR_ENABLE))
        goto io_error;
    error = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++o->branch_polls;
        if (A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_CX_GMU_CLK, &o->cx_gmu))
            goto io_error;
        if (A650GpuClockIsOn(o->cx_gmu)) {
            error = 0;
            break;
        }
        A650GpuDelay(io, 1);
    }
    if (error) {
        o->failed = 1;
        goto out;
    }
    o->step = 5;
    goto out;

io_error:
    o->failed = 2;
out:
    /* Read back the whole chain, so a log shows what the sequence left. */
    A650GpuRead(io, &local, A650GpuWinGccVote, A650_GCC_PLL_VOTE, &o->vote);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_GMU_RCGR, &o->gmu_cmd);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_GMU_RCGR + 4, &o->gmu_cfg);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_CX_GMU_CLK, &o->cx_gmu);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_AHB_CLK, &o->ahb);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_CXO_CLK, &o->cxo);
    A650GpuRead(io, &local, A650GpuWinGccGpu, A650_GCC_DDRSS_GPU_AXI, &o->axi);
    A650GpuRead(io, &local, A650GpuWinGccGpu, A650_GCC_GPU_MEMNOC_GFX, &o->memnoc);
    A650GpuRead(io, &local, A650GpuWinGpucc, A650_CC_PLL1_MODE, &o->pll1_mode);
    /* A read that failed during the readback is an I/O failure, not a
     * sequence result: it must not be reported as success.  The sequence
     * itself reached its last step, so `step` stays where it is. */
    if (local.io_error) o->failed = 2;
    return o->failed ? 1 : 0;
}
