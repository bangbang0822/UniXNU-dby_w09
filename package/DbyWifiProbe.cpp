/* DbyWifiProbe — the Wi-Fi/Bluetooth track's read-only board census. SPEC M0 lives here.
 *
 * WHAT THIS INCREMENT ANSWERS
 *   SPEC M0: which bus carries Bluetooth HCI on this board - the SE0 GENI UART at 0x980000, the
 *   SE6 GENI UART at 0x998000, or SLIMbus (slim@3ac0000)?  work/tracks/wifi-bt/M0-ANSWER.md is the
 *   evidence; this file is the half that can be measured without firmware, without a host, and
 *   without writing a single register.
 *
 * WHAT IT DOES NOT DO, AND WHY THAT IS A DESIGN CHOICE RATHER THAN AN UNFINISHED PART
 *   - No MMIO store exists in this file.  Not "gated": absent.  There is no write helper, no
 *     `*(volatile uint32_t *)x = y`, and DBY_BT_PROBE_ALLOW_WRITES being defined at all is a
 *     compile-time error (the mechanism SPEC N6 requires, kept in the shape it already had in
 *     work/tracks/wifi/STATUS.md:19).
 *   - No rail, no GPIO direction change, no PERST#, no WLAN_EN/BT_EN toggle, no firmware, no
 *     PCIe window.  M1..M10 are not implemented here; the file says so instead of pretending.
 *   - No wait loop of any kind.  Every access is a single load with a bounds check against the
 *     window it belongs to; there is no poll, no timeout, no IODelay.
 *
 * WHAT HAPPENS ON A BUS FAULT (stated because the board has paid for this twice)
 *   An MMIO load to a block whose clocks or power are off does not return an error on this part: it
 *   either reads as garbage or hangs the interconnect, and a hang inside a kext on a board with no
 *   console is a dead device (2026-11-03, the GPU SMMU read at 0x03da0000, work/tracks/gpu-smmu-read-hang).
 *   XNU has no recoverable SError path here, so "bound every access" cannot mean "catch the abort";
 *   it means DO NOT READ A BLOCK THAT IS NOT PROVEN READABLE:
 *     mode 1  TLMM and GCC only.  TLMM is always-on (the vendor pinctrl probe maps it and never asks
 *             for a clock: $V/drivers/pinctrl/qcom/pinctrl-msm.c:1588-1589) and GCC is readable in
 *             any branch state, which clk_branch_is_enabled() relies on ($V/drivers/clk/qcom/
 *             clk-branch.c:47, :52-67) and which this board has already demonstrated for two GCC
 *             pages (package/DbyGpuPowerProbe.cpp:36-40).  No serial engine, no PCIe, no LPASS.
 *     mode 2  mode 1 plus the SE0/SE6 GENI windows, and those only after BOTH the QUPv3 wrapper
 *             votes (core + m_ahb + s_ahb) and the engine's own branch read enabled-and-not-halted.
 *             That is the same prerequisite gate the GPU probe puts in front of the SMMU read, and
 *             it is why dby_bt_m0_branch_running() has no "vote alone is enough" shortcut.
 *   If a freeze happens anyway, the rows published so far are the previous boot's; that is why the
 *   final rows are written only after every read has returned, and why a progress row is published
 *   before the first load (the lesson recorded in package/DbyGpuPowerProbe.cpp:26-28).
 *
 * BOOT ARGUMENT
 *   `dby-bt-m0=<1|2>`; absent or 0 means the probe writes one "skipped" row and touches nothing.
 *   The default is off for two reasons: a probe that runs unasked is an unrequested MMIO access on
 *   the machine the Wi-Fi side is being careful with, and M0's evidence is only worth a flash when
 *   someone is going to read it.  1 is the mode this increment asks anyone to run; 2 adds the GENI
 *   identity reads and is for a boot where mode 1 already showed the SE clocked.
 *
 * ROWS (six 120-byte rows, the package/BootLog.c renderer's shape)
 *   row0  DbyBt: M0 m=<mode> tlmm=<0|1> gcc=<0|1> geni=<0|1> verdict=<UART-SE0|UART-SE6|UNKNOWN> why=<reason>
 *   row1  DbyBt: M0 se0 28=<cfg> 29=<cfg> 30=<cfg> 31=<cfg> f=<four function indices>
 *   row2  DbyBt: M0 se6 16=<cfg> 17=<cfg> 18=<cfg> 19=<cfg> f=<four function indices>
 *   row3  DbyBt: M0 gcc vote=<0x152008> core=<0x123000> se0=<0x27000c> se6=<0x27702c>
 *   row4  DbyBt: M0 pin bt_en=<0|1> wlan=<0|1> sw=<0|1> ind=<0|1> rx31=<0|1> tx30=<0|1>
 *   row5  DbyBt: M0 qup=<on|off> se0clk=<0|1> se6clk=<0|1> geni=<not-read|answered|silent> [init= fw= ios=]
 *   A row this mode did not measure prints "not-read", never a fabricated zero: an unmeasured word
 *   and a measured 0x00000000 are the two things a log reader must be able to tell apart.
 *   Row1's `f=` field is the crux: the vendor's pingroup table puts gpio first and the QUP second
 *   ($V/drivers/pinctrl/qcom/pinctrl-kona.c:26-43, :1474-1479 for gpio28-31 qup0, :1456-1459 for
 *   gpio16-19 qup6), and pinctrl-msm.c:169-183 writes that array index into FUNC_SEL, so f=1111 on
 *   row1 means "some Linux-class client muxed all four SE0 pins onto qup0 and TLMM has not been
 *   reset since", and f=0000 means the engine is detached.
 *
 * ROW SYMBOLS: native_bt_rows / native_bt_stage, deliberately NOT native_wifi_stats.
 *   package/BootLog.c already carries an unused Wi-Fi row schema (native_wifi_stats[29],
 *   native_wifi_text[512][112], ... at :85-99, rendered at :679-818).  This probe does not fill it,
 *   for two reasons that are worth writing down because the next increment will be tempted:
 *     1. Every field in that row is decoded with a Wi-Fi/PCIe label (`gdsc=`, `ltssm=`, `lnk=`,
 *        `mhi:`).  Filling it with pin-mux words would produce a row that says "gdsc=0" about a
 *        register nobody read - a fabricated measurement, which is the one thing a boot log on this
 *        board may not contain.
 *     2. That schema belongs to the Wi-Fi probe still to be written.  Two definitions of
 *        native_wifi_stats in one archive is a link error, so a Bluetooth probe squatting on it
 *        would have to be undone later.
 *   So the rows use the same MECHANISM (a weak-imported char[N][120] that BootLog.c renders
 *   verbatim, plus a stage word for the one-shot) under their own name.  The seven lines BootLog.c
 *   needs are prepared, NOT applied, in work/tracks/wifi-bt/IMPLEMENTATION.md - package/BootLog.c is
 *   the parent's file.
 *
 * CALL SITE
 *   package/BootProgress.c:1251-1259 is where this board calls its late probes, after
 *   boot_log_slots_ok has counted a fully successful log round.  This probe keeps that gate itself
 *   as well (see dby_wifi_probe_entry) so that a careless caller cannot make it read hardware before
 *   there is somewhere to record the result.  The two-line call-site edit is in IMPLEMENTATION.md.
 *
 * PROVENANCE
 *   Every address and every decode rule is in package/DbyWifiProbeBoard.h with a file:line into
 *   _dby/robyy10__DBY-W09.tar/DBY-W09-main.  Nothing here re-derives a hardware value.
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

#include "DbyWifiProbeBoard.h"

/* The read-only gate, kept as the SPEC requires it (N6).  It is also structurally true: this file
 * contains no store to any address.  Anyone who needs a write adds a new file, a new review and a
 * new argument; they do not switch this one off. */
#if defined(DBY_BT_PROBE_ALLOW_WRITES) && DBY_BT_PROBE_ALLOW_WRITES
#error "DbyWifiProbe is a read-only census; there is no MMIO store to enable in this file."
#endif

extern "C" {

/* Row storage the logger reads.  Same contract as native_gpupower_rows (package/BootLog.c:150,
 * rendered at :667-671): six rows of 120 bytes, rendered verbatim, a blank row skipped. */
char native_bt_rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES];

/* 0 = not entered, 1 = entered, 2 = measured, 3 = skipped (no argument), 4 = no window mapped.
 * Kept separate from the rows so a reader of the archive can tell "ran" from "wrote a row". */
volatile uint32_t native_bt_stage;

/* The archive writer's progress, weak-imported from package/BootLog.c:206.  The probe refuses to run
 * until at least one round has genuinely landed (slot + message buffer + header + sync), because a
 * hardware read whose result cannot be stored is a pure risk.  This is the gate the GPU probe was
 * given by review 2026-11-03 15:30 (package/DbyGpuPowerProbe.cpp:162-165, :5166-5174) and it is
 * copied here rather than re-invented. */
extern volatile uint32_t boot_log_slots_ok __attribute__((weak_import));

} /* extern "C" */

/* ------------------------------------------------------------------ */
/* Windows                                                            */
/* ------------------------------------------------------------------ */

/* TLMM is mapped per TILE, not from 0xF100000: the block starts at 0xF000000 with a 16 MiB
 * aperture ($VD/kona-pinctrl.dtsi:4) and the pins this probe needs live in the NORTH tile at
 * +0x900000 ($V/drivers/pinctrl/qcom/pinctrl-kona.c:20-25).  dts/dby_w09.dts maps only the WEST
 * tile, which is why the board's existing "tlmm" node cannot be reused for gpio28-31/16-19/20/21/124. */
#define DBY_BT_TLMM_TILE_BYTES 0x00100000ull   /* 0x1000 stride x 256 pins */

enum {
	DbyWinTlmmNorth = 0,
	DbyWinTlmmWest,
	DbyWinGcc,
	DbyWinSe0,
	DbyWinSe6,
	DbyWinCount
};

struct DbyBtWindow {
	uint64_t base;
	uint64_t bytes;
	const char *name;
};

/* Only the windows the current mode needs are ever mapped, and mapping touches no register; "not this
 * mode" and "failed to map" are different states because an unmapped window is refused by
 * dbyBtMapWindow() and the row prints "not-read" instead of a zero.
 *
 * Note for the reviewer: the WEST tile is the same 1 MiB the board's own `tlmm` node already covers
 * (dts/dby_w09.dts:65-72 maps 0xF100000+0x300000), so gpio145 is the one register in this probe that
 * another XNU driver also has mapped.  Both mappings are device mappings of the same registers and
 * this probe only loads, so no attribute conflict is introduced — but if the reviewer prefers no
 * overlap at all, dropping DbyBtM0PinBtIndep costs one corroborating pin and nothing else. */
static const struct DbyBtWindow kDbyBtWindows[DbyWinCount] = {
	[DbyWinTlmmNorth] = { DBY_BT_M0_TLMM_BASE + DBY_BT_M0_TLMM_NORTH, DBY_BT_TLMM_TILE_BYTES, "tlmm-north" },
	[DbyWinTlmmWest]  = { DBY_BT_M0_TLMM_BASE + DBY_BT_M0_TLMM_WEST,  DBY_BT_TLMM_TILE_BYTES, "tlmm-west"  },
	/* One mapping of the GCC device's own declared aperture ($VD/kona.dtsi:3056) with a bound check,
	 * rather than five single-purpose pages: fewer mappings, and the bound is the vendor's own size. */
	[DbyWinGcc]       = { DBY_BT_M0_GCC_BASE, DBY_BT_M0_GCC_BYTES, "gcc" },
	[DbyWinSe0]       = { DBY_BT_M0_SE0_BASE, DBY_BT_M0_SE_BYTES, "se0" },
	[DbyWinSe6]       = { DBY_BT_M0_SE6_BASE, DBY_BT_M0_SE_BYTES, "se6" },
};

struct DbyBtState {
	OSSharedPtr<IOMemoryMap> keep[DbyWinCount];
	uintptr_t base[DbyWinCount];
	unsigned mapped;
};

static DbyBtState gDbyBt;

static int
dbyBtMapWindow(unsigned window)
{
	IODeviceMemory *memory;
	OSSharedPtr<IOMemoryMap> map;

	if (window >= (unsigned)DbyWinCount || kDbyBtWindows[window].bytes == 0u) {
		return 0;
	}
	if ((gDbyBt.mapped & (1u << window)) != 0u) {
		return 1;
	}
	memory = IODeviceMemory::withRange(kDbyBtWindows[window].base, kDbyBtWindows[window].bytes);
	if (memory == nullptr) {
		return 0;
	}
	map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	if (!map) {
		return 0;
	}
	{
		/* A mapping that comes back at a null virtual address is not a window, and a load through it
		 * faults with no diagnostic on a board whose console is gone after IONVRAM (the same guard as
		 * work/tracks/wifi/repo/package/DbyWifi/DbyBt.cpp:220-227).  Checked before the map is kept,
		 * so a refused mapping simply goes out of scope. */
		const uintptr_t address = static_cast<uintptr_t>(map->getVirtualAddress());
		if (address == 0u) {
			return 0;
		}
		gDbyBt.keep[window] = map;
		gDbyBt.base[window] = address;
	}
	gDbyBt.mapped |= 1u << window;
	/* Mapping touches no hardware, which is why every window of a mode is mapped up front and the
	 * read gate is a separate decision (the same split as package/DbyGpuPowerProbe.cpp:5231). */
	return 1;
}

/* One load, bounded by the window it names.  No retry, no delay, no second attempt: a helper that
 * retries is a helper that can spin on a block that will never answer. */
static int
dbyBtRead32(unsigned window, uint64_t offset, uint32_t *out)
{
	if (out == nullptr || window >= (unsigned)DbyWinCount) {
		return 1;
	}
	if ((gDbyBt.mapped & (1u << window)) == 0u || gDbyBt.base[window] == 0u) {
		return 1;
	}
	if (offset + 4u > kDbyBtWindows[window].bytes) {
		return 1;
	}
	*out = *(volatile uint32_t *)(gDbyBt.base[window] + (uintptr_t)offset);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Publish                                                            */
/* ------------------------------------------------------------------ */

static void
dbyBtPublish(const struct dby_bt_m0_inputs *in, const struct dby_bt_m0_result *out)
{
	char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES];
	unsigned q;

	dby_bt_m0_render(in, out, rows);
	for (q = 0; q < (unsigned)DBY_BT_M0_ROWS; ++q) {
		memcpy(native_bt_rows[q], rows[q], DBY_BT_M0_ROW_BYTES);
	}
	/* The same evidence through IOLog, so a boot whose page rows were never rendered (no BootLog.c
	 * block yet) still leaves the answer in the kernel message buffer, which BootLog.c exports. */
	for (q = 0; q < (unsigned)DBY_BT_M0_ROWS; ++q) {
		if (rows[q][0] != 0) {
			IOLog("%s\n", rows[q]);
		}
	}
}

static void
dbyBtPublishNote(const char *text)
{
	unsigned q;

	for (q = 0; q < (unsigned)DBY_BT_M0_ROWS; ++q) {
		native_bt_rows[q][0] = 0;
	}
	{
		unsigned i = 0;
		while (text[i] != 0 && i + 1u < (unsigned)DBY_BT_M0_ROW_BYTES) {
			native_bt_rows[0][i] = text[i];
			++i;
		}
		native_bt_rows[0][i] = 0;
	}
	IOLog("%s\n", native_bt_rows[0]);
}

/* ------------------------------------------------------------------ */
/* Census                                                             */
/* ------------------------------------------------------------------ */

static void
dbyBtReadPins(struct dby_bt_m0_inputs *in)
{
	unsigned p;

	for (p = 0; p < (unsigned)DbyBtM0PinCount; ++p) {
		const struct dby_bt_m0_pin_fact *fact = &dby_bt_m0_pins[p];
		const unsigned window = (fact->tile == DBY_BT_M0_TLMM_WEST) ?
		    (unsigned)DbyWinTlmmWest : (unsigned)DbyWinTlmmNorth;
		const uint64_t offset = (uint64_t)fact->id * DBY_BT_M0_TLMM_STRIDE;
		uint32_t ctl = 0, io = 0;

		/* Both words or neither: a row that shows a config word but no level would invite reading
		 * an unread level as 0. */
		if (dbyBtRead32(window, offset, &ctl) != 0 ||
		    dbyBtRead32(window, offset + DBY_BT_M0_TLMM_IO_OFF, &io) != 0) {
			continue;
		}
		in->pin[p].ctl = ctl;
		in->pin[p].io = io;
		in->pin[p].valid = 1;
	}
}

static void
dbyBtReadGcc(struct dby_bt_m0_inputs *in)
{
	static const struct {
		uint64_t offset;
		uint32_t bit;
	} words[] = {
		{ DBY_BT_M0_GCC_VOTE_REG,   DBY_BT_M0_GCC_VALID_VOTE },
		{ DBY_BT_M0_GCC_CORE_HALT,  DBY_BT_M0_GCC_VALID_CORE },
		{ DBY_BT_M0_GCC_M_AHB_HALT, DBY_BT_M0_GCC_VALID_M_AHB },
		{ DBY_BT_M0_GCC_S_AHB_HALT, DBY_BT_M0_GCC_VALID_S_AHB },
		{ DBY_BT_M0_GCC_SE0_HALT,   DBY_BT_M0_GCC_VALID_SE0 },
		{ DBY_BT_M0_GCC_SE6_HALT,   DBY_BT_M0_GCC_VALID_SE6 },
	};
	uint32_t value[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
	unsigned i;

	for (i = 0; i < 6u; ++i) {
		if (dbyBtRead32((unsigned)DbyWinGcc, words[i].offset, &value[i]) == 0) {
			in->gcc_valid |= words[i].bit;
		}
	}
	in->gcc_vote      = value[0];
	in->gcc_core_halt = value[1];
	in->gcc_m_ahb_halt = value[2];
	in->gcc_s_ahb_halt = value[3];
	in->gcc_se0_halt  = value[4];
	in->gcc_se6_halt  = value[5];
	/* gcc_mapped stays what the entry point set it to ("the window is mapped"); WHICH of the six words
	 * answered is gcc_valid, and row3 prints "not-read" for the rest.  Conflating the two would make
	 * row0's gcc= field mean different things depending on which read failed. */
}

/* Mode 2.  Reads only registers the vendor names _RO, and only when the engine is clocked.  The
 * FIFO data registers (0x700/0x780) and the IRQ status/clear registers (0x610/0x618/0x640/0x648)
 * are deliberately NOT read: a FIFO read pops a byte and an IRQ status read can drop an event, so
 * neither is a "read-only" access in the sense this probe promises. */
static void
dbyBtReadGeni(struct dby_bt_m0_inputs *in, const struct dby_bt_m0_result *out)
{
	static const uint64_t offsets[DBY_BT_M0_GENI_WORDS] = {
		DBY_BT_M0_GENI_INIT_CFG_REVISION,
		DBY_BT_M0_GENI_STATUS,
		DBY_BT_M0_GENI_SER_M_CLK_CFG,
		DBY_BT_M0_GENI_SER_S_CLK_CFG,
		DBY_BT_M0_GENI_FW_REVISION_RO,
		DBY_BT_M0_GENI_FW_S_REVISION_RO,
		DBY_BT_M0_GENI_IF_FIFO_DISABLE,
		DBY_BT_M0_GENI_IOS,
	};
	unsigned i;

	in->geni_attempted = 1;

	if (out->se0_clocked != 0u && dbyBtMapWindow((unsigned)DbyWinSe0) != 0) {
		unsigned got = 0;
		for (i = 0; i < (unsigned)DBY_BT_M0_GENI_WORDS; ++i) {
			if (dbyBtRead32((unsigned)DbyWinSe0, offsets[i], &in->geni_se0[i]) == 0) {
				++got;
			}
		}
		if (got == (unsigned)DBY_BT_M0_GENI_WORDS) {
			in->geni_read_mask |= 1u;
		}
	}
	if (out->se6_clocked != 0u && dbyBtMapWindow((unsigned)DbyWinSe6) != 0) {
		unsigned got = 0;
		for (i = 0; i < (unsigned)DBY_BT_M0_GENI_WORDS; ++i) {
			if (dbyBtRead32((unsigned)DbyWinSe6, offsets[i], &in->geni_se6[i]) == 0) {
				++got;
			}
		}
		if (got == (unsigned)DBY_BT_M0_GENI_WORDS) {
			in->geni_read_mask |= 2u;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

extern "C" int
dby_wifi_probe_entry(void)
{
	struct dby_bt_m0_inputs in;
	struct dby_bt_m0_result out;
	unsigned mode = 0;
	unsigned windows;

	/* Gate 1: somewhere to put the answer.  Absent symbol or no successful round yet -> return and
	 * stay at stage 0 so a later call retries.  This is package/DbyGpuPowerProbe.cpp:5172-5174's gate,
	 * copied because its reasoning applies unchanged: a probe whose result cannot be stored is pure
	 * risk. */
	if (!&boot_log_slots_ok || boot_log_slots_ok == 0u) {
		return 0;
	}
	/* Gate 2: one shot.  package/BootProgress.c's monitor calls late probes every second. */
	if (native_bt_stage != 0u) {
		return 0;
	}
	native_bt_stage = 1u;

	memset(&in, 0, sizeof in);
	memset(&out, 0, sizeof out);

	if (!PE_parse_boot_argn("dby-bt-m0", &mode, sizeof mode) || mode < 1u || mode > 2u) {
		dbyBtPublishNote("DbyBt: M0 skipped: pass dby-bt-m0=1 (TLMM+GCC) or 2 (+GENI)");
		native_bt_stage = 3u;
		return 0;
	}
	in.mode = mode;

	/* Windows per mode.  Mapping is not a hardware access; the reads below are gated separately. */
	windows = (1u << DbyWinTlmmNorth) | (1u << DbyWinTlmmWest) | (1u << DbyWinGcc);
	if (mode >= 2u) {
		windows |= (1u << DbyWinSe0) | (1u << DbyWinSe6);
	}
	for (unsigned w = 0; w < (unsigned)DbyWinCount; ++w) {
		if ((windows & (1u << w)) != 0u) {
			(void)dbyBtMapWindow(w);
		}
	}
	in.tlmm_mapped = ((gDbyBt.mapped & ((1u << DbyWinTlmmNorth) | (1u << DbyWinTlmmWest))) != 0u) ? 1u : 0u;
	in.gcc_mapped = ((gDbyBt.mapped & (1u << DbyWinGcc)) != 0u) ? 1u : 0u;

	if (in.tlmm_mapped == 0u || in.gcc_mapped == 0u) {
		/* No read of either block happened, so there is nothing to attribute a hang to; say which
		 * mapping failed instead of printing a row of zeros. */
		if (in.tlmm_mapped == 0u && in.gcc_mapped == 0u) {
			dbyBtPublishNote("DbyBt: M0 nomap: TLMM and GCC both refused; nothing was read");
		} else if (in.tlmm_mapped == 0u) {
			dbyBtPublishNote("DbyBt: M0 nomap: TLMM refused; nothing was read");
		} else {
			dbyBtPublishNote("DbyBt: M0 nomap: GCC refused; nothing was read");
		}
		native_bt_stage = 4u;
		return 0;
	}

	/* Progress marker BEFORE the first load.  If a load freezes the machine, the last evidence in the
	 * log is this row, which names the probe and the phase - instead of a log that simply stops. */
	dbyBtPublishNote("DbyBt: M0 running: TLMM+GCC mapped, first load next");

	dbyBtReadPins(&in);
	dbyBtReadGcc(&in);
	dby_bt_m0_evaluate(&in, &out);

	if (mode >= 2u) {
		dbyBtReadGeni(&in, &out);
		dby_bt_m0_evaluate(&in, &out);   /* idempotent; picks up geni_read_mask/geni_answer */
	}

	dbyBtPublish(&in, &out);
	native_bt_stage = 2u;
	return 0;
}
