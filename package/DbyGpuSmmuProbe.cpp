/* DbyGpuSmmuProbe — the cheapest way to clear the GPU entry gate (G-1).
 *
 * WHY THIS EXISTS
 *   CORRECTED 2026-11-03 12:00: the Adreno 650 driver core DOES build, through scripts/build-adreno650.py
 *   in a scratch tree (/root/a650-u, not in the assembled dby_w09 tree, where the wiring is still
 *   reverted): its 15 driver TUs and the 13 kit TUs archive to 28 objects / 1672520 B, G0 PASS.  Two
 *   real blockers had to be cleared first: the recipe's find_tool() was picking binutils nm (which exits
 *   0 with an empty symbol table on Mach-O) and the tree's ArmSmmu4K kit was two revisions behind the
 *   pinned driver.  G0 is a LINK-ONLY result - nothing of it has run on hardware.
 *   What still does not build is one file, NativeAdrenoService.cpp, where clang-24 dies with signal 6 in
 *   clang::ParseAST at :95 (OSDefineMetaClassAndStructors); the archive recipe does not compile it.
 *   See work/tracks/stage19-verification/REPORT.md §5 and work/verify/ice-service-test.py.
 *
 *   The GPU's FIRST gate does not need that driver anyway. `GpuProbe.cpp` refuses every non-kernel client
 *   when the GPU SMMU reports input_bits <= 32, returning e00002be.  The call chain in the official
 *   source is GpuProbe.cpp:1291 -> NativeAdrenoGpuContextInputBits() -> NativeAdrenoSmmuInputBits,
 *   which reads ID2[11:8] and decodes it through {32,36,40,42,44} with 48 as the fallback.  Three
 *   reads (ID0/ID1/ID2) answer it; ID0/ID1 are not used for the verdict itself but they are what tells
 *   a silent or garbage-answering bus apart from a small SMMU, through the capability gate
 *   dbyGpuSmmuReject() below.
 *
 * HARDWARE FACTS (from THIS board's vendor tree, not copied from another board)
 *   _dby/robyy10__DBY-W09.tar/DBY-W09-main/arch/arm64/boot/dts/vendor/qcom/kona-gpu.dtsi:263-266
 *     kgsl_msm_iommu: qcom,kgsl-iommu@3da0000 { compatible = "qcom,kgsl-smmu-v2";
 *                                               reg = <0x03da0000 0x10000>; }
 *   Register offsets 0x20 (ID0), 0x24 (ID1), 0x28 (ID2) are the SMMU-500 identification registers,
 *   the same ones the official driver reads (NativeAdrenoGmuContext.cpp:34 reads 0x28 into id2).
 *
 * SAFETY
 *   Opt-in: nothing happens unless `dby-gpu-smmu=1` is on the kernel command line.  The GPU SMMU sits
 *   behind GPUCC clocks the boot chain may have left off, and an unanswered device read on this SoC is
 *   an SError, i.e. a panic - so this must not run unattended by default.
 *   It performs NO writes: three 32-bit reads (0x20/0x24/0x28) and nothing else.
 *
 * LINE FORMAT (six unlabelled 120-byte rows, appended to the page after the [clocks] row; the page has
 * no free row of its own, and BootLog.c:651-655 emits them without adding a label)
 *   GPU SMMU id0=xxxxxxxx id1=xxxxxxxx id2=xxxxxxxx input_bits=NN oas=NN gated=Y/N
 *   or, when the block did not identify itself as an SMMU-500, one line and no verdict:
 *   GPU SMMU no verdict: <S1TS|SMS|NUMSMRG|NUMCB|NUMS2CB|width-code|PTFS4K|noanswer> id0=... id1=... id2=...
 */

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/libkern.h>
#include <mach/kern_return.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

/* The include set above mirrors package/DbyTouch/DbyTouch.cpp:45-67 - the only place in this board
 * repository that already maps MMIO and parses boot arguments from a prelinked driver. */

/* The archive is force-loaded, so every external here must be a strong definition or a weak import. */
extern "C" {

/* Row storage the logger reads.  Six rows of 120 bytes, NUL-terminated, same shape as
 * native_cpufreq_census so BootLog.c can walk it with the same loop. */
char native_gpusmmu_census[6][120];

/* 0 = not entered, 1 = entered, 2 = measured and verdict printed, 3 = skipped (no boot argument),
 * 4 = map failed, 5 = the block did not identify itself, so no verdict was printed. */
volatile uint32_t native_gpusmmu_stage;

static int
dbyGpuSmmuMap(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> *keep, uintptr_t *base)
{
	IODeviceMemory *memory;

	memory = IODeviceMemory::withRange(phys, length);
	if (memory == nullptr) {
		return 0;
	}
	*keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache),
	    OSNoRetain);
	memory->release();
	if (!*keep) {
		return 0;
	}
	*base = static_cast<uintptr_t>((*keep)->getVirtualAddress());
	return *base != 0;
}

/* Minimal bounded decimal/hex append; this file does not use snprintf (the board's other drivers
 * record that no prototype is reachable from here - DbyTouch.cpp:521-524). */
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
	int shift;

	for (shift = 28; shift >= 0 && o < end; shift -= 4) {
		*o++ = digits[(v >> shift) & 0xF];
	}
	return o;
}

static char *
putDec(char *o, const char *end, uint32_t v)
{
	char tmp[11];
	unsigned n = 0;

	if (v == 0) {
		if (o < end) {
			*o++ = '0';
		}
		return o;
	}
	while (v != 0 && n < sizeof tmp) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (n != 0 && o < end) {
		*o++ = tmp[--n];
	}
	return o;
}

/* input_bits from ID2[11:8]: {32,36,40,42,44} for codes 0..4, 48 otherwise.
 * Same field and same table as the official helper - its own unit test asserts
 * InputBits(0x0233)==40, 0x1533->48 and 0x0f33->48 (a740/tests/test_vm.cpp:215-216), and the ArmSmmu4K
 * kit reads the same bits (ArmSmmu4KContext.c:34). */
static unsigned
dbyGpuSmmuInputBits(uint32_t id2)
{
	static const uint8_t bits[5] = { 32, 36, 40, 42, 44 };
	const unsigned code = (id2 >> 8) & 0xF;

	return code < 5u ? bits[code] : 48u;
}

/* Output address size from ID2[7:4].  CORRECTED 2026-11-03 03:29 after review: this is NOT
 * "32 + code".  The official rule is the table {32,36,40,42,44,48} indexed by OAS, then CAPPED at 40
 * (NativeAdrenoGmuContext.cpp:31-35, "output_bits from ID2.OAS, capped at 40").  The two ID2 values
 * recorded in the official tests make the difference visible: 0x1433 must read 40 and 0x5511 must read
 * 36, where the earlier arithmetic gave 35 and 33. */
static unsigned
dbyGpuSmmuOutputBits(uint32_t id2)
{
	static const uint8_t bits[6] = { 32, 36, 40, 42, 44, 48 };
	const unsigned code = (id2 >> 4) & 0xF;
	unsigned value = code < 6u ? bits[code] : 48u;

	return value > 40u ? 40u : value;
}

/* Positive identification, not just "is it all-ones" (review 2026-11-03 12:55, must-fix 1).
 *
 * The first version of this gate only caught all-0/all-1 on all three words.  A dead bus can also
 * repeat one stale word, answer 0xdeadbeef-style junk, or fail a single register - and any ID2 with
 * an UBS/OAS code of 1..4 decodes into "gated=N", i.e. the same false G-1 pass.
 *
 * So ask the SMMU-500 identity questions the ArmSmmu4K kit itself asks before it will drive one, and
 * refuse to decode unless every answer is coherent:
 *   ID0[30]    S1TS    - stage-1 translation supported      (ArmSmmu4KContext.c:34)
 *   ID0[27]    SMS     - stream matching supported          (ArmSmmu4KStreams.c:17)
 *   ID0[7:0]   NUMSMRG - at least one stream mapping group  (ArmSmmu4KStreams.c:17)
 *   ID1[7:0]   NUMCB   - at least one context bank          (ArmSmmu4KProgram.c:75-78)
 *   ID1[23:16] NUMS2CB - not more stage-2 banks than banks  (ArmSmmu4KProgram.c:78)
 *   ID2[11:8]  UBS, ID2[7:4] OAS - inside {32,36,40,42,44,48}, i.e. code 0..5
 *   ID2[12]    PTFS    - 4 KiB page tables, which is all this kit implements
 *
 * ID0 is NOT a product id (arm-smmu-regs.h:61-75: it is NUMSMRG/SMS/S1TS and friends); an earlier
 * comment here said otherwise and review 12:55 corrected it.
 *
 * Returns nullptr when the block answers like a real SMMU, else the name of the first failing
 * capability.  No verdict is printed for a rejection: the caller only reports raw values plus this
 * word, because a rejected block is a measurement failure, not a small SMMU. */
static const char *
dbyGpuSmmuReject(uint32_t id0, uint32_t id1, uint32_t id2)
{
	const unsigned ubs = (id2 >> 8) & 0xFu;
	const unsigned oas = (id2 >> 4) & 0xFu;
	const unsigned banks = id1 & 0xFFu;
	const unsigned s2 = (id1 >> 16) & 0xFFu;

	/* Any single word pinned at an extreme is a bus artefact, not a capability field. */
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
	if (ubs > 5u || oas > 5u) {
		return "width-code";
	}
	if ((id2 & (1u << 12)) == 0u) {
		return "PTFS4K";
	}
	/* Re-review (2026-11-03 13:05): the field checks above still admit a bus that repeats ONE word,
	 * e.g. W=0x48001406 three times passed every test and printed gated=N.  Three more, cheap: */
	if (id0 == id1 || id1 == id2 || id0 == id2) {
		return "repeat";        /* no real SMMU answers the same word to ID0, ID1 and ID2 */
	}
	if ((id2 & 0xFu) > 5u) {
		return "IAS";           /* ID2[3:0]: input address size, same 0..5 table */
	}
	{
		/* ID1 page geometry, exactly what ArmSmmu4KProgram.c:72-79 requires before programming a bank. */
		const unsigned pages = 1u << (((id1 >> 28) & 7u) + 1u);
		const unsigned stride = (id1 >> 31) ? 65536u : 4096u;

		if (banks > pages) {
			return "NUMCB>pages";
		}
		if ((uint64_t)2u * pages * (uint64_t)stride > 0x10000ull) {
			return "regspace";  /* the 0x10000 map this probe reads could not hold 2*pages*stride */
		}
	}
	return nullptr;
}

void
sheng_gpu_smmu_probe(void)
{
	OSSharedPtr<IOMemoryMap> keep;
	uintptr_t base = 0;
	char *row;
	const char *end;
	uint32_t id0, id1, id2;
	unsigned inBits, outBits;

	if (native_gpusmmu_stage != 0u) {
		return;
	}
	native_gpusmmu_stage = 1u;

	/* Opt-in only: see SAFETY above. */
	{
		unsigned want = 0;

		if (!PE_parse_boot_argn("dby-gpu-smmu", &want, sizeof want) || want != 1u) {
			row = native_gpusmmu_census[0];
			end = row + 119;
			row = putStr(row, end, "GPU SMMU skipped: pass dby-gpu-smmu=1 to read it");
			*row = 0;
			native_gpusmmu_stage = 3u;
			return;
		}
	}

	/* 0x3da0000 + 0x10000, from this board's kona-gpu.dtsi:263-266. */
	if (!dbyGpuSmmuMap(0x03da0000ull, 0x10000ull, &keep, &base)) {
		row = native_gpusmmu_census[0];
		end = row + 119;
		row = putStr(row, end, "GPU SMMU nomap: IODeviceMemory::withRange/map failed");
		*row = 0;
		native_gpusmmu_stage = 4u;
		return;
	}

	id0 = *(volatile uint32_t *)(base + 0x20);
	id1 = *(volatile uint32_t *)(base + 0x24);
	id2 = *(volatile uint32_t *)(base + 0x28);

	/* A block that does not identify itself as an SMMU-500 gets raw values and the name of the first
	 * failing capability - never a G-1 verdict.  This is the gate review must-fix 1 (12:55) asked for;
	 * the earlier all-extremes test is now only one of its cases. */
	{
		const char *reject = dbyGpuSmmuReject(id0, id1, id2);

		if (reject != nullptr) {
			row = native_gpusmmu_census[0];
			end = row + 119;
			row = putStr(row, end, "GPU SMMU no verdict: ");
			row = putStr(row, end, reject);
			row = putStr(row, end, " id0=");
			row = putHex8(row, end, id0);
			row = putStr(row, end, " id1=");
			row = putHex8(row, end, id1);
			row = putStr(row, end, " id2=");
			row = putHex8(row, end, id2);
			*row = 0;
			native_gpusmmu_stage = 5u;
			return;
		}
	}

	inBits = dbyGpuSmmuInputBits(id2);
	outBits = dbyGpuSmmuOutputBits(id2);

	row = native_gpusmmu_census[0];
	end = row + 119;
	row = putStr(row, end, "GPU SMMU id0=");
	row = putHex8(row, end, id0);
	row = putStr(row, end, " id1=");
	row = putHex8(row, end, id1);
	row = putStr(row, end, " id2=");
	row = putHex8(row, end, id2);
	*row = 0;

	row = native_gpusmmu_census[1];
	end = row + 119;
	row = putStr(row, end, "GPU SMMU input_bits=");
	row = putDec(row, end, inBits);
	row = putStr(row, end, " oas=");
	row = putDec(row, end, outBits);
	/* inBits <= 32 is the gate: GpuProbe refuses every non-kernel client with e00002be.
	 * No encoding fallback reaches this line any more: dbyGpuSmmuReject() above refuses any ID2 whose
	 * UBS/OAS code is outside 0..5, so 48 here means a genuine 48-bit encoding (code 5), not the
	 * official helper's "unknown code" default.  Review must-fix 3, 2026-11-03 12:55. */
	row = putStr(row, end, inBits <= 32u ? " gated=Y (publish will refuse user clients)"
	                                     : " gated=N (publish accepts user clients)");
	*row = 0;

	native_gpusmmu_stage = 2u;
	(void)keep;
}

}  /* extern "C" */
