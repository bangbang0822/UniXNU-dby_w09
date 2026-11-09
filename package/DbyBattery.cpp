// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// DbyBattery.cpp — read-only SPMI PMIC-arbiter census for DBY-W09 (SM8250 / kona).
//
// WHAT THIS FILE IS, AND WHAT IT IS NOT
//   This is increment 1 of the battery/charger/power-supply track, and it deliberately does not read
//   the battery. iOS wants an IOPMPowerSource; on this board every number that class needs (SOC,
//   voltage, current, temperature, charger stage, VBUS presence) lives in the PM8150B's gen4 fuel
//   gauge and SMB5 charger, and BOTH are SPMI peripherals of USID 2 behind the SPMI PMIC arbiter
//   (vendor pm8150b.dtsi:11-13 puts PM8150B at `0x2 SPMI_USID`; its charger and gauge sit at
//   0x1000..0x1700 and 0x4000..0x43FF). Nothing in this board's image talks SPMI today: no
//   components.toml entry, and no entry in board.toml:27 [components].include, provides an SPMI bus,
//   a QPNP PMIC, a fuel gauge or a charger. So the first question is not "what is the charge level",
//   it is "is the battery PMIC even visible to the APSS execution environment" — and the arbiter's
//   own tables answer exactly that.
//
//   Therefore: this file maps the arbiter, reads two of its read-only tables, and publishes one
//   diagnostic row. It contains **no register write of any kind** — not to the PMIC, and not to the
//   arbiter's channel/observer command registers either (one SPMI transaction needs one: the command
//   word at channel+0x00, spmi-pmic-arb.c:27). It does not publish an IOPMPowerSource, does not set
//   any capacity, does not touch the charger, and cannot start, stop or bias charging.
//   work/tracks/battery/SPEC.md is the design this implements; its sections (b)-(d) are why the
//   increment is cut here and what a later one may add.
//
// WHY THE ARBITER'S OWN TABLES, AND NOTHING ELSE
//   PM8150B, like every PMIC on this SoC, is only reachable through the SPMI arbiter at 0xC440000
//   (vendor kona.dtsi:3482-3499). Before any transaction can be issued the AP has to know which APID
//   (arbiter channel) carries the PPID (peripheral) it wants, because a v5 arbiter maps more than one
//   APID per PPID and the *ownership table* says which execution environment may write each one
//   (spmi-pmic-arb.c:901-944). Reading those two tables is:
//     core window 0xC440000   APID -> PPID map at 0x900 + 4*i (v5)    spmi-pmic-arb.c:1113-1116
//     cnfg window 0xC40A000   per-APID write-EE at 0x700 + 4*i        spmi-pmic-arb.c:50-51
//   and it needs no transaction, no command word and no PMIC access of any kind.
//
// WHAT THE ROW MEANS
//   `BATTERY m1 st0 arb=50010000 ee0 fg=160 chg=144 vd=158 pmb=9 apid=512 w=0 iq=1`
//     m    the boot-argument mode (always 1 here; this increment has no mode 2)
//     st   0 ok / 2 core window not mappable / 3 arbiter is not v5 / 4 cnfg window not mappable /
//          5 no APID for USID 2 at all (the battery PMIC is invisible to this EE) /
//          6 USID 2 visible but one of the three peripherals below has no APID
//     arb  the arbiter VERSION word, core+0x0000, raw hex (spmi-pmic-arb.c:20)
//     ee   the execution environment this DT says the AP is (kona.dtsi:3492 `qcom,ee = <0>`)
//     fg   APID of PPID 0x240 (PM8150B fg-batt-soc, DT reg 0x4000, pm8150b.dtsi:310-312)
//     chg  APID of PPID 0x210 (PM8150B SMB5 CHGR,  DT reg 0x1000, pm8150b.dtsi:97-98)
//     vd   APID of PPID 0x231 (PM8150B VADC,       DT reg 0x3100, pm8150b.dtsi:379-381)
//     pmb  how many APIDs in the map belong to USID 2
//     apid how many APID slots were scanned (bounded by the core window, spmi-pmic-arb.c:910-912)
//     w    the write-EE recorded for the fg APID; 0 means APSS may write that peripheral
//          (SPMI_OWNERSHIP_PERIPH2OWNER, spmi-pmic-arb.c:51); `-` when the APID was not found
//     iq   1 when the fg APID's map entry carries the IRQ-owner bit (spmi-pmic-arb.c:46, :918):
//          without it that peripheral's PMIC interrupts are not routed to this EE
//   The example values above are illustrative, not measured — see UNVERIFIED below.
//
// BOOT ARGUMENT (gated OFF by default)
//   `dby-battery=1` runs the census. Absent, `=0`, or any other value: the entry point returns before
//   mapping anything, nothing is read, `native_battery_row` stays empty and `native_battery_stage`
//   stays 0 — an image built with this archive and no boot argument behaves exactly like today's.
//   The argument reaches the device through board.toml [payload] kernel_args, which costs a payload
//   rebuild, not a kernel rebuild.
//
// WHERE THE RESULT IS VISIBLE
//   Three places, all existing mechanisms; no second logging mechanism is invented:
//     1. IOLog("DbyBattery: <row>") — the kernel log (this board boots with `serial=3`), reported the
//        way this repository's DbyTouch driver reports (package/DbyTouch/DbyTouch.cpp:1222).
//     2. boot_progress_stage(row) — the diagnostic page's stage line (BootProgress.c:634), declared
//        weak_import the way package/LegacyDisplayProbe.cpp:86 already declares it.
//     3. `native_battery_row[120]` / `native_battery_stage` — the same 120-byte page-row ABI as
//        native_cpufreq_row (CpuFreqProbe.cpp:140-141, consumed at BootProgress.c:362 and
//        BootLog.c:640) and native_gpupower_rows (DbyGpuPowerProbe.cpp:157, consumed at
//        BootLog.c:667-669). No consumer exists for the battery row yet and this agent may not add
//        one: ORCHESTRATOR ACTION (3) is the exact four lines that give it one.
//
// ============================================================================================
// ORCHESTRATOR ACTION REQUIRED — files this agent was not allowed to touch. Nothing below is applied.
//
// (1) configs/dby_w09_defconfig:131 — append ONE force_load word to the existing line (a second
//     CONFIG_XNU_EXTRA_LD_ARCHIVES line would replace this one: the defconfig is last-wins).
//     The tail before:
// ... -Wl,-force_load,{tree}/build/gpupower/libDbyGpuPowerProbe.a {tree}/build/adreno-kits/libAdrenoKits.a"
//     The tail after:
// ... -Wl,-force_load,{tree}/build/gpupower/libDbyGpuPowerProbe.a -Wl,-force_load,{tree}/build/battery/libDbyBattery.a {tree}/build/adreno-kits/libAdrenoKits.a"
//     force_load is a correctness requirement, not a preference: this file defines
//     sheng_battery_probe() STRONGLY, package/LinkStubs.c:116-119 defines it WEAKLY, and
//     libLinkStubs.a is listed FIRST. A plain archive path lets ld satisfy the display archive's
//     reference from the weak stub and never pull this member in — the census would silently never
//     run (the trap build-cpufreq.py:26-30 documents for the CPU probe). It also lets the strong
//     native_battery_row here win over any weak stand-in.
//
// (2) board.toml [kernel] archives — one line, inserted after "build-cpufreq.py", (board.toml:144):
//         "build-battery.py",
//     kernels.py:archive_plan requires every kernel-stage component that has a recipe to be listed.
//
// (3) package/BootLog.c — OPTIONAL; the only way the row reaches the archived log on a board with no
//     console. Next to the existing weak_import block (BootLog.c:149-150) add:
//         extern char native_battery_row[120] __attribute__((weak_import));
//     and, after the native_gpupower_rows block inside the snapshot (BootLog.c:667-669):
//         if (&native_battery_row && native_battery_row[0]) at = put(out, at, cap, native_battery_row,
//                                                              sizeof native_battery_row);
//     Without it the row is still on the diagnostic page and in IOLog; it is simply not archived.
//
// (4) board.toml [payload] kernel_args — add "dby-battery=1" only when the census should run. The
//     argument list is close to its size limit (CpuFreqProbe.cpp:99-100 records ~996/1000), so drop
//     an existing argument of comparable length in the same edit.
// ============================================================================================
//
// UNVERIFIED — read this before believing any of the above
//   * No part of this has run on hardware. The two arbiter windows are unmapped in today's XNU and no
//     probe has ever read them on this board, so "the read returns an arbiter version" is a
//     prediction, not a measurement. The design is built around that: the version word is read first
//     and a value outside the arbiter's own ranges stops everything before the second window is
//     touched (runCensus, isV5).
//   * The APID the map returns for each PPID is whatever firmware left behind at hand-off.
//   * `qcom,ee = <0>` is the DT's claim (kona.dtsi:3492) that the AP is EE0. Nothing here proves the
//     firmware programmed the ownership table that way; `w=` is what makes a mismatch visible.
//   * That reading the core/cnfg windows is safe with no clock or power vote of their own is an
//     argument from the DT (the spmi node lists no `clocks`, kona.dtsi:3482-3499, so the block is
//     always-on) and from the same register region's UFS and TLMM windows reading fine on this
//     board. It is NOT a measurement. SPEC.md §c/d cover the failure mode and the gate.
//   * The v2/v3 APID-map layout (0x800, spmi-pmic-arb.c:1108-1111) is not implemented on purpose:
//     this SoC's DT geometry is v5-shaped and reading the wrong table base on a v3 arbiter would
//     report nonsense, so any non-v5 version is reported and the census stops.
//
// Sibling references this file copies rather than invents: the page-row and report idiom is
// CpuFreqProbe.cpp:137-187 and :237-269; the window-mapping idiom is CpuFreqProbe.cpp:278-288
// (IODeviceMemory::withRange + map(kIOMapAnywhere | kIOMapInhibitCache)); the boot-argument idiom is
// CpuFreqProbe.cpp:572-579; the "define both the C name and its C++ mangling" alias is
// CpuFreqProbe.cpp:621-633.

#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

extern "C" {

// The diagnostic-page ABI, not this file's invention: 120 bytes, one line, exactly the shape of
// native_cpufreq_row (CpuFreqProbe.cpp:140) and of each native_gpupower_rows line
// (DbyGpuPowerProbe.cpp:157). Consumers declare them weak_import so a boot without this archive
// renders what it renders today; these definitions are strong on purpose.
char native_battery_row[120];
volatile uint32_t native_battery_stage;

// The diagnostic page's stage line, declared exactly the way package/LegacyDisplayProbe.cpp:86
// declares it — file scope, C linkage, weak_import — and defined by BootProgress.c:634. weak_import
// so an image without the boot-progress archive still links; the address test in publishRow() is the
// same one BootProgress.c:362 and BootLog.c:640 use for their own weak_import rows.
void boot_progress_stage(const char *text) __attribute__((weak_import));

}

namespace {

// ---- board constants (each one carries the line it came from) -----------------------------------

// The arbiter windows, verbatim from the vendor DT's `reg`/`reg-names`
// (arch/arm64/boot/dts/vendor/qcom/kona.dtsi:3484-3489):
//   "core" 0x0c440000 +0x1100, "chnls" 0x0c600000 +0x2000000, "obsrvr" 0x0e600000 +0x100000,
//   "intr" 0x0e700000 +0xa0000, "cnfg" 0x0c40a000 +0x26000.
// Only core and cnfg are mapped: both hold configuration tables, so reading them needs no SPMI
// transaction. `chnls` (32 MiB) and `obsrvr` are the transaction engines and are deliberately not
// mapped — this increment issues no transaction.
constexpr uint64_t kCoreBase = 0x0C440000ull;
constexpr uint64_t kCoreSize = 0x1100ull;     // kona.dtsi:3484
constexpr uint64_t kCnfgBase = 0x0C40A000ull;
// The cnfg window is 0x26000 bytes (kona.dtsi:3488). The ownership table this file reads ends at
// 0x700 + 4*512 = 0xEFC, so one page is the whole requirement; mapping less than the DT declares is
// intentional.
constexpr uint64_t kCnfgSize = 0x1000ull;

// The v5 APID -> PPID map base and the ownership-table base (all drivers/spmi/spmi-pmic-arb.c):
//   0x900 + 4*n  pmic_arb_apid_map_offset_v5, :1113-1116
//   0x700 + 4*n  SPMI_OWNERSHIP_TABLE_REG,    :50
constexpr uint32_t kApidMapBase = 0x900u;
constexpr uint32_t kOwnershipBase = 0x700u;

// Register and field layout of the arbiter's own tables (all drivers/spmi/spmi-pmic-arb.c):
constexpr uint32_t kRegVersion = 0x0000u;          // PMIC_ARB_VERSION, :20
constexpr uint32_t kVersionV5Min = 0x50000000u;    // PMIC_ARB_VERSION_V5_MIN, :23
constexpr uint32_t kVersionMaxExclusive = 0x70000000u;
constexpr uint32_t kPpidMask = 0xFFFu;             // PMIC_ARB_PPID_MASK, :97
constexpr uint32_t kPpidShift = 8u;                // `ppid = (regval >> 8) & PMIC_ARB_PPID_MASK`, :917
constexpr uint32_t kApidIrqOwnerBit = 1u << 24;    // PMIC_ARB_CHAN_IS_IRQ_OWNER, :46 / :918
constexpr uint32_t kWriteEeMask = 0x7u;            // SPMI_OWNERSHIP_PERIPH2OWNER, :51
constexpr unsigned kMaxApids = 512u;               // PMIC_ARB_MAX_PERIPHS, :92

// The PPIDs of the three peripherals this track needs first. PPID = (USID << 8) | (peripheral
// address >> 8) — spmi-pmic-arb.c:977 (v2) and :996 (v5).
//   USID 2          pm8150b.dtsi:11-13  `qcom,pm8150b@2 { reg = <0x2 SPMI_USID>; }`
//   0x210 CHGR      pm8150b.dtsi:97-98    reg = <0x1000 0x100>
//   0x240 fg-batt-soc pm8150b.dtsi:310-312 reg = <0x4000 0x100>
//   0x231 VADC      pm8150b.dtsi:379-381  reg = <0x3100 0x100>, <0x3700 0x100>
constexpr uint32_t kPmicUsid = 2u;
constexpr uint32_t kPpidFgSoc = (kPmicUsid << 8) | 0x40u;   // 0x240
constexpr uint32_t kPpidChgr = (kPmicUsid << 8) | 0x10u;    // 0x210
constexpr uint32_t kPpidVadc = (kPmicUsid << 8) | 0x31u;    // 0x231

// The EE the DT says the AP is: kona.dtsi:3492 `qcom,ee = <0>;`. Used only to prefer this EE's own
// APID when one PPID is mapped more than once — the rule in pmic_arb_read_apid_map_v5
// (spmi-pmic-arb.c:930-932, "First PPID mapping or one for this EE").
constexpr unsigned kOwnEe = 0u;

constexpr unsigned kRowCap = 120u;      // the page ABI width
constexpr unsigned kModeCensus = 1u;    // the only mode this increment has

static_assert(sizeof native_battery_row == kRowCap, "page row ABI is 120 bytes");

// ---- state --------------------------------------------------------------------------------------

enum Status : unsigned {
    kStatusOk = 0,           // USID 2 visible and fg, chgr and vadc all have an APID
    kStatusCoreUnmapped = 2, // the core window could not be mapped
    kStatusNotV5 = 3,        // the version word is not a v5 arbiter
    kStatusCnfgUnmapped = 4, // the cnfg (ownership) window could not be mapped
    kStatusNoUsid2 = 5,      // the APID map has no entry for USID 2
    kStatusPartial = 6,      // USID 2 is visible but one of the three has no APID
};

struct Census {
    uint32_t version;    // core+0x0000, raw
    unsigned entries;    // APID slots scanned
    unsigned usid2;      // APIDs whose PPID belongs to USID 2
    int fg;              // APID or -1
    int chgr;
    int vadc;
    int fgWriteEe;       // write-EE of the fg APID, -1 when not found
    unsigned fgIrqOwner; // 1 when the fg APID's map entry has BIT(24) set
    unsigned status;
};

IOMemoryMap *gCoreMap;
IOMemoryMap *gCnfgMap;
bool gRan;

// Publish the row through the two mechanisms this board already has. No third mechanism, and no
// state of its own: whatever renderRow() produced is what both consumers see.
void publishRow(void) {
    if (!native_battery_row[0]) return;
    IOLog("DbyBattery: %s\n", native_battery_row);
    if (&boot_progress_stage) boot_progress_stage(native_battery_row);
}

// ---- register access (the idiom this repository already uses: CpuFreqProbe.cpp:215-219) --------

inline uint32_t rd32(uintptr_t base, uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t *>(base + reg);
}

// ---- text assembly (no snprintf here; see CpuFreqProbe.cpp:40-43 and :233-269) -----------------

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

unsigned emitDec(char *out, unsigned cap, unsigned at, unsigned value) {
    char digits[10];
    unsigned n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n) at = emit(out, cap, at, digits[--n]);
    return at;
}

unsigned emitHex32(char *out, unsigned cap, unsigned at, uint32_t value) {
    const char kHex[] = "0123456789abcdef";
    for (unsigned shift = 28;; shift -= 4) {
        at = emit(out, cap, at, kHex[(value >> shift) & 0xfu]);
        if (!shift) break;
    }
    return at;
}

// An APID field: the number, or `-` when this census did not find one.
unsigned emitApid(char *out, unsigned cap, unsigned at, int apid) {
    if (apid < 0) return emit(out, cap, at, '-');
    return emitDec(out, cap, at, static_cast<unsigned>(apid));
}

// The single row. Every field is rendered from the Census, so a failure row has the same shape as a
// success row with fewer numbers — a log reader never has to learn a second format.
void renderRow(const Census &c) {
    char *o = native_battery_row;
    unsigned at = emitStr(o, kRowCap, 0, "BATTERY");
    at = emitStr(o, kRowCap, at, " m");
    at = emitDec(o, kRowCap, at, kModeCensus);
    at = emitStr(o, kRowCap, at, " st");
    at = emitDec(o, kRowCap, at, c.status);
    if (c.status == kStatusCoreUnmapped) {
        emitStr(o, kRowCap, at, " core-nomap");
        return;
    }
    at = emitStr(o, kRowCap, at, " arb=");
    at = emitHex32(o, kRowCap, at, c.version);
    if (c.status == kStatusNotV5) {
        emitStr(o, kRowCap, at, " not-v5");
        return;
    }
    at = emitStr(o, kRowCap, at, " ee");
    at = emitDec(o, kRowCap, at, kOwnEe);
    at = emitStr(o, kRowCap, at, " fg=");
    at = emitApid(o, kRowCap, at, c.fg);
    at = emitStr(o, kRowCap, at, " chg=");
    at = emitApid(o, kRowCap, at, c.chgr);
    at = emitStr(o, kRowCap, at, " vd=");
    at = emitApid(o, kRowCap, at, c.vadc);
    at = emitStr(o, kRowCap, at, " pmb=");
    at = emitDec(o, kRowCap, at, c.usid2);
    at = emitStr(o, kRowCap, at, " apid=");
    at = emitDec(o, kRowCap, at, c.entries);
    at = emitStr(o, kRowCap, at, " w=");
    at = emitApid(o, kRowCap, at, c.fgWriteEe);
    at = emitStr(o, kRowCap, at, " iq=");
    emitDec(o, kRowCap, at, c.fgIrqOwner);
}

// ---- mapping ------------------------------------------------------------------------------------

// Map one physical window uncached. kIOMapInhibitCache is the idiom this board's probes use
// (CpuFreqProbe.cpp:282-285); the map is kept for the life of the boot because this probe is
// one-shot, which is that file's own lifetime policy.
volatile uint32_t *mapWindow(uint64_t base, uint64_t size, IOMemoryMap **keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    if (!memory) return nullptr;
    *keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!*keep) return nullptr;
    return reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>((*keep)->getVirtualAddress()));
}

// ---- the census ---------------------------------------------------------------------------------

// Is this a version the v5 APID-map layout applies to? The lower bound is the vendor driver's v5
// test (spmi-pmic-arb.c:1214/1223-1224) and the upper bound is pmic_arb_read_apid_map_v5's window
// test. A zero read (window not answering) and 0xffffffff (dead bus) are both outside it — which is
// the point: a bad read stops the census before the second window is touched.
bool isV5(uint32_t version) {
    return version >= kVersionV5Min && version < kVersionMaxExclusive;
}

// Walk the APID -> PPID map and, for the PPIDs this track needs, remember the APID and its owner.
//
// The scan bound is the vendor driver's, not a guess: pmic_arb_read_apid_map_v5 stops when the
// computed offset leaves the mapped window (`offset >= pmic_arb->core_size`, spmi-pmic-arb.c:910-912)
// and core_size is the DT `reg` size (spmi-pmic-arb.c:1200). With this board's 0x1100-byte core
// window and the v5 map base of 0x900 that is (0x1100 - 0x900) / 4 = 512 entries, which is also
// PMIC_ARB_MAX_PERIPHS (spmi-pmic-arb.c:92). The bound is therefore derived, not asserted.
//
// `cnfg` may be null (its window failed to map): then the APIDs are still reported and the ownership
// column is not — `owner` is -1 in that case and the row prints `w=-`, because "the table was not
// read" and "APSS owns it" must not look the same.
void consider(Census &c, uint32_t ppid, unsigned apid, int owner, unsigned irqOwner) {
    int *slot;
    if (ppid == kPpidFgSoc) slot = &c.fg;
    else if (ppid == kPpidChgr) slot = &c.chgr;
    else if (ppid == kPpidVadc) slot = &c.vadc;
    else return;

    // One APID per PPID. Preference: an APID this EE may write; otherwise the first match wins,
    // which is what "First PPID mapping or one for this EE" (spmi-pmic-arb.c:930-932) comes down to
    // when a single EE is asking. Two APIDs with the same write owner cannot occur — the arbiter
    // allocates one APID per (PPID, EE) pair — so "the one that is ours, else the first" is exact.
    if (*slot >= 0 && owner != static_cast<int>(kOwnEe)) return;
    *slot = static_cast<int>(apid);
    if (ppid == kPpidFgSoc) {
        // The write EE of the APID this census would use. 0 means APSS owns it; anything else means
        // a transaction from here would be DENIED by the arbiter (spmi-pmic-arb.c:51, :920-924); -1
        // means the ownership table was not readable.
        // `iq` is the IRQ-owner bit of that same map entry. When a PPID is mapped for several EEs the
        // IRQ owner can be a different APID than this one; that is why the bit is reported beside the
        // APID rather than folded into it.
        c.fgWriteEe = owner;
        c.fgIrqOwner = irqOwner;
    }
}

void readApidMap(Census &c, volatile uint32_t *core, volatile uint32_t *cnfg) {
    unsigned entries = 0;
    if (kCoreSize > kApidMapBase) entries = static_cast<unsigned>((kCoreSize - kApidMapBase) / 4u);
    if (entries > kMaxApids) entries = kMaxApids;
    c.entries = entries;

    for (unsigned i = 0; i < entries; ++i) {
        const uint32_t map = rd32(reinterpret_cast<uintptr_t>(core), kApidMapBase + 4u * i);
        if (!map) continue;                                     // spmi-pmic-arb.c:915-916
        const uint32_t ppid = (map >> kPpidShift) & kPpidMask;  // :917
        if ((ppid >> 8) != kPmicUsid) continue;                 // USID 2, pm8150b.dtsi:11-13
        ++c.usid2;
        const unsigned irqOwner = (map & kApidIrqOwnerBit) ? 1u : 0u;   // :46 / :918
        int owner = -1;
        if (cnfg) {
            owner = static_cast<int>(rd32(reinterpret_cast<uintptr_t>(cnfg), kOwnershipBase + 4u * i) &
                                     kWriteEeMask);             // :920-922
        }
        consider(c, ppid, i, owner, irqOwner);
    }
}

// One pass. Every exit leaves a rendered row behind, so "the census ran and said no" and "the census
// never ran" are distinguishable in a log.
void runCensus(void) {
    Census c = {};
    c.fg = -1;
    c.chgr = -1;
    c.vadc = -1;
    c.fgWriteEe = -1;
    native_battery_stage = 1;

    volatile uint32_t *core = mapWindow(kCoreBase, kCoreSize, &gCoreMap);
    if (!core) {
        c.status = kStatusCoreUnmapped;
        renderRow(c);
        publishRow();
        native_battery_stage = 2;
        return;
    }

    // Identity first, and only then anything else: a window that answers garbage never gets a second
    // window mapped and never gets a table walked.
    c.version = rd32(reinterpret_cast<uintptr_t>(core), kRegVersion);
    if (!isV5(c.version)) {
        c.status = kStatusNotV5;
        renderRow(c);
        publishRow();
        native_battery_stage = 2;
        return;
    }

    volatile uint32_t *cnfg = mapWindow(kCnfgBase, kCnfgSize, &gCnfgMap);
    if (!cnfg) c.status = kStatusCnfgUnmapped;   // the APID map is still readable; ownership is not

    readApidMap(c, core, cnfg);
    if (c.status != kStatusCnfgUnmapped) {
        if (c.usid2 == 0) c.status = kStatusNoUsid2;
        else if (c.fg < 0 || c.chgr < 0 || c.vadc < 0) c.status = kStatusPartial;
        else c.status = kStatusOk;
    }

    renderRow(c);
    publishRow();
    native_battery_stage = 2;
}

// Boot-argument parse and the one-shot guard. `PE_parse_boot_argn` is used the way
// CpuFreqProbe.cpp:572-579 uses it: the RETURN VALUE, not a sentinel in the buffer, decides whether
// the argument was on the command line at all.
//
// Default OFF, and that is the whole safety contract of this increment: an image carrying this
// archive with no `dby-battery=` on the command line maps nothing and reads nothing.
void probeInternal(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("dby-battery", &mode, sizeof(mode))) return;
    if (mode != kModeCensus) return;
    runCensus();
}

}  // namespace

// The official entry-point name, because the consumer already calls it: package/LegacyDisplayProbe.cpp:490
// declares `extern void sheng_battery_probe(void);` and :501 calls it from
// native_legacy_display_late_probe(), next to sheng_cpufreq_probe() and native_touch_probe().
// package/LinkStubs.c:116-119 answers it weakly today, which is why the force_load in ORCHESTRATOR
// ACTION (1) is required for this definition to be the one that runs.
extern "C" void sheng_battery_probe(void) {
    if (gRan) return;   // the late probe can be reached more than once; the census is idempotent
    gRan = true;
    probeInternal();
}

// The C++-linkage spelling of the same name, kept for the reason CpuFreqProbe.cpp:621-633 keeps its
// own: the declaration in LegacyDisplayProbe.cpp sits outside that file's extern "C" region, so
// which of the two names this board's display archive imports is a property of that archive, not of
// this file. Both forms are defined, spelled with an asm label the way LegacyDisplayProbe.cpp:17-20
// already spells labels in this repository. An unused duplicate is free; a mismatch is a silent
// no-op, which is the failure this component exists to rule out.
extern "C" void sheng_battery_probe_cxx_linkage(void) __asm__("__Z19sheng_battery_probev");
extern "C" void sheng_battery_probe_cxx_linkage(void) { sheng_battery_probe(); }
