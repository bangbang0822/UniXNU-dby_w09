// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded offline original lifecycle instrument; no desktop claim.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_gop_presenter_create(void);
static IOService *pendingLegacyDisplay;
static IOService *activeGOPPresenter,*activeGOPNub;
// GPU hand-off presenter (sheng-fb-gpu=1, phase 4): lives beside the display driver, which offers it frames instead of copying them.
static IOService *gpuHandoffPresenter;
extern "C" bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
static IOService *activeLegacyDisplay;
// Keep one diagnostic mapping alive until reboot, even after backend stop.
// The pulse is permanently closed before any system-frame copy.
static IOMemoryMap *diagnosticAnchorMapping;
extern "C" int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) __attribute__((weak_import));
static IORegistryEntry *unitScaleChosen;
static bool unitScaleAdded,unitRotationAdded;
static void finishUnitScalePolicy(bool keep) {
    if(unitScaleChosen && !keep) {
        if(unitScaleAdded)unitScaleChosen->removeProperty("display-scale");
        if(unitRotationAdded)unitScaleChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(unitScaleChosen);unitScaleAdded=false;unitRotationAdded=false;
}
static bool publishOrMatchWord(IORegistryEntry *entry,const char *key,uint32_t value,bool &added) {
    OSObject *previous=entry->getProperty(key);
    if(previous) {
        OSData *data=OSDynamicCast(OSData,previous);uint32_t word=0;
        if(!data || data->getLength()!=sizeof(word))return false;
        memcpy(&word,data->getBytesNoCopy(),sizeof(word));return word==value;
    }
    OSData *data=OSData::withBytes(&value,sizeof(value));
    bool result=data && entry->setProperty(key,data);OSSafeReleaseNULL(data);added=result;return result;
}
extern "C" {
volatile uint32_t native_legacy_stage;
/* The stage-3 gate at :557-567: [0] the sheng-fb-gpu argument as parsed, [1] how far the presenter got - 0 gate not
 * reached, 1 creating, 2 native_gop_presenter_create returned null, 3 no resource service, 4 init refused, 5 attach
 * refused, 6 start refused, 7 presenter live. The SWAP row's "gpu offers=0" cannot tell "no presenter" from "every
 * offer refused", because that counter only moves on an accepted offer (drivers/GopFramebuffer/NativeGOPPresenter.cpp:150, the counter bumped only on an accepted offer). */
volatile uint32_t native_legacy_gop_gate[2];
volatile int native_legacy_meta_status=-1;
volatile uint32_t native_legacy_init_result,native_legacy_start_result,native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match,native_legacy_object_size,native_legacy_released;
volatile uint64_t native_legacy_object,native_legacy_nub,native_legacy_workloop;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object,native_legacy_guard_caller;
volatile int native_legacy_surface_status=-1;
volatile uint32_t native_legacy_surface_found;
volatile uint64_t native_legacy_video_base,native_legacy_video_bytes;
volatile uint64_t native_legacy_gop_map,native_legacy_gop_virtual,native_legacy_gop_physical,native_legacy_gop_bytes;
volatile uint32_t native_legacy_gop_width,native_legacy_gop_height;
volatile uint32_t native_legacy_registration_calls,native_legacy_properties_length;
volatile uint32_t native_legacy_dt_attached;
volatile int native_legacy_unit_scale_status=-1;
volatile uint32_t native_legacy_unit_scale_calls,native_legacy_chosen_scale,native_legacy_chosen_rotation;
volatile uint32_t native_legacy_pm_stage,native_legacy_pm_calls,native_legacy_pm_callbacks,native_legacy_pm_ordinal;
volatile int native_legacy_pm_status=-1;
volatile uint32_t native_legacy_swap_validation_calls,native_legacy_swap_validation_result,native_legacy_swap_validation_reason;
volatile uint64_t native_legacy_swap_request,native_legacy_swap_surface;
volatile uint32_t native_legacy_transfer_calls;
volatile int native_legacy_transfer_status=-1;
volatile uint64_t native_legacy_transfer_bytes,native_legacy_transfer_request;
volatile uint32_t native_legacy_retirement_calls,native_legacy_retired_generation,native_legacy_presented_generation,native_legacy_retired_frames;
volatile uint64_t native_legacy_retired_current,native_legacy_retired_pending;
char native_legacy_properties[4096];
volatile uint32_t native_legacy_client_calls,native_legacy_client_type;
volatile int native_legacy_client_pid,native_legacy_client_status=-1;
volatile uint64_t native_legacy_client_task,native_legacy_client_object;
char native_legacy_client_class[64];
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern int boot_progress_prepare_scanout(void) __attribute__((weak_import));
extern void boot_progress_cancel_scanout(void) __attribute__((weak_import));
extern void boot_progress_stage(const char *) __attribute__((weak_import));
volatile uint32_t native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts;
volatile uint32_t native_legacy_prepare_failures,native_legacy_prepare_retried,native_legacy_prepare_max_attempts;
volatile uint64_t native_legacy_prepare_wait_us;
static uint64_t probeNowUs(void) {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
bool native_legacy_prepare_scanout(void) {
    native_legacy_prepare_calls=native_legacy_prepare_calls+1;native_legacy_prepare_reason=0;native_legacy_prepare_attempts=0;
    if (!boot_progress_prepare_scanout) return true;
    if (ml_at_interrupt_context()) {native_legacy_prepare_reason=1;return false;}
    if (!ml_get_interrupts_enabled()) {native_legacy_prepare_reason=2;return false;}
    // Yield to any in-flight diagnostic writer; never spin on a preempted
    // holder or claim a frame that the console can still overwrite.
    const uint64_t began=probeNowUs();
    for (unsigned attempt=0;attempt<100;++attempt) {
        native_legacy_prepare_attempts=attempt+1;
        if (boot_progress_prepare_scanout()) {
            if(attempt) {
                native_legacy_prepare_retried=native_legacy_prepare_retried+1;native_legacy_prepare_wait_us=native_legacy_prepare_wait_us+(probeNowUs()-began);
                if(attempt+1>native_legacy_prepare_max_attempts)native_legacy_prepare_max_attempts=attempt+1;
            }
            return true;
        }
        IOSleep(1);
    }
    native_legacy_prepare_failures=native_legacy_prepare_failures+1;native_legacy_prepare_wait_us=native_legacy_prepare_wait_us+(probeNowUs()-began);
    native_legacy_prepare_reason=3;
    if(boot_progress_cancel_scanout)boot_progress_cancel_scanout();
    return false;
}
/* dby-dpu=1: a read-only snapshot of what the boot loader left in the display controller. Nothing in
 * this port has ever programmed VIG/LM/CTL on this board - the platform SimpleFbDxe (uefi-dby_w09,
 * Platforms/Huawei/dby_w09Pkg/Drivers/SimpleFbDxe/SimpleFb.c: a pool-allocated logical canvas, the
 * real mPhysFb, and the DBY_PHYFB publish that hands mPhysFb to the OS) writes no display register at
 * all - so the pipeline that scans 0x9c000000 is the boot loader's, and "the panel
 * keeps the logo" has two readings this port cannot tell apart today: the controller may have stopped
 * fetching, or it may still fetch and the fault is on the DSI/INTF side. These reads separate them.
 * Rows carry the [dby-dpu] prefix because BootLog.c already prints [dpu]/[dpu pipes]/[dpu ctl] window
 * rows with a different field layout.
 *
 * Offsets are from the MDP node at 0xae01000. The board's vendor DTB (qcom,mdss_mdp@ae00000, read from
 * the stock device tree, not tracked in this repository) declares `reg = <0xae00000 0x84208>` and
 * `qcom,sde-off = 0x1000`, so the kernel's window at 0xae01000 covers 0x84208 - 0x1000 = 0x83208 bytes
 * and nothing past it (UniXNU-alioth/package/DpuProbe.cpp:36-38 makes the same correction; sheng's
 * 0x8f000 runs 44 KiB past the node). The same node gives ctl-off 0x2000 and sspp-off 0x5000/0x7000
 * from 0xae00000, i.e. CTL0 0x1000 and VIG0/VIG1 0x4000/0x6000 from the node; mainline's
 * dpu_6_0_sm8250.h agrees. SSPP: SRC_SIZE 0x00, SRC_XY 0x08, SRC0_ADDR 0x14, SRC1_ADDR 0x18,
 * SRC_YSTRIDE0 0x24, SRC_FORMAT 0x30, SRC_UNPACK_PATTERN 0x34, MULTIRECT_OPMODE 0x170. CTL: FLUSH
 * 0x18, TOP 0x14, INTF_ACTIVE 0xf4, FETCH_PIPE_ACTIVE 0xfc, PIPE_ACTIVE 0x12c, LAYER_ACTIVE 0x130.
 *
 * The power gate is read FIRST: a load from a collapsed MDSS domain is an external abort, and this
 * driver already lost one boot to exactly that read (NativeLegacyDisplay.cpp:259-264, 2026-11-15 23:43).
 * dispcc is at 0xaf00000: mdss_ahb_clk CBCR 0x2080 and mdss_mdp_clk CBCR 0x200c (bit0 enable, bit1
 * clock off), mdss_core_gdsc GDSCR 0x3000 (bit31 power on, bit0 collapse). The gate requires the
 * enable bit SET, not merely the clock-off bit clear: an all-zero CBCR is a clock that was never
 * enabled, and the alioth probe measured ahb=1/mdp=0x221 while the panel still showed the boot frame,
 * so the "on" shape is known on this SoC. If the gate fails the DPU block is not mapped at all. Every
 * store this probe makes is to its own stack; the MDP block is never written. */
static void dpuRegisterSnapshot() {
    IOMemoryMap *clockMap=nullptr;
    if(IODeviceMemory *clockRegion=IODeviceMemory::withRange(0xaf00000,0x20000)) {
        clockMap=clockRegion->map(kIOMapAnywhere|kIOMapInhibitCache);clockRegion->release();
    }
    if(!clockMap) {kprintf("[dby-dpu] dispcc map failed\n");return;}
    const volatile uint32_t *dispcc=reinterpret_cast<const volatile uint32_t *>(clockMap->getVirtualAddress());
    const uint32_t ahb=dispcc[0x2080/4],mdpClock=dispcc[0x200c/4],gdscr=dispcc[0x3000/4];
    /* The MDP gates say nothing about the DSI branches: they are separate CBCRs in the same block,
     * with the same bit0 enable / bit1 clock-off encoding. Offsets from mainline dispcc-sm8250.c,
     * which drives this SoC under its sm8250 name; its mdss_ahb (0x2080) and mdss_mdp (0x200c)
     * entries are exactly the two words above, so the map is anchored on both source and this
     * board readback. Two traps for the next editor: the -4 offset shift in that file lives in its
     * sm8350 branch and does not apply here, and these eight branches are BRANCH_HALT, while
     * mdss_mdp_lut_clk (0x201c, not read here) is BRANCH_VOTED and reads differently.
     * pclk0/pclk1 are the DSI pixel clocks, byte0/byte1 the PHY-side byte clocks, byte0_intf and
     * byte1_intf the INTF-side ones, esc0/esc1 the low-power escape clocks (esc on is normal during
     * command-mode LP and is not evidence that the link is in use). A gated branch means nothing is
     * being sent to the panel at that moment; it does NOT mean the panel is broken, because a
     * command-mode panel holds its last frame while the link is gated. This row prints before the
     * gate verdict on purpose: the DSI branches sit in dispcc, not inside mdss_core_gdsc, so they
     * stay meaningful when the MDP is gated - but an all-zero row together with power=0 is not a
     * reading at all (a CBCR reads 0x0 only when the block is not answering). */
    kprintf("[dby-dpu] dsi pclk0=%08x pclk1=%08x byte0=%08x byte0i=%08x byte1=%08x byte1i=%08x esc0=%08x esc1=%08x\n",
        dispcc[0x2004/4],dispcc[0x2008/4],dispcc[0x2028/4],dispcc[0x202c/4],
        dispcc[0x2030/4],dispcc[0x2034/4],dispcc[0x2038/4],dispcc[0x203c/4]);
    clockMap->release();
    const bool clocksOn=(ahb&1U) && !(ahb&2U) && (mdpClock&1U) && !(mdpClock&2U);
    const bool powered=(gdscr&0x80000000U) && !(gdscr&1U);
    kprintf("[dby-dpu] gdscr=%08x ahb=%08x mdp=%08x clock=%u power=%u\n",
        gdscr,ahb,mdpClock,clocksOn?1U:0U,powered?1U:0U);
    if(!clocksOn || !powered) {
        kprintf("[dby-dpu] block gated, mdp registers not read\n");return;
    }
    IOMemoryMap *map=nullptr;
    if(IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x83208)) {
        map=memory->map(kIOMapAnywhere|kIOMapInhibitCache);memory->release();
    }
    if(!map) {kprintf("[dby-dpu] mdp map failed\n");return;}
    const volatile uint32_t *mdp=reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for(unsigned k=0;k<2;++k) {
        const uint32_t base=k?0x6000U:0x4000U;
        kprintf("[dby-dpu] vig%u size=%08x xy=%08x src0=%08x src1=%08x ystride=%08x fmt=%08x unpack=%08x mr=%08x\n",
            k,mdp[(base+0x00)/4],mdp[(base+0x08)/4],mdp[(base+0x14)/4],mdp[(base+0x18)/4],
            mdp[(base+0x24)/4],mdp[(base+0x30)/4],mdp[(base+0x34)/4],mdp[(base+0x170)/4]);
    }
    /* Store safety, the test alioth used to separate "the value is wrong" from "the store itself is
     * blocked": write a VIG register back with the value it already holds and read it again. alioth
     * measured that even the old value hung, which is why it blamed the secure world; this board's
     * SMMU row shows the 0x821 secure stream is not in the SMR table, so whether a store is safe here
     * is unknown. A hang kills the boot at t=20 s, before any compositor swap, and everything printed
     * up to this row survives in the log slot; a return means the direct-scanout writes are worth
     * attempting. This is the only write the probe performs, and it writes back what it read. */
    {
        const uint32_t before=mdp[(0x4000+0x14)/4];
        volatile uint32_t *w=const_cast<volatile uint32_t *>(mdp);
        w[(0x4000+0x14)/4]=before;
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t after=mdp[(0x4000+0x14)/4];
        kprintf("[dby-dpu] store vig0.src0 %s before=%08x after=%08x\n",
            after==before?"ok":"MISMATCH",before,after);
    }
    for(unsigned k=0;k<2;++k) {
        const uint32_t base=k?0x1200U:0x1000U;
        kprintf("[dby-dpu] ctl%u flush=%08x top=%08x intf=%08x fetch=%08x pipe=%08x layer=%08x\n",
            k,mdp[(base+0x18)/4],mdp[(base+0x14)/4],mdp[(base+0xf4)/4],mdp[(base+0xfc)/4],
            mdp[(base+0x12c)/4],mdp[(base+0x130)/4]);
    }
    /* INTF1 (DSI0) at 0x6a800 and INTF2 (DSI1) at 0x6b000: the vendor node lists intf-off
     * 0x6b000/0x6b800/0x6c000/0x6c800 from 0xae00000 with type "dp dsi dsi dp", i.e. minus the
     * 0x1000 node offset; the 0x800 spacing and the 0x280 block length are mainline's catalog, so
     * only offsets below 0x280 are this block's own. Four words per interface, all read-only:
     *   +0x000 INTF_TIMING_ENGINE_EN   the REQUEST bit (dpu_hw_intf.c writes it; on DPU >=5 it is not
     *          the running state - dpu_hw_intf_get_status reads INTF_STATUS bit0 instead, which is
     *          why both are printed)
     *   +0x26c INTF_STATUS             bit0 = the timing engine is running (DPU 5.0+)
     *   +0x008 INTF_HSYNC_CTL          non-zero means video timing was programmed at all; zero together
     *          with a clear status bit means this INTF was never set up for video
     *   +0x084 INTF_DSI_CMD_MODE_TRIGGER_EN  1 = the command-mode trigger was configured
     * The four combinations separate "video running", "video requested but stopped", "command mode"
     * and "never configured", which the single enable bit cannot. CTL_INTF_ACTIVE (intf=0x6 above)
     * only says the CTL has these interfaces bound at flush time, so it is independent evidence. */
    kprintf("[dby-dpu] intf dsi0_en=%08x st=%08x hsync=%08x cmd=%08x dsi1_en=%08x st=%08x hsync=%08x cmd=%08x\n",
        mdp[0x6a800/4],mdp[(0x6a800+0x26c)/4],mdp[(0x6a800+0x008)/4],mdp[(0x6a800+0x084)/4],
        mdp[0x6b000/4],mdp[(0x6b000+0x26c)/4],mdp[(0x6b000+0x008)/4],mdp[(0x6b000+0x084)/4]);
    map->release();
    /* The VIG's SRC0_ADDR is an IOVA, not a physical address: if the display's apps_smmu context bank
     * translates it, the controller reads a different page from the one the copy writes and the panel
     * keeps the boot loader's frame. This reads the routing and the bank, and nothing else.
     *
     * No power gate here, unlike the MDP block above, and that is deliberate: the whole boot runs its
     * storage DMA through this SMMU (NativeUfs.cpp:456 maps 0x15000000 for the UFS path, :912 again for
     * the root volume), so by the time this probe runs (t>=20 s, userspace reading the root volume) it
     * is clocked and powered by definition. The failure mode is still locatable: an abort would appear
     * after the ctl1 row and before `smmu idr0=`.
     *
     * SMR/S2CR/CBAR and the bank registers are read-only; the same reads run in the window builder
     * (drivers/ShengUfs/DpuWindow.h:144-168) and in the UFS takeover that carries this tablet's root
     * volume (drivers/ShengUfs/SmmuTakeover.h:106-131, which is where the 0xffff SMR compare and the
     * 0x1000+4*bank CBAR read below are taken from). Routing needs both words: S2CR.TYPE says whether
     * the stream translates (0), bypasses (1: the fetch address is physical) or faults (2: the display
     * fetches nothing), and CBAR.Type says whether the bank is set up for stage-1 at all; SCTLR.M then
     * says stage 1 is actually on, and TCR2 decides whether 0x9c000000 (bit 31 set) even falls inside
     * TTBR0's range. Reading SCTLR alone cannot tell a bypass from a translation.
     *
     * Premise for the values: in a boot that passes sheng-dpu-scanout>=1, the bank stream 0x820
     * matches has already been rewritten by our own dpuEnable (SCTLR/TTBR0/TTBR1/TCR2/TCR/MAIR), so
     * those rows describe our table, not the firmware's. This payload has no scanout argument, so what
     * is printed here is the boot loader's state. The =1 round never wrote SMR/S2CR; it only installed
     * a stage-1 table on that bank. */
    IOMemoryMap *smmuMap=nullptr;
    if(IODeviceMemory *smmuRegion=IODeviceMemory::withRange(0x15000000,0xd0000)) {
        smmuMap=smmuRegion->map(kIOMapAnywhere|kIOMapInhibitCache);smmuRegion->release();
    }
    if(!smmuMap) {
        kprintf("[dby-dpu] smmu map failed\n");
    } else {
        const volatile uint32_t *smmu=reinterpret_cast<const volatile uint32_t *>(smmuMap->getVirtualAddress());
        unsigned groups=smmu[0x20/4]&0xffU;if(groups>128)groups=128;
        const unsigned banks=smmu[0x24/4]&0xffU;
        const unsigned pages=1U<<(((smmu[0x24/4]>>28)&7U)+1U);
        kprintf("[dby-dpu] smmu idr0=%08x idr1=%08x groups=%u banks=%u pages=%u\n",
            smmu[0x20/4],smmu[0x24/4],groups,banks,pages);
        if((pages+banks)*4096U>0xd0000U) {
            kprintf("[dby-dpu] smmu banks outside the 0xd0000 window, not read\n");
        } else {
            for(unsigned k=0;k<2;++k) {
                const uint32_t stream=k?0x821U:0x820U;
                bool found=false;
                for(unsigned i=0;i<groups;++i) {
                    const uint32_t smr=smmu[(0x800+4*i)/4];
                    if(!(smr&0x80000000U))continue;
                    const uint32_t mask=(smr>>16)&0x7fffU;
                    if((smr^stream)&0xffffU&~mask)continue;
                    const uint32_t s2cr=smmu[(0xc00+4*i)/4],type=(s2cr>>16)&3U,bank=s2cr&0xffU;
                    if(type!=0U || bank>=banks) {
                        kprintf("[dby-dpu] smmu stream=%x smr=%08x s2cr=%08x type=%u(%s) bank=%u\n",
                            stream,smr,s2cr,type,type==1U?"bypass":type==2U?"fault":"reserved",bank);
                        found=true;break;
                    }
                    const unsigned cb=((pages+bank)*4096)/4;
                    const uint32_t cbar=smmu[(0x1000+4*bank)/4];
                    kprintf("[dby-dpu] smmu stream=%x smr=%08x s2cr=%08x type=0 bank=%u cbar=%08x(%s) sctlr=%08x ttbr0=%08x ttbr1=%08x tcr=%08x tcr2=%08x mair=%08x fsr=%08x\n",
                        stream,smr,s2cr,bank,cbar,((cbar>>16)&3U)==1U?"stage1":"no-stage1",
                        smmu[cb],smmu[cb+0x20/4],smmu[cb+0x24/4],smmu[cb+0x30/4],
                        smmu[cb+0x10/4],smmu[cb+0x38/4],smmu[cb+0x58/4]);
                    found=true;break;
                }
                if(!found)kprintf("[dby-dpu] smmu stream=%x not mapped\n",stream);
            }
        }
        smmuMap->release();
    }
    /* DSI PHY/PLL state, the last block between the INTF and the panel. The board DT gives
     * dsi0 ctrl@ae94000, phy@ae94400, pll@ae94900 and dsi1 ctrl@ae96000, phy@ae96400, pll@ae96900,
     * so one 0x3000 window from 0xae94000 covers both. Register offsets are from the 7nm v4 PHY
     * driver (dsi_phy_7nm / dsi_pll_7nm): CMN_PHY_STATUS 0x140 bit0 = REFGEN ready; CMN_CTRL_0 0x24
     * bit5 = PLL bias enabled; CMN_CLK_CFG1 0x14 bits[1:0] = the PLL source mux (0 = internal);
     * PLL_COMMON_STATUS_ONE 0x1b0 bit0 = PLL locked, which is what dsi_pll_7nm_lock_status polls.
     * All four are read-only. Read them as three verdicts, not one: a locked PLL plus REFGEN ready
     * still does not prove the lanes are powered, so CMN_CTRL_0 bits[4:0] are the lane/LDO
     * power-down-B lines (dsi_7nm_phy_enable writes 0x7f, disable writes 0x00) while bit5 is the PLL
     * bias. lock=1 and status=1 and (cmn0 & 0x1f)==0x1f means the PHY is fully live and the panel is
     * the remaining suspect; the same two bits with (cmn0 & 0x1f)!=0x1f means the lanes are still
     * powered down, so "the DPU is fetching" is idle work; lock=0 means the DSI never transmitted.
     * The source mux is BITCLK_SEL (0 = internal PLL); CMN_CLK_CFG1 bit0 is the pclk mux, a different
     * field, so a cfg1 of 0 means internal PLL and nothing more. Self-check for the offsets: an
     * enabled PHY reads cmn0 with most low bits set (0x7f shape); 0xffffffff or an unrelated value
     * means the offset is wrong and the row must be thrown away rather than interpreted. */
    IOMemoryMap *phyMap=nullptr;
    if(IODeviceMemory *phyRegion=IODeviceMemory::withRange(0xae94000,0x3000)) {
        phyMap=phyRegion->map(kIOMapAnywhere|kIOMapInhibitCache);phyRegion->release();
    }
    if(!phyMap) {
        kprintf("[dby-dpu] dsi phy map failed\n");
    } else {
        const volatile uint32_t *phy=reinterpret_cast<const volatile uint32_t *>(phyMap->getVirtualAddress());
        const uint32_t p0=0x400,p1=0x900,p0b=0x2400,p1b=0x2900;
        kprintf("[dby-dpu] phy dsi0_status=%08x dsi0_cmn0=%08x dsi0_cfg1=%08x dsi0_lock=%08x | dsi1_status=%08x dsi1_cmn0=%08x dsi1_cfg1=%08x dsi1_lock=%08x\n",
            phy[(p0+0x140)/4],phy[(p0+0x24)/4],phy[(p0+0x14)/4],phy[(p1+0x1b0)/4],
            phy[(p0b+0x140)/4],phy[(p0b+0x24)/4],phy[(p0b+0x14)/4],phy[(p1b+0x1b0)/4]);
        /* The DSI host controllers sit at 0xae94000 (dsi0) and 0xae96000 (dsi1), i.e. offsets 0 and
         * 0x2000 in this same window, so no extra map is needed. Offsets and bit meanings are the
         * ones this project already runs on the same SoC: the UniXNU-alioth tree package
         * BacklightProbe.cpp (a sibling checkout, not part of this repository) defines HW_VERSION
         * 0x000 / CTRL 0x004 / STATUS 0x008 and records DSI_CTRL bit0 = DSI_EN, bit1 = video engine,
         * bit2 = command engine; upstream dsi_host.c states the same 0x000 version register and that
         * on DSI6G (this SoC) the whole map shifts by four.
         *
         * Self-check for the base: dsi_host.c records that offset 0x1f0 must read zero on a 6G
         * controller, and both controllers are the same IP, so dsi0_ver should equal dsi1_ver. A
         * non-zero scratch or two different versions means the base or the generation is wrong and
         * the row must be discarded.
         *
         * CTRL is a configuration register, not a progress one: bit0/bit1 say in which mode the host
         * was opened. Read it against the INTF row above, which already proved the DPU side is video
         * (timing engine set, HSYNC_CTL programmed, command trigger clear). ctrl with bit0=1, bit1=0
         * and bit2=1 is the interesting case: the DPU produces video timing while the host sits in
         * command mode, so no frame is ever turned into DSI packets. STATUS bit2 is the command-mode
         * busy bit, so STATUS=0 in video mode is normal and is not evidence of health. */
        kprintf("[dby-dpu] dsictl dsi0_ver=%08x dsi0_ctrl=%08x dsi0_status=%08x dsi0_scratch=%08x | dsi1_ver=%08x dsi1_ctrl=%08x dsi1_status=%08x dsi1_scratch=%08x\n",
            phy[0x000/4],phy[0x004/4],phy[0x008/4],phy[0x1f0/4],
            phy[0x2000/4],phy[0x2004/4],phy[0x2008/4],phy[0x21f0/4]);
        /* Lane configuration, read-only. Offsets from the mainline register database
         * drivers/gpu/drm/msm/registers/display/dsi_phy_7nm.xml (DSI_7nm_PHY_CMN domain):
         * LANE_CFG0 0x34, LANE_CFG1 0x38, LANE_CTRL0 0xa0, LANE_CTRL1 0xa4, LANE_STATUS0 0x148,
         * LANE_STATUS1 0x14c. Read them three ways only:
         *  - the two links should carry identical CFG0 and CFG1 (a dual-DSI panel is wired
         *    symmetrically); a difference between the links is the anomaly, while equality only says
         *    "symmetric", not "correct". LANE_CFG0/1 have no documented bit fields, and the loader's
         *    swap encoding need not equal the value our own driver writes (0x21/0x84), so a value
         *    other than those two is NOT a fault;
         *  - LANE_CTRL0 bits[4:0] should read 0x1f: dsi_7nm_phy_enable writes 0x1f for the four data
         *    lanes plus the clock lane, so a smaller mask means the loader enabled fewer lanes than
         *    the stream needs, which is a real mismatch;
         *  - LANE_CTRL1 bits 5 and 6 are the continuous-clock bits (dsi_7nm_set_continuous_clock sets
         *    both). Video mode needs them set; a clear pair means the clock lane leaves HS only with
         *    data, a genuine candidate for "the panel holds its last frame".
         * LANE_STATUS0/1 carry no documented bitfields in that database, so they are recorded raw
         * and must not be read as lane faults. The evidence that the PHY is driving its lanes is
         * already in CTRL_0=0x7f above (DLN0..3, CLK, PLL and DIGTOP shutdown-bars all released). */
        kprintf("[dby-dpu] lane dsi0_cfg0=%08x dsi0_cfg1=%08x dsi0_ctrl0=%08x dsi0_ctrl1=%08x dsi0_ls0=%08x dsi0_ls1=%08x\n",
            phy[(p0+0x34)/4],phy[(p0+0x38)/4],phy[(p0+0xa0)/4],phy[(p0+0xa4)/4],
            phy[(p0+0x148)/4],phy[(p0+0x14c)/4]);
        kprintf("[dby-dpu] lane dsi1_cfg0=%08x dsi1_cfg1=%08x dsi1_ctrl0=%08x dsi1_ctrl1=%08x dsi1_ls0=%08x dsi1_ls1=%08x\n",
            phy[(p0b+0x34)/4],phy[(p0b+0x38)/4],phy[(p0b+0xa0)/4],phy[(p0b+0xa4)/4],
            phy[(p0b+0x148)/4],phy[(p0b+0x14c)/4]);
        phyMap->release();
    }
    PE_Video video={};
    const int console=PE_current_console(&video);
    kprintf("[dby-dpu] console=%d base=%llx w=%u h=%u row=%u depth=%u\n",console,
        (unsigned long long)video.v_baseAddr,(unsigned)video.v_width,(unsigned)video.v_height,
        (unsigned)video.v_rowBytes,(unsigned)video.v_depth);
}
/* This is a transient-gate detector, not a freeze timeline. The witnesses it reads are values the
 * boot loader wrote and nothing in this tree changes (no code here writes dispcc or MDSS), so on a
 * normal boot they read the same at every sample. What it can catch is a moment when the MDSS domain
 * or one of its clocks is off, which the t=20 s snapshot of course misses. The causes of a panel
 * that keeps the boot frame are told apart by the [gpu clients] and copy rows (whether SpringBoard
 * renders and whether frames retire), not by this one. Read-only, and the MDP/PHY windows are only
 * touched when dispcc reads powered, exactly as in the snapshot above. A slot is marked as sampled
 * only after the checks pass, so a tick that cannot run is retried instead of being lost. */
extern "C" void dby_dpu_timeline(unsigned now) {
    static unsigned seen;
    static const unsigned at[4]={2,5,10,20};
    unsigned slot=0xffffffffU;
    for(unsigned i=0;i<4;++i) if(now>=at[i] && !(seen&(1u<<i))) { slot=i; break; }
    if(slot==0xffffffffU) return;
    unsigned enabled=0;
    if(!PE_parse_boot_argn("dby-dpu",&enabled,sizeof(enabled)) || enabled!=1) return;
    if(ml_at_interrupt_context() || !ml_get_interrupts_enabled()) return;
    IOMemoryMap *clockMap=nullptr;
    if(IODeviceMemory *clockRegion=IODeviceMemory::withRange(0xaf00000,0x20000)) {
        clockMap=clockRegion->map(kIOMapAnywhere|kIOMapInhibitCache);clockRegion->release();
    }
    if(!clockMap) return;                       /* slot stays unmarked; the next tick retries */
    seen|=1u<<slot;
    const volatile uint32_t *dispcc=reinterpret_cast<const volatile uint32_t *>(clockMap->getVirtualAddress());
    const uint32_t gdscr=dispcc[0x3000/4],ahb=dispcc[0x2080/4],mdpClock=dispcc[0x200c/4];
    clockMap->release();
    const bool clocksOn=(ahb&1U)&&!(ahb&2U)&&(mdpClock&1U)&&!(mdpClock&2U);
    const bool powered=(gdscr&0x80000000U)&&!(gdscr&1U);
    if(!clocksOn || !powered) {
        kprintf("[dby-dpu] t=%u at=%u gdscr=%08x ahb=%08x mdp=%08x gated\n",now,at[slot],gdscr,ahb,mdpClock);
        return;
    }
    uint32_t intfSt=0,ctlIntf=0,dsiCtrl=0,pllLock=0;
    IOMemoryMap *mdpMap=nullptr;
    if(IODeviceMemory *m=IODeviceMemory::withRange(0xae01000,0x83208)) {mdpMap=m->map(kIOMapAnywhere|kIOMapInhibitCache);m->release();}
    if(mdpMap) {
        const volatile uint32_t *mdp=reinterpret_cast<const volatile uint32_t *>(mdpMap->getVirtualAddress());
        intfSt=mdp[(0x6a800+0x26c)/4];ctlIntf=mdp[(0x1000+0xf4)/4];
        mdpMap->release();
    }
    IOMemoryMap *phyMap=nullptr;
    if(IODeviceMemory *m=IODeviceMemory::withRange(0xae94000,0x3000)) {phyMap=m->map(kIOMapAnywhere|kIOMapInhibitCache);m->release();}
    if(phyMap) {
        const volatile uint32_t *phy=reinterpret_cast<const volatile uint32_t *>(phyMap->getVirtualAddress());
        dsiCtrl=phy[0x004/4];pllLock=phy[(0x900+0x1b0)/4];
        phyMap->release();
    }
    kprintf("[dby-dpu] t=%u at=%u gdscr=%08x intf0_st=%08x ctl0_intf=%08x dsi0_ctrl=%08x dsi0_lock=%08x\n",
        now,at[slot],gdscr,intfSt,ctlIntf,dsiCtrl,pllLock);
}
/* dby-dpu=2 adds one experiment on top of the snapshot: restart the DSI video path. The panel in a
 * frozen boot shows the firmware frame while the memory already holds the cleared page, and every
 * DSI register reads enabled - so the working hypothesis is that the link lost its framing at the
 * handoff and never re-acquired, not that anything is switched off. This is a HYPOTHESIS, not an
 * upstream contract: mainline dsi_ctrl_disable() writes CTRL as a whole word 0, but only as one step
 * of a teardown that dsi_ctrl_enable() then rebuilds from VID_CFG0 upwards, and
 * msm_dsi_host_disable() states that on this IP clearing the enable bit does not by itself stop the
 * video engine. Bit names are dsi.xml's, 6G-shifted: CTRL bit0 ENABLE, bit1 VID_MODE, bit2 CMD_MODE,
 * bits4-7 LANE0..LANE3, bit8 CLK_EN - so the 0x77 both hosts read is ENABLE|VID_MODE|CMD_MODE|LANE0|
 * LANE1|LANE2, with bit7 and bit8 clear. Writing the whole word therefore restarts ENABLE together
 * with the three lane enables, not ENABLE alone, and that is the meaning a real toggle has here: a
 * still frozen panel says "EN plus LANE0-2 is not enough", never "the host cannot restart"; the next
 * step would be CTRL=0 plus full reprogramming, a different experiment. STATUS0 bit3 is
 * VIDEO_MODE_ENGINE_BUSY and was set in both hosts in the previous round, so the hardware reported a
 * running video engine; if it is set before this kick and clear after it, the kick stopped the engine
 * and did not bring it back - go back to the previous payload instead of pressing on. FIFO_STATUS
 * (0x00c) bit0 VIDEO_MDP_FIFO_OVERFLOW, bit3 VIDEO_MDP_FIFO_UNDERFLOW and bit7 CMD_MDP_FIFO_
 * UNDERFLOW, plus VID_CFG0 (0x010) DST_FORMAT[6:4] and TRAFFIC_MODE[9:8], are read before the kick
 * and FIFO_STATUS again after it: together they name the third cause this cannot treat, a host that
 * stays enabled while it starves or emits the wrong format.
 * Both hosts are switched off together and restored together, because this board splits the panel
 * across DSI0 and DSI1 on one shared PLL and a one-at-a-time window would leave a half-panel running
 * for milliseconds. The row is decidable rather than merely reassuring: the value is read back after
 * the clear, so mid with bit0 clear and after==before is a real toggle, mid==before means the store
 * was ignored (secure world or hardware - stop here, the panel says nothing about DSI), and anything
 * else is a mismatch whose after==0 shape sends the tablet back to the previous payload. A hang kills
 * the boot at t~20 s with the log slot keeping everything printed before it. This value prints no
 * [dby-dpu] t= rows: dby_dpu_timeline() above still requires dby-dpu==1, so do not read their
 * absence as a fault. */
static void dsiVideoKick() {
    IOMemoryMap *map=nullptr;
    if(IODeviceMemory *m=IODeviceMemory::withRange(0xae94000,0x3000)) {map=m->map(kIOMapAnywhere|kIOMapInhibitCache);m->release();}
    if(!map) {kprintf("[dby-dpu] dsi kick: map failed\n");return;}
    const volatile uint32_t *ro=reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    volatile uint32_t *dsi=const_cast<volatile uint32_t *>(ro);
    static const uint32_t off[2]={0,0x2000U};           /* DSI0 and DSI1 are one 0x3000 window */
    uint32_t before[2],mid[2],after[2],stIn[2],stOut[2],fifoIn[2],fifoOut[2],vidCfg[2];
    for(unsigned k=0;k<2;++k) {before[k]=dsi[(off[k]+0x004)/4];stIn[k]=dsi[(off[k]+0x008)/4];
        fifoIn[k]=dsi[(off[k]+0x00c)/4];vidCfg[k]=dsi[(off[k]+0x010)/4];}
    if(!(before[0]&1U) || !(before[1]&1U)) {            /* already off: nothing to restart, no write */
        kprintf("[dby-dpu] dsi kick skipped dis-armed ctrl=%08x/%08x st=%08x/%08x\n",
            before[0],before[1],stIn[0],stIn[1]);
        map->release();return;
    }
    for(unsigned k=0;k<2;++k) dsi[(off[k]+0x004)/4]=0;  /* drops ENABLE with VID_MODE, CMD_MODE, LANE0-2 */
    __asm__ volatile("dsb sy" ::: "memory");
    IODelay(2000);
    for(unsigned k=0;k<2;++k) mid[k]=dsi[(off[k]+0x004)/4];
    for(unsigned k=0;k<2;++k) dsi[(off[k]+0x004)/4]=before[k]; /* the exact word the loader left */
    __asm__ volatile("dsb sy" ::: "memory");
    IODelay(2000);
    for(unsigned k=0;k<2;++k) {after[k]=dsi[(off[k]+0x004)/4];stOut[k]=dsi[(off[k]+0x008)/4];
        fifoOut[k]=dsi[(off[k]+0x00c)/4];}
    for(unsigned k=0;k<2;++k) {
        const bool lost=(stIn[k]&8U)&&!(stOut[k]&8U);   /* engine was busy, now it is not: it did not come back */
        kprintf("[dby-dpu] dsi%u kick ctrl before=%08x mid=%08x after=%08x st=%08x>%08x fifo=%08x>%08x vidcfg=%08x %s\n",
            k,before[k],mid[k],after[k],stIn[k],stOut[k],fifoIn[k],fifoOut[k],vidCfg[k],
            lost?"engine-lost":(mid[k]&1U)?((mid[k]==before[k])?"store-ignored":"MISMATCH")
                :(after[k]==before[k])?"toggled":"MISMATCH");
    }
    map->release();
}
extern "C" void dby_dpu_probe(void) {
    static bool done;
    unsigned enabled=0;
    if(done || !PE_parse_boot_argn("dby-dpu",&enabled,sizeof(enabled)) || enabled<1 || enabled>2)return;
    if(ml_at_interrupt_context() || !ml_get_interrupts_enabled())return;
    done=true;
    dpuRegisterSnapshot();
    if(enabled>=2) dsiVideoKick();
}
__attribute__((noinline)) void native_legacy_display_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_gop_mapping_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_client_checkpoint(void) {__asm__ volatile("" ::: "memory");}
void native_legacy_pm_observed(unsigned stage,IOReturn status,unsigned long ordinal) {
    native_legacy_pm_stage=stage;native_legacy_pm_status=status;native_legacy_pm_ordinal=(uint32_t)ordinal;
    native_legacy_pm_calls=native_legacy_pm_calls+1;
    if(stage==3)native_legacy_pm_callbacks=native_legacy_pm_callbacks+1;
}
__attribute__((noinline)) void native_legacy_swap_validation_checkpoint(bool,unsigned,const void *,const void *) {__asm__ volatile("" ::: "memory");}
// Last rejected swap request, copied from the original request layout the validator reads (offsets
// qualified in NativeLegacyDisplay::validateSwap). Observation only; nothing here changes the verdict.
volatile uint32_t native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason;
/* S0a: the clock reading (microseconds since boot) of the most recently accepted swap - the kernel's first touch of a
 * frame. [frame budget] measures accepted->visible with it, which contains the whole submission path; a stamp taken when
 * the driver picked the swap would happen after the submission returned and would understate the frame by that cost.
 * Newest-accepted is exact while one swap is in flight; the row prints `nostart` when nothing was stamped. */
volatile uint64_t native_legacy_swap_accepted_us;
// Every validateSwap verdict, by reason. 0 accepted; 1 preconditions, 2 the surfaces/layer state,
// 3 geometry, 4 the surface itself, 5 accepted-blank. A run whose histogram is all zeros never
// reached the validator at all, which is a different problem from one that refused frames.
volatile uint32_t native_legacy_swap_reason_counts[8];
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3],native_legacy_rej_options[3],native_legacy_rej_size[2],native_legacy_rej_rects[2][4];
char native_legacy_rej_class[32];
volatile uint64_t native_legacy_rej_surface[7];   // width, height, format, planes, stride, allocation, descriptor bytes of the last rejected surface
void native_legacy_surface_rejected(uint64_t width,uint64_t height,uint32_t format,uint32_t planes,uint64_t stride,uint64_t allocation,uint64_t bytes) {
    native_legacy_rej_surface[0]=width;native_legacy_rej_surface[1]=height;native_legacy_rej_surface[2]=format;native_legacy_rej_surface[3]=planes;
    native_legacy_rej_surface[4]=stride;native_legacy_rej_surface[5]=allocation;native_legacy_rej_surface[6]=bytes;
}
/* The swap request's fields, by offset in the request object the display driver hands its client. These are
 * Apple's ivars, not this driver's: the layout is the one the 21H461 IOMobileFramebuffer request object has,
 * read here on purpose - this probe exists to report what a refused swap carried - and nothing can assert it
 * at compile time because the class is opaque. 2624 is the four surface ids, 792/796/800 the three state words,
 * 804/808/812 their options, 816/820 width and height, 17 and 81 the first two bytes of the two rectangles.
 * A kernel whose object moved reports whatever is at those offsets; the display area's limits record that these
 * are hand-derived offsets rather than a checked ABI. */
static void recordRejectedSwap(unsigned reason,const void *request,const void *surface) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(request);
    if(!bytes)return;
    native_legacy_rej_reason=reason;
    uint64_t surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));
    for(unsigned i=0;i<4;++i)native_legacy_rej_surfaces[i]=surfaces[i];
    const unsigned stateOffsets[3]={792,796,800},optionOffsets[3]={804,808,812};
    for(unsigned i=0;i<3;++i){uint32_t a=0,b=0;memcpy(&a,bytes+stateOffsets[i],4);memcpy(&b,bytes+optionOffsets[i],4);
        native_legacy_rej_state[i]=a;native_legacy_rej_options[i]=b;}
    uint32_t w=0,h=0;memcpy(&w,bytes+816,4);memcpy(&h,bytes+820,4);native_legacy_rej_size[0]=w;native_legacy_rej_size[1]=h;
    const unsigned rectOffsets[2]={17,81};
    for(unsigned r=0;r<2;++r){uint32_t rect[4]={};memcpy(rect,bytes+rectOffsets[r],sizeof(rect));
        for(unsigned i=0;i<4;++i)native_legacy_rej_rects[r][i]=rect[i];}
    char name[32]={};
    if(surface){const OSObject *object=reinterpret_cast<const OSObject *>(surface);
        const OSMetaClass *meta=object->getMetaClass();const char *cls=meta?meta->getClassName():nullptr;
        if(cls)strlcpy(name,cls,sizeof(name));}
    memcpy(native_legacy_rej_class,name,sizeof(name));
}
// Content statistics of presented frames (copy = kind 0, blank fill = kind 1): the first 32 are kept, with the time
// in seconds since the first frame. A per-frame record shows the animation cadence and when the OS blanks the panel.
#define NATIVE_LEGACY_FRAMES 32
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[NATIVE_LEGACY_FRAMES];
volatile uint32_t native_legacy_frame_time[NATIVE_LEGACY_FRAMES];
volatile uint64_t native_legacy_frame_stats[NATIVE_LEGACY_FRAMES][5];
static uint64_t frameEpoch;
void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy) {
    uint32_t n=native_legacy_frame_count;
    uint64_t now,frequency;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(now));__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    if(!frameEpoch)frameEpoch=now;
    if(n<NATIVE_LEGACY_FRAMES) {
        native_legacy_frame_kind[n]=kind;
        native_legacy_frame_time[n]=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
        native_legacy_frame_stats[n][0]=count;native_legacy_frame_stats[n][1]=minx;native_legacy_frame_stats[n][2]=miny;
        native_legacy_frame_stats[n][3]=maxx;native_legacy_frame_stats[n][4]=maxy;
    }
    native_legacy_frame_count=n+1;
}
volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4],native_legacy_frame_color_other[8];
// Called right after the matching native_legacy_frame_observed(); fills slot count-1 (or the blank slot).
void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other) {
    uint32_t n=native_legacy_frame_count;
    if(!n || n>8)return;   // dominant colours are kept for the first eight frames only
    n-=1;
    for(unsigned i=0;i<4;++i){native_legacy_frame_colors[n][i]=colors[i];native_legacy_frame_color_counts[n][i]=counts[i];}
    native_legacy_frame_color_other[n]=other;
}
// The life of every accepted swap, from the kernel's side: validated (the client submitted it), offered to the GPU copy (or started on the CPU),
// and retired. Times are microseconds since boot; the last 64 are kept.
struct SwapEvent {uint64_t submitted,offered,done;uint32_t number;};
static SwapEvent swapEvents[64];static uint32_t swapEventCount;
extern "C" void native_legacy_swap_stage(unsigned stage) {
    const uint64_t now=probeNowUs();
    uint32_t total=swapEventCount;
    if(stage==0) {
        SwapEvent &e=swapEvents[total%64];e={now,0,0,total+1};swapEventCount=total+1;return;
    }
    uint32_t first=total>64?total-64:0;
    for(uint32_t i=first;i<total;++i) {
        SwapEvent &e=swapEvents[i%64];
        if(stage==1 && !e.offered && !e.done){e.offered=now;return;}
        if(stage==2 && !e.done){e.done=now;if(!e.offered)e.offered=now;return;}
    }
}
extern "C" unsigned native_legacy_swap_timeline(char *out,unsigned cap) {
    if(!out || !cap)return 0;   // the same guard native_legacy_properties_now opens with
    unsigned at=0;out[0]=0;
    uint32_t total=swapEventCount,first=total>40?total-40:0;
    for(uint32_t i=first;i<total && at+90<cap;++i) {
        const SwapEvent &e=swapEvents[i%64];
        uint64_t previous=i>first?swapEvents[(i-1)%64].submitted:e.submitted;
        at+=snprintf(out+at,cap-at,"W%u t=%llu.%03llus +%llums offer=%lldms done=%lldms\n",e.number,(unsigned long long)(e.submitted/1000000),
            (unsigned long long)((e.submitted/1000)%1000),(unsigned long long)((e.submitted-previous)/1000),
            e.offered?(long long)((e.offered-e.submitted)/1000):-1LL,e.done?(long long)((e.done-e.submitted)/1000):-1LL);
    }
    return at;
}
// The display refresh tick (NativeLegacyDisplay, sheng-vsync): vsync messages sent, arms, idle stops, late ticks, starts, refresh
// properties reported at that rate, and the rate.
static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHz;
void native_legacy_vsync_observed(unsigned event,unsigned hz) {
    if(event<6)vsyncEvents[event]=vsyncEvents[event]+1;
    vsyncHz=hz;
}
extern "C" unsigned native_legacy_prepare_report(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    return snprintf(out,cap,"prepare calls=%u failures=%u retried=%u max_attempts=%u waited_ms=%llu vsync hz=%u sent=%llu arms=%llu idle=%llu late=%llu start=%llu refresh=%llu"
        " | swap accepted=%u rejected=%u lastReason=%u r0=%u r1=%u r2=%u r3=%u r4=%u r5=%u"
        " | rej wh=%llux%llu fmt=%08x pl=%llu stride=%llu alloc=%llu bytes=%llu",
        native_legacy_prepare_calls,native_legacy_prepare_failures,native_legacy_prepare_retried,native_legacy_prepare_max_attempts,
        (unsigned long long)(native_legacy_prepare_wait_us/1000),vsyncHz,(unsigned long long)vsyncEvents[1],(unsigned long long)vsyncEvents[0],
        (unsigned long long)vsyncEvents[2],(unsigned long long)vsyncEvents[3],(unsigned long long)vsyncEvents[4],(unsigned long long)vsyncEvents[5],
        native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason,
        native_legacy_swap_reason_counts[0],native_legacy_swap_reason_counts[1],native_legacy_swap_reason_counts[2],
        native_legacy_swap_reason_counts[3],native_legacy_swap_reason_counts[4],native_legacy_swap_reason_counts[5],
        (unsigned long long)native_legacy_rej_surface[0],(unsigned long long)native_legacy_rej_surface[1],
        (unsigned)native_legacy_rej_surface[2],(unsigned long long)native_legacy_rej_surface[3],
        (unsigned long long)native_legacy_rej_surface[4],(unsigned long long)native_legacy_rej_surface[5],
        (unsigned long long)native_legacy_rej_surface[6]);
}
void native_legacy_swap_validation_observed(bool accepted,unsigned reason,const void *request,const void *surface) {
    if(reason<8)native_legacy_swap_reason_counts[reason]=native_legacy_swap_reason_counts[reason]+1;
    if(accepted)native_legacy_swap_stage(0);
    if(accepted)native_legacy_swap_accepted_count=native_legacy_swap_accepted_count+1;
 if(accepted) {
  /* S0a: the frame's first touch in the kernel, in microseconds. cntpct/cntfrq are the counters the console timebase
   * already uses; a zero frequency (a platform without the counter) leaves the stamp at its previous value, and the
   * display driver then charges that frame to `nostamp` instead of inventing a budget. */
  uint64_t ticks=0,freq=0;
  __asm__ volatile("mrs %0, cntpct_el0":"=r"(ticks));
  __asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
  if(freq>1000000ull)native_legacy_swap_accepted_us=ticks/(freq/1000000ull);
 }
    else {native_legacy_swap_rejected_count=native_legacy_swap_rejected_count+1;recordRejectedSwap(reason,request,surface);}
    native_legacy_swap_validation_calls=native_legacy_swap_validation_calls+1;
    native_legacy_swap_validation_result=accepted;native_legacy_swap_validation_reason=reason;
    native_legacy_swap_request=(uintptr_t)request;native_legacy_swap_surface=(uintptr_t)surface;
    native_legacy_swap_validation_checkpoint(accepted,reason,request,surface);
}
// Time of the pixel copy into the scanout buffer (counter ticks): swaps completed, last/maximum/sum, for the banner.
volatile uint64_t native_legacy_copy_n,native_legacy_copy_last,native_legacy_copy_max,native_legacy_copy_sum,native_legacy_copy_bytes_last,native_legacy_copy_bytes_sum;
void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes) {
    native_legacy_copy_bytes_last=bytes;native_legacy_copy_bytes_sum=native_legacy_copy_bytes_sum+bytes;
    native_legacy_copy_n=native_legacy_copy_n+1;native_legacy_copy_last=ticks;native_legacy_copy_sum=native_legacy_copy_sum+ticks;
    if(ticks>native_legacy_copy_max)native_legacy_copy_max=ticks;
}
__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status,const void *request,uint64_t bytes) {
    native_legacy_transfer_calls=native_legacy_transfer_calls+1;
    if(status==kIOReturnSuccess)native_legacy_swap_stage(2);
    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;
    if(status!=kIOReturnSuccess && boot_progress_stage) {
        char message[120];snprintf(message,sizeof(message),"DISPLAY COPY FAILED status=%08x prepare=%u tries=%u",
          (unsigned)status,native_legacy_prepare_reason,native_legacy_prepare_attempts);
        boot_progress_stage(message);
    }
}
bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter,display,memory,stride,width,height);
}
void native_legacy_gpu_fallback_observed(int status) {
    native_legacy_gpu_last_status=(uint32_t)status;native_legacy_gpu_fallbacks=native_legacy_gpu_fallbacks+1;
}
/* The display object's retirement counters, by offset: 5472 current, 5480 pending, 5820 completed generation,
 * 5824 presented generation, 252 the frame count. Same rule as recordRejectedSwap above - Apple's ivars read
 * raw because reporting them is the probe's job, with the layout hand-derived from the 21H461 object and no
 * compile-time check possible. */
__attribute__((noinline)) void native_legacy_retirement_observed(IOService *display) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(display);
    uint64_t current=0,pending=0;uint32_t completed=0,presented=0,frames=0;
    memcpy(&current,bytes+5472,8);memcpy(&pending,bytes+5480,8);
    memcpy(&completed,bytes+5820,4);memcpy(&presented,bytes+5824,4);memcpy(&frames,bytes+252,4);
    native_legacy_retired_current=current;native_legacy_retired_pending=pending;
    native_legacy_retired_generation=completed;native_legacy_presented_generation=presented;native_legacy_retired_frames=frames;
    native_legacy_retirement_calls=native_legacy_retirement_calls+1;
}
// The display object's properties as they are now (the registration copy above predates setDisplayRefreshProperties and the rest): for the
// UFS log. Returns the bytes written (NUL-terminated).
extern "C" unsigned native_legacy_properties_now(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    out[0]=0;
    IOService *display=reinterpret_cast<IOService *>(native_legacy_object);
    if(!display)return 0;
    OSSerialize *xml=OSSerialize::withCapacity(8192);
    unsigned size=0;
    if(xml && display->serializeProperties(xml)) {
        size=xml->getLength();if(size>=cap)size=cap-1;
        memcpy(out,xml->text(),size);out[size]=0;
    }
    OSSafeReleaseNULL(xml);
    return size;
}
void native_legacy_registration_observed(IOService *display) {
    native_legacy_registration_calls=native_legacy_registration_calls+1;
    OSSerialize *xml=OSSerialize::withCapacity(2048);
    if(xml && display->serializeProperties(xml)) {
        unsigned size=xml->getLength();if(size>=sizeof(native_legacy_properties))size=sizeof(native_legacy_properties)-1;
        memcpy(native_legacy_properties,xml->text(),size);native_legacy_properties[size]=0;native_legacy_properties_length=size;
    }
    OSSafeReleaseNULL(xml);
}
void native_legacy_client_enter(task_t task,uint32_t type) {
    native_legacy_client_calls=native_legacy_client_calls+1;native_legacy_client_task=(uintptr_t)task;
    native_legacy_client_type=type;native_legacy_client_pid=task?proc_pid((proc_t)get_bsdtask_info(task)):0;
}
// Every framebuffer user client seen (object, pid), with what it does: the original client's vtable is copied and two entries (externalMethod, slot 168,
// and registerNotificationPort(port,type,refCon), slot 175) are pointed at wrappers that count and forward to the original function. The original code
// is not touched; only this object's vptr names the copy. XNU's ARM64 vptr points at slot 0 (the original code dispatches with ldr x8,[vptr,#slot*8]).
enum {kSeenClients=8,kSelectors=96,kRegEvents=8,kVtableSlots=256};
struct ClientStat {
    uintptr_t object;int pid;uint32_t calls,fails;uint32_t selector_calls[kSelectors],selector_fails[kSelectors],selector_status[kSelectors];
    unsigned regs;struct {uint32_t type;uint64_t port;uint32_t ref,status;} reg[kRegEvents];
    uint64_t sel6_first[3],sel6_last[3];uint32_t sel6_distinct;uint64_t sel6_seen[8][3];uint32_t sel6_calls[8],sel6_fails[8],sel6_status[8];
    uint32_t seq_count;struct {uint32_t a,b,c,status,done,presented;} seq[24];
    uintptr_t original_external,original_register;
};
static ClientStat seenClients[kSeenClients];static unsigned seenClientCount;
typedef IOReturn (*ExternalFn)(IOUserClient *,uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *);
typedef IOReturn (*RegisterFn)(IOUserClient *,mach_port_t,UInt32,UInt32);
static ClientStat *statFor(IOUserClient *client) {
    for(unsigned i=0;i<seenClientCount;++i)if(seenClients[i].object==(uintptr_t)client)return &seenClients[i];
    return nullptr;
}
static IOReturn wrappedExternal(IOUserClient *client,uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *dispatch,OSObject *target,void *ref) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<ExternalFn>(stat->original_external)(client,selector,args,dispatch,target,ref):kIOReturnNotAttached;
    if(stat) {
        if(selector==6 && args && args->scalarInput && args->scalarInputCount>=3) {
            const uint64_t *in=args->scalarInput;
            if(!stat->sel6_distinct && !stat->sel6_first[0] && !stat->sel6_first[1])memcpy(stat->sel6_first,in,sizeof stat->sel6_first);
            memcpy(stat->sel6_last,in,sizeof stat->sel6_last);
            int at=-1;
            for(unsigned k=0;k<stat->sel6_distinct && k<8;++k)if(!memcmp(stat->sel6_seen[k],in,24))at=(int)k;
            if(at<0 && stat->sel6_distinct<8){at=(int)stat->sel6_distinct++;memcpy(stat->sel6_seen[at],in,24);}
            if(stat->seq_count<24){
                auto &e=stat->seq[stat->seq_count++];e.a=(uint32_t)in[0];e.b=(uint32_t)in[1];e.c=(uint32_t)in[2];e.status=(uint32_t)status;
                const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
                e.done=e.presented=0;if(object){memcpy(&e.done,object+5820,4);memcpy(&e.presented,object+5824,4);}
            }
            if(at>=0){++stat->sel6_calls[at];if(status)++stat->sel6_fails[at];stat->sel6_status[at]=(uint32_t)status;}
        }
        ++stat->calls;if(status)++stat->fails;
        if(selector<kSelectors){++stat->selector_calls[selector];if(status)++stat->selector_fails[selector];stat->selector_status[selector]=(uint32_t)status;}
    }
    return status;
}
static IOReturn wrappedRegister(IOUserClient *client,mach_port_t port,UInt32 type,UInt32 refCon) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<RegisterFn>(stat->original_register)(client,port,type,refCon):kIOReturnNotAttached;
    if(stat && stat->regs<kRegEvents){auto &e=stat->reg[stat->regs++];e.type=type;e.port=(uint64_t)(uintptr_t)port;e.ref=refCon;e.status=(uint32_t)status;}
    return status;
}
static void interpose(IOUserClient *client,int pid) {
    if(seenClientCount>=kSeenClients)return;
    ClientStat &stat=seenClients[seenClientCount];
    memset(&stat,0,sizeof stat);
    uintptr_t *vtable=*reinterpret_cast<uintptr_t **>(client);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kVtableSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,vtable,kVtableSlots*sizeof(uintptr_t));
    stat.object=(uintptr_t)client;stat.pid=pid;stat.original_external=copy[168];stat.original_register=copy[175];
    copy[168]=(uintptr_t)&wrappedExternal;copy[175]=(uintptr_t)&wrappedRegister;
    ++seenClientCount;
    *reinterpret_cast<uintptr_t **>(client)=copy;
}
// The display object's state words the original wait selector tests (offsets 5832..5864, as dwords), for the log.
extern "C" unsigned native_legacy_state_words(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    out[0]=0;
    const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
    if(!object)return 0;
    unsigned at=snprintf(out,cap,"d+5808:");
    for(unsigned o=5808;o<5872 && at+16<cap;o+=4){uint32_t v;memcpy(&v,object+o,4);at+=snprintf(out+at,cap-at," %x",v);}
    return at;
}
extern "C" unsigned native_legacy_clients_dump(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    unsigned at=0;out[0]=0;
    for(unsigned c=0;c<seenClientCount && at+120<cap;++c) {
        const ClientStat &s=seenClients[c];
        at+=snprintf(out+at,cap-at,"C%u pid=%d calls=%u fails=%u regs=%u",c,s.pid,s.calls,s.fails,s.regs);
        for(unsigned r=0;r<s.regs && r<kRegEvents && at+60<cap;++r)
            at+=snprintf(out+at,cap-at," reg(t%u,p%llx,r%x)=%x",s.reg[r].type,(unsigned long long)s.reg[r].port,s.reg[r].ref,s.reg[r].status);
        at+=snprintf(out+at,cap-at,"\n");
        for(unsigned k=0;k<s.sel6_distinct && k<8 && at+100<cap;++k)
            at+=snprintf(out+at,cap-at,"  sel6 args %llx %llx %llx: %u calls %u fails last %x\n",(unsigned long long)s.sel6_seen[k][0],(unsigned long long)s.sel6_seen[k][1],(unsigned long long)s.sel6_seen[k][2],s.sel6_calls[k],s.sel6_fails[k],s.sel6_status[k]);
        for(unsigned k=0;k<s.seq_count && at+90<cap;++k)
            at+=snprintf(out+at,cap-at,"  s6#%u %x %x %x -> %x [5820]=%x [5824]=%x\n",k,s.seq[k].a,s.seq[k].b,s.seq[k].c,s.seq[k].status,s.seq[k].done,s.seq[k].presented);
        unsigned n=0;
        for(unsigned sel=0;sel<kSelectors && at+40<cap;++sel) {
            if(!s.selector_calls[sel])continue;
            if(!n)at+=snprintf(out+at,cap-at,"  sel");
            at+=snprintf(out+at,cap-at," %u:%u/%u/%x",sel,s.selector_calls[sel],s.selector_fails[sel],s.selector_status[sel]);
            if(++n%6==0)at+=snprintf(out+at,cap-at,"\n  sel");
        }
        if(n)at+=snprintf(out+at,cap-at,"\n");
    }
    return at;
}
void native_legacy_client_observed(IOReturn status,IOUserClient *client) {
    if(client && !status)interpose(client,native_legacy_client_pid);
    native_legacy_client_status=status;native_legacy_client_object=(uintptr_t)client;
    if(client)strlcpy(native_legacy_client_class,client->getMetaClass()->getClassName(),sizeof(native_legacy_client_class));
    native_legacy_client_checkpoint();
}
__attribute__((noinline)) bool native_legacy_gop_mapping_observed(IOService *object,IOMemoryMap *mapping,uint32_t width,uint32_t height) {
    if((uintptr_t)object!=native_legacy_object)panic("NativeLegacyDisplay: wrong mapping owner");
    native_legacy_gop_map=(uintptr_t)mapping;native_legacy_gop_virtual=mapping->getVirtualAddress();
    native_legacy_gop_physical=mapping->getPhysicalAddress();native_legacy_gop_bytes=mapping->getLength();
    native_legacy_gop_width=width;native_legacy_gop_height=height;
    if(boot_progress_pulse_anchor && !diagnosticAnchorMapping) {
        PE_Video anchorVideo={};
        if(!PE_current_console(&anchorVideo) && anchorVideo.v_depth==32 &&
           anchorVideo.v_rowBytes<=65536 &&
           anchorVideo.v_width==width && anchorVideo.v_height==height &&
           anchorVideo.v_baseAddr==mapping->getPhysicalAddress() && !anchorVideo.v_rotate) {
            mapping->retain();
            if(boot_progress_pulse_anchor(mapping->getVirtualAddress(),mapping->getLength(),width,height,
                (unsigned)anchorVideo.v_rowBytes))diagnosticAnchorMapping=mapping;
            else mapping->release();
        }
    }
    native_legacy_gop_mapping_checkpoint();
    unsigned enabled=0;PE_parse_boot_argn("offline-legacy-unit-scale",&enabled,sizeof(enabled));
    if(!enabled)return true;
    native_legacy_unit_scale_calls=native_legacy_unit_scale_calls+1;native_legacy_unit_scale_status=1;
    unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    PE_Video video={};
    // The value is the UI scale: logical points = actual scanout pixels / scale (1..3). It changes
    // only the published display-scale; the scanout copy still moves one actual pixel per pixel.
    if(enabled<1 || enabled>3 || phase!=4 || unitScaleChosen || PE_current_console(&video) ||
       video.v_width!=width || video.v_height!=height || video.v_rotate ||
       mapping->getPhysicalAddress()!=video.v_baseAddr)return false;
    // Explicit offline UI policy: `enabled` actual GOP scanout pixels per logical
    // point in each direction (1 = native pixels). This is not physical pixel density,
    // panel timing, an Apple device identity or a synthesized mode list.
    unitScaleChosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if(!unitScaleChosen || !publishOrMatchWord(unitScaleChosen,"display-scale",enabled,unitScaleAdded) ||
       !publishOrMatchWord(unitScaleChosen,"display-rotation",video.v_rotate,unitRotationAdded)) {
        finishUnitScalePolicy(false);return false;
    }
    OSData *scale=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-scale"));
    OSData *rotation=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-rotation"));
    uint32_t actualScale=0,actualRotation=0;
    if(!scale || !rotation || scale->getLength()!=4 || rotation->getLength()!=4) {
        finishUnitScalePolicy(false);return false;
    }
    memcpy(&actualScale,scale->getBytesNoCopy(),4);memcpy(&actualRotation,rotation->getBytesNoCopy(),4);
    native_legacy_chosen_scale=actualScale;native_legacy_chosen_rotation=actualRotation;
    native_legacy_unit_scale_status=actualScale==enabled && actualRotation==video.v_rotate?0:2;
    if(native_legacy_unit_scale_status){finishUnitScalePolicy(false);return false;}
    return true;
}
__attribute__((noreturn,noinline)) void native_legacy_backend_guard(unsigned slot,void *object,void *caller) {
    native_legacy_guard_slot=slot;native_legacy_guard_object=(uintptr_t)object;native_legacy_guard_caller=(uintptr_t)caller;
    panic("NativeLegacyDisplay: leaf=%u caller=%llx stage=%u PM=%u swap=%u/%u copy=%08x bytes=%llu retire=%u handoff=%u/%u",
      slot,(uint64_t)(uintptr_t)caller,native_legacy_stage,native_legacy_pm_stage,
      native_legacy_swap_validation_result,native_legacy_swap_validation_reason,
      (unsigned)native_legacy_transfer_status,native_legacy_transfer_bytes,native_legacy_retirement_calls,
      native_legacy_prepare_reason,native_legacy_prepare_attempts);
}
void native_legacy_display_probe(void);
void native_legacy_display_probe(void) {
    // The registration path and optional original-userclient inspection both
    // reach this boot-thread entry. Run class constructors only once; a second
    // postModLoad would attempt to register the same metaclasses again.
    if(native_legacy_stage)return;
    native_legacy_stage=1;void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){native_legacy_display_checkpoint();return;}
    native_legacy_display_static_init();native_gop_presenter_static_init();native_legacy_meta_status=OSMetaClass::postModLoad(handle);
    if(native_legacy_meta_status){native_legacy_display_checkpoint();return;}
    PE_Video video={};native_legacy_stage=2;
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth!=32
        || video.v_rotate || video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4
        || video.v_rowBytes>UINT64_MAX/video.v_height){native_legacy_display_checkpoint();return;}
    IOService *display=native_legacy_display_allocate();native_legacy_object=(uintptr_t)display;
    if(!display){native_legacy_display_checkpoint();return;}
    native_legacy_object_size=display->getMetaClass()->getClassSize();
    native_legacy_parent_match=(display->metaCast("IOMobileFramebufferLegacy") && display->metaCast("IOMobileFramebuffer") && display->metaCast("IOService"));
    native_legacy_stage=3;native_legacy_init_result=display->init();
    unsigned phase=1;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    if(native_legacy_init_result && (phase==2 || phase==3 || phase==4)) {
        // IOSurface matches the real IOBSD resource, published by bsd_autoconf.
        // Retain this actual object until that normal lifecycle boundary.
        pendingLegacyDisplay=display;return;
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    native_legacy_display_checkpoint();
}
void native_legacy_display_late_probe(void);
extern void native_hid_input_probe(void);
extern void native_keystore_probe(void);
extern void native_touch_probe(void);
extern void sheng_cpufreq_probe(void);
/* sheng_gpu_power_probe is deliberately NOT called here: bsd_autoconf runs this before the log writer
 * has stored a slot, and the GPU power probe must not touch MMIO before that evidence exists. It is
 * called from BootProgress.c on the late, log-backed gate instead. */
extern void sheng_wifi_probe(void);
extern void sheng_adsp_probe(void);
extern void sheng_battery_probe(void);
extern void sheng_audio_probe(void);
extern void sheng_buttons_probe(void);
extern void sheng_backlight_probe(void);
extern "C" void sheng_dpu_probe(void);
/* ShengUfs's display window entry point (DpuWindow.h, exported from NativeUfs.cpp). Weak: a build
 * without that component still links, and the call site tests the address before using it. */
/* apps_smmu's base: the address NativeUfs.cpp:275 maps and the one the window's stage-1 tables are
 * built against. Named rather than passed inline so the value has a single home. */
static constexpr uintptr_t kAppsSmmuBase=0x15000000U;
extern "C" int native_ufs_dpu_window_build(uintptr_t smmu) __attribute__((weak_import));
void native_legacy_display_late_probe(void) {
    native_keystore_probe();
    sheng_cpufreq_probe();
    native_touch_probe();
    sheng_wifi_probe();
    sheng_adsp_probe();
    sheng_battery_probe();
    sheng_audio_probe();
    sheng_buttons_probe();
    sheng_backlight_probe();
    sheng_dpu_probe();
    native_hid_input_probe();
    IOService *display=pendingLegacyDisplay;if(!display)return;pendingLegacyDisplay=nullptr;
    native_legacy_surface_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching=IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface=matching?IOService::waitForMatchingService(matching,60000000000ULL)/* TCG: IOSurfaceRoot publishes just past 5s; the original Apple wait is UINT64_MAX */:nullptr;
    native_legacy_surface_found=surface!=nullptr;OSSafeReleaseNULL(surface);OSSafeReleaseNULL(matching);
    PE_Video video={};
    if(!native_legacy_surface_status && native_legacy_surface_found && !PE_current_console(&video)
        && video.v_baseAddr && video.v_width && video.v_height && video.v_depth==32
        && !video.v_rotate && video.v_width<=16384 && video.v_height<=16384
        && video.v_rowBytes>=video.v_width*4 && video.v_rowBytes<=UINT64_MAX/video.v_height) {
        native_legacy_video_base=video.v_baseAddr;native_legacy_video_bytes=video.v_rowBytes*video.v_height;
        // A real firmware framebuffer resource, with addresses and extent
        // obtained from the actual GOP handoff. It has no invented compatible,
        // display list, panel timing, GPU/fence completion or vendor registers.
        native_legacy_stage=4;IOPlatformDevice *nub=new IOPlatformDevice;
        native_legacy_nub=(uintptr_t)nub;bool nubAttached=false,nubStarted=false,attached=false,started=false;
        IORegistryEntry *displayTreeParent=nullptr;bool dtAttached=false;
        IOService *parent=IOService::getResourceService();
        IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,video.v_rowBytes*video.v_height);
        OSArray *ranges=OSArray::withCapacity(1);
        if(nub && memory && ranges && parent && nub->init()) {
            ranges->setObject(memory);nub->setDeviceMemory(ranges);nub->setName("NativeGOPFramebuffer");
            nubAttached=nub->attach(parent);if(nubAttached)nubStarted=nub->start(parent);
            if(nubStarted) {
                unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
                if(phase==4) {
                    // This is the actually implemented GOP display subsystem,
                    // attached to its real IOPlatformDevice memory resource.
                    // No Apple/vendor compatible or panel timing is supplied.
                    displayTreeParent=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
                    const char type[]="display-subsystem";OSData *data=OSData::withBytes(type,sizeof(type));
                    if(displayTreeParent && data && nub->setProperty("device_type",data)) {
                        nub->setName("gop-display",gIODTPlane);dtAttached=nub->attachToParent(displayTreeParent,gIODTPlane);
                    }
                    OSSafeReleaseNULL(data);native_legacy_dt_attached=dtAttached;
                }
                if(phase==3) {
                    IOService *presenter=native_gop_presenter_create();bool presenterAttached=false;
                    if(presenter && presenter->init() && (presenterAttached=presenter->attach(nub)) && presenter->start(nub)) {
                        // Explicit own transport service only. The incomplete
                        // original framebuffer backend is never published.
                        activeGOPPresenter=presenter;activeGOPNub=nub;nub=nullptr;nubAttached=false;nubStarted=false;
                    } else {if(presenterAttached)presenter->detach(nub);OSSafeReleaseNULL(presenter);}
                } else {
                    /* Build the display's stage-1 window before the display starts - the order
                     * NativeUfs.cpp:1188 documents. startDirect() in
                     * drivers/GopFramebuffer/NativeLegacyDisplay.cpp refuses unless
                     * native_ufs_dpu_state carries the installed shape 0x80000000|bank, and the DPU can
                     * only fetch through that window once the table exists, so this has to run before
                     * display->start() below, not after.
                     *
                     * 0x15000000 is apps_smmu's base, the address NativeUfs.cpp:275 maps. The window's
                     * boot IOVA is deliberately NOT passed: the header reads it from
                     * native-dpu-boot-iova and defaults to 0x9c000000, which is this board's GOP
                     * mapping per the [swapok] row.
                     *
                     * Weak, so a build without ShengUfs links and this is a no-op rather than a NULL
                     * call. */
                    SInt32 score=0;native_legacy_probe_match=(phase!=4 || dtAttached) && display->probe(nub,&score)==display;
                    if(&native_ufs_dpu_window_build) native_ufs_dpu_window_build(kAppsSmmuBase);
                    if(native_legacy_probe_match)attached=display->attach(nub);
                    if(attached){native_legacy_stage=5;started=display->start(nub);native_legacy_start_result=started;}
                    if(started && phase==4) {
                        activeLegacyDisplay=display;activeGOPNub=nub;native_legacy_stage=10;
                        {
                            unsigned gpu=0;PE_parse_boot_argn("sheng-fb-gpu",&gpu,sizeof gpu);
                            native_legacy_gop_gate[0]=gpu;
                            if(gpu==1) {
                                native_legacy_gop_gate[1]=1;
                                IOService *handoff=native_gop_presenter_create();
                                // Not a child of the display subsystem's node: the original display code enumerates that node's children (int-075/077:
                                // SpringBoard found no display and crashed with the presenter attached there, registered or not).
                                IOService *home=IOService::getResourceService();
                                bool handoffAttached=false;
                                if(!handoff)native_legacy_gop_gate[1]=2;
                                else if(!home)native_legacy_gop_gate[1]=3;
                                else {
                                    native_gop_presenter_set_source(handoff,nub);
                                    if(!handoff->init())native_legacy_gop_gate[1]=4;
                                    else if(!(handoffAttached=handoff->attach(home)))native_legacy_gop_gate[1]=5;
                                    else if(!handoff->start(home))native_legacy_gop_gate[1]=6;
                                    else {gpuHandoffPresenter=handoff;native_legacy_gop_gate[1]=7;}
                                }
                                if(!gpuHandoffPresenter) {if(handoffAttached)handoff->detach(home);OSSafeReleaseNULL(handoff);}
                            }
                        }
                        finishUnitScalePolicy(true);
                        OSSafeReleaseNULL(displayTreeParent);
                        native_legacy_display_checkpoint();return;
                    }
                }
            }
        }
        native_legacy_stage=6;finishUnitScalePolicy(false);
        if(started)display->stop(nub);if(attached)display->detach(nub);
        if(dtAttached)nub->detachFromParent(displayTreeParent,gIODTPlane);
        if(nubStarted)nub->stop(parent);if(nubAttached)nub->detach(parent);
        OSSafeReleaseNULL(ranges);OSSafeReleaseNULL(memory);OSSafeReleaseNULL(nub);
        OSSafeReleaseNULL(displayTreeParent);
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    native_legacy_display_checkpoint();
}
}
