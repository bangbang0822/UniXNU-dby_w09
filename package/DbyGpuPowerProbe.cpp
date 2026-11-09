/* DbyGpuPowerProbe — the GPU power/clock path that has to exist before the GPU SMMU may be read.
 *
 * WHY THIS EXISTS
 *   2026-11-03 13:12: a probe that read the GPU SMMU (0x03da0000) at boot without powering the GPU
 *   froze this device in the first round it ran (evidence/edl-log-s22: uptime_s=18, no slot after the
 *   round the probe fired in; recovery boot of the same kernel without the argument ran 582 s).
 *   Work/tracks/gpu-smmu-read-hang/REPORT.md is the write-up. The vendor driver never reads a GPU SMMU
 *   register before its three GPU branch clocks are on (kgsl_iommu.c:945-955), and the upstream SM8250
 *   kit states the same rule as code: A650GmuPower.c:258-263 refuses to read SMMU registers unless
 *   gpu_cx_gdsc is PWR_ON and the hlos1 SMMU vote clock is running.
 *
 *   So this probe does what that gate expects, in this order:
 *     mode 1  read-only, GCC and GCC_VOTE only: GPUCC, SMMU and GPU windows are masked out, so no
 *             GPUCC/SMMU/RBBM register can be read whatever the bootloader left powered; no writes
 *     mode 2  as 1, plus one bit: gcc_gpu_cfg_ahb_clk's enable (GPUCC is unreadable without it)
 *     mode 3  as 2, plus gpu_cx_gdsc and the gpu_cc_ahb / hlos1 SMMU vote branches; the SMMU identity
 *             registers are read only after that gate passes
 *     mode 4  "3a": the same power-up as mode 3 with the SMMU and GPU windows masked, so the GDSC/clock writes
 *             can be proven on their own
 *     mode 6  CX + the GMU clocks at the GMU OPP 200 MHz (the kit sequence), then the AOP command
 *             database: the level counts for gfx/mx/cx and the four BCM addresses.  Rows 2 and 3 are
 *             reused for GPU CLK and GPU DB; the SMMU and GPU windows are masked because this stage
 *             measures clocks and tables only
 *     mode 5  the SMMU CONFIGURATION, read-only: sACR plus S2CR slot 0 and slot 5, on top of what the kit
 *             already reads in mode 3 (sCR0, ID0-2, sGFSR, SMR and the first valid S2CR, A650GmuPower.c:264-285).
 *             BYPASS on a slot means identity translation is already in place, FAULT means that client is
 *             blocked today; sACR is reported raw because a bare read cannot prove NS programming rights - the rows are rendered only after A650GpuBringup returns, so
 *             a freeze inside the writes would otherwise leave no evidence at all
 *
 *     mode 9  round 2c: modes 7 and 8 first (the table pool, the two normalised slots, the four regions),
 *             then the GMU's own SMMU context - sCR0 on a row of its own, the four ranges built into the
 *             16-page pool with ArmSmmu4KBuild (13 pages), encode, program bank 1 / ASID 2, attach streams
 *             4 and 5, apply the DT's SMR mask, TLB sync, observe and compare.  No cold start.
 *
 *   gcc_gpu_memnoc_gfx is NOT touched by modes 1-5.  Mode 6 runs the kit's GMU clock sequence, which
 *   writes gcc_gpu_memnoc_gfx and gcc_ddrss_gpu_axi (A650GmuPower.c:399-403); gcc_gpu_snoc_dvm_gfx is
 *   only READ there, not written.  That is the whole point of that mode.  Mode 1 read it as 0x80004221: CBCR_ENABLE (bit 0) already
 *   set AND CLK_OFF (bit 31) set, i.e. enabled-but-halted, so a second bit-0 write changes nothing (review
 *   2026-11-03 19:54).  gcc_gpu_snoc_dvm_gfx reads 0x80000001 - the same signature.  Note the vendor DOES
 *   enable these branches from the kernel (kgsl_iommu.c:945-955 clk_prepare_enable’s the three node clocks
 *   before any SMMU access); what this probe relies on is that once CX/GX power is up the branches unhalt,
 *   which is exactly what modes 3/4 measure.  Enabling them from the board probe was rejected because the
 *   kit owns that sequence (its mode 6 has the mechanism, A650GmuPower.c:403).
 *   All of the actual sequencing is upstream code: kits/Sm8250Platform/A650GmuPower.c, called through
 *   A650GpuBringup(). This file maps the seven windows, decides which of them a given mode may touch
 *   (the kit skips any window whose bit is missing from have_windows) and renders the result. The two
 *   files do NOT come from one module revision: the probe is board package code, the kit is the
 *   Sm8250Platform feed at the revision pinned in feeds.toml/components.toml.
 *
 *   That window mask is not cosmetic: the kit's own gates are prerequisite-based, so without it mode 1
 *   would also read GPUCC when the bootloader happened to leave gcc_gpu_cfg_ahb_clk on, and the SMMU
 *   when CX was already powered - the block that froze this device on 2026-11-03.
 *
 * BOOT ARGUMENT
 *   `dby-gpu-power=<1|..|24>`; absent means nothing happens beyond one "skipped" row. Mode 17 is the
 *   first mode where the A650 driver archive owns the address spaces instead of the probe: it runs
 *   the same power and clock path, hands the GMU context to NativeAdrenoGmuContextBringupRoutes
 *   (slots 4,5), cold-starts from that context's payloads, powers GX, then hands the GPU context
 *   to NativeAdrenoGpuContextBringupRoutes (slots 0..3) and reads the five kernel buffers. It
 *   renders `GPU ARC step= gmu= pay= cpu= cold=` and `GPU ARC gx= gpu= buf= bits=`. The argument is
 *   deliberately separate from `dby-gpu-smmu=1` (the older, now disabled probe): power-only runs
 *   (mode 1/2) are safe to take without ever touching the SMMU.
 *
 * ROWS (six 120-byte rows, appended to the page after the [clocks] census rows)
 *   row0  GPU PWR m=<mode> gcc=<state> cc=<state> smmu=<state> gpu=<state> cx=<0|1|2> clk=<0|1> ahb=<0|1>
 *         (cx=2 means the kit found the CX rail already powered, A650GmuPower.c:222)
 *   row1  GPU PWR cfg_ahb=%08x>%08x memnoc=%08x dvm=%08x gpll0=%08x
 *   row2  GPU PWR cxgdscr=%08x hwctrl=%08x cc_ahb=%08x cc_smmu=%08x before=%08x
 *         mode 6 replaces this row with  GPU CLK rc= step= fail= rcg= br= vote= cfg= cmd= ahb= cxm=
 *         (cfg is the RCG config word: 0602 = sel 6 / hid 2 = the 200 MHz GMU source)
 *   row3  GPU SMMU id0=%08x id1=%08x id2=%08x input_bits=<n> oas=<n> gated=Y/N
 *         mode 6 replaces this row with  GPU DB st= gfx= mx= cx= gmxc= mc0= sh0= acv= cn0=
 *         (st: 0 success 1 corrupt 2 missing; gfx/mx/cx are 16-bit word COUNTS of the level lists, not
 *          frequencies; mc0/sh0/acv/cn0 are the four BCM register addresses)
 *         mode 5 replaces this row with  GPU SMMU cfg raw sACR lock=… slot0=… slot5=…
 *   row2 / row3 in mode 7:  GPU GMUCTX slots fixed=<n>/<seen> want=00020000 s2cr4=<hex> s2cr5=<hex>
 *         and  GPU GMUCTX pool phys=<hi><lo> bytes=65536 align4k=<Y|N>
 *         (64 KiB: the 13-page Build in mode 9 must fit in it, see DBY_SMMU_TABLE_POOL_BYTES)
 *         (mode 7 overwrites the cxgdscr/cc_ahb/cc_smmu snapshot and the mode-3 identity row; its gate is stricter: smmu==Done plus a plausible hlos1 vote CBCR and GDSC)
 *   row1 in mode 9:  GPU SMMU GBL before=<hex> after=<hex> ok=<0|1> sACR=<hex> rst=<0|1|2>
 *         (mode 9 replaces the clock row: the sCR0 read-modify-write is its first step, so it gets a row
 *          of its own; rst= is 1 when the pre-boot word was put back after a later failure, 2 when that
 *          failed.  sACR is reported raw and never written - see dbySmmuGlobalEnable)
 *   row2 / row3 in mode 9:  GPU SMMUCTX gbl= bld= enc= pre= prog= gf=<hex>
 *         and  GPU SMMUCTX str= smr= tlb= obs= val= stv= en= step= sFSR=<hex> gnr1=<hex>
 *         (one status per stage: ff = never entered, 0..11 = ArmSmmu4KStatus, 0x100+ = this probe's own
 *          codes from the DBY_GMU_CTX_* defines; the first non-zero status stops the chain and step=
 *          names the stage where it stopped.  gf= is sGFSR, sFSR= is bank 1's, gnr1= is sGFSYNR1: the
 *          unmatched-stream SID, which is how a GMU SID the two SMRs do not cover would show up)
 *   row4 in mode 9:  GPU SMMUCTX mem ic=<phys>>12 du=<phys>>12 lo=<phys>>12 hf=<phys>>12 root=<pool>>12
 *         (the four regions mode 8 allocated and the TTBR0 root, page numbers, so mode 9 is readable alone)
 *   row4 / row5 in modes 10..14:  GPU COLD st= phase= blocks= ver= init= hfi= rdy=   and
 *         GPU COLD rst= rsc= clk=<rc>/<step> ao= HFI pst= stg= ack= err=
 *         (mode 11 replaces row5 with the GMU state, mode 12 with the AO/SMMU fault registers and the
 *          GMU log, modes 13/14 with  GPU GX st= gx= on= oob= info= vote= err=)
 *   row2 / row3 in mode 14:  GPU CP st=<hex> step= sqe=<bytes>/<bad> rptr= wptr= me= mw= hf= mark=<hex>
 *         and  GPU GPUCTX pre= bld= enc= prog= str= tlb= obs= val= pg= rw=<hex> ttbr0=<hex>
 *         (st= 0 is the only success; every other value names the failing step (0xFFFFFF01..19).  step is
 *          the last phase entered, me=/mw= are the two fences the CP wrote into the memptrs page, mark= is
 *          the scratch word CP_MEM_WRITE wrote (0xa6500c0d passes), and hf= is CP_HW_FAULT.  Row 3 is bank
 *          0's context stages in the mode-9 order, pg= its table pages, rw= the first GPU register dword
 *          that refused a write, actlr= the CB0 ACTLR before CPRE was cleared, ttbr0= the read-back
 *          root with ASID 1 in the top half.  Modes 10..13 leave
 *          rows 2/3 to the GMU context and the cold start; mode 14 is the first that overwrites them.)
 *   row2 / row3 in mode 15:  GPU ZAP st=<hex> step= init=<a0>/<a1> mem=<a0>/<a1> auth=<a0>/<a1> sec=<n> tr=<hex>
 *         and  GPU CP st=<hex> rptr= wptr= input_bits=<n|--> id2=<hex> me= mw= sf= hf= mark=<hex>
 *         input_bits is ID2[11:8] decoded exactly as the A650 driver decodes it, taken from the
 *         identity words the GPU context encode already read and proved (G-1); -- means those
 *         words were not available, which is not a verdict.
 *         (st= 0 is the only success; ffffff20 the mode 14 gate refused, ffffff21..2e the PAS load
 *          failed at step, ffffff2f/30 the CP_SET_SECURE_MODE(0) packet or its fence.  step names the
 *          phase: 1 parse, 2 metadata, 3 init_image, 4 mem_setup, 5 segment copy, 6 auth_and_reset,
 *          7 done.  init=/mem=/auth= are the SMC return pairs, sec= the CP packet fence result, tr=
 *          RBBM_SECVID_TRUST_CONTROL as found (read only - writing it stalls this side, s77).  Row 3
 *          carries a progress marker before each phase (GPU ZAP prog=<n> meta=<hi><lo>): if the probe
 *          stops inside one, the log keeps the phase it entered.)
 *   rows 0..2 in mode 16 (diagnostic, no GPU access):  GPU TZ avail=<a0>/<a1> sup=<a0>/<a1>
 *         and  GPU TZ init=<rc> ret=<a0>/<a1> meta=<hi><lo> bits=<32|40> ehdr= hash=
 *         (avail = qcom_scm_is_call_available(PAS init_image), sup = qcom_scm_pas_supported(13);
 *          init is one init_image whose metadata buffer was forced below 4 GiB.  A supported PAS
 *          with init=0 means the image is acceptable and mode 15's failure was the address.)
 *   row4  GPU PWR rbbm=n/a (gpu=<n>) ioerr=%d  — every mode masks the GPU window, so rbbm is a real
 *         read only in a later round that enables GX
 *   row5  GPU PWR note: <what is still missing>
 *   state values are the kit's A650GpuStageState: 0 not-run, 1 done, 2 skipped-prereq,
 *   3 skipped-mode, 4 no-map.
 */

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/libkern.h>
#include <mach/kern_return.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

/* The upstream kit. Its header is in the same directory in the tree, and the kit TU is compiled into
 * this archive by scripts/build-gpu-power-probe.py, so both come from one module revision. */
#include "AdrenoFirmware.h"
#include "AdrenoHfi.h"
#include "AdrenoHfiStartup.h"
#include "AdrenoHfiVotes.h"
#include "DbyA650Gmu.h"
#include "A650GmuPower.h"
#include "DbyGpuFirmware.h"
#include "ArmSmmu4K.h"
#include "ArmSmmu4KStreams.h"
#include "AdrenoPM4.h"

extern "C" {

/* Row storage the logger reads; same shape as native_cpufreq_census / native_gpusmmu_census. */
char native_gpupower_rows[6][120];

/* 0 = not entered, 1 = entered, 2 = measured, 3 = skipped (no argument), 4 = no window mapped. */
volatile uint32_t native_gpupower_stage;

/* The archive writer's progress, weak-imported from package/BootLog.c:206 (declaration) / :1174 (increment). The probe refuses to run
 * until at least one slot has been stored and the last round succeeded - see the gate in
 * sheng_gpu_power_probe(), kept as defence in depth after the early call site was removed. */
extern volatile uint32_t boot_log_slots_ok __attribute__((weak_import));

/* ---- the seven windows, from A650GmuPower.h (DTS values, not invented here) ---- */
struct DbyGpuWindow {
	uint64_t base;
	uint64_t size;
};

static const struct DbyGpuWindow kDbyGpuWindows[A650GpuWindows] = {
	[A650GpuWinGccGpu]  = { A650_GPU_BASE_GCC_GPU,  0x1000ull },  /* 0x171000, regs 0x004..0x154 */
	[A650GpuWinGccVote] = { A650_GPU_BASE_GCC_VOTE, 0x1000ull },  /* 0x152000, reg 0x000 */
	[A650GpuWinGpucc]   = { A650_GPU_BASE_GPUCC,    0x9000ull },  /* header: size 0x9000 */
	[A650GpuWinSmmu]    = { A650_GPU_BASE_SMMU_GR0, 0x10000ull }, /* DTS: 0x3da0000 0x10000 */
	[A650GpuWinGpu]     = { A650_GPU_BASE_GPU,      0x40000ull }, /* DTS: 0x3d00000 0x40000 */
	[A650GpuWinGmu]     = { A650_GPU_BASE_GMU,      0x30000ull }, /* DTS: 0x3d6a000 0x30000 */
	[A650GpuWinRscc]    = { A650_GPU_BASE_RSCC,     0x10000ull }, /* DTS: 0x3de0000 0x10000 */
};

/* One mapping per window plus the cookie the kit's callbacks carry. */
struct DbyGpuState {
	OSSharedPtr<IOMemoryMap> keep[A650GpuWindows];
	uintptr_t base[A650GpuWindows];
	unsigned mapped;
};

static DbyGpuState gDbyGpu;

/* SMMU CONFIGURATION registers read by mode 5 (same window as ID0/1/2, offsets from arm-smmu-regs.h):
 * SCR0 0x000, sACR 0x010, S2CR slot n at 0xc00 + 4n.  These decide whether the GPU and GMU can reach
 * memory; the identity read of 2026-11-03 could not answer that.
 *
 * sACR CACHE_LOCK (bit 26) is NOT a bare-read permission flag: the vendor driver clears it in the
 * non-secure copy and reads it back, because modifying that copy is only allowed when the secure copy
 * permits it (arm-smmu.c:7117-7127).  A cold-boot lock=Y is the normal pre-clear state, not a denial,
 * and the field only exists on MMU-500 r2+ (arm-smmu.c:4852-4862 reads ID7 major first).  The row
 * therefore reports the bit as a raw observation, not a verdict. */
#define A650_SMMU_SCR0          0x000u
#define A650_SMMU_SACR          0x010u
/* AArch64 SMMU-500 identity registers, one quirk-free read each. */
#define A650_SMMU_S2CR_BASE     0xc00u
#define A650_SMMU_ACR_CACHE_LOCK (1u << 26)

/* S2CR type field, bits[17:16]: 0 TRANSLATE, 1 BYPASS, 2 FAULT, 3 reserved (arm-smmu-regs.h:124-128). */
static const char *
dbyGpuS2crType(uint32_t s2cr)
{
	/* vendor drivers/iommu/arm-smmu-regs.h:119-128 and kits/ArmSmmu4K/ArmSmmu4KStreams.c:3:
	 * 0 = TRANSLATE, 1 = BYPASS, 2 = FAULT, 3 = reserved.  The first version of this table had
	 * TRANSLATE and FAULT swapped, which inverted the one conclusion the row exists to produce
	 * (review 2026-11-04 01:10, blocking 2). */
	switch ((s2cr >> 16) & 0x3u) {
	case 0u: return "TRANSLATE";
	case 1u: return "BYPASS";
	case 2u: return "FAULT";
	default: return "RSVD";
	}
}

static int
dbyGpuMapWindow(unsigned window)
{
	IODeviceMemory *memory;
	OSSharedPtr<IOMemoryMap> map;

	if (window >= A650GpuWindows) {
		return 0;
	}
	memory = IODeviceMemory::withRange(kDbyGpuWindows[window].base, kDbyGpuWindows[window].size);
	if (memory == nullptr) {
		return 0;
	}
	map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	if (!map) {
		return 0;
	}
	gDbyGpu.keep[window] = map;
	gDbyGpu.base[window] = static_cast<uintptr_t>(map->getVirtualAddress());
	return gDbyGpu.base[window] != 0;
}

/* The AOP command database the GMU OPP and bandwidth vote tables come from.  It is NOT one of the kit's
 * seven windows, so it gets its own mapping.  Address and size are the kit's own constants
 * (kits/Sm8250Platform/A650GmuPower.h:35-36); the window is plain RAM owned by the AOP, readable without
 * powering any GPU block. */
#define DBY_AOP_CMD_DB_BASE 0x80860000ull
#define DBY_AOP_CMD_DB_SIZE 0x20000ull

static uintptr_t gAopDbBase;

static int
dbyAopMap(void)
{
	IODeviceMemory *memory;
	OSSharedPtr<IOMemoryMap> map;

	if (gAopDbBase != 0) {
		return 1;
	}
	memory = IODeviceMemory::withRange(DBY_AOP_CMD_DB_BASE, DBY_AOP_CMD_DB_SIZE);
	if (memory == nullptr) {
		return 0;
	}
	map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	if (!map) {
		return 0;
	}
	gAopDbBase = static_cast<uintptr_t>(map->getVirtualAddress());
	/* Keep the mapping alive for the life of the kernel: this probe is one-shot but the page must not be
	 * unmapped while the parser may still be reading it, so the OSSharedPtr is leaked deliberately. */
	(void)map.detach();
	return gAopDbBase != 0;
}

static int
dbyGpuRead32(void *cookie, unsigned window, size_t offset, uint32_t *out)
{
	DbyGpuState *state = static_cast<DbyGpuState *>(cookie);

	if (state == nullptr || out == nullptr || window >= A650GpuWindows ||
	    (state->mapped & (1u << window)) == 0u || offset + 4u > kDbyGpuWindows[window].size) {
		return 1;
	}
	*out = *(volatile uint32_t *)(state->base[window] + offset);
	return 0;
}

static int
dbyGpuWrite32(void *cookie, unsigned window, size_t offset, uint32_t value)
{
	DbyGpuState *state = static_cast<DbyGpuState *>(cookie);

	if (state == nullptr || window >= A650GpuWindows ||
	    (state->mapped & (1u << window)) == 0u || offset + 4u > kDbyGpuWindows[window].size) {
		return 1;
	}
	*(volatile uint32_t *)(state->base[window] + offset) = value;
	return 0;
}

/* 64-bit window access: ONE aligned load or store, never two 32-bit halves.  The kit reads TTBR0/TTBR1
 * through read64 and requires "actual aligned 64-bit TTBR accesses" (ArmSmmu4KProgram.h:9-11,
 * ArmSmmu4KProgram.c:12); a TTBR read as two halves can tear, so the callback is real rather than
 * composed.  The SMMU window is mapped kIOMapInhibitCache, i.e. this is a Device access. */
static int
dbyGpuRead64(void *cookie, unsigned window, size_t offset, uint64_t *out)
{
	DbyGpuState *state = static_cast<DbyGpuState *>(cookie);

	if (state == nullptr || out == nullptr || window >= A650GpuWindows ||
		(state->mapped & (1u << window)) == 0u || (offset & 7u) != 0u ||
		offset + 8u > kDbyGpuWindows[window].size) {
		return 1;
	}
	*out = *(volatile uint64_t *)(state->base[window] + offset);
	return 0;
}

static int
dbyGpuWrite64(void *cookie, unsigned window, size_t offset, uint64_t value)
{
	DbyGpuState *state = static_cast<DbyGpuState *>(cookie);

	if (state == nullptr || window >= A650GpuWindows ||
		(state->mapped & (1u << window)) == 0u || (offset & 7u) != 0u ||
		offset + 8u > kDbyGpuWindows[window].size) {
		return 1;
	}
	*(volatile uint64_t *)(state->base[window] + offset) = value;
	return 0;
}

static int
dbyGpuBarrier(void *)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static void
dbyGpuDelay(void *, unsigned us)
{
	IODelay(us == 0u ? 1u : us);
}

/* ---- bounded row rendering (no snprintf is reachable from a prelinked driver here) ---- */
static char *
putStr(char *o, const char *end, const char *s)
{
	while (*s != 0 && o < end) {
		*o++ = *s++;
	}
	return o;
}

static char *
putHex8(char *o, const char *end, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";

	for (int shift = 28; shift >= 0 && o < end; shift -= 4) {
		*o++ = digits[(v >> shift) & 0xFu];
	}
	return o;
}

static char *
putHex4(char *o, const char *end, uint32_t v)
{
	static const char digits[] = "0123456789abcdef";

	for (int shift = 12; shift >= 0 && o < end; shift -= 4) {
		*o++ = digits[(v >> shift) & 0xFu];
	}
	return o;
}

static char *
putDec(char *o, const char *end, uint32_t v)
{
	char tmp[11];
	unsigned n = 0;

	if (v == 0u) {
		if (o < end) {
			*o++ = '0';
		}
		return o;
	}
	while (v != 0u && n < sizeof tmp) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (n != 0u && o < end) {
		*o++ = tmp[--n];
	}
	return o;
}

static char *
putSigned(char *o, const char *end, int v)
{
	if (v < 0 && o < end) {
		*o++ = '-';
		return putDec(o, end, (uint32_t)(-v));
	}
	return putDec(o, end, (uint32_t)v);
}

/* Same tables the older probe uses and the ArmSmmu4K kit asserts: ID2[11:8] = UBS, ID2[7:4] = OAS. */
static unsigned
dbyGpuInputBits(uint32_t id2)
{
	static const uint8_t bits[6] = { 32, 36, 40, 42, 44, 48 };
	const unsigned code = (id2 >> 8) & 0xFu;

	return code < 6u ? bits[code] : 48u;
}

static unsigned
dbyGpuOutputBits(uint32_t id2)
{
	static const uint8_t bits[6] = { 32, 36, 40, 42, 44, 48 };
	const unsigned code = (id2 >> 4) & 0xFu;
	unsigned value = code < 6u ? bits[code] : 48u;

	return value > 40u ? 40u : value;
}

/* One row: "GPU PWR m=<mode> gcc=<s> cc=<s> smmu=<s> gpu=<s> cx=<0|1> clk=<0|1> ahb=<0|1>". */
static void
renderState(unsigned mode, const A650GpuResult *r)
{
	char *row = native_gpupower_rows[0];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU PWR m=");
	at = putDec(at, end, mode);
	at = putStr(at, end, " gcc=");
	at = putDec(at, end, r->gcc);
	at = putStr(at, end, " cc=");
	at = putDec(at, end, r->cc);
	at = putStr(at, end, " smmu=");
	at = putDec(at, end, r->smmu);
	at = putStr(at, end, " gpu=");
	at = putDec(at, end, r->gpu);
	at = putStr(at, end, " cx=");
	at = putDec(at, end, r->powered_cx);
	at = putStr(at, end, " clk=");
	at = putDec(at, end, r->powered_clocks);
	at = putStr(at, end, " ahb=");
	at = putDec(at, end, r->enabled_ahb);
	*at = 0;
}

static void
renderClocks(const A650GpuResult *r)
{
	char *row = native_gpupower_rows[1];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU PWR cfg_ahb=");
	at = putHex8(at, end, r->cfg_ahb);
	at = putStr(at, end, ">");
	at = putHex8(at, end, r->cfg_ahb_after);
	at = putStr(at, end, " memnoc=");
	at = putHex8(at, end, r->memnoc);
	at = putStr(at, end, " dvm=");
	at = putHex8(at, end, r->dvm);
	at = putStr(at, end, " gpll0=");
	at = putHex8(at, end, r->pll_vote);
	*at = 0;

	row = native_gpupower_rows[2];
	end = row + 119;
	at = row;
	at = putStr(at, end, "GPU PWR cxgdscr=");
	at = putHex8(at, end, r->cx_gdscr);
	at = putStr(at, end, " hwctrl=");
	at = putHex8(at, end, r->cx_hwctrl);
	at = putStr(at, end, " cc_ahb=");
	at = putHex8(at, end, r->cc_ahb);
	at = putStr(at, end, " cc_smmu=");
	at = putHex8(at, end, r->cc_smmu);
	at = putStr(at, end, " before=");
	at = putHex8(at, end, r->cc_smmu_before);
	*at = 0;
}

/* Positive identification before any verdict (copied from package/DbyGpuSmmuProbe.cpp, where the
 * 2026-11-03 12:55 review required it).  A650GpuBringup reports A650GpuDone for the SMMU stage even when
 * an individual read failed (A650GmuPower.c:127-137 zeroes the destination and only sets io_error,
 * while :265-285 still marks the stage done), so "Done" is not "answered".
 *
 * The fields and their sources:
 *   ID0[30] S1TS, ID0[27] SMS, ID0[7:0] NUMSMRG   (ArmSmmu4KContext.c:34, ArmSmmu4KStreams.c:17)
 *   ID1[7:0] NUMCB, ID1[23:16] NUMS2CB            (ArmSmmu4KProgram.c:72-79)
 *   ID2[11:8] UBS, ID2[7:4] OAS inside 0..5  (ArmSmmu4KContext.c:8-12)
 *   ID2[3:0] IAS inside 0..5                 (the same width table; the kit does NOT read IAS, so this
 *                                             row is this probe's own sanity check, not a kit rule)
 *   ID2[12] PTFS_4K                          (ArmSmmu4KContext.c:35)
 *   page geometry: NUMCB <= pages and 2*pages*stride <= the 0x10000 map (ArmSmmu4KProgram.c:72-79)
 * Returns nullptr when the block answers like a real SMMU-500, else the first failing capability. */
static const char *
dbyGpuSmmuReject(uint32_t id0, uint32_t id1, uint32_t id2)
{
	const unsigned ubs = (id2 >> 8) & 0xFu;
	const unsigned oas = (id2 >> 4) & 0xFu;
	const unsigned ias = id2 & 0xFu;
	const unsigned banks = id1 & 0xFFu;
	const unsigned s2 = (id1 >> 16) & 0xFFu;

	if (id0 == 0u || id1 == 0u || id2 == 0u ||
	    id0 == 0xFFFFFFFFu || id1 == 0xFFFFFFFFu || id2 == 0xFFFFFFFFu) {
		return "noanswer";
	}
	if ((id0 & (1u << 30)) == 0u) {
		return "S1TS";
	}
	if ((id0 & (1u << 27)) == 0u) {
		return "SMS";
	}
	if ((id0 & 0xFFu) == 0u) {
		return "NUMSMRG";
	}
	if (banks == 0u) {
		return "NUMCB";
	}
	if (s2 > banks) {
		return "NUMS2CB";
	}
	if (ubs > 5u || oas > 5u || ias > 5u) {
		return "width-code";
	}
	{
		/* ID1 page geometry, exactly what ArmSmmu4KProgram.c:72-79 requires before it programs a bank.
		 * (The other probe already carries this; the first version of this gate did not - review
		 * 19:54, must-fix M5.) */
		const unsigned pages = 1u << (((id1 >> 28) & 7u) + 1u);
		const unsigned stride = (id1 >> 31) ? 65536u : 4096u;

		if (banks > pages) {
			return "NUMCB>pages";
		}
		if ((uint64_t)2u * pages * (uint64_t)stride > 0x10000ull) {
			return "regspace";
		}
	}
	if ((id2 & (1u << 12)) == 0u) {
		return "PTFS4K";
	}
	if (id0 == id1 || id1 == id2 || id0 == id2) {
		return "repeat";
	}
	return nullptr;
}

static void
renderSmmu(const A650GpuResult *r)
{
	char *row = native_gpupower_rows[3];
	const char *end = row + 119;
	char *at;

	if (r->smmu != A650GpuDone) {
		/* Not "the SMMU is small" - the read never happened. Say which state stopped it. */
		at = putStr(row, end, "GPU SMMU not read: state=");
		at = putDec(at, end, r->smmu);
		at = putStr(at, end, " (2=prereq 3=mode 4=masked-or-nomap); no verdict");
		*at = 0;
		return;
	}
	/* Two ways to refuse a verdict, one printer (review 2026-11-03 21:45): the preconditions below and
	 * the capability gate.  Both must keep the identity words they already read - withholding a verdict
	 * must not throw the measurement away, which is the case this boot creates (memnoc/dvm stay halted
	 * even with CX powered, while the register read itself may be perfectly valid). */
	{
		const char *why = nullptr;

		if (r->io_error != 0) {
			why = "io_error";
		} else if (r->cc_ahb == 0u || r->cc_ahb == 0xFFFFFFFFu) {
			why = "cc_ahb silent";
		} else if (r->cc_smmu == 0u || r->cc_smmu == 0xFFFFFFFFu) {
			why = "cc_smmu silent";
		} else if ((r->cc_ahb & A650_CBCR_ENABLE) == 0u || (r->cc_ahb & A650_CBCR_CLK_OFF) != 0u) {
			why = "gpu_cc_ahb not running";
		} else if ((r->cc_smmu & A650_CBCR_ENABLE) == 0u || (r->cc_smmu & A650_CBCR_CLK_OFF) != 0u) {
			why = "smmu vote clock not running";
		} else if ((r->memnoc & A650_CBCR_CLK_OFF) != 0u || (r->dvm & A650_CBCR_CLK_OFF) != 0u) {
			why = "gcc gpu branches still halted";
		} else {
			why = dbyGpuSmmuReject(r->id0, r->id1, r->id2);
		}
		if (why != nullptr) {
			at = putStr(row, end, "GPU SMMU no verdict: ");
			at = putStr(at, end, why);
			at = putStr(at, end, " id0=");
			at = putHex8(at, end, r->id0);
			at = putStr(at, end, " id1=");
			at = putHex8(at, end, r->id1);
			at = putStr(at, end, " id2=");
			at = putHex8(at, end, r->id2);
			at = putStr(at, end, " cc_ahb=");
			at = putHex8(at, end, r->cc_ahb);
			*at = 0;
			return;
		}
	}
	at = putStr(row, end, "GPU SMMU id0=");
	at = putHex8(at, end, r->id0);
	at = putStr(at, end, " id1=");
	at = putHex8(at, end, r->id1);
	at = putStr(at, end, " id2=");
	at = putHex8(at, end, r->id2);
	at = putStr(at, end, " input_bits=");
	at = putDec(at, end, dbyGpuInputBits(r->id2));
	at = putStr(at, end, " oas=");
	at = putDec(at, end, dbyGpuOutputBits(r->id2));
	at = putStr(at, end, dbyGpuInputBits(r->id2) <= 32u ? " gated=Y" : " gated=N");
	*at = 0;
}

/* Mode 6: the GMU clocks, rendered over row 2 (the SMMU clock snapshot row is not needed here). */
static void
renderGmuClocks(const A650GmuClocksResult *clk, int rc)
{
	char *row = native_gpupower_rows[2];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU CLK rc=");
	at = putSigned(at, end, rc);
	at = putStr(at, end, " step=");
	at = putDec(at, end, clk->step);
	at = putStr(at, end, " fail=");
	at = putDec(at, end, clk->failed);
	at = putStr(at, end, " rcg=");
	at = putDec(at, end, clk->rcg_polls);
	at = putStr(at, end, " br=");
	at = putDec(at, end, clk->branch_polls);
	at = putStr(at, end, " vote=");
	at = putHex8(at, end, clk->vote);
	at = putStr(at, end, " cfg=");
	at = putHex8(at, end, clk->gmu_cfg);
	at = putStr(at, end, " cmd=");
	at = putHex8(at, end, clk->gmu_cmd);
	at = putStr(at, end, " ahb=");
	at = putHex8(at, end, clk->ahb);
	at = putStr(at, end, " cxm=");
	at = putHex8(at, end, clk->cx_gmu);
	*at = 0;
}

/* Mode 6: the AOP command database, over row 3 (mode 6 reads no SMMU).  The gfx level count is the number
 * the GMU OPP table must match, so it leads the row. */
static void
renderCmdDb(void)
{
	char *row = native_gpupower_rows[3];
	const char *end = row + 119;
	char *at = row;
	A650GpuDbResult db;
	A650GpuDbStatus st;

	memset(&db, 0, sizeof db);
	if (!dbyAopMap()) {
		at = putStr(at, end, "GPU DB nomap");
		*at = 0;
		return;
	}
	st = A650GpuCommandDbParse(reinterpret_cast<const uint8_t *>(gAopDbBase), DBY_AOP_CMD_DB_SIZE, &db);
	at = putStr(at, end, "GPU DB st=");
	at = putDec(at, end, (unsigned)st);
	at = putStr(at, end, " gfx=");
	at = putDec(at, end, (unsigned)(db.gfx_bytes / 2u));
	at = putStr(at, end, " mx=");
	at = putDec(at, end, (unsigned)(db.mx_bytes / 2u));
	at = putStr(at, end, " cx=");
	at = putDec(at, end, (unsigned)(db.cx_bytes / 2u));
	at = putStr(at, end, " gmxc=");
	at = putStr(at, end, db.gmxc_present != 0 ? "y" : "n");
	at = putStr(at, end, " mc0=");
	at = putHex8(at, end, db.mc0);
	at = putStr(at, end, " sh0=");
	at = putHex8(at, end, db.sh0);
	at = putStr(at, end, " acv=");
	at = putHex8(at, end, db.acv);
	at = putStr(at, end, " cn0=");
	at = putHex8(at, end, db.cn0);
	*at = 0;
}

/* --- round 2a: the GMU SMMU context prerequisites ------------------------------------------------
 *
 * The kit contract (A650Gmu.h:9-10) is that the caller already has "the GMU's own SMMU context mapping
 * the four memory regions".  Two things must exist before that context can be encoded:
 *   - the two stream slots, normalised to the canonical FAULT word, because this part's idle slots read
 *   - a physically contiguous, 4 KiB aligned table pool, because ArmSmmu4KConfig.table_physical is a
 *     physical address, not an IOVA (ArmSmmu4K.h:8-13).
 * Both are read back and reported here; the encode, the streams and the TLB sync come next.
 */
/* kona-gpu.dtsi:340 gmu_user iommus = <&kgsl_smmu 0x4 0x400>; :346 gmu_kernel = <0x5 0x400>: the first
 * cell is the stream ID, so the GMU owns slots 4 and 5 (slot == SID on this part). */
#define DBY_GMU_SLOT_FIRST 4u
#define DBY_GMU_SLOT_LAST  5u
#define DBY_GMU_SLOT_FAULT 0x00020000u
/* ArmSmmu4KBuild needs 13 table pages for these four ranges; the pool is 16 pages (64 KiB), so
 * the 3.6 MB arena ArmSmmu4KLive would need for this aperture is not necessary. */

#define DBY_SMMU_TABLE_POOL_BYTES (16u * 4096u)

/* Deliberately leaked for the life of the boot: the pool backs SMMU page tables the GMU walks, so
 * releasing the descriptor would free memory another master is using.  DBY_SMMU_TABLE_POOL_BYTES
 * (16 pages, 64 KiB), bounded and fixed.  gDbySmmuPoolCpu is the pool's CPU view, which is what
 * ArmSmmu4KBuild writes the table pages through;
 * gDbySmmuPoolPhys is the physical base the SMMU's TTBR0 is programmed with (they name one buffer). */
static IOBufferMemoryDescriptor *gDbySmmuPool;
static uint64_t gDbySmmuPoolPhys;
static volatile uint32_t *gDbySmmuPoolCpu;
static unsigned gDbySmmuSlotsFixed, gDbySmmuSlotsSeen, gDbySmmuPoolOk;

static uint32_t
dbySmmuS2crValue(const DbyGpuState *state, unsigned slot, int *ok)
{
	uint32_t value = 0;

	*ok = 0;
	if (dbyGpuRead32(const_cast<DbyGpuState *>(state), A650GpuWinSmmu,
	                 0xC00u + 4u * slot, &value) != 0) {
		return 0;
	}
	*ok = 1;
	return value;
}

static void
dbyGpuGmuContextPrep(void)
{
	DbyGpuState *state = &gDbyGpu;
	unsigned slot;

	gDbySmmuSlotsFixed = 0;
	gDbySmmuSlotsSeen = 0;
	gDbySmmuPoolOk = 0;

	for (slot = DBY_GMU_SLOT_FIRST; slot <= DBY_GMU_SLOT_LAST; slot++) {
		uint32_t before = 0;
		uint32_t after = 0;
		int ok = 0;

		{
			uint32_t smr = 0;
			if (dbyGpuRead32(state, A650GpuWinSmmu, 0x800u + 4u * slot, &smr) != 0) {
				continue;
			}
			/* SMR.V (bit 0) set means this slot already matches a stream ID.  The GMU kernel stream
			 * (SID 5) may legitimately be live from firmware, so a valid slot is left alone
			 * (review 2026-11-04 07:5x, blocking 3). */
			if ((smr & 1u) != 0u) {
				gDbySmmuSlotsSeen++;
				continue;
			}
		}
		before = dbySmmuS2crValue(state, slot, &ok);
		if (ok == 0) {
			continue;
		}
		gDbySmmuSlotsSeen++;
		/* Only where the type is already FAULT (2).  Writing the canonical word over a TRANSLATE slot
		 * would make it look busy, which is the failure this whole block exists to avoid. */
		if (((before >> 16) & 0x3u) != 2u) {
			continue;
		}
		if (before != DBY_GMU_SLOT_FAULT) {
			if (dbyGpuWrite32(state, A650GpuWinSmmu, 0xC00u + 4u * slot,
			                  DBY_GMU_SLOT_FAULT) != 0) {
				continue;
			}
		}
		(void)dbyGpuBarrier(state);      /* the kit orders the write and the read-back the same way */
		after = dbySmmuS2crValue(state, slot, &ok);
		if (ok != 0 && after == DBY_GMU_SLOT_FAULT) {
			gDbySmmuSlotsFixed++;
		}
	}

	{
		IOBufferMemoryDescriptor *pool = IOBufferMemoryDescriptor::withOptions(
		    kIOMemoryPhysicallyContiguous | kIODirectionOutIn, DBY_SMMU_TABLE_POOL_BYTES, 4096);
		if (pool != nullptr) {
			if (pool->prepare(kIODirectionOutIn) == kIOReturnSuccess) {
				IOPhysicalAddress phys = pool->getPhysicalAddress();
				volatile uint32_t *cpu = reinterpret_cast<volatile uint32_t *>(pool->getBytesNoCopy());
				if (phys != 0 && cpu != nullptr) {
					gDbySmmuPoolPhys = (uint64_t)phys;
					gDbySmmuPoolCpu = cpu;
					gDbySmmuPool = pool;
					gDbySmmuPoolOk = 1;
				}
			}
			if (gDbySmmuPoolOk == 0) {
				pool->release();
			}
		}
	}
}

static void
renderGmuContextPrep(void)
{
	char *row2 = native_gpupower_rows[2];
	char *row3 = native_gpupower_rows[3];
	const char *end2 = row2 + 119;
	const char *end3 = row3 + 119;
	char *at = row2;

	at = putStr(at, end2, "GPU GMUCTX slots fixed=");
	at = putDec(at, end2, gDbySmmuSlotsFixed);
	at = putStr(at, end2, "/");
	at = putDec(at, end2, gDbySmmuSlotsSeen);
	at = putStr(at, end2, " want=");
	at = putHex8(at, end2, DBY_GMU_SLOT_FAULT);
	at = putStr(at, end2, " s2cr4=");
	{
		int ok = 0;
		at = putHex8(at, end2, dbySmmuS2crValue(&gDbyGpu, DBY_GMU_SLOT_FIRST, &ok));
	}
	at = putStr(at, end2, " s2cr5=");
	{
		int ok = 0;
		at = putHex8(at, end2, dbySmmuS2crValue(&gDbyGpu, DBY_GMU_SLOT_LAST, &ok));
	}
	*at = 0;

	at = row3;
	at = putStr(at, end3, "GPU GMUCTX pool ");
	if (gDbySmmuPoolOk != 0) {
		at = putStr(at, end3, "phys=");
		at = putHex8(at, end3, (uint32_t)(gDbySmmuPoolPhys >> 32));
		at = putHex8(at, end3, (uint32_t)(gDbySmmuPoolPhys & 0xFFFFFFFFu));
		at = putStr(at, end3, " bytes=");
		at = putDec(at, end3, DBY_SMMU_TABLE_POOL_BYTES);
		at = putStr(at, end3, ((gDbySmmuPoolPhys & 0xFFFu) == 0u) ? " align4k=Y" : " align4k=N");
	} else {
		at = putStr(at, end3, "FAILED (contiguous alloc refused)");
	}
	*at = 0;
}

/* --- round 2b: the four GMU regions and the HFI startup table -------------------------------------
 *
 * A650GmuConfig wants four CPU-writable regions with fixed IOVAs (A650Gmu.h:43): icache at 0x4000,
 * dummy at 0x60000000, plus the log and HFI queues.  A650GmuBuildStartup turns the AOP level lists the
 * command DB parse returned into the AdrenoHfiStartup table.
 * The cold start itself needs the GMU SMMU context to map these regions first, so this mode only builds
 * and reports them; the encode is the next block.
 */
/* 16 MiB - 16 KiB.  The kit puts icache at IOVA 0x4000 (A650Gmu.c:109) and this board's
 * a650_gmu.bin has a payload block at 0x157000, which only fits in a region this size. */
#define DBY_GMU_ICACHE_BYTES (0xffc000u)
#define DBY_GMU_DUMMY_BYTES  (64u * 1024u)
#define DBY_GMU_LOG_BYTES    (16u * 1024u)
#define DBY_GMU_HFI_BYTES    (16u * 1024u)
/* The kit's own constants, restated here because the probe must build the A650GmuIo and the config
 * itself: kona-v2.1-gpu.dtsi:2 qcom,chipid (A650Gmu.h:21 agrees), and the PDC window of
 * kona-gpu.dtsi:302 (kgsl_gmu_pdc_cfg 0xb290000 0x10000). */
#define DBY_GMU_CHIP_ID      (0x06050002u)
#define DBY_GMU_PDC_BASE     (0x0b290000ull)
#define DBY_GMU_PDC_BYTES    (0x10000u)

typedef struct {
	IOBufferMemoryDescriptor *desc;
	OSSharedPtr<IOMemoryMap> keep;
	volatile uint32_t *cpu;
	uint64_t phys;
	uint64_t iova;
	size_t bytes;
} DbyGmuRegion;

static DbyGmuRegion gDbyGmuRegions[4];
static unsigned gDbyGmuRegionsOk;
static unsigned gDbyStartupStatus;
static int gDbyStartupRan;
static AdrenoHfiStartupConfig gDbyStartupCfg;
/* The cold start block owns these; declared here so mode 8 compiles without them.  The two maps are
 * deliberately leaked for the life of the boot: the GMU walks the regions, and the PDC block is live
 * for as long as the GMU is out of reset. */
static A650GmuDevice gDbyColdDevice;
static A650GmuConfig gDbyColdCfg;
static A650GmuStartupResult gDbyColdResult;
static uint32_t gDbyColdStatus;
static int gDbyColdRan;
static A650GmuClocksResult gDbyColdClocks;
static int gDbyColdClocksRc;
/* Mode 11: the GMU's own state after the attempt, read once. */
static uint32_t gDbyColdDbg[6];
/* Mode 13: GX power through the GMU, after a complete cold start. */
static A650GmuGxResult gDbyGxResult;
static uint32_t gDbyGxStatus;
static OSSharedPtr<IOMemoryMap> gDbyPdcMap;
static A650GmuMemory gDbyColdMem[4];

static int
dbyGmuRegionAlloc(DbyGmuRegion *region, size_t bytes, uint64_t iova)
{
	IOBufferMemoryDescriptor *desc;

	if (region->desc != nullptr) {
		return 1;
	}
	desc = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous | kIODirectionOutIn,
	                                             bytes, 4096);
	if (desc == nullptr) {
		return 0;
	}
	if (desc->prepare(kIODirectionOutIn) != kIOReturnSuccess) {
		desc->release();
		return 0;
	}
	/* ONE CPU view, and it is uncached.  The kit copies the GMU firmware through cfg.cpu and orders it
	 * with dsb alone (A650Gmu.c:205-214), so that view must be uncached or the GMU reads stale DDR; and
	 * a second, cacheable view of the same pages would give one physical range two memory types, which
	 * ARMv8 makes UNPREDICTABLE.  The descriptor keeps owning the memory; its own cacheable view is
	 * cleaned once here and never dereferenced again, so no dirty line can later evict over the GMU. */
	region->phys = (uint64_t)desc->getPhysicalAddress();
	region->bytes = bytes;
	region->iova = iova;
	{
		void *raw = desc->getBytesNoCopy();
		uintptr_t at, end;
		IODeviceMemory *memory;

		if (raw != nullptr) {
			__asm__ volatile("dsb sy" ::: "memory");
			for (at = (uintptr_t)raw, end = at + bytes; at < end; at += 64u) {
				__asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
			}
			__asm__ volatile("dsb sy" ::: "memory");
		}
		memory = region->phys != 0u ? IODeviceMemory::withRange((IOPhysicalAddress)region->phys, bytes) : nullptr;
		if (memory != nullptr) {
			region->keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
			memory->release();
		}
		region->cpu = region->keep ? reinterpret_cast<volatile uint32_t *>(region->keep->getVirtualAddress()) : nullptr;
	}
	if (region->cpu == nullptr || region->phys == 0) {
		region->cpu = nullptr;                  /* a half-built region must not look valid */
		region->phys = 0;
		region->bytes = 0;
		region->desc = nullptr;                 /* so a re-run retries the allocation */
		desc->complete(kIODirectionOutIn);
		desc->release();
		return 0;
	}
	region->desc = desc;                        /* deliberately kept: the GMU walks this memory */
	return 1;
}

static void
dbyGmuRegionClean(DbyGmuRegion *region)
{
	uintptr_t at;
	uintptr_t end;

	if (region->cpu == nullptr || region->bytes == 0) {
		return;
	}
	/* Defensive only: the CPU view is now the uncached mapping (dbyGmuRegionAlloc), so the allocator's
	 * own cacheable view was already cleaned once and no later store goes through a cacheable alias.
	 * Kept because a stray dirty line from that allocation would evict over the GMU's memory, and this
	 * is the only place that would still catch it.  dsb sy, dc civac by VA, dsb sy. */
	__asm__ volatile("dsb sy" ::: "memory");
	for (at = (uintptr_t)region->cpu, end = at + region->bytes; at < end; at += 64u) {
		__asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
dbyGmuRegionsBuild(void)
{
	unsigned ok = 0;

	ok += dbyGmuRegionAlloc(&gDbyGmuRegions[0], DBY_GMU_ICACHE_BYTES, 0x00004000ull);  /* icache */
	ok += dbyGmuRegionAlloc(&gDbyGmuRegions[1], DBY_GMU_DUMMY_BYTES, 0x60000000ull);   /* dummy  */
	ok += dbyGmuRegionAlloc(&gDbyGmuRegions[2], DBY_GMU_LOG_BYTES, 0x70000000ull);     /* log    */
	ok += dbyGmuRegionAlloc(&gDbyGmuRegions[3], DBY_GMU_HFI_BYTES, 0x70010000ull);     /* hfi    */
	{ unsigned i; for (i = 0; i < 4; i++) { dbyGmuRegionClean(&gDbyGmuRegions[i]); } }
	gDbyGmuRegionsOk = ok;
}

/* The 16-bit level lists the command DB parse produced, fed to the kit's own table builder. */
static void
dbyGmuStartupBuild(void)
{
	A650GpuDbResult db;
	A650GpuDbStatus st;
	AdrenoHfiStartupStatus ss;

	gDbyStartupRan = 1;
	gDbyStartupStatus = 0xFFFFFFFFu;
	memset(&db, 0, sizeof db);
	if (dbyAopMap() == 0) {
		return;
	}
	st = A650GpuCommandDbParse(reinterpret_cast<const uint8_t *>(gAopDbBase), DBY_AOP_CMD_DB_SIZE, &db);
	gDbyStartupStatus = (unsigned)st;
	if (st != A650GpuDbSuccess) {
		return;
	}
	/* The kit needs a DDR vote before the startup table or its bandwidth list collapses to one entry
	 * (A650Gmu.c:382, A650Gmu.h:86). */
	A650GmuDdrVote = 1u;
	memset(&gDbyStartupCfg, 0, sizeof gDbyStartupCfg);
	ss = A650GmuBuildStartup(db.gfx, db.gfx_bytes, db.mx, db.mx_bytes, db.cx, db.cx_bytes,
		                         db.mc0, db.sh0, db.acv, db.cn0, &gDbyStartupCfg);
	gDbyStartupStatus = (unsigned)ss;
}

static void
renderGmuRegions(void)
{
	char *row2 = native_gpupower_rows[2];
	char *row3 = native_gpupower_rows[3];
	const char *end2 = row2 + 119;
	const char *end3 = row3 + 119;
	char *at = row2;

	at = putStr(at, end2, "GPU GMUREG ok=");
	at = putDec(at, end2, gDbyGmuRegionsOk);
	at = putStr(at, end2, "/4 cache=");
	at = putHex8(at, end2, (uint32_t)(gDbyGmuRegions[0].phys >> 32));
	at = putHex8(at, end2, (uint32_t)(gDbyGmuRegions[0].phys & 0xFFFFFFFFu));
	at = putStr(at, end2, "@");
	at = putHex8(at, end2, (uint32_t)gDbyGmuRegions[0].iova);
	at = putStr(at, end2, " hfi=");
	at = putHex8(at, end2, (uint32_t)(gDbyGmuRegions[3].phys >> 32));
	at = putHex8(at, end2, (uint32_t)(gDbyGmuRegions[3].phys & 0xFFFFFFFFu));
	at = putStr(at, end2, "@");
	at = putHex8(at, end2, (uint32_t)gDbyGmuRegions[3].iova);
	*at = 0;

	at = row3;
	at = putStr(at, end3, "GPU HFI startup st=");
	at = putHex8(at, end3, gDbyStartupStatus);
	if (gDbyStartupStatus == 0u) {
		at = putStr(at, end3, " tables=ok");
	} else {
		at = putStr(at, end3, " (0=ok 1=corrupt 2=missing db; kit enum otherwise)");
	}
	*at = 0;
}

/* --- round 2c: the GMU's own SMMU context: bank 1, ASID 2, streams 4 and 5 ----------------------
 *
 * A650Gmu.h:9-10 is this block's contract from the other side: no clock, power or SMMU action happens in
 * the cold start, because "the caller has CX on, the GMU clocks running and the GMU's own SMMU context
 * mapping the four memory regions".  Mode 7 built the table pool and normalised slots 4/5 to FAULT, mode 8
 * allocated the four regions; this block turns that into an enabled context and only then routes the two
 * GMU streams to it.  No cold start here.
 *
 * WHY ArmSmmu4KBuild AND NOT A LIVE ARENA
 *   Build writes only the 13 pages the four ranges need and needs no arena, no prepared state and no 900-page
 *   pool: the pool stays the 16 pages mode 7 allocates.  Its descriptors are the same expression the live
 *   updater uses (ArmSmmu4K.c:71-87 vs ArmSmmu4KLive.c:36-44), so the mapping is identical.
 *   Mixing the two is what does not work: ArmSmmu4KLiveUpdateContextBatch goes update_context() ->
 *   context_metadata() -> region_metadata() (ArmSmmu4KLive.c:527-539, :331-347), and region_metadata
 *   refuses unless the state IS a live arena's - root == &state->tables, state->prepared (set only by
 *   LivePrepare, :282), same_config(), and table_pages equal to layout()'s fixed-aperture count (:341-342,
 *   re-checked by update() at :396-402).  For this aperture [0, 0x70014000) at 36 bits layout() counts
 *   1 + 2 + 897 = 900 pages, because the dummy region at 0x60000000 is 1.5 GiB from the icache region at
 *   0x4000 (A650Gmu.c:37-38).  A Build result is 13 pages, so it cannot feed that call.
 *   Build performs no cache maintenance (ArmSmmu4K.h:40-48), so stage 1 publishes every page it wrote with
 *   dbyCtxPublish before the bank may walk them.
 *
 * SMR MASK, AND WHY STAGE 6 WRITES THE SMR ITSELF
 *   kona-gpu.dtsi:340,346 give each GMU stream the two-cell specifier <&kgsl_smmu 0x4 0x400>.  The second
 *   cell is the SMR.MASK: arm-smmu.c:7179-7184 reads qcom,stream-id-range from the TBU nodes, and mainline's
 *   arm_smmu_of_xlate encodes args[1] into ARM_SMMU_SMR_MASK, because bit 10 of the SID says which GPU TBU
 *   carried the access - gfx_0_tbu covers 0x000-0x3FF and gfx_1_tbu 0x400-0x7FF (msm-arm-smmu-kona.dtsi:54-68),
 *   and the same SMMU's qcom,actlr gives SIDs 2/4/5/7 the mask 0x400 (:46-52).  The kit's ArmSmmu4KStream has
 *   no mask field (ArmSmmu4KStreams.h:8) and writes MASK=0 (valid_smr() is V only, ArmSmmu4KStreams.c:9,136),
 *   so an exact SID 4 route would miss every gfx_1_tbu transaction of the same client.  Stage 6 ORs the DT's
 *   mask into the two SMRs the kit just installed and reads them back; SID and V are untouched.
 *   Stated openly: the kit's own ArmSmmu4KObserveStreams would now reject those SMRs (ArmSmmu4KStreams.c:111),
 *   so the final observation is ArmSmmu4KObserveOwnedContext, which reads the context and its fault status
 *   but not the SMRs.  Which SID the GMU actually presents is still unproven - that only a real GMU
 *   transaction can settle, and sGFSR/sGFSYNR1 are rendered for exactly that: with USFCFG set, an unmatched
 *   stream raises a global fault whose syndrome names it (mainline arm-smmu.c prints gfsynr1 as the SID).
 *   A 0x404/0x405 transaction now matches instead of faulting; there is no claim here that it does not exist.
 *
 * ASID: BANK 1 OWNS ASID 2, NOT 1
 *   The GPU SMMU node carries qcom,no-dynamic-asid (msm-arm-smmu-kona.dtsi:12) and arm-smmu.c:2414-2415 then
 *   sets cfg->asid = cfg->cbndx + 1, so bank 0 owns ASID 1 and this bank owns 2.  Even with dynamic ASIDs the
 *   idr starts at num_context_banks + 2 (arm-smmu.c:2418-2420), so 2 is never handed out either; under
 *   either policy it is bank 1's own and cannot alias bank 0's.
 *
 * sCR0: WRITE ONLY WHAT CHANGES, ROLL BACK ON FAILURE
 *   The read-modify-write is skipped when the register already reads (scr0 & 0x407) == 0x406, and if a later
 *   stage fails the pre-boot word is written back and re-read (row 1, rst=).  Bits outside 0x407 are
 *   preserved, including bit 3 (EXIDENABLE), which the kit pairs with ID0[8] (ArmSmmu4KStreams.c:22): the
 *   kit refuses the block when they disagree (StateMismatch, 7) and this probe does not change EXIDS
 *   matching for every stream on the block.  sACR is read and reported, never written: CACHE_LOCK (bit 26)
 *   may only be cleared when the secure copy permits it (vendor arm-smmu.c:7117-7127).
 *
 * STAGES, one status each on rows 2 and 3; the first non-zero status stops the chain:
 *   0 sCR0 global configuration - values on row 1, status as gbl= on row 2
 *   1 the four ranges built into the pool and published
 *   2 context image encoded
 *   3 the two routes and bank 1 checked read-only (ArmSmmu4KCheckFaultedStreams)
 *   4 bank 1 programmed and enabled
 *   5 streams 4 and 5 attached
 *   6 the DT's SMR mask applied and read back
 *   7 TLBIASID + TLBSYNC of bank 1
 *   8 the nine context registers observed
 *   9 that image compared with the expected one
 */
#define DBY_GMU_CTX_BANK 1u
/* bank N owns ASID N+1 under qcom,no-dynamic-asid (arm-smmu.c:2414-2415, msm-arm-smmu-kona.dtsi:12) */
#define DBY_GMU_CTX_ASID (DBY_GMU_CTX_BANK + 1u)
#define DBY_GMU_CTX_SMR_MASK 0x400u          /* kona-gpu.dtsi:340,346; gfx_1_tbu's SID base */
#define DBY_SMMU_LEAF_BUDGET 4116u           /* 4092 + 16 + 4 + 4 leaves, ArmSmmu4K.c:48 */
#define DBY_GMU_CTX_NOT_RUN 0xFFFFFFFFu
/* 0x100 and up are this probe's own failures; 0..11 are ArmSmmu4KStatus (ArmSmmu4K.h:10-14). */
#define DBY_GMU_CTX_NOANSWER 0x100u
#define DBY_GMU_CTX_SCR0     0x102u
#define DBY_GMU_CTX_PUBLISH  0x103u
#define DBY_GMU_CTX_SMR      0x104u

/* A650_SMMU_GFSR (0x048) already comes from A650GmuPower.h:96, so only its syndrome register is added
 * here (vendor arm-smmu-regs.h:56-59: sGFSR 0x48, sGFSYNR0 0x50, sGFSYNR1 0x54, sGFSYNR2 0x58). */
#define A650_SMMU_GFSYNR1 0x054u

/* Permanent: once this function returns the GMU walks these tables, so the io object and the context
 * description stay here for the life of the boot. */
static ArmSmmu4KIo gDbyCtxIo;
static ArmSmmu4KConfig gDbyCtxConfig;
static ArmSmmu4KResult gDbyCtxBuild;
static ArmSmmu4KContextConfig gDbyCtxContext;
static uint32_t gDbyCtxStage[10];
static unsigned gDbyCtxStep;
static uint32_t gDbySmmuScr0Before = DBY_GMU_CTX_NOT_RUN;
static uint32_t gDbySmmuScr0After = DBY_GMU_CTX_NOT_RUN;
static uint32_t gDbySmmuSacr;
static uint32_t gDbySmmuGfsr, gDbySmmuGfSid, gDbySmmuSfsr;
static uint64_t gDbySmmuTtbr0;
static unsigned gDbySmmuGblOk, gDbySmmuScr0Changed, gDbySmmuScr0Rst;
static unsigned gDbySmmuCtxEnabled, gDbySmmuStreamsVerified;

/* One range: iova is fixed by the kit (A650Gmu.c:111-112), physical is the region mode 8 allocated.
 * The icache region is executed by the GMU and written only by the CPU, so Read|Execute; the other three
 * are read and written.  writeback 0 (Normal non-cacheable) and shareability 2 (outer) are the DEVICE's
 * stage-1 view of these regions; the CPU's view is the single uncached mapping dbyGmuRegionAlloc makes
 * (kIOMapInhibitCache), so the two sides agree and the kit's dsb is enough for the GMU to see the copy.
 * A650Gmu.h:38 describes the device side; ArmSmmu4K.h:25-27 is the attribute encoding. */
static void
dbyCtxRange(ArmSmmu4KRange *range, uint64_t iova, uint64_t physical, uint64_t bytes,
	unsigned permissions)
{
	range->iova = iova;
	range->physical = physical & 0xFFFFFFFFFFFFF000ull;
	range->bytes = bytes;
	range->permissions = permissions;
	range->writeback = 0u;
	range->shareability = 2u;
}

/* Publish one 4 KiB table page: dsb sy, dc civac per line, dsb sy.  Visits by VA on the CPU's normal
 * mapping of the pool, which is the memory config.table_physical names. */
static int
dbyCtxPublish(volatile uint64_t *page, size_t bytes)
{
	uintptr_t at, end;

	if (page == nullptr || bytes != 4096u || (((uintptr_t)page) & 4095u) != 0u) {
		return 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (at = (uintptr_t)page, end = at + bytes; at < end; at += 64u) {
		__asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* Every page Build wrote, published before a context may walk them (ArmSmmu4K.h:44-48 makes that the
 * caller's obligation). */
static uint32_t
dbyCtxPublishTables(uint64_t pages)
{
	uint64_t page;

	if (pages == 0u || pages > (uint64_t)(DBY_SMMU_TABLE_POOL_BYTES / 4096u)) {
		return DBY_GMU_CTX_PUBLISH;
	}
	for (page = 0; page < pages; page++) {
		if (dbyCtxPublish((volatile uint64_t *)gDbySmmuPoolCpu + page * 512u, 4096u) != 0) {
			return DBY_GMU_CTX_PUBLISH;
		}
	}
	return 0u;
}

/* ArmSmmu4KIo for the SMMU window, over the same window-bound callbacks the rest of the probe uses.  The
 * kit's read_image() reads TTBR0/TTBR1 with the 64-bit callback (ArmSmmu4KProgram.c:12), which reaches
 * dbyGpuRead64/dbyGpuWrite64 - one aligned access each, see there.  ArmSmmu4KBindMmio would also have
 * accepted this mapping (4 KiB-aligned VA, 0x10000 bytes >= 0x2c, ArmSmmu4KMmio.c:22), but it cannot bound
 * an offset against the DTS window size the way these callbacks do, so the object is filled by hand. */
struct DbySmmuWindow {
	DbyGpuState *state;
	unsigned window;
};

static struct DbySmmuWindow gDbySmmuWindow = { &gDbyGpu, A650GpuWinSmmu };

static int
dbySmmuRead32(void *cookie, size_t offset, uint32_t *out)
{
	struct DbySmmuWindow *window = static_cast<struct DbySmmuWindow *>(cookie);

	return dbyGpuRead32(window->state, window->window, offset, out);
}

static int
dbySmmuRead64(void *cookie, size_t offset, uint64_t *out)
{
	struct DbySmmuWindow *window = static_cast<struct DbySmmuWindow *>(cookie);

	return dbyGpuRead64(window->state, window->window, offset, out);
}

static int
dbySmmuWrite32(void *cookie, size_t offset, uint32_t value)
{
	struct DbySmmuWindow *window = static_cast<struct DbySmmuWindow *>(cookie);

	return dbyGpuWrite32(window->state, window->window, offset, value);
}

static int
dbySmmuWrite64(void *cookie, size_t offset, uint64_t value)
{
	struct DbySmmuWindow *window = static_cast<struct DbySmmuWindow *>(cookie);

	return dbyGpuWrite64(window->state, window->window, offset, value);
}

static int
dbySmmuBarrier(void *cookie)
{
	(void)cookie;
	return dbyGpuBarrier(nullptr);
}

/* sCR0 has to read (scr0 & 0x407) == 0x406 before the kit will touch a bank or a stream:
 * ArmSmmu4KStreams.c:18-22 requires CLIENTPD clear (bit 0), GFRE and GFIE set (bits 1, 2) and USFCFG set
 * (bit 10).  The read-modify-write keeps every bit outside 0x407 - bit 3 (EXIDENABLE) included, because the
 * kit pairs it with ID0[8] and this probe must not change EXIDS matching for every stream on the block; a
 * disagreement is refused by the kit (7) at stage 3.  0x002f0406 is what an Adreno SMMU has been observed to
 * read (ArmSmmu4KStreams.c:20), so a block that is already right is not written at all.  sACR is read and
 * reported, never written: CACHE_LOCK (bit 26) may only be cleared when the secure copy permits it
 * (vendor arm-smmu.c:7117-7127), which nothing here can prove for this part. */
static void
dbySmmuGlobalEnable(void)
{
	uint32_t before = 0;
	uint32_t after = 0;
	uint32_t sacr = 0;
	uint32_t want;

	gDbySmmuGblOk = 0u;
	gDbySmmuScr0Changed = 0u;
	gDbySmmuScr0Before = DBY_GMU_CTX_NOT_RUN;
	gDbySmmuScr0After = DBY_GMU_CTX_NOT_RUN;
	gDbySmmuSacr = 0u;

	if (gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_SCR0, &before) != 0 ||
		before == 0xFFFFFFFFu) {
		return;
	}
	gDbySmmuScr0Before = before;
	if (gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_SACR, &sacr) != 0) {
		sacr = 0u;
	}
	gDbySmmuSacr = sacr;
	want = (before & ~0x407u) | 0x406u;
	/* Write only when the block does not already read the required value. */
	if ((before & 0x407u) != 0x406u) {
		if (gDbyCtxIo.write32(gDbyCtxIo.cookie, A650_SMMU_SCR0, want) != 0 ||
			gDbyCtxIo.barrier(gDbyCtxIo.cookie) != 0) {
			return;
		}
		gDbySmmuScr0Changed = 1u;
	}
	if (gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_SCR0, &after) != 0) {
		return;
	}
	gDbySmmuScr0After = after;
	gDbySmmuGblOk = ((after & 0x407u) == 0x406u) ? 1u : 0u;
}

/* The DT's SMR.MASK, applied to the two SMRs ArmSmmu4KAttachStreams just wrote with mask 0 (Streams.c:136
 * writes valid_smr()|sid, i.e. no mask, because ArmSmmu4KStream has no field for one - Streams.h:8).  Only
 * the MASK field [30:16] changes: SID [15:0] and V [31] are read back identical.  See the block comment for
 * why bit 10 must be a wildcard and for what this costs (the kit's stream observation no longer matches). */
static uint32_t
dbySmmuSmrMask(const ArmSmmu4KStream *streams, unsigned count)
{
	const uint32_t mask = DBY_GMU_CTX_SMR_MASK << 16;
	unsigned i;

	for (i = 0u; i < count; i++) {
		size_t at = 0x800u + 4u * streams[i].slot;
		uint32_t value = 0;

		if (gDbyCtxIo.read32(gDbyCtxIo.cookie, at, &value) != 0 ||
			(value & 0xFFFFu) != (uint32_t)streams[i].sid) {
			return DBY_GMU_CTX_SMR;
		}
		if ((value & mask) == mask) {
			continue;
		}
		if (gDbyCtxIo.write32(gDbyCtxIo.cookie, at, value | mask) != 0 ||
			gDbyCtxIo.barrier(gDbyCtxIo.cookie) != 0) {
			return DBY_GMU_CTX_SMR;
		}
		value = 0;
		if (gDbyCtxIo.read32(gDbyCtxIo.cookie, at, &value) != 0 ||
			(value & mask) != mask || (value & 0xFFFFu) != (uint32_t)streams[i].sid) {
			return DBY_GMU_CTX_SMR;
		}
	}
	return 0u;
}

/* TTBR0 is a 64-bit register: the ASID in the top half and the table root in the low half. */
static char *
putHex16(char *o, const char *end, uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	for (shift = 60; shift >= 0 && o < end; shift -= 4) {
		*o++ = digits[(v >> shift) & 0xFu];
	}
	return o;
}

static char *
putStage(char *o, const char *end, const char *name, uint32_t status)
{
	o = putStr(o, end, " ");
	o = putStr(o, end, name);
	o = putStr(o, end, "=");
	if (status == DBY_GMU_CTX_NOT_RUN) {
		return putStr(o, end, "ff");
	}
	return putDec(o, end, status);
}

/* A stage that ran and failed carries a non-zero code; one that was never reached still carries NOT_RUN. */
static unsigned
dbyCtxFailed(void)
{
	unsigned i;

	for (i = 1u; i < 10u; i++) {
		if (gDbyCtxStage[i] != 0u && gDbyCtxStage[i] != DBY_GMU_CTX_NOT_RUN) {
			return 1u;
		}
	}
	return 0u;
}

/* Row 1: the sCR0 step on its own row (mode 9 replaces the clock row).  Rows 2 and 3: one status per stage.
 * Row 4 is filled by renderTail (the four region physicals, mode 9 only).  Nothing here is rendered on the
 * gate paths - those print their own refusal in row 2. */
static void
renderSmmuContext(void)
{
	char *row = native_gpupower_rows[1];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU SMMU GBL before=");
	at = putHex8(at, end, gDbySmmuScr0Before);
	at = putStr(at, end, " after=");
	at = putHex8(at, end, gDbySmmuScr0After);
	at = putStr(at, end, " ok=");
	at = putDec(at, end, gDbySmmuGblOk);
	at = putStr(at, end, " sACR=");
	at = putHex8(at, end, gDbySmmuSacr);
	at = putStr(at, end, " rst=");
	at = putDec(at, end, gDbySmmuScr0Rst);
	*at = 0;

	row = native_gpupower_rows[2];
	end = row + 119;
	at = putStr(row, end, "GPU SMMUCTX");
	at = putStage(at, end, "gbl", gDbyCtxStage[0]);
	at = putStage(at, end, "bld", gDbyCtxStage[1]);
	at = putStage(at, end, "enc", gDbyCtxStage[2]);
	at = putStage(at, end, "pre", gDbyCtxStage[3]);
	at = putStage(at, end, "prog", gDbyCtxStage[4]);
	at = putStr(at, end, " gf=");
	at = putHex8(at, end, gDbySmmuGfsr);
	*at = 0;

	row = native_gpupower_rows[3];
	end = row + 119;
	at = putStr(row, end, "GPU SMMUCTX");
	at = putStage(at, end, "str", gDbyCtxStage[5]);
	at = putStage(at, end, "smr", gDbyCtxStage[6]);
	at = putStage(at, end, "tlb", gDbyCtxStage[7]);
	at = putStage(at, end, "obs", gDbyCtxStage[8]);
	at = putStage(at, end, "val", gDbyCtxStage[9]);
	at = putStr(at, end, " stv=");
	at = putDec(at, end, gDbySmmuStreamsVerified);
	at = putStr(at, end, " en=");
	at = putDec(at, end, gDbySmmuCtxEnabled);
	at = putStr(at, end, " step=");
	at = putDec(at, end, gDbyCtxStep);
	at = putStr(at, end, " sFSR=");
	at = putHex8(at, end, gDbySmmuSfsr);
	at = putStr(at, end, " gnr1=");
	at = putHex8(at, end, gDbySmmuGfSid);
	*at = 0;
}

static void
dbyGpuSmmuContextEncode(uint32_t id0, uint32_t id1, uint32_t id2)
{
	ArmSmmu4KRange ranges[4];
	ArmSmmu4KStreamState streams;
	ArmSmmu4KStreamResult sr;
	ArmSmmu4KProgramResult prog;
	ArmSmmu4KProgramResult obs;
	ArmSmmu4KProgramResult tlb;
	ArmSmmu4KContextImage image;
	ArmSmmu4KStream slots[2];
	const char *why;
	char *row = native_gpupower_rows[2];
	const char *end = row + 119;
	char *at;

	memset(gDbyCtxStage, 0xFF, sizeof gDbyCtxStage);
	gDbyCtxStep = 0u;
	gDbySmmuCtxEnabled = 0u;
	gDbySmmuStreamsVerified = 0u;
	gDbySmmuTtbr0 = 0u;
	gDbySmmuSfsr = 0u;
	gDbySmmuGfsr = 0u;
	gDbySmmuGfSid = 0u;
	gDbySmmuScr0Changed = 0u;
	gDbySmmuScr0Rst = 0u;

	/* The gate.  This mode writes SMMU state, so it refuses to start unless the mode 3 power-up and the
	 * two earlier rounds left exactly the objects it needs.  The three counts are produced by mode 7's prep
	 * (which does write the two S2CR slots to their canonical FAULT word, as mode 7 does on its own) and by
	 * mode 8's region allocation; from this line on, no register is written and no table exists unless all
	 * three are right. */
	if (gDbySmmuSlotsFixed != 2u || gDbyGmuRegionsOk != 4u || gDbySmmuPoolOk == 0u) {
		at = putStr(row, end, "GPU SMMUCTX not run: slots=");
		at = putDec(at, end, gDbySmmuSlotsFixed);
		at = putStr(at, end, " regions=");
		at = putDec(at, end, gDbyGmuRegionsOk);
		at = putStr(at, end, " pool=");
		at = putDec(at, end, gDbySmmuPoolOk);
		*at = 0;
		native_gpupower_rows[3][0] = 0;
		return;
	}
	why = dbyGpuSmmuReject(id0, id1, id2);
	if (why != nullptr) {
		/* renderSmmu() refuses a verdict on such an ID block; a context write is refused harder. */
		at = putStr(row, end, "GPU SMMUCTX not run: smmu=");
		at = putStr(at, end, why);
		*at = 0;
		native_gpupower_rows[3][0] = 0;
		return;
	}
	if (gDbySmmuPoolCpu == nullptr || (((uintptr_t)gDbySmmuPoolCpu) & 4095u) != 0u) {
		/* The table array must be 4 KiB-aligned and it is what Build writes through (ArmSmmu4K.h:46-47). */
		at = putStr(row, end, "GPU SMMUCTX not run: pool cpu=");
		at = putHex8(at, end, (uint32_t)((uintptr_t)gDbySmmuPoolCpu & 0xFFFFFFFFu));
		*at = 0;
		native_gpupower_rows[3][0] = 0;
		return;
	}

	memset(&gDbyCtxIo, 0, sizeof gDbyCtxIo);
	gDbyCtxIo.cookie = &gDbySmmuWindow;
	gDbyCtxIo.register_bytes = (size_t)kDbyGpuWindows[A650GpuWinSmmu].size;
	gDbyCtxIo.read32 = dbySmmuRead32;
	gDbyCtxIo.read64 = dbySmmuRead64;
	gDbyCtxIo.write32 = dbySmmuWrite32;
	gDbyCtxIo.write64 = dbySmmuWrite64;
	gDbyCtxIo.barrier = dbySmmuBarrier;

	/* stage 0: sCR0 first, before a bank, a stream or a table is touched */
	gDbyCtxStep = 0u;
	dbySmmuGlobalEnable();
	if (gDbySmmuGblOk != 0u) {
		gDbyCtxStage[0] = 0u;
	} else if (gDbySmmuScr0Before == DBY_GMU_CTX_NOT_RUN) {
		gDbyCtxStage[0] = DBY_GMU_CTX_NOANSWER;
	} else {
		gDbyCtxStage[0] = DBY_GMU_CTX_SCR0;
	}
	if (gDbyCtxStage[0] != 0u) {
		goto render;
	}

	/* stage 1: the tables.  Build validates every input and the whole leaf/table budget before it writes
	 * (ArmSmmu4K.h:43-48); the 13 pages it does write are then published for the walker. */
	gDbyCtxConfig.input_bits = 36u;      /* ID2[11:8] = 1, so 36 <= UBS (ArmSmmu4KContext.c:34,37) */
	gDbyCtxConfig.output_bits = 36u;     /* ID2[7:4] = 1, so OAS 36 is what the block offers (:38) */
	gDbyCtxConfig.table_physical = gDbySmmuPoolPhys;
	gDbyCtxConfig.leaf_budget = DBY_SMMU_LEAF_BUDGET;
	/* E1 (mode 12, 2026-11-04) read CB1 sFSR=0x408 and sFAR=0x00157398: a PERMISSION fault at an
	 * address inside this very range (vendor arm-smmu-regs.h:251 FSR_PF = 1<<3).  The GMU writes
	 * there, and the range was mapped read-only (5 = Read|Execute).  The reference board maps the
	 * GMU cache region R|W|X|Privileged (NativeAdrenoGmuMemory.cpp:54), so 15 is the value the
	 * firmware expects. */
	dbyCtxRange(&ranges[0], 0x00004000ull, gDbyGmuRegions[0].phys, DBY_GMU_ICACHE_BYTES, 15u);
	dbyCtxRange(&ranges[1], 0x60000000ull, gDbyGmuRegions[1].phys, DBY_GMU_DUMMY_BYTES, 3u);
	dbyCtxRange(&ranges[2], 0x70000000ull, gDbyGmuRegions[2].phys, DBY_GMU_LOG_BYTES, 3u);
	dbyCtxRange(&ranges[3], 0x70010000ull, gDbyGmuRegions[3].phys, DBY_GMU_HFI_BYTES, 3u);
	gDbyCtxStep = 1u;
	gDbyCtxStage[1] = (uint32_t)ArmSmmu4KBuild(&gDbyCtxConfig, ranges, 4u,
		(uint64_t *)gDbySmmuPoolCpu, (size_t)(DBY_SMMU_TABLE_POOL_BYTES / 4096u), &gDbyCtxBuild);
	if (gDbyCtxStage[1] != 0u) {
		goto render;
	}
	gDbyCtxStage[1] = dbyCtxPublishTables((uint64_t)gDbyCtxBuild.table_pages);
	if (gDbyCtxStage[1] != 0u) {
		goto render;
	}

	/* stage 2: the expected image, bank 1 / ASID 2 (ArmSmmu4KContext.c:44-61) */
	gDbyCtxContext.lower = &gDbyCtxConfig;
	gDbyCtxContext.upper = nullptr;
	gDbyCtxContext.lower_tables = &gDbyCtxBuild;
	gDbyCtxContext.upper_tables = nullptr;
	gDbyCtxContext.id0 = id0;
	gDbyCtxContext.id2 = id2;
	gDbyCtxContext.asid = DBY_GMU_CTX_ASID;
	gDbyCtxContext.coherent_walk = 1u;
	gDbyCtxContext.stall_on_fault = 0u;
	gDbyCtxContext.continue_after_fault = 1u;
	gDbyCtxStep = 2u;
	gDbyCtxStage[2] = (uint32_t)ArmSmmu4KEncodeContext(&gDbyCtxContext, &image);
	if (gDbyCtxStage[2] != 0u) {
		goto render;
	}

	/* stage 3: read-only preflight of the two routes and of bank 1, BEFORE the bank is enabled.
	 * ArmSmmu4KAttachStreams documents this position (ArmSmmu4KStreams.h:19-21: "Intended before
	 * programming a disabled context under the same global lease"): both target S2CRs must read FAULT, the
	 * global fault must be clear, and no other live SMR may already route at this bank
	 * (ArmSmmu4KStreams.c:43-50).  Without it a foreign route at bank 1 would surface only at the attach,
	 * after this block had enabled the bank with its own tables - and that master would then translate
	 * through the GMU's. */
	slots[0].slot = DBY_GMU_SLOT_FIRST;
	slots[0].sid = 4u;
	slots[1].slot = DBY_GMU_SLOT_LAST;
	slots[1].sid = 5u;
	memset(&sr, 0, sizeof sr);
	gDbyCtxStep = 3u;
	gDbyCtxStage[3] = (uint32_t)ArmSmmu4KCheckFaultedStreams(&gDbyCtxIo, slots, 2u,
		DBY_GMU_CTX_BANK, &sr);
	if (gDbyCtxStage[3] != 0u) {
		goto render;
	}

	/* stage 4: bank 1.  Writes TCR2/TCR/TTBR0/TTBR1/MAIR/CBAR/CBA2R while SCTLR.M is 0, verifies that
	 * image, and only then enables the bank (ArmSmmu4KProgram.c:96-119).  An already enabled bank is
	 * refused (ArmSmmu4KProgram.c:94), which is the right answer: this block owns bank 1 only if it was
	 * off.  The kit re-reads ID0/ID1/ID2 itself and overrides the config's copies (:72-84). */
	gDbyCtxStep = 4u;
	gDbyCtxStage[4] = (uint32_t)ArmSmmu4KProgramDisabledContext(&gDbyCtxIo, DBY_GMU_CTX_BANK,
		&gDbyCtxContext, 1000u, &prog);
	if (gDbyCtxStage[4] != 0u) {
		goto render;
	}
	gDbySmmuCtxEnabled = prog.enabled;
	gDbySmmuSfsr = prog.fault_status;

	/* stage 5: the two GMU streams.  They must read FAULT first, which mode 7 established
	 * (ArmSmmu4KStreams.c:43-44); the kit writes S2CR before SMR (:130-139). */
	memset(&streams, 0, sizeof streams);
	gDbyCtxStep = 5u;
	gDbyCtxStage[5] = (uint32_t)ArmSmmu4KAttachStreams(&gDbyCtxIo, &streams, slots, 2u,
		DBY_GMU_CTX_BANK, &gDbyCtxContext, &sr);
	if (gDbyCtxStage[5] != 0u) {
		goto render;
	}
	gDbySmmuStreamsVerified = sr.streams_verified;

	/* stage 6: the DT's SMR mask (kona-gpu.dtsi:340,346), which the kit's stream type cannot carry */
	gDbyCtxStep = 6u;
	gDbyCtxStage[6] = dbySmmuSmrMask(slots, 2u);
	if (gDbyCtxStage[6] != 0u) {
		goto render;
	}

	/* stage 7: TLBIASID(2) and the bounded TLBSYNC of bank 1 (ArmSmmu4KProgram.c:26-44).  TLBIASID and
	 * not TLBIALL: this block owns ASID 2, and the bank was disabled before stage 4, so no other ASID can
	 * hold a cached translation in it. */
	gDbyCtxStep = 7u;
	gDbyCtxStage[7] = (uint32_t)ArmSmmu4KSyncContextTlb(&gDbyCtxIo, prog.context_offset,
		DBY_GMU_CTX_ASID, 0u, 1000u, &tlb);
	if (gDbyCtxStage[7] != 0u) {
		goto render;
	}

	/* stage 8 and 9: read the nine registers back and compare them with the image encoded in stage 2 */
	gDbyCtxStep = 8u;
	gDbyCtxStage[8] = (uint32_t)ArmSmmu4KObserveOwnedContext(&gDbyCtxIo, DBY_GMU_CTX_BANK,
		&gDbyCtxContext, &obs);
	if (gDbyCtxStage[8] != 0u) {
		goto render;
	}
	gDbySmmuTtbr0 = obs.readback.ttbr0;
	gDbySmmuSfsr = obs.fault_status;
	gDbyCtxStep = 9u;
	gDbyCtxStage[9] = (uint32_t)ArmSmmu4KValidateContextReadback(&gDbyCtxContext, &obs.readback);

render:
	/* Roll the one global write back if anything after it failed - including a write whose read-back
	 * never confirmed the required value (gDbySmmuGblOk == 0, stage 0): the SMMU is left as it was found. */
	if (gDbySmmuScr0Changed != 0u && (dbyCtxFailed() != 0u || gDbySmmuGblOk == 0u)) {
		uint32_t now = 0;

		gDbySmmuScr0Rst = 2u;
		if (gDbyCtxIo.write32(gDbyCtxIo.cookie, A650_SMMU_SCR0, gDbySmmuScr0Before) == 0 &&
			gDbyCtxIo.barrier(gDbyCtxIo.cookie) == 0 &&
			gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_SCR0, &now) == 0 &&
			now == gDbySmmuScr0Before) {
			gDbySmmuScr0Rst = 1u;
		}
	}
	/* sGFSR: a global fault left by the bootloader is what makes a preflight 11 (Fault) unattributable, and
	 * an unmatched stream (USFCFG set) is what a SID the SMRs do not cover would look like.  sGFSYNR1[15:0]
	 * is the SID mainline prints for that case (arm-smmu.c, ARM_SMMU_GR0_sGFSYNR1 = 0x54). */
	{
		uint32_t gf = 0;
		uint32_t gnr1 = 0;

		(void)gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_GFSR, &gf);
		(void)gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_GFSYNR1, &gnr1);
		gDbySmmuGfsr = gf;
		gDbySmmuGfSid = gnr1;
	}
	renderSmmuContext();
}

static void
renderSmmuConfig(void)
{
	char *row = native_gpupower_rows[3];
	const char *end = row + 119;
	char *at = row;
	uint32_t scr0 = 0, sacr = 0, s2cr0 = 0, s2cr5 = 0;
	int e0, e1, e2, e3;

	e0 = dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_SCR0, &scr0);
	e1 = dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_SACR, &sacr);
	e2 = dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_S2CR_BASE + 0u * 4u, &s2cr0);
	e3 = dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_S2CR_BASE + 5u * 4u, &s2cr5);

	at = putStr(at, end, "GPU SMMU cfg cb-errors=");
	at = putDec(at, end, (unsigned)((e0 != 0) + (e1 != 0) + (e2 != 0) + (e3 != 0)));
	at = putStr(at, end, " scr0=");
	at = putHex8(at, end, scr0);
	at = putStr(at, end, " sACR=");
	at = putHex8(at, end, sacr);
	at = putStr(at, end, " raw sACR lock=");
	at = putStr(at, end, (sacr & A650_SMMU_ACR_CACHE_LOCK) != 0u ? "Y" : "N");
	at = putStr(at, end, " slot0=");
	at = putStr(at, end, dbyGpuS2crType(s2cr0));
	at = putStr(at, end, " slot5=");
	at = putStr(at, end, dbyGpuS2crType(s2cr5));
	*at = 0;
}

static void
renderTail(unsigned mode, const A650GpuResult *r)
{
	char *row = native_gpupower_rows[4];
	const char *end = row + 119;
	char *at = row;

	/* Mode 10 owns rows 4 and 5 itself (renderCold): they carry the cold-start verdict and the HFI
	 * acknowledgement stages, which would be lost to the rbbm/note rows this function writes last. */
	if (mode >= 10u) {
		return;
	}

	if (mode == 9u) {
		/* The four region physicals (page numbers, phys >> 12) and the TTBR0 readback, so mode 9 is
		 * diagnosable from its own rows: a wrong region or a root that does not point at the pool is
		 * visible without another mode.  ttbr0 carries the ASID in its top half. */
		at = putStr(at, end, "GPU SMMUCTX mem ic=");
		at = putHex8(at, end, (uint32_t)(gDbyGmuRegions[0].phys >> 12));
		at = putStr(at, end, " du=");
		at = putHex8(at, end, (uint32_t)(gDbyGmuRegions[1].phys >> 12));
		at = putStr(at, end, " lo=");
		at = putHex8(at, end, (uint32_t)(gDbyGmuRegions[2].phys >> 12));
		at = putStr(at, end, " hf=");
		at = putHex8(at, end, (uint32_t)(gDbyGmuRegions[3].phys >> 12));
		at = putStr(at, end, " ttbr0=");
		at = putHex16(at, end, gDbySmmuTtbr0);
		at = putStr(at, end, " asid=");
		at = putDec(at, end, DBY_GMU_CTX_ASID);
	} else if (r->gpu != A650GpuDone) {
		/* Every mode masks the GPU window, so rbbm_status is the memset zero, not a measurement
		 * (review 2026-11-04 03:47, text 8). */
		at = putStr(at, end, "GPU PWR rbbm=n/a (gpu=");
		at = putDec(at, end, r->gpu);
		at = putStr(at, end, ") ioerr=");
		at = putSigned(at, end, r->io_error);
	} else {
		at = putStr(at, end, "GPU PWR rbbm=");
		at = putHex8(at, end, r->rbbm_status);
		at = putStr(at, end, " ioerr=");
		at = putSigned(at, end, r->io_error);
	}
	*at = 0;

	row = native_gpupower_rows[5];
	end = row + 119;
	at = putStr(row, end,
	    mode < 2u ? "GPU PWR note: mode 1 is GCC only; no GPUCC/SMMU/GX access"
	              : mode < 3u ? "GPU PWR note: mode 2 stops after cfg_ahb; CX/SMMU untouched"
	                          : mode == 6u
	                              ? "GPU PWR note: mode 6 = CX + GMU clocks 200MHz + AOP cmd-db"
	                          : mode == 5u
	                              ? "GPU PWR note: mode 5 = CX + clocks, SMMU config read only"
	                          : mode == 4u ? "GPU PWR note: mode 4 = 3a: CX + clocks only, SMMU masked"
	                          : mode == 13u ? "GPU PWR note: mode 13 = 10 + GX power (perf 1)"
	                          : mode == 12u ? "GPU PWR note: mode 12 = 10 + AO/SMMU fault regs + GMU log"
	                          : mode == 11u ? "GPU PWR note: mode 11 = 10 + GBIF halt clear + GMU state"
	                          : mode == 10u ? "GPU PWR note: mode 10 = 9 + GMU cold start"
	                          : mode == 9u ? "GPU PWR note: mode 9 = 3 + GMU SMMU ctx b1/as2 smr-mask 400"
	                          : mode == 8u ? "GPU PWR note: mode 8 = 3 + GMU regions + HFI startup"
	                          : mode == 7u ? "GPU PWR note: mode 7 = 3 + SMMU slots + table pool"
	                          : "GPU PWR note: mode 3 = CX + clocks, then the SMMU read");
	*at = 0;
}

/* ---- mode 10: the GMU cold start ----------------------------------------------------------------
 *
 * The kit's contract (A650Gmu.h:9-10) is that the caller already has CX on, the GMU clocks running
 * and the GMU's own SMMU context mapping the four regions.  Mode 10 runs the mode-9 chain in THIS
 * boot - the regions' physical addresses are this boot's, so a previous boot's context is worthless -
 * and then calls A650GmuColdStart with the startup table mode 8 built from the AOP command DB.
 *
 * Two defects the 2026-11-04 review found in the mode-9 path are fixed here rather than inherited:
 *  - the PDC block at 0xb290000 was never mapped, while phase 2 writes PDC 0x1148/0x1140
 *    (A650Gmu.c:172) and validate insists on the three window sizes (A650Gmu.c:91,98);
 *  - the regions kept a cacheable CPU view, while the kit copies the firmware through them and only
 *    barriers (A650Gmu.c:205-214).  A cacheable view makes the CPU read-back pass on its own cache
 *    and leaves the GMU reading stale DDR.  dbyGmuRegionAlloc now maps each region ONCE, uncached
 *    (kIOMapInhibitCache) and cleans the descriptor's cacheable alias before handing it out, so there
 *    is one memory type for one physical range and the kit's dsb is the only ordering needed.
 *    Cost of that choice, accepted: phase 4 zeroes icache (0xffc000 = 16.7 MiB) plus dummy and log
 *    through an uncached mapping, ~4.3M ordered stores, estimated a few hundred ms in this one boot.
 * The four iovas are the kit's fixed ones: icache 0x4000, dummy 0x60000000, log and hfi anywhere
 * (A650Gmu.c:109-112); the sizes come from the mode-8 regions.
 */
struct DbyColdIo {
	volatile uint8_t *base[A650GmuWindows];
	size_t bytes[A650GmuWindows];
};

/* Static, not a stack local: the kit's device struct keeps the cookie across the call, so a stack
 * address would dangle as soon as dbyGpuColdStart returned. */
static DbyColdIo gDbyColdIo;

static int
dbyColdRead32(void *cookie, unsigned window, size_t offset, uint32_t *out)
{
	DbyColdIo *io = static_cast<DbyColdIo *>(cookie);

	if (io == nullptr || out == nullptr || window >= A650GmuWindows || (offset & 3u) != 0u ||
	    offset + 4u > io->bytes[window]) {
		return 1;
	}
	*out = *(volatile uint32_t *)(io->base[window] + offset);
	return 0;
}

static int
dbyColdWrite32(void *cookie, unsigned window, size_t offset, uint32_t value)
{
	DbyColdIo *io = static_cast<DbyColdIo *>(cookie);

	if (io == nullptr || window >= A650GmuWindows || (offset & 3u) != 0u || offset + 4u > io->bytes[window]) {
		return 1;
	}
	*(volatile uint32_t *)(io->base[window] + offset) = value;
	return 0;
}

static int
dbyColdBarrier(void *)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static int
dbyColdNow(void *, uint64_t *us)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	*us = ns / 1000u;
	return 0;
}

static int
dbyColdWait(void *, unsigned us)
{
	IODelay(us == 0u ? 1u : us);
	return 0;
}

/* The regions are already mapped uncached by dbyGmuRegionAlloc (one view, one memory type), so the
 * kit is handed exactly that view: its firmware copy through cfg.cpu lands in DDR before the dsb at
 * A650Gmu.c:214. */
static int
dbyColdMapPdc(void)
{
	IODeviceMemory *memory;

	if (gDbyPdcMap) {
		return 1;
	}
	memory = IODeviceMemory::withRange(DBY_GMU_PDC_BASE, DBY_GMU_PDC_BYTES);
	if (memory == nullptr) {
		return 0;
	}
	gDbyPdcMap = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	return gDbyPdcMap != nullptr;
}

/* The mode-9 chain succeeded: sCR0 enabled, all ten stages ran clean. */
static unsigned
dbyCtxReady(void)
{
	return gDbySmmuGblOk != 0u && gDbyCtxStep == 9u && dbyCtxFailed() == 0u;
}

/* The kit's contract also requires "the GMU clocks running" (A650Gmu.h:9-10).  Mode 3 powers CX and
 * the SMMU clock only; the a6xx_gmu_resume clock sequence lives in A650GmuClocksEnable
 * (A650GmuPower.c:359-441) and is what mode 6 measured (rc=0 step=5 cxm=00004ff1).  A650GpuClockIsOn(0)
 * is true, so the verdict needs the value, its enable bit and its CLK_OFF bit, not just the helper. */
static unsigned
dbyGmuClocksForCold(const A650GpuResult *r, const A650GpuIo *io)
{
	A650GpuResult clean = *r;
	int rc;

	memset(&gDbyColdClocks, 0, sizeof gDbyColdClocks);
	clean.io_error = 0;
	rc = A650GmuClocksEnable(io, &clean, &gDbyColdClocks);
	gDbyColdClocksRc = rc;
	if (rc != 0 || gDbyColdClocks.failed != 0u || gDbyColdClocks.step != 5u) {
		return 0u;
	}
	if (gDbyColdClocks.cx_gmu == 0u || gDbyColdClocks.cx_gmu == 0xFFFFFFFFu ||
	    (gDbyColdClocks.cx_gmu & A650_CBCR_ENABLE) == 0u || !A650GpuClockIsOn(gDbyColdClocks.cx_gmu)) {
		return 0u;
	}
	return 1u;
}

static void
dbyGpuColdStart(void)
{
	A650GmuStartupLimits limits = {1000u, 100u, 50000u, 2000000u};
	unsigned i;

	gDbyColdRan = 1;
	gDbyColdStatus = 0xFFFFFFFFu;
	memset(&gDbyColdDevice, 0, sizeof gDbyColdDevice);
	memset(&gDbyColdCfg, 0, sizeof gDbyColdCfg);
	memset(&gDbyColdResult, 0, sizeof gDbyColdResult);
	memset(&gDbyColdIo, 0, sizeof gDbyColdIo);

	if (gDbyStartupRan == 0 || gDbyStartupStatus != 0u) {
		gDbyColdStatus = 0xFFFFFFF1u;          /* no startup table: the AOP command DB was not built */
		return;
	}
	/* DbyGpuFirmware.h defines this TU's own copy of the size word, so an address test would be
	 * tautological; the length check is kept as a guard against a regenerated header. */
	if (native_gpu_gmu_firmware_size != DBY_GPU_GMU_FIRMWARE_BYTES) {
		gDbyColdStatus = 0xFFFFFFF2u;          /* the embedded image is not the recorded length */
		return;
	}
	if (!dbyColdMapPdc()) {
		gDbyColdStatus = 0xFFFFFFF3u;          /* the PDC window did not map */
		return;
	}
	for (i = 0; i < 4u; ++i) {
		if (gDbyGmuRegions[i].cpu == nullptr || gDbyGmuRegions[i].phys == 0u || gDbyGmuRegions[i].bytes == 0u) {
			gDbyColdStatus = 0xFFFFFFF4u;      /* a region was not built this boot */
			return;
		}
		gDbyColdMem[i].cpu = gDbyGmuRegions[i].cpu;
		gDbyColdMem[i].bytes = gDbyGmuRegions[i].bytes;
		gDbyColdMem[i].iova = gDbyGmuRegions[i].iova;
	}

	gDbyColdIo.base[0] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinGmu]);
	gDbyColdIo.base[1] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinRscc]);
	gDbyColdIo.base[2] = reinterpret_cast<volatile uint8_t *>(gDbyPdcMap->getVirtualAddress());
	/* The fourth window is the GPU block: the kit clears GBIF through it immediately before the
	 * CM3 release (the probe's own early clear left the ACK at 0x3). */
	gDbyColdIo.base[3] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinGpu]);
	gDbyColdIo.bytes[0] = 0x30000u;
	gDbyColdIo.bytes[1] = 0x10000u;
	gDbyColdIo.bytes[2] = 0x10000u;
	gDbyColdIo.bytes[3] = A650_GPU_BYTES;
	if (gDbyColdIo.base[0] == nullptr || gDbyColdIo.base[1] == nullptr || gDbyColdIo.base[2] == nullptr ||
	    gDbyColdIo.base[3] == nullptr) {
		gDbyColdStatus = 0xFFFFFFF5u;          /* a window base is not mapped */
		return;
	}

	gDbyColdDevice.io.cookie = &gDbyColdIo;
	gDbyColdDevice.io.bytes[0] = gDbyColdIo.bytes[0];
	gDbyColdDevice.io.bytes[1] = gDbyColdIo.bytes[1];
	gDbyColdDevice.io.bytes[2] = gDbyColdIo.bytes[2];
	gDbyColdDevice.io.bytes[3] = gDbyColdIo.bytes[3];
	gDbyColdDevice.io.read32 = dbyColdRead32;
	gDbyColdDevice.io.write32 = dbyColdWrite32;
	gDbyColdDevice.io.barrier = dbyColdBarrier;
	gDbyColdDevice.services.cookie = nullptr;
	gDbyColdDevice.services.now_us = dbyColdNow;
	gDbyColdDevice.services.wait_us = dbyColdWait;

	gDbyColdCfg.chip_id = DBY_GMU_CHIP_ID;
	gDbyColdCfg.firmware = native_gpu_gmu_firmware;
	gDbyColdCfg.firmware_bytes = native_gpu_gmu_firmware_size;
	gDbyColdCfg.icache = gDbyColdMem[0];
	gDbyColdCfg.dummy = gDbyColdMem[1];
	gDbyColdCfg.log = gDbyColdMem[2];
	gDbyColdCfg.hfi = gDbyColdMem[3];
	/* 1000 polls of 100 us = the vendor's 100 ms GMU_START_TIMEOUT (adreno_a6xx_gmu.c:292); the kit
	 * now caps the loop at 100 ms too, but the budget is what ends it, so both had to move. */
	gDbyColdCfg.poll_budget = 1000u;

	gDbyColdStatus = (uint32_t)A650GmuColdStart(&gDbyColdDevice, &gDbyColdCfg, &gDbyStartupCfg,
	                                           &limits, &gDbyColdResult);
}

static void
renderCold(void)
{
	const A650GmuResult *l = &gDbyColdResult.load;
	char *row = native_gpupower_rows[4];
	const char *end = row + 119;
	char *at = row;

	/* Rows 2 and 3 keep the SMMU-context stages: if the cold start fails, the first question is
	 * whether the context it was given was the one this boot built, so that evidence stays. */
	at = putStr(at, end, "GPU COLD st=");
	at = putDec(at, end, gDbyColdStatus);
	at = putStr(at, end, " phase=");
	at = putDec(at, end, l->phase);
	at = putStr(at, end, " blocks=");
	at = putDec(at, end, l->blocks);
	at = putStr(at, end, " ver=");
	at = putHex8(at, end, l->firmware_version);
	at = putStr(at, end, " init=");
	at = putHex8(at, end, l->firmware_init_result);
	at = putStr(at, end, " hfi=");
	at = putHex8(at, end, l->hfi_status);
	at = putStr(at, end, " rdy=");
	at = putDec(at, end, l->firmware_ready_observed);
	at = putDec(at, end, l->hfi_ready_observed);
	*at = 0;

	row = native_gpupower_rows[5];
	end = row + 119;
	at = row;
	at = putStr(at, end, "GPU COLD rst=");
	at = putDec(at, end, l->reset_verified);
	at = putStr(at, end, " rsc=");
	at = putDec(at, end, l->rsc_stop_status);
	at = putStr(at, end, " clk=");
	at = putSigned(at, end, gDbyColdClocksRc);
	at = putStr(at, end, "/");
	at = putDec(at, end, gDbyColdClocks.step);
	at = putStr(at, end, " ao=");
	at = putHex8(at, end, l->ao_fault_status);
	at = putStr(at, end, " HFI pst=");
	at = putDec(at, end, (unsigned)gDbyColdResult.protocol_status);
	at = putStr(at, end, " stg=");
	at = putDec(at, end, (unsigned)gDbyColdResult.protocol.stage);
	at = putStr(at, end, " ack=");
	at = putHex8(at, end, gDbyColdResult.protocol.acknowledged_stages);
	at = putStr(at, end, " err=");
	at = putHex8(at, end, gDbyColdResult.protocol.firmware_error);
	*at = 0;
}

/* Mode 11: the state the failed attempt left, all read-only, so the next round can tell a dead CM3
 * (busy=0, no fault) from a GMU that never got its clocks (pwr) or whose AHB writes were dropped
 * (fence).  Offsets are the GMU window's dwords: 0x50d0 SPTPRAC_PWR_CLK_STATUS, 0x501a CM3_FW_BUSY,
 * 0x5192 GMU2HOST_INTR_INFO (bit 15 is CM3_FAULT, kgsl_hfi.h:55), 0x9313 AHB_FENCE_STATUS,
 * 0x9305 AO_HOST_INTERRUPT_STATUS (0x9304 is its W1C CLR), and the DTCM breadcrumb at 0x3fdc
 * (vendor adreno_a6xx_gmu.c:396-402).  RSCC_CONTROL_ACK was dropped: it reads 0 in both outcomes. */
static void
renderColdDbg(void)
{
	const A650GmuResult *l = &gDbyColdResult.load;
	char *row = native_gpupower_rows[5];
	const char *end = row + 119;
	char *at = row;

	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x14340u, &gDbyColdDbg[0]);
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x14068u, &gDbyColdDbg[1]);
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x14648u, &gDbyColdDbg[2]);
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x24C4Cu, &gDbyColdDbg[3]);
	/* The vendor's GMU-doesn't-boot breadcrumb: the firmware writes DTCM offset 0x3fdc before it sets
	 * FW_INIT_RESULT (adreno_a6xx_gmu.c:396-402), so a zero here means the CM3 never got that far. */
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x2BF7u * 4u, &gDbyColdDbg[4]);
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x24C14u, &gDbyColdDbg[5]);

	at = putStr(at, end, "GPU DBG busy=");
	at = putHex8(at, end, gDbyColdDbg[1]);
	at = putStr(at, end, " intr=");
	at = putHex8(at, end, gDbyColdDbg[2]);
	at = putStr(at, end, " fence=");
	at = putHex8(at, end, gDbyColdDbg[3]);
	at = putStr(at, end, " dtcm=");
	at = putHex8(at, end, gDbyColdDbg[4]);
	at = putStr(at, end, " gh=");
	at = putHex8(at, end, l->gbif_halt);
	at = putStr(at, end, " ga=");
	at = putHex8(at, end, l->gbif_ack);
	at = putStr(at, end, " bc=");
	at = putHex8(at, end, l->breadcrumb);
	at = putStr(at, end, " n=");
	at = putDec(at, end, l->breadcrumb_changes);
	*at = 0;
}

/* Mode 12: the evidence the first two audits asked for before anything else changes - the AO fault
 * status, the SMMU's global fault status and the faulting stream, and bank 1's sFSR/sFAR - all
 * read-only after the attempt.  Bank 1's context offset is (pages + bank) * stride from ID1
 * (ArmSmmu4KProgram.c:80); this part reports ID1=0x20000005, i.e. 8 pages * 0x1000 + 1 * 0x1000 =
 * 0x9000, so sFSR=0x9058 and sFAR=0x9060 are BYTE offsets in the 0x10000 SMMU window. */
static void
renderFaultDbg(unsigned row_index)
{
	const A650GmuResult *l = &gDbyColdResult.load;
	char *row = native_gpupower_rows[row_index < 6u ? row_index : 5u];
	const char *end = row + 119;
	char *at = row;
	uint32_t ao = 0, gfsr = 0, gnr1 = 0, fsr = 0, far = 0;

	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, 0x24C14u, &ao);      /* AO_HOST_INTERRUPT_STATUS */
	/* GR0 registers are BYTE offsets in this file's convention (A650_SMMU_GFSR = 0x048), and so are
	 * the vendor's (arm-smmu-regs.h:56-58).  A x4 here would read reserved space and report a clean
	 * SMMU, which is the one thing this mode exists to rule out. */
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_GFSR, &gfsr);
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, A650_SMMU_GFSYNR1, &gnr1);
	/* The context bank base is a BYTE offset inside the 0x10000 SMMU window (ArmSmmu4KProgram.c:80:
	 * context_offset = (pages + bank) * stride, both bytes), so CB1 = 0x9000 and its sFSR/sFAR are
	 * 0x9058/0x9060 - not the dword offsets the GR0 registers use. */
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, 0x9058u, &fsr);     /* CB1 sFSR */
	(void)dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, 0x9060u, &far);     /* CB1 sFAR */

	at = putStr(at, end, "GPU DBG ao=");
	at = putHex8(at, end, ao);
	at = putStr(at, end, " gfsr=");
	at = putHex8(at, end, gfsr);
	at = putStr(at, end, " gnr1=");
	at = putHex8(at, end, gnr1);
	at = putStr(at, end, " fsr=");
	at = putHex8(at, end, fsr);
	at = putStr(at, end, " far=");
	at = putHex8(at, end, far);
	at = putStr(at, end, " bc=");
	at = putHex8(at, end, l->breadcrumb);
	at = putStr(at, end, " n=");
	at = putDec(at, end, l->breadcrumb_changes);
	*at = 0;
}

/* The GMU's own 16 KiB log region, written by the firmware.  A run of printable characters is the
 * firmware's message text; REX RTOS images carry strings such as "SIGABRT" and
 * "HAL_clk_ValidateRegTimeout", so the first run names the failure if the firmware got that far. */
static uint32_t gDbyLogOffset;

static const char *
dbyLogString(void)
{
	static char text[112];
	const volatile uint8_t *base = (const volatile uint8_t *)gDbyGmuRegions[2].cpu;
	size_t bytes = gDbyGmuRegions[2].bytes;
	size_t run = 0, start = 0, at = 0;

	if (base == nullptr || bytes == 0u) {
		return nullptr;
	}
	for (size_t i = 0; i < bytes; ++i) {
		const uint8_t c = base[i];
		if (c >= 0x20u && c < 0x7Fu) {
			if (run == 0u) {
				start = i;
			}
			++run;
			if (run >= 16u) {
				at = start;
				break;
			}
		} else {
			run = 0;
		}
	}
	if (at == 0u && run < 16u) {
		return nullptr;
	}
	{
		size_t n = 0;
		while (at + n < bytes && n + 1u < sizeof text && base[at + n] >= 0x20u && base[at + n] < 0x7Fu) {
			text[n] = (char)base[at + n];
			++n;
		}
		text[n] = 0;
	}
	gDbyLogOffset = (uint32_t)at;
	return text[0] != 0 ? text : nullptr;
}

/* Mode 13: GX power through the GMU (A650GmuPowerGx: the GX_BW_PERF_VOTE for perf_index, then the
 * OOB GPU_SET handshake).  The kit's result carries the SPTPRAC status before and after; GX is on
 * when neither GX_HM_GDSC_POWER_OFF nor GX_HM_CLK_OFF is left.  perf_index 1 is the first table
 * entry (305 MHz): the smallest step that proves the path. */
static void
renderGx(void)
{
	char *row = native_gpupower_rows[5];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU GX st=");
	at = putDec(at, end, gDbyGxStatus);
	at = putStr(at, end, " gx=");
	at = putHex8(at, end, gDbyGxResult.gx_before);
	at = putStr(at, end, ">");
	at = putHex8(at, end, gDbyGxResult.gx_after);
	at = putStr(at, end, " on=");
	at = putDec(at, end, gDbyGxResult.gx_on);
	at = putStr(at, end, " oob=");
	at = putDec(at, end, gDbyGxResult.oob_polls);
	at = putStr(at, end, " info=");
	at = putHex8(at, end, gDbyGxResult.oob_info);
	at = putStr(at, end, " vote=");
	at = putDec(at, end, gDbyGxResult.vote_status);
	at = putStr(at, end, " err=");
	at = putHex8(at, end, gDbyGxResult.vote_error);
	*at = 0;
}

static void
renderLogRow(void)
{
	char *row = native_gpupower_rows[3];
	const char *end = row + 119;
	char *at = row;
	const char *text = dbyLogString();

	at = putStr(at, end, "GPU LOG @");
	at = putHex8(at, end, (uint32_t)gDbyLogOffset);
	at = putStr(at, end, " ");
	at = putStr(at, end, text != nullptr ? text : "<no run>=16");
	*at = 0;
}

/* --- mode 14: the GPU's own SMMU context, the SQE, ring 0 and the first CP command -------------------
 *
 * Mode 10 leaves the GMU running on bank 1 / ASID 2 and mode 13 has GX powered, but the GPU itself still
 * has no address space: the GPU node of kona-gpu.dtsi carries iommus = <&kgsl_smmu 0x0 0x401>, i.e. SIDs 0,
 * 1, 0x400 and 0x401, and every one of them is faulted.  This mode builds bank 0 / ASID 1 for those four
 * streams, maps the four kernel buffers the command processor needs at the reference board's fixed GPU
 * addresses (work/tracks/gpu-official-route/scratch/a650/NativeAdrenoGpuContext.h:20-23: SQE 0x02000000,
 * ring 0x02100000, memptrs 0x02200000, scratch 0x02202000), loads a650_sqe.fw, programs ring 0, runs
 * CP_ME_INIT and then proves execution: a CP_MEM_WRITE packet puts a marker into the scratch page and the
 * CPU reads it back.  That is the first time the GPU itself reads and writes memory through this kernel.
 */

/* Bank 0 / ASID 1 is the GPU's own context (the GMU owns bank 1 / ASID 2, DBY_GMU_CTX_BANK).  The slot
 * numbers are this probe's choice, not the SID: the SIDs come from the DT mask above, and slots 4 and 5
 * stay with the GMU (DBY_GMU_SLOT_FIRST/LAST).  Slots 0..3 are used only after
 * ArmSmmu4KCheckFaultedStreams proves they read FAULT and that no other live route uses this bank; if the
 * bootloader left one of them live, this mode refuses instead of taking it. */
#define DBY_GPU_BANK         0u
#define DBY_GPU_ASID         1u
#define DBY_GPU_SLOT_COUNT   4u
#define DBY_CPB_SQE_IOVA     0x02000000ull
#define DBY_CPB_RING_IOVA    0x02100000ull
#define DBY_CPB_MEMPTRS_IOVA 0x02200000ull
#define DBY_CPB_SCRATCH_IOVA 0x02202000ull
#define DBY_CPB_SQE_BYTES    0x8000u
#define DBY_CPB_RING_BYTES   0x8000u
#define DBY_CPB_PAGE_BYTES   0x1000u
#define DBY_CPB_MARKER       0xa6500c0du
#define DBY_SMMU_GPU_POOL_BYTES (16u * 4096u)

/* CP register dword indices (a6xx.xml; the same ones the driver pins in
 * work/tracks/gpu-official-route/scratch/a650/NativeAdrenoRing.cpp:12-20 and NativeAdrenoCpInit.cpp:13-21). */
#define DBY_CP_RB_BASE          0x0800u
#define DBY_CP_RB_CNTL          0x0802u
#define DBY_CP_RB_RPTR_ADDR     0x0804u
#define DBY_CP_RB_RPTR          0x0806u
#define DBY_CP_RB_WPTR          0x0807u
#define DBY_CP_SQE_CNTL         0x0808u
#define DBY_CP_SQE_INSTR_BASE   0x0830u
#define DBY_CP_SCRATCH2         0x0885u
#define DBY_CP_HW_FAULT         0x0821u
#define DBY_CP_INTERRUPT_STATUS 0x0823u
#define DBY_CP_PROTECT_CNTL     0x084Fu
#define DBY_CP_APRIV_CNTL       0x0844u
#define DBY_RBBM_STATUS         0x0210u
#define DBY_RBBM_INT_0_MASK     0x0038u

/* Packet opcodes and the event word (adreno_pm4.xml, pinned by the driver: NativeAdrenoCpInit.cpp:16-17 and
 * NativeAdrenoRing.cpp:62-70): CP_ME_INIT 0x48 carries eight payload words on a6xx, CP_EVENT_WRITE 0x46 with
 * CACHE_FLUSH_TS 0x04 plus the IRQ bit writes the payload's fourth dword, CP_MEM_WRITE 0x3d takes the
 * address low, high and the value. */
#define DBY_PM4_ME_INIT      0x48u
#define DBY_PM4_EVENT_WRITE  0x46u
#define DBY_PM4_MEM_WRITE    0x3du
#define DBY_PM4_FLUSH_TS_IRQ (0x04u | (1u << 31))

static DbyGmuRegion gDbyGpuBuf[4];
static ArmSmmu4KConfig gDbyGpuCtxConfig;
static ArmSmmu4KResult gDbyGpuCtxBuild;
static ArmSmmu4KContextConfig gDbyGpuCtxContext;
/* 1 once the identity words have been read from the bank-0 registers and stored in that
 * config; the encode that consumes them follows at once. The G-1 row renders input_bits
 * from them and prints -- when this is 0. */
static unsigned gDbyGpuCtxIdsOk;
static ArmSmmu4KStreamState gDbyGpuCtxStreams;
static uint32_t gDbyGpuCtxStage[8];
static unsigned gDbyGpuCtxStep;
static IOBufferMemoryDescriptor *gDbyGpuPool;
static uint64_t gDbyGpuPoolPhys;
static volatile uint64_t *gDbyGpuPoolCpu;
static unsigned gDbyGpuPoolOk;
static uint64_t gDbyGpuTtbr0;
static uint32_t gDbyGpuCtxActlr;
static uint32_t gDbyGpuCpStep;
static uint32_t gDbyGpuCpRc;
static uint32_t gDbyGpuCpWptr;
static uint32_t gDbyGpuCpRptr;
static uint32_t gDbyGpuCpHwFault;
static uint32_t gDbyGpuCpIntStatus;
static uint32_t gDbyGpuCpMarker;
static uint32_t gDbyGpuCpMeInitFence;
static uint32_t gDbyGpuCpMemWriteFence;
static uint32_t gDbyGpuCpSqeBytes;
static uint32_t gDbyGpuCpSqeBad;
static uint32_t gDbyGpuCpRegFail;
/* Mode 14 round 2: what the four GPU slots carried before this block normalised them. */
static uint32_t gDbyGpuSlotSmr[DBY_GPU_SLOT_COUNT];
static uint32_t gDbyGpuSlotS2cr[DBY_GPU_SLOT_COUNT];
static unsigned gDbyGpuSlotNormalized;
static uint32_t gDbyGpuGfsr, gDbyGpuGfSid;

static unsigned
dbyGpuIl2(uint32_t value)
{
	unsigned result = 0u;

	while (value > 1u) {
		value >>= 1;
		result++;
	}
	return result;
}

/* GPU-window register access, bounded by the DTS window size and by the state's mapped bit (dbyGpuRead32 /
 * dbyGpuWrite32 check both). */
static int
dbyGpuCpWrite(uint32_t dword, uint32_t value)
{
	if ((uint64_t)dword * 4ull + 4ull > (uint64_t)A650_GPU_BYTES) {
		return 1;
	}
	return dbyGpuWrite32(&gDbyGpu, A650GpuWinGpu, (size_t)dword * 4u, value);
}

static int
dbyGpuCpRead(uint32_t dword, uint32_t *out)
{
	if ((uint64_t)dword * 4ull + 4ull > (uint64_t)A650_GPU_BYTES) {
		return 1;
	}
	return dbyGpuRead32(&gDbyGpu, A650GpuWinGpu, (size_t)dword * 4u, out);
}

static int
dbyGpuCpW(uint32_t dword, uint32_t value)
{
	if (dbyGpuCpWrite(dword, value) != 0) {
		if (gDbyGpuCpRegFail == 0u) {
			gDbyGpuCpRegFail = dword;
		}
		return 1;
	}
	return 0;
}

static int
dbyGpuCpW64(uint32_t dword, uint64_t value)
{
	if (dbyGpuCpW(dword, (uint32_t)value) != 0) {
		return 1;
	}
	return dbyGpuCpW(dword + 1u, (uint32_t)(value >> 32));
}

/* The bank-0 table pool.  Deliberately leaked like the GMU's (the GPU walks these pages for the life of
 * the boot) and separate from it: each context's TTBR0 names its own root. */
static void
dbyGpuCtxPoolAlloc(void)
{
	IOBufferMemoryDescriptor *pool;

	gDbyGpuPoolOk = 0u;
	if (gDbyGpuPool != nullptr) {
		return;
	}
	pool = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous | kIODirectionOutIn,
	                                             DBY_SMMU_GPU_POOL_BYTES, 4096);
	if (pool == nullptr) {
		return;
	}
	if (pool->prepare(kIODirectionOutIn) != kIOReturnSuccess) {
		pool->release();
		return;
	}
	{
		IOPhysicalAddress phys = pool->getPhysicalAddress();
		void *raw = pool->getBytesNoCopy();

		if (phys == 0u || raw == nullptr || (((uintptr_t)raw) & 4095u) != 0u) {
			pool->complete(kIODirectionOutIn);
			pool->release();
			return;
		}
		gDbyGpuPoolPhys = (uint64_t)phys;
		gDbyGpuPoolCpu = reinterpret_cast<volatile uint64_t *>(raw);
		gDbyGpuPool = pool;
		gDbyGpuPoolOk = 1u;
	}
}

static uint32_t
dbyGpuCtxPublishTables(uint64_t pages)
{
	uint64_t page;

	if (pages == 0u || pages > (uint64_t)(DBY_SMMU_GPU_POOL_BYTES / 4096u)) {
		return 1u;
	}
	for (page = 0; page < pages; page++) {
		if (dbyCtxPublish((volatile uint64_t *)gDbyGpuPoolCpu + page * 512u, 4096u) != 0) {
			return 1u;
		}
	}
	return 0u;
}

/* a6xx hw_init for the A650 (Linux v6.6 a6xx_gpu.c a650_hw_init), as the reference board runs it
 * (work/tracks/cpu-official-route/scratch/pipa/package/GpuProbe.cpp:620-675), minus a6xx_set_hwcg: clock
 * gating only matters once a shader runs, and setHwcg writes two GMU registers, which this step does not
 * need.  CP_PROTECT is Linux's a650_protect, 39 entries encoded as A6XX_PROTECT_RDONLY
 * (start | (count - 1) << 18) or A6XX_PROTECT_NORDWR (that | 1 << 31); the last entry, the unbounded tail,
 * goes into CP_PROTECT(47) exactly as Linux writes it. */
static uint32_t
dbyGpuProtect(unsigned index)
{
	/* {register, count - 1, 0 = read-only or 1 = no-read-no-write}, a650_protect in order. */
	static const uint32_t spec[39][3] = {
		{0x00000u, 0x04ffu, 0u}, {0x00501u, 0x0005u, 0u}, {0x0050bu, 0x02f4u, 0u},
		{0x0050eu, 0x0000u, 1u}, {0x00510u, 0x0000u, 1u}, {0x00534u, 0x0000u, 1u},
		{0x00800u, 0x0082u, 1u}, {0x008a0u, 0x0008u, 1u}, {0x008abu, 0x0024u, 1u},
		{0x008deu, 0x00aeu, 0u}, {0x00900u, 0x004du, 1u}, {0x0098du, 0x0272u, 1u},
		{0x00e00u, 0x0001u, 1u}, {0x00e03u, 0x000cu, 1u}, {0x03c00u, 0x00c3u, 1u},
		{0x03cc4u, 0x1fffu, 0u}, {0x08630u, 0x01cfu, 1u}, {0x08e00u, 0x0000u, 1u},
		{0x08e08u, 0x0000u, 1u}, {0x08e50u, 0x001fu, 1u}, {0x08e80u, 0x027fu, 1u},
		{0x09624u, 0x01dbu, 1u}, {0x09e60u, 0x0011u, 1u}, {0x09e78u, 0x0187u, 1u},
		{0x0a630u, 0x01cfu, 1u}, {0x0ae02u, 0x0000u, 1u}, {0x0ae50u, 0x032fu, 1u},
		{0x0b604u, 0x0000u, 1u}, {0x0b608u, 0x0007u, 1u}, {0x0be02u, 0x0001u, 1u},
		{0x0be20u, 0x17dfu, 1u}, {0x0f000u, 0x0bffu, 1u}, {0x0fc00u, 0x1fffu, 0u},
		{0x18400u, 0x1fffu, 1u}, {0x1a800u, 0x1fffu, 1u}, {0x1f400u, 0x0443u, 1u},
		{0x1f844u, 0x007bu, 0u}, {0x1f887u, 0x001bu, 1u}, {0x1f8c0u, 0x0000u, 1u},
	};
	uint32_t value;

	if (index >= 39u) {
		return 0u;
	}
	value = ((spec[index][1] & 0x3FFFu) << 18) | (spec[index][0] & 0x3FFFFu);
	return spec[index][2] != 0u ? (value | (1u << 31)) : value;
}

static int
dbyGpuHwInit(void)
{
	static const uint32_t addr_mode[] = {0x842u, 0xC01u, 0x8601u, 0x8E05u, 0x9E01u, 0xBE05u,
	                                     0xA601u, 0x9601u, 0xE00u, 0xAE01u, 0xB601u, 0xF810u};
	static const uint32_t bicubic[5] = {0x00000000u, 0x3fe05ff4u, 0x3fa0ebeeu, 0x3f5193edu, 0x3f0243f0u};
	unsigned i;

	gDbyGpuCpRegFail = 0u;
	if (dbyGpuCpW(0x3C45u, 0u) != 0 || dbyGpuCpW(0x0016u, 0u) != 0 ||
	    dbyGpuCpW(0xF803u, 0u) != 0 || dbyGpuCpW64(0xF800u, 0ull) != 0 || dbyGpuCpW(0xF802u, 0u) != 0) {
		return 1;
	}
	for (i = 0; i < sizeof addr_mode / sizeof addr_mode[0]; i++) {
		if (dbyGpuCpW(addr_mode[i], 1u) != 0) {
			return 1;
		}
	}
	for (i = 0x3C03u; i <= 0x3C06u; i++) {
		if (dbyGpuCpW(i, 0x00071620u) != 0) {
			return 1;
		}
	}
	if (dbyGpuCpW(0x0011u, 3u) != 0 || dbyGpuCpW(0x050Bu, 0xFFFFFFFFu) != 0 ||
	    dbyGpuCpW64(0xE05u, 0x0001ffffffffffc0ull) != 0 ||
	    dbyGpuCpW64(0xE09u, 0x0001fffffffff000ull) != 0 ||
	    dbyGpuCpW64(0xE07u, 0x0001fffffffff000ull) != 0 ||
	    dbyGpuCpW(0xE18u, 0x804u) != 0 || dbyGpuCpW(0xE17u, 4u) != 0 ||
	    dbyGpuCpW(0x8C2u, 0x02000140u) != 0 || dbyGpuCpW(0x8C1u, 0x8040362cu) != 0 ||
	    dbyGpuCpW(0x8C3u, 128u) != 0 || dbyGpuCpW(0x9E00u, 0x00300200u) != 0 ||
	    dbyGpuCpW(0x98Du, 1u) != 0 || dbyGpuCpW(0x500u, 1u) != 0 || dbyGpuCpW(0x8D0u, 0u) != 0 ||
	    dbyGpuCpW(0x8E08u, 0x816u) != 0 || dbyGpuCpW(0xB604u, 6u) != 0 ||
	    dbyGpuCpW(0xAE02u, 0x26u) != 0 || dbyGpuCpW(0xE01u, 3u << 21) != 0 ||
	    dbyGpuCpW(0x001Fu, (1u << 30) | 0x1FFFFFu) != 0 || dbyGpuCpW(0xE19u, 1u) != 0) {
		return 1;
	}
	for (i = 0; i < 5u; i++) {
		if (dbyGpuCpW(0xB608u + i, bicubic[i]) != 0) {
			return 1;
		}
	}
	if (dbyGpuCpW(DBY_CP_PROTECT_CNTL, 0x0Bu) != 0) {
		return 1;
	}
	for (i = 0; i < 39u; i++) {
		uint32_t reg = (i == 38u) ? (0x850u + 47u) : (0x850u + i);

		if (dbyGpuCpW(reg, dbyGpuProtect(i)) != 0) {
			return 1;
		}
	}
	/* CP_APRIV_CNTL: the CP's own instruction, ring and rptr accesses are privileged (hw_apriv); that is what
	 * lets the ring run WITHOUT NO_UPDATE and still have the CP write the RPTR shadow
	 * (NativeAdrenoRing.cpp:361-374).  RBBM_INT_0_MASK stays 0: nothing here handles a GPU interrupt, and
	 * the fences are memory writes, so every CP interrupt is masked. */
	if (dbyGpuCpW(DBY_CP_APRIV_CNTL, 0x6Eu) != 0 || dbyGpuCpW(DBY_RBBM_INT_0_MASK, 0u) != 0) {
		return 1;
	}
	return 0;
}

/* Kick the ring and wait for the fence at memptrs + 4 (msm_rbmemptrs.fence, NativeAdrenoRing.cpp:227-229).
 * A write to CP_RB_WPTR can be dropped while the GPU domain is not ready (NativeAdrenoRing.cpp:93-96), so
 * the write is repeated up to four times, each with a 50 ms poll. */
static unsigned
dbyGpuCpWait(uint32_t want, uint32_t *observed)
{
	unsigned kick;

	*observed = 0u;
	for (kick = 0; kick < 4u; kick++) {
		unsigned i;

		if (dbyGpuCpWrite(DBY_CP_RB_WPTR, gDbyGpuCpWptr) != 0) {
			return 1u;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		for (i = 0; i < 5000u; i++) {
			uint32_t value = gDbyGpuBuf[2].cpu[1];

			if (value >= want) {
				*observed = value;
				return 0u;
			}
			IODelay(10u);
		}
	}
	*observed = gDbyGpuBuf[2].cpu[1];
	return 2u;
}

/* Append a TYPE4 CP_SCRATCH(2) sequence number and a TYPE7 CP_EVENT_WRITE / CACHE_FLUSH_TS fence to a ring
 * packet; returns the new word count or 0 when a header was refused (NativeAdrenoRing.cpp:219-232). */
static unsigned
dbyGpuCpAppendFence(uint32_t *words, unsigned at, uint32_t seqno)
{
	const uint64_t fence = DBY_CPB_MEMPTRS_IOVA + 4u;

	if (AdrenoPM4Type4(DBY_CP_SCRATCH2, 1u, &words[at]) != AdrenoPM4Success) {
		return 0u;
	}
	words[at + 1u] = seqno;
	at += 2u;
	if (AdrenoPM4Type7(DBY_PM4_EVENT_WRITE, 4u, &words[at]) != AdrenoPM4Success) {
		return 0u;
	}
	words[at + 1u] = DBY_PM4_FLUSH_TS_IRQ;
	words[at + 2u] = (uint32_t)fence;
	words[at + 3u] = (uint32_t)(fence >> 32);
	words[at + 4u] = seqno;
	return at + 5u;
}

static void
dbyGpuCpStart(void)
{
	ArmSmmu4KRange ranges[4];
	ArmSmmu4KStreamResult sr;
	ArmSmmu4KProgramResult prog;
	ArmSmmu4KProgramResult obs;
	ArmSmmu4KContextImage image;
	ArmSmmu4KStream slots[DBY_GPU_SLOT_COUNT];
	uint32_t id0 = 0;
	uint32_t id2 = 0;
	uint32_t control;
	uint32_t sqeBytes;
	unsigned i;
	unsigned at;

	gDbyGpuCpStep = 0u;
	gDbyGpuCpRc = 0xFFFFFFFFu;
	gDbyGpuCpWptr = 0u;
	gDbyGpuCpRptr = 0u;
	gDbyGpuCpHwFault = 0u;
	gDbyGpuCpIntStatus = 0u;
	gDbyGpuCpMarker = 0u;
	gDbyGpuCpMeInitFence = 0u;
	gDbyGpuCpMemWriteFence = 0u;
	gDbyGpuCpSqeBytes = 0u;
	gDbyGpuCpSqeBad = 0u;
	gDbyGpuCpRegFail = 0u;
	gDbyGpuTtbr0 = 0u;
	gDbyGpuCtxActlr = 0u;
	gDbyGpuSlotNormalized = 0u;
	gDbyGpuGfsr = 0u;
	gDbyGpuGfSid = 0u;
	memset(gDbyGpuSlotSmr, 0, sizeof gDbyGpuSlotSmr);
	memset(gDbyGpuSlotS2cr, 0, sizeof gDbyGpuSlotS2cr);
	memset(gDbyGpuCtxStage, 0xFF, sizeof gDbyGpuCtxStage);
	gDbyGpuCtxStep = 0u;

	/* The gate: a complete cold start, GX on, and the GMU context this boot built.  Anything less and the
	 * GPU block is not powered, or the SMMU objects this mode reuses (gDbyCtxIo, the window cookie) were
	 * never set up. */
	gDbyGpuCpStep = 1u;
	if (gDbyColdStatus != 0u || gDbyGxStatus != 0u || gDbyGxResult.gx_on == 0u ||
	    gDbyColdClocksRc != 0 || gDbyColdClocks.failed != 0u || gDbyColdClocks.step != 5u ||
	    dbyCtxReady() == 0u) {
		gDbyGpuCpRc = 0xFFFFFF01u;
		return;
	}
	dbyGpuCtxPoolAlloc();
	if (gDbyGpuPoolOk == 0u) {
		gDbyGpuCpRc = 0xFFFFFF02u;
		return;
	}

	gDbyGpuCpStep = 2u;
	if (!dbyGmuRegionAlloc(&gDbyGpuBuf[0], DBY_CPB_SQE_BYTES, DBY_CPB_SQE_IOVA) ||
	    !dbyGmuRegionAlloc(&gDbyGpuBuf[1], DBY_CPB_RING_BYTES, DBY_CPB_RING_IOVA) ||
	    !dbyGmuRegionAlloc(&gDbyGpuBuf[2], DBY_CPB_PAGE_BYTES, DBY_CPB_MEMPTRS_IOVA) ||
	    !dbyGmuRegionAlloc(&gDbyGpuBuf[3], DBY_CPB_PAGE_BYTES, DBY_CPB_SCRATCH_IOVA)) {
		gDbyGpuCpRc = 0xFFFFFF03u;
		return;
	}
	for (i = 0; i < gDbyGpuBuf[1].bytes / 4u; i++) {
		gDbyGpuBuf[1].cpu[i] = 0u;
	}
	for (i = 0; i < gDbyGpuBuf[2].bytes / 4u; i++) {
		gDbyGpuBuf[2].cpu[i] = 0u;
	}
	for (i = 0; i < gDbyGpuBuf[3].bytes / 4u; i++) {
		gDbyGpuBuf[3].cpu[i] = 0u;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* SQE: adreno_fw_create_bo drops the firmware's first dword, then reads the copy back. */
	gDbyGpuCpStep = 3u;
	sqeBytes = native_gpu_sqe_firmware_size;
	if (native_gpu_sqe_firmware == nullptr || sqeBytes < 8u || sqeBytes - 4u > DBY_CPB_SQE_BYTES) {
		gDbyGpuCpRc = 0xFFFFFF04u;
		return;
	}
	sqeBytes -= 4u;
	gDbyGpuCpSqeBytes = sqeBytes;
	for (i = 0; i < sqeBytes; i++) {
		((volatile uint8_t *)gDbyGpuBuf[0].cpu)[i] = native_gpu_sqe_firmware[4u + i];
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < sqeBytes; i++) {
		if (((volatile uint8_t *)gDbyGpuBuf[0].cpu)[i] != native_gpu_sqe_firmware[4u + i]) {
			gDbyGpuCpSqeBad = i + 1u;
			break;
		}
	}
	if (gDbyGpuCpSqeBad != 0u) {
		gDbyGpuCpRc = 0xFFFFFF05u;
		return;
	}

	/* Bank 0 / ASID 1.  Stages 0..7 are the same kit calls mode 9 makes for the GMU's bank, with the
	 * preflight first: the four target streams must read FAULT and no other live route may use bank 0. */
	gDbyGpuCpStep = 4u;
	for (i = 0; i < DBY_GPU_SLOT_COUNT; i++) {
		slots[i].slot = i;
		slots[i].sid = (uint16_t)(i < 2u ? i : (0x400u + (i - 2u)));
	}
	/* What the four slots carry, then the canonical FAULT word where a slot is not already there.
	 * The kit refuses to attach over anything that is not exactly FAULT (ArmSmmu4KStreams.c:43-44),
	 * and the first attempt measured that refusal (pre=10, evidence/edl-log-s69).  S2CR is written
	 * before any SMR, the same order ArmSmmu4KAttachStreams uses (:129-139).  A slot whose SMR is
	 * valid with another SID is left for the preflight to report: this block only stops translation. */
	for (i = 0; i < DBY_GPU_SLOT_COUNT; i++) {
		uint32_t smr = 0;
		uint32_t s2cr = 0;

		if (gDbyCtxIo.read32(gDbyCtxIo.cookie, 0x800u + 4u * i, &smr) != 0 ||
		    gDbyCtxIo.read32(gDbyCtxIo.cookie, 0xC00u + 4u * i, &s2cr) != 0) {
			gDbyGpuCpRc = 0xFFFFFF06u;
			goto render;
		}
		gDbyGpuSlotSmr[i] = smr;
		gDbyGpuSlotS2cr[i] = s2cr;
		if ((s2cr & 0xFFFFu) == 0u && ((s2cr >> 16) & 0x3u) == 2u) {
			continue;                       /* already the canonical FAULT word */
		}
		if (gDbyCtxIo.write32(gDbyCtxIo.cookie, 0xC00u + 4u * i, DBY_GMU_SLOT_FAULT) != 0 ||
		    gDbyCtxIo.barrier(gDbyCtxIo.cookie) != 0) {
			gDbyGpuCpRc = 0xFFFFFF06u;
			goto render;
		}
		s2cr = 0;
		if (gDbyCtxIo.read32(gDbyCtxIo.cookie, 0xC00u + 4u * i, &s2cr) != 0 ||
		    (s2cr & 0xFFFFu) != 0u || ((s2cr >> 16) & 0x3u) != 2u) {
			gDbyGpuCpRc = 0xFFFFFF06u;
			goto render;
		}
		gDbyGpuSlotNormalized++;
	}
	memset(&sr, 0, sizeof sr);
	gDbyGpuCtxStage[0] = (uint32_t)ArmSmmu4KCheckFaultedStreams(&gDbyCtxIo, slots, DBY_GPU_SLOT_COUNT,
	                                                            DBY_GPU_BANK, &sr);
	if (gDbyGpuCtxStage[0] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF06u;
		goto render;
	}
	(void)gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_GFSR, &gDbyGpuGfsr);
	(void)gDbyCtxIo.read32(gDbyCtxIo.cookie, A650_SMMU_GFSYNR1, &gDbyGpuGfSid);
	gDbyGpuCtxConfig.input_bits = 36u;
	gDbyGpuCtxConfig.output_bits = 36u;
	gDbyGpuCtxConfig.table_physical = gDbyGpuPoolPhys;
	gDbyGpuCtxConfig.leaf_budget = DBY_SMMU_LEAF_BUDGET;
	/* The SQE is fetched and executed by the CP (Read|Execute, 5); the ring, the memptrs page and the
	 * scratch page are read and written (3).  Non-cacheable and outer shareable, the same device-side
	 * view the GMU context gives its regions, because every buffer's CPU view is uncached too. */
	dbyCtxRange(&ranges[0], DBY_CPB_SQE_IOVA, gDbyGpuBuf[0].phys, DBY_CPB_SQE_BYTES, 5u);
	dbyCtxRange(&ranges[1], DBY_CPB_RING_IOVA, gDbyGpuBuf[1].phys, DBY_CPB_RING_BYTES, 3u);
	dbyCtxRange(&ranges[2], DBY_CPB_MEMPTRS_IOVA, gDbyGpuBuf[2].phys, DBY_CPB_PAGE_BYTES, 3u);
	dbyCtxRange(&ranges[3], DBY_CPB_SCRATCH_IOVA, gDbyGpuBuf[3].phys, DBY_CPB_PAGE_BYTES, 3u);
	gDbyGpuCtxStage[1] = (uint32_t)ArmSmmu4KBuild(&gDbyGpuCtxConfig, ranges, 4u,
	                                              (uint64_t *)gDbyGpuPoolCpu,
	                                              (size_t)(DBY_SMMU_GPU_POOL_BYTES / 4096u), &gDbyGpuCtxBuild);
	if (gDbyGpuCtxStage[1] == 0u) {
		gDbyGpuCtxStage[1] = dbyGpuCtxPublishTables((uint64_t)gDbyGpuCtxBuild.table_pages);
	}
	if (gDbyGpuCtxStage[1] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF07u;
		goto render;
	}
	/* The kit reads ID0/ID1/ID2 at GR0 0x20/0x24/0x28 (ArmSmmu4KStreams.c:16, ArmSmmu4KProgram.c:72-73).
	 * 0x0 and 0x8 are sCR0 and sCR2, and an id0 without bit 30 makes EncodeContext refuse
	 * (ArmSmmu4KContext.c:35) - the first version of this block did exactly that. */
	if (gDbyCtxIo.read32(gDbyCtxIo.cookie, 0x020u, &id0) != 0 ||
	    gDbyCtxIo.read32(gDbyCtxIo.cookie, 0x028u, &id2) != 0) {
		gDbyGpuCpRc = 0xFFFFFF08u;
		goto render;
	}
	gDbyGpuCtxContext.lower = &gDbyGpuCtxConfig;
	gDbyGpuCtxContext.upper = nullptr;
	gDbyGpuCtxContext.lower_tables = &gDbyGpuCtxBuild;
	gDbyGpuCtxContext.upper_tables = nullptr;
	gDbyGpuCtxContext.id0 = id0;
	gDbyGpuCtxContext.id2 = id2;
	/* Both words were read from GR0 0x020/0x028 and are about to be encoded and
	 * validated; the G-1 row renders them from here instead of re-reading. */
	gDbyGpuCtxIdsOk = 1u;
	gDbyGpuCtxContext.asid = DBY_GPU_ASID;
	gDbyGpuCtxContext.coherent_walk = 1u;
	gDbyGpuCtxContext.stall_on_fault = 0u;
	gDbyGpuCtxContext.continue_after_fault = 1u;
	gDbyGpuCtxStage[2] = (uint32_t)ArmSmmu4KEncodeContext(&gDbyGpuCtxContext, &image);
	if (gDbyGpuCtxStage[2] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF09u;
		goto render;
	}
	gDbyGpuCtxStep = 5u;
	memset(&prog, 0, sizeof prog);
	gDbyGpuCtxStage[3] = (uint32_t)ArmSmmu4KProgramDisabledContext(&gDbyCtxIo, DBY_GPU_BANK,
	                                                               &gDbyGpuCtxContext, 1000u, &prog);
	if (gDbyGpuCtxStage[3] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF0Au;
		goto render;
	}
	gDbyGpuCtxStep = 6u;
	memset(&gDbyGpuCtxStreams, 0, sizeof gDbyGpuCtxStreams);
	memset(&sr, 0, sizeof sr);
	gDbyGpuCtxStage[4] = (uint32_t)ArmSmmu4KAttachStreams(&gDbyCtxIo, &gDbyGpuCtxStreams, slots,
	                                                      DBY_GPU_SLOT_COUNT, DBY_GPU_BANK,
	                                                      &gDbyGpuCtxContext, &sr);
	if (gDbyGpuCtxStage[4] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF0Bu;
		goto render;
	}
	gDbyGpuCtxStep = 7u;
	gDbyGpuCtxStage[5] = (uint32_t)ArmSmmu4KSyncContextTlb(&gDbyCtxIo, prog.context_offset,
	                                                        DBY_GPU_ASID, 0u, 1000u, &prog);
	if (gDbyGpuCtxStage[5] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF0Cu;
		goto render;
	}
	memset(&obs, 0, sizeof obs);
	gDbyGpuCtxStage[6] = (uint32_t)ArmSmmu4KObserveOwnedContext(&gDbyCtxIo, DBY_GPU_BANK,
	                                                            &gDbyGpuCtxContext, &obs);
	if (gDbyGpuCtxStage[6] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF0Du;
		goto render;
	}
	gDbyGpuTtbr0 = obs.readback.ttbr0;
	gDbyGpuCtxStage[7] = (uint32_t)ArmSmmu4KValidateContextReadback(&gDbyGpuCtxContext, &obs.readback);
	if (gDbyGpuCtxStage[7] != 0u) {
		gDbyGpuCpRc = 0xFFFFFF0Eu;
		goto render;
	}

	/* arm_mmu500_reset: MMU-500 errata 841119/826419 want CB0 ACTLR.CPRE (bit 1) clear, then TLBIALL and
	 * a bounded TLBSYNC of the bank (pipa GpuProbe.cpp:543-552, whose row prints the value as actlr 3).
	 * The ArmSmmu4K kit never writes ACTLR (ArmSmmu4KProgram.h:31 mentions it only in prose), so the
	 * board does it here, after the bank is enabled and before any GPU traffic.  The offsets are the
	 * bank-relative SMMUv2 context registers: ACTLR 0x4, CB_TLBIALL 0x618, CB_TLBSYNC 0x7f0,
	 * CB_TLBSTATUS 0x7f4, all against the context offset the kit reported. */
	gDbyGpuCpStep = 8u;
	{
		uint32_t actlr = 0;
		uint32_t tlb = 0;
		unsigned poll;
		unsigned synced = 0u;

		gDbyGpuCtxActlr = 0u;
		if (gDbyCtxIo.read32(gDbyCtxIo.cookie, (size_t)prog.context_offset + 0x4u, &actlr) != 0 ||
		    actlr == 0xFFFFFFFFu) {
			gDbyGpuCpRc = 0xFFFFFF0Fu;
			goto render;
		}
		gDbyGpuCtxActlr = actlr;
		if (gDbyCtxIo.write32(gDbyCtxIo.cookie, (size_t)prog.context_offset + 0x4u, actlr & ~2u) != 0 ||
		    gDbyCtxIo.barrier(gDbyCtxIo.cookie) != 0 ||
		    gDbyCtxIo.write32(gDbyCtxIo.cookie, (size_t)prog.context_offset + 0x618u, 0u) != 0 ||
		    gDbyCtxIo.barrier(gDbyCtxIo.cookie) != 0 ||
		    gDbyCtxIo.write32(gDbyCtxIo.cookie, (size_t)prog.context_offset + 0x7F0u, 0u) != 0) {
			gDbyGpuCpRc = 0xFFFFFF0Fu;
			goto render;
		}
		/* A failed TLBSTATUS read leaves tlb at 0; only an observed clear bit counts as a completed
		 * sync, so the read result gates the flag (ArmSmmu4KProgram.c:35 treats it as an error). */
		for (poll = 0; poll < 1000u; poll++) {
			if (gDbyCtxIo.read32(gDbyCtxIo.cookie, (size_t)prog.context_offset + 0x7F4u, &tlb) != 0) {
				break;
			}
			if ((tlb & 1u) == 0u) {
				synced = 1u;
				break;
			}
			IODelay(1u);
		}
		if (synced == 0u) {
			gDbyGpuCpRc = 0xFFFFFF0Fu;
			goto render;
		}
	}

	/* The GPU's own registers are only reachable with GX on, which the gate above established. */
	gDbyGpuCpStep = 9u;
	if (dbyGpuHwInit() != 0) {
		gDbyGpuCpRc = 0xFFFFFF10u;
		goto render;
	}

	gDbyGpuCpStep = 10u;
	if (dbyGpuCpW64(DBY_CP_SQE_INSTR_BASE, DBY_CPB_SQE_IOVA) != 0) {
		gDbyGpuCpRc = 0xFFFFFF11u;
		goto render;
	}
	/* Ring attach, as the current fork's program() writes it (NativeAdrenoRing.cpp:358-388): CP_RB_BASE is
	 * the ring's GPU address (raw, not shifted), CP_RB_CNTL = ilog2(bytes / 8) | ilog2(32 / 8) << 8 and
	 * NO_UPDATE CLEAR, because this A650 has hardware APRIV (mainline only sets NO_UPDATE when it has
	 * none, a6xx_gpu.c:1452-1456); the RPTR shadow address is the memptrs page and CP_SCRATCH(2) is 0.
	 * CP_RB_WPTR is deliberately NOT written here: mainline never zeroes it, and writing 0 leaves the
	 * SQE parked with nothing to fetch (NativeAdrenoRing.cpp:377-380, fix63). */
	control = (dbyGpuIl2(DBY_CPB_RING_BYTES / 8u) & 0x3Fu) |
	          ((dbyGpuIl2(32u / 8u) & 0x3Fu) << 8);
	if (dbyGpuCpW64(DBY_CP_RB_BASE, DBY_CPB_RING_IOVA) != 0 ||
	    dbyGpuCpW(DBY_CP_RB_CNTL, control) != 0 ||
	    dbyGpuCpW64(DBY_CP_RB_RPTR_ADDR, DBY_CPB_MEMPTRS_IOVA) != 0 ||
	    dbyGpuCpW(DBY_CP_SCRATCH2, 0u) != 0) {
		gDbyGpuCpRc = 0xFFFFFF12u;
		goto render;
	}
	/* With hardware APRIV the authority is the RPTR shadow the CP writes into the memptrs page, not
	 * CP_RB_RPTR (NativeAdrenoRing.cpp:130-145 reads fence_[0] for exactly this reason).  A shadow past
	 * the ring would make every offset wrong, so it is refused rather than trusted. */
	gDbyGpuCpRptr = gDbyGpuBuf[2].cpu[0];
	if (gDbyGpuCpRptr >= DBY_CPB_RING_BYTES / 4u) {
		gDbyGpuCpRc = 0xFFFFFF13u;
		goto render;
	}
	if (dbyGpuCpW(DBY_CP_SQE_CNTL, 1u) != 0) {
		gDbyGpuCpRc = 0xFFFFFF14u;
		goto render;
	}

	/* CP_ME_INIT, the eight a6xx words (mask 0x2f, three hardware contexts, 0x20000000 error detection,
	 * then zeros) and a fence. */
	gDbyGpuCpStep = 11u;
	{
		uint32_t words[32];

		if (AdrenoPM4Type7(DBY_PM4_ME_INIT, 8u, &words[0]) != AdrenoPM4Success) {
			gDbyGpuCpRc = 0xFFFFFF15u;
			goto render;
		}
		words[1] = 0x2Fu;
		words[2] = 3u;
		words[3] = 0x20000000u;
		for (i = 4; i < 9u; i++) {
			words[i] = 0u;
		}
		at = dbyGpuCpAppendFence(words, 9u, 1u);
		if (at == 0u || gDbyGpuCpRptr + at > DBY_CPB_RING_BYTES / 4u) {
			gDbyGpuCpRc = 0xFFFFFF15u;
			goto render;
		}
		for (i = 0; i < at; i++) {
			gDbyGpuBuf[1].cpu[gDbyGpuCpRptr + i] = words[i];
		}
		__asm__ volatile("dsb sy" ::: "memory");
		gDbyGpuCpWptr = gDbyGpuCpRptr + at;
		if (dbyGpuCpWait(1u, &gDbyGpuCpMeInitFence) != 0u) {
			gDbyGpuCpRc = 0xFFFFFF16u;
			goto render;
		}
	}

	/* The first command: CP_MEM_WRITE into the scratch page, then a second fence. */
	gDbyGpuCpStep = 12u;
	{
		uint32_t words[16];
		uint32_t first = gDbyGpuCpWptr;

		if (AdrenoPM4Type7(DBY_PM4_MEM_WRITE, 3u, &words[0]) != AdrenoPM4Success) {
			gDbyGpuCpRc = 0xFFFFFF17u;
			goto render;
		}
		words[1] = (uint32_t)DBY_CPB_SCRATCH_IOVA;
		words[2] = (uint32_t)(DBY_CPB_SCRATCH_IOVA >> 32);
		words[3] = DBY_CPB_MARKER;
		at = dbyGpuCpAppendFence(words, 4u, 2u);
		if (at == 0u || first + at > DBY_CPB_RING_BYTES / 4u) {
			gDbyGpuCpRc = 0xFFFFFF17u;
			goto render;
		}
		for (i = 0; i < at; i++) {
			gDbyGpuBuf[1].cpu[first + i] = words[i];
		}
		__asm__ volatile("dsb sy" ::: "memory");
		gDbyGpuCpWptr = first + at;
		if (dbyGpuCpWait(2u, &gDbyGpuCpMemWriteFence) != 0u) {
			gDbyGpuCpRc = 0xFFFFFF18u;
			goto render;
		}
		gDbyGpuCpMarker = gDbyGpuBuf[3].cpu[0];
		if (gDbyGpuCpMarker != DBY_CPB_MARKER) {
			gDbyGpuCpRc = 0xFFFFFF19u;
			goto render;
		}
	}
	gDbyGpuCpRc = 0u;

render:
	/* The CP's own view, read once whatever the outcome: RPTR (did it consume the ring?), HW_FAULT and
	 * the interrupt status are what separate "the CP never started" from "it faulted on a packet". */
	gDbyGpuCpRptr = gDbyGpuBuf[2].cpu[0];
	(void)dbyGpuCpRead(DBY_CP_HW_FAULT, &gDbyGpuCpHwFault);
	(void)dbyGpuCpRead(DBY_CP_INTERRUPT_STATUS, &gDbyGpuCpIntStatus);
}

/* G-1: the GPU's address-size code, taken from the identity words the GPU context encode
 * already read and proved.
 *
 * NativeAdrenoSmmuMemory decodes ID2[11:8] as {32,36,40,42,44} else 48 and refuses every
 * non-kernel client when the result is <= 32, so the publish gate needs that number measured.
 * The ladder has it: dbyGpuCpStart reads ID0 at GR0 0x020 and ID2 at 0x028 (:2704-2705) and
 * stores them in gDbyGpuCtxContext (:2713-2714); the encode at :2722 and the read-back
 * validation at :2760 only pass if they are coherent, and the encode additionally requires
 * this board's input_bits (36, :2680) to fit in va, so a rendered row already proves >= 36.
 * Reading the block a second time would add an MMIO access this boot does not need; the first
 * version of this change did exactly that and used sCR0/sCR1/sCR2 offsets by mistake (review
 * 2026-11-06, must-fix 1).
 */

static void
renderCp(void)
{
	char *row = native_gpupower_rows[2];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU CP st=");
	at = putHex8(at, end, gDbyGpuCpRc);
	at = putStr(at, end, " step=");
	at = putDec(at, end, gDbyGpuCpStep);
	at = putStr(at, end, " sqe=");
	at = putHex8(at, end, gDbyGpuCpSqeBytes);
	at = putStr(at, end, "/");
	at = putDec(at, end, gDbyGpuCpSqeBad);
	at = putStr(at, end, " rptr=");
	at = putHex8(at, end, gDbyGpuCpRptr);
	at = putStr(at, end, " wptr=");
	at = putHex8(at, end, gDbyGpuCpWptr);
	at = putStr(at, end, " me=");
	at = putDec(at, end, gDbyGpuCpMeInitFence);
	at = putStr(at, end, " mw=");
	at = putDec(at, end, gDbyGpuCpMemWriteFence);
	at = putStr(at, end, " hf=");
	at = putHex8(at, end, gDbyGpuCpHwFault);
	at = putStr(at, end, " mark=");
	at = putHex8(at, end, gDbyGpuCpMarker);
	at = putStr(at, end, " nrm=");
	at = putDec(at, end, gDbyGpuSlotNormalized);
	*at = 0;

	row = native_gpupower_rows[3];
	end = row + 119;
	at = putStr(row, end, "GPU GPUCTX");
	at = putStage(at, end, "pre", gDbyGpuCtxStage[0]);
	at = putStage(at, end, "bld", gDbyGpuCtxStage[1]);
	at = putStage(at, end, "enc", gDbyGpuCtxStage[2]);
	at = putStage(at, end, "prog", gDbyGpuCtxStage[3]);
	at = putStage(at, end, "str", gDbyGpuCtxStage[4]);
	at = putStage(at, end, "tlb", gDbyGpuCtxStage[5]);
	at = putStage(at, end, "obs", gDbyGpuCtxStage[6]);
	at = putStage(at, end, "val", gDbyGpuCtxStage[7]);
	at = putStr(at, end, " pg=");
	at = putDec(at, end, (uint32_t)gDbyGpuCtxBuild.table_pages);
	at = putStr(at, end, " rw=");
	at = putHex8(at, end, gDbyGpuCpRegFail);
	at = putStr(at, end, " actlr=");
	at = putHex4(at, end, gDbyGpuCtxActlr);
	at = putStr(at, end, " ttbr=");
	at = putHex16(at, end, gDbyGpuTtbr0);
	*at = 0;

	/* A preflight refusal names no slot (ArmSmmu4KBusy), so on that path the four slots and sGFSR
	 * replace the cold-start and GX rows: those were already proven by mode 13 on this chain. */
	if (gDbyGpuCpRc == 0xFFFFFF06u) {
		row = native_gpupower_rows[4];
		end = row + 119;
		at = putStr(row, end, "GPU SLOT s0 ");
		at = putHex8(at, end, gDbyGpuSlotSmr[0]);
		at = putStr(at, end, "/");
		at = putHex8(at, end, gDbyGpuSlotS2cr[0]);
		at = putStr(at, end, " s1 ");
		at = putHex8(at, end, gDbyGpuSlotSmr[1]);
		at = putStr(at, end, "/");
		at = putHex8(at, end, gDbyGpuSlotS2cr[1]);
		*at = 0;

		row = native_gpupower_rows[5];
		end = row + 119;
		at = putStr(row, end, "GPU SLOT s2 ");
		at = putHex8(at, end, gDbyGpuSlotSmr[2]);
		at = putStr(at, end, "/");
		at = putHex8(at, end, gDbyGpuSlotS2cr[2]);
		at = putStr(at, end, " s3 ");
		at = putHex8(at, end, gDbyGpuSlotSmr[3]);
		at = putStr(at, end, "/");
		at = putHex8(at, end, gDbyGpuSlotS2cr[3]);
		at = putStr(at, end, " gf=");
		at = putHex8(at, end, gDbyGpuGfsr);
		at = putStr(at, end, " gn=");
		at = putHex8(at, end, gDbyGpuGfSid);
		*at = 0;
	}
}

/* --- mode 15: the stock secure-mode exit - this device's a650_zap through PAS 13 -------------------
 *
 * Mode 14 proved the CP executes and writes memory, but the GPU is still in the secure mode the
 * bootloader/TZ put it in.  The stock sequence, the same one a6xx_rb_start ends with
 * (adreno_a6xx.c:894-933, adreno.c:2001-2021), is what this mode runs:
 *   1. qcom_scm_pas_supported(13) and qcom_scm_is_call_available(PAS init_image), read-only.  A zero in
 *      pas_supported's a1 is ADVISORY only: kgsl never calls it (qcom_wcnss.c:480 is the only caller in
 *      the vendor tree), so it does not mean init_image would fail.
 *   2. RBBM_SECVID_TRUST_CONTROL (0xF400) is read for the record and never written: the vendor's
 *      no-zap write of 0 stalls this side (evidence/edl-log-s77, prog=4), and the stock path does not
 *      need it.
 *   3. dbyZapLoad: qcom_scm PAS init_image, mem_setup, the segments into pil_gpu_region@8681a000,
 *      auth_and_reset - with THIS device's /vendor/firmware/a650_zap.elf (DbyGpuFirmware.h).
 *   4. dbyZapLeaveSecure: CP_SET_SECURE_MODE(0) through the ring mode 14 started, fenced (seqno 3).
 */

#define DBY_ZAP_PAS_ID          13u
#define DBY_ZAP_REGION_PHYS     0x8681a000ull
#define DBY_ZAP_REGION_BYTES    0x2000u
#define DBY_ZAP_SECURE_OPCODE   0x66u
#define DBY_SCM_PIL(cmd)        (0x42000000ull | (0x02ull << 8) | (cmd))
#define DBY_SCM_INIT_IMAGE      0x01u
#define DBY_SCM_MEM_SETUP       0x02u
#define DBY_SCM_AUTH_AND_RESET  0x05u

typedef struct {
	int64_t a0;
	uint64_t a1;
} DbyScmRes;

static unsigned gDbyScmResumes;
static uint32_t gDbyZapRc;
/* RBBM_SECVID_TRUST_CONTROL (0xF400) as found.  The vendor only ever writes 0 there (adreno.c:2039-2043),
 * so the meaning of a non-zero value is UNVERIFIED. */
static uint32_t gDbyZapTrustBefore;
/* The phase marker row 3 carries while this mode runs: 0 = before the first read, 3 = before the
 * second (see dbyZapMark). */
static unsigned gDbyZapProg;
/* State of the PAS load (work/verify/zap-pas-code.txt): the SMC return pairs, the ELF facts
 * and the CP secure-mode fence. */
static unsigned gDbyZapStep;
static int64_t gDbyZapInit0, gDbyZapInit1;
static int64_t gDbyZapMem0, gDbyZapMem1;
static int64_t gDbyZapAuth0, gDbyZapAuth1;
static uint32_t gDbyZapPhnum, gDbyZapReloc, gDbyZapSpan;
static uint32_t gDbyZapMetaPages;
static uint32_t gDbyZapMs[3];
static unsigned gDbyZapSecRc;
static uint32_t gDbyZapSecFence;
static uint32_t gDbyZapSecWptr;
static IOBufferMemoryDescriptor *gDbyZapMeta;
/* The mode-16 TrustZone queries, repeated here so one boot carries both answers. */
static uint32_t gDbyTzAvail0, gDbyTzAvail1;
static uint32_t gDbyTzSup0, gDbyTzSup1;
static uint64_t gDbyZapMetaPhys;

extern boolean_t ml_set_interrupts_enabled(boolean_t);

/* The SIP SMC exactly as the reference board issues it (GpuProbe.cpp:839-861, from qcom_scm-smc.c):
 * x0 function, x1 arginfo, x2.. arguments; x0 == 1 is INTERRUPTED and resumes with x6, V2_EBUSY (-12)
 * retries after 30 ms.  Interrupts stay masked across the call and its resumes. */
static DbyScmRes
dbyScmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2)
{
	boolean_t was;
	DbyScmRes out;
	uint64_t first = fn;
	uint64_t a6 = 0;
	unsigned resume = 0u;
	unsigned busy = 0u;

	out.a0 = -1000;
	out.a1 = 0u;
	was = ml_set_interrupts_enabled(FALSE);
	gDbyScmResumes = 0u;
	while (resume < 10000u) {
		register uint64_t x0 __asm__("x0") = first;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = p0;
		register uint64_t x3 __asm__("x3") = p1;
		register uint64_t x4 __asm__("x4") = p2;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = a6;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
		if (x0 == 1u) {
			first = 1u;
			a6 = x6;
			resume++;
			gDbyScmResumes = resume;
			continue;
		}
		if ((int64_t)x0 == -12 && busy++ < 20u) {
			first = fn;
			a6 = 0u;
			IODelay(30000u);
			continue;
		}
		out.a0 = (int64_t)x0;
		out.a1 = x1;
		break;
	}
	ml_set_interrupts_enabled(was);
	return out;
}

static uint32_t
dbyLe32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
dbyLe16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Writeback of the metadata to the point of coherency before TrustZone reads it (dma_alloc_coherent on
 * the Linux side); the loop is the reference board's cleanToPoc (GpuProbe.cpp:868-874). */
static void
dbyCleanToPoc(const void *p, size_t len)
{
	uint64_t ctr;
	size_t line;
	uintptr_t at;
	uintptr_t end;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = (size_t)4u << ((ctr >> 16) & 15u);
	for (at = (uintptr_t)p & ~(line - 1u), end = (uintptr_t)p + len; at < end; at += line) {
		__asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* One line per phase, written before the phase starts: if the thread stops inside it, the log keeps
 * the number of the phase it entered (mode 15 round 4, after the <4 GiB allocation stopped the run
 * with no ZAP row at all). */
static void
dbyZapMark(unsigned step)
{
	char *row = native_gpupower_rows[3];
	const char *end = row + 119;
	char *at = row;

	gDbyZapProg = step;
	at = putStr(at, end, "GPU ZAP prog=");
	at = putDec(at, end, step);
	at = putStr(at, end, " meta=");
	at = putHex8(at, end, (uint32_t)(gDbyZapMetaPhys >> 32));
	at = putHex8(at, end, (uint32_t)(gDbyZapMetaPhys & 0xFFFFFFFFu));
	*at = 0;
}

static uint32_t
dbyZapMs(uint64_t start)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return (uint32_t)(ns / 1000000u);
}

/* qcom_mdt_load for the zap shader (GpuProbe.cpp:894-984): the ELF header and the hash segment as the
 * metadata for init_image, mem_setup for a relocatable image, the loadable segments copied into the
 * reserved region, then auth_and_reset.  Returns 0 only when TrustZone accepted the image. */
static uint32_t
dbyZapLoad(void)
{
	const uint8_t *fw;
	uint64_t size;
	uint32_t phoff;
	uint32_t phent;
	uint32_t phnum;
	uint32_t hash = 0u;
	uint64_t lo = ~0ull;
	uint64_t hi = 0u;
	unsigned relocate = 0u;
	uint64_t ehdr;
	uint64_t hashBytes;
	uint64_t hashOff;
	size_t metaBytes;
	uint8_t *meta;
	uint64_t metaPhys;
	DbyScmRes r;
	uint64_t start;
	unsigned i;

	gDbyZapStep = 1u;
	gDbyZapRc = 0xFFFFFF20u;
	dbyZapMark(1u);
	if (native_gpu_zap_firmware == nullptr || native_gpu_zap_firmware_size < 52u) {
		return 0xFFFFFF21u;                     /* no embedded image */
	}
	fw = native_gpu_zap_firmware;
	size = native_gpu_zap_firmware_size;
	if (fw[0] != 0x7Fu || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1u) {
		return 0xFFFFFF22u;                     /* not a 32-bit little-endian ELF */
	}
	phoff = dbyLe32(fw + 28);
	phent = dbyLe16(fw + 42);
	phnum = dbyLe16(fw + 44);
	gDbyZapPhnum = phnum;
	if (phent != 32u || phnum < 2u || phnum > 16u ||
	    (uint64_t)phoff + (uint64_t)phnum * 32u > size) {
		return 0xFFFFFF23u;
	}
	for (i = 1u; i < phnum && hash == 0u; i++) {
		if (((dbyLe32(fw + phoff + i * 32u + 24u) >> 24) & 7u) == 2u) {
			hash = i;
		}
	}
	if (hash == 0u) {
		return 0xFFFFFF24u;                     /* no hash segment: TZ cannot authenticate it */
	}
	for (i = 0u; i < phnum; i++) {
		uint32_t type = dbyLe32(fw + phoff + i * 32u + 0u);
		uint32_t off = dbyLe32(fw + phoff + i * 32u + 4u);
		uint32_t pa = dbyLe32(fw + phoff + i * 32u + 12u);
		uint32_t filesz = dbyLe32(fw + phoff + i * 32u + 16u);
		uint32_t flags = dbyLe32(fw + phoff + i * 32u + 24u);
		uint64_t end;

		if (type != 1u || ((flags >> 24) & 7u) == 2u || filesz == 0u) {
			continue;                       /* mdt_phdr_loadable */
		}
		if ((flags & (1u << 27)) != 0u) {
			relocate = 1u;
		}
		if ((uint64_t)pa < lo) {
			lo = pa;
		}
		end = ((uint64_t)pa + dbyLe32(fw + phoff + i * 32u + 20u) + 0xFFFu) & ~0xFFFull;
		if (end > hi) {
			hi = end;
		}
		if (off + filesz > size) {
			return 0xFFFFFF25u;
		}
	}
	gDbyZapReloc = relocate;
	if (lo >= hi || hi - lo > DBY_ZAP_REGION_BYTES) {
		return 0xFFFFFF26u;                     /* does not fit the reserved region */
	}
	gDbyZapSpan = (uint32_t)(hi - lo);
	/* ph0.p_filesz is at +16 (p_type 0, p_offset 4, p_vaddr 8, p_paddr 12, p_filesz 16): the ELF
	 * header and program headers TrustZone parses.  +4 would read p_offset, which is 0 here, and the
	 * metadata would carry only the hash segment (review 2026-11-05 12:35, M1). */
	ehdr = dbyLe32(fw + phoff + 16u);
	hashOff = dbyLe32(fw + phoff + hash * 32u + 4u);
	hashBytes = dbyLe32(fw + phoff + hash * 32u + 16u);
	if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000u) {
		return 0xFFFFFF27u;
	}

	/* The metadata: ELF header plus hash segment, physically contiguous, written back before the SMC. */
	gDbyZapStep = 2u;
	dbyZapMark(2u);
	metaBytes = (size_t)((ehdr + hashBytes + 0xFFFu) & ~0xFFFull);
	gDbyZapMetaPages = (uint32_t)(metaBytes >> 12);
	/* The reference board's 40-bit mask only: the <4 GiB attempt of round 3 stopped this thread
	 * (evidence/edl-log-s73, no ZAP row at all), the same shape pipa recorded for a below-4-GiB
	 * allocation (GpuProbe.cpp:927-930).  The address hypothesis is tested by mode 16 instead, which
	 * runs before anything else and has its own bounded attempt. */
	gDbyZapMeta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
	    metaBytes, 0xFFFFFFF000ull);
	if (gDbyZapMeta == nullptr || gDbyZapMeta->prepare() != kIOReturnSuccess) {
		return 0xFFFFFF28u;
	}
	meta = static_cast<uint8_t *>(gDbyZapMeta->getBytesNoCopy());
	metaPhys = (uint64_t)gDbyZapMeta->getPhysicalSegment(0, nullptr);
	gDbyZapMetaPhys = metaPhys;
	if (meta == nullptr || metaPhys == 0u) {
		return 0xFFFFFF29u;
	}
	for (i = 0u; i < (unsigned)ehdr; i++) {
		meta[i] = fw[i];
	}
	for (i = 0u; i < (unsigned)hashBytes; i++) {
		meta[ehdr + i] = fw[hashOff + i];
	}
	dbyCleanToPoc(meta, metaBytes);

	/* qcom_scm_pas_init_image: ARGS(2, VAL, RW). */
	gDbyZapStep = 3u;
	dbyZapMark(3u);
	start = mach_absolute_time();
	r = dbyScmCall(DBY_SCM_PIL(DBY_SCM_INIT_IMAGE), 2u | (2u << 6), DBY_ZAP_PAS_ID, metaPhys, 0u);
	gDbyZapInit0 = r.a0;
	gDbyZapInit1 = (int64_t)r.a1;
	gDbyZapMs[0] = dbyZapMs(start);
	if (r.a0 != 0 || r.a1 != 0u) {
		return 0xFFFFFF2Au;
	}
	if (relocate != 0u) {
		/* qcom_scm_pas_mem_setup: ARGS(3). */
		gDbyZapStep = 4u;
		dbyZapMark(4u);
		start = mach_absolute_time();
		r = dbyScmCall(DBY_SCM_PIL(DBY_SCM_MEM_SETUP), 3u, DBY_ZAP_PAS_ID, DBY_ZAP_REGION_PHYS,
		               hi - lo);
		gDbyZapMem0 = r.a0;
		gDbyZapMem1 = (int64_t)r.a1;
		gDbyZapMs[1] = dbyZapMs(start);
		if (r.a0 != 0 || r.a1 != 0u) {
			return 0xFFFFFF2Bu;
		}
	}

	/* qcom_mdt_load_no_init: the loadable segments into the reserved region (relocatable: offsets from
	 * the lowest segment address), then the CPU view is gone before TrustZone takes the region. */
	/* The reference board refuses a region that lies inside the memory XNU hands out (GpuProbe.cpp:900,
	 * vm_first_phys/vm_last_phys).  This region is the DT's own no-map reservation
	 * (9707f.dts:34628-34633) and the UEFI reserves /reserved-memory before the kernel sees the map, so
	 * XNU never owns it; the two globals are not otherwise used in this tree, so the check is not
	 * repeated and the reservation is the evidence instead. */
	gDbyZapStep = 5u;
	dbyZapMark(5u);
	{
		IODeviceMemory *memory = IODeviceMemory::withRange(DBY_ZAP_REGION_PHYS, DBY_ZAP_REGION_BYTES);
		OSSharedPtr<IOMemoryMap> map;

		if (memory != nullptr) {
			map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
			memory->release();
		}
		if (!map) {
			return 0xFFFFFF2Cu;
		}
		{
			volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
			uint64_t base = relocate != 0u ? lo : DBY_ZAP_REGION_PHYS;

			for (i = 0u; i < phnum; i++) {
				uint32_t type = dbyLe32(fw + phoff + i * 32u + 0u);
				uint32_t off = dbyLe32(fw + phoff + i * 32u + 4u);
				uint32_t pa = dbyLe32(fw + phoff + i * 32u + 12u);
				uint32_t filesz = dbyLe32(fw + phoff + i * 32u + 16u);
				uint32_t memsz = dbyLe32(fw + phoff + i * 32u + 20u);
				uint32_t flags = dbyLe32(fw + phoff + i * 32u + 24u);
				uint64_t at;
				uint64_t b;

				if (type != 1u || ((flags >> 24) & 7u) == 2u || filesz == 0u) {
					continue;
				}
				if ((uint64_t)pa < base || (uint64_t)pa - base + memsz > DBY_ZAP_REGION_BYTES ||
				    filesz > memsz || (uint64_t)off + filesz > size) {
					return 0xFFFFFF2Du;
				}
				at = (uint64_t)pa - base;
				for (b = 0; b < filesz; b++) {
					region[at + b] = fw[off + b];
				}
				for (b = filesz; b < memsz; b++) {
					region[at + b] = 0u;
				}
			}
			__asm__ volatile("dsb sy" ::: "memory");
		}
	}

	/* qcom_scm_pas_auth_and_reset: ARGS(1). */
	gDbyZapStep = 6u;
	dbyZapMark(6u);
	start = mach_absolute_time();
	r = dbyScmCall(DBY_SCM_PIL(DBY_SCM_AUTH_AND_RESET), 1u, DBY_ZAP_PAS_ID, 0u, 0u);
	gDbyZapAuth0 = r.a0;
	gDbyZapAuth1 = (int64_t)r.a1;
	gDbyZapMs[2] = dbyZapMs(start);
	if (r.a0 != 0 || r.a1 != 0u) {
		return 0xFFFFFF2Eu;
	}
	gDbyZapStep = 7u;
	dbyZapMark(7u);
	return 0u;
}

/* The CP has to be told the shader ran: CP_SET_SECURE_MODE(0) through the ring it is already running,
 * with a fence, exactly as the reference board submits it (GpuProbe.cpp:986-993). */
static uint32_t
dbyZapLeaveSecure(void)
{
	uint32_t words[16];
	unsigned at;
	unsigned n;
	unsigned i;

	if (AdrenoPM4Type7(DBY_ZAP_SECURE_OPCODE, 1u, &words[0]) != AdrenoPM4Success) {
		return 0xFFFFFF2Fu;
	}
	words[1] = 0u;
	n = dbyGpuCpAppendFence(words, 2u, 3u);
	if (n == 0u || gDbyGpuCpWptr + n > DBY_CPB_RING_BYTES / 4u) {
		return 0xFFFFFF2Fu;
	}
	for (i = 0; i < n; i++) {
		gDbyGpuBuf[1].cpu[gDbyGpuCpWptr + i] = words[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");
	gDbyGpuCpWptr += n;
	gDbyZapSecWptr = gDbyGpuCpWptr;
	gDbyZapSecRc = dbyGpuCpWait(3u, &gDbyZapSecFence);
	(void)dbyGpuCpRead(DBY_CP_HW_FAULT, &gDbyGpuCpHwFault);
	if (gDbyZapSecRc != 0u) {
		return 0xFFFFFF30u;
	}
	return 0u;
}


static void
dbyGpuZapStart(void)
{
	gDbyZapStep = 0u;
	gDbyZapRc = 0xFFFFFF20u;
	gDbyZapInit0 = gDbyZapInit1 = 0;
	gDbyZapMem0 = gDbyZapMem1 = 0;
	gDbyZapAuth0 = gDbyZapAuth1 = 0;
	gDbyZapPhnum = 0u;
	gDbyZapReloc = 0u;
	gDbyZapSpan = 0u;
	gDbyZapMetaPages = 0u;
	gDbyZapMs[0] = gDbyZapMs[1] = gDbyZapMs[2] = 0u;
	gDbyZapSecRc = 0u;
	gDbyZapSecFence = 0u;
	gDbyZapSecWptr = 0u;
	gDbyZapTrustBefore = 0u;
	gDbyZapProg = 0u;
	gDbyZapMetaPhys = 0u;

	/* Read-only TrustZone queries first: they need no rail and touch no GPU register. */
	{
		DbyScmRes q;

		/* avail= is ambiguous when the queried function id carries the SMC64 bit (0x42000201);
		 * the vendor probes with the unencoded 0x02000201 (qcom_scm-64.c:214-215), so a zero
		 * a1 there would not mean the call is missing.  sup= is advisory only: kgsl never calls
		 * qcom_scm_pas_supported (qcom_wcnss.c:480 is the only caller in the vendor tree). */
		q = dbyScmCall(0x42000601ull, 1u, DBY_SCM_PIL(DBY_SCM_INIT_IMAGE), 0u, 0u);
		gDbyTzAvail0 = (uint32_t)q.a0;
		gDbyTzAvail1 = (uint32_t)q.a1;
		q = dbyScmCall(DBY_SCM_PIL(0x07u), 1u, DBY_ZAP_PAS_ID, 0u, 0u);
		gDbyTzSup0 = (uint32_t)q.a0;
		gDbyTzSup1 = (uint32_t)q.a1;
	}

	/* Only after mode 14 proved the CP executes: the secure-mode exit is a packet through that ring. */
	if (gDbyGpuCpRc != 0u || gDbyGpuCpMarker != DBY_CPB_MARKER) {
		gDbyZapRc = 0xFFFFFF20u;
		return;
	}
	/* The trust register as found, for the record: it is readable (evidence s76) and writing it stalls
	 * this side (evidence s77), so it is only read. */
	dbyZapMark(0u);
	(void)dbyGpuCpRead(0xF400u, &gDbyZapTrustBefore);

	/* The stock path: this device's own a650_zap through PAS 13, then CP_SET_SECURE_MODE(0). */
	gDbyZapRc = dbyZapLoad();
	if (gDbyZapRc != 0u) {
		return;
	}
	gDbyZapRc = dbyZapLeaveSecure();
}

static void
renderZap(void)
{
	char *row = native_gpupower_rows[2];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU ZAP st=");
	at = putHex8(at, end, gDbyZapRc);
	at = putStr(at, end, " step=");
	at = putDec(at, end, gDbyZapStep);
	at = putStr(at, end, " init=");
	at = putHex8(at, end, (uint32_t)gDbyZapInit0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, (uint32_t)gDbyZapInit1);
	at = putStr(at, end, " mem=");
	at = putHex8(at, end, (uint32_t)gDbyZapMem0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, (uint32_t)gDbyZapMem1);
	at = putStr(at, end, " auth=");
	at = putHex8(at, end, (uint32_t)gDbyZapAuth0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, (uint32_t)gDbyZapAuth1);
	at = putStr(at, end, " sec=");
	at = putDec(at, end, gDbyZapSecRc);
	at = putStr(at, end, " tr=");
	at = putHex8(at, end, gDbyZapTrustBefore);
	*at = 0;

	row = native_gpupower_rows[3];
	end = row + 119;
	at = putStr(row, end, "GPU CP st=");
	at = putHex8(at, end, gDbyGpuCpRc);
	at = putStr(at, end, " rptr=");
	at = putHex8(at, end, gDbyGpuCpRptr);
	at = putStr(at, end, " wptr=");
	at = putHex8(at, end, gDbyGpuCpWptr);
	/* G-1 first: the three counters after it are unbounded decimals, and losing the low
	 * digits of id2 to truncation would publish a wrong input_bits (review must-fix 3). */
	if (gDbyGpuCtxIdsOk != 0u) {
		at = putStr(at, end, " input_bits=");
		at = putDec(at, end, dbyGpuInputBits(gDbyGpuCtxContext.id2));
		at = putStr(at, end, " id2=");
		at = putHex8(at, end, gDbyGpuCtxContext.id2);
	} else {
		at = putStr(at, end, " input_bits=--");
	}
	at = putStr(at, end, " me=");
	at = putDec(at, end, gDbyGpuCpMeInitFence);
	at = putStr(at, end, " mw=");
	at = putDec(at, end, gDbyGpuCpMemWriteFence);
	at = putStr(at, end, " sf=");
	at = putDec(at, end, gDbyZapSecFence);
	at = putStr(at, end, " hf=");
	at = putHex8(at, end, gDbyGpuCpHwFault);
	at = putStr(at, end, " mark=");
	at = putHex8(at, end, gDbyGpuCpMarker);
	*at = 0;
}

/* --- mode 16: TrustZone's own view of the GPU's PAS, plus one init_image below 4 GiB ---------------
 *
 * Mode 15's init_image returned 0xFFCFFFBB.  Two candidates: the embedded a650_zap.elf is not this
 * device's image (it comes from the alioth tree - a Xiaomi-signed zap on a Huawei device), or the
 * metadata buffer lay above 4 GiB (XNU's RAM here is 2..6 GiB, so a 0xFFFFFFF000-masked allocation
 * usually does).  This mode asks TrustZone both questions and touches neither the GPU nor the SMMU:
 *   qcom_scm_is_call_available(PAS init_image) and qcom_scm_pas_supported(PAS 13), exactly as the
 *   tb128fu port queries them (work/tracks/gpu-peer-progress/gh/tb128fu-GpuProbe.cpp:404-416),
 * then one init_image whose metadata buffer is forced below 4 GiB.  A supported PAS with a
 * successful init_image means the image is acceptable and the earlier failure was the address;
 * supported + refused means the image is not this device's.
 */

static uint32_t gDbyTzInitRc;
static uint32_t gDbyTzInit0, gDbyTzInit1;
static uint32_t gDbyTzMetaHi, gDbyTzMetaLo;
static uint32_t gDbyTzMaskBits;
static uint32_t gDbyTzEhdr, gDbyTzHash;

/* The metadata only: the ELF header plus the hash segment, as init_image wants them.  Returns 0 on
 * success and stores the buffer, its physical address and the mask that produced it. */
static uint32_t
dbyTzMeta(uint64_t mask, IOBufferMemoryDescriptor **out, uint64_t *phys, uint32_t *bits)
{
	const uint8_t *fw = native_gpu_zap_firmware;
	uint64_t size = native_gpu_zap_firmware_size;
	uint32_t phoff, phent, phnum, hash = 0u, ehdr, hashBytes, hashOff, i;
	size_t metaBytes;
	IOBufferMemoryDescriptor *meta;
	uint8_t *m;

	if (fw == nullptr || size < 52u || fw[0] != 0x7Fu || fw[4] != 1u) {
		return 1u;
	}
	phoff = dbyLe32(fw + 28);
	phent = dbyLe16(fw + 42);
	phnum = dbyLe16(fw + 44);
	if (phent != 32u || phnum < 2u || (uint64_t)phoff + (uint64_t)phnum * 32u > size) {
		return 1u;
	}
	for (i = 1u; i < phnum && hash == 0u; i++) {
		if (((dbyLe32(fw + phoff + i * 32u + 24u) >> 24) & 7u) == 2u) {
			hash = i;
		}
	}
	if (hash == 0u) {
		return 1u;
	}
	ehdr = dbyLe32(fw + phoff + 16u);
	hashOff = dbyLe32(fw + phoff + hash * 32u + 4u);
	hashBytes = dbyLe32(fw + phoff + hash * 32u + 16u);
	if (ehdr > size || hashOff + hashBytes > size) {
		return 1u;
	}
	gDbyTzEhdr = ehdr;
	gDbyTzHash = hashBytes;
	metaBytes = (size_t)((ehdr + hashBytes + 0xFFFu) & ~0xFFFull);
	meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
	    metaBytes, mask);
	if (meta == nullptr || meta->prepare() != kIOReturnSuccess) {
		return 2u;
	}
	m = static_cast<uint8_t *>(meta->getBytesNoCopy());
	*phys = (uint64_t)meta->getPhysicalSegment(0, nullptr);
	if (m == nullptr || *phys == 0u) {
		return 3u;
	}
	for (i = 0u; i < ehdr; i++) {
		m[i] = fw[i];
	}
	for (i = 0u; i < hashBytes; i++) {
		m[ehdr + i] = fw[hashOff + i];
	}
	dbyCleanToPoc(m, metaBytes);
	*out = meta;
	*bits = mask == 0xFFFFF000ull ? 32u : 40u;
	return 0u;
}

static void
dbyTzQueries(void)
{
	IOBufferMemoryDescriptor *meta = nullptr;
	uint64_t phys = 0u;
	uint32_t bits = 0u;
	uint32_t rc;
	DbyScmRes r;

	gDbyTzAvail0 = gDbyTzAvail1 = 0u;
	gDbyTzSup0 = gDbyTzSup1 = 0u;
	gDbyTzInitRc = 0xFFFFFF40u;
	gDbyTzInit0 = gDbyTzInit1 = 0u;
	gDbyTzMetaHi = gDbyTzMetaLo = 0u;
	gDbyTzMaskBits = 0u;
	gDbyTzEhdr = gDbyTzHash = 0u;

	/* qcom_scm_is_call_available: ARGS(1), the function id of PAS init_image. */
	r = dbyScmCall(0x42000601ull, 1u, DBY_SCM_PIL(DBY_SCM_INIT_IMAGE), 0u, 0u);
	gDbyTzAvail0 = (uint32_t)r.a0;
	gDbyTzAvail1 = (uint32_t)r.a1;
	/* qcom_scm_pas_supported: PAS_IS_SUPPORTED (cmd 7), ARGS(1), the PAS id. */
	r = dbyScmCall(DBY_SCM_PIL(0x07u), 1u, DBY_ZAP_PAS_ID, 0u, 0u);
	gDbyTzSup0 = (uint32_t)r.a0;
	gDbyTzSup1 = (uint32_t)r.a1;

	/* Below 4 GiB first: that is the difference from mode 15's 40-bit buffer. */
	rc = dbyTzMeta(0xFFFFF000ull, &meta, &phys, &bits);
	if (rc == 2u) {
		rc = dbyTzMeta(0xFFFFFFF000ull, &meta, &phys, &bits);   /* no <4 GiB run available */
	}
	if (rc != 0u) {
		gDbyTzInitRc = 0xFFFFFF41u + rc;
		return;
	}
	gDbyTzMetaHi = (uint32_t)(phys >> 32);
	gDbyTzMetaLo = (uint32_t)(phys & 0xFFFFFFFFu);
	gDbyTzMaskBits = bits;
	r = dbyScmCall(DBY_SCM_PIL(DBY_SCM_INIT_IMAGE), 2u | (2u << 6), DBY_ZAP_PAS_ID, phys, 0u);
	gDbyTzInit0 = (uint32_t)r.a0;
	gDbyTzInit1 = (uint32_t)r.a1;
	gDbyTzInitRc = (r.a0 == 0 && r.a1 == 0u) ? 0u : 0xFFFFFF50u;
}

static void
renderTz(void)
{
	char *row = native_gpupower_rows[0];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU TZ avail=");
	at = putHex8(at, end, gDbyTzAvail0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, gDbyTzAvail1);
	at = putStr(at, end, " sup=");
	at = putHex8(at, end, gDbyTzSup0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, gDbyTzSup1);
	*at = 0;

	row = native_gpupower_rows[1];
	end = row + 119;
	at = row;
	at = putStr(at, end, "GPU TZ init=");
	at = putHex8(at, end, gDbyTzInitRc);
	at = putStr(at, end, " ret=");
	at = putHex8(at, end, gDbyTzInit0);
	at = putStr(at, end, "/");
	at = putHex8(at, end, gDbyTzInit1);
	at = putStr(at, end, " meta=");
	at = putHex8(at, end, gDbyTzMetaHi);
	at = putHex8(at, end, gDbyTzMetaLo);
	at = putStr(at, end, " bits=");
	at = putDec(at, end, gDbyTzMaskBits);
	at = putStr(at, end, " ehdr=");
	at = putDec(at, end, gDbyTzEhdr);
	at = putStr(at, end, " hash=");
	at = putDec(at, end, gDbyTzHash);
	*at = 0;

	row = native_gpupower_rows[2];
	end = row + 119;
	at = putStr(row, end, "GPU TZ note: avail=is_call_available(PAS init), sup=pas_supported(13)");
	*at = 0;
}

/* Called from the boot-progress monitor thread. One shot, and only when asked for. */
// mode 17: the A650 archive owns both address spaces.
//
// Why this is a separate path and not an extension of mode 9: the archive takes the slot set as
// a parameter and this board's measured layout is GMU bank 1 / asid 2 on SMR slots {4,5} with
// sids {4,5}, GPU bank 0 / asid 1 on slots {0..3} with sids {0,1,0x400,0x401}. The probe's own
// ladder programs those same slots itself, with its own tables and its own preflight, so running
// the archive on top of it would hand it slots that are already attached to a live context.
// Mode 17 therefore skips the probe's own GMU/GPU context work entirely and lets the archive
// program both, after normalising the slots here.
//
// This file holds the archive-side half; the dispatcher calls it from the mode >= 9 gate and
// from the mode >= 10 cold-start block, and skips the probe's own CP and ZAP paths for 17.
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoSubmit.h"

#define DBY_ARC_GMU_BANK 1u
#define DBY_ARC_GMU_ASID 2u
#define DBY_ARC_GPU_BANK 0u
#define DBY_ARC_GPU_ASID 1u

/* This tablet's own layout, not the reference board's (review 5bc9517e must-fix 4): kona's
 * qcom,no-dynamic-asid pairs SID and ASID, the GMU owns SMR slots 4 and 5 with sids 4 and 5,
 * and the GPU owns slots 0..3 with sids 0, 1, 0x400, 0x401. Both were measured here — see
 * DBY_GMU_SLOT_FIRST/LAST below and the slot normalisation in dbyGpuCpStart. */
static const unsigned kDbyArcGmuSlots[2] = {4u, 5u};
static const unsigned kDbyArcGmuSids[2]  = {4u, 5u};
static const unsigned kDbyArcGpuSlots[4] = {0u, 1u, 2u, 3u};
static const unsigned kDbyArcGpuSids[4]  = {0u, 1u, 0x400u, 0x401u};

static uint32_t gDbyArcStep, gDbyArcGmuRc, gDbyArcPayRc, gDbyArcCpuRc;
static uint32_t gDbyArcGpuRc, gDbyArcBufRc, gDbyArcBits;
static NativeAdrenoGmuContextReport gDbyArcReport;
static NativeAdrenoGmuPayload gDbyArcPayload[5];
static NativeAdrenoGpuBuffer gDbyArcBuffer[5];
static OSSharedPtr<IOMemoryMap> gDbyArcKeep[5];
static volatile uint32_t *gDbyArcCpu[5];

/* The archive's attach refuses a target slot whose S2CR is not the canonical FAULT word
 * (ArmSmmu4KStreams.c:43-44) and the first un-normalised attempt on this tablet returned
 * pre=10 = ArmSmmu4KBusy (evidence/edl-log-s69). So every slot this mode hands over is
 * normalised here first, in the same order dbyGpuCpStart uses (:2650-2676): S2CR to FAULT,
 * barrier, read back, then SMR left invalid so the attach can claim the SID. */
static uint32_t gDbyArcNorm;

static int
dbyArcSmmuWrite(uint32_t offset, uint32_t value)
{
	if (dbyGpuWrite32(&gDbyGpu, A650GpuWinSmmu, offset, value) != 0) {
		return 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 1;
}

static int
dbyArcSmmuSlotFault(unsigned slot)
{
	uint32_t s2cr = 0;

	if (dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, 0xC00u + 4u * slot, &s2cr) != 0) {
		return 0;
	}
	if ((s2cr & 0xFFFFu) == 0u && ((s2cr >> 16) & 0x3u) == 2u) {
		return 1;                       /* already the canonical FAULT word */
	}
	if (!dbyArcSmmuWrite(0xC00u + 4u * slot, DBY_GMU_SLOT_FAULT)) {
		return 0;
	}
	s2cr = 0;
	if (dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, 0xC00u + 4u * slot, &s2cr) != 0) {
		return 0;
	}
	if ((s2cr & 0xFFFFu) != 0u || ((s2cr >> 16) & 0x3u) != 2u) {
		return 0;
	}
	gDbyArcNorm++;
	return 1;
}

static int
dbyArcSmmuPreflight(const unsigned *slots, unsigned count)
{
	unsigned i;

	for (i = 0u; i < count; ++i) {
		/* S2CR first, SMR second: the order ArmSmmu4KStreams.c:129-139 uses and the one the
		 * probe's own path takes (:2647-2648). The SMR is then cleared so the archive's attach
		 * is the one that claims the SID (valid_smr() looks at bit 31 only). */
		if (!dbyArcSmmuSlotFault(slots[i])) {
			return 0;
		}
		if (!dbyArcSmmuWrite(0x800u + 4u * slots[i], 0u)) {
			return 0;
		}
	}
	return 1;
}

/* The archive allocated these with its own descriptors; the CPU only needs an uncached view of
 * each, so map (never allocate) exactly the physical range it reports. The map is kept for the
 * life of the machine: the GMU walks that memory after this function returns. */
static int
dbyArcMapPayload(unsigned index)
{
	IODeviceMemory *memory;
	uint64_t physical = gDbyArcPayload[index].physical;
	size_t bytes = (size_t)gDbyArcPayload[index].bytes;

	gDbyArcCpu[index] = nullptr;
	if (physical == 0u || bytes == 0u) {
		return 0;
	}
	memory = IODeviceMemory::withRange((IOPhysicalAddress)physical, bytes);
	if (memory == nullptr) {
		return 0;
	}
	gDbyArcKeep[index] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	if (gDbyArcKeep[index] == nullptr) {
		return 0;
	}
	gDbyArcCpu[index] = reinterpret_cast<volatile uint32_t *>(gDbyArcKeep[index]->getVirtualAddress());
	return gDbyArcCpu[index] != nullptr;
}

/* Steps 1-2: the GMU's address space through the archive, then its five payloads. */
static void
dbyArcGmuContext(void)
{
	volatile uint8_t *smmu = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinSmmu]);
	size_t bytes = (size_t)kDbyGpuWindows[A650GpuWinSmmu].size;
	unsigned i;

	gDbyArcStep = 1u;
	gDbyArcGmuRc = 0xFFFFFFFFu;
	gDbyArcPayRc = 0xFFFFFFFFu;
	gDbyArcCpuRc = 0xFFFFFFFFu;
	if (smmu == nullptr || bytes == 0u) {
		return;
	}
	memset(&gDbyArcReport, 0, sizeof gDbyArcReport);
	memset(gDbyArcPayload, 0, sizeof gDbyArcPayload);

	if (!dbyArcSmmuPreflight(kDbyArcGmuSlots, 2u)) {
		gDbyArcGmuRc = 0xFFFFFFF9u;      /* the target slots could not be normalised */
		return;
	}
	gDbyArcGmuRc = (uint32_t)NativeAdrenoGmuContextBringupRoutes(smmu, bytes,
		DBY_ARC_GMU_BANK, DBY_ARC_GMU_ASID, kDbyArcGmuSlots, kDbyArcGmuSids, 2u, nullptr, &gDbyArcReport);
	if (gDbyArcGmuRc != 0u) {
		return;
	}
	/* The archive attaches the exact SIDs 4 and 5 and cannot carry the device tree's mask
	 * (valid_smr() is V only, ArmSmmu4KStreams.c:9,136 - the driver has no mask path). The DT
	 * encodes args[1]=0x400 into ARM_SMMU_SMR_MASK because bit 10 of the SID says which GPU TBU
	 * the transaction came from (kona-gpu.dtsi:340,346), so an exact-SID route misses every
	 * gfx_1_tbu transfer of that client - the probe's own GMU path ORs the same mask at
	 * :1610-1612. Without it the GMU's first icache fetch can arrive unmatched: a global fault
	 * and a cold-start timeout. */
	{
		unsigned k;

		for (k = 0u; k < 2u; ++k) {
			const uint32_t offset = 0x800u + 4u * kDbyArcGmuSlots[k];
			uint32_t smr = 0;
			uint32_t want;

			if (dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, offset, &smr) != 0) {
				gDbyArcGmuRc = 0xFFFFFFF8u;
				return;
			}
			want = smr | (DBY_GMU_CTX_SMR_MASK << 16);
			if (want != smr) {
				if (!dbyArcSmmuWrite(offset, want)) {
					gDbyArcGmuRc = 0xFFFFFFF8u;
					return;
				}
				smr = 0;
				if (dbyGpuRead32(&gDbyGpu, A650GpuWinSmmu, offset, &smr) != 0 || smr != want) {
					gDbyArcGmuRc = 0xFFFFFFF8u;
					return;
				}
				gDbyArcNorm++;
			}
		}
	}

	gDbyArcStep = 2u;
	gDbyArcPayRc = (uint32_t)NativeAdrenoGmuContextPayloads(gDbyArcPayload);
	if (gDbyArcPayRc != 0u) {
		return;
	}
	/* Only the four the cold start consumes: icache, dummy, log, hfi (payload index 2 is the
	 * debug region the kit does not map). */
	gDbyArcCpuRc = 0u;
	for (i = 0u; i < 5u; ++i) {
		if (i == 2u) {
			continue;
		}
		if (!dbyArcMapPayload(i)) {
			gDbyArcCpuRc = 0xFFFFFFF0u | i;
			return;
		}
	}
}

/* Step 3: the cold start, with the archive's payloads instead of the probe's own regions.
 * gDbyColdDevice and the service/limit tables come from the ladder, so only the four memory
 * descriptors differ; everything the kit does with them is untouched. */
static void
dbyArcColdStart(void)
{
	A650GmuStartupLimits limits = {1000u, 100u, 50000u, 2000000u};

	gDbyArcStep = 3u;
	gDbyColdRan = 1;
	gDbyColdStatus = 0xFFFFFFFFu;
	memset(&gDbyColdDevice, 0, sizeof gDbyColdDevice);
	memset(&gDbyColdCfg, 0, sizeof gDbyColdCfg);
	memset(&gDbyColdResult, 0, sizeof gDbyColdResult);
	memset(&gDbyColdIo, 0, sizeof gDbyColdIo);

	/* This path has its own status range (0xFFFFFE0x) so a refusal here can never be read as
	 * one of the mode-10 codes (:1904-1941) that mean something else. */
	if (gDbyArcCpuRc != 0u || gDbyArcPayload[0].physical == 0u) {
		gDbyColdStatus = 0xFFFFFE01u;
		return;
	}
	if (gDbyStartupRan == 0 || gDbyStartupStatus != 0u) {
		gDbyColdStatus = 0xFFFFFE02u;
		return;
	}
	/* The same guard mode 10 keeps: a regenerated firmware header must not silently change
	 * the image the GMU boots. */
	if (native_gpu_gmu_firmware_size != DBY_GPU_GMU_FIRMWARE_BYTES) {
		gDbyColdStatus = 0xFFFFFE03u;
		return;
	}
	if (!dbyColdMapPdc()) {
		gDbyColdStatus = 0xFFFFFE04u;
		return;
	}

	gDbyColdCfg.chip_id = DBY_GMU_CHIP_ID;
	gDbyColdCfg.firmware = native_gpu_gmu_firmware;
	gDbyColdCfg.firmware_bytes = native_gpu_gmu_firmware_size;
	gDbyColdCfg.icache.cpu = gDbyArcCpu[0];
	gDbyColdCfg.icache.bytes = (size_t)gDbyArcPayload[0].bytes;
	gDbyColdCfg.icache.iova = gDbyArcPayload[0].iova;
	gDbyColdCfg.dummy.cpu = gDbyArcCpu[1];
	gDbyColdCfg.dummy.bytes = (size_t)gDbyArcPayload[1].bytes;
	gDbyColdCfg.dummy.iova = gDbyArcPayload[1].iova;
	gDbyColdCfg.log.cpu = gDbyArcCpu[3];
	gDbyColdCfg.log.bytes = (size_t)gDbyArcPayload[3].bytes;
	gDbyColdCfg.log.iova = gDbyArcPayload[3].iova;
	gDbyColdCfg.hfi.cpu = gDbyArcCpu[4];
	gDbyColdCfg.hfi.bytes = (size_t)gDbyArcPayload[4].bytes;
	gDbyColdCfg.hfi.iova = gDbyArcPayload[4].iova;
	gDbyColdCfg.poll_budget = 1000u;

	gDbyColdIo.base[0] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinGmu]);
	gDbyColdIo.base[1] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinRscc]);
	gDbyColdIo.base[2] = reinterpret_cast<volatile uint8_t *>(gDbyPdcMap->getVirtualAddress());
	gDbyColdIo.base[3] = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinGpu]);
	gDbyColdIo.bytes[0] = 0x30000u;
	gDbyColdIo.bytes[1] = 0x10000u;
	gDbyColdIo.bytes[2] = 0x10000u;
	gDbyColdIo.bytes[3] = A650_GPU_BYTES;
	if (gDbyColdIo.base[0] == nullptr || gDbyColdIo.base[1] == nullptr ||
	    gDbyColdIo.base[2] == nullptr || gDbyColdIo.base[3] == nullptr) {
		gDbyColdStatus = 0xFFFFFE05u;
		return;
	}
	gDbyColdDevice.io.cookie = &gDbyColdIo;
	gDbyColdDevice.io.bytes[0] = gDbyColdIo.bytes[0];
	gDbyColdDevice.io.bytes[1] = gDbyColdIo.bytes[1];
	gDbyColdDevice.io.bytes[2] = gDbyColdIo.bytes[2];
	gDbyColdDevice.io.bytes[3] = gDbyColdIo.bytes[3];
	gDbyColdDevice.io.read32 = dbyColdRead32;
	gDbyColdDevice.io.write32 = dbyColdWrite32;
	gDbyColdDevice.io.barrier = dbyColdBarrier;
	gDbyColdDevice.services.cookie = nullptr;
	gDbyColdDevice.services.now_us = dbyColdNow;
	gDbyColdDevice.services.wait_us = dbyColdWait;

	gDbyColdStatus = (uint32_t)A650GmuColdStart(&gDbyColdDevice, &gDbyColdCfg, &gDbyStartupCfg,
		&limits, &gDbyColdResult);
}

/* Steps 4-5: the GPU's own address space through the archive, then its five kernel buffers. */
static void
dbyArcGpuContext(void)
{
	volatile uint8_t *smmu = reinterpret_cast<volatile uint8_t *>(gDbyGpu.base[A650GpuWinSmmu]);
	size_t bytes = (size_t)kDbyGpuWindows[A650GpuWinSmmu].size;

	gDbyArcStep = 4u;
	gDbyArcGpuRc = 0xFFFFFFFFu;
	gDbyArcBufRc = 0xFFFFFFFFu;
	gDbyArcBits = 0u;
	if (smmu == nullptr || bytes == 0u) {
		return;
	}
	if (!dbyArcSmmuPreflight(kDbyArcGpuSlots, 4u)) {
		gDbyArcGpuRc = 0xFFFFFFF9u;
		return;
	}
	gDbyArcGpuRc = (uint32_t)NativeAdrenoGpuContextBringupRoutes(smmu, bytes,
		DBY_ARC_GPU_BANK, DBY_ARC_GPU_ASID, kDbyArcGpuSlots, kDbyArcGpuSids, 4u, nullptr, &gDbyArcReport);
	if (gDbyArcGpuRc != 0u) {
		return;
	}
	gDbyArcStep = 5u;
	memset(gDbyArcBuffer, 0, sizeof gDbyArcBuffer);
	gDbyArcBufRc = (uint32_t)NativeAdrenoGpuContextBuffers(gDbyArcBuffer);
	gDbyArcBits = NativeAdrenoGpuContextInputBits();
}

static void
renderArchive(void)
{
	char *row = native_gpupower_rows[2];
	const char *end = row + 119;
	char *at = row;
	char *row3 = native_gpupower_rows[3];
	const char *end3 = row3 + 119;
	char *at3 = row3;

	/* The mode-3 gate refused before the archive was entered: leave the rows it wrote alone
	 * instead of replacing its reason with an empty ARC row. */
	if (gDbyArcStep == 0u && gDbyArcGmuRc == 0u && gDbyArcCpuRc == 0u) {
		return;
	}
	at = putStr(at, end, "GPU ARC step=");
	at = putDec(at, end, gDbyArcStep);
	at = putStr(at, end, " gmu=");
	at = putHex8(at, end, gDbyArcGmuRc);
	at = putStr(at, end, " pay=");
	at = putHex8(at, end, gDbyArcPayRc);
	at = putStr(at, end, " cpu=");
	at = putHex8(at, end, gDbyArcCpuRc);
	at = putStr(at, end, " cold=");
	at = putHex8(at, end, gDbyColdStatus);
	*at = 0;

	at3 = putStr(at3, end3, "GPU ARC gx=");
	at3 = putHex8(at3, end3, gDbyGxStatus);
	at3 = putStr(at3, end3, " gpu=");
	at3 = putHex8(at3, end3, gDbyArcGpuRc);
	at3 = putStr(at3, end3, " buf=");
	at3 = putHex8(at3, end3, gDbyArcBufRc);
	at3 = putStr(at3, end3, " bits=");
	at3 = putDec(at3, end3, gDbyArcBits);
	at3 = putStr(at3, end3, " nrm=");
	at3 = putDec(at3, end3, gDbyArcNorm);
	*at3 = 0;
}

// mode 18: the ring and the command processor, over the archive's own buffers.
//
// Mode 17 proved the archive owns both address spaces and hands back five kernel buffers
// (sqe, ring, memptrs, pwrup, scratch). This mode does the next thing a real client needs:
// a NativeAdrenoRing on generation 6 (A6xx), attached to the ring and memptrs buffers, and
// one NativeAdrenoCpInit run on it. Publishing needs its own round because it also wants the
// interrupt, the timer and the ABI device info.
//
// The adapters are deliberately thin: the ring's register window is the same GPU block the
// probe already maps, and both ring buffers are mapped Normal non-cacheable, so the cache
// hooks only need a dsb (there is no cacheable alias to clean).
/* The ring hands out DWORD register indices (NativeAdrenoRing.h:9-10); dbyGpuWrite32/Read32
 * take BYTE offsets (they add the offset to the window base without scaling). Passing the
 * index straight through would put CP_RB_BASE at byte 0x800 - dword 0x200 - and make
 * CP_RB_CNTL/RPTR/WPTR unaligned stores into the RBBM block while GX is powered. */
static int
dbyRingWrite32(void *cookie, uint32_t reg, uint32_t value)
{
	return dbyGpuWrite32(reinterpret_cast<DbyGpuState *>(cookie), A650GpuWinGpu,
	                      (size_t)reg * 4u, value) == 0 ? 0 : -1;
}

static int
dbyRingRead32(void *cookie, uint32_t reg, uint32_t *value)
{
	return dbyGpuRead32(reinterpret_cast<DbyGpuState *>(cookie), A650GpuWinGpu,
	                     (size_t)reg * 4u, value) == 0 ? 0 : -1;
}

/* GMU AHB fence status, dword 0x9313: bit 0 says a write to a fenced GPU register was dropped
 * while the GPU domain was not ready. Without it the ring cannot tell a dropped WPTR from a
 * slow CP, so a hung CP would read as success (review must-fix 4). */
static int
dbyRingFenceStatus(void *cookie, int clear, uint32_t *status)
{
	uint32_t value = 0;
	(void)cookie;

	if (dbyGpuRead32(&gDbyGpu, A650GpuWinGmu, (size_t)0x9313u * 4u, &value) != 0) {
		return -1;
	}
	if (status != nullptr) {
		*status = value;
	}
	/* The clear is a separate register: GMU_AHB_FENCE_STATUS_CLR at dword 0x9314, written with
	 * 0x7 (NativeAdrenoService.cpp:166-173). Writing bit0 back into the status register never
	 * clears it. */
	if (clear != 0 && (value & 1u) != 0u) {
		(void)dbyGpuWrite32(&gDbyGpu, A650GpuWinGmu, (size_t)0x9314u * 4u, 0x7u);
	}
	return 0;
}

static uint64_t
dbyRingNow(void *cookie)
{
	uint64_t now = 0;
	(void)cookie;
	if (dbyColdNow(nullptr, &now) != 0) {
		return 0u;
	}
	return now;
}

static int
dbyRingBarrier(void *)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static int
dbyRingPublish(void *, const volatile uint32_t *, size_t)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static int
dbyRingAcquire(void *, volatile uint32_t *, size_t)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

static uint32_t gDbyRingRc, gDbyRingCpRc, gDbyRingFence, gDbyRingGen, gDbyRingStep;
static uint32_t gDbyRingWait, gDbyRingObserved, gDbyRingSqeBytes, gDbyRingVerdict;
static uint64_t gDbyRingIova, gDbyRingFenceIova;
/* Three maps: ring, fence page and the SQE buffer the mode-18 path fills. */
static OSSharedPtr<IOMemoryMap> gDbyRingKeep[3];
static void *gDbyRingCpu[3];
static NativeAdrenoRing gDbyRingObject;
static NativeAdrenoCpInit gDbyRingCp;
/* The adapters are shared with the publish step, so they live here, not in the callee. */
static NativeAdrenoRingIo gDbyRingIo;
static NativeAdrenoRingCache gDbyRingCache;

/* Map one of the archive's buffers into an uncached CPU view, the same way the GMU payloads
 * are mapped: the archive owns the memory, we only look at it. */
static int
dbyRingMap(unsigned index, const NativeAdrenoGpuBuffer *buffer, void **out)
{
	IODeviceMemory *memory;
	IOMemoryMap *map;

	*out = nullptr;
	if (buffer->physical == 0u || buffer->bytes == 0u) {
		return 0;
	}
	memory = IODeviceMemory::withRange((IOPhysicalAddress)buffer->physical, (IOByteCount)buffer->bytes);
	if (memory == nullptr) {
		return 0;
	}
	gDbyRingKeep[index] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	if (gDbyRingKeep[index] == nullptr) {
		return 0;
	}
	map = gDbyRingKeep[index].get();
	*out = reinterpret_cast<void *>(map->getVirtualAddress());
	return *out != nullptr;
}

static void
dbyRingBringup(void)
{
	NativeAdrenoRingIo &io = gDbyRingIo;
	NativeAdrenoRingCache &cache = gDbyRingCache;
	NativeAdrenoCpInitConfig config;
	void *ring_cpu = nullptr;
	void *fence_cpu = nullptr;

	gDbyRingStep = 1u;
	gDbyRingRc = 0xFFFFFFFFu;
	gDbyRingCpRc = 0xFFFFFFFFu;
	gDbyRingFence = 0u;
	gDbyRingGen = 6u;
	if (gDbyArcBufRc != 0u) {
		return;                       /* mode 17 did not produce the buffers */
	}
	/* The device-proven preamble every earlier CP mode ran before touching the ring: CP_PROTECT,
	 * CP_MEMPOOL, the ROQ/IB thresholds, CP_APRIV_CNTL and RBBM_INT_0_MASK. Mode 18 skipped it and
	 * the CP accepted the WPTR but never fetched (evidence/edl-log-s81: w=e00002d6 o=0). Its own
	 * prerequisite is GX on, which the caller gates, so it costs nothing when the buffers are
	 * absent and cannot render a BSS zero as a CP result. */
	if (dbyGpuHwInit() != 0) {
		gDbyRingRc = 0xFFFFFFE6u;
		return;
	}

	/* out[1] is the ring, out[2] the memptrs/fence page, out[0] the SQE, out[3] the power-up
	 * register list, out[4] the scratch page (NativeAdrenoGpuContext.h: GpuBufferCount). */
	gDbyRingIova = gDbyArcBuffer[1].iova;
	gDbyRingFenceIova = gDbyArcBuffer[2].iova;
	gDbyRingStep = 2u;
	if (!dbyRingMap(0u, &gDbyArcBuffer[1], &ring_cpu) ||
	    !dbyRingMap(1u, &gDbyArcBuffer[2], &fence_cpu)) {
		gDbyRingRc = 0xFFFFFFE1u;     /* a buffer did not map */
		return;
	}

	io.cookie = &gDbyGpu;
	io.write32 = dbyRingWrite32;
	io.read32 = dbyRingRead32;
	io.barrier = dbyRingBarrier;
	io.fence_cookie = nullptr;
	io.fence_status = dbyRingFenceStatus;
	cache.cookie = nullptr;
	cache.publish = dbyRingPublish;
	cache.acquire = dbyRingAcquire;

	gDbyRingStep = 3u;
	gDbyRingObject.setGeneration(6u);
	gDbyRingRc = (uint32_t)gDbyRingObject.attach(&io, &cache,
		ring_cpu, (size_t)gDbyArcBuffer[1].bytes, gDbyArcBuffer[1].iova,
		fence_cpu, (size_t)gDbyArcBuffer[2].bytes, gDbyArcBuffer[2].iova);
	if (gDbyRingRc != 0u) {
		return;
	}

	/* CP_ME_INIT for an A6xx core: no power-up register list, no CP_THREAD_CONTROL, and the
	 * SQE base was already programmed by the context bring-up, so the kit compares instead of
	 * rewriting it after the SQE started (NativeAdrenoCpInit.h:8-19). */
	memset(&config, 0, sizeof config);
	config.sqe_iova = gDbyArcBuffer[0].iova;
	config.pwrup_reglist_iova = 0u;
	config.sqe_base_programmed = 1u;
	config.generation = 6u;

	/* The SQE buffer the archive allocates is empty and its CP registers are untouched:
	 * NativeAdrenoGpuContextBringupRoutes takes only the SMMU pointer, and the probe's own CP
	 * setup lives in dbyGpuCpStart, which mode 18 does not run. So this loads the microcode and
	 * programs the base here, the same work mode 14 does at :2616-2633, :2826 and :2853. */
	gDbyRingStep = 4u;
	{
		void *sqe_cpu = nullptr;
		const uint8_t *image = native_gpu_sqe_firmware;
		uint32_t bytes = (uint32_t)native_gpu_sqe_firmware_size;
		uint32_t i;

		if (image == nullptr || bytes < 8u || (uint64_t)(bytes - 4u) > gDbyArcBuffer[0].bytes) {
			gDbyRingCpRc = 0xFFFFFFE2u;
			return;
		}
		if (!dbyRingMap(2u, &gDbyArcBuffer[0], &sqe_cpu)) {
			gDbyRingCpRc = 0xFFFFFFE3u;
			return;
		}
		for (i = 0u; i < bytes - 4u; ++i) {
			((volatile uint8_t *)sqe_cpu)[i] = image[4u + i];
		}
		__asm__ volatile("dsb sy" ::: "memory");
		for (i = 0u; i < bytes - 4u; ++i) {
			if (((volatile uint8_t *)sqe_cpu)[i] != image[4u + i]) {
				gDbyRingCpRc = 0xFFFFFFE4u;
				return;
			}
		}
		gDbyRingSqeBytes = bytes - 4u;
		if (dbyGpuCpW64(DBY_CP_SQE_INSTR_BASE, gDbyArcBuffer[0].iova) != 0 ||
		    dbyGpuCpW(DBY_CP_SQE_CNTL, 1u) != 0) {
			gDbyRingCpRc = 0xFFFFFFE5u;
			return;
		}
	}

	gDbyRingStep = 5u;
	gDbyRingCpRc = (uint32_t)gDbyRingCp.run(&io, &gDbyRingObject, &config, &gDbyRingFence, true);
	gDbyRingStep = 6u;
	/* A started CP is not ready until its init fence is observed (NativeAdrenoGpu.cpp:36-40). */
	{
		NativeAdrenoRingClock clock;
		uint32_t observed = 0u;

		clock.cookie = nullptr;
		clock.now_us = dbyRingNow;
		clock.idle = nullptr;
		gDbyRingWait = (uint32_t)gDbyRingObject.idle(&clock, 100000u);
		if (gDbyRingObject.fenceValue(&observed) == 0) {
			gDbyRingObserved = observed;
		}
		gDbyRingVerdict = (gDbyRingCpRc == 0u && gDbyRingWait == 0u &&
		                  gDbyRingObserved >= gDbyRingFence) ? 1u : 0u;
	}
	gDbyRingStep = 7u;
}

/* Appended to the ARC row rather than rendered over it: mode 18 runs the whole mode-17 path,
 * and losing gmu=/pay=/cpu=/cold= would hide where a refused ring bring-up stopped (review
 * must-fix 5). Row 3 has ~60 characters left after the ARC fields. */
static void
renderRing(void)
{
	char *row3 = native_gpupower_rows[3];
	const char *end3 = row3 + 119;
	char *at3 = row3;
	const char *scan = row3;

	while (*scan != 0 && scan < end3) {
		scan++;
	}
	at3 = const_cast<char *>(scan);
	/* Compact: the ARC fields already use ~59 of 119 bytes. `st=` is the verdict - 1 only when
	 * CP init returned success, the wait returned success and the observed fence reached the one
	 * the ring handed out, so `cp=0 wait=0 obs=0` can no longer read as success. */
	at3 = putStr(at3, end3, " | RING st=");
	at3 = putDec(at3, end3, gDbyRingVerdict);
	at3 = putStr(at3, end3, " at=");
	at3 = putHex8(at3, end3, gDbyRingRc);
	at3 = putStr(at3, end3, " cp=");
	at3 = putHex8(at3, end3, gDbyRingCpRc);
	at3 = putStr(at3, end3, " w=");
	at3 = putHex8(at3, end3, gDbyRingWait);
	at3 = putStr(at3, end3, " o=");
	at3 = putDec(at3, end3, gDbyRingObserved);
	*at3 = 0;
}

// mode 19: the GPU interrupt and the published device.
//
// Mode 18 proved the CP executes from the archive's ring. What a user-space client needs next is
// the service: the GPU SPI attached to XNU, the A6xx interrupt set programmed, the submit and
// timer objects on the ring, and native_adreno_publish with the measured address sizes.
//
// The device info is copied from the reference board (same SoC, same A650) instead of being
// derived here: work/tracks/cpu-official-route/scratch/alioth/package/GpuProbe.cpp:1282-1291.
#include "NativeAdrenoABI.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoTimer.h"

extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *),
                                      void *context, uint32_t *readback, uint64_t *router);
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *vm, NativeAdrenoSubmit *submit,
                                          NativeAdrenoRing *ring, const NativeAdrenoRingClock *clock,
                                          const NativeAdrenoTimer *timer,
                                          const NativeAdrenoAbiDeviceInfo *info,
                                          uint32_t input_bits, uint32_t output_bits);

static uint32_t gDbyPubStep, gDbyPubSubmit, gDbyPubTimer, gDbyPubIrq, gDbyPubMask;
static uint32_t gDbyPubSpi, gDbyPubSpiReadback, gDbyPubRc;
static uint32_t gDbyPubIrqCount, gDbyPubIrqLast, gDbyPubIrqSame, gDbyPubIrqMasked;
static uint64_t gDbyPubRouter;
/* native_adreno_publish stores this pointer (Publish.cpp:1095) and every user client
 * dereferences it later (UserClient.cpp:444, :452), so it must live for the machine's life.
 * Same silicon, so the reference board's numbers apply (alioth GpuProbe.cpp:1246 and
 * :1282-1291): chip 0x06050002, 1.125 MiB GMEM, UCHE trap base, a 512 MiB..1.5 GiB client
 * window, bank bit 16, UBWC 4. gmem_base is 0, the A650 family's Linux value: this board's own
 * header marks DBY_GPU_GMEM_BASE as TODO_MEASURE and forbids substituting 0x100000
 * (DbyGpuRegs.h:87-99), and 0x1000000 belongs to the gmem16m experiment that reference recorded
 * as failing. */
static NativeAdrenoAbiDeviceInfo gDbyPubInfo = {
	0x06050002u, 0x120000u, 0x0ull, 0x1fffffffff000ull,
	0x20000000ull, 0x60000000ull, 16u, 6u, 1u, 4u,
	NativeAdrenoDeviceClientIova, 0u
};
static NativeAdrenoSubmit gDbySubmit;
static NativeAdrenoTimer gDbyTimer;
static NativeAdrenoIrq gDbyIrq;

/* The CP always-on counter is read through the GPU window at dword 0x0980 (NativeAdrenoTimer.h:
 * CP window addresses are dword register indices). */
static int
dbyPubTimeRead32(void *cookie, uint32_t address, uint32_t *value)
{
	(void)cookie;
	return dbyGpuRead32(&gDbyGpu, A650GpuWinGpu, (size_t)address * 4u, value) == 0 ? 0 : -1;
}

/* The XNU interrupt handler only decodes and acknowledges; the mask is programmed once below and
 * the driver requires the hardware line to be masked at the controller by the caller. */
static void
dbyPubIrqHandler(void *context)
{
	NativeAdrenoIrqResult result;

	(void)context;
	if (gDbyIrq.handle(&result) != 0) {
		/* A decoder that could not clear stays armed; without a mask the level line would spin
		 * forever. Mask every source and stop counting. */
		if (gDbyRingIo.write32 != nullptr && gDbyPubIrqMasked == 0u) {
			(void)gDbyRingIo.write32(gDbyRingIo.cookie, DBY_RBBM_INT_0_MASK, 0u);
			gDbyPubIrqMasked = 1u;
		}
		return;
	}
	gDbyPubIrqCount++;
	if (result.status == gDbyPubIrqLast) {
		gDbyPubIrqSame++;
	} else {
		gDbyPubIrqLast = result.status;
		gDbyPubIrqSame = 1u;
	}
	/* One source that never clears would spin this line forever. The reference masks every
	 * source after 100000 identical statuses; this does the same through the ring's adapter. */
	if (gDbyPubIrqSame > 100000u && gDbyPubIrqMasked == 0u) {
		if (gDbyRingIo.write32 != nullptr) {
			(void)gDbyRingIo.write32(gDbyRingIo.cookie, DBY_RBBM_INT_0_MASK, 0u);
		}
		gDbyPubIrqMasked = 1u;
	}
}

static void
dbyPublishBringup(unsigned upto)
{
	NativeAdrenoTimeIo timeIo;
	NativeAdrenoRingClock clock;
	NativeAdrenoVm *vm = nullptr;

	gDbyPubStep = 1u;
	if (gDbyRingVerdict == 0u) {
		return;                       /* the CP never ran: nothing to publish over */
	}
	vm = NativeAdrenoGpuContextVm();
	if (vm == nullptr) {
		gDbyPubSubmit = 0xFFFFFFD1u;
		return;
	}

	gDbyPubStep = 2u;
	gDbyPubSubmit = (uint32_t)gDbySubmit.attach(vm, &gDbyRingObject);
	if (gDbyPubSubmit != 0u) {
		return;
	}

	gDbyPubStep = 3u;
	timeIo.cookie = nullptr;
	timeIo.read32 = dbyPubTimeRead32;
	gDbyPubTimer = (uint32_t)gDbyTimer.attach(&timeIo, NativeAdrenoTimeCp);
	if (gDbyPubTimer != 0u) {
		return;
	}

	if (upto < 3u) {
		return;                       /* stage 20 stops after submit + timer */
	}
	gDbyPubStep = 4u;
	gDbyPubIrq = (uint32_t)gDbyIrq.attach(&gDbyRingIo);
	if (gDbyPubIrq != 0u) {
		return;
	}
	/* A6xx enable set: programMask()'s default is the A7XX set, which on an A6xx core leaves the
	 * two IB interrupts masked (NativeAdrenoIrq.h:44-58). */
	gDbyPubMask = 0u;
	gDbyPubIrq = (uint32_t)gDbyIrq.programMask(&gDbyPubMask, true);
	if (gDbyPubIrq != 0u) {
		return;
	}

	if (upto < 4u) {
		return;                       /* stage 21 stops after the mask */
	}
	gDbyPubStep = 5u;
	/* SPI 300, level: the GPU's DT interrupt on this SoC, INTID = 32 + 300 (ArmCpu.cpp:784). */
	gDbyPubSpi = (uint32_t)armcpu_spi_attach(32u + DBY_GPU_IRQ_GPU, false, dbyPubIrqHandler, nullptr,
	                                         &gDbyPubSpiReadback, &gDbyPubRouter);
	if (gDbyPubSpi != 0u) {
		return;
	}

	if (upto < 5u) {
		return;                       /* stage 22 stops before publication */
	}
	gDbyPubStep = 6u;
	clock.cookie = nullptr;
	clock.now_us = dbyRingNow;
	clock.idle = nullptr;
	/* The address sizes are measured, not assumed: the input size is the context's own (48 here)
	 * and the output size is ID2[7:4] decoded by the probe's G-1 read (36). */
	gDbyPubRc = (uint32_t)native_adreno_publish(vm, &gDbySubmit, &gDbyRingObject, &clock, &gDbyTimer,
	                                            &gDbyPubInfo, NativeAdrenoGpuContextInputBits(),
	                                            gDbyArcReport.output_bits);
	gDbyPubStep = 7u;
}

/* Its own row: ARC (59) + RING (51) already use 110 of row 3's 119 bytes and the publish row
 * needs 79, so appending it there would hide every value. In mode 19 row 1 holds renderClocks's
 * cfg_ahb/memnoc/dvm/gpll0 verdict, which this overwrites - accepted, because the RAM clocks are
 * not what this mode is testing and the ARC row already proves the SMMU path. */
static void
renderPublish(void)
{
	char *row3 = native_gpupower_rows[1];
	const char *end3 = row3 + 119;
	char *at3 = row3;

	/* A refused cold start or GX says so instead of rendering st=0 with every other field zero,
	 * which would read like a publish that ran and failed at step 0. The test is the gate itself:
	 * a ring that refused after a good cold start is a different failure, and the RING row carries
	 * its st/at/cp/w/o. */
	if (gDbyColdStatus != 0u || gDbyGxStatus != 0u || gDbyGxResult.gx_on == 0u) {
		at3 = putStr(at3, end3, "GPU PUB not run: cold=");
		at3 = putHex8(at3, end3, gDbyColdStatus);
		at3 = putStr(at3, end3, " gx=");
		at3 = putHex8(at3, end3, gDbyGxStatus);
		at3 = putStr(at3, end3, " ring=");
		at3 = putDec(at3, end3, gDbyRingVerdict);
		*at3 = 0;
		return;
	}
	at3 = putStr(at3, end3, "GPU PUB st=");
	at3 = putDec(at3, end3, gDbyPubStep);
	at3 = putStr(at3, end3, " s=");
	at3 = putHex8(at3, end3, gDbyPubSubmit);
	at3 = putStr(at3, end3, " t=");
	at3 = putHex8(at3, end3, gDbyPubTimer);
	at3 = putStr(at3, end3, " i=");
	at3 = putHex8(at3, end3, gDbyPubIrq);
	at3 = putStr(at3, end3, " m=");
	at3 = putHex8(at3, end3, gDbyPubMask);
	at3 = putStr(at3, end3, " spi=");
	at3 = putHex8(at3, end3, gDbyPubSpi);
	at3 = putStr(at3, end3, " p=");
	at3 = putHex8(at3, end3, gDbyPubRc);
	/* The interrupt is proven by the GPIO/SPI side, not by the GPU mask: the readback carries
	 * group/enable/level (ArmCpu.cpp:807-811) and therefore proves the configuration even on a
	 * quiet line; n= is the handler count at the moment this row was rendered, so it is a
	 * snapshot, and n=0 is the expected value in a mode that provokes no GPU interrupt. */
	at3 = putStr(at3, end3, " rb=");
	at3 = putHex8(at3, end3, gDbyPubSpiReadback);
	at3 = putStr(at3, end3, " n=");
	at3 = putDec(at3, end3, gDbyPubIrqCount);
	if (gDbyPubIrqMasked != 0u) {
		at3 = putStr(at3, end3, " masked");
	}
	*at3 = 0;
}

// mode 23: the first client round. Not a user process, but the exact path one takes minus the
// Mach trap (same approach as the reference board's wrapperRound): match the published service,
// open it with the plugin's open type, ask for the ABI and the device info, and confirm a second
// open is refused while one is live. Buffers and submissions are a later mode; this one answers
// only "can anything actually open the device we just published".
#include <IOKit/IOUserClient.h>

static uint32_t gDbyUcStep, gDbyUcOpen, gDbyUcAgain, gDbyUcQuery, gDbyUcInfo;
static uint32_t gDbyUcChip, gDbyUcAbi, gDbyUcInBits, gDbyUcOutBits, gDbyUcChipOk;

/* The plugin's open type is ABI (NativeAdrenoPublish.cpp:50). The device is reached through the
 * driver's own accessor: serviceMatching() matches the C++ CLASS name, not the registry IOName
 * that setName("NativeAdrenoGpu") writes, so matching that string can never succeed. */
#define DBY_UC_OPEN_TYPE 0x4147u
extern "C" IOService *native_adreno_published_service(void);

static IOReturn
dbyUcCall(IOUserClient *uc, uint32_t method, const void *in, void *out, size_t in_bytes,
          size_t out_bytes)
{
	IOExternalMethodArguments args;

	memset(&args, 0, sizeof args);
	args.version = kIOExternalMethodArgumentsCurrentVersion;
	args.selector = method;
	args.structureInput = in_bytes ? in : nullptr;
	args.structureInputSize = (uint32_t)in_bytes;
	args.structureOutput = out_bytes ? out : nullptr;
	args.structureOutputSize = (uint32_t)out_bytes;
	return uc->externalMethod(method, &args, nullptr, nullptr, nullptr);
}

static void
dbyClientBoRound(IOUserClient *uc);

static void
dbyClientBringup(unsigned bo)
{
	IOService *service = nullptr;
	IOUserClient *uc = nullptr;
	IOUserClient *again = nullptr;
	NativeAdrenoAbiParams params;
	NativeAdrenoAbiDeviceInfo info;

	gDbyUcStep = 1u;
	gDbyUcOpen = 0xFFFFFFFFu;
	gDbyUcAgain = 0xFFFFFFFFu;
	gDbyUcQuery = 0xFFFFFFFFu;
	gDbyUcInfo = 0xFFFFFFFFu;
	gDbyUcChip = 0u;
	gDbyUcAbi = 0u;
	gDbyUcInBits = 0u;
	gDbyUcOutBits = 0u;
	gDbyUcChipOk = 0u;
	if (gDbyPubRc != 0u || gDbyPubStep != 7u) {
		return;                       /* nothing was published to open */
	}

	gDbyUcStep = 2u;
	service = native_adreno_published_service();
	if (service == nullptr) {
		gDbyUcOpen = 0xFFFFFFC2u;
		return;
	}

	gDbyUcStep = 3u;
	gDbyUcOpen = (uint32_t)service->newUserClient(kernel_task, nullptr, DBY_UC_OPEN_TYPE,
	                                              nullptr, &uc);
	if (gDbyUcOpen != 0u || uc == nullptr) {
		return;   /* the accessor hands back the driver's own pointer, not a reference we took */
	}

	/* A second open while one is live must be refused with Busy; that is the driver's own
	 * one-client rule, and it is the cheapest proof that the open really reached it. */
	gDbyUcStep = 4u;
	{
		const IOReturn second = service->newUserClient(kernel_task, nullptr, DBY_UC_OPEN_TYPE,
		                                               nullptr, &again);
		gDbyUcAgain = (second == kIOReturnBusy) ? 0u : (uint32_t)second;
		if (again != nullptr) {
			again->release();
		}
	}

	gDbyUcStep = 5u;
	memset(&params, 0, sizeof params);
	gDbyUcQuery = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodQuery, nullptr, &params, 0u,
	                                  sizeof params);
	gDbyUcAbi = params.abi_version;
	gDbyUcInBits = params.input_bits;
	gDbyUcOutBits = params.output_bits;

	gDbyUcStep = 6u;
	memset(&info, 0, sizeof info);
	gDbyUcInfo = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info, 0u,
	                                 sizeof info);
	gDbyUcChip = info.chip_id;
	/* The reference compares the reply against what it published (GpuProbe.cpp:1331);
	 * chip_id alone is the field that proves the reply came from our device. */
	gDbyUcChipOk = (info.chip_id == gDbyPubInfo.chip_id) ? 1u : 0u;

	/* Mode 24 continues the round with buffers, an IB and a submission before closing. */
	if (bo != 0u) {
		dbyClientBoRound(uc);
	}

	gDbyUcStep = 7u;
	/* clientClose() is where the driver drops kernel_client, frees the row and terminates the
	 * core (NativeAdrenoPublish.cpp:430-470); release() alone leaks all three. */
	(void)uc->clientClose();
	uc->release();
}

static void
renderClient(void)
{
	char *row = native_gpupower_rows[0];
	const char *end = row + 119;
	char *at = row;

	/* Row 0 holds the power/clock row in lower modes; in mode 23 the client round is the point. */
	at = putStr(at, end, "GPU UC st=");
	at = putDec(at, end, gDbyUcStep);
	at = putStr(at, end, " open=");
	at = putHex8(at, end, gDbyUcOpen);
	at = putStr(at, end, " again=");
	at = putHex8(at, end, gDbyUcAgain);
	at = putStr(at, end, " q=");
	at = putHex8(at, end, gDbyUcQuery);
	at = putStr(at, end, " info=");
	at = putHex8(at, end, gDbyUcInfo);
	at = putStr(at, end, " chip=");
	at = putHex8(at, end, gDbyUcChip);
	at = putStr(at, end, " ok=");
	at = putDec(at, end, gDbyUcChipOk);
	at = putStr(at, end, " abi=");
	at = putDec(at, end, gDbyUcAbi);
	at = putStr(at, end, " in=");
	at = putDec(at, end, gDbyUcInBits);
	at = putStr(at, end, " out=");
	at = putDec(at, end, gDbyUcOutBits);
	*at = 0;
}

// mode 24: the client round's second half - buffers, an IB and a submission.
//
// Mode 23 proved a client can open the device and read its ABI. This mode does what a real client
// does with that ABI (the reference's ucRound, GpuProbe.cpp:1312-1460): allocate two buffers, map
// them for the GPU, map them for the CPU, write an IB of three CP_MEM_WRITEs that targets the
// second buffer, submit it, wait for the fence, and read the target back. Every step renders, so a
// refusal is attributable.
static uint32_t gDbyBoStep, gDbyBoAlloc, gDbyBoMap, gDbyBoCpu, gDbyBoMem, gDbyBoSub, gDbyBoWait;
static uint32_t gDbyBoRead = 0xFFFFFFFFu, gDbyBoFence;   /* sentinel from the first instruction */
static uint64_t gDbyBoIbIova, gDbyBoTgtIova;

/* The IB: three CP_MEM_WRITE packets (type 7, opcode 0x3d) writing a known value to the target
 * buffer's GPU address. Same shape as the reference's, which is the smallest IB that proves the
 * client's memory is reachable from the CP. */
static int
dbyBoBuildIb(uint32_t *words, size_t capacity, uint64_t target)
{
	static const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
	unsigned i;

	if (capacity < 12u) {
		return 0;
	}
	for (i = 0u; i < 3u; ++i) {
		if (AdrenoPM4Type7(0x3d, 3, &words[i * 4u]) != AdrenoPM4Success) {
			return 0;
		}
		words[i * 4u + 1u] = (uint32_t)(target + i * 4u);
		words[i * 4u + 2u] = (uint32_t)((target + i * 4u) >> 32);
		words[i * 4u + 3u] = values[i];
	}
	return 1;
}

static void
dbyClientBoRound(IOUserClient *uc)
{
	NativeAdrenoAbiBoAlloc alloc;
	NativeAdrenoAbiBoAllocReply ib, tgt;
	NativeAdrenoAbiBoMap mapIb, mapTgt;
	NativeAdrenoAbiBoMapReply mapReply;
	NativeAdrenoAbiBoCpuMap cmIb, cmTgt;
	NativeAdrenoAbiBoCpuMapReply crIb, crTgt;
	IOMemoryDescriptor *descIb = nullptr;
	IOMemoryDescriptor *descTgt = nullptr;
	IOMemoryMap *maps[2] = {nullptr, nullptr};
	IOOptionBits opts[2] = {0, 0};
	NativeAdrenoAbiSubmit submit;
	NativeAdrenoAbiSubmitReply submitReply;
	NativeAdrenoAbiWaitFence wait;
	volatile uint32_t *target = nullptr;
	uint32_t words[12];
	uint32_t token[2];
	unsigned i;
	IOReturn status;

	if (uc == nullptr) {
		return;
	}

	gDbyBoStep = 1u;
	memset(&alloc, 0, sizeof alloc);
	alloc.bytes = 4096u;
	alloc.align_log2 = 12u;
	alloc.contiguous = 0u;
	gDbyBoAlloc = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib, sizeof alloc,
	                                  sizeof ib);
	gDbyBoStep = 2u;
	if (gDbyBoAlloc != 0u) {
		return;
	}
	gDbyBoAlloc = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt, sizeof alloc,
	                                  sizeof tgt);
	gDbyBoStep = 3u;
	if (gDbyBoAlloc != 0u) {
		return;
	}

	/* Inside the window a real client is given: GpuClientVaStart is 0x100000000
	 * (NativeAdrenoGpuContext.h:21) and a client's BoMap below that is refused
	 * (NativeAdrenoUserClient.cpp:316). 0x20004000 would only work for this kernel-owned
	 * connection, whose scope is set to zero, so it would prove the wrong thing. */
	gDbyBoIbIova = 0x100004000ull;
	gDbyBoTgtIova = 0x100005000ull;
	memset(&mapIb, 0, sizeof mapIb);
	mapIb.handle = ib.handle;
	mapIb.iova = gDbyBoIbIova;
	mapIb.permissions = 3u;                /* read + write */
	mapIb.shareability = 2u;
	memset(&mapTgt, 0, sizeof mapTgt);
	mapTgt.handle = tgt.handle;
	mapTgt.iova = gDbyBoTgtIova;
	mapTgt.permissions = 3u;
	mapTgt.shareability = 2u;
	memset(&mapReply, 0, sizeof mapReply);
	gDbyBoMap = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoMap, &mapIb, &mapReply, sizeof mapIb,
	                                sizeof mapReply);
	gDbyBoStep = 4u;
	if (gDbyBoMap != 0u) {
		return;
	}
	gDbyBoMap = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoMap, &mapTgt, &mapReply, sizeof mapTgt,
	                                sizeof mapReply);
	gDbyBoStep = 5u;
	if (gDbyBoMap != 0u) {
		return;
	}

	memset(&cmIb, 0, sizeof cmIb);
	cmIb.handle = ib.handle;
	memset(&cmTgt, 0, sizeof cmTgt);
	cmTgt.handle = tgt.handle;
	memset(&crIb, 0, sizeof crIb);
	memset(&crTgt, 0, sizeof crTgt);
	gDbyBoCpu = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb, sizeof cmIb,
	                                sizeof crIb);
	gDbyBoStep = 6u;
	if (gDbyBoCpu != 0u) {
		return;
	}
	gDbyBoCpu = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt, sizeof cmTgt,
	                                sizeof crTgt);
	gDbyBoStep = 7u;
	if (gDbyBoCpu != 0u) {
		return;
	}

	token[0] = crIb.token;
	token[1] = crTgt.token;
	gDbyBoMem = 0u;
	for (i = 0u; i < 2u; ++i) {
		IOMemoryDescriptor *descriptor = nullptr;
		IOReturn got = uc->clientMemoryForType(token[i], &opts[i], &descriptor);
		if (got != 0 || descriptor == nullptr) {
			gDbyBoMem = (uint32_t)got;
			if (gDbyBoMem == 0u) {
				gDbyBoMem = 0xFFFFFFB1u;   /* null descriptor */
			}
			return;
		}
		if (i == 0u) {
			descIb = descriptor;
		} else {
			descTgt = descriptor;
		}
		maps[i] = descriptor->createMappingInTask(kernel_task, 0, opts[i]);
		if (maps[i] == nullptr) {
			gDbyBoMem = 0xFFFFFFB2u;
			return;
		}
	}

	gDbyBoStep = 8u;
	target = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
	if (target == nullptr) {
		gDbyBoMem = 0xFFFFFFB3u;
		return;
	}
	for (i = 0u; i < 1024u; ++i) {
		target[i] = 0xdeadbeefu;
	}
	if (!dbyBoBuildIb((uint32_t *)maps[0]->getVirtualAddress(), 1024u, gDbyBoTgtIova)) {
		gDbyBoMem = 0xFFFFFFB4u;
		return;
	}

	memset(&submit, 0, sizeof submit);
	submit.buffer_count = 1u;
	submit.buffers[0].handle = ib.handle;
	submit.buffers[0].iova = gDbyBoIbIova;
	submit.buffers[0].bytes = 12u * sizeof(uint32_t);
	memset(&submitReply, 0, sizeof submitReply);
	gDbyBoStep = 9u;
	gDbyBoSub = (uint32_t)dbyUcCall(uc, NativeAdrenoMethodSubmit, &submit, &submitReply,
	                                sizeof submit, sizeof submitReply);
	if (gDbyBoSub != 0u) {
		return;
	}
	gDbyBoFence = (uint32_t)submitReply.fence;

	memset(&wait, 0, sizeof wait);
	wait.fence = submitReply.fence;
	wait.timeout_ns = 5000000000ull;
	gDbyBoStep = 10u;
	/* WaitFence's output record is zero bytes in both the dispatch table and the size check
	 * (NativeAdrenoPublish.cpp:813, NativeAdrenoUserClient.cpp:503), so passing one fails with
	 * kIOReturnBadArgument before the method runs. */
	status = dbyUcCall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr, sizeof wait, 0u);
	gDbyBoWait = (uint32_t)status;
	if (gDbyBoWait != 0u) {
		return;
	}

	gDbyBoStep = 11u;
	/* The three writes must be visible in the target buffer. The view is uncached and the CP
	 * writes non-cacheably, so a barrier is enough; no invalidate is needed (the driver issues
	 * its own dsb before submitting, NativeAdrenoSubmit.cpp:266). */
	__asm__ volatile("dsb sy" ::: "memory");
	gDbyBoRead = (target[0] == 0x0badc0deu && target[1] == 0xfeed0001u &&
	              target[2] == 0x600d0003u) ? 0u : 1u;

	/* Give the two views back before the client is closed: the descriptor is retained for the
	 * caller and the map is the caller's, so nothing else frees them, and a mapping that outlives
	 * its BO is the panic Publish.cpp:260-263 documents. */
	for (i = 0u; i < 2u; ++i) {
		if (maps[i] != nullptr) {
			maps[i]->release();
			maps[i] = nullptr;
		}
	}
	if (descIb != nullptr) {
		descIb->release();
	}
	if (descTgt != nullptr) {
		descTgt->release();
	}
	{
		NativeAdrenoAbiBoFree freeIb, freeTgt;

		memset(&freeIb, 0, sizeof freeIb);
		freeIb.handle = ib.handle;
		memset(&freeTgt, 0, sizeof freeTgt);
		freeTgt.handle = tgt.handle;
		(void)dbyUcCall(uc, NativeAdrenoMethodBoFree, &freeIb, nullptr, sizeof freeIb, 0u);
		(void)dbyUcCall(uc, NativeAdrenoMethodBoFree, &freeTgt, nullptr, sizeof freeTgt, 0u);
	}
}

static void
renderClientBo(void)
{
	char *row = native_gpupower_rows[4];
	const char *end = row + 119;
	char *at = row;

	at = putStr(at, end, "GPU BO st=");
	at = putDec(at, end, gDbyBoStep);
	at = putStr(at, end, " a=");
	at = putHex8(at, end, gDbyBoAlloc);
	at = putStr(at, end, " m=");
	at = putHex8(at, end, gDbyBoMap);
	at = putStr(at, end, " c=");
	at = putHex8(at, end, gDbyBoCpu);
	at = putStr(at, end, " mem=");
	at = putHex8(at, end, gDbyBoMem);
	at = putStr(at, end, " sub=");
	at = putHex8(at, end, gDbyBoSub);
	at = putStr(at, end, " w=");
	at = putHex8(at, end, gDbyBoWait);
	at = putStr(at, end, " rd=");
	at = putDec(at, end, gDbyBoRead);
	*at = 0;
}

void
sheng_gpu_power_probe(void)
{
	unsigned mode = 0;
	A650GpuResult result;
	A650GpuIo io;

	/* ONE CALL SITE: BootProgress.c on the late gate (now>=20 with a successful log round). It used to
	 * be called earlier as well - package/LegacyDisplayProbe.cpp ran it from
	 * native_legacy_display_late_probe(), which bsd_init.c:1314-1315 runs inside bsd_autoconf(), before
	 * the archive writer had stored a single slot - and that call was removed rather than merely gated
	 * (review 2026-11-03 15:30). The gate below stays anyway, because a probe that can freeze the SoC
	 * must not depend on its caller being careful:
	 *   - boot_log_slots_ok counts rounds where the slot, the message buffer, the header AND the
	 *     sync all succeeded (BootLog.c, end of the writer round). boot_log_rounds is NOT usable for
	 *     this: it is bumped before the header write, so a reader can observe it mid-round. Review
	 *     2026-11-03 15:30 made that distinction a must-fix.
	 *   - symbol absent / zero -> nothing archived yet: return, and stage stays 0 so a later call
	 *     retries. */
	if (!&boot_log_slots_ok || boot_log_slots_ok == 0u) {
		return;
	}

	if (native_gpupower_stage != 0u) {
		return;
	}
	native_gpupower_stage = 1u;

	if (!PE_parse_boot_argn("dby-gpu-power", &mode, sizeof mode) || mode < 1u || mode > 24u) {
		char *row = native_gpupower_rows[0];
		const char *end = row + 119;
		char *at = putStr(row, end, "GPU PWR skipped: pass dby-gpu-power=1..24");

		*at = 0;
		native_gpupower_stage = 3u;
		return;
	}

	if (mode == 16u) {
		/* TrustZone's own view of the GPU's PAS, before any window is mapped or any rail is powered. */
		dbyTzQueries();
		renderTz();
		native_gpupower_stage = 2u;
		return;
	}

	for (unsigned window = 0; window < A650GpuWindows; ++window) {
		if (dbyGpuMapWindow(window)) {
			gDbyGpu.mapped |= 1u << window;
		}
	}
	if ((gDbyGpu.mapped & (1u << A650GpuWinGccGpu)) == 0u ||
	    (gDbyGpu.mapped & (1u << A650GpuWinGpucc)) == 0u) {
		char *row = native_gpupower_rows[0];
		const char *end = row + 119;
		char *at = putStr(row, end, "GPU PWR nomap: GCC_GPU and GPUCC are required; mapped=");

		at = putDec(at, end, gDbyGpu.mapped);
		*at = 0;
		native_gpupower_stage = 4u;
		return;
	}

	io.cookie = &gDbyGpu;
	io.read32 = dbyGpuRead32;
	io.write32 = dbyGpuWrite32;
	io.barrier = dbyGpuBarrier;
	io.delay_us = dbyGpuDelay;

	/* Which windows the kit may touch is decided HERE, not only by the kit's prerequisite gates.
	 * A650GmuPower.c gates the GPUCC reads on cfg_ahb being enabled and the SMMU/GPU reads on CX being
	 * PWR_ON with the hlos1 vote clock running (:179-190, :258-296) - conditions this probe cannot see
	 * before it runs. If the bootloader left those clocks or the CX rail on, mode 1 would therefore read
	 * the SMMU too, which is exactly the block that froze the device on 2026-11-03. The kit already
	 * skips any window whose bit is missing from have_windows, so the mode decides:
	 *   1 -> GCC only (no GPUCC, no SMMU, no GPU reads)
	 *   2 -> GCC + GPUCC (the cfg_ahb write and its readback)
	 *   3 -> everything, i.e. CX/vote clocks and then the gated SMMU identity read
	 * Mapping a window touches no hardware, so all seven stay mapped. */
	unsigned windows = gDbyGpu.mapped;
	unsigned kitMode = mode;

	/* Mode 4 is "3a" (review 2026-11-03 19:54): run the kit's mode 3 - CX GDSC and the GPU clocks -
	 * with the SMMU window masked out.  The probe renders rows only after A650GpuBringup returns, so a
	 * freeze inside the power writes would leave no row at all; 3a proves those writes separately, and
	 * the full mode 3 then becomes a pure SMMU read whose failure is attributable. */
	/* Mode 5 is "read the SMMU's configuration": the same power-up as mode 3 and then register reads only.
	 * It exists because the identity read of 2026-11-03 (IAS=36, OAS=36, UBS=5) settled the address width
	 * but not the question that decides whether the GPU/GMU can reach memory: what the bootloader left in
	 * sACR (may the non-secure side program the SMMU at all?) and in S2CR[0]/S2CR[5] (BYPASS, FAULT or
	 * TRANSLATE for the GPU and GMU streams).  Read-only: no SMMU write of any kind. */
	if (mode >= 9u) {
		/* Rounds 2c..2f: the same mode-3 power-up as mode 7, then the context.  The SMMU window stays
		 * mapped: this is the mode that programs it, and the GPU window stays masked like every other
		 * mode.  Mode 10 adds the cold start after the context (it must run in this same boot: the
		 * regions' physical addresses are this boot's). */
		kitMode = 3u;
		windows &= ~(1u << A650GpuWinGpu);
	} else if (mode == 8u) {
		/* Round 2b: mode 3 power, then the four GMU regions and the HFI startup table.  No cold start
		 * yet: its regions are unreachable until the GMU SMMU context is encoded. */
		kitMode = 3u;
		windows &= ~(1u << A650GpuWinGpu);
	} else if (mode == 7u) {
		/* Round 2a: the same power-up as mode 6 (mode 3), then the GMU SMMU context prerequisites
		 * (slot normalisation + the physical table pool).  This mode does NOT run the GMU clock
		 * sequence, and it keeps the SMMU window mapped because that is what it programs. */
		kitMode = 3u;
		windows &= ~(1u << A650GpuWinGpu);
	} else if (mode == 6u) {
		/* Mode 6: the GMU clocks a6xx_gmu_resume enables, at the GMU OPP 200 MHz, plus the AOP command
		 * database that supplies the real OPP levels.  Same power-up as mode 3; the SMMU and GPU windows
		 * are masked because this stage measures clocks only (review 2026-11-04 03:47, blocking 5). */
		kitMode = 3u;
		windows &= ~((1u << A650GpuWinSmmu) | (1u << A650GpuWinGpu));
	} else if (mode == 5u) {
		kitMode = 3u;
		windows &= ~(1u << A650GpuWinGpu);
	} else if (mode == 4u) {
		/* SMMU *and* GPU.  Masking only the SMMU was wrong (review 20:52, must-fix 1): this branch is
		 * taken before the mode<3 mask below, and with the GPU window present the kit's RBBM read
		 * (GPU+0x840) runs whenever the pre-power gxgdscr read shows PWR_ON - an unclocked GX access. */
		kitMode = 3u;
		windows &= ~((1u << A650GpuWinSmmu) | (1u << A650GpuWinGpu));
	} else if (mode < 2u) {
		windows &= (1u << A650GpuWinGccGpu) | (1u << A650GpuWinGccVote);
	} else if (mode < 3u) {
		windows &= ~((1u << A650GpuWinSmmu) | (1u << A650GpuWinGpu));
	} else {
		/* Mode 3 keeps the SMMU window (that is the point) but still drops the GPU window: the kit's GPU
		 * stage would otherwise read RBBM (GPU+0x840) whenever the pre-power gxgdscr read shows PWR_ON,
		 * and gxgdscr is never rendered, so a hang there would be misattributed to the SMMU read (review
		 * 2026-11-03 21:45, blocking 2).  The identity read does not need the GPU window: expect gpu=4. */
		windows &= ~(1u << A650GpuWinGpu);
	}

	/* Pre-flight before any power write (review 20:52, must-fix 2).  A650GpuRead zeroes its destination
	 * and only sets io_error (A650GmuPower.c:127-137), while the kit still marks the GPUCC stage Done
	 * (:190) and then writes `cx_gdscr & ~SW_COLLAPSE` (:203).  A failed 0x106c read therefore becomes a
	 * write of 0, which clears the GDSC's timing fields (EN_REST/EN_FEW/CLK_DIS waits) - a corrupted
	 * power-up.  Refuse to hand that to the kit: require cfg_ahb enabled and a plausible cx_gdscr. */
	if (kitMode >= 3u && (windows & (1u << A650GpuWinGpucc)) != 0u) {
		uint32_t cfgAhb = 0;
		uint32_t cxGdscr = 0;

		if (dbyGpuRead32(&gDbyGpu, A650GpuWinGccGpu, A650_GCC_GPU_CFG_AHB, &cfgAhb) != 0 ||
		    (cfgAhb & A650_CBCR_ENABLE) == 0u) {
			char *row = native_gpupower_rows[0];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU PWR abort: gcc_gpu_cfg_ahb not enabled (");

			at = putHex8(at, end, cfgAhb);
			at = putStr(at, end, "); GPUCC is unreadable without it");
			*at = 0;
			native_gpupower_stage = 4u;
			return;
		}
		if (dbyGpuRead32(&gDbyGpu, A650GpuWinGpucc, A650_CC_CX_GDSCR, &cxGdscr) != 0 ||
		    cxGdscr == 0u || cxGdscr == 0xFFFFFFFFu) {
			char *row = native_gpupower_rows[0];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU PWR abort: GPUCC cx_gdscr did not answer (");

			at = putHex8(at, end, cxGdscr);
			at = putStr(at, end, "); a write would clear its timing fields");
			*at = 0;
			native_gpupower_stage = 4u;
			return;
		}
	}

	memset(&result, 0, sizeof result);
	A650GpuBringup(&io, kitMode, windows, &result);

	renderState(mode, &result);
	renderClocks(&result);
	if (mode >= 9u) {
		/* The mode-3 outcome is the licence to program the SMMU at all: the same gate mode 7 uses
		 * (smmu==Done plus a plausible CX GDSC and hlos1 vote CBCR).  An unclocked SMMU *write* is worse
		 * than the read that froze this board on 2026-11-03.  Modes 7 and 8 run first because their two
		 * readbacks ARE mode 9's pre-check - mode 7's prep does write the two S2CR slots to their canonical
		 * FAULT word, exactly as mode 7 does on its own, and mode 8 allocates the four regions.  Everything
		 * the gate protects (sCR0, the tables, the bank, the routes) is refused when one of those two
		 * readbacks is incomplete; see dbyGpuSmmuContextEncode.
		 * Mode 10 runs the whole chain in this boot and only then releases the GMU: the context is a
		 * precondition of A650GmuColdStart (A650Gmu.h:9-10), not a result it can reuse from a past boot. */
		if (result.smmu == A650GpuDone && A650GpuGdscIsOn(result.cx_gdscr) &&
		    A650GpuClockIsOn(result.cc_smmu) && result.cc_smmu != 0u && result.cc_smmu != 0xFFFFFFFFu &&
		    (result.cc_smmu & A650_CBCR_ENABLE) != 0u && result.cx_gdscr != 0u && result.cx_gdscr != 0xFFFFFFFFu) {
			if (mode >= 17u) {
				/* The archive owns both address spaces: SMR slots {4,5} for the GMU and
				 * {0..3} for the GPU - this board's measured layout - so none of the probe's
				 * own context work may run in this mode. */
				dbyArcGmuContext();
				if (gDbyArcCpuRc == 0u) {
					if (!dbyGmuClocksForCold(&result, &io)) {
						gDbyColdRan = 1;
						gDbyColdStatus = 0xFFFFFFF6u;
					} else {
						dbyGmuStartupBuild();
						dbyArcColdStart();
					}
				} else {
					/* The archive path never reached the cold start: say so, or the row and
					 * GPU COLD render the BSS zero as a success. */
					gDbyColdRan = 1;
					gDbyColdStatus = 0xFFFFFE06u;
				}
			} else {
			dbyGpuGmuContextPrep();
			dbyGmuRegionsBuild();
			dbyGpuSmmuContextEncode(result.id0, result.id1, result.id2);
			if (mode >= 10u) {
				/* The context and the clocks are both preconditions of A650GmuColdStart
				 * (A650Gmu.h:9-10); mode 3 gives neither the GMU clocks nor the regions, so
				 * both are produced here, in this boot, and the clock verdict is checked
				 * before the one attempt the kit allows. */
				if (!dbyCtxReady()) {
					gDbyColdRan = 1;
					gDbyColdStatus = 0xFFFFFFF7u;   /* the SMMU context is not ready */
				} else if (!dbyGmuClocksForCold(&result, &io)) {
					gDbyColdRan = 1;
					gDbyColdStatus = 0xFFFFFFF6u;   /* the GMU clocks refused */
				} else {
					dbyGmuStartupBuild();
					dbyGpuColdStart();
				}
			}
			}
		} else {
			char *row = native_gpupower_rows[2];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU SMMUCTX not run: smmu=");

			at = putDec(at, end, result.smmu);
			at = putStr(at, end, " cx=");
			at = putHex8(at, end, result.cx_gdscr);
			at = putStr(at, end, " vote=");
			at = putHex8(at, end, result.cc_smmu);
			*at = 0;
			native_gpupower_rows[3][0] = 0;
			if (mode >= 10u) {
				gDbyColdRan = 1;
				gDbyColdStatus = 0xFFFFFFF7u;       /* the mode-3 gate refused the SMMU */
			}
		}
		/* Rendered on both paths, so a refused context still says why the GMU was not released. */
		if (mode >= 10u) {
			renderCold();
		}
		/* Only with the GMU clocks proven on: reading the GMU aperture unclocked is the recipe
		 * that froze this board on 2026-11-03.  A gate refusal (F6/F7) leaves them off. */
		if (mode == 11u && gDbyColdClocksRc == 0 && gDbyColdClocks.failed == 0u && gDbyColdClocks.step == 5u) {
			renderColdDbg();
		}
		if (mode == 12u && gDbyColdClocksRc == 0 && gDbyColdClocks.failed == 0u && gDbyColdClocks.step == 5u) {
			renderFaultDbg(5u);
			renderLogRow();
		}
		if (mode >= 13u) {
			/* GX only after a complete cold start: the kit refuses otherwise (startup.completed). */
			if (gDbyColdStatus == 0u && gDbyColdClocksRc == 0 && gDbyColdClocks.failed == 0u &&
			    gDbyColdClocks.step == 5u) {
				memset(&gDbyGxResult, 0, sizeof gDbyGxResult);
				gDbyGxStatus = (uint32_t)A650GmuPowerGx(&gDbyColdDevice, 1u, &gDbyGxResult);
			} else {
				gDbyGxStatus = 0xFFFFFFF0u;     /* the cold start did not complete */
			}
			/* Same gate as mode 12: reading the GMU/SMMU apertures with the GMU clocks never
			 * started is the recipe that froze this board on 2026-11-03.  renderGx only prints
			 * static values, so it stays unconditional. */
			if (gDbyColdClocksRc == 0 && gDbyColdClocks.failed == 0u && gDbyColdClocks.step == 5u) {
				renderFaultDbg(3u);
			}
			renderGx();
		}
		if (mode >= 14u && mode < 17u) {
			/* Mode 14: the GPU's own address space, then the first command.  dbyGpuCpStart
			 * gates itself on the same cold start + GX verdict this branch just made, so a
			 * refused GX cannot reach a GPU register. */
			if (gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyGpuCpStart();
			} else {
				gDbyGpuCpRc = 0xFFFFFF01u;
				gDbyGpuCpStep = 1u;
				/* No stage ran, so ff (never entered) is the truthful code, not the BSS zero. */
				memset(gDbyGpuCtxStage, 0xFF, sizeof gDbyGpuCtxStage);
			}
			renderCp();
			if (mode >= 15u) {
				dbyGpuZapStart();
				if (gDbyGpuCpRc == 0u) {
					renderZap();
				}
			}
		}
		if (mode >= 24u) {
			/* Mode 24 is mode 23 plus the buffer, IB and submission half of the round. */
			if (gDbyColdStatus == 0u && gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyArcGpuContext();
				dbyRingBringup();
				dbyPublishBringup(5u);
				dbyClientBringup(1u);
			} else {
				gDbyArcGpuRc = 0xFFFFFF02u;
			}
			renderArchive();
			renderRing();
			renderPublish();
			renderClient();
			renderClientBo();
		} else if (mode >= 23u) {
			/* Mode 23 is mode 19 plus the first client round: open the device we published. */
			if (gDbyColdStatus == 0u && gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyArcGpuContext();
				dbyRingBringup();
				dbyPublishBringup(5u);
				dbyClientBringup(0u);
			} else {
				gDbyArcGpuRc = 0xFFFFFF02u;
			}
			renderArchive();
			renderRing();
			renderPublish();
			renderClient();
		} else if (mode >= 19u) {
			/* Mode 19 is mode 18 plus the interrupt and the published service. */
			if (gDbyColdStatus == 0u && gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyArcGpuContext();
				dbyRingBringup();
				/* Mode 19 hung on hardware (evidence/edl-log-s83: one slot, no GPU row), so the new
				 * calls are bisected along the order they already have: 20 stops after submit+timer,
				 * 21 adds the interrupt attach and mask, 22 adds the SPI, 19 adds publish. A staged
				 * mode renders zeros for the steps it never reached: read them as "not attempted",
				 * not as a result. */
				dbyPublishBringup(mode == 20u ? 2u : (mode == 21u ? 3u : (mode == 22u ? 4u : 5u)));
			} else {
				gDbyArcGpuRc = 0xFFFFFF02u;
			}
			renderArchive();
			renderRing();
			renderPublish();
		} else if (mode >= 18u) {
			/* Mode 18 is mode 17 plus the ring and CP init: same archive-owned contexts, then the
			 * five buffers become a NativeAdrenoRing on generation 6 and one CP_ME_INIT. */
			if (gDbyColdStatus == 0u && gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyArcGpuContext();
				dbyRingBringup();
			} else {
				gDbyArcGpuRc = 0xFFFFFF02u;
			}
			renderArchive();
			renderRing();
		} else if (mode >= 17u) {
			/* The GPU's address space, taken over by the archive once the GMU and GX are up.
			 * No CP command and no ZAP in this mode: those belong to the probe's own context.
			 * Gated on the same verdict dbyGpuCpStart uses (:2574-2576): taking the GPU's SMMU
			 * over after a failed cold start or a refused GX would program a bank no powered
			 * master backs. */
			if (gDbyColdStatus == 0u && gDbyGxStatus == 0u && gDbyGxResult.gx_on != 0u) {
				dbyArcGpuContext();
			} else {
				/* No GPU context after a refused cold start or GX. gDbyArcStep is left where the
				 * archive path stopped, so the row says how far it actually got. */
				gDbyArcGpuRc = 0xFFFFFF02u;
			}
			renderArchive();
		}
	} else if (mode == 8u) {
		if (result.cc == A650GpuDone && A650GpuGdscIsOn(result.cx_gdscr) &&
		    A650GpuClockIsOn(result.cc_ahb) && result.powered_cx != 0u && result.powered_clocks != 0u && result.cx_gdscr != 0xFFFFFFFFu && result.cc_ahb != 0u &&
		    result.cc_ahb != 0xFFFFFFFFu && (result.cc_ahb & A650_CBCR_ENABLE) != 0u) {
			dbyGmuRegionsBuild();
			dbyGmuStartupBuild();
			renderGmuRegions();
		} else {
			char *row = native_gpupower_rows[2];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU GMUREG not run: cc=");

			at = putDec(at, end, result.cc);
			at = putStr(at, end, " ahb=");
			at = putHex8(at, end, result.cc_ahb);
			*at = 0;
			native_gpupower_rows[3][0] = 0;
		}
	} else if (mode == 7u) {
		/* Nothing here may touch the SMMU unless the mode-3 block really powered CX and left the
		 * hlos1 vote clock running: an unclocked SMMU WRITE is worse than the read that froze this
		 * board on 2026-11-03 (review 2026-11-04 07:5x, blocking 2). */
		if (result.smmu == A650GpuDone && A650GpuGdscIsOn(result.cx_gdscr) &&
		    A650GpuClockIsOn(result.cc_smmu) && result.cc_smmu != 0u && result.cc_smmu != 0xFFFFFFFFu && (result.cc_smmu & A650_CBCR_ENABLE) != 0u && result.cx_gdscr != 0xFFFFFFFFu) {
			dbyGpuGmuContextPrep();
			renderGmuContextPrep();
		} else {
			char *row = native_gpupower_rows[2];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU GMUCTX not run: smmu=");

			at = putDec(at, end, result.smmu);
			at = putStr(at, end, " cx=");
			at = putHex8(at, end, result.cx_gdscr);
			at = putStr(at, end, " vote=");
			at = putHex8(at, end, result.cc_smmu);
			*at = 0;
			native_gpupower_rows[3][0] = 0;
		}
	} else if (mode == 6u) {
		/* The kit own gate is CX powered and the clock CBCR readable (A650GmuPower.c:371-375); it does NOT
		 * look at smmu==Done.  A bringup that failed at the clock step would otherwise let the kit write
		 * GPUCC/GCC on an unpowered rail (review 2026-11-04 03:47, blocking 1).  Gate it here on the mode-3
		 * outcome, and hand the kit a copy whose io_error is cleared: the kit folds local.io_error into its
		 * own failed=2 (A650GmuPower.c:439), so a bringup I/O error would mislabel a good clock sequence
		 * (blocking 2). */
		/* A650GpuClockIsOn(0) is TRUE (it only tests bit 31), and a failed A650GpuRead leaves the
		 * destination zero, so a bare "clock is on" test accepts a register that never answered.  Require
		 * the same plausibility the SMMU verdict requires, plus the two power flags the kit itself sets
		 * (review 2026-11-04 04:00, blocking). */
		{
		const bool powerOk = result.cc == A650GpuDone && result.powered_cx != 0u &&
		    result.powered_clocks != 0u && A650GpuGdscIsOn(result.cx_gdscr) &&
		    result.cx_gdscr != 0u && result.cx_gdscr != 0xFFFFFFFFu &&
		    result.cc_ahb != 0u && result.cc_ahb != 0xFFFFFFFFu &&
		    (result.cc_ahb & A650_CBCR_ENABLE) != 0u && A650GpuClockIsOn(result.cc_ahb);

		if (powerOk) {
			A650GmuClocksResult clk;
			A650GpuResult clean = result;
			int rc;

			clean.io_error = 0;
			memset(&clk, 0, sizeof clk);
			rc = A650GmuClocksEnable(&io, &clean, &clk);
			renderGmuClocks(&clk, rc);
		} else {
			char *row = native_gpupower_rows[2];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU CLK not run: cc=");

			at = putDec(at, end, result.cc);
			at = putStr(at, end, " cx=");
			at = putHex8(at, end, result.cx_gdscr);
			at = putStr(at, end, " ahb=");
			at = putHex8(at, end, result.cc_ahb);
			at = putStr(at, end, " pwr=");
			at = putDec(at, end, result.powered_cx);
			at = putStr(at, end, "/");
			at = putDec(at, end, result.powered_clocks);
			*at = 0;
		}
		}
		renderCmdDb();
	} else if (mode == 5u) {
		/* Only read the SMMU when the kit own gate says the block is powered and clocked
		 * (A650GmuPower.c:259-263).  An unpowered SMMU read is the 2026-11-03 freeze recipe
		 * (review 2026-11-04 01:10, blocking 1). */
		if (result.smmu == A650GpuDone && A650GpuGdscIsOn(result.cx_gdscr) &&
		    A650GpuClockIsOn(result.cc_smmu) && result.cc_smmu != 0u && result.cc_smmu != 0xFFFFFFFFu && (result.cc_smmu & A650_CBCR_ENABLE) != 0u && result.cx_gdscr != 0xFFFFFFFFu) {
			renderSmmuConfig();
		} else {
			char *row = native_gpupower_rows[3];
			const char *end = row + 119;
			char *at = putStr(row, end, "GPU SMMU cfg not read: state=");

			at = putDec(at, end, result.smmu);
			at = putStr(at, end, " cx=");
			at = putHex8(at, end, result.cx_gdscr);
			at = putStr(at, end, " vote=");
			at = putHex8(at, end, result.cc_smmu);
			*at = 0;
		}
	} else {
		renderSmmu(&result);
	}
	renderTail(mode, &result);
	native_gpupower_stage = 2u;
}

}  /* extern "C" */
