/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,
    SubmitMaxBuffers=(uint32_t)NativeAdrenoSubmitMaxBuffers,SubmitMaxIbBytes=0xfffffu*4u,
    SubmitAuditBytes=256u*1024u};   /* IB bytes the write-combine audit compares per submission */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
/* IBs per submission: [0] submissions, [1] IBs, [2] the most in one, [3] submissions with more than 16 (one frame was several before). */
extern "C" {volatile uint32_t native_adreno_submit_ibs[4];}
/* Where a submission's time goes, in mach_absolute_time ticks summed over the successful ones: [0] the buffers' validation,
 * [1] the sweep of completed jobs and the job references, [2] the write-combine re-assertion and audit, [3] the BO publication
 * (cache maintenance), [4] the coherence sample after it, [5] the IB record and the ring write; [6] the count. The board log
 * divides each by the count, so a submission that costs the render thread milliseconds shows which part does. */
extern "C" {volatile uint64_t native_adreno_submit_phase[7];}
extern "C" uint64_t mach_absolute_time(void);
/* The submission (1-based count) at which the write-combine audit found a dirty IB line or failed; 0 = never. */
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
/* The write-combine audit before each submission while trusted: [0] submissions audited, [1] IB lines compared, [2] dirty lines,
 * [3] stale clean lines, [4] the last audit failure (any failure also ends the trust), [5] pages found back at the default attribute
 * and write-combined again. */
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The CP's state once a fence stopped advancing (NativeAdrenoRing::stallState): [0] the fence, [1] the timed-out waits of it so far, then
 * the ring's words. Taken twice per stalled fence, gated on time rather than on a count: snapshot_due() below returns
 * true after 50 ms and again after 1 s of the fence that is the next to complete having made no progress.
 * native_adreno_stall_mem
 * holds, from the same moment, the SMMU context's FSR, FAR (lo, hi) and FSYNR0, then four 17-word windows at indices 4, 21, 38 and 55: the
 * BR IB1, the BV IB1, the packet CP_SQE_STAT says the SQE holds (a generation-6 core has no BV thread, so that slot used to read deadbeef
 * only), and the word that packet polls when it is a CP_WAIT_REG_MEM on memory (marker = its IOVA, then that word and its three
 * neighbours, then the same word as the previous snapshot saw it, whether it moved, and how many snapshots this fence has taken; the
 * rest is deadbeef). The map
 * handles sit after them at 72..75. Each of the first three windows is a marker followed by 16 words. Marker 0 means the 16 words begin at the
 * IB's first dword: either the ring packet before the read
 * pointer named the IB and the CP is within 12 dwords of its start, or only CP_IB1_BASE was known (the marker does not say which).
 * 0xffff0000 + N means N words before the fetch address, the IB start being unknown. For the third window the marker packs two things: the
 * top nibble is 0 when the packet was found in the BR IB1 itself, 1..3 when it was found in a CP_INDIRECT_BUFFER target that IB1 chains
 * to, counted backwards from the cursor: 1 is the newest (the last such packet at or before the dword the CP has fetched to), 2..3 the ones
 * before it, because the CP sits in the most recent buffer it was handed, and 4 when it was found in IB2 (CP_IB2_BASE). The low bits are the dword the packet starts at. 0xffffffff means
 * it was not found: no ring record for the buffer, a buffer of one dword or over 256, an unreadable buffer, a failed CP_SQE_STAT read (its
 * data is 0xffffffff then, and searching for it is refused), a header of 0 (never a real PM4 header), the packet lying beyond the dword the
 * CP has fetched to, or more than three chained buffers to search. Words past the buffer's end, and the whole window when nothing was found,
 * are deadbeef.
 * The last 4: the map handle that was next when the stalled job was submitted, then the handles of the mappings under the BR IB1, the BV IB1
 * and the BV IB2 (0: unmapped). A handle at or above the first was mapped after the submission: its memory was freed and reused under the GPU. */
extern "C" {volatile uint32_t native_adreno_stall[2+NativeAdrenoRingStallWords];volatile uint32_t native_adreno_stall_mem[4+4*17+4];}
/* The stalled submission's own buffer list and one hop of CP_INDIRECT_BUFFER from it: [0] entries of the first group, [1] of the second,
 * then 4 x (iova low, dwords scanned, CP_WAIT_REG_MEM headers found) for the submitted buffers and 4 more for their chained targets. The
 * all-ones wait drop only scans what the submission names (NativeAdrenoVm::dropAllOnesWaits), so this is what says whether the packet the CP
 * is stuck on was even handed to it. */
extern "C" {volatile uint32_t native_adreno_ib_chain[2+4*3+4*3];}
/* 16 words of the IB at start, from 12 dwords before the dword the thread has reached (size minus what remains). */
static void peekIb(const NativeAdrenoVm *vm,uint64_t start,uint32_t size,uint32_t remaining,volatile uint32_t *slot)
{
    uint32_t words[16]={};
    const uint32_t reached=remaining<=size?size-remaining:0,first=reached>12?reached-12:0;
    if(vm->peekWordsUncached(start+4ull*first,words,16))for(unsigned i=0;i<16;++i)words[i]=0xdeadbeef;
    slot[0]=first;
    for(unsigned i=0;i<16;++i)slot[1+i]=words[i];
}
/* 16 words around a fetch address whose IB start is unknown: from 12 words before it, or nearer when that crosses into no buffer. */
static void peekFetch(const NativeAdrenoVm *vm,uint64_t fetch,volatile uint32_t *slot)
{
    uint32_t words[16]={},before=12;
    while(before && vm->peekWordsUncached(fetch-4*before,words,16))before-=4;
    if(!before && vm->peekWordsUncached(fetch,words,16))for(unsigned i=0;i<16;++i)words[i]=0xdeadbeef;
    slot[0]=0xffff0000u+before;
    for(unsigned i=0;i<16;++i)slot[1+i]=words[i];
}
/* 16 words from the IB's own first dword. Used when no ring packet names the stalled IB, so only CP_IB1_BASE
 * does: the fetch-relative form then reads a page the command stream may never have touched, and sixteen zeros
 * cannot be told apart from "this buffer was never written". Marker 0 marks this form, so a row says which one
 * it is: 0 = from the IB's start, ffff000N = N words before the fetch address. */
static void peekStart(const NativeAdrenoVm *vm,uint64_t start,volatile uint32_t *slot)
{
    uint32_t words[16]={};
    if(vm->peekWordsUncached(start,words,16))for(unsigned i=0;i<16;++i)words[i]=0xdeadbeef;
    slot[0]=0;
    for(unsigned i=0;i<16;++i)slot[1+i]=words[i];
}
static uint32_t stall_fence,stall_timeouts,stall_stage;   /* stage: 0 none yet, 1 write pointer given again, 2 translations dropped, 3 bus drained */
static uint64_t stall_began_us,stall_last_us;   /* the fence's first and latest timed-out wait */
/* Does a CP store reach a client BO? The userspace self-test writes three markers into its own target page with CP_MEM_WRITE packets and
 * reads them back; the driver reads the same page once and prints it. Nothing is written here: this is the read side only. The engine
 * self-tests report their stores never land (evidence/edl-log-s119), while the ring's own fence - a CP store - keeps advancing, so this
 * separates "a client BO cannot be written by the GPU at all" from "the engine never runs". Two phases, because the page still holds the
 * client's sentinels when the packet is submitted: submit() captures the target address out of the first IB carrying the self-test's
 * three-packet run, and wait() reads it once the fence has passed. [0] set once the page was read, [1..2] its IOVA low/high, [3..6] the
 * four dwords, [7] the fence of the submission that was read. */
extern "C" {volatile uint32_t native_adreno_tgt[8];}
/* The capture and the read are two calls, so the pending target carries the address space it was captured in: every userspace client
 * submits its own VM (NativeAdrenoSubmit.h:54), and the member vm_ is the kernel's default table (NativeAdrenoSubmit.h:72), so a read
 * that did not name the job's VM would look up a client address in the wrong page table and find nothing. */
static uint64_t tgt_pending;              /* captured at submit, read at the next wait that completes */
static const NativeAdrenoVm *tgt_pending_vm;   /* the VM the capture was made in; a wait in another VM never reads it */
/* Restarts in a row that the board's hook refused, so one failure is not the submit path's death: see recover(). */
static uint32_t restart_failures;
/* How long fences stayed pending from their first timed-out wait to their last: [i] under 1 << i ms for i < 7, [7] 64 ms or more. */
extern "C" {volatile uint32_t native_adreno_fence_pending[8];}
/* What the stall handler tried: [0] first-stage attempts (the write pointer given again after 50 ms), [1] the fence of the last
 * attempt, [4] bus drains, [5] client halt acknowledge, [6] arbiter halt acknowledge, [7] the fence of the last drain, [9] first-stage
 * recoveries, [11] bus-drain recoveries, [12] the most timed-out waits any one stall accumulated, [13] the wait timeout in us.
 * Three fields of this row are never written and read as zero, which is what the code does rather than an event that did not happen:
 * [2]/[3] ("attempts after which that fence completed" and "the waits it had seen") and [8] ("drains after which that fence
 * completed") were in an earlier design, [10] belongs to a stage-2 "translations dropped" step the handler never takes - it goes from
 * 1 straight to 3 - and [8+0] is excluded by the stall_stage test that guards the increment. porting.toml's gpu limits record that
 * the row prints three fields the driver does not maintain. */
extern "C" {volatile uint32_t native_adreno_unstick[15];}
/* The last four submissions' ring words, linearised by NativeAdrenoRing::submitWords. */
extern "C" {extern volatile uint32_t native_adreno_ring_log[4][4+112];extern volatile uint32_t native_adreno_ring_log_count;}
/* The dword count of the indirect buffer at `base`, from the record of the submission that owns `fence`.
 * A stalled fence usually means the CP has already fetched past the CP_INDIRECT_BUFFER that named the stalled
 * IB, so ibBefore() refuses: without the count the dump cannot tell where in the IB the fetch is, and the
 * fetch-relative form reads before the buffer (sixteen zeros that look like an empty IB). The fence selects
 * the record, because a later submission may reuse the same base with a different size, or never reach the CP
 * at all (the record is written before the write pointer is given). No matching record returns 0. */
static uint32_t ibDwordsAt(uint32_t fence,uint64_t base)
{
    const uint32_t count=native_adreno_ring_log_count;
    uint32_t header=0;
    if(!count || AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)return 0;
    for(uint32_t back=0;back<4 && back<count;++back) {
        const volatile uint32_t *r=native_adreno_ring_log[(count-1-back)&3];
        if(r[0]!=fence)continue;
        const uint32_t words=r[3]<112?r[3]:112;
        for(uint32_t i=0;i+3<words;++i)
            if(r[4+i]==header && ((uint64_t)r[4+i+2]<<32|r[4+i+1])==base)return r[4+i+3]&0xfffffu;
        return 0;
    }
    return 0;
}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
/* [0] fence, [1..2] the submission's iova, [3] its bytes, [4..5] the MAPPING's iova the handle resolves to (the
 * base the buffer sits in, so a nonzero [1] minus [4] says the IB starts inside its mapping), [6..33] the head. */
/* The head is read twice: [6..101] through peekWords (the board's dramRead32 hook, a fresh uncached
 * mapping per page) and [102..197] through peekWordsUncached (the kernel CPU view with cache
 * maintenance first). The two branches in NativeAdrenoVm are different code paths, and a client IB
 * that reads as zeros through one and real packets through the other says the reader is wrong, not
 * the GPU. [198] the two statuses, [199] how many words differ. */
extern "C" {volatile uint32_t native_adreno_ib_log[4][6+96+96+2+16+80];volatile uint32_t native_adreno_ib_log_count;}
extern "C" volatile uint32_t native_adreno_table_switches[8];
/* CP_WAIT_FOR_IDLE packets the board removed from client IBs before the CP saw them. */
extern "C" volatile uint32_t native_adreno_wfi_dropped=0;   /* a defining initialiser: bare extern "C" is only a declaration */
/* Why the drop found nothing: [0] buffers looked at, [1] no mapping for the iova, [2] mapping exists but no
 * CPU view, [3] buffers scanned, [4] the IOReturn the last refusal returned. */
extern "C" volatile uint32_t native_adreno_wfi_try[5]={};
/* The all-ones CP_WAIT_REG_MEM packets the board removed for the same reason (NativeAdrenoVm::dropAllOnesWaits):
 * how many were replaced, and why a buffer produced none - [0] buffers handed to the drop, [1] refusals that were
 * not a bad argument, [2] refusals because the iova or the length did not fit a mapping, [3] the bytes those buffers
 * cover. native_adreno_wait_last is the status of the last refusal. The three readings separate the cases the
 * 2026-11-08 boot could not: calls=0 means the drop never ran, dropped=0 with calls>0 means it ran and the packet is
 * not in any submitted IB, dropped>0 with the stall still in the log means the replacement did not reach the CP. */
extern "C" volatile uint32_t native_adreno_wait_dropped=0;
extern "C" volatile uint32_t native_adreno_wait_hops=0;   /* of `dropped`, how many were in a buffer reached through CP_INDIRECT_BUFFER */
extern "C" volatile uint32_t native_adreno_wait_try[4]={};
extern "C" volatile uint32_t native_adreno_wait_last=0;
extern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);
/* Where the last recover() stopped: [0] the step (1 fence read, 2 restart hook, 3 table write, 4 nop encode, 5 replay vm, 6 replay submit), [1] the IOReturn or value there, [2] fence, [3] last submitted. */
extern "C" {volatile uint32_t native_adreno_recover_trace[4];}
/* The jobs' timeline (the newest native_adreno_tl_cap retired, allocated at attach): [0] fence | indirect buffers << 32 | ring status << 58, [1] submit
 * time (us), [2] the CP's always-on counter before the job's buffers, [3] after them (19.2 MHz ticks; 0 when not stamped), [4] GPU level at submit |
 * the submitting pid << 32, [5] when the driver saw the job retired (us, the submission clock). native_adreno_tl_count is the total. */
extern "C" {volatile uint64_t (*native_adreno_tl)[6]=nullptr;volatile uint32_t native_adreno_tl_cap=0;volatile uint32_t native_adreno_tl_count;}
/* The stall handler's actions (the newest 32): [0] time (us), [1] fence, [2] event (1 write pointer given again and translations dropped, 3 bus drain,
 * 4 snapshot, 0 the stall ended) | timed-out waits << 8, [3] time the fence had been pending (us), [4] read and write pointer before | after << 32 for
 * events 1 and 3, [5] client queue, [6] the job's first buffer handle | its buffer count << 48, [7] the sequence number. */
extern "C" {volatile uint64_t native_adreno_stall_log[32][8];volatile uint32_t native_adreno_stall_log_count;}
extern "C" volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    Job **items=IONewData(Job *,jobs_.count+1);
    Job *job=items?new Job:nullptr;
    if(!job) {if(items)IODeleteData(items,Job *,jobs_.count+1);return nullptr;}
    for(size_t i=0;i<jobs_.count;++i)items[i]=jobs_.items[i];
    items[jobs_.count]=job;
    if(jobs_.items)IODeleteData(jobs_.items,Job *,jobs_.count);
    jobs_.items=items;++jobs_.count;
    return job;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};table_vm_=vm;
    if(!native_adreno_tl) {
        const uint32_t cap=16384;
        volatile uint64_t (*memory)[6]=(volatile uint64_t (*)[6])IOMallocZero(sizeof(uint64_t)*6*cap);
        if(memory) {native_adreno_tl=memory;native_adreno_tl_cap=cap;}
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::setTableSwitching(unsigned mode,IOReturn (*write)(void *,uint64_t),void *cookie)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(mode>2 || (mode && !write) || outstanding())return kIOReturnBadArgument;
    table_mode_=mode;write_table_=write;table_cookie_=cookie;table_vm_=vm_;
    if(mode==2)ring_->tableInstalled(vm_->ttbr0());
    return kIOReturnSuccess;
}
/* Makes vm the table the GPU runs for the next submission. Mode 1: once every earlier job has finished, the CPU writes TTBR0 (the bank
 * caches translations per ASID, so the switch needs no TLB operation). Mode 2: the ring's next prologue carries the switch, which the CP
 * runs after every earlier submission. */
IOReturn NativeAdrenoSubmit::useVm(NativeAdrenoVm *vm,const NativeAdrenoRingClock *clock,bool drained)
{
    if(!table_mode_ || vm==table_vm_)return kIOReturnSuccess;
    if(table_mode_==2) {ring_->useTable(vm->ttbr0());table_vm_=vm;return kIOReturnSuccess;}
    if(!clock)return kIOReturnBadArgument;
    /* Mode 1 (the CPU writes TTBR0) needs a quiet ring first. recover() empties the ring with its restart,
     * so on the replay path there is nothing to drain: waiting there spends the whole 2 s budget per
     * replayed job, and returns a timeout outright when the GPU is the thing that hung. */
    if(!drained) {
        IOReturn status=waitAll(clock,2000000);
        if(status)return status;
    }
    IOReturn status=write_table_(table_cookie_,vm->ttbr0());
    if(status)return poison();
    native_adreno_table_switches[1]=native_adreno_table_switches[1]+1;
    table_vm_=vm;return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::forgetVm(NativeAdrenoVm *vm,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    /* Before any early return: the table this pending target was captured in is going away, and a later VM can be allocated at the
     * same address with its window at the same 0x1_00000000. */
    if(vm && vm==tgt_pending_vm) {tgt_pending=0;tgt_pending_vm=nullptr;}
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!vm || vm==vm_)return kIOReturnBadArgument;
    IOReturn status=waitAll(clock,timeout_us);
    if(status)return status;
    if(table_vm_!=vm && !(table_mode_==2 && ring_->table()==vm->ttbr0()))return kIOReturnSuccess;
    /* The GPU is idle and every queued switch has run: the CPU puts the default table back. */
    status=write_table_?write_table_(table_cookie_,vm_->ttbr0()):kIOReturnNotReady;
    if(status)return poison();
    native_adreno_table_switches[1]=native_adreno_table_switches[1]+1;
    if(table_mode_==2)ring_->tableInstalled(vm_->ttbr0());
    table_vm_=vm_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    NativeAdrenoVm *vm=job.vm?job.vm:vm_;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    if(vm->acquireImported())return poison();
    {
        uint64_t start=0,end=0;
        const uint32_t stamped=ring_?ring_->timeline(job.fence,&start,&end):0;
        if(native_adreno_tl_cap) {
            const uint32_t n=native_adreno_tl_count;volatile uint64_t *slot=native_adreno_tl[n&(native_adreno_tl_cap-1)];
            slot[0]=(uint64_t)job.fence|(uint64_t)job.count<<32|(uint64_t)stamped<<58;slot[1]=job.submit_us;
            slot[2]=start;slot[3]=end;slot[4]=(uint64_t)job.level|(uint64_t)(uint32_t)job.owner<<32;
            slot[5]=clock_now_?clock_now_(clock_cookie_):0;
            native_adreno_tl_count=n+1;
        }
    }
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;
    tgt_pending=0;tgt_pending_vm=nullptr;   /* nothing can be read once the engine is detached */
    return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint64_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint64_t first,uint64_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint64_t queue,uint32_t *fence)
{
    return submit(nullptr,buffers,count,queue,fence,nullptr);
}
IOReturn NativeAdrenoSubmit::submit(NativeAdrenoVm *vm,const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint64_t queue,uint32_t *fence,const NativeAdrenoRingClock *clock)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!vm)vm=vm_;
    NativeAdrenoVm *const vm_=vm;   /* this submission's VM: every vm_ below is the job's own */
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    /* msm_ioctl_gem_submit refuses a VM a hang made unusable. */
    if(vm->unusable())return kIOReturnNotPermitted;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    native_adreno_submit_ibs[0]=native_adreno_submit_ibs[0]+1;native_adreno_submit_ibs[1]=native_adreno_submit_ibs[1]+(uint32_t)count;
    if(count>native_adreno_submit_ibs[2])native_adreno_submit_ibs[2]=(uint32_t)count;
    if(count>16)native_adreno_submit_ibs[3]=native_adreno_submit_ibs[3]+1;
    /* Phase 0 opens at the first buffer's validation and phase 1 at the sweep below; each later mark closes the section before it. */
    uint64_t phase[7];phase[0]=mach_absolute_time();
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    static_assert(sizeof(scratch_words_)/sizeof(scratch_words_[0])>=SubmitMaxBuffers*(1+SubmitIbWords),"ring words");
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    phase[1]=mach_absolute_time();
    IOReturn swept=retireCompleted();
    if(swept)return swept;
    Job *job=free();
    if(!job)return kIOReturnNoMemory;
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    phase[2]=mach_absolute_time();
    uint32_t value=0;
    /* int-070: the first IB word as the kernel's cacheable alias and DRAM see it before any maintenance, then DRAM after the CPU
     * caches over every mapped BO are cleaned and invalidated. The board shows the last submission's record on a stall. */
    uint32_t cached=~0u,before=~0u,ignored=0,after=~0u;
    /* Write-combined stores this CPU made before the call (the client's command words) complete before anything below. */
    __asm__ volatile("dsb sy":::"memory");
    /* int-098/099: with write-combined user BOs and no maintenance the CP stopped at the end of the first IB, as in int-065..069 when an
     * IB sat in a CPU cache. While write-combined BOs are trusted, every line of this submission's IBs is compared as a CPU cache and
     * DRAM hold it (bounded). A dirty line means a client view still caches its BOs: maintain every BO from now on, this submission
     * included (the audit already wrote the IB lines back; the rest of the BOs may hold more). */
    if(vm_->writeCombinedTrusted()) {
        /* int-100: the dirty IB page was back at XNU's default (cacheable) attribute. Write-combine every such page again first; the
         * pmap writes its lines back on the way, so the audit below should then find none. */
        uint32_t reset=0;
        IOReturn audited=vm_->reassertWriteCombined(&reset);
        native_adreno_wc_audit[5]=native_adreno_wc_audit[5]+reset;
        uint32_t lines=0,dirty=0,stale=0;size_t budget=SubmitAuditBytes;
        /* The re-assertion above is the correction and runs every time; the audit only detects. It found no dirty line in int-141's
         * 133798 nor in i566's 2765132 (1358 audits of every eighth submission, about 2 ms each under the publish lock, so every eighth
         * frame of every client waited), so only the first 16 submissions are compared now. */
        ++audit_tick_;
        const bool audit_now=audit_tick_<=16;
        if(!audit_now)budget=0;
        for(size_t i=0;i<count && budget && !audited;++i) {   /* a failed re-assertion skips the audit and ends the trust */
            const size_t bytes=buffers[i].bytes<budget?(size_t)buffers[i].bytes:budget;
            audited=vm_->auditRange(buffers[i].iova,bytes,&lines,&dirty,&stale);budget-=bytes;
        }
        if(audit_now && audited!=kIOReturnUnsupported) {   /* no board hook (host): nothing to compare */
            native_adreno_wc_audit[0]=native_adreno_wc_audit[0]+1;
            native_adreno_wc_audit[1]=native_adreno_wc_audit[1]+lines;
            native_adreno_wc_audit[2]=native_adreno_wc_audit[2]+dirty;
            native_adreno_wc_audit[3]=native_adreno_wc_audit[3]+stale;
            if(audited)native_adreno_wc_audit[4]=audited;
        }
        if(dirty || (audited && audited!=kIOReturnUnsupported)) {
            vm_->distrustWriteCombined();
            /* The header documents this as the 1-based submission that ended the trust, and it was being set from
             * the coherence sample counter, which counts something else. native_adreno_submit_ibs[0] is the
             * submission counter (incremented at the top of submit()), so that is what it reports now. */
            native_adreno_wc_distrusted=native_adreno_submit_ibs[0];
        }
    }
    /* The coherence sample reads the first IB's word through a fresh uncached mapping before and after the publication: every 64th
     * submission only (two such mappings per submission were CPU time on every frame). */
    /* The board's CP stops on CP_WAIT_FOR_IDLE (see NativeAdrenoVm::dropWaitForIdle): take it out of the
     * IB the CP is about to read. Counted so the log can say how often it happens. */
    /* Off by default: the scrub writes a client BO's pages through a fresh write-combined mapping before
     * every submission, and from fix41 on the self test stopped completing at its first submission
     * (fix40 read rounds 4 4, fix41 read -1 -1). One round with it off says whether that write is the cause.
     * pipa-gpu-wfi=1 turns it back on. */
    unsigned scrub = 0;
    PE_parse_boot_argn("pipa-gpu-wfi", &scrub, sizeof scrub);
    for(size_t i=0;i<count && scrub;++i) {
        uint32_t patched=0;
        native_adreno_wfi_try[0]=native_adreno_wfi_try[0]+1;
        const IOReturn dropped=vm->dropWaitForIdle(buffers[i].iova,buffers[i].bytes,&patched);   /* the job s VM, not the kernel s */
        native_adreno_wfi_try[3]=native_adreno_wfi_try[3]+1;
        if(dropped==kIOReturnSuccess) native_adreno_wfi_dropped=native_adreno_wfi_dropped+patched;
        else if(dropped==kIOReturnBadArgument) native_adreno_wfi_try[1]=native_adreno_wfi_try[1]+1;   /* no mapping for the iova */
        else {native_adreno_wfi_try[2]=native_adreno_wfi_try[2]+1;native_adreno_wfi_try[4]=uint32_t(dropped);}   /* no spine: uncached view or leaf */
    }
    /* The all-ones CP_WAIT_REG_MEM, on by default with pipa-gpu-keep-wait=1 as the way back. Nothing in this tree
     * or upstream emits the timestamp it waits for, so the CP never leaves it: the control boot read 56,728 fence
     * timeouts against 4 when the word was written instead (evidence/edl-log-s110 against s109). Removing the packet
     * unblocks the queue without touching the client's data, which the write did; the cost is that the query the
     * client reads back accumulates a stale stop value, recorded as a limit in porting.toml. */
    unsigned keepWait = 0;
    PE_parse_boot_argn("pipa-gpu-keep-wait", &keepWait, sizeof keepWait);
    for(size_t i=0;i<count && !keepWait;++i) {
        uint32_t patched=0;
        native_adreno_wait_try[0]=native_adreno_wait_try[0]+1;
        native_adreno_wait_try[3]=native_adreno_wait_try[3]+(uint32_t)buffers[i].bytes;
        const IOReturn dropped=vm->dropAllOnesWaits(buffers[i].iova,buffers[i].bytes,&patched);
        if(dropped==kIOReturnSuccess) native_adreno_wait_dropped=native_adreno_wait_dropped+patched;
        else if(dropped==kIOReturnBadArgument) native_adreno_wait_try[2]=native_adreno_wait_try[2]+1;   /* no mapping for the iova, or it does not cover the length */
        else {native_adreno_wait_try[1]=native_adreno_wait_try[1]+1;native_adreno_wait_last=(uint32_t)dropped;}   /* a span or page the walk could not reach */
        /* The CP also executes what this buffer chains to with CP_INDIRECT_BUFFER, and that is where this board's wait actually
         * lives: the stalling submission's four IBs hold none of it, their first hop holds one (evidence/edl-log-s115, [gpu
         * ibchain]). Scanning only the submitted list - which is what this loop did until now - can therefore never remove it.
         * One level and at most four targets per buffer, because that is what the evidence shows and a deeper walk would need a
         * cycle guard; rewriteStream() re-checks each target against the VM's mappings, so a stale or foreign address costs a
         * refusal, not a write. */
        {
            uint64_t targets[4]={0,0,0,0};
            uint32_t dwords[4]={0,0,0,0};
            if(vm->indirectTargets(buffers[i].iova,buffers[i].bytes,targets,dwords,4)==kIOReturnSuccess)
                for(size_t t=0;t<4 && targets[t];++t) {
                    uint32_t hopPatched=0;
                    const uint32_t words=dwords[t]>4096u?4096u:dwords[t];   /* a declared length is a hint, not a bound */
                    if(!words)continue;
                    native_adreno_wait_try[0]=native_adreno_wait_try[0]+1;
                    const IOReturn hop=vm->dropAllOnesWaits(targets[t],(size_t)words*4,&hopPatched);
                    if(hop==kIOReturnSuccess) {
                        native_adreno_wait_dropped=native_adreno_wait_dropped+hopPatched;
                        native_adreno_wait_hops=native_adreno_wait_hops+hopPatched;
                    }
                    else if(hop==kIOReturnBadArgument) native_adreno_wait_try[2]=native_adreno_wait_try[2]+1;
                    else {native_adreno_wait_try[1]=native_adreno_wait_try[1]+1;native_adreno_wait_last=(uint32_t)hop;}
                }
        }
    }
    phase[3]=mach_absolute_time();
    static uint32_t coherence_turn;
    const bool sample=!(coherence_turn++&63);
    if(sample)(void)vm_->sampleWord(buffers[0].iova,&cached,&before);
    IOReturn status=vm_->publishMapped();
    if(!status && sample) {
        (void)vm_->sampleWord(buffers[0].iova,&ignored,&after);
        native_adreno_coherence[0]=native_adreno_coherence[0]+1;
        native_adreno_coherence[1]=cached;native_adreno_coherence[2]=before;native_adreno_coherence[3]=after;
        if(cached!=before || before!=after) {
            if(!native_adreno_coherence[4]) {
                native_adreno_coherence[5]=cached;native_adreno_coherence[6]=before;native_adreno_coherence[7]=after;
            }
            native_adreno_coherence[4]=native_adreno_coherence[4]+1;
        }
    }
    phase[4]=mach_absolute_time();
    /* No TLB invalidation before a submission: a map fills only invalid leaves (no TLB holds an invalid descriptor), an unmap drops
     * its own pages before it returns, and the tables above the leaves never change. The TLBIALL here emptied the GPU's TLB on every
     * submission (10944 in i568), so each frame began with page walks. A stalled fence still invalidates (below). */
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    {
        /* Zeroed, because a failed peekWords leaves these uninitialised and the dump below would then print stack garbage as if
         * it were the buffer's content. */
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[96]={},view[96]={};
        const IOReturn hookStatus=vm_->peekWords(buffers[0].iova,head,96);
        const IOReturn viewStatus=vm_->peekWordsUncached(buffers[0].iova,view,96);
        native_adreno_ib_log[slot][0]=ring_->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        {   /* The mapping the first buffer's handle resolves to: its base, so [1..2] - [4..5] is the buffer's offset inside it. */
            uint64_t base=0;size_t mapped=0;unsigned permissions=0;
            (void)vm_->describe(buffers[0].handle,&base,&mapped,&permissions);
            native_adreno_ib_log[slot][4]=(uint32_t)base;native_adreno_ib_log[slot][5]=(uint32_t)(base>>32);
        }
        for(unsigned i=0;i<96;++i)native_adreno_ib_log[slot][6+i]=head[i];
        for(unsigned i=0;i<96;++i)native_adreno_ib_log[slot][102+i]=view[i];
        /* The tail: the head is 96 of up to 184 dwords, and the packet that stops the CP is past it.
         * The last 16 dwords of the buffer, read the same way, say whether the fence write is even
         * present at the end of the IB. */
        {uint32_t tail[16]={};const uint32_t bytes=buffers[0].bytes;
         if(bytes>64) (void)vm_->peekWordsUncached(buffers[0].iova+bytes-64,tail,16);
         for(unsigned i=0;i<16;++i)native_adreno_ib_log[slot][200+i]=tail[i];}
        /* The middle: the head is 96 of 184 dwords, so words 96..183 were never seen, and a BO that
         * reads as real packets in its first 96 and zeros after 96 says the client's own write stopped
         * there rather than that the CP failed. */
        {uint32_t mid[80]={};const uint32_t bytes=buffers[0].bytes;
         if(bytes>384)(void)vm_->peekWordsUncached(buffers[0].iova+384,mid,80);
         for(unsigned i=0;i<80;++i)native_adreno_ib_log[slot][216+i]=mid[i];}
        native_adreno_ib_log[slot][198]=uint32_t(hookStatus)|(uint32_t(viewStatus)<<16);
        {unsigned differ=0;for(unsigned i=0;i<96;++i)if(head[i]!=view[i])++differ;native_adreno_ib_log[slot][199]=differ;}
        /* The userspace self-test writes its markers with three CP_MEM_WRITE packets of its own (NativeAdrenoUserTest.h:148-155:
         * CP_MEM_WRITE 0x3d to tgt+0/+4/+8 with 0adc0de1, feed0002, 600d0003, over a 0xdeadbeef-filled page), then reads them
         * back. The CP has not run yet at this point, so only the address is captured; wait() reads the page once the fence
         * passes. The header is AdrenoPM4Type7(0x3d, 3): count 3 has even population so its odd bit is set (0x8000) and opcode
         * 0x3d has odd population so its odd bit is clear, which makes the word 0x703d8003.
         * All three packets are required, at a fixed stride and with addresses stepping by 4, because a flat search for one
         * header would also match a data or address word inside a Metal/Turnip IB - and since only the first match is taken, one
         * such match would spend the whole boot on a page that is not the self-test's.
         * The three DATA words are required as well, because this board's own DbyGpuPowerProbe submits the same three-packet
         * shape first: mode 26 runs dbyClientBoRound (package/DbyGpuPowerProbe.cpp:4834, :5674-5680) with 0badc0de, feed0001,
         * 600d0003, and in evidence/edl-log-s119 it is submission fence 2 (iova 0x1_00004000) while the userspace runTest is
         * fence 20 (iova 0x1_00000000). Matching the header shape alone would therefore capture the probe's page - which its own
         * read already reports as written - and never read the client page this exists to answer for. 0adc0de1 and feed0002 are
         * the two words the probe does not use. vm_ here is the shadowed job VM (NativeAdrenoSubmit.cpp:295), so the capture is
         * recorded against the table the submission actually uses. */
        if(!native_adreno_tgt[0] && !tgt_pending) {
            for(unsigned i=0;i+11<96;++i) {
                if(head[i]!=0x703d8003u || head[i+4]!=0x703d8003u || head[i+8]!=0x703d8003u)continue;
                if(head[i+3]!=0x0adc0de1u || head[i+7]!=0xfeed0002u || head[i+11]!=0x600d0003u)continue;
                const uint64_t first=(uint64_t)head[i+2]<<32|head[i+1];
                const uint64_t second=(uint64_t)head[i+6]<<32|head[i+5];
                const uint64_t third=(uint64_t)head[i+10]<<32|head[i+9];
                if(!first || second!=first+4 || third!=first+8)continue;
                tgt_pending=first;tgt_pending_vm=vm_;
                break;
            }
        }
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    phase[5]=mach_absolute_time();
    status=useVm(vm,clock);
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    status=ring_->submitFramed(words,word_count,&value);
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(ring_->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;job->vm_next=vm_->nextHandle();job->vm=vm;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        /* No room in the ring yet: the caller waits for an earlier job and submits again. */
        return status==kIOReturnNoResources?kIOReturnBusy:status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;job->vm_next=vm_->nextHandle();job->vm=vm;
    job->context=ring_->context();job->word_count=word_count;
    job->submit_us=clock && clock->now_us?clock->now_us(clock->cookie):0;
    if(clock && clock->now_us) {clock_now_=clock->now_us;clock_cookie_=clock->cookie;}
    job->level=&native_gpu_dcvs?native_gpu_dcvs[1]:0;
    job->owner=next_owner_;next_owner_=0;
    for(size_t i=0;i<word_count;++i)job->words[i]=words[i];
    ++stats_.submissions;
    phase[6]=mach_absolute_time();
    for(unsigned i=0;i<6;++i)native_adreno_submit_phase[i]=native_adreno_submit_phase[i]+(phase[i+1]-phase[i]);
    native_adreno_submit_phase[6]=native_adreno_submit_phase[6]+1;
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=ring_->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    return kIOReturnSuccess;
}
/* A stage runs once per stalled fence: when the stage before it has run and the fence has been pending long enough. */
static bool stage_due(uint32_t stage,uint32_t before,uint64_t pending,uint64_t after_us)
{
    return stage==before && pending>=after_us;
}
/* The CP snapshot is taken at 50 ms and at 1 s of one stall. */
static uint32_t stall_snapshots;
/* The word the held packet polls, as seen by the previous snapshot, so the row can say whether anything is ever
 * going to write it. Only a diagnostic: noteStall is not serialised, so two clients stalling at once share these,
 * which is why the fence and both halves of the address are stored with the value and any mismatch disables the
 * comparison. Whether there is a comparable earlier reading is decided by that guard alone - never by the value,
 * because 0xffffffff is a real value and is the one this stall happens to poll. */
static uint32_t gDbyStallPriorWord,gDbyStallPriorAddrHi,gDbyStallPriorAddrLo,gDbyStallPriorValid,gDbyStallSnapshots,gDbyStallFence;
/* The fence whose polled word the opt-in pipa-gpu-force-poll experiment has already written. */
static uint32_t gDbyPollWritten=0xffffffffu;
static bool snapshot_due(uint32_t *taken,uint64_t pending)
{
    const uint32_t want=pending>=1000000?2:pending>=50000?1:0;
    if(want<=*taken)return false;
    *taken=want;return true;
}
void NativeAdrenoSubmit::noteStall(uint32_t observed,const NativeAdrenoRingClock *clock)
{
    const uint32_t next=observed+1;   /* the fence the CP is working on */
    const uint64_t now=clock && clock->now_us?clock->now_us(clock->cookie):0;
    auto log=[&](uint32_t fence,uint32_t event,uint64_t pending,uint32_t before_r,uint32_t before_w) {
        const uint32_t n=native_adreno_stall_log_count;volatile uint64_t *slot=native_adreno_stall_log[n&31];
        const Job *owner=find(fence);
        slot[0]=now;slot[1]=fence;slot[2]=(uint64_t)event|(uint64_t)stall_timeouts<<8;slot[3]=pending;
        slot[4]=(uint64_t)before_r|(uint64_t)before_w<<16|(uint64_t)ring_->readPointer()<<32|(uint64_t)(ring_->writePointer()&0xffff)<<48;
        slot[5]=owner?owner->queue:0;slot[6]=owner?(owner->handles[0]&0xffffffffffffull)|(uint64_t)owner->count<<48:0;slot[7]=n;
        native_adreno_stall_log_count=n+1;
    };
    if(next!=stall_fence) {
        if(stall_timeouts && now) {
            const uint64_t ms=(stall_last_us-stall_began_us)/1000;
            uint32_t bucket=0;while(bucket<7 && ms>=(1ull<<bucket))++bucket;
            native_adreno_fence_pending[bucket]=native_adreno_fence_pending[bucket]+1;
            if(stall_last_us-stall_began_us>=1000)log(stall_fence,0,stall_last_us-stall_began_us,0,0);   /* shorter ends are ordinary jobs */
        }
        stall_began_us=now;stall_snapshots=0;
        /* The CP moved on: what the stall that just ended took to clear. [9] first-stage recoveries, [11] bus-drain recoveries (stage 2 does
         * not exist, so [10] is never written), [12] the most timeouts one stall accumulated. */
        if(stall_stage && stall_timeouts) {
            native_adreno_unstick[8+stall_stage]=native_adreno_unstick[8+stall_stage]+1;
            if(stall_timeouts>native_adreno_unstick[12])native_adreno_unstick[12]=stall_timeouts;
        }
        stall_fence=next;stall_timeouts=0;stall_stage=0;
    }
    stall_last_us=now;
    const uint64_t pending=now-stall_began_us;
    /* A fence that stops while the CP sits idle with work in the ring (int-317 hung with RBBM idle, RPTR short of WPTR, both threads at the same
     * cache clean) is nearly always a write pointer the CP never took: giving it again after a few waits ends it in a couple of milliseconds
     * (i343: 206 of 213 stalls ended at the second stage, 60 to 85 ms each, most of it the 100 waits before the write). After 10 waits now:
     * i469 stalled 143 times in about 200 s, each a visible hitch. The same value written again changes nothing when the CP is busy. */
    /* After 10 waits the translations are dropped and the write pointer is given again. i494/i495 measured what ends these: a job lasts about
     * 5 ms against 1 ms waits, so most "stalls" are ordinary jobs still running; the write pointer is never dropped (fenced writes saw no
     * drop) and invalidating at map time changed nothing. i553: the handler ran on 6712 of 6911 jobs. It now waits for 50 ms without
     * progress, measured in time (a timed-out wait is one poll, 100 us or less), and so does the snapshot below. */
    if(now && stage_due(stall_stage,0,pending,50000)) {
        native_adreno_unstick[0]=native_adreno_unstick[0]+1;native_adreno_unstick[1]=stall_fence;stall_stage=1;
        const uint32_t r0=ring_->readPointer(),w0=ring_->writePointer()&0xffff;
        (void)vm_->invalidateTlb();(void)ring_->kick();
        log(stall_fence,1,pending,r0&0xffff,w0);
    }
    /* Still stopped well after that: the way a6xx drains the bus before a reset, which lets a write that was never acknowledged end. */
    if(now && stage_due(stall_stage,1,pending,200000)) {
        uint32_t ack[2]={};
        native_adreno_unstick[4]=native_adreno_unstick[4]+1;native_adreno_unstick[7]=stall_fence;stall_stage=3;
        const uint32_t r0=ring_->readPointer(),w0=ring_->writePointer()&0xffff;
        (void)ring_->drainBus(ack);
        native_adreno_unstick[5]=ack[0];native_adreno_unstick[6]=ack[1];
        log(stall_fence,3,pending,r0&0xffff,w0);
    }
    ++stall_timeouts;
    if(!now || !snapshot_due(&stall_snapshots,pending))return;
    uint32_t words[NativeAdrenoRingStallWords]={},fault[4]={};
    if(ring_->stallState(words))return;
    log(next,4,pending,words[8]&0xffff,words[9]&0xffff);
    (void)vm_->faultState(fault);
    for(uint32_t i=0;i<4;++i)native_adreno_stall_mem[i]=fault[i];
    uint64_t ib=0;uint32_t size=0;
    /* The IBs are the stalled job's: read them through its address space (the kernel's maps none of a client's BOs). */
    const Job *job=find(next);
    /* Not const: the opt-in pipa-gpu-force-poll experiment writes one word through this address space
     * (NativeAdrenoVm::writeWord). Everything else here calls const members. */
    NativeAdrenoVm *space=job && job->vm?job->vm:vm_;
    const uint64_t brIbBase=(uint64_t)words[11]<<32|words[10];
    if(!ring_->ibBefore(words[8],&ib,&size))peekIb(space,ib,size,words[12],&native_adreno_stall_mem[4]);
    else {
        const uint32_t known=ibDwordsAt(next,brIbBase);
        if(known)peekIb(space,brIbBase,known,words[12],&native_adreno_stall_mem[4]);
        else peekStart(space,brIbBase,&native_adreno_stall_mem[4]);
    }
    if(!ring_->ibBefore(words[16],&ib,&size))peekIb(space,ib,size,words[18],&native_adreno_stall_mem[4+17]);
    else peekFetch(space,(uint64_t)words[11]<<32|words[17],&native_adreno_stall_mem[4+17]);   /* BV IB1 base: the low word only */
    /* The packet the SQE holds, found in the stalled buffer and kept whole. words[25] is CP_SQE_STAT's data
     * for selector 1 (see NativeAdrenoRing.cpp) - the header the CP is executing; words[24] is 1 only when that
     * read succeeded, because the driver writes 0xffffffff into both when it did not, and searching for that
     * would "find" a value that is not a header at all. A generation-6 core has no BV thread, so the third slot
     * always read deadbeef there and carries this instead.
     * A PM4 header is type/opcode/count only, so every count-6 CP_WAIT_REG_MEM has the same word and the buffer
     * may hold several: inside the BR IB1 the one that matters is the last at or before the dword the CP has
     * fetched to (cap - CP_IB1_REM_SIZE), because an earlier copy is one it already passed.
     * If it is not there at all, the CP is executing inside a buffer IB1 chains to: a frozen CP_IB1_REM_SIZE
     * with no matching header in the fetched part is exactly that shape, and it is what the 20:13 boot showed.
     * So the fetched part is scanned for CP_INDIRECT_BUFFER packets too, and up to three of their targets are
     * searched, newest first. Still not there, it is searched in IB2 (CP_IB2_BASE), whose length is asked of the
     * ring record and otherwise taken as the first 256 dwords.
     * slot[0] packs the answer: top nibble 0 = found in the BR IB1 itself, 1 = the newest indirect buffer, i.e. the
     * last CP_INDIRECT_BUFFER at or before the cursor, 2..3 = the ones before it, 4 = IB2, low bits = the dword the
     * packet starts at; 0xffffffff = not found anywhere. slot[1..16] are that packet's words, deadbeef-padded so
     * the row never prints stale slots. */
    {
        volatile uint32_t *slot=&native_adreno_stall_mem[4+34];
        uint32_t scan[256],at=0xffffffffu,hop=0,hdr=0,buflen=0;
        const uint32_t header=words[25],cap=ibDwordsAt(next,brIbBase);
        const uint32_t reached=words[12]<=cap?cap-words[12]:cap;
        for(uint32_t k=1;k<17;++k)slot[k]=0xdeadbeefu;
        slot[0]=0xffffffffu;
        /* header must be a real PM4 header: 0 is never one (the type nibble is 4 or 7), and a short mapping leaves
         * zeros in scan's tail, so searching for 0 would match padding. */
        if(words[24]==1u && header && header!=0xffffffffu) {
            if(cap>1 && cap<=256 && !space->peekWordsUncached(brIbBase,scan,cap)) {
                buflen=cap;
                for(uint32_t i=0;i<cap && i<=reached;++i)
                    if(scan[i]==header)at=i;
                /* cap>3: the chained-target walk reads scan[i..i+3], and peekWordsUncached filled only cap words, so a
                 * buffer of two or three dwords would decode uninitialised stack. */
                if(at==0xffffffffu && cap>3 && AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&hdr)==AdrenoPM4Success) {
                    uint64_t tgt[3];uint32_t len[3],n=0;
                    /* Newest first, for the same reason the direct match takes the last one: the CP is in the most
                     * recent buffer it was handed, and an older sibling may hold an identical packet. */
                    uint32_t hi=cap>=4?cap-4:0;
                    if(hi>reached)hi=reached;
                    for(uint32_t i=hi+1;i-- >0 && n<3;)
                        if(scan[i]==hdr) {
                            tgt[n]=(uint64_t)scan[i+2]<<32|scan[i+1];
                            len[n]=scan[i+3]&0xfffffu;
                            n=n+1;
                        }
                    for(uint32_t c=0;c<n && at==0xffffffffu;++c)
                        if(len[c]>1 && len[c]<=256 && !space->peekWordsUncached(tgt[c],scan,len[c])) {
                            buflen=len[c];
                            for(uint32_t j=0;j<len[c];++j)
                                if(scan[j]==header) {at=j;hop=c+1;break;}
                        }
                }
            }
            /* Then IB2, which shares nothing with the block above: CP_IB2_BASE needs no ring record, and a frozen
             * CP_IB1_REM_SIZE beside CP_IB2_REM_SIZE == 0 says the CP moved on to IB2 and fetched it to its end
             * (the 21:44 boot). IB2's length is not a register, so the ring record is asked first and the first 256
             * dwords are read when it cannot answer; hop 4 names IB2. This search takes the FIRST match, because
             * there is no IB2 cursor to bound it. */
            if(at==0xffffffffu) {
                const uint64_t ib2=(uint64_t)words[14]<<32|words[13];
                const uint32_t known2=ib2?ibDwordsAt(next,ib2):0;
                const uint32_t want2=known2>1&&known2<=256?known2:256;
                if(ib2 && ib2!=brIbBase && !space->peekWordsUncached(ib2,scan,want2)) {
                    buflen=want2;
                    for(uint32_t j=0;j<want2;++j)
                        if(scan[j]==header) {at=j;hop=4;break;}
                }
            }
        }
        /* The polled word's window is cleared before anything else, so a snapshot that does not find the packet
         * cannot print the previous snapshot's word as if it were current. */
        volatile uint32_t *poll=&native_adreno_stall_mem[4+51];
        poll[0]=0xffffffffu;
        for(uint32_t k=1;k<16;++k)poll[k]=0xdeadbeefu;
        if(at!=0xffffffffu) {
            slot[0]=(hop<<28)|at;
            for(uint32_t k=0;k<16 && at+k<buflen;++k)slot[1+k]=scan[at+k];
            /* The held packet is usually a CP_WAIT_REG_MEM, and what decides the whole question is the word it
             * polls: a full-mask mismatch says the value simply never appears, and reading it says whether the
             * comparison could ever hold. slot[1] is the header itself, so payload dword n is slot[2+n]: 0 flags,
             * 1 address lo, 2 address hi. POLL[5:4] in the flags says whether the address is memory (1), a register
             * (0) or scratch (2), and only memory has an IOVA this can read. That word and its neighbours are
             * kept, with the IOVA as the window's marker (0xffffffff when the packet is not such a wait or the read
             * refused); the words after them are what the previous snapshot read there and whether it moved. */
            const uint32_t flags=slot[2],addrlo=slot[3],addrhi=slot[4];
            if(((flags>>4)&3u)==1u) {
                const uint64_t addr=(uint64_t)addrhi<<32|addrlo;
                /* The window reads from 0x10 BELOW the polled word, because the packet after the wait is
                 * CP_MEM_TO_MEM (type 7 opcode 0x73, DOUBLE|NEG_C) and computes
                 *     *(u64)(addr-8) += *(u64)addr - *(u64)(addr-0x10)
                 * (RESEARCH-ALLONES-WAIT.md). Six words are read so that the whole query is visible: [1..2] the
                 * start timestamp, [3..4] the accumulator, [5..6] the stop timestamp the CP is waiting for. Four
                 * were read before, which stopped two dwords short of addr - the one value this window exists for,
                 * and the reason s113 printed `poll @8108010: 0 0 0 0` while the CP waited on 0x8108020. */
                const uint64_t base=addr>=0x10u?addr-0x10u:addr;
                uint32_t peeked[6]={0,0,0,0,0,0};
                if(base && !space->peekWordsUncached(base,peeked,6)) {
                    poll[0]=(uint32_t)base;
                    for(uint32_t k=0;k<6;++k)poll[1+k]=peeked[k];
                    /* [7] the polled word at the previous snapshot of this stall, [8] 1 if it moved, 0 if it did
                     * not, 0xffffffff if there is nothing to compare, [9] how many snapshots this fence has taken.
                     * Whether a comparison is possible comes from the fence and address guard, never from the value:
                     * 0xffffffff is a real reading and is exactly what this stall was found polling, so deciding on
                     * it would throw the answer away in the one case the window exists for. Snapshots are time-gated
                     * (50 ms, then 1 s, NativeAdrenoSubmit.cpp:470), so "unchanged" covers that whole interval: it
                     * means no producer is coming, and a change means the producer runs and the comparison or the
                     * value is what is wrong. */
                    if(gDbyStallFence!=next) {
                        gDbyStallFence=next;
                        gDbyStallPriorValid=0u;
                        gDbyStallSnapshots=0u;
                    }
                    const uint32_t addrHi=(uint32_t)(addr>>32),addrLo=(uint32_t)addr;
                    const bool has=gDbyStallPriorValid && gDbyStallPriorAddrHi==addrHi && gDbyStallPriorAddrLo==addrLo;
                    poll[7]=has?gDbyStallPriorWord:0xffffffffu;
                    poll[8]=has?(gDbyStallPriorWord==peeked[4]?0u:1u):0xffffffffu;
                    poll[9]=gDbyStallSnapshots;
                    gDbyStallPriorWord=peeked[4];
                    gDbyStallPriorAddrHi=addrHi;
                    gDbyStallPriorAddrLo=addrLo;
                    gDbyStallPriorValid=1u;
                    gDbyStallSnapshots=gDbyStallSnapshots+1u;
                    /* Who names the polled address? The buffer the packet was found in is still in scan[0..buflen),
                     * so count the dwords equal to it, remember the first and last, and keep the wait's own index:
                     * a producer in that buffer shows up as a hit before the wait, a producer that would run after
                     * it as a hit behind, and "hits == 1" means only the wait itself names the address - nothing in
                     * this buffer will ever write it. [11] hits, [12] first, [13] last, [14] the wait's dword,
                     * [15] which buffer (hop). */
                    {
                        uint32_t hits=0,first=0xffffffffu,last=0xffffffffu;
                        for(uint32_t k=0;k<buflen;++k)
                            if(scan[k]==(uint32_t)addr) {
                                if(first==0xffffffffu)first=k;
                                last=k;
                                hits=hits+1u;
                            }
                        poll[11]=hits;
                        poll[12]=first;
                        poll[13]=last;
                        poll[14]=at;
                        poll[15]=hop;
                    }
                    /* Opt-in experiment, pipa-gpu-force-poll=<value>: write the polled word from the CPU once per
                     * stalled fence and see what the CP does. It separates the last two possibilities - the CP reads
                     * this page and only its producer is missing (the fence retires and no further rows appear), or
                     * the CP polls a different physical page because its translation is stale (the row repeats with
                     * the word already changed). Off unless the argument is passed, because it writes into a live
                     * client buffer; the window is cleared to deadbeef, so [10] is deadbeef until the write is made. */
                    if(poll[10]==0xdeadbeefu && gDbyPollWritten!=next) {
                        uint32_t forced=0;
                        if(PE_parse_boot_argn("pipa-gpu-force-poll",&forced,sizeof forced) && forced!=0xffffffffu) {
                            if(space->writeWord(addr,forced)==kIOReturnSuccess) {
                                gDbyPollWritten=next;
                                poll[10]=1u;
                            }
                        }
                    }
                }
            }
        }
    }
    /* Which buffers the stalled job actually handed the CP, and what those chain to. The all-ones wait drop walks only the submission's own
     * list (NativeAdrenoVm::dropAllOnesWaits -> rewriteStream, one mapping lookup per iova), so a wait that lives in a buffer reached through
     * CP_INDIRECT_BUFFER is never scanned and the CP spins on it for the rest of the boot. Each entry is (iova low, dwords scanned, waits
     * found), so a zero in every "found" says the packet is outside the submitted set and the drop cannot be the fix.
     * Read the same way as the held packet above: through the job's address space, bounded to 256 dwords per buffer, one hop only. */
    {
        volatile uint32_t *chain=&native_adreno_ib_chain[2];
        uint32_t buf[256],sub=0,hops=0,indirect=0;
        uint64_t target[4]={0,0,0,0};
        uint32_t targetWords[4]={0,0,0,0};
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&indirect)!=AdrenoPM4Success)indirect=0;
        for(size_t i=0;job && i<job->count && sub<4;++i) {
            uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
            if(space->describe(job->handles[i],&iova,&bytes,&permissions))continue;
            uint32_t dwords=(uint32_t)(bytes/4);if(dwords>256)dwords=256;
            if(!dwords || space->peekWordsUncached(iova,buf,dwords))continue;
            uint32_t found=0;
            for(uint32_t k=0;k<dwords;++k) {
                if(buf[k]==0x70bc8006u)++found;
                if(indirect && hops<4 && k+3<dwords && buf[k]==indirect) {
                    target[hops]=(uint64_t)buf[k+2]<<32|buf[k+1];
                    targetWords[hops]=buf[k+3]&0xfffffu;
                    ++hops;
                }
            }
            chain[sub*3+0]=(uint32_t)iova;chain[sub*3+1]=dwords;chain[sub*3+2]=found;++sub;
        }
        uint32_t used=0;
        for(uint32_t t=0;t<hops && used<4;++t) {
            uint32_t dwords=targetWords[t];if(dwords>256)dwords=256;
            if(!dwords || space->peekWordsUncached(target[t],buf,dwords))continue;
            uint32_t found=0;
            for(uint32_t k=0;k<dwords;++k)if(buf[k]==0x70bc8006u)++found;
            chain[12+used*3+0]=(uint32_t)target[t];chain[12+used*3+1]=dwords;chain[12+used*3+2]=found;++used;
        }
        native_adreno_ib_chain[0]=sub;
        native_adreno_ib_chain[1]=used;
    }
    {
        uint64_t brIb=(uint64_t)words[11]<<32|words[10],bvIb=(uint64_t)words[11]<<32|words[17];
        if(!ring_->ibBefore(words[8],&ib,&size))brIb=ib;
        if(!ring_->ibBefore(words[16],&ib,&size))bvIb=ib;
        native_adreno_stall_mem[72]=job?(uint32_t)job->vm_next:0;
        native_adreno_stall_mem[73]=(uint32_t)space->handleAt(brIb);
        native_adreno_stall_mem[74]=(uint32_t)space->handleAt(bvIb);
        native_adreno_stall_mem[75]=(uint32_t)space->handleAt((uint64_t)words[21]<<32|words[20]);
    }
    for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)native_adreno_stall[2+i]=words[i];
    native_adreno_stall[1]=stall_timeouts;
    native_adreno_stall[0]=next;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;native_adreno_unstick[13]=timeout_us;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=ring_->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
            noteStall(observed,clock);
            return kIOReturnTimeout;
        }
        return kIOReturnSuccess;
    }
    const NativeAdrenoRingFence result=ring_->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
        noteStall(observed,clock);
        return kIOReturnTimeout;
    }
    /* The fence passed, so the CP has executed what this submission chained to. Read the self-test's target page now: submit() only
     * captured its address, because the page still held the client's 0xdeadbeef sentinels when the packet went in. Through the job's
     * own VM - the member vm_ is the kernel's default table and does not map a client address (NativeAdrenoSubmit.h:72) - and only
     * in the VM the capture came from, so a pending target left behind by a timed-out fence cannot be read out of a later client's
     * page table, whose window starts at the same 0x1_00000000. One attempt: on failure [0] is set to 2 and the row says the page
     * could not be read, instead of a retry that would keep a stale address alive and charge a cache maintenance pass per wait. */
    if(tgt_pending && !native_adreno_tgt[0]) {
        NativeAdrenoVm *space=job->vm?job->vm:vm_;
        if(space==tgt_pending_vm) {
            uint32_t words4[4]={0,0,0,0};
            const IOReturn read=space->peekWordsUncached(tgt_pending,words4,4);
            native_adreno_tgt[1]=(uint32_t)tgt_pending;native_adreno_tgt[2]=(uint32_t)(tgt_pending>>32);
            native_adreno_tgt[7]=fence;   /* which run the row describes, so it cannot be misattributed */
            if(read) {
                __asm__ volatile("dmb sy" ::: "memory");
                native_adreno_tgt[0]=2u;   /* captured, but the page is not readable: no row may claim a store did not land */
            }
            else {
                for(unsigned k=0;k<4;++k)native_adreno_tgt[3+k]=words4[k];
                __asm__ volatile("dmb sy" ::: "memory");   /* the four dwords before the status word that publishes them */
                native_adreno_tgt[0]=1u;
            }
            tgt_pending=0;tgt_pending_vm=nullptr;
        }
    }
    IOReturn status=release(*job);
    if(status)return status;
    ++stats_.completed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint64_t first,uint64_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(auto &job:jobs_) {
        if(!job.active)continue;
        const NativeAdrenoRingFence state=ring_->observeFence(job.fence,nullptr,0,nullptr);
        if(state==NativeAdrenoRingFencePoisoned)return poison();
        if(state!=NativeAdrenoRingFenceComplete)continue;
        const IOReturn status=release(job);
        if(status)return status;
        ++stats_.completed;
    }
    return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::madeProgress()
{
    /* hangCheck() reads this as "the CP is still moving, do not declare a hang"; the counter therefore
     * has to bound the checks that saw NOTHING move, not the ones that did. msm does the same: it resets
     * its retries when the state changes. Counting progress here turned the third good observation into
     * a hang report. */
    uint32_t state[6]={};
    if(ring_->progressState(state))return false;
    bool progress=false;
    for(unsigned i=0;i<6;++i)if(state[i]!=last_cp_[i])progress=true;
    for(unsigned i=0;i<6;++i)last_cp_[i]=state[i];
    if(progress) {progress_retries_=0;return true;}
    if(progress_retries_>=3)return false;
    ++progress_retries_;
    return true;
}
IOReturn NativeAdrenoSubmit::hangCheck(bool *hung)
{
    if(!hung)return kIOReturnBadArgument;*hung=false;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    /* Nothing outstanding means nothing can hang: after a restart the ring's seqno equals the fence it
     * completed, and there is no job to be stuck on. Without this the check compares the same two numbers
     * every 500 ms and reports the same hang forever (fix49 read 338 restarts, fix50 still 171). */
    if(!ring_->outstanding()) {hang_fence_=0;progress_retries_=0;return kIOReturnSuccess;}
    const uint32_t last=ring_->lastSubmitted();
    if(!last)return kIOReturnSuccess;
    uint32_t fence=0;
    if(ring_->observeFence(last,nullptr,0,&fence)==NativeAdrenoRingFencePoisoned)return poison();
    if(fence!=hang_fence_) {hang_fence_=fence;progress_retries_=0;return kIOReturnSuccess;}
    if(fence>=last || madeProgress())return kIOReturnSuccess;
    progress_retries_=0;++stats_.hangs;*hung=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::recover(NativeAdrenoRestart restart,void *cookie,const NativeAdrenoRingClock *clock,
    NativeAdrenoRecovery *out)
{
    if(!out)return kIOReturnBadArgument;*out={};
    if(!attached_)return kIOReturnNotReady;
    if(!restart)return kIOReturnUnsupported;
    /* A restart replays the jobs, so a target captured before it no longer names the run it was meant to read. */
    tgt_pending=0;tgt_pending_vm=nullptr;
    /* Cleared here, not on the failure paths alone: a recover() that succeeds writes none of the four, so
     * the log would otherwise print the previous failure's step and value beside a successful restart. */
    native_adreno_recover_trace[0]=0;native_adreno_recover_trace[1]=0;
    const uint32_t last=ring_->lastSubmitted();
    /* Each replayed job is submitted again, which advances the ring's seqno: the loop's bound has to be the
     * number captured here, not a value re-read after the replays. */
    const uint32_t replayLast=last;
    uint32_t fence=0;
    {const IOReturn fr=ring_->fenceValue(&fence);
    native_adreno_recover_trace[2]=fence;native_adreno_recover_trace[3]=last;
    if(fr || fence>last) {native_adreno_recover_trace[0]=1;native_adreno_recover_trace[1]=uint32_t(fr);return poison();}}
    /* What already completed retires as usual. */
    for(auto &job:jobs_)if(job.active && job.fence<=fence) {
        IOReturn status=release(job);if(status)return status;
        ++stats_.completed;
    }
    const uint32_t hung=fence<last?fence+1:fence;
    Job *guilty=fence<last?find(hung):nullptr;
    out->fence=hung;
    if(guilty) {
        /* The job on the hung fence is the guilty one (NativeAdrenoSubmit.h:141): the VM that submitted it is
         * marked whether or not it is this submit object's own. The old argument here said the opposite, and
         * NativeAdrenoVm::noteHang dropped it anyway. */
        NativeAdrenoVm *vm=guilty->vm?guilty->vm:vm_;
        vm->noteHang(true);out->guilty=vm;
    }
    IOReturn status=restart(cookie,ring_,hung);
    out->status=status;
    if(status) {
        native_adreno_recover_trace[0]=2;native_adreno_recover_trace[1]=uint32_t(status);
        /* A restart that failed leaves the GPU where it was, which is what the board had before any hook
         * existed: the queue stays stopped but every other client keeps running. Poisoning here would be
         * worse - submit() and hangCheck() then answer kIOReturnNotReady for the rest of the boot
         * (submit() and hangCheck(), the two kIOReturnNotReady returns above) and recover() is never reached
         * again, so a hook that cannot bring this part back
         * would turn "the GPU is stuck" into "the GPU is permanently refused". Three failures in a row is
         * the point at which that is the honest answer instead.
         * The retry is not free and usually does not happen: a hook that fails after its ring->restart()
         * succeeded has already moved the fence to the hung value (NativeAdrenoRing.cpp:423), so the next
         * hangCheck sees nothing outstanding and returns before recover() - one attempt, no poison,
         * and the row freezes at hangs=1. Only a hook that fails before that point is retried.
         * Left as it is, that shape also means a client's WaitFence(hung) reads "complete" for work the
         * GPU never ran. Nothing is reclaimed wrongly while the CP stays stopped, which is why this is
         * acceptable for an experiment and has to be solved before the hook becomes the default. */
        if(++restart_failures<3) return status;
        return poison();
    }
    restart_failures=0;
    /* After the restart the GPU is idle with nothing outstanding, so there is no fence to be stuck on:
     * hang_fence_ must start over, or the very next check compares against the hung fence again and reports
     * the same hang forever (fix49 read hangs=338 restarts=338 in 176 s, one every 500 ms). */
    poisoned_=false;hang_fence_=0;progress_retries_=0;
    if(guilty) {status=release(*guilty);if(status)return status;}
    /* The GPU is idle after the restart's CP init: the CPU installs the kernel table, and each replayed job switches to its own. */
    if(table_mode_) {
        if(!write_table_) {native_adreno_recover_trace[0]=3;native_adreno_recover_trace[1]=0;return poison();}
        status=write_table_(table_cookie_,vm_->ttbr0());
        if(status) {native_adreno_recover_trace[0]=3;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
        if(table_mode_==2)ring_->tableInstalled(vm_->ttbr0());
        table_vm_=vm_;
    }
    uint32_t nop=0;
    if(AdrenoPM4Type7(0x10,0,&nop)!=AdrenoPM4Success) {native_adreno_recover_trace[0]=4;return poison();}   /* CP_NOP */
    /* `hung` is the fence the GPU never wrote, so that job is the one being dropped; the replay starts at
     * the next one. Each replayed job must come back as its own fence: restart() set seqno_ to `completed`,
     * so submitFramed hands out completed+1, completed+2, ... and a job whose fence is further ahead than
     * that position cannot match. Fix47 read step=6 value=8 with hung=5: the loop asked for fence 6 and
     * got 8, i.e. the ring had already issued two more. */
    /* The ring's seqno carries on from where it was (fence 7 was already handed out before the hang),
     * so a replayed submission's fence is seqno_+1 whatever the job's original number was. Match the job
     * by its own fence and accept the ring's value: the two agree only when no submission happened between
     * the hung job and the restart, and that is not the case here (fix47/48 both read value=8 for next=6). */
    /* A job whose IB the GPU already stopped on is not replayed: it is what hung the ring, and replaying it
     * hangs the ring again. fix51 read hangs=169 restarts=169 with the fence climbing 173..175 in 176 s --
     * each restart replayed the same bad submission and the next check found the same hang. */
    /* A replay submits jobs the loop has not reached yet, and restart() moved seqno_ to the hung fence, so those
     * jobs' original fences are above it. useVm()'s waitAll() would wait on them and the ring answers Argument
     * for a fence it never issued, which poisons the submit path on the second table switch of a replay. Wait for
     * what this loop itself submitted instead: the ring's last fence, which is the replayed prefix and nothing
     * else. The first job skips even that, because restart() has just emptied the ring. */
    bool firstReplay=true;
    for(uint32_t next=hung+1;next && next<=replayLast;++next) {
        Job *job=find(next);
        NativeAdrenoVm *vm=job && job->vm?job->vm:vm_;
        const bool run=job && !vm->unusable() && job->word_count;
        if(!firstReplay) {
            const uint32_t replayed=ring_->lastSubmitted();
            if(replayed && ring_->wait(replayed,clock,2000000,nullptr)!=NativeAdrenoRingFenceComplete) {
                native_adreno_recover_trace[0]=7;native_adreno_recover_trace[1]=replayed;return poison();
            }
        }
        status=useVm(vm,clock,true);
        firstReplay=false;
        if(status) {native_adreno_recover_trace[0]=5;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
        ring_->useContext(job?job->context:0);
        uint32_t value=0;
        status=ring_->submitFramed(run?job->words:&nop,run?job->word_count:1,&value);
        if(status) {native_adreno_recover_trace[0]=6;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
        /* The replayed job is now watched under the fence the ring just issued, not under its old number. */
        if(job && job->active) job->fence=value;
        ++out->replayed;
    }
    ++stats_.restarts;
    return kIOReturnSuccess;
}
