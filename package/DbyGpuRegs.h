/* SPDX-License-Identifier: MIT */
/* DBY-W09 GPU register and window map: Huawei MatePad 11, SM8250 / kona,
 * Adreno 650 (chip id 0x06050002).
 *
 * PROVENANCE RULE (enforced by scripts/check-dby-gpu.py):
 *   Every numeric value below carries one of these tags on the same line or in
 *   the comment block immediately above it:
 *
 *     DTB:<node path>:<line>   _scratch/analysis/base-stock.dts, which is dtc's
 *                              output for _scratch/fw/parsed/dby-stock.dtb, the
 *                              DTB appended to BOOT_0.img of the official
 *                              DBY-W09 HOS 4.2.0.208 update_full_base.zip.
 *     LINUX:<url>              upstream Linux source, fetched 2026-10-XX.
 *     TODO(measure)            no source yet. The value is the sentinel below
 *                              and the driver refuses to program it.
 *
 * NOTHING here may come from the A740 / SM8550 components. A740 register
 * offsets, the A740 power-up list, the 0x13000 SQE length, the 0x43050a01
 * chip id and the gmu_gen70200 / a740_sqe / a740_zap firmware names are all
 * different hardware and are rejected by check-dby-gpu.py. */
#ifndef DBY_GPU_REGS_H
#define DBY_GPU_REGS_H

#include <stdint.h>

/* Sentinel for "the value is required but has not been measured". Distinct from
 * every real MMIO offset we expect, and refused by every programming path. */
#define DBY_GPU_TODO_MEASURE 0xFFFFFFFFu
#define DBY_GPU_IS_MEASURED(v) ((v) != DBY_GPU_TODO_MEASURE)

/* ------------------------------------------------------------- build gates */

/* The only store instruction this archive can contain lives behind this macro,
 * and the default is 0. With 0 the single write helper in DbyGpuProbe.cpp is not
 * compiled at all, so a call site that appears without flipping the macro is a
 * COMPILE error, not an MMIO store on a board where a wrong store does not fault
 * (gpu-plan.md risk R1: a write to 0x15000000 or 0x17410008 silently killed the
 * unit). scripts/build-dby-gpu.py also refuses to build when this value is not 0
 * unless --allow-writes is passed, so "read-only" is checked at build time too.
 *
 * Flipping it is NOT sufficient to write anything: every write class additionally
 * has to pass dby_gpu_plan_allows() (DbyGpuCensus.h:145), which requires both the
 * stage and the dby-gpu-confirm token. Two independent gates on purpose. */
#ifndef DBY_GPU_ENABLE_WRITES
#define DBY_GPU_ENABLE_WRITES 0
#endif
#if DBY_GPU_ENABLE_WRITES != 0 && DBY_GPU_ENABLE_WRITES != 1
#error "DBY_GPU_ENABLE_WRITES must be 0 or 1"
#endif

/* Boot argument that arms the early read-only census. PE_parse_boot_argn reads it
 * as a NUMBER, so the documented spelling is `dby-gpu=1`; `dby-gpu=on` parses as
 * nothing and is therefore treated as absent. Absent or 0 means NO MMIO at all:
 * not one window is mapped. */
#define DBY_GPU_BOOT_ARG "dby-gpu"

/* Published in DbyGpuCensusResult.magic once a pass completed. Not a hardware
 * value: it only lets a reader tell "ran and read zeros" apart from "never ran".
 * 'G','P','U','1'. */
#define DBY_GPU_CENSUS_MAGIC 0x47505531u

/* ---------------------------------------------------------------- identity */

/* Adreno 650 on kona. DTB:/soc/qcom,kgsl-3d0@3d00000:23927 -> qcom,chipid */
#define DBY_GPU_CHIP_ID 0x06050002u

/* Memory/UBWC facts the command stream and the client ABI need.
 * DTB:/soc/qcom,kgsl-3d0@3d00000:23930 highest-bank-bit
 * DTB:/soc/qcom,kgsl-3d0@3d00000:23931 min-access-length
 * DTB:/soc/qcom,kgsl-3d0@3d00000:23932 ubwc-mode
 * DTB:/soc/qcom,kgsl-3d0@3d00000:23933 snapshot-size */
#define DBY_GPU_HIGHEST_BANK_BIT 0x10u
#define DBY_GPU_MIN_ACCESS_LENGTH 0x20u
#define DBY_GPU_UBWC_VERSION 4u
#define DBY_GPU_SNAPSHOT_BYTES 0x200000u

/* GMEM (the GPU's on-die tile memory) size. The client ABI's
 * NativeAdrenoAbiDeviceInfo.gmem_bytes takes this value.
 *
 * LINUX:https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/msm/adreno/a6xx_catalog.c
 * The chip-table entry whose .chip_ids is 0x06050002 (revn 650, family
 * ADRENO_6XX_GEN3) says `.gmem = SZ_1M + SZ_128K`, i.e. 0x100000 + 0x20000.
 * It is a chip-table constant selected by the chip id DTB:23927 already gives
 * us, not a register read, so it needs no measurement on this unit. */
#define DBY_GPU_GMEM_BYTES 0x00120000u

/* GMEM window base in GPU address space (UCHE_GMEM_RANGE_MIN on A6xx). Still
 * NOT sourced, and the reason is not laziness:
 *   LINUX a6xx_gpu.c guards the whole
 *     REG_A6XX_UCHE_GMEM_RANGE_MIN / _MAX write with
 *     `if (!(adreno_is_a650_family(...) || adreno_is_a702(...) || a730))`
 *   so for this chip upstream programs NO GMEM range at all. There is
 *   therefore no upstream value to copy, and the UCHE register offsets are not
 *   pinned in this tree either.
 * REQUIRED_GMEM_BASE: either (a) the pinned a6xx.xml offsets plus a read-back
 * of both UCHE_GMEM_RANGE registers on this unit, or (b) a decision that the
 * client ABI field stays zero because upstream leaves it unprogrammed. Do not
 * substitute the 0x100000 that other A6xx parts use. */
#define DBY_GPU_GMEM_BASE DBY_GPU_TODO_MEASURE

/* ---------------------------------------------------------------- windows */
/* The five kgsl-3d0 windows, in reg order.
 * DTB:/soc/qcom,kgsl-3d0@3d00000:23922 reg, :23923 reg-names
 *
 * reg = <0x3d00000 0x40000 0x3d61000 0x800 0x3de0000 0x10000
 *        0x3d8b000 0x2000 0x6900000 0x80000>
 * reg-names = "kgsl_3d0_reg_memory" "cx_dbgc" "rscc" "isense_cntl" "qdss_gfx"
 *
 * The M0 census reads NONE of these five windows: it reads GPUCC. All three
 * census rows that target the 3d0 window are still TODO(measure), and reading a
 * window whose GX domain is collapsed is the same class of risk as writing it.
 * cx_dbgc and isense_cntl are needed before any clock vote, rscc before GMU
 * start; this comment used to claim the 3d0 window was "the only one an M0
 * census reads", which was wrong in both halves. */
#define DBY_GPU_WIN_3D0_BASE 0x03D00000u
#define DBY_GPU_WIN_3D0_BYTES 0x00040000u
#define DBY_GPU_WIN_CX_DBGC_BASE 0x03D61000u
#define DBY_GPU_WIN_CX_DBGC_BYTES 0x00000800u
#define DBY_GPU_WIN_RSCC_BASE 0x03DE0000u
#define DBY_GPU_WIN_RSCC_BYTES 0x00010000u
#define DBY_GPU_WIN_ISENSE_BASE 0x03D8B000u
#define DBY_GPU_WIN_ISENSE_BYTES 0x00002000u
#define DBY_GPU_WIN_QDSS_GFX_BASE 0x06900000u
#define DBY_GPU_WIN_QDSS_GFX_BYTES 0x00080000u

/* On-chip trace sink for the GPU. DTB:/soc/qcom,kgsl-3d0@3d00000:23934
 * qcom,gpu-qdss-stm = <0x161c0000 0x40000>. Not mapped by this driver. */
#define DBY_GPU_QDSS_STM_BASE 0x161C0000u
#define DBY_GPU_QDSS_STM_BYTES 0x00040000u

/* GMU windows. DTB:/soc/qcom,gmu@3d6a000:24424 reg, :24425 reg-names
 * reg = <0x3d6a000 0x30000 0xb290000 0x10000 0xb490000 0x10000>
 * reg-names = "kgsl_gmu_reg" "kgsl_gmu_pdc_cfg" "kgsl_gmu_pdc_seq" */
#define DBY_GPU_WIN_GMU_BASE 0x03D6A000u
#define DBY_GPU_WIN_GMU_BYTES 0x00030000u
#define DBY_GPU_WIN_GMU_PDC_CFG_BASE 0x0B290000u
#define DBY_GPU_WIN_GMU_PDC_CFG_BYTES 0x00010000u
#define DBY_GPU_WIN_GMU_PDC_SEQ_BASE 0x0B490000u
#define DBY_GPU_WIN_GMU_PDC_SEQ_BYTES 0x00010000u

/* GPU clock controller. DTB:/soc/qcom,gpucc@3d90000:3593
 * reg = <0x3d90000 0x9000>, compatible "qcom,gpucc-kona","syscon".
 * Linux's own regmap for this block allows reads up to 0x8008, so the DTB's
 * 0x9000 window is large enough. */
#define DBY_GPU_WIN_GPUCC_BASE 0x03D90000u
#define DBY_GPU_WIN_GPUCC_BYTES 0x00009000u

/* GPU SMMU (a separate instance from apps-smmu@15000000, see README).
 * DTB:/soc/kgsl-smmu@3da0000:10105
 * reg = <0x3da0000 0x10000 0x3dc2000 0x20>, reg-names "base" "tcu-base" */
#define DBY_GPU_WIN_SMMU_BASE 0x03DA0000u
#define DBY_GPU_WIN_SMMU_BYTES 0x00010000u
#define DBY_GPU_WIN_SMMU_TCU_BASE 0x03DC2000u
#define DBY_GPU_WIN_SMMU_TCU_BYTES 0x00000020u

/* ------------------------------------------------------------ interrupts */
/* All three are GIC SPIs, level-high, PPI-free.
 * GPU:  DTB:/soc/qcom,kgsl-3d0@3d00000:23924  interrupts = <0x00 0x12c 0x04>
 * HFI:  DTB:/soc/qcom,gmu@3d6a000:24426       interrupts = <0x00 0x130 0x04 ...>
 * GMU:  same line, second triple              <0x00 0x131 0x04> */
#define DBY_GPU_IRQ_GPU 300u
#define DBY_GPU_IRQ_HFI 304u
#define DBY_GPU_IRQ_GMU 305u

/* GPU SMMU interrupts. DTB:/soc/kgsl-smmu@3da0000:10119 declares TEN level-high
 * SPI triples -- 672, 673, 678, 679, 680, 681, 682, 683, 684, 685; the stretch
 * 674..677 is NOT present, so the range is not contiguous and a "672 + n" loop
 * would address four interrupts that do not exist. DTB:10111 sets
 * #global-interrupts = <0x02>, so the first pair is global and the remaining
 * eight are one combined interrupt per context bank. That bounds a context-bank
 * index to 0..7. It is an inference from two DTB properties, not a measurement
 * of which bank each Adreno stream is routed to, so DBY_GPU_SMMU_BANK stays
 * TODO(measure). Nothing here is programmed before M3. */
#define DBY_GPU_SMMU_IRQ_FIRST 672u
/* Number of per-context-bank interrupt triples the DTB lists: 10 - 2 global. */
#define DBY_GPU_SMMU_IRQ_CB_COUNT 8u

/* ------------------------------------------------------------- SMMU IDs */
/* Stream IDs (the SID is the first #iommu-cells cell).
 *   gfx3d_user   DTB:/soc/qcom,kgsl-iommu@3da0000/gfx3d_user:24406   <&kgsl_smmu 0x00 0x401>
 *   gfx3d_secure DTB:/soc/qcom,kgsl-iommu@3da0000/gfx3d_secure:24415 <&kgsl_smmu 0x02 0x400>
 *   gmu_user     DTB:/soc/qcom,gmu@3d6a000/gmu_user:24443            <&kgsl_smmu 0x04 0x400>
 *   gmu_kernel   DTB:/soc/qcom,gmu@3d6a000/gmu_kernel:24450          <&kgsl_smmu 0x05 0x400>
 *
 * The third cell is the SMR mask and is preserved here because it is part of
 * the binding, not a guess: 0x401 for the user CB, 0x400 for the other three.
 *
 * The GPU SMMU is NOT the apps SMMU. DTB:/soc/kgsl-smmu@3da0000:10122 is
 * phandle 0x17e; DTB:/soc/apps-smmu@15000000:10159 is phandle 0x48; every
 * Adreno stream above points at 0x17e. UFS DMA does not use this instance. */
#define DBY_GPU_SID_GFX3D_USER 0x00u
#define DBY_GPU_SID_GFX3D_SECURE 0x02u
#define DBY_GPU_SID_GMU_USER 0x04u
#define DBY_GPU_SID_GMU_KERNEL 0x05u
#define DBY_GPU_SMR_GFX3D_USER 0x401u
#define DBY_GPU_SMR_DEFAULT 0x400u

/* TBU stream ranges. DTB:/soc/kgsl-smmu@3da0000/gfx_0_tbu@3dc5000:10128
 * qcom,stream-id-range = <0x00 0x400>; gfx_1_tbu@3dc9000:10136 = <0x400 0x400>.
 * Together they explain why the user CB carries mask 0x401: bit 10 selects
 * the second TBU. */
#define DBY_GPU_TBU0_SID_RANGE 0x000u
#define DBY_GPU_TBU0_SID_COUNT 0x400u
#define DBY_GPU_TBU1_SID_RANGE 0x400u
#define DBY_GPU_TBU1_SID_COUNT 0x400u

/* Context-bank number each stream is routed to. The DTB does not state it:
 * it states qcom,gpu-offset = <0xa8000> (DTB:.../gfx3d_user:24408), which is
 * an offset inside the SMMU window, not a bank index. Adreno740's bring-up
 * takes the bank as a caller input for exactly this reason. */
#define DBY_GPU_SMMU_BANK DBY_GPU_TODO_MEASURE
#define DBY_GPU_SMMU_ASID DBY_GPU_TODO_MEASURE
/* DTB:/soc/qcom,kgsl-iommu@3da0000:24395 qcom,protect = <0xa0000 0xc000> */
#define DBY_GPU_SMMU_PROTECT_OFFSET 0x000A0000u
#define DBY_GPU_SMMU_PROTECT_BYTES 0x0000C000u
/* DTB:/soc/qcom,kgsl-iommu@3da0000:24398 qcom,secure_align_mask = <0xfff> */
#define DBY_GPU_SMMU_SECURE_ALIGN_MASK 0xFFFu
/* DTB:/soc/qcom,kgsl-iommu@3da0000/gfx3d_user:24408 */
#define DBY_GPU_SMMU_GPU_OFFSET 0x000A8000u

/* --------------------------------------------------------------- GPUCC */
/* LINUX:https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/clk/qcom/gpucc-sm8250.c
 *
 * The two GDSC indentifiers below are cross-checked against our own DTB:
 *   gpu_cx_gdsc DTB:/soc/qcom,gdsc@3d9106c:3847  == 0x3d90000 + 0x106c
 *   gpu_gx_gdsc DTB:/soc/qcom,gdsc@3d9100c:3874  == 0x3d90000 + 0x100c
 * Two independent sources agree, so these are measured, not inferred. */
#define DBY_GPUCC_GDSC_CX_GDSCR 0x106Cu    /* LINUX gpucc-sm8250.c .gdscr */
#define DBY_GPUCC_GDSC_CX_HW_CTRL 0x1540u  /* LINUX .gds_hw_ctrl; DTB syscon@3d91540:3841 */
#define DBY_GPUCC_GDSC_GX_GDSCR 0x100Cu    /* LINUX .gdscr */
#define DBY_GPUCC_GDSC_GX_CLAMP_IO 0x1508u /* LINUX .clamp_io_ctrl; DTB syscon@3d91508:3861 */

/* Block resets (qcom_reset_map). Each is a BCR register: write bit 0. */
#define DBY_GPUCC_BCR_XO 0x1000u       /* GPU_CC_XO_BCR, LINUX resets[] */
#define DBY_GPUCC_BCR_GX 0x1008u       /* GPU_CC_GX_BCR */
#define DBY_GPUCC_BCR_CX 0x1068u       /* GPU_CC_CX_BCR */
#define DBY_GPUCC_BCR_GFX3D_AON 0x10A0u /* GPU_CC_GFX3D_AON_BCR */
#define DBY_GPUCC_BCR_GMU 0x111Cu      /* GPU_CC_GMU_BCR */
#define DBY_GPUCC_BCR_ACD 0x1160u      /* GPU_CC_ACD_BCR */

/* Branch clock enable registers. bit 0 is the enable in every case. */
#define DBY_GPUCC_CXO_AON_CBCR 0x1004u       /* gpu_cc_cxo_aon_clk */
#define DBY_GPUCC_GX_GMU_CBCR 0x1064u        /* gpu_cc_gx_gmu_clk */
#define DBY_GPUCC_AHB_CBCR 0x1078u           /* gpu_cc_ahb_clk */
#define DBY_GPUCC_CRC_AHB_CBCR 0x107Cu       /* gpu_cc_crc_ahb_clk */
#define DBY_GPUCC_CX_APB_CBCR 0x1088u        /* gpu_cc_cx_apb_clk */
#define DBY_GPUCC_CX_SNOC_DVM_CBCR 0x108Cu   /* gpu_cc_cx_snoc_dvm_clk */
#define DBY_GPUCC_CX_GMU_CBCR 0x1098u        /* gpu_cc_cx_gmu_clk */
#define DBY_GPUCC_CXO_CBCR 0x109Cu           /* gpu_cc_cxo_clk */
#define DBY_GPUCC_HLOS1_VOTE_SMMU_CBCR 0x5000u /* gpu_cc_hlos1_vote_gpu_smmu_clk */

/* CX_GMU_CBCR wake/sleep field. LINUX gpucc-sm8250.c sets wake=0xf (bits 8..11)
 * and sleep=0xf (bits 4..7) in probe(); reproducing that update is a WRITE and
 * therefore belongs to a gated stage, not to the census. */
#define DBY_GPUCC_CX_GMU_SLEEP_MASK 0xFu
#define DBY_GPUCC_CX_GMU_SLEEP_SHIFT 4u
#define DBY_GPUCC_CX_GMU_WAKE_MASK 0xFu
#define DBY_GPUCC_CX_GMU_WAKE_SHIFT 8u

/* GMU clock root. LINUX gpucc-sm8250.c gpu_cc_gmu_clk_src .cmd_rcgr = 0x1120,
 * freq table 19.2 MHz (TCXO) / 200 MHz (GPLL0 div) / 500 MHz (PLL1).
 * OPP levels from our own DTB:/soc/gpu-opp-table:23902..23915:
 *   480 MHz  opp-microvolt 0xc0
 *   381 MHz  opp-microvolt 0x80
 *   290 MHz  opp-microvolt 0x40
 * The microvolt values are RPMh corner indices in the vendor binding, not mV;
 * do not convert them. */
#define DBY_GPUCC_GMU_CLK_SRC_CMD_RCGR 0x1120u
#define DBY_GPU_OPP_MAX_HZ 480000000u
#define DBY_GPU_OPP_MID_HZ 381000000u
#define DBY_GPU_OPP_MIN_HZ 290000000u

/* GDSC status and collapse layout, taken from upstream instead of assumed.
 *
 * The previous revision of this block was WRONG, and it is worth stating how so
 * the mistake is not repeated: it said both GPU GDSCs are POLL_CFG_GDSCR-shaped
 * and that "the status really does live inside GDSCR", then defined PWR_ON as
 * bit 31 of GDSCR. Upstream's status function does not read GDSCR for either of
 * these two domains.
 *
 * LINUX:https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/clk/qcom/gdsc.c
 * gdsc_check_status() (gdsc.c:61-95) picks the register to read FIRST:
 *
 *     if (sc->flags & POLL_CFG_GDSCR)  reg = sc->gdscr + CFG_GDSCR_OFFSET (0x4);
 *     else if (sc->gds_hw_ctrl)        reg = sc->gds_hw_ctrl;
 *     else                             reg = sc->gdscr;
 *
 * and then picks the bits by domain type:
 *
 *     POLL_CFG_GDSCR domain:  ON  = val & GDSC_POWER_UP_COMPLETE   (BIT(16))
 *                             OFF = val & GDSC_POWER_DOWN_COMPLETE (BIT(15))
 *     any other domain:       ON  = val & PWR_ON_MASK              (BIT(31))
 *                             OFF = !(val & PWR_ON_MASK)
 *
 * Applying that to our two domains (flags from LINUX gpucc-sm8250.c:241-249):
 *
 *   gpu_gx_gdsc  flags = CLAMP_IO | AON_RESET | POLL_CFG_GDSCR, no gds_hw_ctrl
 *                -> status register = GDSCR + 0x4 = GPUCC + 0x1010
 *                   ON bit 16, OFF bit 15
 *   gpu_cx_gdsc  flags = VOTABLE, gds_hw_ctrl = 0x1540
 *                -> status register = 0x1540 (NOT GDSCR)
 *                   ON bit 31
 *
 * The collapse REQUEST bit is the one genuinely in GDSCR for both:
 * SW_COLLAPSE_MASK = BIT(0) of `gdscr`, written through
 * gdsc_update_collapse_bit() (gdsc.c:127-141); writing 0 asks for power on.
 *
 * Every one of these is read-only INTERPRETATION. The census logs the raw word
 * of each register and the decode line names the register it belongs to, so a
 * decode that disagrees with this unit is visible next to the value. */
#define DBY_GPUCC_GDSCR_SW_COLLAPSE_BIT 0u   /* LINUX gdsc.c:27 SW_COLLAPSE_MASK */
#define DBY_GPUCC_CFG_GDSCR_OFFSET 0x4u      /* LINUX gdsc.c:35 CFG_GDSCR_OFFSET */
/* PWR_ON lives in gds_hw_ctrl (0x1540), NOT in a GDSCR -- that is the whole
 * point of the register selection at LINUX:gdsc.c:67-72. The name says so,
 * because a GDSCR-prefixed name is exactly how the reviewed revision's bug would
 * come back: the value is unchanged at 31, only the prefix moved to the register
 * it is actually read from. Rename requested by the second audit (nit 4). */
#define DBY_GPUCC_GDS_HW_CTRL_PWR_ON_BIT 31u /* LINUX gdsc.c:21 PWR_ON_MASK */
#define DBY_GPUCC_CFG_GDSC_POWER_UP_COMPLETE_BIT 16u   /* LINUX gdsc.c:32 */
#define DBY_GPUCC_CFG_GDSC_POWER_DOWN_COMPLETE_BIT 15u /* LINUX gdsc.c:33 */
/* GMEM_CLAMP_IO_MASK = BIT(0), and it belongs to clamp_io_ctrl, not to GDSCR.
 * LINUX gdsc.c:28 defines the mask; the register is gpu_gx_gdsc.clamp_io_ctrl
 * (LINUX gpucc-sm8250.c:242) = GPUCC + 0x1508. */
#define DBY_GPUCC_CLAMP_IO_BIT 0u            /* LINUX gdsc.c:28 GMEM_CLAMP_IO_MASK */

/* Derived status-register addresses, stated as sums so they cannot drift from
 * the two sources above. Neither is a new hardware number: the base is the
 * GDSCR offset already sourced in the GPUCC table and the addend is
 * CFG_GDSCR_OFFSET. */
#define DBY_GPUCC_GDSC_GX_STATUS_REG \
	(DBY_GPUCC_GDSC_GX_GDSCR + DBY_GPUCC_CFG_GDSCR_OFFSET)

/* Reads per census point, and the settle time between re-reads.
 * DBY_GPU_CENSUS_READS is the stability sample count the M0 acceptance test
 * uses. DBY_GPU_CENSUS_SETTLE_US is NOT used by the read-only census on
 * purpose: back-to-back re-reads are what expose a clock that is being gated
 * underneath us, and inserting a delay there would mask exactly that. It is
 * reserved for the post-write settle of the M1/M2 stages, which have no code
 * path in this delivery. A constant that claims a behaviour the code does not
 * have is worse than no constant. */
#define DBY_GPU_CENSUS_READS 8u
#define DBY_GPU_CENSUS_SETTLE_US 50u

/* Memory granularity. This XNU profile is a 4 KiB build
 * (docs/xnu/four-k-page-tables.md: ARM_PGSHIFT 12 on BCM2837), and the SMMU
 * granule on this board is 4 KiB too, so one number serves both. */
#define DBY_GPU_PAGE_BYTES 0x1000u
#define DBY_GPU_CACHE_LINE_BYTES 64u

/* How the driver reports. One prefix keeps the rows greppable in a serial or
 * BootProgress log, the way sheng greps [gpu ...]. */
#define DBY_GPU_LOG_PREFIX "DbyGpu"

#endif /* DBY_GPU_REGS_H */
