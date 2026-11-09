// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// CpuFreqProbe.cpp — EPSS CPU frequency census and vote for DBY-W09 (SM8250 / kona).
//
// WHAT THIS FILE IS
//   The official CPU-frequency component of the UniXNU boards (UniXNU-sheng, SM8550), ported to
//   this board's EPSS. The mechanism is the one the vendor driver uses (qcom-cpufreq-hw.c): the
//   control is ONE 32-bit write of a LUT ROW INDEX to perf_state, then `dsb sy`. There is no OPP,
//   no regulator, no RPMh and no device-tree parsing anywhere in it — the hardware applies the
//   voltage for the row itself, so "regulator not modelled" is not a blocker (SPEC §1).
//
// WHERE IT COMES FROM
//   Primary template: FFT-infra/UniXNU-alioth@f1883782 package/CpuFreqProbe.cpp, which is the
//   SM8250 port of the same probe. Constants and control flow are its; the four board deltas of
//   SPEC §3 are the only functional changes, plus the removal of the enable gate SPEC §4 asks for.
//   Shape and reporting format: UniXNU/UniXNU-sheng@3730af8c package/CpuFreqProbe.cpp.
//   Design: work/tracks/cpu-freq/SPEC.md (this file implements §3, §4, §6 and milestone C0).
//
// DELTAS FROM alioth (SPEC §3)
//   1  domain windows 0x18591000 / 0x18592000 / 0x18593000            (alioth: same)
//   2  kDomainCpus {4, 3, 1}  — cpu0-3 -> d0, cpu4-6 -> d1, cpu7 -> d2  (this board's DT)
//   3  no EPSS L3 vote: kona has no EPSS L3 window, so nothing is mapped for it and no 0x90
//      register exists anywhere in this file
//   4  NO `ENABLE & 1` GATE. This board's DT sets qcom,skip-enable-check (kona.dtsi:727), whose
//      only meaning in the vendor driver is "skip the enable-bit test" (qcom-cpufreq-hw.c:603-610).
//      sheng and alioth gate every vote on that bit; here that would map three windows, vote
//      nothing and report success — a silent no-op (SPEC §4). The gate is "did the LUT decode"
//      instead, and `enable` is printed as data.
//
// WHAT IS DELIBERATELY NOT PORTED
//   * mode 4 (the load governor). It needs sheng_cpu_idle_mach(), a helper that comes from the
//     sheng kernel patch (patches/xnu/zzye-sheng-cpu-idle.patch), which this tree does not have.
//     SPEC §7 C3 says not to promise it. Without mode 4 there is no use for native_cpufreq_gov,
//     native_cpufreq_boosts, native_cpufreq_frame or native_cpufreq_throttle, so none is defined
//     and this board's BootLog.c (which has no `[cpu gov]`/`[cpu throttle]` row) needs no change.
//   * the L3 window and its 0x90 vote (delta 3 above).
//   * dcvs_ctrl 0xb0. alioth reads it to decide whether to write a perf_state row per CPU; the
//     vendor driver has no REG_DCVS_CTRL at all and its meaning on kona is unverified (SPEC §3),
//     so this file neither reads nor writes it. One vote = one 32-bit store to 0x320.
//   * snprintf. DbyTouch.cpp:521-524 records that no formatting prototype is reachable from the
//     headers a driver TU gets in this tree ("no driver in this tree calls it"), and a wrong
//     declaration in kernel code is silent stack corruption. The two output forms are assembled
//     with the pointer-arithmetic helpers below instead, the same idiom DbyTouch uses for its rows.
//
// WHAT IS UNVERIFIED (do not read this file as "it works"; nothing here has been on hardware)
//   * that perf_state 0x320 takes a write on this board at all, and that the RPMh/ARC DCVS loop
//     (three dcvsh interrupts, no RPMh driver here) does not overwrite it — SPEC §9.1.
//   * the polarity of the enable bit — SPEC §9.2. That is why it is only printed.
//   * that the LUT row stride here is 4 bytes (the vendor default is 32) — SPEC §9.3. The census
//     prints the raw word beside every decoded MHz so a wrong stride is visible in one line.
//   * that 0x3c4 is this SoC's per-core cycle counter — SPEC §9.4. Only read when
//     `sheng-cpufreq-cycles=1` is passed, because a wrong offset inside the window could fault.
//   * alioth's own port was never device-verified either — SPEC §9.6.
//
// ============================================================================================
// ORCHESTRATOR ACTION REQUIRED — edits in files this agent was not allowed to touch.
// Nothing below is applied. Without (1) this component is not in the link at all.
//
// (1) configs/dby_w09_defconfig  (line 131 today; then re-run `unixnu defconfig dby_w09_defconfig`)
//     BEFORE:
// CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a {tree}/build/dby-touch/libDbyTouch.a -Wl,-force_load,{tree}/build/dby-hid-input/libDbyHidInput.a"
//     AFTER:
// CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a {tree}/build/dby-touch/libDbyTouch.a -Wl,-force_load,{tree}/build/dby-hid-input/libDbyHidInput.a -Wl,-force_load,{tree}/build/cpufreq/libCpuFreqProbe.a"
//     force_load, not a plain path, and this is the whole reason: this file defines
//     sheng_cpufreq_probe() STRONGLY, but package/LinkStubs.c:96-99 defines it WEAKLY and
//     libLinkStubs.a is listed FIRST. EXTRA_LD_ARCHIVES is appended as a plain archive (see the
//     defconfig's own comment at :111-112), so ld would satisfy the display archive's undefined
//     reference from the weak stub and never pull this member: the census would silently never
//     run. force_load also lets the strong native_cpufreq_row/native_cpufreq_stage here override
//     the weak stand-ins in package/BootProgressStubs.c (:23, :209).
//
// (2) package/LinkStubs.c lines 96-99 — OPTIONAL, once (1) is in and `nm` shows the strong
//     symbols coming from libCpuFreqProbe.a. Delete:
// __attribute__((weak)) void sheng_cpufreq_probe(void);
// __attribute__((weak)) void sheng_cpufreq_probe(void)
// {
// }
//     Keep it if you add a plain archive path instead of force_load in (1): with force_load the
//     strong definition wins and the stub is dead weight only.
//
// (3) package/BootProgressStubs.c:209 — the stand-in has the wrong type (consumers, BootProgress.c:110
//     and BootLog.c:143, declare `char native_cpufreq_row[120]`; this declares 120 uint64_t = 960 B).
//     BEFORE: __attribute__((weak)) uint64_t native_cpufreq_row[120];
//     AFTER:  __attribute__((weak)) char native_cpufreq_row[120];
//     Cosmetic while the strong definition here is linked; it is the wrong shape on the boot where
//     this archive is not.
//
// (4) package/BootLog.c — the rich census (native_cpufreq_census, kCensusRows lines) has no reader
//     yet, and this board has no console (AGENTS.md), so nothing shows it. After the `[clocks]`
//     row at BootLog.c:628-633, insert:
//     extern char native_cpufreq_census[24][120] __attribute__((weak_import));
//       ... at file scope (next to the existing extern at :143)
//     and inside the snapshot, after `at = put(out, at, cap, row, sizeof row);` of `[clocks]`:
//     if (&native_cpufreq_census) for (unsigned q = 0; q < 24; ++q)
//       if (native_cpufreq_census[q][0]) at = put(out, at, cap, native_cpufreq_census[q],
//                                                  sizeof native_cpufreq_census[q]);
//
// (5) board.toml `[payload] kernel_args` — NOT needed for the census. The census is the default
//     mode when `sheng-cpufreq` is absent, precisely because the command line is ~996/1000
//     (SPEC §10) and because C0 must cost one kernel cycle, not a payload edit. Add
//     `"sheng-cpufreq=2"` / `"3"` only when a vote is wanted, and `"sheng-cpufreq=0"` to make the
//     probe inert if the mapping turns out to fault on this board.
//     Anchor for the vote case (board.toml:357-358):
//   "-no_slto_panic",
//     Replace with:
//   "-no_slto_panic",
//   "sheng-cpufreq=2",
//     A vote needs a payload rebuild and the line only has ~4 bytes of headroom, so drop an
//     existing argument of at least 16 bytes in the same edit. The census needs none of this.
// ============================================================================================
//
// `sheng-cpufreq=<n>` (the official argument name; absent = 1):
//   0  inert: no window is mapped and no register is read
//   1  CENSUS, read-only: map the three windows and publish enable, domain_state, perf_state,
//      every decoded LUT row and the CPU mask. Writes nothing. THE DEFAULT.
//   2  census, then one vote in domain 0 (the boot cluster) once its LUT decoded
//   3  census, then one vote in each domain once that domain's LUT decoded
// Optional `sheng-cpufreq-cap=<MHz>`: mode 2/3 vote the highest non-turbo row at or below it (as upstream).
// Optional `sheng-cpufreq-cycles=1`: additionally measure each domain with its EPSS cycle counter
//   (SPEC §6 method A) and print the measured MHz in the census. Read-only. Off by default because
//   the 0x3c4 offset on kona is UNVERIFIED (SPEC §9.4).
//
// WHERE THE RESULT IS VISIBLE
//   native_cpufreq_row[120] is the page row the board already renders: BootProgress.c:356 draws
//   grid row 39 from it once native_cpufreq_stage >= 2, BootProgress.c:1110 puts it in the banner
//   as `CPU   ...`, and BootLog.c:628-633 archives it as `[clocks] ...`. native_cpufreq_census is
//   the multi-line form for the log (see ORCHESTRATOR ACTION (4)).

#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

extern "C" {

// The diagnostic-page ABI, not this file's invention. Both are declared weak on the consumer side
// (BootProgress.c:110/118, BootLog.c:143, BootProgressStubs.c:23/209) so a boot without this
// archive renders exactly what it renders today. These definitions are strong on purpose.
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;

// The rich form: one 120-byte line per census row, built once from the LUT plus the live per-domain
// header. kCensusRows (24) is the array's first dimension in every consumer; it is also written
// out below so a reader can size it without including this file.
char native_cpufreq_census[24][120];
}

namespace {

// ---- board constants (SPEC §3; every one of these is a value this port had to change) --------

constexpr uint64_t kDomainBase[3] = {0x18591000, 0x18592000, 0x18593000};
constexpr unsigned kDomainCpus[3] = {4, 3, 1};
constexpr unsigned kDomainFirstCpu[3] = {0, 4, 7};   // cpu0-3 d0, cpu4-6 d1, cpu7 d2
constexpr uint64_t kWindowSize = 0x1000;

// qcom-cpufreq-hw "epss" offsets, unchanged from upstream and matching the vendor driver
// (qcom-cpufreq-hw.c:109-119). dcvs_ctrl 0xb0 is absent on purpose — see the header.
constexpr uint32_t kRegEnable = 0x0;
constexpr uint32_t kRegDomainState = 0x20;
constexpr uint32_t kRegFreqLut = 0x100;
constexpr uint32_t kRegPerfState = 0x320;
constexpr uint32_t kRegCycles = 0x3c4;   // SPEC §6 method A; UNVERIFIED on kona, opt-in only

// SPEC §3: the row stride is 4 bytes. The vendor default is 32 and only qcom,lut-row-size=<4>
// changes it, so this constant is a claim the census has to make checkable, not an assumption.
constexpr uint32_t kLutStride = 4;
constexpr unsigned kLutRows = 40;

// A LUT that decoded into fewer than this many distinct rows is treated as "not decoded", which
// is the gate the enable bit used to be. A 32-byte stride read at 4 bytes decodes as
// [row0, 300 MHz, 300 MHz, ...] = 2 rows, so the misalignment fails the gate instead of passing it.
constexpr unsigned kMinLutRows = 4;

constexpr uint32_t kXoKHz = 19200;         // bi_tcxo_div2, the L value's unit
constexpr uint32_t kGpllHalfKHz = 300000;  // source 0 is GPLL0 600 MHz / 2 (SPEC §9.5: assumed)

constexpr unsigned kRowCap = 120;          // the page ABI width
constexpr unsigned kCensusRows = 24;
constexpr unsigned kTokensPerLine = 7;     // LUT entries per census line: 7 * 14 + 10 < 120

constexpr unsigned kModeDefault = 1;       // census; 0 makes the probe inert unless asked

static_assert(sizeof native_cpufreq_row == kRowCap, "page row ABI is 120 bytes");
static_assert(sizeof native_cpufreq_census / sizeof native_cpufreq_census[0] == kCensusRows,
              "census row count is part of the ABI line readers size their loop with");

// ---- state -----------------------------------------------------------------------------------

struct Window {
    IOMemoryMap *map;    // kept for the life of the boot: this probe is one-shot and never stops
    uintptr_t va;
    int voted;           // LUT row written to perf_state, -1 = never written
    uint32_t votedKHz;   // the frequency that row decodes to
    uint32_t preKHz;     // domain_state before the vote
    uint32_t postKHz;    // domain_state read straight back after the vote
    bool readBackOk;     // perf_state read back the level that was written
    uint32_t enable;     // raw kRegEnable word, printed as data (never used as a gate)
    uint32_t stateKHz;   // (domain_state & 0xff) * 19.2 MHz
    uint32_t perf;       // raw kRegPerfState word
    unsigned rows;       // decoded LUT rows (0 = not decoded)
    uint32_t cyclesMHz;  // measured with 0x3c4 when sheng-cpufreq-cycles=1, 0 otherwise
};

Window gDomain[3];
uint32_t gLutRaw[3][kLutRows];
unsigned gMode;
uint32_t gCapKHz;
bool gCycles;
IOLock *gLock;
volatile uint32_t gState;   // 0 idle, 1 initialising (one thread owns it), 2 ready, 3 unavailable
unsigned gMapped;

// ---- register access (the idiom this repository already uses: DbyTouch.cpp:497-515) -----------

inline uint32_t rd32(uintptr_t base, uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t *>(base + reg);
}

// The reference's write, verbatim: one 32-bit store plus `dsb sy`. Nothing else is written
// anywhere in this file in census mode, and no 0xb0 write exists at all.
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// The author's decode (SPEC §1): source bits 31:30 choose the L value's clock, source 0 is GPLL0/2.
uint32_t lutKHz(uint32_t row) {
    return (row >> 30) ? kXoKHz * (row & 0xffu) : kGpllHalfKHz;
}

// ---- text assembly (no snprintf here; see the header) -----------------------------------------

// Every helper keeps the buffer terminated: one byte is always reserved, and `at` may run past the
// end, at which point further writes are dropped. Callers never need to bounds-check themselves.
unsigned emit(char *out, unsigned cap, unsigned at, char c) {
    if (at + 1u < cap) {
        out[at] = c;
        out[at + 1u] = '\0';
    }
    return at + 1u;
}

unsigned emitStr(char *out, unsigned cap, unsigned at, const char *text) {
    for (unsigned i = 0; text[i]; ++i) at = emit(out, cap, at, text[i]);
    return at;
}

unsigned emitDec(char *out, unsigned cap, unsigned at, uint32_t value) {
    char digits[10];
    unsigned n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n) at = emit(out, cap, at, digits[--n]);
    return at;
}

// Always eight digits: the raw LUT word is what makes a wrong stride visible next to its MHz.
unsigned emitHex32(char *out, unsigned cap, unsigned at, uint32_t value) {
    const char kHex[] = "0123456789abcdef";
    for (unsigned shift = 28;; shift -= 4) {
        at = emit(out, cap, at, kHex[(value >> shift) & 0xfu]);
        if (!shift) break;
    }
    return at;
}

unsigned emitMHz(char *out, unsigned cap, unsigned at, uint32_t khz) {
    at = emitDec(out, cap, at, khz / 1000u);
    return emitStr(out, cap, at, "MHz");
}

// ---- mapping and the census ------------------------------------------------------------------

bool mapWindow(unsigned d) {
    Window &w = gDomain[d];
    IODeviceMemory *memory = IODeviceMemory::withRange(kDomainBase[d], kWindowSize);
    if (!memory) return false;
    // kIOMapInhibitCache is the idiom DbyTouch.cpp:468-478 and drivers/DbyUfs/NativeUfs.cpp use;
    // map() returns +1 and this probe never unmaps, which is the reference's own lifetime.
    w.map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
    return w.va != 0;
}

// Read the whole candidate table once: the LUT is programmed by the bootloader and does not move,
// so a later poll does not re-read it. `rows` counts up to the first repeat, which is where the
// hardware table ends (two equal frequencies in a row).
void readLuts(void) {
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        w.rows = 0;
        for (unsigned i = 0; i < kLutRows; ++i)
            gLutRaw[d][i] = w.va ? rd32(w.va, kRegFreqLut + i * kLutStride) : 0;
        if (!w.va) continue;
        uint32_t prev = 0;
        for (unsigned i = 0; i < kLutRows; ++i) {
            const uint32_t khz = lutKHz(gLutRaw[d][i]);
            if (i > 0u && khz == prev) break;
            prev = khz;
            w.rows = i + 1u;
        }
    }
}

// The enable-free gate (SPEC §4): the table decoded, so a row index means something.
bool lutUsable(unsigned d) {
    return gDomain[d].va != 0 && gDomain[d].rows >= kMinLutRows;
}

// The census reads every domain; the vote path uses the same sample for its before/after values.
void sampleDomain(unsigned d) {
    Window &w = gDomain[d];
    if (!w.va) {
        w.enable = 0;
        w.stateKHz = 0;
        w.perf = 0;
        return;
    }
    w.enable = rd32(w.va, kRegEnable);
    w.stateKHz = (rd32(w.va, kRegDomainState) & 0xffu) * kXoKHz;
    w.perf = rd32(w.va, kRegPerfState);
}

// SPEC §6 method A: the per-core EPSS cycle counter at 0x3c4 + 4 * (cpu - first + 1), timed with
// cntpct_el0 (both registers are read from C elsewhere in this tree: BootLog.c:298/618,
// BootProgress.c:335). Read-only. That the offset exists on kona is UNVERIFIED (SPEC §9.4), which
// is why it only runs when the boot argument asks for it.
uint32_t cyclesMHz(unsigned d) {
    Window &w = gDomain[d];
    if (!w.va) return 0;
    uint64_t freq = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) return 0;
    const uint32_t reg = kRegCycles + 4u * (kDomainFirstCpu[d] + 1u);
    const uint32_t before = rd32(w.va, reg);
    uint64_t t0 = 0, now = 0;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    const uint64_t span = freq / 1000u;   // ~1 ms
    if (!span) return 0;
    now = t0;
    // Bounded, because a counter that does not advance must not hang the boot thread.
    for (unsigned spin = 0; spin < 4000000u && now - t0 < span; ++spin)
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    const uint32_t after = rd32(w.va, reg);
    const uint64_t dt = now - t0;
    if (!dt) return 0;
    const uint32_t delta = after - before;   // 32-bit, no wrap inside 1 ms
    return static_cast<uint32_t>(static_cast<uint64_t>(delta) * freq / dt / 1000000ull);
}

// Line 0: this port's own identity, so a log shows which constants produced the numbers below.
void renderCensusTitle(void) {
    char *o = native_cpufreq_census[0];
    o[0] = '\0';
    unsigned at = emitStr(o, kRowCap, 0, "CPUFREQ census m=");
    at = emitDec(o, kRowCap, at, gMode);
    at = emitStr(o, kRowCap, at, " lutstride=");
    at = emitDec(o, kRowCap, at, kLutStride);
    at = emitStr(o, kRowCap, at, "B xo=");
    at = emitDec(o, kRowCap, at, kXoKHz);
    at = emitStr(o, kRowCap, at, "kHz rowmax=");
    at = emitDec(o, kRowCap, at, kLutRows);
    at = emitStr(o, kRowCap, at, " mapped=");
    at = emitDec(o, kRowCap, at, gMapped);
    at = emitStr(o, kRowCap, at, "/3 cyc=");
    at = emitDec(o, kRowCap, at, gCycles ? 1u : 0u);
    at = emitStr(o, kRowCap, at, " cap=");
    emitDec(o, kRowCap, at, gCapKHz / 1000u);
}

// One live header per domain: raw enable/domain_state/perf_state words beside their meaning, the
// CPU mask from this board's qcom,freq-domain mapping, and how many rows decoded.
void renderDomainHeader(unsigned d, unsigned line) {
    Window &w = gDomain[d];
    char *o = native_cpufreq_census[line];
    o[0] = '\0';
    unsigned at = emit(o, kRowCap, 0, 'd');
    at = emitDec(o, kRowCap, at, d);
    at = emit(o, kRowCap, at, ' ');
    at = emitHex32(o, kRowCap, at, static_cast<uint32_t>(kDomainBase[d]));
    if (!w.va) {
        emitStr(o, kRowCap, at, " nomap");
        return;
    }
    at = emitStr(o, kRowCap, at, " en=");
    at = emitHex32(o, kRowCap, at, w.enable);
    at = emitStr(o, kRowCap, at, " ds=");
    at = emitHex32(o, kRowCap, at, (w.stateKHz / kXoKHz) & 0xffu);
    at = emit(o, kRowCap, at, '=');
    at = emitMHz(o, kRowCap, at, w.stateKHz);
    at = emitStr(o, kRowCap, at, " ps=");
    at = emitHex32(o, kRowCap, at, w.perf);
    at = emitStr(o, kRowCap, at, " cpus=");
    at = emitDec(o, kRowCap, at, kDomainFirstCpu[d]);
    at = emit(o, kRowCap, at, '-');
    at = emitDec(o, kRowCap, at, kDomainFirstCpu[d] + kDomainCpus[d] - 1u);
    at = emitStr(o, kRowCap, at, " lut=");
    at = emitDec(o, kRowCap, at, w.rows);
    at = emit(o, kRowCap, at, '/');
    at = emitDec(o, kRowCap, at, kLutRows);
    if (w.voted >= 0) {
        at = emitStr(o, kRowCap, at, " v=");
        at = emitDec(o, kRowCap, at, static_cast<uint32_t>(w.voted));
        at = emit(o, kRowCap, at, '@');
        at = emitMHz(o, kRowCap, at, w.votedKHz);
        if (!w.readBackOk) at = emit(o, kRowCap, at, '!');
        at = emitStr(o, kRowCap, at, " pre=");
        at = emitMHz(o, kRowCap, at, w.preKHz);
        at = emitStr(o, kRowCap, at, " post=");
        emitMHz(o, kRowCap, at, w.postKHz);
        return;
    }
    if (w.cyclesMHz) {
        at = emitStr(o, kRowCap, at, " f=");
        emitMHz(o, kRowCap, at, w.cyclesMHz * 1000u);
    }
}

// One line per kTokensPerLine rows: `d0 r00-06 4000005e=1804 40000058=1690 ...`. Index, raw word
// and decoded MHz on the same line is the whole point — a 32-byte stride shows up here as
// `00000000=300` rows between the real ones, in this boot's log, without another cycle.
void renderLutLine(unsigned d, unsigned base, unsigned line) {
    Window &w = gDomain[d];
    char *o = native_cpufreq_census[line];
    o[0] = '\0';
    unsigned at = emit(o, kRowCap, 0, 'd');
    at = emitDec(o, kRowCap, at, d);
    at = emit(o, kRowCap, at, ' ');
    at = emit(o, kRowCap, at, 'r');
    at = emitDec(o, kRowCap, at, base);
    at = emit(o, kRowCap, at, '-');
    const unsigned end = (base + kTokensPerLine > kLutRows) ? kLutRows : base + kTokensPerLine;
    at = emitDec(o, kRowCap, at, end - 1u);
    at = emit(o, kRowCap, at, ' ');
    for (unsigned i = base; i < end; ++i) {
        if (!w.va) {
            at = emitStr(o, kRowCap, at, "- ");
            continue;
        }
        at = emitHex32(o, kRowCap, at, gLutRaw[d][i]);
        at = emit(o, kRowCap, at, '=');
        at = emitDec(o, kRowCap, at, lutKHz(gLutRaw[d][i]) / 1000u);
        at = emit(o, kRowCap, at, ' ');
    }
}

// Build the whole multi-line form. Cheap enough to redo on every poll, which keeps it free of any
// "which parts are stale" state; the LUT words themselves were read once by readLuts().
void buildCensus(void) {
    renderCensusTitle();
    unsigned line = 1;
    for (unsigned d = 0; d < 3 && line < kCensusRows; ++d) {
        renderDomainHeader(d, line++);
        for (unsigned base = 0; base < kLutRows && line < kCensusRows; base += kTokensPerLine)
            renderLutLine(d, base, line++);
    }
    for (; line < kCensusRows; ++line) native_cpufreq_census[line][0] = '\0';
}

// The page row: one line, <= 120 bytes, the reference's shape.
//   CPUFREQ d0:e1 s1804 v12@1804 p12 r16 d1:... d2:... m3
// e = enable bit (data), s = domain_state in MHz, v = the level voted @ its MHz, p = perf_state
// low byte, r = decoded rows, m = mode. `!` after v means perf_state did not read back the level:
// the store was swallowed or the DCVS loop already replaced it, which is worth seeing rather than
// reporting as success.
void refreshRow(void) {
    char *o = native_cpufreq_row;
    unsigned at = emitStr(o, kRowCap, 0, "CPUFREQ");
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        at = emit(o, kRowCap, at, ' ');
        at = emit(o, kRowCap, at, 'd');
        at = emitDec(o, kRowCap, at, d);
        if (!w.va) {
            at = emitStr(o, kRowCap, at, ":nomap");
            continue;
        }
        at = emitStr(o, kRowCap, at, ":e");
        at = emitDec(o, kRowCap, at, w.enable & 1u);
        at = emitStr(o, kRowCap, at, " s");
        at = emitDec(o, kRowCap, at, w.stateKHz / 1000u);
        if (w.voted >= 0) {
            at = emitStr(o, kRowCap, at, " v");
            at = emitDec(o, kRowCap, at, static_cast<uint32_t>(w.voted));
            at = emit(o, kRowCap, at, '@');
            at = emitDec(o, kRowCap, at, w.votedKHz / 1000u);
            if (!w.readBackOk) at = emit(o, kRowCap, at, '!');
        }
        at = emitStr(o, kRowCap, at, " p");
        at = emitDec(o, kRowCap, at, w.perf & 0xffu);
        at = emitStr(o, kRowCap, at, " r");
        at = emitDec(o, kRowCap, at, w.rows);
    }
    at = emitStr(o, kRowCap, at, " m");
    emitDec(o, kRowCap, at, gMode);
}

// ---- the vote ---------------------------------------------------------------------------------

// Highest non-turbo row at or below the cap; last one wins on equal frequencies, as upstream.
int pickLevel(unsigned d, uint32_t *khzOut) {
    int best = -1;
    uint32_t bestKHz = 0;
    const unsigned rows = gDomain[d].rows;
    for (unsigned i = 0; i < rows; ++i) {
        const uint32_t raw = gLutRaw[d][i];
        const uint32_t khz = lutKHz(raw);
        if (((raw >> 16) & 7u) == 1u) continue;                    // turbo row
        if (gCapKHz && khz > gCapKHz) continue;
        if (khz >= bestKHz) {
            best = static_cast<int>(i);
            bestKHz = khz;
        }
    }
    if (khzOut) *khzOut = bestKHz;
    return best;
}

// The one place this file writes hardware: perf_state = a LUT row index, then `dsb sy`. Not gated
// on enable, and 0xb0 is not consulted. The read-back is reported, never retried.
void voteDomain(unsigned d) {
    Window &w = gDomain[d];
    uint32_t khz = 0;
    const int level = pickLevel(d, &khz);
    if (level < 0) return;
    w.preKHz = (rd32(w.va, kRegDomainState) & 0xffu) * kXoKHz;
    wr32(w.va, kRegPerfState, static_cast<uint32_t>(level));
    w.voted = level;
    w.votedKHz = khz;
    w.readBackOk = (rd32(w.va, kRegPerfState) & 0xffu) == static_cast<uint32_t>(level);
    w.postKHz = (rd32(w.va, kRegDomainState) & 0xffu) * kXoKHz;
    native_cpufreq_stage = 3;   // 3 = a vote happened; 2 = census only
}

// One pass: sample, vote whatever is due, render. Both exported entry points go through here, and
// they call it directly rather than through sheng_cpufreq_poll(), so a weak stand-in for the poll
// symbol cannot turn the probe into a no-op.
void pollInternal(void) {
    if (!gLock) return;
    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) sampleDomain(d);
    if (gMode >= 2) {
        for (unsigned d = 0; d < 3; ++d) {
            const bool wanted = (gMode >= 3) || (gMode == 2 && d == 0);
            if (wanted && gDomain[d].voted < 0 && lutUsable(d)) voteDomain(d);
        }
        for (unsigned d = 0; d < 3; ++d) sampleDomain(d);
    }
    if (gCycles)
        for (unsigned d = 0; d < 3; ++d) gDomain[d].cyclesMHz = cyclesMHz(d);
    buildCensus();
    refreshRow();
    if (native_cpufreq_stage < 2) native_cpufreq_stage = 2;
    IOLockUnlock(gLock);
}

// Idempotent, single-owner initialisation. Two threads can reach the probe and the poll (the boot
// thread calls sheng_cpufreq_probe() from the display late probe, the progress writer calls
// sheng_cpufreq_poll() every round), so the loser of the CAS returns without touching anything and
// simply runs on its next round.
void initOnce(void) {
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&gState, &expected, 1u, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;

    // Absent = census. PE_parse_boot_argn's return value is used rather than a sentinel in the
    // buffer, so "not on the command line" and "=0" cannot be confused even if it zeroes first.
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode))) mode = kModeDefault;
    if (mode == 0 || mode > 3) {
        __atomic_store_n(&gState, 3u, __ATOMIC_RELEASE);   // explicitly off, or not a mode we have
        return;
    }
    unsigned capMHz = 0, cycles = 0;
    PE_parse_boot_argn("sheng-cpufreq-cap", &capMHz, sizeof(capMHz));
    PE_parse_boot_argn("sheng-cpufreq-cycles", &cycles, sizeof(cycles));

    gLock = IOLockAlloc();
    if (!gLock) {
        __atomic_store_n(&gState, 3u, __ATOMIC_RELEASE);
        return;
    }
    gMode = mode;
    gCapKHz = capMHz * 1000u;
    gCycles = cycles != 0;
    native_cpufreq_stage = 1;
    for (unsigned d = 0; d < 3; ++d) {
        // gDomain[] is zero-filled, and 0 is a legal LUT row: -1 is what "never voted" means, so
        // this assignment is not optional. Without it every domain would report `v0@300` on the
        // first line and the vote would never be attempted.
        gDomain[d].voted = -1;
        if (mapWindow(d)) ++gMapped;
    }
    readLuts();
    if (gCycles)
        for (unsigned d = 0; d < 3; ++d) gDomain[d].cyclesMHz = cyclesMHz(d);
    __atomic_store_n(&gState, 2u, __ATOMIC_RELEASE);
}
}

// ---- entry points: the official names, because the consumers already call these -----------------

extern "C" void sheng_cpufreq_poll(void) {
    initOnce();
    if (__atomic_load_n(&gState, __ATOMIC_ACQUIRE) != 2u) return;
    pollInternal();
}

extern "C" void sheng_cpufreq_probe(void) {
    initOnce();
    if (__atomic_load_n(&gState, __ATOMIC_ACQUIRE) != 2u) return;
    pollInternal();
}

// One alias, and it is a link-time fact rather than a guess. The caller in
// package/LegacyDisplayProbe.cpp:484 reads `extern void sheng_cpufreq_probe(void);` and that line is
// OUTSIDE the file's only extern "C" region (:52-114), so this tree's build gives it C++ linkage
// and the reference is `__Z19sheng_cpufreq_probev`; upstream alioth's caller sits inside an
// extern "C" block that runs to the end of its file (alioth LegacyDisplayProbe.cpp:49 and the note
// at :968-971) and so needs the C name. Which of the two this board's display archive produced
// cannot be checked here (scripts/build-arm-driver.py belongs to the assembled tree), and a
// mismatch would be exactly the silent no-op this component exists to rule out: alioth's caller
// guards on `if (&sheng_cpufreq_probe)`, where a mismatched name simply reads 0. So both forms are
// defined. The second name is the first one's Itanium mangling, spelled with an asm label the way
// LegacyDisplayProbe.cpp:17-20 already spells labels in this repository.
extern "C" void sheng_cpufreq_probe_cxx_linkage(void) __asm__("__Z19sheng_cpufreq_probev");
extern "C" void sheng_cpufreq_probe_cxx_linkage(void) { sheng_cpufreq_probe(); }
