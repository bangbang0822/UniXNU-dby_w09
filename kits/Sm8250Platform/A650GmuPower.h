/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A650 CX power-on, GPU clock bring-up, and AOP command-DB parser for SM8250 / Adreno 650,
 * after UniXNU-pipa's staged sequence in GpuProbe.cpp modes 2, 3, 5, 6 (2026-10-24).
 *
 * This is the pure C subset of pipa's GpuPowerBoard.h, ported without A740 dependencies,
 * matching SM8250's device tree and Linux v6.6 clk-rcg2.c / gpucc-sm8250.c / gcc-sm8250.c /
 * gdsc.c / cmd-db.c exactly, and shaped to feed A650GmuColdStart:
 *
 *  - mode 1: read-only GCC, GPUCC, SMMU, GPU RBBM_STATUS (no writes)
 *  - mode 2: enable gcc_gpu_cfg_ahb_clk (single branch, no power)
 *  - mode 3: gdsc_enable for gpu_cx_gdsc (VOTABLE, gds_hw_ctrl), then the gpu_cc_ahb_clk
 *            and gpu_cc_hlos1_vote_gpu_smmu_clk branches
 *  - mode 6: GPLL0 GPU votes, gpu_cc_gmu_clk_src at 200 MHz (P_GPLL0_OUT_MAIN_DIV / hid 2),
 *            gpu_cc_cx_gmu_clk's wake/sleep fields at 0xf, then the five branches
 *
 * Windows 0..6 are the 7 MMIO regions, as the I/O struct maps them.  No hardware is touched
 * outside mode >= 2, and every register the sequence may write is listed explicitly.
 * Host-tested by A650GmuPowerTests.c.
 */
#ifndef A650_GMU_POWER_H
#define A650_GMU_POWER_H
#include <stdint.h>
#include <stddef.h>

/* DTS / SM8250 register ranges (pipa.dts agrees with Linux v6.6 sm8250.dtsi). */
#define A650_GPU_BASE_GCC_GPU     0x00171000ULL   /* GCC 0x100000 + 0x71000 */
#define A650_GPU_BASE_GCC_VOTE    0x00152000ULL   /* GCC 0x100000 + 0x52000 */
#define A650_GPU_BASE_GPUCC       0x03d90000ULL   /* qcom,gpucc@3d90000 size 0x9000 */
#define A650_GPU_BASE_SMMU_GR0    0x03da0000ULL   /* qcom,kgsl-smmu@3da0000 size 0x10000 */
#define A650_GPU_BASE_GPU         0x03d00000ULL   /* qcom,kgsl-3d0@3d00000 size 0x40000 */
#define A650_GPU_BASE_GMU         0x03d6a000ULL   /* qcom,gmu@3d6a000 size 0x30000 */
#define A650_GPU_BASE_RSCC        0x03de0000ULL   /* qcom,kgsl-3d0 reg "rscc" size 0x10000 */

/* AOP command-db reservation. DTS reserved-memory@80860000 size 0x20000. */
#define A650_CMD_DB_BASE          0x80860000ULL
#define A650_CMD_DB_BYTES         0x20000u

enum {A650GpuWinGccGpu, A650GpuWinGccVote, A650GpuWinGpucc, A650GpuWinSmmu,
      A650GpuWinGpu, A650GpuWinGmu, A650GpuWinRscc, A650GpuWindows};

/* ---------------------------------------------------------------------------
 * Window 0 (GCC_GPU): registers relative to A650_GPU_BASE_GCC_GPU.
 * Source: gcc-sm8250.c (Linux v6.6).
 */
#define A650_GCC_GPU_CFG_AHB          0x004    /* gcc_gpu_cfg_ahb_clk */
#define A650_GCC_GPU_MEMNOC_GFX       0x00c    /* gcc_gpu_memnoc_gfx_clk */
#define A650_GCC_GPU_SNOC_DVM_GFX     0x018    /* gcc_gpu_snoc_dvm_gfx_clk */
#define A650_GCC_GPU_MISC             0x028    /* disable GPLL0 active input bits 1:0 */
#define A650_GCC_DDRSS_GPU_AXI        0x154    /* gcc_ddrss_gpu_axi_clk */
/* Window 1 (GCC_VOTE): GCC 0x100000 + 0x52000, the two GPLL0 GPU votes. */
#define A650_GCC_PLL_VOTE             0x000
#define A650_GPLL0_VOTES              ((1u << 15) | (1u << 16))

/* Common clk-branch flags. */
#define A650_CBCR_CLK_OFF             (1u << 31)
#define A650_CBCR_ENABLE              (1u << 0)

/* ---------------------------------------------------------------------------
 * Window 2 (GPUCC): registers relative to A650_GPU_BASE_GPUCC (0x3d90000).
 * Source: gpucc-sm8250.c (Linux v6.6).
 */
#define A650_CC_CX_GDSCR              0x106c   /* gpu_cx_gdsc: SW_COLLAPSE bit 0 */
#define A650_CC_GX_GDSCR              0x100c   /* gpu_gx_gdsc: owned by GMU */
#define A650_CC_CX_GDS_HW_CTRL        0x1540   /* VOTABLE gds_hw_ctrl */
#define A650_CC_AHB_CLK               0x1078   /* gpu_cc_ahb_clk */
#define A650_CC_CX_GMU_CLK            0x1098   /* gpu_cc_cx_gmu_clk: wake/sleep fields */
#define A650_CC_CXO_CLK               0x109c   /* gpu_cc_cxo_clk */
/* gpu_cc_cxo_aon_clk is 0x1004.  The GMU clock sequence does not write it, so
 * it is not defined here; the note is here so a later reader does not think it
 * was overlooked. */
#define A650_CC_SMMU_VOTE_CLK         0x5000   /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define A650_CC_GMU_RCGR              0x1120   /* gpu_cc_gmu_clk_src: cmd_rcgr */
#define A650_CC_PLL1_MODE             0x100    /* gpu_cc_pll1 (read only here) */

/* GDSC flags (gdsc.c). */
#define A650_GDSC_PWR_ON              (1u << 31)
#define A650_GDSC_SW_COLLAPSE         (1u << 0)
#define A650_GDSC_POLL_US             2000u

/* clk-rcg2 masks (Linux v6.6). */
#define A650_RCG_CMD_UPDATE           (1u << 0)
#define A650_RCG_ROOT_EN              (1u << 1)
#define A650_RCG_ROOT_OFF             (1u << 31)
#define A650_RCG_CFG_CLEAR            (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))
#define A650_RCG_SRC_SEL              6u       /* P_GPLL0_OUT_MAIN_DIV (parent 6) */
#define A650_RCG_SRC_HID              2u       /* hid = 2 -> 1.5 divider */
#define A650_CX_GMU_WAKE_SLEEP        (0xffu << 4)   /* 0xf in both fields */

/* ---------------------------------------------------------------------------
 * Window 3 (SMMU): global space 0, AArch64 SMMU-500.
 */
#define A650_SMMU_SCR0                0x000
#define A650_SMMU_ID0                 0x020
#define A650_SMMU_ID1                 0x024
#define A650_SMMU_ID2                 0x028
#define A650_SMMU_GFSR                0x048
#define A650_SMMU_SMR(n)              (0x800 + 4 * (n))
#define A650_SMMU_S2CR(n)             (0xc00 + 4 * (n))
#define A650_SMMU_MAX_SMR             128u

/* Window 4 (GPU): RBBM_STATUS. */
#define A650_RBBM_STATUS_BYTES        (0x0210 * 4u)   /* dword offset -> byte */

/* Command DB format (cmd-db.c, AOP 16 KiB database). */
#define A650_CMD_DB_MAGIC             0x0c0330dbu
#define A650_CMD_DB_DATA_OFFSET       144u

/* ---------------------------------------------------------------------------
 * Return types and the I/O vector.  Matches A650GmuIo in A650Gmu.h for the
 * GMU cold-start sequence so the same peer can drive both.
 */
typedef enum {
    A650GpuDbSuccess = 0,
    A650GpuDbCorrupt,     /* bad header magic, or an extent out of bounds */
    A650GpuDbMissing,     /* one or more required entries not found */
} A650GpuDbStatus;

typedef enum {
    A650GpuNotRun = 0, A650GpuDone, A650GpuSkippedPrereq,
    A650GpuSkippedMode, A650GpuNoMap,
} A650GpuStageState;

/* A single entry from the AOP command database. aux points into the input
 * byte buffer (no allocation). */
typedef struct {
    uint32_t address;
    uint32_t slave;
    const uint8_t *aux;
    unsigned aux_bytes;
} A650GpuDbEntry;

/* The ARC level lists (16-bit words) for A650GmuBuildStartup, plus the four
 * BCM register addresses the HFI bandwidth table names. */
typedef struct {
    const uint8_t *gfx;   size_t gfx_bytes;
    const uint8_t *mx;    size_t mx_bytes;
    const uint8_t *cx;    size_t cx_bytes;
    const uint8_t *gmxc;  size_t gmxc_bytes;  /* optional: not required */
    uint32_t sh0, mc0, acv, cn0;              /* BCM register addresses */
    int gmxc_present;
} A650GpuDbResult;

/* Bring-up results. */
typedef struct {
    unsigned mode;
    A650GpuStageState gcc, cc, smmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, memnoc, dvm, misc, pll_vote;
    uint32_t cx_gdscr, gx_gdscr, cc_ahb, cc_smmu, cc_gmu;
    uint32_t scr0, id0, id1, id2, gfsr;
    uint32_t smr_valid, smr_first, smr_first_val, s2cr_first_val;
    uint32_t rbbm_status;
    uint32_t cx_before, cx_hwctrl, cc_smmu_before;
    unsigned powered_cx, powered_clocks;
    unsigned enabled_ahb;
    int io_error;
} A650GpuResult;

/* Bring-up state for the GMU clock sequence (mode 6). */
typedef struct {
    unsigned step;               /* 1..4, 5 when all passed */
    int failed;                  /* 1 timeout or refusal at step, 2 I/O error */
    unsigned rcg_polls, branch_polls;
    uint32_t vote, gmu_cmd, gmu_cfg, cx_gmu, ahb, cxo, axi, memnoc, pll1_mode;
} A650GmuClocksResult;

/* The I/O vector.  Windows are numbered A650GpuWin*, the same seven windows
 * and the same numbering as pipa's GPB_WIN_* so its probe code and this kit
 * agree.
 *
 * read32, write32 and barrier have the same signatures, argument order and
 * success convention (0 = ok, byte offsets) as A650GmuIo in A650Gmu.h.  One
 * board driver therefore implements one set of callbacks and fills both
 * structs; the structs are not cast to each other, because A650Gmu.h numbers
 * only the three GMU windows (core, RSCC, PDC).  delay_us is extra here and
 * may be NULL, which makes the sequence run without waits in host tests.
 */
typedef struct A650GpuIo {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, size_t byte_offset, uint32_t *out);
    int (*write32)(void *cookie, unsigned window, size_t byte_offset, uint32_t value);
    int (*barrier)(void *cookie);                   /* dsb sy */
    void (*delay_us)(void *cookie, unsigned us);    /* may be NULL (host tests) */
} A650GpuIo;

#ifdef __cplusplus
extern "C" {
#endif

/* --- pure parsing (no I/O) --- */
/* Parse the complete AOP command database snapshot, return the eight entries
 * required to feed A650GmuBuildStartup.  No allocation; all aux pointers are
 * aliases into `db`.  Returns 0 on success. */
A650GpuDbStatus A650GpuCommandDbParse(const uint8_t *db, size_t bytes, A650GpuDbResult *out);

/* --- staged bring-up --- */
/* Modes 1..3: GCC, GPUCC (CX power), SMMU global space 0. */
void A650GpuBringup(const A650GpuIo *io, unsigned mode, unsigned have_windows, A650GpuResult *r);

/* Mode 6: after mode 3, the GMU clocks (200 MHz).  Returns 0 on success. */
int A650GmuClocksEnable(const A650GpuIo *io, const A650GpuResult *r, A650GmuClocksResult *o);

/* Helpers. */
static inline uint32_t A650GpuLe16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static inline uint32_t A650GpuLe32(const uint8_t *p) {return A650GpuLe16(p) | A650GpuLe16(p + 2) << 16;}
static inline int A650GpuClockIsOn(uint32_t cbcr) {return (cbcr & A650_CBCR_CLK_OFF) == 0;}
static inline int A650GpuGdscIsOn(uint32_t gdscr) {return (gdscr & A650_GDSC_PWR_ON) != 0;}
static inline int A650GpuCfgAhbIsEnabled(uint32_t cbcr) {return (cbcr & A650_CBCR_ENABLE) != 0;}
static inline unsigned A650GpuSmmuNumSmrs(uint32_t id0) {return id0 & 0xff;}

#ifdef __cplusplus
}
#endif
#endif
