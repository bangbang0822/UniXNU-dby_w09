/* DbyWifiProbeBoard.h — DBY-W09 (Huawei MatePad 11, SM8250 / kona) wireless probe facts.
 *
 * WHY THIS HEADER EXISTS
 *   package/DbyWifiProbe.cpp is the IOKit half: it parses a boot argument, maps windows and reads
 *   words.  Every ADDRESS and every DECODE RULE lives here instead, in plain C with no IOKit and no
 *   libc dependency, so that scripts/build-wifi-probe.py can compile package/DbyWifiProbeBoard.c on
 *   a development host, run the decision table, and print the six rows without a device.  That is
 *   the same split the previous Wi-Fi round used (work/tracks/wifi/repo/package/DbyWifi/DbyWifiCensus.c
 *   is IOKit-free "on purpose", its header says so, and scripts/check-dby-wifi.py builds it).
 *
 * WHAT INCREMENT THIS IS
 *   SPEC M0 only: decide which bus carries Bluetooth HCI on this board, from a read-only census.
 *   Nothing here touches PCIe, powers a rail, writes a register or downloads firmware.  M1..M10 are
 *   not implemented by this file and the header does not pretend otherwise.
 *
 * EVERY VALUE BELOW IS A VENDOR-TREE FACT WITH file:line.  The vendor tree is
 *   $V = _dby/robyy10__DBY-W09.tar/DBY-W09-main
 * and the comments use the REPORT/SPEC shorthand: $VD = $V/arch/arm64/boot/dts/vendor/qcom.
 *
 * THE THREE THINGS THIS PROBE MEASURES, AND WHY EACH ONE IS SAFE TO READ
 *   1. TLMM (pin mux / pull / drive / level).  The block is in the always-on domain: the vendor
 *      pinctrl driver's probe does devm_ioremap_resource and nothing else - no clk_get, no
 *      regulator_get, no power domain ($V/drivers/pinctrl/qcom/pinctrl-msm.c:1588-1589 in a probe
 *      whose body never asks for a clock).  Reading GPIO_CFG/GPIO_IN_OUT therefore cannot hang on a
 *      vote this probe forgot to cast, and the board already exercises the same block every boot
 *      through package/BootProgress.c's touch path.
 *   2. GCC branch state.  CBCR reads are what clk_branch_is_enabled() does with the branch OFF
 *      ($V/drivers/clk/qcom/clk-branch.c:47, :52-67), i.e. the vendor expects them to work in any
 *      power state.  The GPU probe on this very board reads two GCC pages read-only in its mode 1
 *      and has returned real values (package/DbyGpuPowerProbe.cpp:13-17, :36-40), so "GCC is
 *      readable before anything is powered" is measured on this device, not assumed.
 *   3. GENI serial-engine registers.  **NOT safe read-only**, which is why they are behind mode 2
 *      and behind a clock gate: 0x980000 is a QUPv3 serial engine whose clocks are voted by
 *      Linux's geni driver, and reading an unclocked Qualcomm register block hangs the NoC.  That
 *      is exactly the failure that froze this board on 2026-11-03 at 0x03da0000 (the GPU SMMU read,
 *      work/tracks/gpu-smmu-read-hang/REPORT.md, quoted in package/DbyGpuPowerProbe.cpp:3-10).
 *      Mode 2 reads the SE only when BOTH gcc_qupv3_wrap0_core_clk and that SE's own branch read
 *      enabled-and-not-halted, which is the same prerequisite gate the GPU probe applies to the
 *      SMMU.  Mode 1 (the default, and the only mode this increment asks anyone to run) never maps
 *      the SE at all.
 *
 * WHAT THIS PROBE CANNOT DO
 *   It cannot prove SLIMbus, and it must never print that verdict.  Nothing in a static register
 *   read distinguishes "the chip's HCI is on SLIMbus and nothing has powered the bus yet" from
 *   "the chip's HCI is on a UART and no client has opened it yet".  DBY_BT_M0_VERDICT_SLIMBUS is
 *   reserved for the powered measurement described in work/tracks/wifi-bt/M0-ANSWER.md and is not
 *   produced by dby_bt_m0_verdict().  Printing it from a cold read would be the SPEC 3.1 sin of
 *   adjusting a criterion until it passes.
 */

#ifndef DBY_WIFI_PROBE_BOARD_H
#define DBY_WIFI_PROBE_BOARD_H

/* The header is includable from the IOKit translation unit (C++) and from the host self-test (C),
 * so the only thing it asks of either is <stdint.h>.  Both files that use it include that first
 * (the .cpp because DbyGpuPowerProbe.cpp:139 already does, the .c because a host C99 compiler has it),
 * and check-parse.py substitutes the four typedefs when it feeds pycparser, which has no libc. */
#include <stdint.h>

/* Everything below is shared with a C translation unit, so the C++ side must see C linkage.  Without
 * this block the .cpp would reference `_Z20dby_bt_m0_evaluatePK...` while DbyWifiProbeBoard.c defines
 * `dby_bt_m0_evaluate`: the archive would link and every row would silently stay blank.  The recipe's
 * EXPORTS check exists for the same failure one step later. */
#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Rows                                                               */
/* ------------------------------------------------------------------ */
/* Six rows of 120 bytes: the same shape and the same renderer contract as package/BootLog.c's
 * native_gpupower_rows[q] loop (BootLog.c:667-671) and package/DbyGpuPowerProbe.cpp:157.  The
 * consumer side needs seven lines in BootLog.c; work/tracks/wifi-bt/IMPLEMENTATION.md carries them
 * prepared and NOT applied, because package/BootLog.c is not this track's file to edit. */
#define DBY_BT_M0_ROWS        6
#define DBY_BT_M0_ROW_BYTES 120

/* ------------------------------------------------------------------ */
/* ADDRESSES                                                          */
/* ------------------------------------------------------------------ */

/* TLMM.  The kona pin controller is a 16 MiB window, NOT the 3 MiB one the board's dts declares:
 *   $VD/kona-pinctrl.dtsi:2-4   tlmm: pinctrl@f000000 { reg = <0x0F000000 0x1000000>; }
 * The board's own dts/dby_w09.dts:65-72 maps "pinctrl@f100000" with <0x0f100000 0x300000>, which is
 * the WEST tile only (0x100000 into the block, see DBY_BT_M0_TLMM_WEST below).  Touch works through
 * that mapping because the panel's pins are WEST-tile pins; the BT/Debug pins this probe needs are
 * NORTH-tile pins at 0xF900000 and are NOT reachable through the board's existing mapping.  This
 * probe therefore maps its own windows and does not depend on dts/, which it may not edit. */
#define DBY_BT_M0_TLMM_BASE      0x0F000000u   /* $VD/kona-pinctrl.dtsi:4 */
#define DBY_BT_M0_TLMM_BYTES     0x01000000u   /* $VD/kona-pinctrl.dtsi:4 */

/* Tile bases inside that window, and the per-pin stride:
 *   $V/drivers/pinctrl/qcom/pinctrl-kona.c:20-25  NORTH 0x00900000, SOUTH 0x00500000,
 *                                                 WEST 0x00100000, REG_SIZE 0x1000
 *   :44-47  ctl_reg = base + REG_SIZE * id,  io_reg = base + 0x4 + REG_SIZE * id
 * Kona has no EAST tile pins in pinctrl-kona.c (the macro is empty at :23), so only NORTH and WEST
 * are used here.  Getting this wrong is the documented board hazard: AGENTS.md's
 * "0x0F100000 + pin * 0x1000 (the wrong pin tile)" cost a flash cycle. */
#define DBY_BT_M0_TLMM_NORTH     0x00900000u
#define DBY_BT_M0_TLMM_WEST      0x00100000u
#define DBY_BT_M0_TLMM_STRIDE    0x00001000u
#define DBY_BT_M0_TLMM_IO_OFF    0x4u

/* GCC.  reg = <0x100000 0x1f0000> ($VD/kona.dtsi:3054-3056), so the vote register at offset
 * 0x52008 lands at 0x152008 and the QUPv3 branch halt registers at 0x2700xx / 0x27702c - all inside
 * the declared aperture.  One mapping of the whole declared window with a bound check is fewer
 * mappings than five pages and uses the vendor's own size. */
#define DBY_BT_M0_GCC_BASE       0x00100000u   /* $VD/kona.dtsi:3056 */
#define DBY_BT_M0_GCC_BYTES      0x001F0000u   /* $VD/kona.dtsi:3056 */

/* QUPv3_0 serial engines, 0x4000 each ("se_phys"):
 *   SE0  $VD/kona-qupv3.dtsi:111-113  qcom,msm-geni-serial-hs @ 0x980000, status "ok" at :124
 *   SE6  $VD/kona-qupv3.dtsi:85-87    qcom,msm-geni-serial-hs @ 0x998000, status "disabled" at :102,
 *        enabled for the board by $VD/kona-qrd.dtsi:30-36 (the board's base is kona-qrd, see
 *        $VD/kona/common/common.dtsi:6-8).
 * Both carry qcom,wakeup-byte = <0xFD> (:103 and :125).  That property is NOT a Bluetooth marker:
 * five kona HS-UART nodes carry it (SE0 :125, SE6 :103, SE13 :528, SE17 :816, SE18 :837). */
#define DBY_BT_M0_SE0_BASE       0x00980000u   /* $VD/kona-qupv3.dtsi:113 */
#define DBY_BT_M0_SE6_BASE       0x00998000u   /* $VD/kona-qupv3.dtsi:87  */
#define DBY_BT_M0_SE_BYTES       0x00004000u   /* $VD/kona-qupv3.dtsi:113 */

/* GENI serial-engine register offsets.  $V/include/linux/qcom-geni-se.h, "Common SE registers":
 *   :85  GENI_INIT_CFG_REVISION   0x00
 *   :90  SE_GENI_STATUS           0x40   (fields :151-152  M_GENI_CMD_ACTIVE BIT(0),
 *                                                          S_GENI_CMD_ACTIVE BIT(12))
 *   :91  GENI_SER_M_CLK_CFG       0x48   (fields :155-157  SER_CLK_EN BIT(0), CLK_DIV GENMASK(15,4))
 *   :92  GENI_SER_S_CLK_CFG       0x4c
 *   :93  GENI_CLK_CTRL_RO         0x60
 *   :94  GENI_IF_FIFO_DISABLE_RO  0x64   (field :162 FIFO_IF_DISABLE BIT(0))
 *   :95  GENI_FW_REVISION_RO      0x68   (fields :165-167 protocol GENMASK(15,8), version GENMASK(7,0))
 *   :96  GENI_FW_S_REVISION_RO    0x6c
 *   :97  SE_GENI_CLK_SEL          0x7c   (field :170 CLK_SEL_MSK GENMASK(2,0))
 *   :117 SE_GENI_RX_FIFO_STATUS   0x804
 *   :121 SE_GENI_IOS              0x908
 * Every one of them is named _RO by the vendor, i.e. a read changes no state.  That is why the
 * probe reads these and nothing else in the SE (no FIFO pop at :114-115, no IRQ status clear). */
#define DBY_BT_M0_GENI_INIT_CFG_REVISION 0x000u
#define DBY_BT_M0_GENI_STATUS            0x040u
#define DBY_BT_M0_GENI_SER_M_CLK_CFG     0x048u
#define DBY_BT_M0_GENI_SER_S_CLK_CFG     0x04cu
#define DBY_BT_M0_GENI_CLK_CTRL_RO       0x060u
#define DBY_BT_M0_GENI_IF_FIFO_DISABLE   0x064u
#define DBY_BT_M0_GENI_FW_REVISION_RO    0x068u
#define DBY_BT_M0_GENI_FW_S_REVISION_RO  0x06cu
#define DBY_BT_M0_GENI_CLK_SEL           0x07cu
#define DBY_BT_M0_GENI_RX_FIFO_STATUS    0x804u
#define DBY_BT_M0_GENI_IOS               0x908u
#define DBY_BT_M0_GENI_WORDS                 8u   /* how many of the above are stored per SE */

/* GCC branch state.  One vote register, one halt (CBCR) register per branch; the pairs are:
 *   core   gcc_qupv3_wrap0_core_clk   $V/drivers/clk/qcom/gcc-kona.c:2694-2705  vote 0x52008 bit 8,
 *                                                                                halt 0x23000
 *   m_ahb  gcc_qupv3_wrap_0_m_ahb_clk :3119-3130                                vote 0x52008 bit 6,
 *                                                                                halt 0x17004
 *   s_ahb  gcc_qupv3_wrap_0_s_ahb_clk :3132-3145                                vote 0x52008 bit 7,
 *                                                                                halt 0x17008
 *   se0    gcc_qupv3_wrap0_s0_clk     :2707-2723                                vote 0x52008 bit 10,
 *                                                                                halt 0x1700c
 *   se6    gcc_qupv3_wrap0_s6_clk     :2815-2831                                vote 0x52008 bit 16,
 *                                                                                halt 0x1772c
 * BRANCH_CLK_OFF is BIT(31) of the halt register ($V/drivers/clk/qcom/clk-branch.c:47, tested at
 * :64-67).  Note the polarity trap: for a BRANCH_HALT branch bit 31 SET means halted, so the
 * "running" test is enabled-vote AND NOT bit31. */
#define DBY_BT_M0_GCC_VOTE_REG       0x052008u
#define DBY_BT_M0_GCC_CORE_HALT      0x023000u
#define DBY_BT_M0_GCC_M_AHB_HALT     0x017004u
#define DBY_BT_M0_GCC_S_AHB_HALT     0x017008u
#define DBY_BT_M0_GCC_SE0_HALT       0x01700cu
#define DBY_BT_M0_GCC_SE6_HALT       0x01772cu
#define DBY_BT_M0_GCC_VOTE_CORE      (1u << 8)
#define DBY_BT_M0_GCC_VOTE_M_AHB     (1u << 6)
#define DBY_BT_M0_GCC_VOTE_S_AHB     (1u << 7)
#define DBY_BT_M0_GCC_VOTE_SE0       (1u << 10)
#define DBY_BT_M0_GCC_VOTE_SE6       (1u << 16)
#define DBY_BT_M0_CBCR_CLK_OFF       (1u << 31)

/* One bit per word in struct dby_bt_m0_inputs.gcc_valid, so "read" and "0x00000000" stay apart. */
#define DBY_BT_M0_GCC_VALID_VOTE     0x01u
#define DBY_BT_M0_GCC_VALID_CORE     0x02u
#define DBY_BT_M0_GCC_VALID_M_AHB    0x04u
#define DBY_BT_M0_GCC_VALID_S_AHB    0x08u
#define DBY_BT_M0_GCC_VALID_SE0      0x10u
#define DBY_BT_M0_GCC_VALID_SE6      0x20u
#define DBY_BT_M0_GCC_VALID_ALL      0x3Fu

/* ------------------------------------------------------------------ */
/* PINS                                                               */
/* ------------------------------------------------------------------ */
/* Every pin this probe reads, with the vendor line that gives it.  The tile column is what
 * pinctrl-kona.c's PINGROUP() call for that id passes as `base`; that is the whole reason a pin
 * cannot be addressed as 0xF100000 + id * 0x1000.
 *
 *   id   signal                 vendor line
 *   16   SE6 CTS                $VD/kona-pinctrl.dtsi:137  (qupv3_se6_default_cts: pins = "gpio16")
 *   17   SE6 RTS                $VD/kona-pinctrl.dtsi:151  (qupv3_se6_default_rtsrx: gpio17, gpio19)
 *   18   SE6 TX                 $VD/kona-pinctrl.dtsi:165
 *   19   SE6 RX                 $VD/kona-pinctrl.dtsi:151  (same group; and this is the pin
 *                                $VD/kona-qupv3.dtsi:101 uses as SE6's wake interrupt)
 *   20   WLAN_EN                $VD/kona.dtsi:5318          (wlan-en-gpio = <&tlmm 20 0>)
 *   21   BT_EN                  $VD/kona.dtsi:5319 / $VD/kona-qrd.dtsi:1143
 *   28   SE0 CTS                $VD/kona-pinctrl.dtsi:220
 *   29   SE0 RTS                $VD/kona-pinctrl.dtsi:234  (qupv3_se0_default_rtsrx: gpio29, gpio31)
 *   30   SE0 TX                 $VD/kona-pinctrl.dtsi:248
 *   31   SE0 RX                 $VD/kona-pinctrl.dtsi:234; also SE0's wake interrupt
 *                                ($VD/kona-qupv3.dtsi:123, <&tlmm 31 0>)
 *   124  SW_CTRL                $VD/kona.dtsi:5320 / $VD/kona-qrd.dtsi:1144
 *   145  BT_INDEPENDENT         $VD/kona-qrd.dtsi:1145 - WEST tile, and its first alternate
 *                                function is lpass_slimbus ($V/drivers/pinctrl/qcom/pinctrl-kona.c:1708)
 *
 * Mux function index.  pinctrl-kona.c:26-43 builds each pingroup's funcs[] as
 * {msm_mux_gpio, msm_mux_f1, msm_mux_f2, ...} and pinctrl-msm.c:169-183 writes that array INDEX into
 * the FUNC_SEL field, so the number to compare against is the position in the pingroup's own list:
 *   gpio28-31  PINGROUP(28, NORTH, qup0, ...)  pinctrl-kona.c:1474-1479  -> qup0 is index 1
 *   gpio16-19  PINGROUP(16, NORTH, qup6, ...)  pinctrl-kona.c:1456-1459  -> qup6 is index 1
 *   gpio20-21  PINGROUP(20, NORTH, qup7, '')   pinctrl-kona.c:1461-1462
 *   gpio124    PINGROUP(124, NORTH, qup_l5, '') pinctrl-kona.c:1666
 *   gpio145    PINGROUP(145, WEST, lpass_slimbus, '') pinctrl-kona.c:1708
 * So on this SoC "FUNC_SEL == 1" means "this pin is driven by its QUP".  For gpio20/21/124 the
 * interesting value is 0 (plain GPIO), which is what a power-enable line must be. */
#define DBY_BT_M0_FUNC_GPIO  0u
#define DBY_BT_M0_FUNC_QUP   1u

/* Field decode: pinctrl-msm.c:560-563 is the vendor's own debugfs decoder for a ctl_reg
 *   is_out = !!(ctl & BIT(oe_bit=9));  func = (ctl >> mux_bit=2) & 7;
 *   drive  = (ctl >> drv_bit=6) & 7;   pull = (ctl >> pull_bit=0) & 3;
 * with the bit positions from pinctrl-kona.c:49-56.  The 3-bit func mask is the driver's, not an
 * invention: nfuncs is 10, so a 4th bit exists in hardware but the vendor never reads it back.
 * The level bits are pinctrl-kona.c:55-56 (in_bit 0, out_bit 1) applied to io_reg. */
#define DBY_BT_M0_CTL_FUNC_SHIFT  2u
#define DBY_BT_M0_CTL_FUNC_MASK   7u
#define DBY_BT_M0_CTL_DRIVE_SHIFT 6u
#define DBY_BT_M0_CTL_DRIVE_MASK  7u
#define DBY_BT_M0_CTL_PULL_MASK   3u
#define DBY_BT_M0_CTL_OE_BIT      9u
#define DBY_BT_M0_IO_IN_BIT       0u

enum dby_bt_m0_pin_id {
	DbyBtM0PinSe6Cts = 0,   /* gpio16,  NORTH */
	DbyBtM0PinSe6Rts,       /* gpio17,  NORTH */
	DbyBtM0PinSe6Tx,        /* gpio18,  NORTH */
	DbyBtM0PinSe6Rx,        /* gpio19,  NORTH */
	DbyBtM0PinWlanEn,       /* gpio20,  NORTH */
	DbyBtM0PinBtEn,         /* gpio21,  NORTH */
	DbyBtM0PinSe0Cts,       /* gpio28,  NORTH */
	DbyBtM0PinSe0Rts,       /* gpio29,  NORTH */
	DbyBtM0PinSe0Tx,        /* gpio30,  NORTH */
	DbyBtM0PinSe0Rx,        /* gpio31,  NORTH */
	DbyBtM0PinSwCtrl,       /* gpio124, NORTH */
	DbyBtM0PinBtIndep,      /* gpio145, WEST  */
	DbyBtM0PinCount
};

struct dby_bt_m0_pin_fact {
	uint16_t id;
	/* uint32_t and not uint16_t: the tile bases are 0x900000 (NORTH) and 0x100000 (WEST), both of
	 * which a 16-bit field would silently truncate to 0x0000 and 0x0000 - i.e. every pin would be read
	 * from the wrong address with no diagnostic at all. */
	uint32_t tile;
	const char *name;
};

/* The table itself, so the probe and the host self-test cannot disagree about which pin is which. */
extern const struct dby_bt_m0_pin_fact dby_bt_m0_pins[DbyBtM0PinCount];
/* ------------------------------------------------------------------ */
/* INPUTS / RESULT                                                    */
/* ------------------------------------------------------------------ */
struct dby_bt_m0_pin_value {
	uint32_t ctl;         /* GPIO_CFG at tile + 0x1000*id */
	uint32_t io;          /* GPIO_IN_OUT at tile + 0x1000*id + 4 */
	uint32_t valid;       /* 0 = not read (window unmapped, or a refused access) */
};

struct dby_bt_m0_inputs {
	uint32_t mode;                 /* 1 = TLMM+GCC, 2 = + GENI identity reads */
	uint32_t tlmm_mapped;
	uint32_t gcc_mapped;
	uint32_t geni_attempted;       /* mode 2 only */
	uint32_t geni_read_mask;       /* bit per engine: 1 = SE0, 2 = SE6 */

	struct dby_bt_m0_pin_value pin[DbyBtM0PinCount];

	uint32_t gcc_vote;             /* 0x152008 */
	uint32_t gcc_core_halt;        /* 0x123000 */
	uint32_t gcc_m_ahb_halt;       /* 0x270004 */
	uint32_t gcc_s_ahb_halt;       /* 0x270008 */
	uint32_t gcc_se0_halt;         /* 0x27000c */
	uint32_t gcc_se6_halt;         /* 0x27702c */
	uint32_t gcc_valid;            /* bit per word above: 0x3f = all six read */

	/* Mode 2: the RO words of each engine, in DBY_BT_M0_GENI_WORD_* order. */
	uint32_t geni_se0[DBY_BT_M0_GENI_WORDS];
	uint32_t geni_se6[DBY_BT_M0_GENI_WORDS];
};

enum dby_bt_m0_geni_word {
	DbyBtM0GeniInitCfg = 0,
	DbyBtM0GeniStatus,
	DbyBtM0GeniSerMClkCfg,
	DbyBtM0GeniSerSClkCfg,
	DbyBtM0GeniFwRevision,
	DbyBtM0GeniFwSRevision,
	DbyBtM0GeniIfFifoDisable,
	DbyBtM0GeniIos
};

enum dby_bt_m0_verdict {
	DbyBtM0VerdictUnknown = 0,   /* nothing in this boot's state separates the buses */
	DbyBtM0VerdictUartSe0,
	DbyBtM0VerdictUartSe6,
	DbyBtM0VerdictSlimbus   = 3 /* NEVER produced by dby_bt_m0_verdict(); see the file header */
};

struct dby_bt_m0_result {
	uint32_t se0_qup_pins;      /* how many of gpio28-31 sit at FUNC_SEL == 1 (qup0) */
	uint32_t se6_qup_pins;      /* same for gpio16-19 (qup6) */
	uint32_t se0_clocked;       /* vote bit set AND halt bit clear, and core+m_ahb+s_ahb too */
	uint32_t se6_clocked;
	uint32_t qup_domain_on;     /* wrapper core + both AHB votes running */
	uint32_t bt_en_level;       /* GPIO_IN_OUT bit 0 of gpio21 */
	uint32_t wlan_en_level;     /* gpio20  */
	uint32_t sw_ctrl_level;     /* gpio124 */
	uint32_t bt_indep_level;    /* gpio145 */
	uint32_t se0_rx_level;      /* gpio31 */
	uint32_t se0_tx_level;      /* gpio30 */
	uint32_t se6_rx_level;      /* gpio19 */
	uint32_t geni_answer;       /* 1 = an engine answered with non-0/0xffffffff identity words */
	enum dby_bt_m0_verdict verdict;
	const char *why;              /* a static string, never a formatted one */
};

/* ------------------------------------------------------------------ */
/* DECODE                                                             */
/* ------------------------------------------------------------------ */
struct dby_bt_m0_ctl {
	uint32_t func;
	uint32_t drive;
	uint32_t pull;
	uint32_t is_output;
};

struct dby_bt_m0_ctl dby_bt_m0_decode_ctl(uint32_t ctl);

/* 1 when this branch is genuinely running: its vote bit is set and its CBCR's CLK_OFF is clear.
 * `halt` is the word read from the halt register, `vote` the 0x152008 word, `mask` the bit. */
uint32_t dby_bt_m0_branch_running(uint32_t vote, uint32_t halt, uint32_t mask);

/* Fill the derived fields and the verdict.  Pure: no I/O, no globals, no formatting. */
void dby_bt_m0_evaluate(const struct dby_bt_m0_inputs *in, struct dby_bt_m0_result *out);

/* The verdict as the four words the SPEC's M0 row uses: UART-SE0 / UART-SE6 / SLIMBUS / UNKNOWN. */
const char *dby_bt_m0_verdict_name(enum dby_bt_m0_verdict verdict);

/* Render all six rows.  `rows` is DBY_BT_M0_ROWS * DBY_BT_M0_ROW_BYTES bytes.  Rows that this mode
 * did not measure are still rendered, with the word "not-read" - an absent measurement must not
 * look like a measured zero (the same rule package/BootLog.c follows by gating each block on its
 * own first element). */
void dby_bt_m0_render(const struct dby_bt_m0_inputs *in, const struct dby_bt_m0_result *out,
    char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DBY_WIFI_PROBE_BOARD_H */
