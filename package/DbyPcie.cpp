/* DbyPcie — the wireless bring-up's board-level probe: M1 census (read-only) and M2a power-up.
 *
 * SPEC  docs/wireless-spec-2026-11-20.md §4.2 (stage ladder), §4.5 (register sheet), §5 (acceptance).
 * PLAN  docs/wireless-bring-up-plan-2026-11-20.md §1 (why this chip is a QCA6390 on PCIe0).
 *
 * THE LADDER (kernel argument dby-pcie=N, one stage per flash)
 *   N=1  M1  census only, ZERO writes: GCC GDSCR and the six halt registers, the M2 "before" state
 *            (votes, TBU/PHY-AUX/DDRSS enables, three reset registers), the three TLMM tiles for
 *            GPIO20/GPIO79, and - only if GDSCR says the controller is powered - PARF, ELBI and (only
 *            if ELBI reports link-up) the DBI vendor/device ID.
 *   N=2  M2a power-up from GCC alone, no RPMh rail vote: vote the six PCIe0 branches and the four
 *            single-bit enables, clear SW_COLLAPSE and poll PWR_ON, then release the two PCIe0 resets
 *            ONLY if the GDSC reports power.  If power is refused every vote and GDSCR itself are
 *            written back, so the boot ends where it started.  Whether the GDSC needs the rail vote is
 *            exactly what this stage answers (M2b is the razor-thin follow-up if it says no).
 *   N=3  M3 pads, PHY and LTSSM: WLAN_EN high, 1 ms, PERST# released, the 99-write PHY sequence from
 *            this board's own device tree, then LTSSM enabled with ELBI polled for 100 ms.  Only on
 *            link-up are DBI 0x00/0x80 and BAR0's TCSR_SOC_HW_VERSION read - that is where the chip's
 *            vendor/device ID (17cb:1101 vs 17cb:1103) finally comes from.  On failure the pads go
 *            back to sleep rather than leaving a released reset on an untrained link.
 *   N=4  M4 the address translator: program the memory window that covers BAR0 and a CFG0 region that
 *            turns the conf window into the endpoint's configuration space, then read the identity that
 *            only a real endpoint can answer - vendor/device (17cb:1101 for a QCA6390), class code, and
 *            BAR0's TCSR_SOC_HW_VERSION, whose major field must be 2.
 *   N>=5 is not implemented yet.
 *
 * WHY READS ARE GATED (this is the rule the two earlier work lines paid for)
 *   The GPU SMMU read froze this device at t=19 s on 2026-11-03, and alioth's first DBI read never
 *   returned until the PCIe pipe clock was voted (UniXNU-alioth/porting.toml, area network, pc3).  On
 *   this SoC an unanswered device read is an SError, i.e. a panic.  So:
 *     - GCC at 0x00100000 and the three TLMM tiles are always-on domains and are read unconditionally;
 *     - PARF/ELBI are read only when GCC_PCIE_0_GDSCR bit 31 says the controller is powered;
 *     - DBI is read only when ELBI already reports XMLH_LINK_UP, because DBI is the access that hung
 *       alioth with the pipe clock off.
 *
 * WHAT M1 SETTLED ON HARDWARE (2026-11-20): the bootloader hands PCIe0 over POWERED DOWN
 * (gdscr=0x0022f001, same shape as alioth's), GCC_PCIE_WIFI_CLKREF_EN off, PHY REFGEN already on.  That
 * is why M2a exists and why it starts from the reset side.
 *
 * WRITES: none below stage 2.  Every store goes through wr() and every wr() call site is inside
 * stage2a(), which the dispatch reaches only when the argument parsed as >= 2.
 *
 * ROWS (kprintf is today's only channel; see the note on native_dbypcie_census)
 *   [dby-pcie] gcc gdscr=xxxxxxxx clkref=xxxxxxxx refgen=xxxxxxxx
 *   [dby-pcie] halt xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx     (pre-write snapshot)
 *   [dby-pcie] gcc2 votes=xxxxxxxx tbu=xxxxxxxx phyaux=xxxxxxxx ddrss=xxxxxxxx
 *   [dby-pcie] resets bcr=xxxxxxxx phybcr=xxxxxxxx phycom=xxxxxxxx
 *   [dby-pcie] pin20 west=xxxxxxxx/xxxxxxxx south=... north=...
 *   [dby-pcie] pin79 west=xxxxxxxx/xxxxxxxx south=... north=...
 *   [dby-pcie] tile wlan_en=NORTH(src) cfg=xxxxxxxx perst=NORTH(src) cfg=xxxxxxxx
 *   [dby-pcie] m2 before gdscr=... votes=... clkref=... refgen=...              (stage >= 2)
 *   [dby-pcie] m2 voted votes=... clkref=... refgen=... tbu=... phyaux=... ddrss=...
 *   [dby-pcie] m2 halt xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx  (after the votes)
 *   [dby-pcie] m2 gdsc after=xxxxxxxx pwron=n waited=nnnus
 *   [dby-pcie] m2 resets bcr=xxxxxxxx phybcr=xxxxxxxx released                (only when pwron=1)
 *   [dby-pcie] m2 power refused, votes and gdscr restored, resets untouched   (only when pwron=0)
 *   [dby-pcie] parf sys=... phy=... refclk=... dbi_base=... ltssm=xx
 *   [dby-pcie] m3 pads gpio20=xxxxxxxx/xxxxxxxx gpio79=xxxxxxxx/xxxxxxxx        (stage >= 3)
 *   [dby-pcie] m3 phy count=nn first=xxx:xx last=xxx:xx
 *   [dby-pcie] m3 ltssm xx->xx elbi=xxxxxxxx up=n waited=nnnnnus
 *   [dby-pcie] m3 dbi id=xxxxxxxx dllla=n bar0_soc_hw=xxxxxxxx major=n
 *   [dby-pcie] m3 no link in 100 ms, pads returned to sleep (link stays down)
 *   [dby-pcie] m4 atu r0 ctrl1=xxxxxxxx ctrl2=xxxxxxxx base=xxxxxxxx limit=xxxxxxxx   (stage >= 4)
 *   [dby-pcie] m4 atu mem ctrl1=xxxxxxxx ctrl2=xxxxxxxx base=xxxxxxxx limit=xxxxxxxx
 *   [dby-pcie] m4 cfg id=xxxxxxxx class=xxxxxxxx soc_hw=xxxxxxxx major=n
 *   [dby-pcie] m4 no config answer (vendor xxxx), windows left as programmed
 *   [dby-pcie] link elbi=xxxxxxxx up=n dbi=xxxxxxxx dllla=n
 *   [dby-pcie] stage=N rc=n
 */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

/* 99 PHY writes copied verbatim from this tablet's device tree (qcom,phy-sequence); see the
 * header for the dtb path, the line and why it is generated rather than typed. */
#include "DbyPciePhySeq.h"
/* The board's stream-mapping planner, the same one the audio and touch paths use: a stream is
 * mapped by pointing it at the last context bank - which the firmware leaves in
 * S1_TRANS_S2_BYPASS - with an S2CR of TYPE=TRANSLATE, because a TYPE=BYPASS S2CR reads back as
 * FAULT on this firmware (package/SmmuBypass.h:4-7). Reusing it is the point: a second
 * hand-written copy of that sequence is exactly what the repository rules forbid. */
#include "SmmuBypass.h"
/* The PMIC clock outputs the pin/clock controller does not own are voted through the apps RSC, and this
 * tree already does that for other subsystems: package/AudioProbe.cpp's micRail() is the reference copy of
 * the sequence (cmd-db copy, RSC layout, active request on one TCS, trigger, finish), and
 * package/AdspProbe.cpp uses the same planner for its own rails.  QcomRpmh.h is the planner; nothing here
 * touches the RSC registers directly except through the writes it returns. */
#include "QcomRpmh.h"
/* The board's own read-only FAT reader and the two UFS hooks package/AdspFirmware.c already
 * uses to take firmware out of a partition in place (no copy, no install).  M7 uses them to
 * enumerate what this tablet actually ships, because the port has never read its firmware
 * partition and the vendor's file names are the one thing a download cannot guess. */
#include "FatVolume.h"
/* POLARITY, because getting it wrong silently deletes this stage: native_ufs_part_find() returns 1 when
 * the partition was found and 0 otherwise (NativeUfs.cpp, and package/AdspFirmware.c tests it with
 * `if (!native_ufs_part_find(...))`), and native_ufs_part_read() returns 1 on success and 0 on failure
 * (AdspFirmware.c tests it with `if (!native_ufs_part_read(...))`).  Both are truthy-on-success, i.e. the
 * opposite of the usual 0-is-success convention of the rest of this file. */
extern "C" int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));
/* Absolute block read on LUN 0, same 4096-byte block unit and the same truthy-on-success polarity
 * (NativeUfs.cpp:1364).  M7b uses it to read the partition table itself. */
extern "C" int native_ufs_abs_read(uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));

extern "C" {

/* The row mirror.  Seventy-two rows of 120 bytes, NUL-terminated, the same shape as native_gpusmmu_census and
 * native_cpufreq_census, which package/BootLog.c walks for the boot page.  Nothing walks this one yet:
 * package/BootLog.c declares and walks the other two (:901, :909) but has no reference to this array, so
 * kprintf is today's only channel for the [dby-pcie] rows.  Wiring it in is a follow-up that touches a file
 * another work line holds, and it is recorded as a gap rather than claimed as a feature.  Seventy-two rows because the
 * stage-10 success path prints up to sixty and M1 alone prints ten; a mirror exactly as full as its writers silently drops the
 * next row, which this array has already happened to twice at eight and twelve. */
char native_dbypcie_census[72][120];

/* 0 = the probe never ran this boot, otherwise the stage number from dby-pcie=N. */
volatile uint32_t native_dbypcie_stage;
/* 0 = the census completed; negative = the step that refused, positive = a verdict code. */
volatile int32_t native_dbypcie_rc;

/* The entry point named by the weak import in package/BootProgress.c. */
void dby_pcie_probe(void);

}

namespace {

/* Register windows, all physical.  GCC 0x00100000 is this board's own value, agreed by two
 * independent board files (package/AppleARMBacklight.cpp:61 and package/DbyTouch/DbyTouchBoard.h:52);
 * the PCIe0 windows are the device tree's reg entries (dts says parf 0x01c00000+0x3000,
 * dm_core 0x60000000+0xf1d with ELBI at +0xf20). */
constexpr uint64_t kGcc = 0x00100000, kGccSpan = 0x94000;   /* covers 0x6b004 .. 0x9000c */
constexpr uint64_t kParf = 0x01C00000, kParfSpan = 0x3000;
constexpr uint64_t kDbi = 0x60000000, kDbiSpan = 0x2000;    /* DBI at +0x000, ELBI status at +0xf28 */
constexpr uint64_t kTile[3] = {0x0F100000, 0x0F500000, 0x0F900000};
constexpr const char *kTileName[3] = {"WEST", "SOUTH", "NORTH"};
constexpr uint64_t kTileSpan = 0x80000;                     /* pins 0..127 per tile */

/* GCC offsets (unchanged across kona and the same ones UniXNU-alioth's probe uses,
 * package/alioth_pcie.c:328-346). */
constexpr uint32_t kGdscr = 0x6B004;
constexpr uint32_t kHalts[6] = {0x6B010, 0x6B014, 0x6B01C, 0x6B024, 0x6B028, 0x6B02C};
constexpr uint32_t kClkref = 0x8C004;     /* GCC_PCIE_WIFI_CLKREF_EN, the DT's pcie_0_ldo */
constexpr uint32_t kRefgen = 0x6F02C;     /* GCC_PCIE_0_PHY_REFGEN */

/* The rest of M2's GCC surface, censused read-only so M2 starts from a measured state instead of an
 * assumed one (docs/wireless-spec-2026-11-20.md §4.5; all from mainline drivers/clk/qcom/gcc-sm8250.c,
 * fetched 2026-11-20, with the line each value came from recorded in that table):
 *   0x52008 bits 0..5    the six PCIe0 branch votes (slv_axi, mstr_axi, cfg_ahb, aux, pipe, q2a)
 *   0x9000c bit 0        gcc_aggre_noc_pcie_tbu_clk (enable and halt share the register)
 *   0x6f004 bit 0        gcc_pcie_phy_aux_clk
 *   0x8d058 bit 0        gcc_ddrss_pcie_sf_tbu_clk (not in alioth's list; M2 votes it and watches the halt)
 *   0x6b000 / 0x6c01c    GCC_PCIE_0_BCR and GCC_PCIE_0_PHY_BCR: assert = bit 0 set, release = clear
 *   0x6f000              GCC_PCIE_PHY_BCR, which alioth's corrected sequence deliberately leaves alone */
constexpr uint32_t kVotes = 0x52008;
constexpr uint32_t kTbu = 0x9000C;
constexpr uint32_t kPhyAux = 0x6F004;
constexpr uint32_t kDdrssSfTbu = 0x8D058;
constexpr uint32_t kPcieBcr = 0x6B000;
constexpr uint32_t kPciePhyBcr = 0x6C01C;
constexpr uint32_t kPciePhyComBcr = 0x6F000;

/* GDSC bits and the poll bound.  SW_COLLAPSE is bit 0 of the gdscr and PWR_ON is bit 31; the same two
 * bits the GPU's GDSC uses (package/Sm8250Platform/A650GmuPower.h:76-78), with the 2 ms bound alioth's
 * probe uses for the PCIe gdsc instead of the GPU's shorter one. */
constexpr uint32_t kGdscSwCollapse = 1u << 0;
constexpr uint32_t kGdscPwrOn = 1u << 31;
constexpr unsigned kGdscPollUs = 2000;

/* Stage 3: the PHY window (device tree `phy` = 0x01C06000 + 0x1000), the two NORTH pads, and the
 * vendor's two delays.  Pad values are built from the device tree's own pinctrl entries:
 *   cnss_wlan_en_active: function gpio, drive-strength 16, bias-pull-up, output-high
 *   pcie0's perst pad:   function gpio, drive-strength 2,  bias-pull-down, then output-high releases
 *                        the active-low reset
 * with the TLMM bit layout this board already uses (package/DbyTouch/DbyTouchBoard.h:234-241 and
 * UniXNU-alioth/package/alioth_pcie.c:355-363): function [5:2], drive ((mA/2)-1) at [8:6], pull at
 * [1:0], output-enable bit 9, and the level as bit 1 of the IN register. */
constexpr uint64_t kPhy = 0x01C06000, kPhySpan = 0x1000;
constexpr uint32_t kPadFuncGpio = 0u << 2;
constexpr uint32_t kPadDrive16 = 7u << 6;
constexpr uint32_t kPadDrive2 = 0u << 6;
constexpr uint32_t kPadPullUp = 3u;
constexpr uint32_t kPadPullDown = 1u;
constexpr uint32_t kPadOutputEnable = 1u << 9;
constexpr uint32_t kPadOut = 1u << 1;
constexpr unsigned kWlanEnableDelayUs = 1000;   /* alioth's WLAN_ENABLE_DELAY, same vendor constant */
constexpr unsigned kEpLatencyUs = 10000;        /* the device tree's qcom,ep-latency = <10> ms, waited
                                                 * after WLAN_EN and before PERST# is released */
constexpr unsigned kPerstDelayUs = 1000;
constexpr unsigned kLinkPollUs = 100000;        /* 100 ms, pci-msm's link-up bound */
constexpr unsigned kLinkPollStepUs = 100;

/* Stage 3b: the controller initialisation and DBI training words the vendor writes around PERST#.
 * Every offset and value below is UniXNU-alioth's hardware-validated sequence for this same SoC
 * (package/alioth_pcie.c, step 10 "parf init" and step 11 "vendor link training"), not a guess:
 *   parf device type 0x1000 = 4 (root complex), phy_ctrl bit 0 out, phy_refclk bit 12 out,
 *   pm_ctrl bit 5 out, dbi_base 0x350 = 0 (verified by read-back), sys_ctrl bit 29 out with bits 6
 *   and 1 in, slv_addr_space_size 0x358 = 0x4000000 (the device tree's own value), mhi_clock_reset
 *   bit 4 in, axi_mstr_wr_addr_halt bit 31 in, PARF interrupts masked and cleared, and the PHY
 *   reference generator's RCG set to source 1 divide 6 = 100 MHz.
 * The DBI half is pci-msm's seven training words (alioth's DWC_* constants): gen3_gen2_ctrl field
 * 0x1f00 = 1, gen3_eq_control bit 5, gen3_related bit 0 out, gen3_misc_control bit 0 in and back
 * out around the 0x55555555 preset write, and device_control_status field 0x7000 = 5. */
constexpr uint32_t kParfDeviceType = 0x1000;
constexpr uint32_t kParfPhyCtrlReg = 0x40;
constexpr uint32_t kParfPhyRefclkReg = 0x4C;
constexpr uint32_t kParfPmCtrl = 0x20;
constexpr uint32_t kParfSlvAddrSpace = 0x358;
constexpr uint32_t kParfSlvAddrSpaceValue = 0x4000000;
constexpr uint32_t kParfMhiClkReset = 0x174;
constexpr uint32_t kParfAxiMstrWrHalt = 0x1A8;
constexpr uint32_t kParfIntAllMask = 0x22C;
constexpr uint32_t kParfIntAllClear = 0x228;
constexpr uint32_t kGccRefgenCfg = 0x6F018;      /* GCC RCG: source 1, divide by 6 = 100 MHz */
constexpr uint32_t kGccRefgenCfgValue = 0x105;
constexpr uint32_t kGccRefgenRootEn = 0x6F014;
constexpr uint32_t kGccRefgenRootEnValue = 0x2;
constexpr uint32_t kDbiGen3Gen2Ctrl = 0x80C;
constexpr uint32_t kDbiGen3EqControl = 0x8A8;
constexpr uint32_t kDbiGen3Related = 0x890;
constexpr uint32_t kDbiGen3MiscControl = 0x8BC;
constexpr uint32_t kDbiSpcieCap = 0x154;
constexpr uint32_t kDbiDeviceControlStatus = 0x78;
constexpr uint32_t kDbiCorePreset = 0x55555555;
constexpr uint32_t kDbiTlpRdSize = 5;

/* M4: the address translator.  DWC's iATU region registers are 0x200 apart (drivers/pci/controller/
 * dwc/pcie-designware.h: PCIE_ATU_REGION_CTRL1_OFF 0x000, CTRL2 0x004, LOWER_BASE 0x008, UPPER_BASE
 * 0x00C, LIMIT 0x010, LOWER_TARGET 0x014, UPPER_TARGET 0x018), and the device tree gives the
 * translator its own window at `iatu` = 0x60001000 + 0x1000.  The two ranges M4 programs are the
 * device tree's own: IO 0x60200000 + 0x100000 and MEM 0x60300000 + 0x3d00000, of which the first
 * megabyte covers BAR0.  CPIO target addresses: a MEM region maps CPU -> PCI unchanged, a CFG0 region
 * maps CPU -> (bus << 20 | dev << 15 | fn << 12), which is how the endpoint's own configuration space
 * becomes readable at the conf window (0x60100000). */
constexpr uint32_t kAtuRegionStride = 0x200;
constexpr uint32_t kAtuCtrl1 = 0x000;
constexpr uint32_t kAtuCtrl2 = 0x004;
constexpr uint32_t kAtuLowerBase = 0x008;
constexpr uint32_t kAtuUpperBase = 0x00C;
constexpr uint32_t kAtuLimit = 0x010;
constexpr uint32_t kAtuLowerTarget = 0x014;
constexpr uint32_t kAtuUpperTarget = 0x018;
constexpr uint32_t kAtuTypeMem = 0x0;      /* PCIE_ATU_TYPE_MEM */
constexpr uint32_t kAtuTypeCfg0 = 0x4;     /* PCIE_ATU_TYPE_CFG0 */
constexpr uint32_t kAtuEnable = 1u << 31;  /* PCIE_ATU_ENABLE */
constexpr uint64_t kAtuWindow = 0x60001000, kAtuWindowSpan = 0x1000;
/* Inbound regions interleave with the outbound ones in the unrolled layout: outbound n starts at
 * n * 0x200, inbound n at 0x100 + n * 0x200.  UniXNU-alioth package/alioth_pcie.c:1975 uses exactly
 * that for its inbound region 1 (`const uint32_t r = 0x100 + 0x200;`) and says why the endpoint needs
 * one at all (:1952-1966): "MHI's SBL load is the first transfer the endpoint starts by itself, and it
 * reads host memory ... the endpoint reads that address as a PCI memory read", with region 0 inbound
 * being the MSI doorbell page.  This probe has never programmed an inbound region, so the read the
 * doorbell asks for had no window on this side of the link. */
constexpr uint32_t kAtuInboundOffset = 0x100;
constexpr uint64_t kMemWindow = 0x60300000, kMemWindowSpan = 0x100000;   /* BAR0 is the first 1 MiB */
constexpr uint64_t kConfWindow = 0x60100000, kConfWindowSpan = 0x1000;
constexpr uint32_t kEndpointBusDevFn = (1u << 24);   /* ATU_BUS=31:24, ATU_DEV=23:19, ATU_FUNC=18:16
                                                      * (mainline pcie-designware.h:212-215); bus 1, dev 0, fn 0.
                                                      * The first draft used 1<<20, which is the DEV field - it would
                                                      * have aimed the conf window at bus 0 device 2. */
constexpr uint32_t kParfAtuBase = 0x634;             /* PARF_ATU_BASE_ADDR: where the core's iATU CSRs live */
constexpr uint32_t kParfAtuBaseHi = 0x638;
constexpr uint32_t kParfDbiBaseHi = 0x354;           /* PARF_DBI_BASE_ADDR_V2_HI */
constexpr uint32_t kParfSlvSize = 0x358;             /* PARF_SLV_ADDR_SPACE_SIZE_V2 */
constexpr uint32_t kParfSlvSizeHi = 0x35C;
constexpr uint32_t kParfSlvSizeValueHi = 0x80000000u; /* SLV_ADDR_SPACE_SZ, mainline's value */

/* M5: the applications SMMU and the endpoint's stream id.  Base and size are the ones the touch and
 * audio paths already map (package/TouchBoard.h:19-20, 0x15000000 + 0x100000); the stream id comes
 * from the device tree's pcie0 iommu-map, requester 0x100 -> SID 0x1c01 (/root/dby-full.dts:8968,
 * qcom,smmu-sid-base = 0x1c00).  The DMA arena is 32 MiB physically contiguous, the size the same-SoC
 * record settled on for the endpoint. */
constexpr uint64_t kAppsSmmu = 0x15000000, kAppsSmmuSpan = 0x100000;
/* 0x1c01 is the device tree's own mapping, not a choice: /root/dby-full.dts:8968-8969 has
 * qcom,smmu-sid-base = <0x1c00> and iommu-map = <0x00 0x47 0x1c00 0x01 0x100 0x47 0x1c01 0x01>, i.e.
 * requester 0x100 - the endpoint - at SID 0x1c01.  This repository's own dts/dby_w09.dts does not carry
 * an iommu-map (the translator is not part of the ported device tree yet), which is why the citation is
 * the tablet's DT dump rather than a line in this repo. */
constexpr uint16_t kPcieSid = 0x1C01;
constexpr unsigned kDmaArenaBytes = 32u << 20;

/* M6: the MHI register block inside the endpoint's BAR0, read through the outbound memory window M4
 * programmed.  The offsets are the ones the working same-chip port uses - repository
 * rui12366B/UniXNU-gts7xlwifi, file package/MhiBoot.h (lines 15-29 of the checkout this was read
 * from: MHI_REGLEN 0x00, MHI_BHIOFF 0x28, MHI_BHIEOFF 0x2c, MHI_CTRL 0x38, MHI_STATUS 0x48, state
 * field 15:8, SYSERR 1<<2, READY 1<<0) - not a copy of that header, only the fields this stage reads;
 * the boot state machine itself is M6b's job, not this census's.  That file is not part of this
 * repository. */
constexpr uint32_t kMhiRegLen = 0x00;
constexpr uint32_t kMhiBhiOff = 0x28;
constexpr uint32_t kMhiBhieOff = 0x2C;
constexpr uint32_t kMhiCtrl = 0x38;
constexpr uint32_t kMhiStatus = 0x48;
constexpr uint32_t kMhiStatusReady = 1u << 0;
/* MHI_CTRL bit 0 is MHE, the block enable.  Mainline does not touch it for a BHI download, which is
 * why M6e tries it as ONE of three preambles rather than as the answer. */

constexpr uint32_t kMhiStatusSysErr = 1u << 2;
/* BHI_STATUS: offset and field from drivers/bus/mhi/common.h (BHI_STATUS 0x2c, BHI_STATUS_MASK
 * GENMASK(31, 30), BHI_STATUS_RESET 0x00, SUCCESS 0x02, ERROR 0x03).  The same header names MHICFG at
 * 0x10, which is what the census reads as MHI_CFG. */
constexpr uint32_t kBhiStatusReg = 0x2C;
/* The doorbell's low thirty bits carry a NON-ZERO session id (drivers/bus/mhi/host/boot.c:
 * MHI_RANDOM_U32_NONZERO(BHI_TXDB_SEQNUM_BMSK)); a fixed non-zero value serves the same purpose here and
 * keeps the row reproducible. */
/* BHI_TXDB_SEQNUM_BMSK is GENMASK(29, 0): bits 31:30 are reserved, so the session id has to stay
 * inside thirty bits.  The first value written here had bit 30 set, which the device would see as
 * something other than a sequence number. */
constexpr uint32_t kBhiSessionId = 0x1A5A0001u;
/* How much of the firmware goes to BHI.  The working same-chip port hands over 0x80000 bytes - the boot
 * loader's portion of amss.bin - and only ever verifies that first 512 KiB against the file
 * (UniXNU-gts7xlwifi package/T970WifiStack.cpp: preBhiImage(), "n = imageBytes < 0x80000 ? imageBytes :
 * 0x80000").  This probe has been ringing the doorbell with the whole 4,352,144-byte file, which is not
 * what the device's BHI expects: the SBL is the head of the image, and the rest belongs to BHIe after
 * the SBL runs. */
constexpr uint32_t kBhiSblBytes = 0x80000;
/* The MSI vector handed to the boot host interface.  One is a real vector index for a function with the
 * usual 32-vector table, and matches what the working same-chip port writes (its msi_vector). */
/* 0 from the 2026-11-23 16:30 boot on, which is the last value in the transfer descriptor this probe had
 * never changed.  The reasoning that made it 1 is in the record - the working same-chip port's start_sbl
 * writes "m->msi_vector", which read like a vector the device needs - but the only value that port ever
 * passes is 0 (T970WifiProbe.cpp:747, mhi_boot_begin(&boot, 0, pa, kSblBytes, 0, 0, ...)), and mainline's
 * mhi_fw_load_bhi() does not write BHI_INTVEC at all.  With the image verified in DRAM, no SMMU fault across
 * the transfer and the device staying in PBL, the descriptor's own values are what is left to vary. */
constexpr uint32_t kBhiMsiVector = 0;
/* true: hand the whole image to BHI; false: only the first 0x80000 bytes.  One constant so the two
 * slices can be compared boot by boot without touching anything else.
 *
 * false from the 2026-11-22 21:00 boot on, and the reason is the state that boot finally reached: with
 * the power-on sequence in place (WLAON_QFPROM VDD4BLOW cleared, interrupts cleared, the SoC global reset
 * pulsed, MHICTRL's reset bit seen to clear) the doorbell is rung on a block that is no longer in reset,
 * and it is still not consumed.  The remaining difference from the working same-chip port is the amount
 * of data: it hands 0x80000 bytes - ath11k's sbl_size, the boot loader's own size - while this probe has
 * been asking a 4,352,144-byte transfer of a part whose boot loader is a fraction of that.  A request the
 * device considers impossible is one way to get neither a transfer nor an error out of it. */
/* true again from the 2026-11-23 15:25 boot, together with amss20.bin back at the head of the path list
 * below, because with m3.bin chosen this constant makes no difference at all: 266,684 bytes are below
 * 0x80000, so "whole" and "sbl" are the same number for it.  What this boot tests is therefore one input -
 * the COMPLETE amss20.bin, 4,352,144 bytes, one ELF32 with eighteen program headers - which is mainline's
 * own behaviour when fbc_download is false (mhi_fw_load_handler: size = firmware->size) and the one
 * combination this record never actually measured on an awake endpoint: the last whole-image attempt
 * predates the power-on sequence, when VDD4BLOW was set and MHI was left in reset, so its silence says
 * nothing about the device in the state it is in now.  m3.bin stays in the list as the fallback. */
constexpr bool kBhiWholeFile = true;
/* false: do not touch PERST# before handing the image over.  The toggle was the recovery step of the
 * soc-reset experiment that has since been removed, so it now resets an endpoint M4 had already armed.
 * HYPOTHESIS, not a proven mechanism: an asserted PERST# is expected to send BAR0 back to its default
 * and to clear Memory-Enable, after which every BAR0 read is answered UR and reads back as all ones,
 * which is what the 2026-11-22 19:50 boot printed right after this block ran (tools/console-m30.log:
 * "retrain ltssm 10d->10d ... up=1" then "m6h after reset reglen=ffffffff bhi=ffffffff ...").  That
 * frame cannot settle it - the retrain's poll starts only after its two PERST# delays, so a link that
 * dropped and came back inside them looks like up=1 waited=0us - and what it does establish is only
 * that the block's own reads went all-ones after this block ran, while the boots without it read real
 * values.  Removing the writes is therefore a one-variable experiment, not a repair on its own.  Kept
 * as a switch rather than deleted: a cold-boot path will want a real PERST# followed by a full re-arm. */
constexpr bool kRetrainBeforeBhi = false;
/* The boot host interface's own registers, offsets inside the BHI space whose base M6a read from
 * BAR0+0x28 (0x100 on this unit) - rui12366B/UniXNU-gts7xlwifi package/MhiBoot.h:20-31, the same
 * header the census offsets came from.  M6b writes the image address and size, clears the interrupt
 * vector, rings the doorbell, and then reads the execution environment back until the secondary boot
 * loader reports itself. */
constexpr uint32_t kBhiImgAddrLow = 0x08;
constexpr uint32_t kBhiImgAddrHigh = 0x0C;
constexpr uint32_t kBhiImgSize = 0x10;
constexpr uint32_t kBhiImgTxDb = 0x18;
constexpr uint32_t kBhiIntVec = 0x20;
constexpr uint32_t kBhiExecEnv = 0x28;
constexpr uint32_t kBhiErrCode = 0x30;
/* BHI_ERRDBG1..3: what the boot loader leaves behind when it fails - the working port reads all three
 * the moment BHI_STATUS reports an error (UniXNU-gts7xlwifi package/T970WifiProbe.cpp:752-756, into its
 * check array at bhi + 0x34/0x38/0x3c), and mainline's mhi_fw_load_error_dump() reads the same trio
 * (drivers/bus/mhi/host/boot.c).  The 2026-11-22 21:19 boot is why they are read here: the device accepted
 * a 0x80000-byte transfer, took 416 ms over it and then reported BHI_ERRCODE 0xef128100, which is the boot
 * loader saying something - and these three registers are the only place it says what. */
constexpr uint32_t kBhiErrDbg1 = 0x34;
constexpr uint32_t kBhiErrDbg2 = 0x38;
constexpr uint32_t kBhiErrDbg3 = 0x3C;
constexpr unsigned kBhiPollUs = 20000000;    /* 20 s, the working same-chip port's own deadline for the boot
                                              * loader (it samples every 800 ms), and no longer a guess: the
                                              * device transfers 524,288 bytes in 416,500 us and 266,684 in
                                              * 115,000 us, i.e. about 1.2-2.3 MB/s, so the whole 4,352,144
                                              * byte image needs 2-4 s and a 3 s window would have expired
                                              * just before the answer arrived - printing xfer=0, which is
                                              * indistinguishable from the silence that four earlier versions
                                              * of this probe read as 'the doorbell is ignored'.  A wait that
                                              * is too short is worse than no wait, because its result looks
                                              * like a device fault. */
constexpr unsigned kBhiPollStepUs = 500;
constexpr uint32_t kMhiEePbl = 0;

/* ath11k reaches the chip's SoC-side registers through a WINDOW: an access to BAR0+0x310c with the
 * enable bit and a window number, after which the register inside that window is read or written at
 * BAR0+0x80000+(offset & 0x7ffff).  ath11k's pci.c declares exactly these (ATH11K_PCI_WINDOW_REG_ADDRESS
 * 0x310c, ATH11K_PCI_WINDOW_ENABLE_BIT 0x40000000, ATH11K_PCI_WINDOW_START 0x80000,
 * ATH11K_PCI_WINDOW_RANGE_MASK 0x7ffff, WINDOW_VALUE 1), and the working same-chip port implements the
 * same mapping in its `struct W` (UniXNU-gts7xlwifi package/T970WifiProbe.cpp:694-700) with a comment
 * saying offsets above 512 KiB go through 0x310c.  This probe has never used it, which is why every
 * ath11k offset at or above 0x80000 has been out of reach: the record's own 2026-11-22 note about
 * "window-relative" offsets is this. */
constexpr uint32_t kPcieWindowSel = 0x310C;
constexpr uint32_t kPcieWindowEnable = 0x40000000u;
constexpr uint32_t kPcieWindowStart = 0x80000;
constexpr uint32_t kPcieWindowRangeMask = 0x7FFFF;
/* ath11k_pci_select_window() writes enable | ((offset >> 19) & 0x3f) and nothing else; the working port
 * does the same (T970WifiProbe.cpp:698).  An earlier draft of this probe ORed in ath11k's WINDOW_VALUE
 * (1) as well, which is a no-op for every offset used here - they all land in window 63, whose bit 0 is
 * already set - and would diverge on an even-numbered window.  Not written. */
/* The WLAN always-on registers the same port clears before its boot host interface will answer
 * (T970WifiProbe.cpp:719-730).  WLAON_QFPROM_PWR_CTRL governs the block's power: VDD4BLOW is bit 2 and
 * the port clears bit 0 with it, citing cnss2's set_wlaon_pwr_ctrl(false, false, ...).  WARM_SW_ENTRY
 * makes the chip's Q6 take the warm path into a dead loop when it is non-zero, and PCIE_Q6_COOKIE_ADDR
 * is read beside it because the two are the pair the vendor's own WAR touches. */
constexpr uint32_t kWlaonQfpromPwrCtrl = 0x1F8031C;
constexpr uint32_t kWlaonQ6CookieAddr = 0x1F80500;
constexpr uint32_t kWlaonWarmSwEntry = 0x1F80504;
constexpr uint32_t kWlaonQfpromClear = 0x5;       /* VDD4BLOW (bit 2) and bit 0 */
/* ath11k_mhi_clear_vector() in ath11k's pci.c, as the same port performs it (T970WifiProbe.cpp:730). */
constexpr uint32_t kMhiVectorRegs[4] = {0x360, 0x368, 0x394, 0x39C};
constexpr uint32_t kMhiCtrlReset = 2;             /* MHICTRL_RESET; clears the SYSERR a reset leaves */
/* The two registers the same port writes between the power fix and the MHI reset
 * (T970WifiProbe.cpp:713-734): PCIE_PCIE_INT_ALL_CLEAR at 0x1e08228, and PCIE_SOC_GLOBAL_RESET at 0x3008,
 * which ath11k pulses 1 -> 0 with 10 ms on each side ("10 ms is the time delay specified by the hardware
 * team").  The 2026-11-22 20:52 boot is why they are here rather than skipped: with the power fix in place
 * MHICTRL's reset bit took but never cleared (m6k mhi ctrl 00000000 -> 00000002), and the working port's
 * state machine will not start a boot loader until that bit is gone (package/MhiBoot.inc: MhiResetting
 * waits for it before start_sbl).  The pulse is the step this probe had been missing in between, and the
 * two boots that died on 0x3008 reached it with the subsystem unpowered. */
constexpr uint32_t kPcieIntAllClear = 0x1E08228;
constexpr uint32_t kPcieSocGlobalReset = 0x3008;
constexpr uint32_t kPcieSocGlobalResetV = 1;
constexpr unsigned kSocResetDelayUs = 10000;
constexpr unsigned kMhiResetPollUs = 200000;      /* how long the reset bit is given to clear itself */
constexpr unsigned kMhiResetPollStepUs = 2000;
/* How long the block is given to report READY after the interface stops answering.  The same-chip port
 * allows 20 s for the whole handoff and samples every 800 ms; this is a shorter, separate look because it
 * only decides whether the loader started, not whether it finished. */
constexpr unsigned kMhiReadyPollUs = 2000000;
constexpr unsigned kMhiReadyPollStepUs = 20000;
/* The apps SMMU's fault status, read after a failed handoff.  These are the offsets the working same-chip
 * port reads for the same purpose (UniXNU-gts7xlwifi package/T970WifiProbe.cpp:749 and :770-772:
 * wdom::r32(0x48) before and after the transfer, and its context bank's FSR at CBBASE+0x58): the global
 * fault status register itself, the faulting address, and the two syndrome registers, one of which carries
 * the stream id of the access that faulted.  A stream id of 0x1c01 with an address inside the arena means
 * the endpoint really does read this memory through the SMMU and the read failed; silence in all four means
 * either that its reads succeeded or that they never reached the SMMU at all - different answers, which is
 * why the row prints the raw registers rather than a verdict. */
constexpr uint64_t kSmmuFaultStatus = 0x48, kSmmuFaultAddress = 0x40;
constexpr uint64_t kSmmuFaultSynr0 = 0x50, kSmmuFaultSynr1 = 0x54;
/* The primary boot loader's own account of a failed boot, read through the same 0x310c window.  The
 * offsets are the working same-chip port's periph23 (UniXNU-gts7xlwifi package/T970WifiProbe.cpp:774-786),
 * taken there from cnss2's cnss_pci_dump_qca6390_sram_mem: TCSR_PBL_LOGGING, PBL_WLAN_BOOT_CFG,
 * PBL_BOOTSTRAP_STATUS, the PCie error-debug register, and then a twenty-word log buffer.  That port used
 * exactly this census to find that its device read every page of its boot loader and still failed its check
 * at stage 0x0e with code 3, and that the boot loader faulted at an address in the 0xa0000000 range - which
 * is why this probe reads them too: the device answers a failed handoff with an error code (this boot:
 * 0xef128100) and nothing else, and the log is where it explains itself.  All read-only. */
constexpr uint32_t kPblLogging = 0x1B000F8;
constexpr uint32_t kPblWlanBootCfg = 0x1E22B34;
constexpr uint32_t kPblBootstrapStatus = 0x1910008;
constexpr uint32_t kPcieBhiErrDbg2 = 0x1E0E238;
constexpr uint32_t kPblLogBase = 0x1403D58;
constexpr unsigned kPblLogWords = 20;
/* How the WLAN subsystem is woken: the command database and the applications RSC, and the four clock
 * outputs the working same-chip port turns on for the same
 * reason - T970WifiProbe.cpp:227-231, "the PMIC clock outputs the command DB lists (clk-rpmh: one word,
 * 1 = on).  The PBL runs from the chip's own oscillator, then fails 300 ms after the boot loader arrives
 * (ERRCODE ef0e0003, PBL stage 0x0e02), which fits a wait for an XO that never comes."  Which output feeds
 * QCA6390's XO is not in this unit's device tree either, so all four go on; a clock enable is idempotent
 * and cannot brown out a rail.  TCS 0 is the WiFi bring-up's by convention - package/AdspProbe.cpp:248 says
 * "the WiFi bring-up sends through TCS 0, so the two never share one", and the audio path's comment says the
 * same - but this repository has a real collision here: package/DbyCamera/DbyCamera.cpp:32 takes kArcTcs = 0
 * because its header (:8-10) believed the board had no WiFi path.  That reasoning no longer holds, and the
 * camera is the one that should move (to 2, which the audio copy nominally uses but does not run on this
 * board).  Until it does, this probe and the camera driver must not vote at the same instant; the probe runs
 * once, early, before anything opens a camera, so the exposure is a boot and not a steady state. */
/* This board's addresses, not the ones the audio copy carries: package/AdspProbe.cpp:15 states them for
 * kona - "RSC DRV 2 at 0x18220000 with TCS block at +0xd00 ... cmd-db 0x80860000" - and :57 defines them,
 * while package/AudioProbe.cpp's micRail() still holds SM8550's 0x17a20000 / 0x81c60000, which
 * package/DbyCamera/DbyCamera.cpp:8-10 records explicitly ("its RSC and cmd-db bases are still SM8550's;
 * this board is 0x18220000 / 0x80860000").  Copying the audio addresses would have voted into a window that
 * is not the RSC on this chip. */
constexpr uint64_t kCmdDb = 0x80860000, kCmdDbBytes = 0x20000;
constexpr uint64_t kAppsRsc = 0x18220000, kAppsRscBytes = 0x10000;
constexpr uint32_t kRscTcsWindow = 0xD00;
constexpr unsigned kWifiTcs = 0;
constexpr const char *kWifiClocks[4] = {"rfclka1", "rfclka3", "lnbclka2", "lnbclka3"};
/* The WLAN rails, named and priced by this tablet's own device tree rather than guessed: the cnss node
 * declares vdd-wlan-aon and vdd-wlan-dig on the same supply, then vdd-wlan-io, vdd-wlan-rfa1 and
 * vdd-wlan-rfa2, and those phandles resolve to the rpmh regulators smpa6, smpa4, smpa5 and smpc8 with the
 * voltages below (/root/dby-full.dts: the cnss node at 6270, its supply phandles 0xa8..0xab, and the
 * regulator nodes that own them - smpa6 0xe7ef0 = 950 mV, smpa4 0x1b7740 = 1800 mV, smpa5 0x1cfde0 = 1900
 * mV, smpc8 via pm8150a-s8 0x149970 = 1350 mV).  The device tree also names smpa6 outright as the WLAN
 * command-database resource (qcom,cmd_db_name = smpa6), which is the one resource the vendor driver votes
 * beside the supplies.  The working same-chip port votes four rails of its own for the same reason
 * (T970WifiProbe.cpp:145: smpa4 1800, smpa6 950, smpa5 2040, smpc8 1350) and this is the same set - the only
 * difference is smpa5's voltage, where this unit's tree says 1900 mV and the tree wins.
 *
 * Two things this table's first version was asked to say and did not.  First, attribution: the four clock
 * votes below are unchanged in content by this change, and the boot that first made them (2026-11-23 15:54)
 * showed they make no difference - so if the handoff behaves differently now it is the rails that moved, even
 * though the clock requests now follow ~8 ms of rail voting.  Second, the hazard a rail vote carries, stated
 * correctly this time: an active vote REPLACES the level the boot loader left behind rather than joining it,
 * because a level left by ABL is static hardware state and not a vote.  What makes 1900 mV safe here is
 * therefore not aggregation but the two facts around it - RPMh does aggregate concurrent active votes by
 * taking the highest, so this cannot pull a rail below what another *active* client asks for, and the only
 * other consumer of smpa5 in this device tree is the vddio supply of qcom,wil6210 (/root/dby-full.dts:6421),
 * a 60 GHz device this tablet does not carry.  If that device were fitted, this vote would change its supply
 * level and the value below would have to be reconciled with its driver rather than with cnss. */
constexpr struct {const char *name; uint32_t millivolts;} kWifiRails[4] = {
	{"smpa6", 950}, {"smpa4", 1800}, {"smpa5", 1900}, {"smpc8", 1350},
};

constexpr unsigned kMhiStateShift = 8;        /* the state / execution-environment field is 15:8 */
constexpr uint64_t kMhiWindowSpan = 0x1000;   /* one page: the stage accepts a register block of at most 0x1000 */
/* The root complex's own type-0 header.  Mainline and the same-SoC record both program it before the
 * translator is asked to do anything, and the reason is the one line UniXNU-alioth/package/alioth_pcie.c
 * records at its step for this: "without those two the address translator has nothing to translate and
 * the window reads 0xffffffff however it is programmed".  Config writes to the core's own header are
 * gated by DBI 0x8BC bit 0. */
constexpr uint32_t kDbiCommandStatus = 0x04;
constexpr uint32_t kDbiBusNumbers = 0x18;
constexpr uint32_t kDbiCommandMemIoBm = 0x7;
constexpr uint32_t kDbiBusNumbersValue = 0x00FF0100u;   /* primary 0, secondary 1, subordinate 0xff */
/* The endpoint's own configuration space, reached through the CFG0 window once it is aimed. */
constexpr uint32_t kCfgCommand = 0x04;
constexpr uint32_t kCfgBar0 = 0x10;
constexpr uint32_t kCfgBar0Value = 0x60300000u;         /* the memory window's base */
constexpr uint32_t kCfgCommandMemMaster = 0x6;          /* memory space + bus master */

/* The second translator layout, and the one this board actually answers: the viewport registers at
 * DBI + 0x900.  M4's first run read the unrolled block the device tree calls "iatu" (0x60001000) and
 * got 0xffffffff for every field, before and after programming - so on this board that window does not
 * decode, exactly as the same-SoC record warns ("dm_core and iatu both hang on their first access",
 * UniXNU-alioth/package/alioth_pcie.c:1086).  Its step_config therefore probes the viewport at
 * DBI+0x900 and programs one layout or the other; M4 does the same, and reports which answered. */
constexpr uint32_t kAtuViewport = 0x900;
constexpr uint32_t kAtuVpCtrl1 = 0x904;
constexpr uint32_t kAtuVpCtrl2 = 0x908;
constexpr uint32_t kAtuVpLowerBase = 0x90C;
constexpr uint32_t kAtuVpUpperBase = 0x910;
constexpr uint32_t kAtuVpLimit = 0x914;
constexpr uint32_t kAtuVpLowerTarget = 0x918;
constexpr uint32_t kAtuVpUpperTarget = 0x91C;


/* PARF offsets (pci-msm.c through UniXNU-alioth/package/alioth_pcie.c:487-495). */
constexpr uint32_t kParfSysCtrl = 0x00;
constexpr uint32_t kParfPhyCtrl = 0x40;
constexpr uint32_t kParfPhyRefclk = 0x4C;
constexpr uint32_t kParfDbiBase = 0x350;  /* 2.7.0 layout; 0x168 is the 2.3.2 one */
constexpr uint32_t kParfLtssm = 0x1B0;
/* LNKCTL2, the root port's link control 2: bits 3:0 are the target link speed, and the working
 * same-chip port sets them to 1 (2.5 GT/s) BEFORE it releases PERST# and enables the LTSSM, with this
 * note (UniXNU-gts7xlwifi package/T970WifiProbe.cpp:388-391): "Samsung's msm_pcie (TWRP dmesg 'PCIe RC0
 * Target GEN1 ... Current GEN1') leaves the root port's LNKCTL2 (DBI 0xa0) at 0x00010001; at the Gen2 this
 * file trained to (LNKSTA 0xb012) the PBL read the SBL and rejected it (ERRCODE ef0e0003) although DRAM
 * held the right bytes."  That is the one observation in this line's record that explains a boot loader
 * rejecting two different images with the same code, and this probe trains at Gen2 (its own m3 row prints
 * dll=b0120008, whose LNKSTA speed field - bits 19:16 - is 2), so it now asks for Gen1 the same way. */
constexpr uint32_t kDbiLinkControl2 = 0xA0;
/* DBI 0x80 is the link control/status PAIR as one 32-bit read: LNKCTL in bits 15:0 and LNKSTA in 31:16.
 * The negotiated speed is LNKSTA bits 3:0, i.e. bits 19:16 of that word, and DLLLA - which the link test
 * uses - is LNKSTA bit 13, bit 29 of the word.  The first version of this row printed `dll & 0xf` and the
 * review caught it against the probe's own evidence: dll = 0xb0120008 has 8 in the low nibble (LNKCTL's
 * ASPM control) and the negotiated 2 in nibble 4. */
constexpr uint32_t kDbiLinkSpeedShift = 16;
constexpr uint32_t kLinkSpeedGen1 = 1;

/* ELBI / DBI.  The ELBI status word is at 0xf20 + 0x08; XMLH_LINK_UP is bit 10.  DLLLA is bit 29 of
 * DBI 0x80.  (pci-msm.c:4379-4424, alioth's :967-1004.) */
constexpr uint32_t kElbiStatus = 0xF28;
constexpr uint32_t kDbiId = 0x000;
constexpr uint32_t kDbiLinkStatus = 0x080;
constexpr uint32_t kLinkUpBit = 1u << 10;
constexpr uint32_t kDlllaBit = 1u << 29;

/* TLMM per-pin layout, verified in package/DbyTouch/DbyTouch.cpp:1239-1242: CFG at +0, IN at +4,
 * stride 0x1000.  The fields the census tests come from package/alioth's own translation
 * (alioth_pcie.c:355-363): function bits [5:2], drive ((mA/2)-1) at [8:6], pull at [1:0],
 * output-enable at bit 9, and the output level as bit 1 of the IN register. */
constexpr uint32_t kPinCfg = 0x0;
constexpr uint32_t kPinIn = 0x4;
constexpr uint32_t kPinStride = 0x1000;

struct Window {
	IOMemoryMap *map;
	volatile uint32_t *va;
};

bool mapWindow(Window &w, uint64_t base, uint64_t size)
{
	IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
	w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
	if (memory) memory->release();
	w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
	return w.va != nullptr;
}

void unmapWindow(Window &w)
{
	if (w.map) { w.map->release(); w.map = nullptr; }
	w.va = nullptr;
}

inline uint32_t rd(volatile uint32_t *base, uint32_t reg)
{
	return base[reg / 4];
}

/* A store into one of the windows above.  M1 never reaches this: only stage 2 and later call it, and
 * the reviewer's check for "reads only" is therefore now "reads only while stage < 2".  The barrier is
 * the same one package/AppleARMBacklight.cpp uses for its GCC votes. */
inline void wr(volatile uint32_t *base, uint32_t reg, uint32_t value)
{
	base[reg / 4] = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

bool gDone;
char *gRow;

/* One census row: printed to the console and mirrored for the page.  Rows are filled in order, so a
 * stage that stops early leaves the rows it did produce instead of an empty array. */
void row(const char *text)
{
	unsigned index = 0;
	while (index < 72 && gRow[index * 120] != '\0') index++;
	if (index < 72) {
		unsigned i = 0;
		while (text[i] != '\0' && i < 119) { gRow[index * 120 + i] = text[i]; i++; }
		gRow[index * 120 + i] = '\0';
	} else {
		/* The mirror is full.  The row still reaches the console below - that is today's only channel
		 * anyway - and this one line says why a later row is missing from the array, instead of leaving
		 * a reader to wonder. */
		static bool announced;
		if (!announced) {announced = true; kprintf("[dby-pcie] census full, page mirror stopped\n");}
	}
	kprintf("%s\n", text);
	/* Pace the serial.  Two boots in a row lost the rows that followed a burst - one of them lost the
	 * whole m6c/m7d block while the rows on either side printed, and a capture duplicated a row, which
	 * is what a CDC endpoint without flow control does when the device writes faster than the host
	 * drains.  Three milliseconds per row costs a fifth of a second across the whole probe and makes the
	 * console trustworthy again. */
	IODelay(3000);
}

/* The tile a pin lives in comes from the SoC's pin table, not from what the pad happens to contain.
 *
 * RETIRED 2026-11-21: M1 shipped with a matcher that picked the tile whose CFG matched the device
 * tree's cnss_wlan_en_active programming (gpio, 16 mA, pull-up).  On hardware it printed "none" for a
 * reason that should have been obvious in advance - that programming is what M3 writes, so before M3
 * every candidate is at its reset default - and the observation it did produce (WEST 0x0, SOUTH 0x0,
 * NORTH 0x1 for both pins) was three candidates and no answer.
 *
 * The authority is mainline drivers/pinctrl/qcom/pinctrl-sm8250.c, which carries the tile of every
 * pin (fetched 2026-11-21, 45,514 bytes; this board's own touch header already pointed at it,
 * package/DbyTouch/DbyTouchBoard.h:222-236):
 *     pinctrl-sm8250.c:1151  [20] = PINGROUP(20, NORTH, qup7, ...)        <- WLAN_EN
 *     pinctrl-sm8250.c:1152  [21] = PINGROUP(21, NORTH, qup7, ...)        <- BT_EN
 *     pinctrl-sm8250.c:1210  [79] = PINGROUP(79, NORTH, pci_e0, ...)      <- PERST#, first function pci_e0
 *     pinctrl-sm8250.c:1211  [80] = PINGROUP(80, NORTH, pci_e0, ...)      <- CLKREQ
 *     pinctrl-sm8250.c:1212  [81] = PINGROUP(81, NORTH, phase_flag, ...)  <- WAKE
 * So all five pads this area drives are in NORTH, and the 0x0 reads from WEST and SOUTH are simply
 * pins that do not exist in those tiles.  The census now prints the source tile beside the raw
 * three-tile reading, so a later reader sees both the authority and the state. */
constexpr int kPinTile[2] = {2, 2};   /* both pins are NORTH; index into kTile/kTileName */

/* ---- M2a: bring PCIe0 up from GCC alone -------------------------------------------------------
 *
 * The gate this stage exists for (spec §5 M2): PCIe0 arrives powered down (measured 2026-11-20,
 * gdscr=0x0022f001), and the question is whether the GDSC can be powered by GCC votes alone or needs
 * the vendor's rail vote first.  M2a answers that without mixing in the RPMh work:
 *
 *   1. vote the six PCIe0 branches (one register, 0x52008 bits 0..5) and the four single-bit enables;
 *   2. clear GDSCR.SW_COLLAPSE as a read-modify-write - a blind value would clear the GDSC's timing
 *      fields, which is the failure package/DbyGpuPowerProbe.cpp:5778 records for the GPU;
 *   3. poll GDSCR.PWR_ON for up to 2 ms;
 *   4. only when the GDSC reports power: release GCC_PCIE_0_BCR and GCC_PCIE_0_PHY_BCR.  Releasing a
 *      reset into an unpowered controller is the one ordering this stage refuses.
 *
 * If power is refused, every vote is written back to its snapshot, so the boot ends where it started
 * and the next stage (the rail vote) is the only new variable.
 *
 * Returns 0 powered and resets released, 2 power refused, -1 the GCC window did not map. */
int stage2a(uint32_t *gdscrAfter)
{
	Window gcc = {};
	if (!mapWindow(gcc, kGcc, kGccSpan)) return -1;

	char line[120];
	const uint32_t gdscr = rd(gcc.va, kGdscr);
	const uint32_t votesBefore = rd(gcc.va, kVotes);
	const uint32_t phyAuxBefore = rd(gcc.va, kPhyAux);
	const uint32_t refgenBefore = rd(gcc.va, kRefgen);
	const uint32_t clkrefBefore = rd(gcc.va, kClkref);
	const uint32_t tbuBefore = rd(gcc.va, kTbu);
	const uint32_t ddrssBefore = rd(gcc.va, kDdrssSfTbu);
	snprintf(line, sizeof(line), "[dby-pcie] m2 before gdscr=%08x votes=%08x clkref=%08x refgen=%08x",
	    gdscr, votesBefore, clkrefBefore, refgenBefore);
	row(line);

	/* One register carries all six branch votes (mainline gcc-sm8250.c:1615-1704); the other four are
	 * their own enable registers and halt in place. */
	wr(gcc.va, kVotes, votesBefore | 0x3Fu);
	wr(gcc.va, kPhyAux, phyAuxBefore | 1u);
	wr(gcc.va, kRefgen, refgenBefore | 1u);
	wr(gcc.va, kClkref, clkrefBefore | 1u);
	wr(gcc.va, kTbu, tbuBefore | 1u);
	wr(gcc.va, kDdrssSfTbu, ddrssBefore | 1u);
	snprintf(line, sizeof(line), "[dby-pcie] m2 voted votes=%08x clkref=%08x refgen=%08x tbu=%08x phyaux=%08x ddrss=%08x",
	    rd(gcc.va, kVotes), rd(gcc.va, kClkref), rd(gcc.va, kRefgen), rd(gcc.va, kTbu),
	    rd(gcc.va, kPhyAux), rd(gcc.va, kDdrssSfTbu));
	row(line);
	/* The halt registers again, after the votes: the snapshot row above is the "before", and §5's M2a
	 * verdict ("a halt that stays asserted names which leaf is still off") cannot be read from it. */
	snprintf(line, sizeof(line), "[dby-pcie] m2 halt %08x %08x %08x %08x %08x %08x",
	    rd(gcc.va, kHalts[0]), rd(gcc.va, kHalts[1]), rd(gcc.va, kHalts[2]),
	    rd(gcc.va, kHalts[3]), rd(gcc.va, kHalts[4]), rd(gcc.va, kHalts[5]));
	row(line);

	uint32_t now = gdscr;
	unsigned waited = 0;
	if ((gdscr & kGdscPwrOn) == 0) {
		wr(gcc.va, kGdscr, gdscr & ~kGdscSwCollapse);
		IODelay(1);
		for (;;) {
			now = rd(gcc.va, kGdscr);
			if ((now & kGdscPwrOn) != 0 || waited >= kGdscPollUs) break;
			IODelay(10);
			waited += 10;
		}
	} else {
		now = gdscr;
	}
	*gdscrAfter = now;
	const bool powered = (now & kGdscPwrOn) != 0;
	snprintf(line, sizeof(line), "[dby-pcie] m2 gdsc after=%08x pwron=%u waited=%uus",
	    now, powered ? 1u : 0u, waited);
	row(line);

	int rc = 0;
	if (!powered) {
		wr(gcc.va, kVotes, votesBefore);
		wr(gcc.va, kPhyAux, phyAuxBefore);
		wr(gcc.va, kRefgen, refgenBefore);
		wr(gcc.va, kClkref, clkrefBefore);
		wr(gcc.va, kTbu, tbuBefore);
		wr(gcc.va, kDdrssSfTbu, ddrssBefore);
		/* GDSCR goes back as well, and this is the point the review caught: restoring only the votes
		 * leaves SW_COLLAPSE clear, so the GDSC keeps asking for power on rails nobody voted for the
		 * rest of the boot - the opposite of "the boot ends where it started".  The snapshot value is
		 * written back verbatim, timing fields included. */
		wr(gcc.va, kGdscr, gdscr);
		rc = 2;
		row("[dby-pcie] m2 power refused, votes and gdscr restored, resets untouched (M2b: rail vote)");
	} else {
		wr(gcc.va, kPcieBcr, rd(gcc.va, kPcieBcr) & ~1u);
		wr(gcc.va, kPciePhyBcr, rd(gcc.va, kPciePhyBcr) & ~1u);
		snprintf(line, sizeof(line), "[dby-pcie] m2 resets bcr=%08x phybcr=%08x released",
		    rd(gcc.va, kPcieBcr), rd(gcc.va, kPciePhyBcr));
		row(line);
	}
	unmapWindow(gcc);
	return rc;
}

/* One pad, written in the order that avoids a glitch: the output level first (the pad is still an
 * input, so the level is only latched), then CFG with output-enable, then a read-back compared under
 * the mask of the fields this code owns.  package/DbyTouch/DbyTouch.cpp:1505-1521 is the same shape -
 * mask what you own, keep the rest, verify what came back - and the review of this stage asked for it
 * because a blanket CFG write with OE set first can drive a pad for the instants between the two
 * writes, which for an active-low PERST# is a reset pulse nobody asked for.
 *
 * Returns true when the read-back matches; on a mismatch `got` carries what the register holds. */
constexpr uint32_t kPadOwnedMask = 0x3FFu;   /* func [5:2], drive [8:6], pull [1:0], OE bit 9 */

bool padWrite(Window &tile, int pin, uint32_t cfg, uint32_t level, uint32_t *got)
{
	const uint32_t base = static_cast<uint32_t>(pin) * kPinStride;
	if (level != 0) wr(tile.va, base + kPinIn, kPadOut);
	else wr(tile.va, base + kPinIn, 0);
	wr(tile.va, base + kPinCfg, cfg);
	*got = rd(tile.va, base + kPinCfg);
	return (*got & kPadOwnedMask) == (cfg & kPadOwnedMask);
}

/* ---- M3: pads, PHY, LTSSM -----------------------------------------------------------------------
 *
 * M2a released the resets but the link cannot train without the PHY programming, so this stage is
 * three steps in the vendor's order (pci-msm's msm_pcie_enable): pads up (WLAN_EN high, then PERST#
 * released), the 99-write PHY sequence from this board's own tree, then LTSSM enabled with the link
 * polled on ELBI bit 10.  Only when the link reports up are the identification registers read - the
 * same guard M1 uses, because that is the access that hung alioth with the pipe clock off.
 *
 * Returns 0 when the link came up and the DBI ID was read, 3 when it did not within 100 ms (the pads
 * are then returned to their sleep levels so the tablet does not sit with a powered, unclocked
 * endpoint), -1 when a window did not map. */
int stage3a(uint32_t *dbiId, uint32_t *dllla, uint32_t *barSocHw)
{
	*dbiId = 0;
	*dllla = 0;
	*barSocHw = 0;

	Window north = {}, phy = {}, parf = {}, dbi = {}, gcc = {};
	char line[120];
	if (!mapWindow(north, kTile[kPinTile[0]], kTileSpan)) return -1;
	if (!mapWindow(gcc, kGcc, kGccSpan)) {unmapWindow(north); return -1;}
	if (!mapWindow(phy, kPhy, kPhySpan)) {unmapWindow(north); unmapWindow(gcc); return -1;}
	if (!mapWindow(parf, kParf, kParfSpan)) {unmapWindow(north); unmapWindow(phy); unmapWindow(gcc); return -1;}
	if (!mapWindow(dbi, kDbi, kDbiSpan)) {unmapWindow(north); unmapWindow(phy); unmapWindow(parf); unmapWindow(gcc); return -1;}

	/* ---- 1. WLAN_EN, the vendor's 1 ms, then the device tree's ep-latency --------------------
	 * The order is the vendor's (cnss2/power.c then pci-msm.c): the radio's enable goes high, the
	 * controller is brought up, and PERST# is released only after qcom,ep-latency = <10> ms has
	 * passed.  M3 released PERST# after 1 ms and the link stopped in LTSSM state 7 - config but
	 * never L0 - which is what this wait is for. */
	const uint32_t cfg20 = kPadFuncGpio | kPadDrive16 | kPadPullUp | kPadOutputEnable;
	const uint32_t cfg79 = kPadFuncGpio | kPadDrive2 | kPadPullDown | kPadOutputEnable;
	uint32_t got20 = 0, got79 = 0;
	if (!padWrite(north, 20, cfg20, kPadOut, &got20)) {
		snprintf(line, sizeof(line), "[dby-pcie] m3 pad20 mismatch want=%08x got=%08x",
		    cfg20 & kPadOwnedMask, got20 & kPadOwnedMask);
		row(line);
		unmapWindow(dbi); unmapWindow(parf); unmapWindow(phy); unmapWindow(north); unmapWindow(gcc);
		return -7;
	}
	IODelay(kWlanEnableDelayUs);       /* cnss2's WLAN_ENABLE_DELAY, 1000 us */
	IODelay(kEpLatencyUs);             /* the device tree's qcom,ep-latency, 10 ms */
	snprintf(line, sizeof(line), "[dby-pcie] m3 wlan_en high, ep-latency %uus waited", kEpLatencyUs);
	row(line);

	/* ---- 2. the controller's own initialisation --------------------------------------------- */
	/* GCC_PCIE_0_BCR is not re-toggled here: stage2a released it after the GDSC came up, and the
	 * same-SoC record's init block does not touch it either, so re-asserting it would add a variable
	 * this stage is not measuring. */
	wr(parf.va, kParfDeviceType, 4u);                       /* root complex */
	/* These two are GCC registers (gcc_pcie0_phy_refgen_clk_src's RCG), NOT PARF PHY registers:
	 * the same-SoC record writes them through its GCC window (UniXNU-alioth/package/alioth_pcie.c:777-778),
	 * and aiming them at the 4 KB PHY mapping would have stored ~0x6F018 bytes past it. */
	wr(gcc.va, kGccRefgenCfg, kGccRefgenCfgValue);          /* 100 MHz reference */
	wr(gcc.va, kGccRefgenRootEn, kGccRefgenRootEnValue);
	IODelay(2000);
	wr(parf.va, kParfPhyCtrlReg, rd(parf.va, kParfPhyCtrlReg) & ~1u);
	wr(parf.va, kParfPhyRefclkReg, rd(parf.va, kParfPhyRefclkReg) & ~(1u << 12));
	wr(parf.va, kParfPmCtrl, rd(parf.va, kParfPmCtrl) & ~(1u << 5));
	wr(parf.va, kParfDbiBase, 0u);
	const uint32_t dbiBase = rd(parf.va, kParfDbiBase);
	wr(parf.va, kParfSysCtrl, (rd(parf.va, kParfSysCtrl) & ~(1u << 29)) | (1u << 6) | (1u << 1));
	wr(parf.va, kParfSlvAddrSpace, kParfSlvAddrSpaceValue);
	wr(parf.va, kParfMhiClkReset, rd(parf.va, kParfMhiClkReset) | (1u << 4));
	wr(parf.va, kParfAxiMstrWrHalt, rd(parf.va, kParfAxiMstrWrHalt) | (1u << 31));
	wr(parf.va, kParfIntAllMask, 0u);
	wr(parf.va, kParfIntAllClear, 0xffffffffu);
	snprintf(line, sizeof(line), "[dby-pcie] m3 parf dev=%x sys=%08x refclk=%08x dbi_base=%08x slv=%08x",
	    rd(parf.va, kParfDeviceType), rd(parf.va, kParfSysCtrl), rd(parf.va, kParfPhyRefclkReg),
	    dbiBase, rd(parf.va, kParfSlvAddrSpace));
	row(line);
	if (dbiBase != 0) {
		/* The same-SoC record stops here for a reason (alioth_pcie.c:807-815): with a non-zero base
		 * in 0x350 the DBI window is not aimed at the core's own 4 KB, so every later DBI access is an
		 * unaimed read that no timeout can catch.  Restore the pads and say which offset is real. */
		uint32_t got = 0;
		(void)padWrite(north, 20, kPadFuncGpio | kPadDrive2 | kPadPullDown | kPadOutputEnable, 0, &got);
		snprintf(line, sizeof(line), "[dby-pcie] m3 dbi_base did not take zero (reads %08x), training skipped",
		    dbiBase);
		row(line);
		unmapWindow(dbi); unmapWindow(parf); unmapWindow(phy); unmapWindow(north); unmapWindow(gcc);
		return 4;
	}

	/* ---- 3. the PHY sequence, verbatim from the device tree -------------------------------- */
	for (unsigned i = 0; i < kDbyPciePhySeqCount; i++) {
		wr(phy.va, kDbyPciePhySeq[i].offset, kDbyPciePhySeq[i].value);
		if (kDbyPciePhySeq[i].delay != 0) IODelay(kDbyPciePhySeq[i].delay);
	}
	snprintf(line, sizeof(line), "[dby-pcie] m3 phy count=%u first=%03x:%02x last=%03x:%02x",
	    kDbyPciePhySeqCount, kDbyPciePhySeq[0].offset, kDbyPciePhySeq[0].value,
	    kDbyPciePhySeq[kDbyPciePhySeqCount - 1].offset, kDbyPciePhySeq[kDbyPciePhySeqCount - 1].value);
	row(line);

	/* ---- 4. PERST# released, 2 ms ---------------------------------------------------------- */
	if (!padWrite(north, 79, cfg79, kPadOut, &got79)) {
		snprintf(line, sizeof(line), "[dby-pcie] m3 pad79 mismatch want=%08x got=%08x",
		    cfg79 & kPadOwnedMask, got79 & kPadOwnedMask);
		row(line);
		(void)padWrite(north, 79, cfg79, 0, &got79);
		(void)padWrite(north, 20, kPadFuncGpio | kPadDrive2 | kPadPullDown | kPadOutputEnable, 0, &got20);
		unmapWindow(dbi); unmapWindow(parf); unmapWindow(phy); unmapWindow(north); unmapWindow(gcc);
		return -7;
	}
	IODelay(kPerstDelayUs);
	snprintf(line, sizeof(line), "[dby-pcie] m3 pads gpio20=%08x/%08x gpio79=%08x/%08x",
	    rd(north.va, 20u * kPinStride + kPinCfg), rd(north.va, 20u * kPinStride + kPinIn),
	    rd(north.va, 79u * kPinStride + kPinCfg), rd(north.va, 79u * kPinStride + kPinIn));
	row(line);

	/* ---- 5. the vendor's DBI training words, then LTSSM ------------------------------------ */
	uint32_t value = rd(dbi.va, kDbiGen3Gen2Ctrl);
	wr(dbi.va, kDbiGen3Gen2Ctrl, (value & ~0x1F00u) | (1u << 8));
	value = rd(dbi.va, kDbiGen3EqControl);
	wr(dbi.va, kDbiGen3EqControl, value | 0x20u);
	value = rd(dbi.va, kDbiGen3Related);
	wr(dbi.va, kDbiGen3Related, value & ~1u);
	value = rd(dbi.va, kDbiGen3MiscControl);
	wr(dbi.va, kDbiGen3MiscControl, value | 1u);
	wr(dbi.va, kDbiSpcieCap, kDbiCorePreset);
	wr(dbi.va, kDbiGen3MiscControl, value & ~1u);
	value = rd(dbi.va, kDbiDeviceControlStatus);
	wr(dbi.va, kDbiDeviceControlStatus, (value & ~0x7000u) | (kDbiTlpRdSize << 12));
	snprintf(line, sizeof(line), "[dby-pcie] m3 dbi gen2=%08x eq=%08x misc=%08x devctl=%08x",
	    rd(dbi.va, kDbiGen3Gen2Ctrl), rd(dbi.va, kDbiGen3EqControl),
	    rd(dbi.va, kDbiGen3MiscControl), rd(dbi.va, kDbiDeviceControlStatus));
	row(line);

	/* Ask for Gen1 before the link trains, and say what the register held first: a port already at
	 * 0x00010001 needs no change and that reading is itself the answer to "was this the difference". */
	const uint32_t lnkctl2Before = rd(dbi.va, kDbiLinkControl2);
	if ((lnkctl2Before & 0xFu) != kLinkSpeedGen1) {
		wr(dbi.va, kDbiLinkControl2, (lnkctl2Before & ~0xFu) | kLinkSpeedGen1);
	}
	snprintf(line, sizeof(line), "[dby-pcie] m3 lnkctl2 %08x -> %08x (target Gen1)",
	    lnkctl2Before, rd(dbi.va, kDbiLinkControl2));
	row(line);

	const uint32_t ltssmBefore = rd(parf.va, kParfLtssm);
	wr(parf.va, kParfLtssm, ltssmBefore | (1u << 8));
	unsigned waited = 0;
	uint32_t elbi = rd(dbi.va, kElbiStatus);
	uint32_t id = 0, dll = 0;
	bool up = false;
	for (;;) {
		elbi = rd(dbi.va, kElbiStatus);
		/* ELBI first, and the DBI reads only once it reports XMLH_LINK_UP: this file's own guard two
		 * places up says a DBI access with the pipe clock off is what hung alioth, and no poll bound
		 * can rescue a read that never returns. */
		if ((elbi & kLinkUpBit) != 0) {
			dll = rd(dbi.va, kDbiLinkStatus);
			id = rd(dbi.va, kDbiId);
			/* The vendor's three checks, pci-msm.c:4416-4424: ELBI link-up, DBI DLLLA, and a real
			 * vendor/device word rather than the all-ones a silent bus returns. */
			up = (dll & kDlllaBit) != 0 && id != 0xFFFFFFFFu && id != 0;
		}
		if (up || waited >= kLinkPollUs) break;
		IODelay(kLinkPollStepUs);
		waited += kLinkPollStepUs;
	}
	snprintf(line, sizeof(line), "[dby-pcie] m3 ltssm %x->%x elbi=%08x dll=%08x id=%08x up=%u spd=%u waited=%uus",
	    ltssmBefore, rd(parf.va, kParfLtssm), elbi, dll, id, up ? 1u : 0u,
	    (dll >> kDbiLinkSpeedShift) & 0xFu, waited);
	row(line);

	int rc = 3;
	if (up) {
		*dbiId = id;
		*dllla = (dll & kDlllaBit) ? 1u : 0u;
		Window bar = {};
		if (mapWindow(bar, 0x60300000u, 0x1000)) {
			*barSocHw = rd(bar.va, 0x224);
			unmapWindow(bar);
		}
		snprintf(line, sizeof(line), "[dby-pcie] m3 dbi id=%08x dllla=%u bar0_soc_hw=%08x major=%u",
		    *dbiId, *dllla, *barSocHw, (*barSocHw >> 8) & 0xFu);
		row(line);
		rc = 0;
	} else {
		uint32_t got = 0;
		(void)padWrite(north, 79, cfg79, 0, &got);
		(void)padWrite(north, 20, kPadFuncGpio | kPadDrive2 | kPadPullDown | kPadOutputEnable, 0, &got);
		wr(parf.va, kParfLtssm, ltssmBefore);
		snprintf(line, sizeof(line), "[dby-pcie] m3 no link in 100 ms, pads low and ltssm=%x restored",
		    rd(parf.va, kParfLtssm));
		row(line);
	}
	unmapWindow(dbi);
	unmapWindow(parf);
	unmapWindow(phy);
	unmapWindow(north);
	unmapWindow(gcc);
	return rc;
}


/* ---- M4: program the outbound windows and identify the endpoint ---------------------------------
 *
 * M3b proved the link trains, but every read past the link still returned all-ones because the address
 * translator had never been programmed: BAR0 at 0x60300000 was not aimed at anything and the DBI ID
 * read 0x010b17cb, the DWC core's own pre-configuration ID, not the endpoint's.  This stage programs
 * two outbound regions - the memory window that covers BAR0 and a CFG0 region that turns the conf
 * window (0x60100000) into the endpoint's configuration space - and then reads what only a real
 * endpoint can answer: the vendor/device word (17cb:1101 for a QCA6390), its class code, and BAR0's
 * TCSR_SOC_HW_VERSION, whose major field must be 2 for ath11k.
 *
 * Returns 0 when the config space answered with a real vendor id, 5 when it stayed all-ones (the
 * window or the endpoint is not answering), -1 when a window did not map. */
/* Program one outbound region in whichever layout answered.  `viewport` selects DBI+0x900 (one region
 * at a time, selected by the index register) or the unrolled block at 0x60001000 (index << 9).  In
 * both layouts: CTRL2 = 0 first (bit 31 is the enable), then base/limit/target, then the type, then
 * enable, and LIMIT is the region's last address, not its size. */
void atuProgram(Window &atu, Window &dbi, bool viewport, uint32_t index, uint32_t type,
    uint32_t hostBase, uint32_t hostSpan, uint32_t target)
{
	const uint32_t base = viewport ? 0 : index * kAtuRegionStride;
	volatile uint32_t *w = viewport ? dbi.va : atu.va;
	const uint32_t off = viewport ? kAtuViewport : 0;
	const uint32_t ctrl1 = viewport ? kAtuVpCtrl1 : kAtuCtrl1;
	const uint32_t ctrl2 = viewport ? kAtuVpCtrl2 : kAtuCtrl2;
	const uint32_t lowerBase = viewport ? kAtuVpLowerBase : kAtuLowerBase;
	const uint32_t upperBase = viewport ? kAtuVpUpperBase : kAtuUpperBase;
	const uint32_t limit = viewport ? kAtuVpLimit : kAtuLimit;
	const uint32_t lowerTarget = viewport ? kAtuVpLowerTarget : kAtuLowerTarget;
	const uint32_t upperTarget = viewport ? kAtuVpUpperTarget : kAtuUpperTarget;

	if (viewport) {
		wr(w, off, index);            /* the viewport selects the region */
		wr(w, ctrl2, 0);
		wr(w, ctrl1, 0);
		wr(w, lowerBase, hostBase);
		wr(w, upperBase, 0);
		wr(w, limit, hostBase + hostSpan - 1);
		wr(w, lowerTarget, target);
		wr(w, upperTarget, 0);
		wr(w, ctrl1, type);
		wr(w, ctrl2, kAtuEnable);
		return;
	}
	wr(w, base + kAtuCtrl2, 0);
	wr(w, base + kAtuCtrl1, 0);
	wr(w, base + kAtuLowerBase, hostBase);
	wr(w, base + kAtuUpperBase, 0);
	wr(w, base + kAtuLimit, hostBase + hostSpan - 1);
	wr(w, base + kAtuLowerTarget, target);
	wr(w, base + kAtuUpperTarget, 0);
	wr(w, base + kAtuCtrl1, type);
	wr(w, base + kAtuCtrl2, kAtuEnable);
}

/* Whether the rails and clocks were actually voted before the chip was enabled; the probe's entry point
 * prints a row when they were not, because the boot continues either way. */
bool gPowerVoted;

/* Which iATU layout answered in M4: false = the unrolled block at 0x60001000, true = the one-region
 * viewport at DBI+0x900.  The doorbell's inbound window has to be written in the same layout. */
bool gAtuViewport;

/* Program one INBOUND region: the window the endpoint uses when IT reads host memory.  The only caller
 * passes viewport=false, because the direction bit that selects INBOUND in the viewport layout has never
 * been exercised on this core (see the block in bhiHandoff); the viewport branch is kept so the two
 * layouts stay side by side, not because it is trusted.  Same register
 * order as atuProgram(), but LOWER_BASE/LIMIT are the PCI (device-side) address range and
 * LOWER_TARGET is the system address it is translated to.  Identity here on purpose: the address the
 * device is handed in BHI_IMGADDR is the arena's physical address, so the two are the same number and
 * a wrong mapping cannot hide behind a second translation.  Returns true when the region reads back
 * enabled with the target it was given. */
bool atuProgramInbound(Window &atu, Window &dbi, bool viewport, uint32_t index, uint32_t pciBase,
    uint32_t span, uint32_t sysBase)
{
	if (viewport) {
		/* PCIE_ATU_REGION_INBOUND is bit 31 of the viewport index (mainline pcie-designware.h). */
		wr(dbi.va, kAtuViewport, index | 0x80000000u);
		wr(dbi.va, kAtuVpCtrl2, 0);
		wr(dbi.va, kAtuVpCtrl1, 0);
		wr(dbi.va, kAtuVpLowerBase, pciBase);
		wr(dbi.va, kAtuVpUpperBase, 0);
		wr(dbi.va, kAtuVpLimit, pciBase + span - 1);
		wr(dbi.va, kAtuVpLowerTarget, sysBase);
		wr(dbi.va, kAtuVpUpperTarget, 0);
		wr(dbi.va, kAtuVpCtrl1, kAtuTypeMem);
		wr(dbi.va, kAtuVpCtrl2, kAtuEnable);
		return (rd(dbi.va, kAtuVpCtrl2) & kAtuEnable) != 0 && rd(dbi.va, kAtuVpLowerTarget) == sysBase;
	}
	const uint32_t base = kAtuInboundOffset + index * kAtuRegionStride;
	wr(atu.va, base + kAtuCtrl2, 0);
	wr(atu.va, base + kAtuCtrl1, 0);
	wr(atu.va, base + kAtuLowerBase, pciBase);
	wr(atu.va, base + kAtuUpperBase, 0);
	wr(atu.va, base + kAtuLimit, pciBase + span - 1);
	wr(atu.va, base + kAtuLowerTarget, sysBase);
	wr(atu.va, base + kAtuUpperTarget, 0);
	wr(atu.va, base + kAtuCtrl1, kAtuTypeMem);
	wr(atu.va, base + kAtuCtrl2, kAtuEnable);
	return (rd(atu.va, base + kAtuCtrl2) & kAtuEnable) != 0 && rd(atu.va, base + kAtuLowerTarget) == sysBase;
}

int stage4a(uint32_t *cfgId, uint32_t *cfgClass, uint32_t *socHw)
{
	*cfgId = 0;
	*cfgClass = 0;
	*socHw = 0;

	Window atu = {}, mem = {}, conf = {}, parf = {}, dbi = {};
	char line[120];
	if (!mapWindow(atu, kAtuWindow, kAtuWindowSpan)) return -1;
	if (!mapWindow(parf, kParf, kParfSpan)) {unmapWindow(atu); return -1;}
	if (!mapWindow(dbi, kDbi, kDbiSpan)) {unmapWindow(atu); unmapWindow(parf); return -1;}
	if (!mapWindow(conf, kConfWindow, kConfWindowSpan)) {
		unmapWindow(atu); unmapWindow(parf); unmapWindow(dbi); return -1;
	}
	if (!mapWindow(mem, kMemWindow, kMemWindowSpan)) {
		unmapWindow(atu); unmapWindow(parf); unmapWindow(dbi); unmapWindow(conf); return -1;
	}

	/* 1. Tell the core where its iATU CSRs live (mainline qcom_pcie_configure_dbi_atu_base(),
	 * pcie-qcom.c:446-459); nothing on this boot has written it. */
	wr(parf.va, kParfAtuBase, static_cast<uint32_t>(kAtuWindow & 0xFFFFFFFFu));
	wr(parf.va, kParfAtuBaseHi, static_cast<uint32_t>(kAtuWindow >> 32));

	/* 2. Which layout answers.  The vendor's own test reads the viewport register and treats
	 * 0xffffffff as "unrolled", but on this board the unrolled window itself reads 0xffffffff, so
	 * the probe below is empirical: program a region each way and see which one reads back. */
	const uint32_t vpProbe = rd(dbi.va, kAtuViewport);
	const uint32_t unrolledProbe = rd(atu.va, kAtuCtrl1);
	snprintf(line, sizeof(line), "[dby-pcie] m4 atu probe vp=%08x unrolled=%08x", vpProbe, unrolledProbe);
	row(line);

	const uint32_t memBase = static_cast<uint32_t>(kMemWindow & 0xFFFFFFFFu);
	const uint32_t memSpan = static_cast<uint32_t>(kMemWindowSpan);
	const uint32_t confBase = static_cast<uint32_t>(kConfWindow & 0xFFFFFFFFu);
	const uint32_t confSpan = static_cast<uint32_t>(kConfWindowSpan);

	/* Region 0 = the memory window.  Try the viewport layout first: it is the one that answered on
	 * the same-SoC board, and the unrolled block is the one this board just read as all ones. */
	atuProgram(atu, dbi, true, 0, kAtuTypeMem, memBase, memSpan, memBase);
	bool viewport = rd(dbi.va, kAtuVpLowerBase) == memBase && (rd(dbi.va, kAtuVpCtrl2) & kAtuEnable) != 0;
	if (!viewport) {
		atuProgram(atu, dbi, false, 0, kAtuTypeMem, memBase, memSpan, memBase);
	}
	const bool unrolled = !viewport && rd(atu.va, kAtuLowerBase) == memBase && (rd(atu.va, kAtuCtrl2) & kAtuEnable) != 0;
	snprintf(line, sizeof(line), "[dby-pcie] m4 atu layout vp=%u unrolled=%u", viewport ? 1u : 0u,
	    unrolled ? 1u : 0u);
	row(line);

	/* 2b. Neither layout answered, which on this board is a real possibility: the unrolled window is
	 * the device tree's `iatu` and it reads all ones, and DBI+0x900 reads all ones too.  Mainline's
	 * qcom_pcie_configure_dbi_atu_base() writes four PARF registers for this IP generation
	 * (pcie-qcom.c:440-465) - DBI_BASE_ADDR_V2/_HI with the DBI's physical address and
	 * SLV_ADDR_SPACE_SIZE_V2/_HI - and this boot has only ever written 0x350 = 0, because that is
	 * what the same-SoC record found made ELBI work.  Attempt C therefore re-points the pair the way
	 * mainline does and retries both layouts, restoring the previous values if it still fails. */
	bool vpC = false, unrolledC = false;
	if (!viewport && !unrolled) {
		const uint32_t dbiBefore = rd(parf.va, kParfDbiBase);
		const uint32_t dbiHiBefore = rd(parf.va, kParfDbiBaseHi);
		const uint32_t slvBefore = rd(parf.va, kParfSlvSize);
		const uint32_t slvHiBefore = rd(parf.va, kParfSlvSizeHi);
		wr(parf.va, kParfDbiBase, static_cast<uint32_t>(kDbi & 0xFFFFFFFFu));
		wr(parf.va, kParfDbiBaseHi, 0);
		wr(parf.va, kParfSlvSize, 0);
		wr(parf.va, kParfSlvSizeHi, kParfSlvSizeValueHi);
		snprintf(line, sizeof(line), "[dby-pcie] m4 atu retry dbi_base=%08x slv=%08x/%08x",
		    rd(parf.va, kParfDbiBase), rd(parf.va, kParfSlvSize), rd(parf.va, kParfSlvSizeHi));
		row(line);
		atuProgram(atu, dbi, true, 0, kAtuTypeMem, memBase, memSpan, memBase);
		vpC = rd(dbi.va, kAtuVpLowerBase) == memBase && (rd(dbi.va, kAtuVpCtrl2) & kAtuEnable) != 0;
		if (!vpC) {
			atuProgram(atu, dbi, false, 0, kAtuTypeMem, memBase, memSpan, memBase);
			unrolledC = rd(atu.va, kAtuLowerBase) == memBase && (rd(atu.va, kAtuCtrl2) & kAtuEnable) != 0;
		}
		snprintf(line, sizeof(line), "[dby-pcie] m4 atu retry layout vp=%u unrolled=%u",
		    vpC ? 1u : 0u, unrolledC ? 1u : 0u);
		row(line);
		if (!vpC && !unrolledC) {
			wr(parf.va, kParfDbiBase, dbiBefore);
			wr(parf.va, kParfDbiBaseHi, dbiHiBefore);
			wr(parf.va, kParfSlvSize, slvBefore);
			wr(parf.va, kParfSlvSizeHi, slvHiBefore);
			row("[dby-pcie] m4 no translator layout answered, config read skipped");
			unmapWindow(mem); unmapWindow(conf); unmapWindow(dbi); unmapWindow(parf); unmapWindow(atu);
			return 5;
		}
	}
	const bool useViewport = viewport || vpC;
	gAtuViewport = useViewport;
	snprintf(line, sizeof(line), "[dby-pcie] m4 atu mem ctrl2=%08x base=%08x limit=%08x",
	    useViewport ? rd(dbi.va, kAtuVpCtrl2) : rd(atu.va, kAtuCtrl2),
	    useViewport ? rd(dbi.va, kAtuVpLowerBase) : rd(atu.va, kAtuLowerBase),
	    useViewport ? rd(dbi.va, kAtuVpLimit) : rd(atu.va, kAtuLimit));
	row(line);

	/* Region 1 = CFG0 for bus 1 device 0 function 0 at the conf window. */
	atuProgram(atu, dbi, useViewport, 1, kAtuTypeCfg0, confBase, confSpan, kEndpointBusDevFn);
	const uint32_t cfgCtrl2 = useViewport ? rd(dbi.va, kAtuVpCtrl2) : rd(atu.va, 1 * kAtuRegionStride + kAtuCtrl2);
	snprintf(line, sizeof(line), "[dby-pcie] m4 atu cfg ctrl2=%08x base=%08x limit=%08x",
	    cfgCtrl2, useViewport ? rd(dbi.va, kAtuVpLowerBase) : rd(atu.va, 1 * kAtuRegionStride + kAtuLowerBase),
	    useViewport ? rd(dbi.va, kAtuVpLimit) : rd(atu.va, 1 * kAtuRegionStride + kAtuLimit));
	row(line);
	if ((cfgCtrl2 & kAtuEnable) == 0) {
		row("[dby-pcie] m4 the cfg window did not enable, config read skipped");
		unmapWindow(mem); unmapWindow(conf); unmapWindow(dbi); unmapWindow(parf); unmapWindow(atu);
		return 5;
	}

	/* 3. The root complex's own type-0 header.  Without a command word and bus numbers the
	 * translator has nothing to translate, whatever the windows say.  DBI 0x8BC bit 0 unlocks the
	 * write and is put back; 0x18 keeps its top byte, as the vendor's dw_pcie_setup_rc does. */
	const uint32_t miscBefore = rd(dbi.va, kDbiGen3MiscControl);
	wr(dbi.va, kDbiGen3MiscControl, miscBefore | 1u);
	const uint32_t rcCmd = rd(dbi.va, kDbiCommandStatus);
	wr(dbi.va, kDbiCommandStatus, (rcCmd & 0xFFFF0000u) | kDbiCommandMemIoBm);
	const uint32_t rcBus = rd(dbi.va, kDbiBusNumbers);
	wr(dbi.va, kDbiBusNumbers, (rcBus & 0xFF000000u) | kDbiBusNumbersValue);
	wr(dbi.va, kDbiGen3MiscControl, miscBefore);
	snprintf(line, sizeof(line), "[dby-pcie] m4 rc hdr cmd=%08x busn=%08x",
	    rd(dbi.va, kDbiCommandStatus), rd(dbi.va, kDbiBusNumbers));
	row(line);

	/* 4. The endpoint's own header through the aimed CFG0 window: BAR0 has to name the memory
	 * window's base and the command register has to allow memory cycles before BAR0 means anything. */
	const uint32_t epCmdBefore = rd(conf.va, kCfgCommand);
	const uint32_t epBar0Before = rd(conf.va, kCfgBar0);
	if (epBar0Before != kCfgBar0Value) wr(conf.va, kCfgBar0, kCfgBar0Value);
	if ((epCmdBefore & kCfgCommandMemMaster) != kCfgCommandMemMaster)
		wr(conf.va, kCfgCommand, epCmdBefore | kCfgCommandMemMaster);
	/* How big is BAR0?  The next stage needs a register at offset 0x3008 inside it (the SoC global reset
	 * ath11k performs before MHI), and two boots died on that access without printing anything - which is
	 * what an access outside a BAR's decode does.  Measured the standard way: write all ones, read the
	 * mask back, restore.  Read-only in effect: the original value is written back immediately. */
	const uint32_t bar0Before = rd(conf.va, kCfgBar0);
	wr(conf.va, kCfgBar0, 0xFFFFFFFFu);
	const uint32_t bar0Mask = rd(conf.va, kCfgBar0);
	wr(conf.va, kCfgBar0, bar0Before);
	const uint32_t bar0Size = (~(bar0Mask & ~0xFu) + 1u) & ~0xFu;
	snprintf(line, sizeof(line), "[dby-pcie] m4 bar0 size=%08x (mask %08x, restored %08x)", bar0Size,
	    bar0Mask, rd(conf.va, kCfgBar0));
	row(line);

	*cfgId = rd(conf.va, 0x00);
	*cfgClass = rd(conf.va, 0x08);
	snprintf(line, sizeof(line), "[dby-pcie] m4 ep cmd=%04x->%04x bar0=%08x->%08x id=%08x",
	    epCmdBefore & 0xFFFFu, rd(conf.va, kCfgCommand) & 0xFFFFu, epBar0Before, rd(conf.va, kCfgBar0), *cfgId);
	row(line);

	/* 5. BAR0's TCSR_SOC_HW_VERSION, now that the memory window and BAR0 agree. */
	*socHw = rd(mem.va, 0x224);
	snprintf(line, sizeof(line), "[dby-pcie] m4 cfg id=%08x class=%08x soc_hw=%08x major=%u",
	    *cfgId, *cfgClass, *socHw, (*socHw >> 8) & 0xFu);
	row(line);

	const uint32_t vendor = *cfgId & 0xFFFFu;
	int rc = 5;
	if (vendor == 0x17CBu) rc = 0;
	else {
		snprintf(line, sizeof(line), "[dby-pcie] m4 no config answer (vendor %04x), windows left as programmed",
		    vendor);
		row(line);
	}
	unmapWindow(mem);
	unmapWindow(conf);
	unmapWindow(dbi);
	unmapWindow(parf);
	unmapWindow(atu);
	return rc;
}


/* ---- M5: the endpoint's SMMU stream and its DMA arena --------------------------------------------
 *
 * The endpoint now speaks PCIe (M4 read its configuration space), but any DMA it does still has to pass
 * the applications SMMU, and that SMMU does not know SID 0x1c01 yet: the stream falls through to
 * whatever default the firmware left, which is a fault rather than a pass-through.  This stage maps the
 * stream the way this board already maps the audio and touch streams - through package/SmmuBypass.h,
 * which encodes the one thing a hand-written copy would get wrong: on this firmware a TYPE=BYPASS S2CR
 * reads back as FAULT, so the stream is pointed at the last context bank (left in S1_TRANS_S2_BYPASS by
 * the firmware) with TYPE=TRANSLATE.  It also allocates the 32 MiB physically contiguous arena the MHI
 * rings will come out of, because an arena that is not contiguous is not usable for that.
 *
 * Returns 0 when the stream is mapped and the arena exists, 6 when the SMMU refused, -9 when the arena
 * could not be allocated. */
IOBufferMemoryDescriptor *gDmaArena;
uint64_t gDmaArenaPa;
uint32_t gDmaArenaBytes;
unsigned gSmmuSlot = 0xFFFFFFFFu;
unsigned gSmmuBank;
uint32_t gSmmuSmr, gSmmuS2cr;

int stage5a(uint32_t *slot, uint32_t *bank, uint32_t *smr, uint32_t *s2cr, uint64_t *arenaPa, uint32_t *arenaBytes)
{
	*slot = 0xFFFFFFFFu; *bank = 0; *smr = 0; *s2cr = 0; *arenaPa = 0; *arenaBytes = 0;
	char line[120];

	/* 1. the arena: physically contiguous, 4 KiB aligned by the mask, retained for the rest of the boot. */
	if (!gDmaArena) {
		/* Ask for memory BELOW 4 GiB first.  Every arena so far landed above it (0x145628000,
		 * 0x14a400000, 0x153428000 ...) and the boot host interface never answered a doorbell, which is
		 * also what a device that cannot fetch from a 36-bit address would look like.  A 32-bit-bounded
		 * allocation turns that hypothesis into a reading: if BHI completes with the image low, the
		 * address width is the cause; if it behaves the same, the cause is elsewhere and the higher
		 * allocation is restored on the next run. */
		gDmaArena = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous,
		    kDmaArenaBytes, 0xFFFFFFFFull);
		if (!gDmaArena || gDmaArena->prepare() != kIOReturnSuccess) {
			/* Leave nothing armed behind: gDone makes this probe one-shot, so a descriptor left in the
			 * static after a failed prepare() could never be completed or released by anything. */
			if (gDmaArena) {
				/* prepare() failed, so there is no preparation to complete - releasing the descriptor is
				 * what keeps the static from holding an unusable arena for the rest of the boot. */
				gDmaArena->release();
				gDmaArena = nullptr;
			}
			row("[dby-pcie] m5 dma arena allocation failed");
			return -9;
		}
		gDmaArenaPa = gDmaArena->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
		gDmaArenaBytes = static_cast<uint32_t>(gDmaArena->getLength());
	}
	*arenaPa = gDmaArenaPa;
	*arenaBytes = gDmaArenaBytes;
	snprintf(line, sizeof(line), "[dby-pcie] m5 arena pa=%llx size=%u %s", gDmaArenaPa, gDmaArenaBytes,
	    (gDmaArenaPa >> 32) ? "(above 4 GiB)" : "(below 4 GiB)");
	row(line);
	if (!gDmaArenaPa) return -9;

	/* 2. the stream.  The planner needs a read/write view of the SMMU's GR0 block; the callbacks are the
	 * same shape package/DbyAudio.cpp uses (smmuRd/smmuWr), local here because the audio ones are static. */
	Window smmu = {};
	if (!mapWindow(smmu, kAppsSmmu, kAppsSmmuSpan)) {
		row("[dby-pcie] m5 smmu map failed");
		return -9;
	}
	struct LocalSmmu {volatile uint8_t *va;};
	LocalSmmu view = {reinterpret_cast<volatile uint8_t *>(smmu.va)};
	struct smmu_io io = {};
	io.ctx = &view;
	io.rd = [](void *ctx, unsigned at) -> uint32_t {
		return *reinterpret_cast<volatile uint32_t *>(static_cast<LocalSmmu *>(ctx)->va + at);
	};
	io.wr = [](void *ctx, unsigned at, uint32_t value) {
		*reinterpret_cast<volatile uint32_t *>(static_cast<LocalSmmu *>(ctx)->va + at) = value;
		__asm__ volatile("dsb sy" ::: "memory");
	};

	struct smmu_plan plan = smmu_plan_bypass(&io, kPcieSid, gDmaArenaPa, gDmaArenaBytes);
	const char *error = plan.error;
	if (!error) {
		error = smmu_configure_bypass(&io, &plan, kPcieSid);
		/* Latch and print only what the configure call accepted: a slot row that looks healthy after a
		 * refused write is the shape of evidence this project has been burned by.  The failure branch
		 * below still prints the slot and the two words, so a refusal is readable rather than silent. */
		if (!error && plan.slot <= 127) {
			gSmmuSlot = plan.slot;
			gSmmuBank = plan.bank;
			gSmmuSmr = io.rd(io.ctx, 0x800 + 4 * plan.slot);
			gSmmuS2cr = io.rd(io.ctx, 0xc00 + 4 * plan.slot);
			*slot = plan.slot; *bank = plan.bank; *smr = gSmmuSmr; *s2cr = gSmmuS2cr;
			snprintf(line, sizeof(line), "[dby-pcie] m5 smmu sid=%04x slot=%u bank=%u smr=%08x s2cr=%08x t=%u",
			    kPcieSid, gSmmuSlot, gSmmuBank, gSmmuSmr, gSmmuS2cr, (gSmmuS2cr >> 16) & 3u);
			row(line);
		} else {
			snprintf(line, sizeof(line), "[dby-pcie] m5 smmu %s slot=%u bank=%u smr=%08x s2cr=%08x",
			    error ? error : "no free stream-match group", plan.slot, plan.bank,
			    plan.slot <= 127 ? io.rd(io.ctx, 0x800 + 4 * plan.slot) : 0u,
			    plan.slot <= 127 ? io.rd(io.ctx, 0xc00 + 4 * plan.slot) : 0u);
			row(line);
		}
	} else {
		snprintf(line, sizeof(line), "[dby-pcie] m5 smmu %s", error);
		row(line);
	}
	unmapWindow(smmu);
	return error ? 6 : 0;
}



/* ---- M6a: the MHI register block, read before anything is downloaded ------------------------------
 *
 * M4 proved BAR0 answers at 0x224 (TCSR_SOC_HW_VERSION, major 2).  The next gate is the endpoint's MHI
 * register block, which sits at the start of that same BAR0 and is what the firmware download will be
 * driven through.  This stage reads it and nothing else: the register-space length, the two boot-host
 * interface offsets, MHI_CTRL and MHI_STATUS decoded into the state and execution-environment fields.
 * A device that has not been given firmware should read state PBL (0) with READY clear, which is the
 * baseline M6b has to move away from; a SYSERR bit or an implausible register length means the block
 * is not what we think it is, and is worth knowing before any download code exists.
 *
 * Returns 0 when the block looks like MHI, 7 when it does not, -1 when the window did not map. */
int stage6a(uint32_t *regLen, uint32_t *bhiOff, uint32_t *bhieOff, uint32_t *status)
{
	*regLen = 0; *bhiOff = 0; *bhieOff = 0; *status = 0;
	Window bar = {};
	if (!mapWindow(bar, kMemWindow, kMhiWindowSpan)) return -1;

	const uint32_t reglen = rd(bar.va, kMhiRegLen);
	const uint32_t bhi = rd(bar.va, kMhiBhiOff);
	const uint32_t bhie = rd(bar.va, kMhiBhieOff);
	const uint32_t ctrl = rd(bar.va, kMhiCtrl);
	const uint32_t sts = rd(bar.va, kMhiStatus);
	*regLen = reglen; *bhiOff = bhi; *bhieOff = bhie; *status = sts;

	char line[120];
	snprintf(line, sizeof(line), "[dby-pcie] m6 mhi reglen=%08x bhi=%08x bhie=%08x ctrl=%08x sts=%08x",
	    reglen, bhi, bhie, ctrl, sts);
	row(line);
	snprintf(line, sizeof(line), "[dby-pcie] m6 mhi state=%u ee=%u ready=%u syserr=%u",
	    (sts >> kMhiStateShift) & 0xFFu, (ctrl >> kMhiStateShift) & 0xFFu,
	    (sts & kMhiStatusReady) ? 1u : 0u, (sts & kMhiStatusSysErr) ? 1u : 0u);
	row(line);
	unmapWindow(bar);

	/* A register-space length of zero or one page's worth of ones is the "nothing here" shape; the
	 * MHI block declares a small length (0x100..0x1000 on these parts). */
	const bool plausible = reglen != 0 && reglen != 0xFFFFFFFFu && reglen <= 0x1000u &&
	    (sts & kMhiStatusSysErr) == 0;
	if (!plausible) {
		row("[dby-pcie] m6 mhi block does not look like MHI (length or SYSERR), no download attempted");
		return 7;
	}
	return 0;
}

/* Hand one image at a known physical address to the boot host interface, in the order
 * drivers/bus/mhi/host/boot.c uses (mhi_fw_load_bhi): BHI_STATUS cleared to zero, then the address
 * high and low, the size, the interrupt vector, a doorbell carrying a NON-ZERO session id, and a poll
 * of BHI_STATUS's low two bits (2 = transfer complete, 3 = error).  The execution environment is read
 * too, for the row, but it is not the completion signal - that was the mistake of the first attempt. */

bool retrainLink(Window &north, Window &parf, Window &dbi);

bool retrainLink(Window &north, Window &parf, Window &dbi)
{
	char line[120];
	const uint32_t cfg79 = kPadFuncGpio | kPadDrive2 | kPadPullDown | kPadOutputEnable;
	uint32_t got = 0;
	if (!padWrite(north, 79, cfg79, 0, &got)) {
		snprintf(line, sizeof(line), "[dby-pcie] retrain pad79 low mismatch got=%08x", got & kPadOwnedMask);
		row(line);
		return false;
	}
	IODelay(kPerstDelayUs);
	if (!padWrite(north, 79, cfg79, kPadOut, &got)) {
		snprintf(line, sizeof(line), "[dby-pcie] retrain pad79 high mismatch got=%08x", got & kPadOwnedMask);
		row(line);
		return false;
	}
	IODelay(kPerstDelayUs);
	/* A retrain re-latches the target speed, so the Gen1 request has to be repeated here or this helper
	 * would silently return the link to Gen2 (its caller is off by default, but it is the recovery path). */
	const uint32_t lnkctl2 = rd(dbi.va, kDbiLinkControl2);
	if ((lnkctl2 & 0xFu) != kLinkSpeedGen1) wr(dbi.va, kDbiLinkControl2, (lnkctl2 & ~0xFu) | kLinkSpeedGen1);
	const uint32_t ltssmBefore = rd(parf.va, kParfLtssm);
	wr(parf.va, kParfLtssm, ltssmBefore | (1u << 8));
	unsigned waited = 0;
	uint32_t elbi = 0, dll = 0, id = 0;
	bool up = false;
	while (waited < kLinkPollUs) {
		elbi = rd(dbi.va, kElbiStatus);
		if ((elbi & kLinkUpBit) != 0) {
			dll = rd(dbi.va, kDbiLinkStatus);
			id = rd(dbi.va, kDbiId);
			up = (dll & kDlllaBit) != 0 && id != 0xFFFFFFFFu && id != 0;
			if (up) break;
		}
		IODelay(kLinkPollStepUs);
		waited += kLinkPollStepUs;
	}
	snprintf(line, sizeof(line), "[dby-pcie] retrain ltssm %x->%x elbi=%08x dll=%08x id=%08x up=%u waited=%uus",
	    ltssmBefore, rd(parf.va, kParfLtssm), elbi, dll, id, up ? 1u : 0u, waited);
	row(line);
	return up;
}


/* Vote the four WLAN rails and the four PMIC clock outputs, once per boot, before the chip is enabled.
 * Split out of stage6k for one reason: ordering.  See the comment inside for the source that fixes the
 * order, and note that this is idempotent - a second call returns immediately, so the caller cannot vote
 * twice even though the probe reaches its entry point more than once. */
int wifiPowerVote(void)
{
	static bool ran;
	if (ran) return 0;
	char line[120];
	/* Function scope, so the latch and the return value below can see it. */
	bool ok = false;
	/* POWER FIRST, AND BEFORE THE CHIP IS ENABLED.  This block used to run inside the handoff, after the
	 * link was already trained - and that ordering was wrong against the one implementation that works on
	 * this tablet: the mainline port's device tree models the same part as qcom,qca6390-pmu with four
	 * external rails (s6a 0.95 V, s4a 1.8 V, s5a 1.9 V, s8c 1.35 V) and wlan-enable-gpios on GPIO20, and
	 * its power sequencing driver brings the rails up before it raises WLAN_EN (svesnav/linux-dby-w09
	 * sm8250-huawei-dby-w09.dts:137-160, and configure-and-build.sh enables POWER_SEQUENCING_QCOM_WCN and
	 * PCI_PWRCTRL_PWRSEQ for exactly that).  Raising WLAN_EN with the rails down and voting them later is
	 * the one thing this probe has done differently from every working implementation, so the vote moved
	 * here, ahead of every pad and every link access.  The result is printed as two masks and four addresses
	 * so a later boot can tell "voted" from "there was nothing to vote". */
	{
		constexpr size_t kDbSize = static_cast<size_t>(kCmdDbBytes);
		uint8_t *db = static_cast<uint8_t *>(IOMallocData(kDbSize));
		Window dbw = {}, rsc = {};
		if (db && mapWindow(dbw, kCmdDb, kCmdDbBytes) && mapWindow(rsc, kAppsRsc, kAppsRscBytes)) {
			/* The planner walks the database byte by byte, so it is copied out of the uncached mapping
			 * once rather than read through it at memory speed for every lookup. */
			for (size_t i = 0; i < kDbSize; i += 4) {
				const uint32_t w = rd(dbw.va, static_cast<uint32_t>(i));
				memcpy(db + i, &w, 4);
			}
			const QcomRscLayout layout = qcom_rsc_layout(rd(rsc.va, 0));
			uint32_t found = 0, voted = 0;
			uint32_t addresses[4] = {0, 0, 0, 0};
			/* Rails first: a clock on an unpowered rail is not a clock.  Each rail takes the same three
			 * writes the audio path uses for its own - high power mode at +8, the voltage at the resource
			 * address, then enable at +4 - so one loop, one helper. */
			{
				auto rail = [&](const QcomRpmhResource &res, uint32_t millivolts) {
					const uint32_t words[3] = {res.address + 8u, res.address, res.address + 4u};
					const uint32_t values[3] = {7u, millivolts, 1u};
					QcomRpmhWrite writes[16];
					const unsigned n = qcom_rpmh_active_request(&layout, kWifiTcs, words, values, 3, writes, 16);
					for (unsigned k = 0; k < n; k++) wr(rsc.va, kRscTcsWindow + writes[k].offset, writes[k].value);
					IODelay(2000);
					const unsigned m = qcom_rpmh_finish(&layout, kWifiTcs, writes, 16);
					for (unsigned k = 0; k < m; k++) wr(rsc.va, kRscTcsWindow + writes[k].offset, writes[k].value);
				};
				uint32_t railsFound = 0, railsVoted = 0;
				uint32_t railAddresses[4] = {0, 0, 0, 0};
				for (unsigned i = 0; i < 4; i++) {
					QcomRpmhResource res;
					if (qcom_cmd_db_find(db, kDbSize, kWifiRails[i].name, &res) != 0) continue;
					railsFound |= 1u << i;
					railAddresses[i] = res.address;
					rail(res, kWifiRails[i].millivolts);
					railsVoted |= 1u << i;
				}
				/* Two masks, and the distinction matters: found says the name is in this unit's command
				 * database, voted says the request was issued for it.  Neither says the RSC accepted it -
				 * this path does not wait for the done interrupt (the audio path it copies does not either,
				 * while package/AdspProbe.cpp's does) - so a mask that matches is not proof of a live vote,
				 * only of an attempted one. */
				snprintf(line, sizeof(line), "[dby-pcie] m6k rails found=%x voted=%x %08x %08x %08x %08x",
				    railsFound, railsVoted, railAddresses[0], railAddresses[1], railAddresses[2],
				    railAddresses[3]);
				row(line);
			}
			for (unsigned i = 0; i < 4; i++) {
				QcomRpmhResource res;
				if (qcom_cmd_db_find(db, kDbSize, kWifiClocks[i], &res) != 0) continue;
				found |= 1u << i;
				addresses[i] = res.address;
				/* A clock's vote is one word at the resource address with the value that turns it on:
				 * the same shape the port writes with t970_rpmh_write(name, 0, 1). */
				const uint32_t value = 1;
				QcomRpmhWrite writes[16];
				const unsigned n = qcom_rpmh_active_request(&layout, kWifiTcs, &res.address, &value, 1,
				    writes, 16);
				for (unsigned k = 0; k < n; k++) wr(rsc.va, kRscTcsWindow + writes[k].offset, writes[k].value);
				IODelay(2000);
				const unsigned m = qcom_rpmh_finish(&layout, kWifiTcs, writes, 16);
				for (unsigned k = 0; k < m; k++) wr(rsc.va, kRscTcsWindow + writes[k].offset, writes[k].value);
				voted |= 1u << i;
			}
			snprintf(line, sizeof(line), "[dby-pcie] m6k clocks found=%x voted=%x %08x %08x %08x %08x",
			    found, voted, addresses[0], addresses[1], addresses[2], addresses[3]);
			row(line);
			/* Latched below, only after the vote has actually been issued: a mapping or allocation failure
			 * is not something to remember as done, even though this probe reaches its entry point once. */
			ok = true;
		} else {
			row("[dby-pcie] m6k clocks not votable: command database or RSC window did not map");
		}
		/* One exit, so the mapping and the allocation are released exactly once on both paths. */
		unmapWindow(dbw);
		unmapWindow(rsc);
		if (db) IOFreeData(db, kDbSize);
	}
	if (ok) ran = true;
	return ok ? 0 : -1;
}

/* A windowed access to the chip's SoC-side registers.  `at()` is the whole protocol: below the window
 * start the offset is used as it is; above it the selector is written first and the access lands at
 * the window start plus the offset's low nineteen bits.  Both the write and a read-back of the selector
 * complete before the real access, because a posted selector write that is still in flight would send
 * the access to the previous window. */
uint32_t regAt(volatile uint32_t *regs, uint32_t off)
{
	if (off < kPcieWindowStart) return off;
	wr(regs, kPcieWindowSel, kPcieWindowEnable | ((off >> 19) & 0x3F));
	(void)rd(regs, kPcieWindowSel);
	return kPcieWindowStart + (off & kPcieWindowRangeMask);
}

uint32_t regRd(volatile uint32_t *regs, uint32_t off) {return rd(regs, regAt(regs, off));}
void regWr(volatile uint32_t *regs, uint32_t off, uint32_t value) {wr(regs, regAt(regs, off), value);}

/* M6k: wake the WLAN subsystem up before ringing its doorbell.
 *
 * The 2026-11-22 20:35 boot settled a lot and left one thing unexplained: the boot host interface is
 * readable, writable, in PBL, and has a live inbound window over the image, yet a correct doorbell is
 * never consumed - the status register does not even echo the sequence number.  A device whose PCIe
 * stub answers while the subsystem behind it is not running looks exactly like that, and the working
 * same-chip port does a great deal about it before its first doorbell (UniXNU-gts7xlwifi
 * package/T970WifiProbe.cpp:687-740, "ath11k_pci_sw_reset(power_on) before MHI, as Linux v6.6
 * drivers/net/wireless/ath/ath11k/pci.c does for QCA6390"): it clears WLAON_QFPROM_PWR_CTRL's
 * VDD4BLOW and bit 0, zeroes WLAON_WARM_SW_ENTRY so the chip's Q6 does not take the warm path into a
 * dead loop, clears the MHI vector registers with ath11k_mhi_clear_vector(), and resets MHI_CTRL to
 * clear the error state a reset leaves behind.
 *
 * What this stage deliberately does NOT do yet, and why: the same port's sequence also writes the
 * PCIe hot reset through the window, clears all interrupts, pulses PCIE_SOC_GLOBAL_RESET (0x3008,
 * which took this probe down twice when it was reached without any of this preamble) and writes the
 * LTSSM.  Each of those can drop the link that the rest of this boot depends on.  They belong in the
 * next stage, one at a time, after this one's rows show whether the power and warm-entry state was
 * the reason the doorbell was ignored. */
int stage6k(void)
{
	/* ONCE per boot, not once per handoff.  The probe reaches this function twice on a stage-10 boot (two
	 * runs, two arena addresses), and the sequence below resets MHI_CTRL: on a second pass that would reset
	 * the endpoint the first pass had just woken, and if the reset drops the BAR decode there is no
	 * recovery inside the same boot - kRetrainBeforeBhi is off by default.  This is the same one-shot
	 * idiom the probe's entry point uses (gDone). */
	static bool ran;
	if (ran) return 0;
	ran = true;

	/* Note on state this leaves behind: the last windowed access selects window 60 (0x1e08228), so the
	 * selector is left at 60 rather than 63.  Every later access in this file either goes through regAt()
	 * - which re-selects from the offset - or addresses a register below 0x80000, which does not use the
	 * selector at all.  Nothing else may assume the selector still holds the last window it wrote. */
	Window regs = {};
	if (!mapWindow(regs, kMemWindow, kMemWindowSpan)) {
		row("[dby-pcie] m6k register window did not map, endpoint left as it was");
		return -1;
	}
	char line[120];
	volatile uint32_t *r = regs.va;


	const uint32_t qfBefore = regRd(r, kWlaonQfpromPwrCtrl);
	const bool qfPlausible = qfBefore != 0xFFFFFFFFu && qfBefore != 0;
	if (qfPlausible) regWr(r, kWlaonQfpromPwrCtrl, qfBefore & ~kWlaonQfpromClear);
	const uint32_t qfAfter = regRd(r, kWlaonQfpromPwrCtrl);
	snprintf(line, sizeof(line), "[dby-pcie] m6k qfprom %08x -> %08x (clear mask %x) plausible=%u",
	    qfBefore, qfAfter, kWlaonQfpromClear, qfPlausible ? 1u : 0u);
	row(line);

	const uint32_t cookie = regRd(r, kWlaonQ6CookieAddr);
	const uint32_t warmBefore = regRd(r, kWlaonWarmSwEntry);
	if (warmBefore != 0xFFFFFFFFu && warmBefore != 0) regWr(r, kWlaonWarmSwEntry, 0);
	snprintf(line, sizeof(line), "[dby-pcie] m6k cookie=%08x warm=%08x -> %08x",
	    cookie, warmBefore, regRd(r, kWlaonWarmSwEntry));
	row(line);

	for (unsigned i = 0; i < 4; i++) wr(r, kMhiVectorRegs[i], 0);
	snprintf(line, sizeof(line), "[dby-pcie] m6k mhi vectors cleared %08x %08x %08x %08x",
	    rd(r, kMhiVectorRegs[0]), rd(r, kMhiVectorRegs[1]), rd(r, kMhiVectorRegs[2]), rd(r, kMhiVectorRegs[3]));
	row(line);

	/* Clear any interrupt the chip is still holding.  The working port's comment is specific about why:
	 * a reset leaves one pending that crashes the secondary boot loader as soon as it unmasks interrupts
	 * (T970WifiProbe.cpp:712, ath11k's clear_all_intrs). */
	row("[dby-pcie] m6k int clear: touching 1e08228");
	regWr(r, kPcieIntAllClear, 0xFFFFFFFFu);
	snprintf(line, sizeof(line), "[dby-pcie] m6k int clear readback=%08x", regRd(r, kPcieIntAllClear));
	row(line);

	/* The SoC global reset, pulsed through the same window.  The row that says it is being touched comes
	 * before the access, not after: two earlier boots died on this register without printing anything, and
	 * if that happens again this boot's rows must say which line it was. */
	row("[dby-pcie] m6k soc reset: touching 3008");
	const uint32_t socBefore = rd(r, kPcieSocGlobalReset);
	if (socBefore == 0xFFFFFFFFu) {
		/* All ones is not a value this register can hold; writing it back would assert every bit - and
		 * this is the register whose access took two boots down.  Report and leave it alone. */
		row("[dby-pcie] m6k soc reset reads all ones, not written; the reset pulse is skipped");
	} else {
		wr(r, kPcieSocGlobalReset, socBefore | kPcieSocGlobalResetV);
		IODelay(kSocResetDelayUs);
		wr(r, kPcieSocGlobalReset, socBefore & ~kPcieSocGlobalResetV);
		IODelay(kSocResetDelayUs);
		snprintf(line, sizeof(line), "[dby-pcie] m6k soc reset %08x -> %08x", socBefore,
		    rd(r, kPcieSocGlobalReset));
		row(line);
	}

	/* The reset a driver performs before MHI: the bit is expected to clear itself when the block has
	 * taken it, and MHI_STATUS's SYSERR goes with it.  If the block answers all ones afterwards, the
	 * endpoint dropped its own configuration and the BAR/CONFIG pair has to be written again - that is
	 * the lesson of the PERST# experiment (kRetrainBeforeBhi), applied here instead of assumed away. */
	const uint32_t ctrlBefore = rd(r, kMhiCtrl);
	wr(r, kMhiCtrl, kMhiCtrlReset);
	/* Wait for the block to take it.  This is the step the 20:52 boot shows was missing: the bit read
	 * back set ten milliseconds later, and the doorbell was rung while the block was still in reset. */
	unsigned resetWaited = 0;
	uint32_t ctrlAfter = rd(r, kMhiCtrl);
	while ((ctrlAfter & kMhiCtrlReset) != 0 && resetWaited < kMhiResetPollUs) {
		IODelay(kMhiResetPollStepUs);
		resetWaited += kMhiResetPollStepUs;
		ctrlAfter = rd(r, kMhiCtrl);
	}
	const bool resetReleased = (ctrlAfter & kMhiCtrlReset) == 0;
	if (resetReleased) {
		snprintf(line, sizeof(line), "[dby-pcie] m6k mhi reset released after %uus (ctrl=%08x)",
		    resetWaited, ctrlAfter);
		row(line);
	} else {
		/* The working port's state machine does not ring a doorbell in this state, and neither should
		 * this probe: a block that is still in reset ignores it, and ringing anyway would put a row that
		 * looks like a device fault on top of a condition this probe caused itself. */
		snprintf(line, sizeof(line),
		    "[dby-pcie] m6k mhi reset still set after %uus (ctrl=%08x), the handoff is refused",
		    resetWaited, ctrlAfter);
		row(line);
	}
	const uint32_t regLenAfter = rd(r, kMhiRegLen);
	snprintf(line, sizeof(line), "[dby-pcie] m6k mhi ctrl %08x -> %08x reglen=%08x sts=%08x",
	    ctrlBefore, ctrlAfter, regLenAfter, rd(r, kMhiStatus));
	row(line);
	bool recovered = regLenAfter != 0xFFFFFFFFu;
	if (!recovered) {
		Window conf = {};
		if (mapWindow(conf, kConfWindow, kConfWindowSpan)) {
			/* Same two writes M4 does, with the same treatment of the command/status word: keep every
			 * low-half command bit and carry the status half back exactly as it was read, because those
			 * bits are write-one-to-clear and dropping them to zero would acknowledge them. */
			const uint32_t cmdBefore = rd(conf.va, kCfgCommand);
			wr(conf.va, kCfgBar0, kCfgBar0Value);
			wr(conf.va, kCfgCommand,
			    (cmdBefore & 0xFFFF0000u) | ((cmdBefore & 0xFFFFu) | kCfgCommandMemMaster));
			snprintf(line, sizeof(line),
			    "[dby-pcie] m6k endpoint re-armed cmd=%04x bar0=%08x reglen=%08x",
			    rd(conf.va, kCfgCommand) & 0xFFFFu, rd(conf.va, kCfgBar0), rd(r, kMhiRegLen));
			row(line);
			recovered = rd(r, kMhiRegLen) != 0xFFFFFFFFu;
		}
		unmapWindow(conf);
	}
	unmapWindow(regs);
	/* A block that still reads all ones after the reset and the re-arm is an endpoint this boot cannot
	 * talk to, and ringing its doorbell would only add rows that look like a device failure.  A block still
	 * held in reset is the same answer for the same reason. */
	if (!resetReleased) return -1;
	if (!recovered) {
		row("[dby-pcie] m6k the register block did not come back, the handoff is refused");
		return -1;
	}
	return 0;
}

/* Print what the primary boot loader logged about the handoff that just failed.  Five rows: the four
 * single registers, then the twenty-word log in groups of five.  Called only after the poll, because the
 * window it maps (the whole 1 MiB BAR0) is not the one bhiHandoff is using, and only on a failure, because
 * on success these registers describe a boot the device has already left behind. */
void pblLog(void)
{
	Window regs = {};
	if (!mapWindow(regs, kMemWindow, kMemWindowSpan)) {
		row("[dby-pcie] m6l pbl log window did not map");
		return;
	}
	char line[120];
	volatile uint32_t *r = regs.va;
	snprintf(line, sizeof(line), "[dby-pcie] m6l pbl log=%08x cfg=%08x boot=%08x dbg2=%08x",
	    regRd(r, kPblLogging), regRd(r, kPblWlanBootCfg), regRd(r, kPblBootstrapStatus),
	    regRd(r, kPcieBhiErrDbg2));
	row(line);
	for (unsigned base = 0; base < kPblLogWords; base += 5) {
		uint32_t w[5];
		for (unsigned i = 0; i < 5; i++)
			w[i] = regRd(r, kPblLogBase + 4u * (base + i));
		snprintf(line, sizeof(line), "[dby-pcie] m6l log%02u %08x %08x %08x %08x %08x",
		    base, w[0], w[1], w[2], w[3], w[4]);
		row(line);
	}
	unmapWindow(regs);
}

int bhiHandoff(uint64_t pa, uint32_t bytes, uint32_t *eeOut, unsigned *waitedOut)
{
	char line[120];
	/* 0xFF rather than PBL: on the refusal paths below nothing was measured, and a caller printing this
	 * value must not be able to read it as "the endpoint is in PBL". */
	*eeOut = 0xFFu;
	*waitedOut = 0;
	/* Wake the subsystem before anything is read from or written to its boot host interface: M6k's rows
	 * are the state the handoff starts from, so they come first.  A refusal there means the block is not
	 * readable any more, and the honest answer to the caller is the same as for an offset outside the
	 * window: nothing was measured, nothing was written. */
	if (stage6k() != 0) return -1;
	if (bytes == 0) {
		row("[dby-pcie] bhi image length is zero, refused");
		return -1;
	}
	if ((bytes & 3u) != 0) {
		snprintf(line, sizeof(line), "[dby-pcie] bhi image length %u is not a multiple of 4, refused", bytes);
		row(line);
		return -1;
	}
	Window bar = {};
	if (!mapWindow(bar, kMemWindow, kMhiWindowSpan)) {
		row("[dby-pcie] bhi bar0 map failed");
		return -1;
	}
	if (kRetrainBeforeBhi) {
		/* The chip reset itself: bring the link back with the host-side registers alone.  Off by default;
		 * see kRetrainBeforeBhi for what a PERST# assertion does to the endpoint's BAR0. */
		Window north = {}, parfForTrain = {}, dbiForTrain = {};
		if (mapWindow(north, kTile[kPinTile[0]], kTileSpan) &&
		    mapWindow(parfForTrain, kParf, kParfSpan) &&
		    mapWindow(dbiForTrain, kDbi, kDbiSpan)) {
			const bool back = retrainLink(north, parfForTrain, dbiForTrain);
			if (!back) {
				unmapWindow(north);
				unmapWindow(parfForTrain);
				unmapWindow(dbiForTrain);
				unmapWindow(bar);
				row("[dby-pcie] m6g link did not come back after the soc reset");
				return -1;
			}
			unmapWindow(north);
			unmapWindow(parfForTrain);
			unmapWindow(dbiForTrain);
		}
	}
	/* Read the block as it stands, before anything is written to it.  This row used to describe a block the
	 * probe had just reset; with kRetrainBeforeBhi off nothing has touched the endpoint since M4 armed it, so
	 * these numbers are the state the handoff starts from and the reference the next row's write-backs are
	 * compared against.  The last boots also ended with the link DOWN (elbi=0x5101, up=0, dbi=0) after the
	 * firmware listing, so a later read of the same registers is worth having on the way out as well. */
	const uint32_t regLenAfter = rd(bar.va, kMhiRegLen);
	const uint32_t bhi = rd(bar.va, kMhiBhiOff);
	const uint32_t bhieAfter = rd(bar.va, kMhiBhieOff);
	const uint32_t ctrlAfter = rd(bar.va, kMhiCtrl);
	/* The offset comes from the device.  Measured 0x100 is safe; anything that would put the LAST register
	 * we touch outside the mapped BAR0 page is refused rather than written.  The bound is kBhiErrDbg3, the
	 * furthest one: it used to be kBhiErrCode, which stopped matching the reads once the error-debug
	 * registers joined them (a device-reported bhi in 0xFC4..0xFCC would have read up to twelve bytes past
	 * the mapping, and rd() does not bounds-check). */
	if (bhi > kMhiWindowSpan || bhi + kBhiErrDbg3 + 4u > kMhiWindowSpan) {
		snprintf(line, sizeof(line), "[dby-pcie] m6h bhi=%08x bhie=%08x ctrl=%08x", bhi, bhieAfter, ctrlAfter);
		row(line);
		snprintf(line, sizeof(line), "[dby-pcie] bhi offset %08x outside the mapped window, refused", bhi);
		row(line);
		unmapWindow(bar);
		return -1;
	}
	/* BHI_EXECENV says which boot stage the device is in, and the same-chip reference refuses to start a
	 * download unless it is PBL (0) or EDL (6) (UniXNU-gts7xlwifi package/MhiBoot.inc: mhi_boot_begin).
	 * This probe has never read it BEFORE the doorbell - it only reported the value afterwards, and set 0
	 * for itself when the read failed - so a device sitting in another stage looked exactly like a device
	 * that ignored a correct doorbell.  MHI_STATUS is printed beside it for the same reason the sibling
	 * treats 0xffffffff there as "not answering" rather than as a status. */
	const uint32_t execBefore = rd(bar.va, bhi + kBhiExecEnv);
	const uint32_t stsBefore = rd(bar.va, kMhiStatus);
	snprintf(line, sizeof(line),
	    "[dby-pcie] m6h before handoff reglen=%08x bhi=%08x bhie=%08x ctrl=%08x sts=%08x ee=%08x",
	    regLenAfter, bhi, bhieAfter, ctrlAfter, stsBefore, execBefore);
	row(line);
	/* Give the endpoint a window for the read the doorbell asks for.  Before this, every access on this
	 * link went host to device (outbound MEM and CFG0); the first device-initiated memory read is MHI's
	 * boot loader fetching the image, and this controller has inbound regions for exactly that.  The row
	 * prints the region as it stood BEFORE it was programmed, because "the firmware already mapped it" and
	 * "nobody had" are different answers and the second one is the reason the doorbell did nothing.
	 *
	 * Three limits, each from a review of the first version of this block:
	 *  - the identity mapping is only an identity below 4 GiB.  LOWER/UPPER_BASE and TARGET are written
	 *    with an upper word of zero, so an arena above 4 GiB would be mapped to a different address while
	 *    both read-backs still matched what was asked.  Refused rather than programmed.
	 *  - the viewport layout is not attempted at all.  In that layout the direction is bit 31 of the
	 *    viewport index, and this probe has never set it: if this core ignored it, the write would land on
	 *    OUTBOUND region 1 - the CFG0 window M4 programmed - and the read-back would still agree, because
	 *    those CSRs echo what they were given.  This unit answered the unrolled layout in M4, so the
	 *    branch that matters is the one taken here.
	 *  - CTRL1 and LIMIT are snapshotted with CTRL2, base and target, so a collision with something the
	 *    firmware had programmed is visible in full rather than half.
	 */
	{
		Window atuIn = {}, dbiIn = {};
		const uint64_t lastByte = pa + static_cast<uint64_t>(bytes) - 1ull;
		if (gAtuViewport) {
			row("[dby-pcie] m6j inbound skipped: viewport layout, direction bit unverified on this core");
		} else if ((pa >> 32) != 0 || lastByte > 0xFFFFFFFFull) {
			snprintf(line, sizeof(line),
			    "[dby-pcie] m6j inbound needs an address below 4 GiB (pa=%llx last=%llx), skipped",
			    pa, lastByte);
			row(line);
		} else if (mapWindow(atuIn, kAtuWindow, kAtuWindowSpan) && mapWindow(dbiIn, kDbi, kDbiSpan)) {
			const uint32_t at = kAtuInboundOffset + 1u * kAtuRegionStride;
			const uint32_t beforeCtrl1 = rd(atuIn.va, at + kAtuCtrl1);
			const uint32_t beforeCtrl2 = rd(atuIn.va, at + kAtuCtrl2);
			const uint32_t beforeBase = rd(atuIn.va, at + kAtuLowerBase);
			const uint32_t beforeLimit = rd(atuIn.va, at + kAtuLimit);
			const uint32_t beforeTarget = rd(atuIn.va, at + kAtuLowerTarget);
			snprintf(line, sizeof(line),
			    "[dby-pcie] m6j inbound r1 unrolled before c1=%08x c2=%08x base=%08x lim=%08x tgt=%08x",
			    beforeCtrl1, beforeCtrl2, beforeBase, beforeLimit, beforeTarget);
			row(line);
			const uint32_t low = static_cast<uint32_t>(pa & 0xFFFFFFFFu);
			/* The span is rounded up to a power of two that still fits the arena.  The DWC inbound region
			 * takes a base and a limit, and a span that is not a power of two is not something its address
			 * match is specified for; the 266,684-byte image did transfer through one, so this is not a
			 * proven failure, but a window that cannot be mis-matched is worth more here than three bytes
			 * of accuracy in a diagnostic row. */
			uint32_t span = 0x1000u;
			while (span < bytes && span < gDmaArenaBytes) span <<= 1;
			if (span > gDmaArenaBytes) span = gDmaArenaBytes;
			const bool mapped = atuProgramInbound(atuIn, dbiIn, false, 1, low, span, low);
			snprintf(line, sizeof(line),
			    "[dby-pcie] m6j inbound r1 mapped=%u c2=%08x base=%08x lim=%08x tgt=%08x",
			    mapped ? 1u : 0u, rd(atuIn.va, at + kAtuCtrl2), rd(atuIn.va, at + kAtuLowerBase),
			    rd(atuIn.va, at + kAtuLimit), rd(atuIn.va, at + kAtuLowerTarget));
			row(line);
		} else {
			row("[dby-pcie] m6j inbound window did not map, endpoint left without one");
		}
		unmapWindow(atuIn);
		unmapWindow(dbiIn);
	}
	/* The SMMU's fault registers are sticky, so the only useful reading is a difference: snapshot them
	 * before the doorbell and compare with the row read after the poll.  Nothing is cleared - the same SMMU
	 * serves the audio and touch streams, and acknowledging their faults here would hide them from whoever
	 * owns them. */
	{
		Window smmu = {};
		if (mapWindow(smmu, kAppsSmmu, kAppsSmmuSpan)) {
			snprintf(line, sizeof(line), "[dby-pcie] m6n smmu before fsr48=%08x r40=%08x synr50=%08x synr54=%08x",
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultStatus)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultAddress)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultSynr0)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultSynr1)));
			row(line);
		}
		unmapWindow(smmu);
	}
	row("[dby-pcie] bhi handoff begin");
	wr(bar.va, bhi + kBhiStatusReg, 0);
	wr(bar.va, bhi + kBhiImgAddrHigh, static_cast<uint32_t>(pa >> 32));
	wr(bar.va, bhi + kBhiImgAddrLow, static_cast<uint32_t>(pa & 0xFFFFFFFFu));
	wr(bar.va, bhi + kBhiImgSize, bytes);
	/* The interrupt vector is written as ZERO from the 2026-11-23 16:30 boot on, and the comment that used to
	 * sit here argued the opposite.  The history is worth one sentence, because it is why the value moved: the
	 * working same-chip port's start_sbl() does write a vector here ("a reset clears it"), which read as a
	 * requirement, so this probe wrote 1 - but the only value that port's caller ever passes is 0
	 * (T970WifiProbe.cpp:747), and mainline's mhi_fw_load_bhi() does not write BHI_INTVEC at all.  Nothing in
	 * the register file changes with it: BHI_INTVEC is not read back and not compared, and the probe learns
	 * the transfer's outcome by polling BHI_STATUS and MHI_STATUS, not by waiting for an interrupt this
	 * controller never enables.  The row still prints vec=, so this boot says which value it used. */
	wr(bar.va, bhi + kBhiIntVec, kBhiMsiVector);
	wr(bar.va, bhi + kBhiImgTxDb, kBhiSessionId);

	/* Read back what was just written.  Two boots now have shown BHI_STATUS reading zero after a clear
	 * and a doorbell, so the first question is not what the device did but whether these writes reach
	 * BAR0: address, size and doorbell reading back as written means the window works and the endpoint
	 * is not answering; reading zero means the writes are being dropped and the fault is on our side of
	 * the link. */
	const uint32_t rbLow = rd(bar.va, bhi + kBhiImgAddrLow);
	const uint32_t rbHigh = rd(bar.va, bhi + kBhiImgAddrHigh);
	const uint32_t rbSize = rd(bar.va, bhi + kBhiImgSize);
	const uint32_t rbDb = rd(bar.va, bhi + kBhiImgTxDb);
	snprintf(line, sizeof(line),
	    "[dby-pcie] bhi wrote addr=%08x:%08x size=%08x vec=%u db=%08x | read back %08x:%08x %08x %08x",
	    static_cast<uint32_t>(pa >> 32), static_cast<uint32_t>(pa & 0xFFFFFFFFu), bytes, kBhiMsiVector,
	    kBhiSessionId, rbHigh, rbLow, rbSize, rbDb);
	row(line);

	unsigned waited = 0;
	uint32_t status = rd(bar.va, bhi + kBhiStatusReg);
	uint32_t exec = rd(bar.va, bhi + kBhiExecEnv);
	/* drivers/bus/mhi/common.h puts the transfer status in BHI_STATUS bits 31:30 (BHI_STATUS_MASK is
	 * GENMASK(31, 30), with SUCCESS 0x02 and ERROR 0x03 as the shifted field values).  Polling bits 1:0 -
	 * what the first version did - reads zero forever however the device answers, which is exactly the
	 * hardware result: sts=00000000 xfer=0 for 300 ms. */
	while (waited < kBhiPollUs) {
		status = rd(bar.va, bhi + kBhiStatusReg);
		exec = rd(bar.va, bhi + kBhiExecEnv);
		const uint32_t xfer = (status >> 30) & 0x3u;
		if (xfer == 2u || xfer == 3u) break;          /* success or error: both end the wait */
		IODelay(kBhiPollStepUs);
		waited += kBhiPollStepUs;
	}
	const uint32_t xfer = (status >> 30) & 0x3u;
	const uint32_t ee = exec & 0xFFu;
	const uint32_t err = rd(bar.va, bhi + kBhiErrCode);
	/* The error-debug registers are read next and unconditionally, because the block does not stay
	 * readable after a failure: the 21:19 boot's status read back all ones, which is the endpoint having
	 * reset itself, so anything read a row later would be gone.  They mean nothing on a successful
	 * transfer, so they are reported beside the status rather than in a row of their own. */
	const uint32_t dbg1 = rd(bar.va, bhi + kBhiErrDbg1);
	const uint32_t dbg2 = rd(bar.va, bhi + kBhiErrDbg2);
	const uint32_t dbg3 = rd(bar.va, bhi + kBhiErrDbg3);
	const uint32_t stsAgain = rd(bar.va, bhi + kBhiStatusReg);
	const uint32_t regLenNow = rd(bar.va, kMhiRegLen);
	const uint32_t ctrlNow = rd(bar.va, kMhiCtrl);
	snprintf(line, sizeof(line), "[dby-pcie] bhi sts=%08x xfer=%u exec=%08x ee=%u err=%08x waited=%uus",
	    status, xfer, exec, ee, err, waited);
	row(line);
	snprintf(line, sizeof(line), "[dby-pcie] bhi dbg %08x %08x %08x sts2=%08x reglen=%08x ctrl=%08x",
	    dbg1, dbg2, dbg3, stsAgain, regLenNow, ctrlNow);
	row(line);
	/* What the MHI block says about itself once the transfer is over, and it is read for a reason that has
	 * been overlooked for a week of boots: every reading in this record so far came from the BOOT HOST
	 * INTERFACE (bhi+0x08..0x3c), and when the loader takes over it stops answering there - which is exactly
	 * what the rows show (BHI_STATUS all ones, EXECENV 0, ERRCODE parked at a constant).  Whether the loader
	 * actually started is a different question, and MHI_STATUS answers it: bit 0 is READY and bits 15:8 are
	 * the state.  A device that reports BHI_STATUS all ones and MHI_STATUS READY is a device that has moved
	 * on, not a device that failed. */
	const uint32_t mhiStatusNow = rd(bar.va, kMhiStatus);
	const uint32_t execAgain = rd(bar.va, bhi + kBhiExecEnv);
	/* All ones is the reset block, not a status - and its bit 0 would otherwise read as "READY" and turn
	 * this row into the opposite of its own purpose.  The same idiom is used for a device-reported offset
	 * and for the registration value earlier in this file. */
	const bool blockAnswers = mhiStatusNow != 0xFFFFFFFFu;
	snprintf(line, sizeof(line), "[dby-pcie] bhi post mhi_sts=%08x state=%u ready=%u exec=%08x",
	    mhiStatusNow, (mhiStatusNow >> kMhiStateShift) & 0xFFu,
	    (blockAnswers && (mhiStatusNow & kMhiStatusReady)) ? 1u : 0u, execAgain);
	row(line);
	if (!blockAnswers) {
		row("[dby-pcie] bhi post the block does not answer (all ones), no ready claim");
	} else {
		/* And give it time: the loader may set READY a little after the interface stops answering. */
		unsigned readyWaited = 0;
		uint32_t mhiStatusLater = mhiStatusNow;
		while ((mhiStatusLater & kMhiStatusReady) == 0 && readyWaited < kMhiReadyPollUs) {
			IODelay(kMhiReadyPollStepUs);
			readyWaited += kMhiReadyPollStepUs;
			mhiStatusLater = rd(bar.va, kMhiStatus);
			if (mhiStatusLater == 0xFFFFFFFFu) break;
		}
		if (mhiStatusLater != 0xFFFFFFFFu && (mhiStatusLater & kMhiStatusReady) != 0) {
			snprintf(line, sizeof(line), "[dby-pcie] bhi ready after %uus mhi_sts=%08x state=%u ee=%08x",
			    readyWaited, mhiStatusLater, (mhiStatusLater >> kMhiStateShift) & 0xFFu,
			    rd(bar.va, bhi + kBhiExecEnv));
			row(line);
		}
	}
	/* Did the endpoint's own reads of the image fault on the way?  Read-only, and the row is the difference
	 * between two explanations that look identical in every other register: a device that read the image and
	 * rejected it, and a device that never managed to read it at all. */
	{
		Window smmu = {};
		if (mapWindow(smmu, kAppsSmmu, kAppsSmmuSpan)) {
			snprintf(line, sizeof(line), "[dby-pcie] m6n smmu after fsr48=%08x r40=%08x synr50=%08x synr54=%08x",
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultStatus)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultAddress)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultSynr0)),
			    rd(smmu.va, static_cast<uint32_t>(kSmmuFaultSynr1)));
			row(line);
		} else {
			row("[dby-pcie] m6n the SMMU window did not map, fault state unknown");
		}
		unmapWindow(smmu);
	}
	/* Only when the device reported an error: on the success path the log belongs to a boot it has already
	 * moved past, and on the silent path there is nothing to explain yet. */
	if (xfer == 3u) pblLog();
	unmapWindow(bar);
	*eeOut = ee;
	*waitedOut = waited;
	if (xfer == 2u) return 0;
	if (xfer == 3u) {
		row("[dby-pcie] bhi reported a transfer error");
		return 10;
	}
	row("[dby-pcie] bhi did not complete; a retry needs an MHI reset, not another doorbell");
	return 10;
}

struct FatLister {char *row; size_t used; unsigned count;};

void fatEmit(void *context, const char *name, int directory, uint32_t size)
{
	(void)size;   /* the listing only needs the names */
	struct FatLister *l = static_cast<struct FatLister *>(context);
	const size_t room = 119 - l->used;
	if (room < 16) return;
	char item[48];
	int n = snprintf(item, sizeof(item), "%s%s%s", directory ? "/" : "", name, directory ? "/" : "");
	if (n > 0 && static_cast<size_t>(n) < room - 1) {
		memcpy(l->row + l->used, item, static_cast<size_t>(n));
		l->used += static_cast<size_t>(n);
		l->row[l->used++] = ' ';
		l->row[l->used] = '\0';
		l->count++;
	}
}

struct PartitionReader {uint32_t lun; uint64_t first; uint64_t blocks;};
int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
	struct PartitionReader *p = static_cast<struct PartitionReader *>(context);
	/* This is the only fence between the walkers and the device: FatVolume's own bounds check covers
	 * files it opened, and fat16Find reads directory sectors directly.  Anything past the partition is a
	 * refusal, not a read. */
	if (offset > p->blocks * 4096u || length > p->blocks * 4096u - offset) return -1;
	uint8_t *at = static_cast<uint8_t *>(to);
	while (length) {
		const uint64_t lba = p->first + offset / 4096u;
		const size_t inBlock = static_cast<size_t>(offset % 4096u);
		const size_t chunk = (length < 4096u - inBlock) ? length : 4096u - inBlock;
		uint8_t block[4096];
		/* 1 = success (NativeUfs.cpp), the opposite of this file's own 0-is-success convention. */
		if (native_ufs_part_read(p->lun, lba, 1, block) != 1) return -1;
		memcpy(at, block + inBlock, chunk);
		at += chunk;
		offset += chunk;
		length -= chunk;
	}
	return 0;
}

/* Walk one directory of a mounted volume and hand every name to emit.  FatVolume opens files, not
 * directories, so the directory is walked here: root for the empty path, otherwise fat_open() gives the
 * first cluster and the chain is followed through the volume's own allocation table with the same
 * bounds the reader uses (a damaged volume ends the walk, never loops). */
int fatList(FatVolume *v, const char *path, void (*emit)(void *, const char *, int, uint32_t), void *context)
{
	uint32_t cluster = 0;
	int isRoot = 1;
	if (path && path[0]) {
		FatFile dir = {};
		if (fat_open(v, path, &dir) != FatOk) return FatNotFound;
		if (!dir.directory) return FatNotDirectory;
		cluster = dir.first_cluster;
		isRoot = 0;
	}
	const uint32_t perCluster = v->cluster_bytes / 32u;
	unsigned guard = 0;
	for (;;) {
		uint64_t offset = 0;
		uint32_t entries = 0;
		if (isRoot && !v->fat32) {
			offset = v->root_offset;
			entries = v->root_entries;
		} else {
			if (cluster < 2) return FatDamaged;
			const uint32_t index = (isRoot && v->fat32) ? v->root_cluster : cluster;
			if (index < 2) return FatDamaged;
			offset = v->data_offset + static_cast<uint64_t>(index - 2) * v->cluster_bytes;
			entries = perCluster;
		}
		for (uint32_t i = 0; i < entries; i++) {
			uint8_t entry[32];
			if (v->read(v->context, offset + i * 32u, entry, sizeof(entry)) != 0) return FatReadError;
			if (entry[0] == 0x00) return FatOk;                 /* end of directory */
			if (entry[0] == 0xE5) continue;                     /* deleted */
			const uint8_t attr = entry[11];
			if (attr == 0x0F || (attr & 0x08)) continue;        /* LFN entry, volume label */
			char name[13];
			unsigned n = 0;
			for (unsigned c = 0; c < 8 && entry[c] != ' '; c++) name[n++] = static_cast<char>(entry[c]);
			if ((attr & 0x10) == 0 && entry[8] != ' ') {
				name[n++] = '.';
				for (unsigned c = 8; c < 11 && entry[c] != ' '; c++) name[n++] = static_cast<char>(entry[c]);
			}
			name[n] = '\0';
			const uint32_t size = static_cast<uint32_t>(entry[28]) | (static_cast<uint32_t>(entry[29]) << 8) |
			    (static_cast<uint32_t>(entry[30]) << 16) | (static_cast<uint32_t>(entry[31]) << 24);
			emit(context, name, (attr & 0x10) ? 1 : 0, size);
			if (++guard > 512) return FatOk;                    /* one screen is enough to see a layout */
		}
		if (isRoot && !v->fat32) return FatOk;                  /* the FAT16 root is one fixed run */
		/* next cluster of the chain */
		uint8_t fat[32];
		uint64_t at;
		if (v->fat32) at = v->fat_offset + static_cast<uint64_t>(cluster) * 4u;
		else at = v->fat_offset + static_cast<uint64_t>(cluster) * 2u;
		if (v->read(v->context, at, fat, sizeof(fat)) != 0) return FatReadError;
		uint32_t next;
		if (v->fat32) next = (static_cast<uint32_t>(fat[0]) | (static_cast<uint32_t>(fat[1]) << 8) |
		    (static_cast<uint32_t>(fat[2]) << 16) | (static_cast<uint32_t>(fat[3]) << 24)) & 0x0FFFFFFFu;
		else next = static_cast<uint32_t>(fat[0]) | (static_cast<uint32_t>(fat[1]) << 8);
		if (v->fat32 ? next >= 0x0FFFFFF8u : next >= 0xFFF8u) return FatOk;
		if (next < 2 || next > (v->fat32 ? v->clusters + 1u : 0xFFF6u)) return FatDamaged;
		cluster = next;
	}
}


int stage6b(uint64_t *arenaPa, uint32_t *imageBytes, uint32_t *eeAfter)
{
	*arenaPa = 0; *imageBytes = 0; *eeAfter = 0;
	char line[120];

	if (!gDmaArena || !gDmaArenaPa) {
		row("[dby-pcie] m6b no arena (M5 did not allocate one)");
		return 9;
	}

	/* 1. the image, out of the tablet's own firmware partition.  The candidate names are the vendor's
	 * (image/qca6390/m3.bin) and the flat variants a converged firmware volume sometimes carries; M7a's
	 * listing is what turns this list into a fact. */
	if (!native_ufs_part_find || !native_ufs_part_read) {
		row("[dby-pcie] m6b ufs hooks not linked");
		return 9;
	}
	static const char *const kVolumes[] = {"firmware", "firmware_a", "firmware_b", "modem"};
	struct PartitionReader part = {};
	const char *volume = nullptr;
	for (unsigned i = 0; i < sizeof(kVolumes) / sizeof(kVolumes[0]); i++) {
		uint32_t lun = 0;
		uint64_t first = 0, blocks = 0;
		if (native_ufs_part_find(kVolumes[i], &lun, &first, &blocks) == 1 && blocks) {
			volume = kVolumes[i];
			part.lun = lun; part.first = first; part.blocks = blocks;
			break;
		}
	}
	if (!volume) {
		row("[dby-pcie] m6b no firmware partition");
		return 9;
	}
	FatVolume fat = {};
	if (fat_mount(&fat, part.blocks * 4096u, partitionRead, &part) != FatOk) {
		row("[dby-pcie] m6b firmware volume does not mount");
		return 9;
	}

	static const char *const kImages[] = {
	    "image/qca6390/m3.bin", "image/qca6390/M3.BIN", "image/m3.bin", "qca6390/m3.bin",
	    "M3.BIN", "m3.bin",
	};
	FatFile file = {};
	const char *chosen = nullptr;
	for (unsigned i = 0; i < sizeof(kImages) / sizeof(kImages[0]); i++) {
		if (fat_open(&fat, kImages[i], &file) == FatOk && file.size > 0 && file.size <= gDmaArenaBytes) {
			chosen = kImages[i];
			break;
		}
	}
	if (!chosen) {
		snprintf(line, sizeof(line), "[dby-pcie] m6b no sbl image in %s (tried %u names)", volume,
		    static_cast<unsigned>(sizeof(kImages) / sizeof(kImages[0])));
		row(line);
		return 9;
	}

	uint8_t *dst = static_cast<uint8_t *>(gDmaArena->getBytesNoCopy());
	if (!dst || fat_read(&fat, &file, 0, dst, file.size) != FatOk) {
		row("[dby-pcie] m6b image read failed");
		return 9;
	}
	snprintf(line, sizeof(line), "[dby-pcie] m6b sbl %s bytes=%u pa=%llx", chosen, file.size, gDmaArenaPa);
	row(line);

	/* 2. clean the image out of the data cache before the endpoint reads it. */
	for (uint64_t at = reinterpret_cast<uint64_t>(dst) & ~63ull;
	     at < reinterpret_cast<uint64_t>(dst) + file.size; at += 64ull) {
		__asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");

	unsigned waited = 0;
	uint32_t ee = kMhiEePbl;
	const int bhiRc = bhiHandoff(gDmaArenaPa, static_cast<uint32_t>(file.size), &ee, &waited);
	if (bhiRc < 0) return 9;

	*arenaPa = gDmaArenaPa;
	*imageBytes = static_cast<uint32_t>(file.size);
	*eeAfter = ee;
	if (ee == kMhiEePbl) {
		row("[dby-pcie] m6b endpoint stayed in PBL, the sbl handshake did not take");
		return 10;
	}
	return 0;
}


/* ---- M7b: the partition table itself -------------------------------------------------------------
 *
 * M7a's result on hardware was decisive and negative: this tablet has no partition named "firmware",
 * "firmware_a" or "firmware_b" (the only name that answered was "modem", LUN 4), and the modem volume
 * is not FAT (fat_mount answered FatNotFat).  So the firmware is somewhere whose name nobody has read
 * yet, and the partition table is what knows.  This stage reads the GPT through the driver's own
 * absolute-block hook and prints the names it finds, which turns "which partition holds qca6390" from a
 * guess into a reading.
 *
 * Read-only: one hook, two blocks, no writes.  The GPT's own layout is the UEFI one - a protective MBR
 * in the first 512 bytes with the header at LBA 1 and the entry array at the LBA the header names, all
 * in 512-byte units even on a 4096-byte-block device (this board's own userdata note records exactly
 * that: header at byte 512, entries 128 bytes each). */


/* ---- M7a: what firmware this tablet actually ships -----------------------------------------------
 *
 * Everything above the link is proven, and M6a showed the endpoint sitting in PBL waiting for firmware -
 * but this port has never read the tablet's own firmware partition, so the vendor's file names are still
 * a guess.  This stage removes the guess without flashing anything and without EDL: the board already
 * carries a read-only FAT reader and the two UFS hooks package/AdspFirmware.c uses to take the ADSP
 * image out of a partition in place, so the same route can simply list what is there.
 *
 * It mounts the partitions whose names this board's own code already knows (the audio reader looks for
 * "modem"), lists the root and then /image, and prints up to eight names per row.  Read-only: no
 * install, no copy, nothing written to any volume.
 *
 * Returns 0 when a partition mounted and listed, 8 when none did. */
int stage7a(void)
{
	char line[120], line2[120];
	if (!native_ufs_part_find || !native_ufs_part_read) {
		row("[dby-pcie] m7 ufs partition hooks are not linked (ShengUfs provides them)");
		return 8;
	}
	static const char *const kVolumes[] = {"firmware", "firmware_a", "firmware_b", "modem"};
	struct PartitionReader part = {};
	const char *chosen = nullptr;
	for (unsigned i = 0; i < sizeof(kVolumes) / sizeof(kVolumes[0]); i++) {
		uint32_t lun = 0;
		uint64_t first = 0, blocks = 0;
		if (native_ufs_part_find(kVolumes[i], &lun, &first, &blocks) == 0 && blocks) {
			chosen = kVolumes[i];
			part.lun = lun; part.first = first; part.blocks = blocks;
			break;
		}
	}
	if (!chosen) {
		row("[dby-pcie] m7 no firmware partition by any known name");
		return 8;
	}
	snprintf(line, sizeof(line), "[dby-pcie] m7 part=%s lun=%u first=%llu blocks=%llu", chosen, part.lun,
	    part.first, part.blocks);
	row(line);

	FatVolume volume = {};
	const int mounted = fat_mount(&volume, part.blocks * 4096u, partitionRead, &part);
	if (mounted != FatOk) {
		snprintf(line, sizeof(line), "[dby-pcie] m7 fat mount failed rc=%d", mounted);
		row(line);
		return 8;
	}
	snprintf(line, sizeof(line), "[dby-pcie] m7 fat32=%u cluster=%u clusters=%u",
	    volume.fat32, volume.cluster_bytes, volume.clusters);
	row(line);

	static const char *const kDirs[] = {"", "image", "image/qca6390", "image/wlan", "qca6390"};
	for (unsigned d = 0; d < sizeof(kDirs) / sizeof(kDirs[0]); d++) {
		struct FatLister list = {};
		line[0] = '\0';
		list.row = line;
		list.used = 0;
		const int listed = fatList(&volume, kDirs[d], fatEmit, &list);
		snprintf(line2, sizeof(line2), "[dby-pcie] m7 ls /%s n=%u %s", kDirs[d], list.count,
		    listed == FatOk ? "" : "(unreadable)");
		row(line2);
		if (list.count) row(line);
	}
	return 0;
}

int stage7b(void)
{
	/* Every LUN, not just LUN 0: M7a found a partition called modem on LUN 4, and native_ufs_abs_read
	 * can only reach LUN 0 while native_ufs_part_read takes the LUN - so the table that holds the
	 * firmware may be a different LUN's, and reading only LUN 0 would have hidden it.  The GPT layout is
	 * the driver's, not the spec's: 4096-byte blocks, header at block 1 (its own reader refuses LBA 0
	 * and memcmps "EFI PART" at the start of a block), entry array at block `entriesLba` in the same
	 * 4096-byte units. */
	static uint8_t table[12288];
	static unsigned found;
	char line[120];
	char packed[120];
	unsigned lun;
	char flag;

	if (!native_ufs_part_read || !native_ufs_abs_read) {
		row("[dby-pcie] m7b ufs hooks not linked");
		return 8;
	}
	for (lun = 0; lun < 8; lun++) {
		/* LUN 0 goes through the absolute hook, which is the one the touch path uses; the rest only
		 * exists as a LUN-taking read.  Both are truthy-on-success. */
		const int read = lun == 0 ? native_ufs_abs_read(1, 3, table)
		    : native_ufs_part_read(lun, 1, 3, table);
		if (read != 1) continue;
		if (memcmp(table, "EFI PART", 8) != 0) continue;

		const uint8_t *header = table;
		const uint32_t entriesLba = static_cast<uint32_t>(header[72]) |
		    (static_cast<uint32_t>(header[73]) << 8) | (static_cast<uint32_t>(header[74]) << 16) |
		    (static_cast<uint32_t>(header[75]) << 24);
		const uint32_t numParts = static_cast<uint32_t>(header[80]) |
		    (static_cast<uint32_t>(header[81]) << 8) | (static_cast<uint32_t>(header[82]) << 16) |
		    (static_cast<uint32_t>(header[83]) << 24);
		const uint32_t entrySize = static_cast<uint32_t>(header[84]) |
		    (static_cast<uint32_t>(header[85]) << 8);
		snprintf(line, sizeof(line), "[dby-pcie] m7b lun=%u entries_lba=%u num=%u size=%u", lun,
		    entriesLba, numParts, entrySize);
		row(line);
		if (entrySize < 128 || numParts == 0 || numParts > 256 || entriesLba < 1) continue;
		const uint64_t arrayByte = (static_cast<uint64_t>(entriesLba) - 1u) * 4096u;
		if (arrayByte >= sizeof(table)) {
			snprintf(line, sizeof(line), "[dby-pcie] m7b lun=%u entries beyond the three blocks read", lun);
			row(line);
			continue;
		}
		found++;
		const uint8_t *entries = table + arrayByte;
		unsigned used = 0, printed = 0;
		packed[0] = '\0';
		for (uint32_t i = 0; i < numParts; i++) {
			const uint8_t *e = entries + static_cast<uint64_t>(i) * entrySize;
			if (arrayByte + static_cast<uint64_t>(i + 1) * entrySize > sizeof(table)) break;
			bool inUse = false;
			for (unsigned c = 0; c < 16; c++) if (e[c]) {inUse = true; break;}
			if (!inUse) continue;
			char name[40];
			unsigned n = 0;
			for (unsigned c = 0; c < 72 && n < 36; c += 2) {
				const uint16_t ch = static_cast<uint16_t>(static_cast<uint16_t>(e[56 + c]) |
				    static_cast<uint16_t>(static_cast<uint16_t>(e[57 + c]) << 8));
				if (!ch) break;
				name[n++] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?';
			}
			name[n] = '\0';
			if (!n) continue;
			/* Classify the partition's first block: the firmware is somewhere the GPT knows, and
			 * "which of these is a filesystem we can mount" is cheaper to read than to reason about.
			 * F = FAT (boot sector's media tag), e = ext4 magic, M = a Qualcomm MBN header, ? = none. */
			uint64_t firstLba = 0;
			memcpy(&firstLba, e + 32, 8);
			char kind = '?';
			if (firstLba) {
				static uint8_t probe[4096];
				const int got = native_ufs_part_read(lun, firstLba, 1, probe);
				if (got == 1) {
					if ((probe[3] == 'M' && probe[4] == 'S') ||
					    (probe[82] == 'F' && probe[83] == 'A' && probe[84] == 'T')) kind = 'F';
					else if (probe[1080] == 0x53 && probe[1081] == 0xEF) kind = 'e';
					else if (probe[0] == 0xD1 && probe[1] == 0xDC) kind = 'M';
				}
			}
			char item[64];
			const int wrote = snprintf(item, sizeof(item), "%u=%s%c ", i, name, kind);
			if (wrote <= 0) continue;
			if (used + static_cast<unsigned>(wrote) + 12u < 112u) {
				memcpy(packed + used, item, static_cast<size_t>(wrote));
				used += static_cast<unsigned>(wrote);
				packed[used] = '\0';
			} else {
				snprintf(line, sizeof(line), "[dby-pcie] m7b %s", packed);
				row(line);
				snprintf(packed, sizeof(packed), "%s", item);
				used = static_cast<unsigned>(strlen(packed));
			}
			printed++;
		}
		if (used) {
			snprintf(line, sizeof(line), "[dby-pcie] m7b %s", packed);
			row(line);
		}
		if (!printed) {
			snprintf(line, sizeof(line), "[dby-pcie] m7b lun=%u has no named entries", lun);
			row(line);
		}
	}
	flag = found ? 0 : 8;
	if (!found) row("[dby-pcie] m7b no gpt on any lun 0..7");
	return flag;
}


/* ---- M7d: what is inside the two FAT partitions -----------------------------------------------
 *
 * M7b's classifier answered the question the last three rounds could not: modem (LUN 4 entry 3) and
 * bluetooth (entry 4) both begin with a FAT boot sector, while everything else is ext4, an MBN header or
 * nothing.  fat_mount refuses them anyway (rc -2, FatNotFat) because package/FatVolume.h is deliberately
 * a FAT12-refusing, 4085-cluster-minimum reader for the ADSP image - so this stage reads the two volumes
 * the way their own geometry says: parse the BPB, then walk the root directory, which on FAT12 and FAT16
 * is a fixed run that needs no allocation table at all.  Nothing is mounted and nothing is written.
 *
 * This is the listing that decides whether the wireless firmware ships inside one of these containers
 * (bluetooth is the plausible one - the same part carries both radios) or whether it is genuinely
 * absent from this unit.
 *
 * Returns 0 when at least one of the two volumes listed, 11 when neither did. */
struct Fat16Geom {
	uint32_t bps, spc, reserved, fats, rootEntries, fatSectors;
	uint64_t rootOffset, fatOffset, dataOffset;
};

/* One directory of a FAT12/FAT16 volume, with long names assembled and `depth` levels of descent.
 * The root is the fixed region the BPB describes; every subdirectory is a cluster chain followed
 * through the 16-bit allocation table.  Names are printed as they are found - the long name when the
 * 0x0F entries that precede an entry assemble into one, else the 8.3 name - and at most kPerDir of
 * them, because modem/image holds 366 files and a row per file would spend the whole census on it;
 * the `n=` row carries the true count. */
constexpr unsigned kPerDir = 16;

void fatEmitRow(const char *path, char *packed, unsigned *used, const char *item)
{
	if (!*used) return;
	char line[120];
	snprintf(line, sizeof(line), "[dby-pcie] m7d %s %s", path, packed);
	row(line);
	(void)item;
	*used = 0;
	packed[0] = '\0';
}

unsigned listFat16Dir(const struct PartitionReader *part, const struct Fat16Geom *g, uint32_t startCluster,
    int depth, const char *path)
{
	char packed[120];
	unsigned used = 0, shown = 0, total = 0;
	packed[0] = '\0';
	char longName[261];
	unsigned longLen = 0;
	bool haveLong = false;
	uint32_t cluster = startCluster;
	unsigned hops = 0;

	for (;;) {
		uint64_t base;
		uint32_t entries;
		if (cluster == 0) {
			base = g->rootOffset;
			entries = g->rootEntries;
		} else {
			if (cluster < 2 || cluster >= 0xFFF8u) break;
			base = g->dataOffset + static_cast<uint64_t>(cluster - 2u) *
			    static_cast<uint64_t>(g->bps * g->spc);
			entries = (g->bps * g->spc) / 32u;
		}
		for (uint32_t i = 0; i < entries; i++) {
			uint8_t entry[32];
			if (partitionRead(const_cast<struct PartitionReader *>(part), base + i * 32u, entry,
			    sizeof(entry)) != 0) return shown;
			if (entry[0] == 0x00) {cluster = 0xFFFFu; break;}
			if (entry[0] == 0xE5) {longLen = 0; haveLong = false; continue;}
			const uint8_t attr = entry[11];
			if (attr == 0x0F) {
				const unsigned seq = entry[0] & 0x3Fu;
				if (seq >= 1 && seq <= 20) {
					const unsigned at = (seq - 1u) * 13u;
					if (at + 13u < sizeof(longName)) {
						for (unsigned c = 0; c < 13; c++) {
							unsigned src;
							if (c < 5) src = 1 + c * 2;
							else if (c < 11) src = 14 + (c - 5) * 2;
							else src = 28 + (c - 11) * 2;
							const uint16_t ch = static_cast<uint16_t>(entry[src]) |
							    static_cast<uint16_t>(static_cast<uint16_t>(entry[src + 1]) << 8);
							longName[at + c] = (ch == 0xFFFF || ch == 0) ? '\0'
							    : ((ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?');
						}
						if (at + 13u > longLen) longLen = at + 13u;
						haveLong = true;
					}
				}
				continue;
			}
			if (attr & 0x08) {longLen = 0; haveLong = false; continue;}
			char name[13];
			unsigned n = 0;
			for (unsigned c = 0; c < 8 && entry[c] != ' '; c++) name[n++] = static_cast<char>(entry[c]);
			if ((attr & 0x10) == 0 && entry[8] != ' ') {
				name[n++] = '.';
				for (unsigned c = 8; c < 11 && entry[c] != ' '; c++) name[n++] = static_cast<char>(entry[c]);
			}
			name[n] = '\0';
			const char *shownName = name;
			if (haveLong && longLen) {
				longName[longLen] = '\0';
				unsigned l = 0;
				while (l < longLen && longName[l]) l++;
				if (l) shownName = longName;
			}
			longLen = 0;
			haveLong = false;
			if (!n) continue;
			if (strcmp(shownName, ".") == 0 || strcmp(shownName, "..") == 0) continue;
			total++;
			const uint32_t firstCluster = static_cast<uint32_t>(entry[26]) |
			    (static_cast<uint32_t>(entry[27]) << 8);
			/* descend before the cap so a directory with many files still gets its subdirectories
			 * visited; only the printing is capped. */
			if ((attr & 0x10) && depth > 0 && firstCluster >= 2) {
				char sub[160];
				snprintf(sub, sizeof(sub), "%s/%s", path, shownName);
				char line[120];
				fatEmitRow(path, packed, &used, "");
				const unsigned inn = listFat16Dir(part, g, firstCluster, depth - 1, sub);
				snprintf(line, sizeof(line), "[dby-pcie] m7d %s n=%u", sub, inn);
				row(line);
				continue;
			}
			if (shown >= kPerDir) continue;
			char item[64];
			const int wrote = snprintf(item, sizeof(item), "%s%s ", (attr & 0x10) ? "/" : "", shownName);
			if (wrote <= 0) continue;
			if (used + static_cast<unsigned>(wrote) + 8u < 112u) {
				memcpy(packed + used, item, static_cast<size_t>(wrote));
				used += static_cast<unsigned>(wrote);
				packed[used] = '\0';
			} else {
				char line[120];
				snprintf(line, sizeof(line), "[dby-pcie] m7d %s %s", path, packed);
				row(line);
				snprintf(packed, sizeof(packed), "%s", item);
				used = static_cast<unsigned>(strlen(packed));
			}
			shown++;
		}
		if (cluster == 0 && !startCluster) break;               /* FAT16 fixed root is one run */
		if (cluster >= 0xFFF8u) break;
		if (hops++ > 2048) break;
		uint8_t fatEntry[2];
		if (partitionRead(const_cast<struct PartitionReader *>(part), g->fatOffset +
		    static_cast<uint64_t>(cluster) * 2u, fatEntry, sizeof(fatEntry)) != 0) break;
		const uint32_t next = static_cast<uint32_t>(fatEntry[0]) | (static_cast<uint32_t>(fatEntry[1]) << 8);
		if (next == cluster) break;
		cluster = next;
	}
	if (used) {
		char line[120];
		snprintf(line, sizeof(line), "[dby-pcie] m7d %s %s", path, packed);
		row(line);
	}
	if (total > shown) {
		char line[120];
		snprintf(line, sizeof(line), "[dby-pcie] m7d %s shown=%u of %u", path, shown, total);
		row(line);
	}
	return total;
}

int stage7d(void)
{
	row("[dby-pcie] m7d enter");
	static const struct {uint32_t lun, index; const char *name;} kParts[] = {
	    {4, 3, "modem"}, {4, 4, "bluetooth"},
	};
	char line[120];
	unsigned listed = 0;

	for (unsigned pidx = 0; pidx < sizeof(kParts) / sizeof(kParts[0]); pidx++) {
		const uint32_t lun = kParts[pidx].lun;
		static uint8_t table[12288];
		if (native_ufs_part_read(lun, 1, 3, table) != 1 || memcmp(table, "EFI PART", 8) != 0) {
			snprintf(line, sizeof(line), "[dby-pcie] m7d %s: no gpt on lun %u", kParts[pidx].name, lun);
			row(line);
			continue;
		}
		const uint32_t entriesLba = static_cast<uint32_t>(table[72]) |
		    (static_cast<uint32_t>(table[73]) << 8) | (static_cast<uint32_t>(table[74]) << 16) |
		    (static_cast<uint32_t>(table[75]) << 24);
		const uint32_t entrySize = static_cast<uint32_t>(table[84]) | (static_cast<uint32_t>(table[85]) << 8);
		const uint64_t arrayByte = (static_cast<uint64_t>(entriesLba) - 1u) * 4096u;
		if (entrySize < 128 || arrayByte >= sizeof(table)) continue;
		const uint8_t *e = table + arrayByte + static_cast<uint64_t>(kParts[pidx].index) * entrySize;
		uint64_t firstLba = 0, lastLba = 0;
		memcpy(&firstLba, e + 32, 8);
		memcpy(&lastLba, e + 40, 8);
		char ename[40];
		unsigned en = 0;
		for (unsigned c = 0; c < 72 && en < 36; c += 2) {
			const uint16_t ch = static_cast<uint16_t>(static_cast<uint16_t>(e[56 + c]) |
			    static_cast<uint16_t>(static_cast<uint16_t>(e[57 + c]) << 8));
			if (!ch) break;
			ename[en++] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?';
		}
		ename[en] = '\0';
		if (!firstLba || lastLba < firstLba || strcmp(ename, kParts[pidx].name) != 0) continue;

		struct PartitionReader part = {};
		part.lun = lun;
		part.first = firstLba;
		part.blocks = lastLba - firstLba + 1;

		uint8_t boot[512];
		if (partitionRead(&part, 0, boot, sizeof(boot)) != 0) continue;
		struct Fat16Geom g = {};
		g.bps = static_cast<uint32_t>(boot[11]) | (static_cast<uint32_t>(boot[12]) << 8);
		g.spc = boot[13];
		g.reserved = static_cast<uint32_t>(boot[14]) | (static_cast<uint32_t>(boot[15]) << 8);
		g.fats = boot[16];
		g.rootEntries = static_cast<uint32_t>(boot[17]) | (static_cast<uint32_t>(boot[18]) << 8);
		g.fatSectors = static_cast<uint32_t>(boot[22]) | (static_cast<uint32_t>(boot[23]) << 8);
		if (g.bps < 512 || g.bps > 4096 || (g.bps & (g.bps - 1u)) || !g.spc || g.spc > 128 ||
		    (g.spc & (g.spc - 1u)) || !g.reserved || !g.fats || g.fats > 2 || !g.rootEntries ||
		    g.rootEntries > 512 || !g.fatSectors) {
			snprintf(line, sizeof(line), "[dby-pcie] m7d %s: bpb bps=%u spc=%u res=%u fats=%u root=%u fat=%u",
			    kParts[pidx].name, g.bps, g.spc, g.reserved, g.fats, g.rootEntries, g.fatSectors);
			row(line);
			continue;
		}
		g.rootOffset = static_cast<uint64_t>(g.reserved + g.fats * g.fatSectors) * g.bps;
		g.fatOffset = static_cast<uint64_t>(g.reserved) * g.bps;
		g.dataOffset = g.rootOffset + static_cast<uint64_t>(g.rootEntries) * 32u;
		snprintf(line, sizeof(line), "[dby-pcie] m7d %s first=%llu root=%llu entries=%u cluster=%u",
		    kParts[pidx].name, firstLba, g.rootOffset, g.rootEntries, g.bps * g.spc);
		row(line);

		const unsigned n = listFat16Dir(&part, &g, 0, 2, kParts[pidx].name);
		snprintf(line, sizeof(line), "[dby-pcie] m7d %s total=%u", kParts[pidx].name, n);
		row(line);
		if (n) listed++;
	}
	return listed ? 0 : 11;
}


/* One directory level: find `name` (long name first, then 8.3, case-insensitive) and return its first
 * cluster and size.  `startCluster` 0 means the fixed FAT16 root.  This is the read path the listing
 * stage only printed. */
bool fat16Find(const struct PartitionReader *part, const struct Fat16Geom *g, uint32_t startCluster,
    const char *name, uint32_t *firstCluster, uint32_t *bytes, bool *isDir)
{
	char longName[261];
	unsigned longLen = 0;
	bool haveLong = false;
	uint32_t cluster = startCluster;
	unsigned hops = 0;
	for (;;) {
		uint64_t base;
		uint32_t entries;
		if (cluster == 0) {
			base = g->rootOffset;
			entries = g->rootEntries;
		} else {
			if (cluster < 2 || cluster >= 0xFFF8u) return false;
			base = g->dataOffset + static_cast<uint64_t>(cluster - 2u) * static_cast<uint64_t>(g->bps * g->spc);
			entries = (g->bps * g->spc) / 32u;
		}
		for (uint32_t i = 0; i < entries; i++) {
			uint8_t entry[32];
			if (partitionRead(const_cast<struct PartitionReader *>(part), base + i * 32u, entry,
			    sizeof(entry)) != 0) return false;
			if (entry[0] == 0x00) return false;
			if (entry[0] == 0xE5) {longLen = 0; haveLong = false; continue;}
			const uint8_t attr = entry[11];
			if (attr == 0x0F) {
				const unsigned seq = entry[0] & 0x3Fu;
				if (seq >= 1 && seq <= 20) {
					const unsigned at2 = (seq - 1u) * 13u;
					if (at2 + 13u <= sizeof(longName)) {
						for (unsigned c = 0; c < 13; c++) {
							unsigned src;
							if (c < 5) src = 1 + c * 2;
							else if (c < 11) src = 14 + (c - 5) * 2;
							else src = 28 + (c - 11) * 2;
							const uint16_t ch = static_cast<uint16_t>(entry[src]) |
							    static_cast<uint16_t>(static_cast<uint16_t>(entry[src + 1]) << 8);
							longName[at2 + c] = (ch == 0xFFFF || ch == 0) ? '\0'
							    : ((ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?');
						}
						if (at2 + 13u > longLen) longLen = at2 + 13u;
						haveLong = true;
					}
				}
				continue;
			}
			if (attr & 0x08) {longLen = 0; haveLong = false; continue;}
			char shortName[13];
			unsigned n = 0;
			for (unsigned c = 0; c < 8 && entry[c] != ' '; c++) shortName[n++] = static_cast<char>(entry[c]);
			if ((attr & 0x10) == 0 && entry[8] != ' ') {
				shortName[n++] = '.';
				for (unsigned c = 8; c < 11 && entry[c] != ' '; c++) shortName[n++] = static_cast<char>(entry[c]);
			}
			shortName[n] = '\0';
			bool match = false;
			if (haveLong && longLen) {
				longName[longLen] = '\0';
				unsigned l = 0;
				while (l < longLen && longName[l]) l++;
				longName[l] = '\0';
				match = l > 0 && strcasecmp(longName, name) == 0;
			}
			if (!match) match = strcasecmp(shortName, name) == 0;
			longLen = 0;
			haveLong = false;
			if (!match) continue;
			*firstCluster = static_cast<uint32_t>(entry[26]) | (static_cast<uint32_t>(entry[27]) << 8);
			*bytes = static_cast<uint32_t>(entry[28]) | (static_cast<uint32_t>(entry[29]) << 8) |
			    (static_cast<uint32_t>(entry[30]) << 16) | (static_cast<uint32_t>(entry[31]) << 24);
			if (isDir) *isDir = (attr & 0x10) != 0;
			return true;
		}
		if (cluster == 0) return false;                    /* the fixed root is one run */
		if (hops++ > 2048) return false;
		uint8_t fatEntry[2];
		if (partitionRead(const_cast<struct PartitionReader *>(part), g->fatOffset +
		    static_cast<uint64_t>(cluster) * 2u, fatEntry, sizeof(fatEntry)) != 0) return false;
		const uint32_t next = static_cast<uint32_t>(fatEntry[0]) | (static_cast<uint32_t>(fatEntry[1]) << 8);
		if (next == cluster) return false;
		cluster = next;
	}
}

/* ---- M6c: the image the tablet actually ships, handed to BHI ------------------------------------
 *
 * The recursive listing settled where the wireless firmware lives on this unit: modem (LUN 4) ->
 * /image/qca6390 -> amss20.bin, alongside Data.msc and the bdwlan.e01..e0e board files.  It is inside a
 * 4Kn FAT16 volume that package/FatVolume.h refuses (its reader is FAT12-free with a cluster floor),
 * which is why this stage carries its own geometry and chain walker: find the path by name with long
 * names assembled, follow the cluster chain into the arena M5 allocated, clean the range, and ring the
 * boot host interface.
 *
 * Read-only on the volume side; the only writes are the BHI registers.  Returns 0 when the endpoint
 * left PBL, 12 when the image could not be read, 10 when BHI did not take it. */
/* One path component at a time, root first: the walker below only needs to descend, and every
 * directory in this volume is small. */
/* Walk a path one component at a time, carrying the directory's own cluster down: the first draft
 * restarted at the root for every component, which is why "image/qca6390/amss20.bin" was reported as
 * missing even though the listing had printed it.  A component that is not found prints which one and
 * where, so a wrong path is a reading rather than a shrug. */
bool fat16LookupAt(const struct PartitionReader *part, const struct Fat16Geom *g, uint32_t startCluster,
    const char *path, uint32_t *firstCluster, uint32_t *bytes, const char *fullPath)
{
	char component[64];
	const char *at = path;
	char *out = component;
	while (*at && *at != '/' && static_cast<size_t>(out - component) < sizeof(component) - 1) *out++ = *at++;
	*out = '\0';
	if (!component[0]) return false;

	uint32_t fc = 0, size = 0;
	bool dir = false;
	if (!fat16Find(part, g, startCluster, component, &fc, &size, &dir)) {
		char line[120];
		snprintf(line, sizeof(line), "[dby-pcie] m6c %s: component \"%s\" not found at cluster %u",
		    fullPath, component, startCluster);
		row(line);
		return false;
	}
	if (*at == '/') {
		if (!dir || fc < 2) {
			char line[120];
			snprintf(line, sizeof(line), "[dby-pcie] m6c %s: \"%s\" is not a directory", fullPath, component);
			row(line);
			return false;
		}
		return fat16LookupAt(part, g, fc, at + 1, firstCluster, bytes, fullPath);
	}
	*firstCluster = fc;
	*bytes = size;
	return true;
}

bool fat16Lookup(const struct PartitionReader *part, const struct Fat16Geom *g, const char *path,
    uint32_t *firstCluster, uint32_t *bytes)
{
	return fat16LookupAt(part, g, 0, path, firstCluster, bytes, path);
}

/* ---- M6e: why does the doorbell do nothing? ------------------------------------------------------
 *
 * The read-back row answered the first question: address, size and doorbell all read back exactly as
 * written, so the outbound window, BAR0 and the endpoint's decode are all fine.  What stays zero is
 * BHI_STATUS and BHI_EXECENV - the device never reacts.  Three preambles are plausible and the record
 * cannot settle which one this silicon wants, so this stage tries them in one boot, in the order that
 * costs least, and prints what each did:
 *
 *   1. plain: clear BHI_STATUS and ring, which is what the last two boots already did (control).
 *   2. MHI_CTRL enable (bit 0): mainline does not touch it for a BHI download, but the opinion in this
 *      repository's own record disagrees with the silicon often enough to be worth one test.
 *   3. SOC reset request: drivers/bus/mhi/host/internal.h defines MHI_SOC_RESET_REQ_OFFSET 0xb0 and
 *      MHI_SOC_RESET_REQ bit 0, which mhi_soc_reset() writes through the BHI window before a download.
 *
 * Each attempt clears BHI_STATUS first, so an attempt that works cannot be confused with one that
 * inherited the previous attempt's state.  The row per attempt carries the status it ended with.
 *
 * Returns the number of the attempt that made BHI_STATUS move, or 0 when none did. */


int stage6c(uint64_t *arenaPa, uint32_t *imageBytes, uint32_t *eeAfter)
{
	*arenaPa = 0; *imageBytes = 0; *eeAfter = 0;
	char line[120];
	/* Entry marker: the stage-10 boot printed no m6c row at all while the rows on either side of the
	 * dispatch did print, so this separates "never called" from "called and returned without a row". */
	row("[dby-pcie] m6c enter");

	if (!gDmaArena || !gDmaArenaPa || !native_ufs_part_read) {
		row("[dby-pcie] m6c no arena or no ufs hooks");
		return 12;
	}

	/* The same GPT lookup M7d does: modem is entry 3 on LUN 4. */
	static uint8_t table[12288];
	if (native_ufs_part_read(4, 1, 3, table) != 1 || memcmp(table, "EFI PART", 8) != 0) {
		row("[dby-pcie] m6c no gpt on lun 4");
		return 12;
	}
	const uint32_t entriesLba = static_cast<uint32_t>(table[72]) |
	    (static_cast<uint32_t>(table[73]) << 8) | (static_cast<uint32_t>(table[74]) << 16) |
	    (static_cast<uint32_t>(table[75]) << 24);
	const uint32_t entrySize = static_cast<uint32_t>(table[84]) | (static_cast<uint32_t>(table[85]) << 8);
	const uint64_t arrayByte = (static_cast<uint64_t>(entriesLba) - 1u) * 4096u;
	if (entrySize < 128 || arrayByte >= sizeof(table)) {
		row("[dby-pcie] m6c gpt geometry unusable");
		return 12;
	}
	const uint8_t *e = table + arrayByte + 3u * entrySize;
	uint64_t firstLba = 0, lastLba = 0;
	memcpy(&firstLba, e + 32, 8);
	memcpy(&lastLba, e + 40, 8);
	if (!firstLba || lastLba < firstLba) {
		row("[dby-pcie] m6c modem entry has no extent");
		return 12;
	}

	struct PartitionReader part = {};
	part.lun = 4;
	part.first = firstLba;
	part.blocks = lastLba - firstLba + 1;

	uint8_t boot[512];
	if (partitionRead(&part, 0, boot, sizeof(boot)) != 0) {
		row("[dby-pcie] m6c boot sector unreadable");
		return 12;
	}
	struct Fat16Geom g = {};
	g.bps = static_cast<uint32_t>(boot[11]) | (static_cast<uint32_t>(boot[12]) << 8);
	g.spc = boot[13];
	g.reserved = static_cast<uint32_t>(boot[14]) | (static_cast<uint32_t>(boot[15]) << 8);
	g.fats = boot[16];
	g.rootEntries = static_cast<uint32_t>(boot[17]) | (static_cast<uint32_t>(boot[18]) << 8);
	g.fatSectors = static_cast<uint32_t>(boot[22]) | (static_cast<uint32_t>(boot[23]) << 8);
	if (g.bps < 512 || g.bps > 4096 || (g.bps & (g.bps - 1u)) || !g.spc || g.spc > 128 ||
	    (g.spc & (g.spc - 1u)) || !g.reserved || !g.fats || g.fats > 2 || !g.rootEntries ||
	    g.rootEntries > 512 || !g.fatSectors) {
		row("[dby-pcie] m6c modem volume geometry unusable");
		return 12;
	}
	g.rootOffset = static_cast<uint64_t>(g.reserved + g.fats * g.fatSectors) * g.bps;
	g.fatOffset = static_cast<uint64_t>(g.reserved) * g.bps;
	g.dataOffset = g.rootOffset + static_cast<uint64_t>(g.rootEntries) * 32u;

	/* amss20.bin first for this boot, with the comment on kBhiWholeFile saying why: the two lines together
	 * hand over one complete ELF image rather than a slice, which is the input this record had never
	 * measured on an awake endpoint.  The order below is a preference, not a claim: the first name that
	 * resolves in the volume wins, and m3.bin and amss.bin stay behind it as fallbacks.
	 */
	static const char *const kPaths[] = {
	    "image/qca6390/amss20.bin", "image/qca6390/m3.bin", "image/qca6390/amss.bin",
	    "image/amss20.bin", "image/m3.bin",
	};
	uint32_t cluster = 0, bytes = 0;
	const char *chosen = nullptr;
	for (unsigned i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); i++) {
		if (fat16Lookup(&part, &g, kPaths[i], &cluster, &bytes) && cluster >= 2 && bytes > 0 &&
		    bytes <= gDmaArenaBytes) {
			chosen = kPaths[i];
			break;
		}
	}
	if (!chosen) {
		row("[dby-pcie] m6c image not found in modem/image/qca6390");
		return 12;
	}

	uint8_t *dst = static_cast<uint8_t *>(gDmaArena->getBytesNoCopy());
	if (!dst) return 12;
	unsigned copied = 0, hops = 0;
	uint32_t at = cluster;
	while (at >= 2 && at < 0xFFF8u && copied < bytes && hops < 4096) {
		const uint64_t off = g.dataOffset + static_cast<uint64_t>(at - 2u) *
		    static_cast<uint64_t>(g.bps * g.spc);
		const unsigned chunk = (bytes - copied < g.bps * g.spc) ? bytes - copied : g.bps * g.spc;
		if (partitionRead(&part, off, dst + copied, chunk) != 0) break;
		copied += chunk;
		uint8_t fatEntry[2];
		if (partitionRead(&part, g.fatOffset + static_cast<uint64_t>(at) * 2u, fatEntry, sizeof(fatEntry)) != 0) break;
		const uint32_t next = static_cast<uint32_t>(fatEntry[0]) | (static_cast<uint32_t>(fatEntry[1]) << 8);
		if (next == at) break;
		at = next;
		hops++;
	}
	snprintf(line, sizeof(line), "[dby-pcie] m6c %s bytes=%u copied=%u pa=%llx", chosen, bytes, copied,
	    gDmaArenaPa);
	row(line);
	if (copied != bytes) {
		row("[dby-pcie] m6c short read, image not handed to bhi");
		return 12;
	}

	/* Clean AND invalidate, not just clean.  The sibling port's record is explicit that the boot loader's
	 * page 1 differed in DRAM from the CPU's write-combine alias, and that rewriting through the cached
	 * mapping plus a clean-invalidate (dc civac) by VA was what made the handover work (T970WifiStack.cpp:
	 * preBhiImage()).  A write-combined line that is only cleaned can leave the device reading what is
	 * still in the cache, which is indistinguishable from a device that ignores its doorbell. */
	for (uint64_t c = reinterpret_cast<uint64_t>(dst) & ~63ull;
	     c < reinterpret_cast<uint64_t>(dst) + bytes; c += 64ull) {
		__asm__ volatile("dc civac, %0" :: "r"(c) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* The SOC-reset experiment is no longer run here: it belongs to the power-on sequence in stage6k, which
	 * runs during the handoff in the order the working same-chip port uses - and its earlier failure was a
	 * consequence of the subsystem being unpowered.  The rails and clocks are no longer part of that
	 * sequence: they are voted at the probe's entry, before the chip is enabled (wifiPowerVote). */
	/* What the device will actually read, checked the way the working same-chip port checks it: through a
	 * second, uncached mapping of the same physical page, compared with the bytes the CPU wrote.  Until now
	 * this probe has only ever verified that its own READS of the registers came back - never that the image
	 * in DRAM is what the file says - so a cache flush that did not do what its comment claims would be
	 * invisible, and it would look exactly like what the rows show: the device reads something, spends a
	 * fixed time on it and rejects it, with the same verdict for every image because it never sees any of
	 * them.  Four words from each side plus a running sum, so a difference is visible and not just asserted. */
	{
		Window alias = {};
		const uint32_t span = 0x1000;
		if (mapWindow(alias, gDmaArenaPa, span)) {
			uint32_t seen = 0, expect = 0;
			for (uint32_t off = 0; off < span; off += 4) seen = seen * 31u + rd(alias.va, off);
			const uint32_t *cpu = reinterpret_cast<const uint32_t *>(dst);
			for (uint32_t i = 0; i < span / 4u; i++) expect = expect * 31u + cpu[i];
			snprintf(line, sizeof(line),
			    "[dby-pcie] m6i dram %08x %08x %08x %08x sum=%08x cpu=%08x %s",
			    rd(alias.va, 0), rd(alias.va, 4), rd(alias.va, 8), rd(alias.va, 12), seen, expect,
			    seen == expect ? "match" : "DIFFER");
			row(line);
			unmapWindow(alias);
		} else {
			row("[dby-pcie] m6i the arena's uncached alias did not map, image not verified");
		}
	}

	unsigned waited = 0;
	uint32_t ee = kMhiEePbl;
	/* Which slice of this file is the boot loader.  kBhiWholeFile decides, and it says why in one place;
	 * what matters here is that the row names the slice actually handed over, so two boots with different
	 * settings are comparable readings rather than a memory of which one was which. */
	const uint32_t offer = kBhiWholeFile ? bytes : (bytes < kBhiSblBytes ? bytes : kBhiSblBytes);
	snprintf(line, sizeof(line), "[dby-pcie] m6c offer %s %u of %u bytes",
	    kBhiWholeFile ? "whole" : "sbl", offer, bytes);
	row(line);
	const int rc = bhiHandoff(gDmaArenaPa, offer, &ee, &waited);
	*arenaPa = gDmaArenaPa;
	*imageBytes = bytes;
	*eeAfter = ee;
	return rc;
}

}  /* namespace */

extern "C" void dby_pcie_probe(void)
{
	uint32_t stage = 0;
	if (!PE_parse_boot_argn("dby-pcie", &stage, sizeof(stage)) || stage == 0) return;
	if (gDone) return;
	gDone = true;
	/* How many times this boot reaches the probe.  A stage-10 boot printed the whole ladder twice with
	 * two different arena addresses, so something calls in twice; a number in the row settles it instead
	 * of another guess. */
	static unsigned gRuns;
	{
		char run[120];
		snprintf(run, sizeof(run), "[dby-pcie] probe run #%u", gRuns + 1u);
		gRuns++;
		kprintf("%s\n", run);
	}

	native_dbypcie_stage = stage;
	native_dbypcie_rc = 0;
	/* The row mirror has to be pointed at its array before anything prints, and the power vote is the first
	 * thing that prints - it runs before any register work, which is the point of it. */
	gRow = &native_dbypcie_census[0][0];
	/* Power the subsystem before anything talks to it, which is what the mainline port does and what this
	 * probe has never done: its rails used to be voted inside the handoff, after WLAN_EN had been raised and
	 * the link trained.  A chip enabled without its rails can latch an internal state that a later vote does
	 * not undo, so the order matters more than the vote. */
	gPowerVoted = wifiPowerVote() == 0;
	/* A vote that did not happen must not pass silently: the boot goes on to raise WLAN_EN either way, and
	 * without this row that would look exactly like a successful vote at the handoff rows. */
	if (native_dbypcie_rc == 0 && !gPowerVoted) row("[dby-pcie] wifi power vote did not complete, chip enabled unpowered");


	/* ---- 1. always-on domains: GCC and the three TLMM tiles ------------------------------ */
	Window gcc = {};
	if (!mapWindow(gcc, kGcc, kGccSpan)) {
		native_dbypcie_rc = -1;
		row("[dby-pcie] gcc map failed");
		return;
	}
	char line[120];
	const uint32_t gdscr = rd(gcc.va, kGdscr);
	snprintf(line, sizeof(line), "[dby-pcie] gcc gdscr=%08x clkref=%08x refgen=%08x",
	    gdscr, rd(gcc.va, kClkref), rd(gcc.va, kRefgen));
	row(line);
	snprintf(line, sizeof(line), "[dby-pcie] halt %08x %08x %08x %08x %08x %08x",
	    rd(gcc.va, kHalts[0]), rd(gcc.va, kHalts[1]), rd(gcc.va, kHalts[2]),
	    rd(gcc.va, kHalts[3]), rd(gcc.va, kHalts[4]), rd(gcc.va, kHalts[5]));
	row(line);
	/* The rest of the surface M2 will write, so that stage can compare against a measured "before"
	 * from the same boot rather than against a guess.  Still reads only. */
	snprintf(line, sizeof(line), "[dby-pcie] gcc2 votes=%08x tbu=%08x phyaux=%08x ddrss=%08x",
	    rd(gcc.va, kVotes), rd(gcc.va, kTbu), rd(gcc.va, kPhyAux), rd(gcc.va, kDdrssSfTbu));
	row(line);
	snprintf(line, sizeof(line), "[dby-pcie] resets bcr=%08x phybcr=%08x phycom=%08x",
	    rd(gcc.va, kPcieBcr), rd(gcc.va, kPciePhyBcr), rd(gcc.va, kPciePhyComBcr));
	row(line);
	unmapWindow(gcc);

	Window tiles[3] = {};
	for (int t = 0; t < 3; t++) {
		if (!mapWindow(tiles[t], kTile[t], kTileSpan)) {
			/* Unmap what did map before returning: the sibling GPU probe had this exact defect
			 * (an unwind that only ran on the success path, recorded in the gpu area's findings). */
			for (int done = 0; done < t; done++) unmapWindow(tiles[done]);
			native_dbypcie_rc = -2;
			row("[dby-pcie] tlmm map failed");
			return;
		}
	}
	/* GPIO20 WLAN_EN and GPIO79 PERST#.  Two explicit pins rather than a loop over a stride: a third
	 * pin added here should be a visible edit, not a changed magic constant. */
	constexpr int kPins[2] = {20, 79};
	for (int index = 0; index < 2; index++) {
		const int pin = kPins[index];
		snprintf(line, sizeof(line), "[dby-pcie] pin%d west=%08x/%08x south=%08x/%08x north=%08x/%08x",
		    pin,
		    rd(tiles[0].va, static_cast<uint32_t>(pin) * kPinStride + kPinCfg),
		    rd(tiles[0].va, static_cast<uint32_t>(pin) * kPinStride + kPinIn),
		    rd(tiles[1].va, static_cast<uint32_t>(pin) * kPinStride + kPinCfg),
		    rd(tiles[1].va, static_cast<uint32_t>(pin) * kPinStride + kPinIn),
		    rd(tiles[2].va, static_cast<uint32_t>(pin) * kPinStride + kPinCfg),
		    rd(tiles[2].va, static_cast<uint32_t>(pin) * kPinStride + kPinIn));
		row(line);
	}
	/* Source tile plus what the pad currently holds.  Before M3 the CFG is the reset default, so a
	 * mismatch against the DT's active programming here is expected and is not a fault; after M3 the
	 * same row is how a bad pad write is caught. */
	const uint32_t cfg20 = rd(tiles[kPinTile[0]].va, 20u * kPinStride + kPinCfg);
	const uint32_t cfg79 = rd(tiles[kPinTile[1]].va, 79u * kPinStride + kPinCfg);
	snprintf(line, sizeof(line), "[dby-pcie] tile wlan_en=%s(src) cfg=%08x perst=%s(src) cfg=%08x",
	    kTileName[kPinTile[0]], cfg20, kTileName[kPinTile[1]], cfg79);
	row(line);
	for (int t = 0; t < 3; t++) unmapWindow(tiles[t]);

	/* ---- 2. M2a: bring the block up from GCC alone (stage 2 and later) -------------------- */
	uint32_t poweredGdscr = gdscr;
	if (stage >= 2) {
		const int m2 = stage2a(&poweredGdscr);
		if (m2 < 0) {
			native_dbypcie_rc = -5;
			snprintf(line, sizeof(line), "[dby-pcie] stage=%u rc=%d", stage, (int)native_dbypcie_rc);
			row(line);
			return;
		}
		native_dbypcie_rc = m2;
	}

	/* ---- 3. M3: pads, PHY and LTSSM (stage 3 and later) ---------------------------------- */
	if (stage >= 3 && (poweredGdscr & kGdscPwrOn) == 0)
		row("[dby-pcie] m3 skipped, block gated (M2a did not power it)");
	if (stage >= 3 && (poweredGdscr & kGdscPwrOn) != 0) {
		uint32_t id = 0, dllla = 0, socHw = 0;
		const int m3 = stage3a(&id, &dllla, &socHw);
		if (m3 < 0) {
			native_dbypcie_rc = -6;
			snprintf(line, sizeof(line), "[dby-pcie] stage=%u rc=%d", stage, (int)native_dbypcie_rc);
			row(line);
			return;
		}
		native_dbypcie_rc = m3;
	}

	/* ---- 3b. M4: the outbound windows and the endpoint's identity (stage 4 and later) ----- */
	if (stage >= 4 && native_dbypcie_rc == 0) {
		uint32_t cfgId = 0, cfgClass = 0, socHw = 0;
		const int m4 = stage4a(&cfgId, &cfgClass, &socHw);
		native_dbypcie_rc = m4 < 0 ? -8 : m4;
	}

	/* ---- 3c. M5: the endpoint's SMMU stream and DMA arena (stage 5 and later) ------------- */
	if (stage >= 5 && native_dbypcie_rc != 0)
		row("[dby-pcie] m5 skipped, the translator stage did not succeed");
	/* Everything below needs the chain up to M5, and only that: M6a's MHI census, M7a's firmware
	 * listing, M7b's partition table and M6b's download are independent of each other, and a failure in
	 * one of them must not silently delete the rest - which is exactly what happened on hardware when
	 * M7a's rc=8 gated M6b out of the boot.  busOk is the one gate; each later stage records its own
	 * verdict and the worst one is what the final row reports. */
	const bool busOk = native_dbypcie_rc == 0;
	if (stage >= 5 && busOk) {
		uint32_t slot = 0, bank = 0, smr = 0, s2cr = 0, bytes = 0;
		uint64_t pa = 0;
		const int m5 = stage5a(&slot, &bank, &smr, &s2cr, &pa, &bytes);
		native_dbypcie_rc = m5 < 0 ? -9 : m5;
	}

	/* ---- 3d. M6a: the MHI register block (stage 6 and later) ----------------------------- */
	if (stage >= 6 && busOk) {
		uint32_t reglen = 0, bhi = 0, bhie = 0, sts = 0;
		const int m6 = stage6a(&reglen, &bhi, &bhie, &sts);
		if (m6 != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m6 < 0 ? -10 : m6;
	}

	/* ---- 3e. M7a: what firmware the tablet ships (stage 7 and later) ---------------------- */
	if (stage >= 7 && busOk) {
		const int m7 = stage7a();
		if (m7 != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m7;
	}

	/* ---- 3e2. M7b: the partition table (stage 9 only) ------------------------------------ */
	/* The name listing is an inventory stage and it prints ten rows in one burst; at stage 10 the
	 * console has to carry the download rows instead, and a capture of exactly that boot showed the
	 * burst costing them - the same capture had a mangled command-line row (tail=o=2), which is the
	 * dropped-character signature.  So the inventory runs at stage 9 and the download boot skips it. */
	if (stage >= 9 && stage < 10) {
		const int m7b = stage7b();
		if (m7b != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m7b;
	}

	/* ---- 3d3. M6c: the shipped image into the arena and into BHI (stage 10 and later) ----- */
	{
		char gate[120];
		snprintf(gate, sizeof(gate), "[dby-pcie] gate stage=%u busOk=%u rc=%d hooks=%u", (unsigned)stage,
		    busOk ? 1u : 0u, (int)native_dbypcie_rc,
		    (native_ufs_part_read && native_ufs_part_find) ? 1u : 0u);
		row(gate);
	}
	if (stage >= 10 && busOk) {
		uint64_t pa = 0;
		uint32_t bytes = 0, ee = 0;
		const int m6c = stage6c(&pa, &bytes, &ee);
		if (m6c != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m6c;
	}

	/* ---- 3e3. M7d: the two FAT partitions' root directories (stage 9 and later) ----------- */
	if (stage >= 9) {
		const int m7d = stage7d();
		if (m7d != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m7d;
	}

	/* ---- 3d2. M6b: hand the endpoint its SBL (stage 8 and later) ------------------------- */
	/* M6b is the older path that looks for a volume named firmware/firmware_a/firmware_b/modem.  It
	 * must not run at all once M6c exists: at stage 10 it ran after M6c had already handed amss20.bin to
	 * BHI and its own failure verdict overwrote that success (stage=10 rc=9 after a handoff that
	 * worked), and had a volume ever mounted it would have rung the doorbell a second time on an
	 * endpoint that had left PBL.  Stage 10 means M6c. */
	if (stage >= 8 && stage < 10 && busOk) {
		uint64_t pa = 0;
		uint32_t bytes = 0, ee = 0;
		const int m6b = stage6b(&pa, &bytes, &ee);
		if (m6b != 0 && native_dbypcie_rc == 0) native_dbypcie_rc = m6b;
	}

	/* ---- 4. the controller itself, only when GCC says it is powered ---------------------- */
	if ((poweredGdscr & kGdscPwrOn) == 0) {
		if (stage < 2) native_dbypcie_rc = 1;   /* collapsed: nothing beyond this is safe to read */
		row("[dby-pcie] block gated, parf not read");
		snprintf(line, sizeof(line), "[dby-pcie] stage=%u rc=%d", stage, (int)native_dbypcie_rc);
		row(line);
		return;
	}

	Window parf = {}, dbi = {};
	if (!mapWindow(parf, kParf, kParfSpan)) {
		if (native_dbypcie_rc == 0) native_dbypcie_rc = -3;
		row("[dby-pcie] parf map failed");
		return;
	}
	if (!mapWindow(dbi, kDbi, kDbiSpan)) {
		unmapWindow(parf);
		if (native_dbypcie_rc == 0) native_dbypcie_rc = -4;
		row("[dby-pcie] dbi map failed");
		return;
	}
	const uint32_t sysCtrl = rd(parf.va, kParfSysCtrl);
	const uint32_t ltssm = rd(parf.va, kParfLtssm);
	snprintf(line, sizeof(line), "[dby-pcie] parf sys=%08x phy=%08x refclk=%08x dbi_base=%08x ltssm=%x",
	    sysCtrl, rd(parf.va, kParfPhyCtrl), rd(parf.va, kParfPhyRefclk),
	    rd(parf.va, kParfDbiBase), ltssm);
	row(line);
	unmapWindow(parf);

	/* The DBI identification read is the one that hung alioth with the pipe clock off, so it waits
	 * for the link: link-up means the endpoint is answering and the reference clock is running. */
	const uint32_t elbi = rd(dbi.va, kElbiStatus);
	const bool up = (elbi & kLinkUpBit) != 0;
	uint32_t id = 0;
	unsigned dllla = 0;
	if (up) {
		id = rd(dbi.va, kDbiId);
		dllla = (rd(dbi.va, kDbiLinkStatus) & kDlllaBit) ? 1u : 0u;
	}
	snprintf(line, sizeof(line), "[dby-pcie] link elbi=%08x up=%u dbi=%08x dllla=%u",
	    elbi, up ? 1u : 0u, id, dllla);
	row(line);
	unmapWindow(dbi);

	/* The verdict is read off the dbi= field, never encoded here: 17cb:1101 is QCA6390 and 17cb:1103 is
	 * QCA6490, both ath11k, and the pair decides which firmware set M6 asks for.  rc stays "this stage
	 * completed" so a non-zero rc always means the census stopped somewhere. */
	snprintf(line, sizeof(line), "[dby-pcie] stage=%u rc=%d", stage, (int)native_dbypcie_rc);
	row(line);
}
