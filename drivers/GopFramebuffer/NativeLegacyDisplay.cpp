#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
// The component flushes the surface's lines before it copies them (upstream NativeLegacyDisplay.cpp does this
// immediately before its copy). The renderer wrote the frame through its own mapping; a read through the
// kernel's must not be able to see stale lines. XNU exports this from osfmk/arm64/machine_routines_asm.s.
extern "C" void flush_dcache64(addr64_t addr,unsigned count,int phys);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
// The compositor's swapped surfaces, for a display that fetches them itself: [0] distinct surfaces seen, [1] swaps checked; for the first four, at
// [2 + 3 * k]: allocation bytes, physical segments, largest segment (KiB). The pages are wired for the count and released.
extern "C" {volatile uint32_t native_legacy_surfaces[14];}
// Direct scanout (sheng-dpu-scanout=2): the display's two pipes fetch the compositor's surface through the display bank's window
// (NativeUfs native_ufs_dpu_map). [0] flips, [1] surfaces mapped, [2] map failures, [3] latches that took over 50 ms, [4] longest latch (us),
// [5] latch total (us, for the mean), [6] surfaces let go, [7] swaps that fell back to the copy, [8] flips back to the boot memory.
extern "C" {volatile uint32_t native_legacy_direct[9];}
/* Frames that became visible and what the path cost them. S0a of the 120 Hz specification: this array is instrumentation
 * only and changes no control flow.
 *   [0] frames that became visible                    [1] by direct scanout (the latch)   [2] by the CPU copy
 *   [3] by the GPU client's copy     [4] blank frames  [5] frame-to-frame intervals measured (us)  [6] interval ring head
 *   [7] the longest interval (us)    [8] intervals that went backwards, clamped to 0 (two writers)
 *   [9] budget samples (validator accept -> visible)   [10] unused (kept so the offsets above stay fixed)
 *   [11] the longest budget (us)     [12] visible frames with no accepted stamp (the probe is not linked)
 *   [13] visible frames whose accepted stamp was ahead of now (another writer's clock; no budget invented)
 *   [14] the budget ring's head
 *   [16..143]  the last 128 intervals in microseconds [144..271] the last 128 budgets in microseconds
 * The rings let the board log print p50/p95 exactly instead of approximating from buckets. Both writers (the work loop's
 * latch and the GPU client's completion) are lock-free here on purpose: a frame counter must not take a lock the waiting
 * path could hold, and a lost sample only skews a percentile, never behaviour. */
enum {NativeLegacyVisibleRing=16,NativeLegacyVisibleRingLen=128,NativeLegacyVisibleBudget=144,NativeLegacyVisibleWords=272};
extern "C" {volatile uint32_t native_legacy_visible[NativeLegacyVisibleWords];}
/* Totals that would wrap inside a uint32 microsecond counter (71.6 minutes): [0] the intervals' sum, [1] the budgets' sum. */
extern "C" {volatile uint64_t native_legacy_visible_sum[2];}
/* The most recently accepted swap's clock, written by the display probe at the validator's accept. It is the kernel's first
 * touch of a frame, so accepted->visible contains the whole submission path; a stamp taken when this driver picked the
 * swap would happen after the submission had already returned and would understate the frame by exactly that cost. */
extern "C" volatile uint64_t native_legacy_swap_accepted_us __attribute__((weak_import));
static uint64_t visibleLastTick;    /* tick of the last visible frame; written with an atomic exchange */
static void noteFrameVisible(unsigned kind)
{
    const uint64_t nowTick=mach_absolute_time();
    uint64_t ns=0;absolutetime_to_nanoseconds(nowTick,&ns);
    const uint64_t us=ns/1000;
    native_legacy_visible[0]=native_legacy_visible[0]+1;
    if(kind<5)native_legacy_visible[kind]=native_legacy_visible[kind]+1;
    const uint64_t prev=__atomic_exchange_n(&visibleLastTick,nowTick,__ATOMIC_RELAXED);
    if(prev) {
        uint64_t dns=0;absolutetime_to_nanoseconds(nowTick>prev?nowTick-prev:0,&dns);
        const uint32_t delta=uint32_t(dns/1000);
        if(nowTick<prev)native_legacy_visible[8]=native_legacy_visible[8]+1;
        const uint32_t at=native_legacy_visible[6];
        native_legacy_visible[NativeLegacyVisibleRing+(at%NativeLegacyVisibleRingLen)]=delta;
        native_legacy_visible[6]=at+1;
        native_legacy_visible[5]=native_legacy_visible[5]+1;
        if(delta>native_legacy_visible[7])native_legacy_visible[7]=delta;
        native_legacy_visible_sum[0]=native_legacy_visible_sum[0]+delta;
    }
    const uint64_t acceptedUs=&native_legacy_swap_accepted_us?native_legacy_swap_accepted_us:0;
    if(!acceptedUs) native_legacy_visible[12]=native_legacy_visible[12]+1;      /* no stamp: the probe is not linked */
    else if(acceptedUs>us) native_legacy_visible[13]=native_legacy_visible[13]+1;  /* another writer's clock: do not invent a budget */
    else {
        const uint64_t span=us-acceptedUs;
        const uint32_t budget=uint32_t(span>0xffffffffull?0xffffffffull:span);
        const uint32_t at=native_legacy_visible[14];
        native_legacy_visible[NativeLegacyVisibleBudget+(at%NativeLegacyVisibleRingLen)]=budget;
        native_legacy_visible[14]=at+1;
        native_legacy_visible[9]=native_legacy_visible[9]+1;
        native_legacy_visible_sum[1]=native_legacy_visible_sum[1]+budget;
        if(budget>native_legacy_visible[11])native_legacy_visible[11]=budget;
    }
}
extern "C" uint32_t native_ufs_dpu_map(unsigned slot,const uint64_t *pages,unsigned count) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
static void noteSurface(OSObject *surface,IOMemoryDescriptor *memory)
{
    static OSObject *seen[4];
    native_legacy_surfaces[1]=native_legacy_surfaces[1]+1;
    for(OSObject *known:seen)if(known==surface)return;
    const unsigned k=native_legacy_surfaces[0];
    native_legacy_surfaces[0]=k+1;
    if(k>=4)return;
    seen[k]=surface;
    if(memory->prepare()!=kIOReturnSuccess)return;
    uint32_t segments=0;uint64_t largest=0;
    for(IOByteCount offset=0,length=0;offset<memory->getLength() && segments<100000;offset+=length,++segments) {
        if(!memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone) || !length)break;
        if(length>largest)largest=length;
    }
    memory->complete();
    native_legacy_surfaces[2+3*k]=uint32_t(memory->getLength());native_legacy_surfaces[3+3*k]=segments;native_legacy_surfaces[4+3*k]=uint32_t(largest>>10);
}
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}

// The compositor also hands full-screen frames in Apple's two-plane lossless 'b3a8'
// spelling. The pixels sit in plane 0 as plain 32-bit BGRA rows, but the stride accessor
// reports the LAST plane's row, i.e. width bytes instead of the width*4 the copy must
// step, so the source row is derived from the display width rather than read back.
// Provenance: the opi5plus port reached its lock screen on exactly this shape (54 swaps
// accepted, 0 refused, commit 1ef2c87b); no other board in this project accepts it yet.
static inline bool nativeLosslessBgra(uint32_t format,uint32_t planes) {
    return (format==0x62336138 && planes==2);
}
static inline uint64_t nativeSourceRow(uint32_t format,uint32_t planes,uint64_t reported) {
    return nativeLosslessBgra(format,planes)?reported*4:reported;
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    uint32_t scanBudget=32;
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    // Direct scanout state, used on the work loop only. MDP node at 0xae01000: VIG0/VIG1 at 0x4000 and
    // 0x6000 (SRC0_ADDR +0x14, SRC_YSTRIDE0 +0x24; VIG1 takes the right half through its own source x,
    // which this board's probe reads as xy=0x320), CTL0 at 0x1000 (flush +0x18). The flush bits are
    // the one part of this path never exercised on kona: bit0/bit1 for the two pipes and bit17 for the
    // CTL come from the mainline DPU 6.0 driver, not from a measurement here.
    struct DirectSlot {OSObject *surface;IOMemoryDescriptor *memory;uint32_t iova;uint64_t used;};
    static constexpr unsigned kDirectSlots=4;
    DirectSlot directSlots[kDirectSlots]={};
    IOMemoryMap *mdpMap=nullptr;
    volatile uint32_t *mdp=nullptr;
    uint64_t *directPages=nullptr;
    IOTimerEventSource *latchTimer=nullptr;
    const OriginalLegacySwapRequest *latchRequest=nullptr;
    uint64_t latchStart=0,directClock=0;
    int shownSlot=-1,pendingSlot=-1;
    bool directOn=false,directBoot=true;
    /* kBootIova/kBootStride are what directToBoot() programs into both VIG pipes when the window is
     * torn down, so they have to be THIS board's firmware scanout, not sheng's: 0x9c000000 with a
     * 6400-byte row, the base and row this board's own [swapok] row reports. They were sheng's
     * 0xb8000000/0x2fa0, which meant the "fall back to the boot scanout" claim was writing a
     * different address from the one the copy path writes. */
    /* kVig is correct: the driver maps the MDP node at 0xae01000, the vendor device tree lists
     * sspp-off 0x5000/0x7000 from 0xae00000, i.e. 0x4000/0x6000 from this base, and mainline
     * dpu_6_0_sm8250.h agrees. kCtlFlush was SM8550's 0x15018, an address with no register on kona.
     * ONE CANDIDATE explanation for the earlier =2 round stopping after a single accepted swap is
     * a store to that empty address; that round's userspace also failed to come up, which explains
     * it without any hang, so the cause is not settled. CTL0 here is 0x1000 (vendor ctl-off 0x2000
     * from 0xae00000) and CTL_FLUSH is +0x18, so 0x1018.
     * flip() rewrites only SRC0_ADDR and YSTRIDE0, so it is correct only while the surface keeps the
     * firmware geometry and format (1600x2560, 8-bit BGRA, stride 6400); any other surface would scan
     * the wrong region silently instead of failing. */
    static constexpr uint32_t kVig[2]={0x4000,0x6000},kCtlFlush=0x1018,kBootIova=0x9c000000U,kBootStride=0x1900;
    void startDirect() {
        unsigned mode=0;PE_parse_boot_argn("sheng-dpu-scanout",&mode,sizeof(mode));
        /* The window's state word is either the installed form 0x80000000|bank or the refusal
         * 0xdead0001, and BOTH have bit 31 set: testing state>>31 alone accepts a refused window and
         * hands the DPU an empty page table. Accept only the installed shape - bit 31 set and every
         * bit above the low byte clear - so a refusal is refused here too. */
        const uint32_t directState=native_ufs_dpu_state;
        const bool directInstalled=(directState&0x80000000U) && !(directState&0x7fffff00U);
        static int directShown=0;
        const bool directProbe=(directShown==0);
        if(directProbe) directShown=1;
        if(directProbe) kprintf("[dpu start] mode=%u mapSym=%d state=%08x installed=%d primary=%d\n",
            mode,( &native_ufs_dpu_map )?1:0,(unsigned)directState,directInstalled?1:0,primary?1:0);
        if(mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state || !directInstalled || !primary)return;
        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);
        mdpMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(memory)memory->release();
        directPages=static_cast<uint64_t *>(IOMallocData(8192*sizeof(uint64_t)));
        IOWorkLoop *loop=getWorkLoop();
        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;
        const IOReturn addResult=(latchTimer && loop)?loop->addEventSource(latchTimer):(IOReturn)0xffffffffU;
        if(directProbe) kprintf("[dpu start] mdpMap=%d pages=%d timer=%d loop=%d add=%08x\n",
            mdpMap?1:0,directPages?1:0,latchTimer?1:0,loop?1:0,(unsigned)addResult);
        if(!mdpMap || !directPages || !latchTimer || !loop || addResult!=kIOReturnSuccess) {
            OSSafeReleaseNULL(mdpMap);
            /* addEventSource may already have chained the timer; releasing it without removing it
             * leaves the loop holding a retain on a source whose owner is gone, and stop() can no
             * longer reach it because latchTimer is nulled right after. This is the failure path
             * the probe watches, so it has to be correct. */
            if(latchTimer && loop && addResult==kIOReturnSuccess) loop->removeEventSource(latchTimer);
            OSSafeReleaseNULL(latchTimer);
            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
            return;
        }
        mdp=reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());
        /* sheng-dpu-map-only=1 proves the mapping and then stops: directOn stays false, so the frame
         * path is exactly the =1 round's. It answers whether mapping 0xae01000 is what leaves the
         * compositor with nothing to render, without also running anything that depends on directOn. */
        /* 3 is this argument's "map but stop" value. The command line has no room for a separate
         * sheng-dpu-map-only (991 of 1000 bytes), and no reader requires exactly 2: BootLog tests
         * non-zero and DpuWindow.h builds for any non-zero, so =3 installs the same window and
         * prints the same [direct scanout] row as =2. */
        unsigned mapOnly=(mode>=3)?1u:0u;
        if(directProbe) kprintf("[dpu start] mdp=%p mapOnly=%u directOn=%u\n",
            (void *)mdp,mapOnly,mapOnly?0u:1u);
        if(!mapOnly) directOn=true;
    }
    void flip(uint32_t iova,uint32_t stride) {
        for(uint32_t vig:kVig){mdp[(vig+0x14)/4]=iova;mdp[(vig+0x24)/4]=stride&0xffff;}
        __asm__ volatile("dsb sy" ::: "memory");
        /* Two pipe bits and the CTL commit bit. ASSUMED, not measured: that the two VIG pipes are flush
         * bits 0 and 1, and that DPU 6.0 needs bit 17 (CTL_FLUSH_MASK_CTL) for the commit to be
         * consumed. No flip has been observed to latch on this port. If flips climbs while the picture
         * stays, this mask is the first suspect; the alternatives are that the pipe bits are LM bits
         * rather than VIG bits, and that bit 17 is not the CTL commit. The register self-clears once
         * the hardware takes it, which is what latchTick polls. */
        mdp[kCtlFlush/4]=0x20003;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    // The slot that holds this surface, mapping it into a free one (never the one on screen or the one waiting to latch) the first time.
    int directSlot(OSObject *surface,IOMemoryDescriptor *memory) {
        ++directClock;
        int pick=-1;
        for(unsigned k=0;k<kDirectSlots;++k)if(directSlots[k].surface==surface && directSlots[k].memory==memory){directSlots[k].used=directClock;return int(k);}
        for(unsigned k=0;k<kDirectSlots;++k) {
            if(int(k)==shownSlot || int(k)==pendingSlot)continue;
            if(pick<0 || !directSlots[k].surface || (directSlots[pick].surface && directSlots[k].used<directSlots[pick].used))pick=int(k);
        }
        if(pick<0)return -1;
        DirectSlot &slot=directSlots[pick];
        if(slot.surface) {
            slot.memory->complete();OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);
            native_legacy_direct[6]=native_legacy_direct[6]+1;
        }
        if(memory->getLength()>8192ull*4096 || memory->prepare()!=kIOReturnSuccess){native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        unsigned count=0;bool ok=true;
        for(IOByteCount offset=0;offset<memory->getLength() && ok;) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!pa || (pa&0xfff) || !length){ok=false;break;}
            for(IOByteCount at=0;at<length && offset+at<memory->getLength();at+=4096)directPages[count++]=pa+at;
            offset+=length;
        }
        const uint32_t iova=ok?native_ufs_dpu_map(unsigned(pick),directPages,count):0;
        if(!iova){memory->complete();native_legacy_direct[2]=native_legacy_direct[2]+1;return -1;}
        surface->retain();memory->retain();
        slot={surface,memory,iova,directClock};
        native_legacy_direct[1]=native_legacy_direct[1]+1;
        return pick;
    }
    static void latchFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->latchTick();}
    void latchTick() {
        if(!latchRequest)return;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchStart,&ns);
        if((mdp[kCtlFlush/4]&0x3) && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}
        if(ns>=50000000ull)native_legacy_direct[3]=native_legacy_direct[3]+1;
        const uint32_t us=uint32_t(ns/1000);
        if(us>native_legacy_direct[4])native_legacy_direct[4]=us;
        native_legacy_direct[5]=native_legacy_direct[5]+us;
        shownSlot=pendingSlot;pendingSlot=-1;
        const OriginalLegacySwapRequest *request=latchRequest;latchRequest=nullptr;
        finishTransfer(request,true);
        noteFrameVisible(1);
    }
    // Back to the boot loader's memory (the blank frame and the copy are drawn there).
    void directToBoot() {
        if(!directOn || directBoot)return;
        flip(kBootIova,kBootStride);directBoot=true;shownSlot=-1;
        native_legacy_direct[8]=native_legacy_direct[8]+1;
    }
    // What the display controller is fetching would be the other half of the white-line question, and reading
    // it directly is not possible from here: mapping the MDP block (0xae01000) and reading VIG 0/1's registers
    // aborts the kernel right after the first copy ("Unclassified kernel abort", 2026-11-15 23:43, copy=1
    // printed and nothing after it), so the block is not safely readable without the clocks and the power
    // domain the direct-scanout path sets up first. The pipes are therefore still the firmware's: nothing in
    // this driver writes them, and the firmware points them at the GOP framebuffer the copy lands in.
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {stopVsync();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;gpuRetired=false;
        directToBoot();latchRequest=nullptr;pendingSlot=-1;
        if(latchTimer){latchTimer->cancelTimeout();if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(latchTimer);OSSafeReleaseNULL(latchTimer);}
        directOn=false;
        /* Everything the direct path allocated, released here and nowhere else: the four slots' surface and
         * memory references (taken by directSlot(), dropped when a slot is reused but never at stop), the
         * page array handed to the DPU, the MDP window and the differential-copy shadow (up to 64 MiB). The
         * DPU's own map has no unmap entry point on this driver, so the slot iovas go with the pages. */
        for(auto &slot:directSlots) {
            if(slot.memory){slot.memory->complete();OSSafeReleaseNULL(slot.memory);}
            OSSafeReleaseNULL(slot.surface);
        }
        if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
        mdp=nullptr;OSSafeReleaseNULL(mdpMap);
        if(shadow){IOFreeAligned(shadow,(vm_size_t)shadowBytes);shadow=nullptr;}
        shadowBytes=0;shadowValid=false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();startDirect();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // The original maximum interval is 1/24 s (int-240), which offers the render server a 24 to sheng-vsync Hz range. The panel has no
    // variable refresh, so sheng-vsync-fixed=1 reports the maximum interval equal to the minimum: one fixed rate.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        /* S0a: nothing is stamped here. The budget's start is the validator's accept (native_legacy_swap_accepted_us), which is earlier than this pick and therefore contains the submission the frame waited on. */
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            directToBoot();
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            {finishTransfer(request);noteFrameVisible(4);}
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=nativeSourceRow(native_legacy_surface_format(surface),
            native_legacy_surface_planes(surface),native_legacy_surface_stride(surface));
        uint64_t allocation=native_legacy_surface_allocation(surface);
        /* Split into named terms and report the failing one. This check sits before BOTH the direct
         * path's counters and the CPU copy's row, so when all of them read zero (sheng-dpu-scanout=2
         * round: surfaces=0 flips=0 fallback=0 and no DbyDisplay copy= at all, where =1 printed
         * copy=6..8) the only way to tell which term rejected is to print them. One line per failure,
         * not per frame, so it cannot flood the console. */
        const bool chkMemory=memory && memory->metaCast("IOMemoryDescriptor");
        const bool chkSurfBounds=chkMemory && native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength());
        const bool chkSurfW=native_legacy_surface_width(surface)>=width;
        const bool chkSurfH=native_legacy_surface_height(surface)>=height;
        const bool chkFormat=(nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
                              nativeLosslessBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)));
        const bool chkConsole=PE_current_console(&video)==0;
        const bool chkBase=chkConsole && video.v_baseAddr==bootMapping->getPhysicalAddress();
        const bool chkGeom=chkConsole && video.v_width==width && video.v_height==height && video.v_depth==32 && !video.v_rotate;
        const bool chkBootBounds=chkConsole && native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength());
        if(!(chkMemory && chkSurfBounds && chkSurfW && chkSurfH && chkFormat && chkConsole && chkBase && chkGeom && chkBootBounds)) {
            static unsigned shown;
            if(shown++<3) {
                kprintf("[dpu reject] mem=%d surfBounds=%d surfW=%d(%u/%u) surfH=%d(%u/%u) fmt=%d(%08x pl=%u) console=%d base=%d(%llx/%llx) geom=%d(%ux%u/%ux%u d%u r%u) bootBounds=%d\n",
                    chkMemory?1:0, chkSurfBounds?1:0,
                    chkSurfW?1:0,(unsigned)native_legacy_surface_width(surface),(unsigned)width,
                    chkSurfH?1:0,(unsigned)native_legacy_surface_height(surface),(unsigned)height,
                    chkFormat?1:0,(unsigned)native_legacy_surface_format(surface),(unsigned)native_legacy_surface_planes(surface),
                    chkConsole?1:0, chkBase?1:0,
                    (unsigned long long)video.v_baseAddr,(unsigned long long)bootMapping->getPhysicalAddress(),
                    chkGeom?1:0,(unsigned)video.v_width,(unsigned)video.v_height,(unsigned)width,(unsigned)height,
                    (unsigned)video.v_depth,(unsigned)video.v_rotate,
                    chkBootBounds?1:0);
            }
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        noteSurface(surface,memory);
        if(directOn && !gpuBypass && !latchRequest) {
            const int slot=directSlot(surface,memory);
            if(slot>=0) {
                native_legacy_swap_stage(1);
                flip(directSlots[slot].iova,uint32_t(sourceStride));directBoot=false;
                native_legacy_direct[0]=native_legacy_direct[0]+1;
                shadowValid=false;pendingSlot=slot;latchRequest=request;transferred=true;
                latchStart=mach_absolute_time();latchTimer->setTimeoutUS(500);
                return;
            }
            native_legacy_direct[7]=native_legacy_direct[7]+1;
        }
        directToBoot();
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof(early));
            if(early==1 && retireSwap(request,true)) {
                gpuRetired=true;native_legacy_early_retires=native_legacy_early_retires+1;
            }
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The renderer wrote this frame through its own mapping; drop what the CPU may still hold for it
        // before anything below reads it through the kernel's, the way the component does before its copy.
        flush_dcache64(reinterpret_cast<addr64_t>(source->getVirtualAddress()),(unsigned)source->getLength(),false);
        // One console row per 32 copies: the first pixels of the compositor's surface and of the scanout
        // separate a surface the renderer never drew (empty or white) from a copy that lands somewhere
        // else. The 2026-11-15 boot showed white horizontal lines on the panel and this is the reading
        // that says which half of the path produced them.
        {
            static unsigned copies;
            if(++copies<=8u || (copies & 63u)==0u) {
                const uint32_t *srcp=reinterpret_cast<const uint32_t *>(source->getVirtualAddress());
                const volatile uint32_t *dstp=reinterpret_cast<const volatile uint32_t *>(bootMapping->getVirtualAddress());
                kprintf("DbyDisplay: copy=%u fmt=%08x stride=%llu src=%08x %08x %08x dst=%08x %08x %08x\n",
                    copies,(unsigned)native_legacy_surface_format(surface),(unsigned long long)sourceStride,
                    srcp[0],srcp[1],srcp[width/2],dstp[0],dstp[1],dstp[width/2]);
            }
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        {finishTransfer(request);noteFrameVisible(2);}
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired=false;
            if(status==0){native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);noteFrameVisible(3);}
            else {native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_gpu_fallback_observed(status);gpuBypass=true;}
            transferred=false;kickSwapQueue();return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);noteFrameVisible(3);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!retireSwap(request,force))return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       (nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
                        nativeLosslessBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface))) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,
                           nativeSourceRow(native_legacy_surface_format(surface),native_legacy_surface_planes(surface),
                               native_legacy_surface_stride(surface)),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
