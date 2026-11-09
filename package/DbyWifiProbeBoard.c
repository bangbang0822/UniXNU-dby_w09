/* DbyWifiProbeBoard.c — the IOKit-free half of package/DbyWifiProbe.cpp (SPEC M0, Bluetooth bus).
 *
 * WHY IT IS SEPARATE
 *   Everything here is a pure function of words that were already read: decode a GPIO_CFG word,
 *   decide whether a GCC branch is running, fill the derived fields, pick the verdict, render six
 *   rows.  Keeping that out of the IOKit translation unit is what lets scripts/build-wifi-probe.py
 *   compile this file with a plain host compiler, run DBY_BT_M0_SELFTEST's cases, and print the
 *   rows that a given set of register values would produce - on a development machine, without a
 *   device and without an SDK.  The same split, for the same reason, as the previous Wi-Fi round's
 *   package/DbyWifi/DbyWifiCensus.c.
 *
 * NO LIBC.  No snprintf, no string.h: the kernel link of this board carries no formatted output that
 * a prelinked board TU may reach (package/DbyGpuPowerProbe.cpp:351 says the same and hand-rolls its
 * helpers).  The self-test links a 12-line main() and nothing else.
 *
 * READ-ONLY, STRUCTURALLY.  This file performs no I/O of any kind, and the file that does (the .cpp)
 * has no store helper at all.  There is nothing here that could write a register.
 */

#include "DbyWifiProbeBoard.h"

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

const struct dby_bt_m0_pin_fact dby_bt_m0_pins[DbyBtM0PinCount] = {
	[DbyBtM0PinSe6Cts]    = { 16,  DBY_BT_M0_TLMM_NORTH, "se6_cts"   },
	[DbyBtM0PinSe6Rts]    = { 17,  DBY_BT_M0_TLMM_NORTH, "se6_rts"   },
	[DbyBtM0PinSe6Tx]     = { 18,  DBY_BT_M0_TLMM_NORTH, "se6_tx"    },
	[DbyBtM0PinSe6Rx]     = { 19,  DBY_BT_M0_TLMM_NORTH, "se6_rx"    },
	[DbyBtM0PinWlanEn]    = { 20,  DBY_BT_M0_TLMM_NORTH, "wlan_en"   },
	[DbyBtM0PinBtEn]      = { 21,  DBY_BT_M0_TLMM_NORTH, "bt_en"     },
	[DbyBtM0PinSe0Cts]    = { 28,  DBY_BT_M0_TLMM_NORTH, "se0_cts"   },
	[DbyBtM0PinSe0Rts]    = { 29,  DBY_BT_M0_TLMM_NORTH, "se0_rts"   },
	[DbyBtM0PinSe0Tx]     = { 30,  DBY_BT_M0_TLMM_NORTH, "se0_tx"    },
	[DbyBtM0PinSe0Rx]     = { 31,  DBY_BT_M0_TLMM_NORTH, "se0_rx"    },
	[DbyBtM0PinSwCtrl]    = { 124, DBY_BT_M0_TLMM_NORTH, "sw_ctrl"   },
	[DbyBtM0PinBtIndep]   = { 145, DBY_BT_M0_TLMM_WEST,  "bt_indep"  },
};

/* ------------------------------------------------------------------ */
/* Decode                                                             */
/* ------------------------------------------------------------------ */

struct dby_bt_m0_ctl
dby_bt_m0_decode_ctl(uint32_t ctl)
{
	struct dby_bt_m0_ctl out;

	/* Exactly the vendor's own decode (pinctrl-msm.c:560-563): the 3-bit func mask is the driver's,
	 * so the probe reports what the driver would report, not a wider field it never reads back. */
	out.func      = (ctl >> DBY_BT_M0_CTL_FUNC_SHIFT) & DBY_BT_M0_CTL_FUNC_MASK;
	out.drive     = (ctl >> DBY_BT_M0_CTL_DRIVE_SHIFT) & DBY_BT_M0_CTL_DRIVE_MASK;
	out.pull      = ctl & DBY_BT_M0_CTL_PULL_MASK;
	out.is_output = (ctl >> DBY_BT_M0_CTL_OE_BIT) & 1u;
	return out;
}

uint32_t
dby_bt_m0_branch_running(uint32_t vote, uint32_t halt, uint32_t mask)
{
	/* Both halves are required.  The vote alone says software asked; the CBCR's CLK_OFF
	 * (clk-branch.c:47, :64-67) says the branch is actually out of halt.  Treating a set vote as
	 * "running" is how a probe talks itself into reading an unclocked block. */
	if ((vote & mask) == 0u) {
		return 0u;
	}
	if ((halt & DBY_BT_M0_CBCR_CLK_OFF) != 0u) {
		return 0u;
	}
	return 1u;
}

const char *
dby_bt_m0_verdict_name(enum dby_bt_m0_verdict verdict)
{
	/* The SPEC's own words; the row is read against work/tracks/wifi-bt/SPEC.md 3.1. */
	switch (verdict) {
	case DbyBtM0VerdictUartSe0: return "UART-SE0";
	case DbyBtM0VerdictUartSe6: return "UART-SE6";
	case DbyBtM0VerdictSlimbus: return "SLIMBUS";
	default:                    return "UNKNOWN";
	}
}

static uint32_t
dby_bt_m0_pin_valid(const struct dby_bt_m0_inputs *in, unsigned id)
{
	return in->pin[id].valid != 0u;
}

static uint32_t
dby_bt_m0_pin_func(const struct dby_bt_m0_inputs *in, unsigned id)
{
	return dby_bt_m0_decode_ctl(in->pin[id].ctl).func;
}

static uint32_t
dby_bt_m0_pin_level(const struct dby_bt_m0_inputs *in, unsigned id)
{
	return (in->pin[id].io >> DBY_BT_M0_IO_IN_BIT) & 1u;
}

/* ------------------------------------------------------------------ */
/* Evaluate                                                           */
/* ------------------------------------------------------------------ */

/* Count the four pins of one 4-wire UART that sit on their QUP function.  A pin counts only when it
 * was actually read: an unmapped window must not look like "function 0", which would be the same
 * fabrication the row renderer refuses ("not-read" is printed instead of a zero). */
static void
dby_bt_m0_count_qup(const struct dby_bt_m0_inputs *in, unsigned first, uint32_t *qup,
    uint32_t *all_valid, uint32_t *all_gpio)
{
	unsigned i;

	*qup = 0u;
	*all_valid = 1u;
	*all_gpio = 1u;
	for (i = 0; i < 4u; ++i) {
		const unsigned id = first + i;
		uint32_t func;

		if (!dby_bt_m0_pin_valid(in, id)) {
			*all_valid = 0u;
			*all_gpio = 0u;
			continue;
		}
		func = dby_bt_m0_pin_func(in, id);
		if (func == DBY_BT_M0_FUNC_QUP) {
			*qup += 1u;
			*all_gpio = 0u;
		} else if (func != DBY_BT_M0_FUNC_GPIO) {
			*all_gpio = 0u;      /* an alternate function that is neither: not a "detached" pin */
		}
	}
}

void
dby_bt_m0_evaluate(const struct dby_bt_m0_inputs *in, struct dby_bt_m0_result *out)
{
	uint32_t se0_all_valid = 0u, se0_all_gpio = 0u;
	uint32_t se6_all_valid = 0u, se6_all_gpio = 0u;
	uint32_t core, m_ahb, s_ahb;

	/* Zero everything first: a result that keeps a previous run's field is worse than a blank. */
	{
		uint8_t *raw = (uint8_t *)out;
		uint32_t i;
		for (i = 0; i < (uint32_t)sizeof(*out); ++i) {
			raw[i] = 0u;
		}
	}
	out->verdict = DbyBtM0VerdictUnknown;
	out->why = "not-run";

	if (in == (const struct dby_bt_m0_inputs *)0) {
		return;
	}

	dby_bt_m0_count_qup(in, (unsigned)DbyBtM0PinSe0Cts, &out->se0_qup_pins,
	    &se0_all_valid, &se0_all_gpio);
	dby_bt_m0_count_qup(in, (unsigned)DbyBtM0PinSe6Cts, &out->se6_qup_pins,
	    &se6_all_valid, &se6_all_gpio);

	out->bt_en_level     = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinBtEn);
	out->wlan_en_level   = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinWlanEn);
	out->sw_ctrl_level   = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinSwCtrl);
	out->bt_indep_level  = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinBtIndep);
	out->se0_rx_level    = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinSe0Rx);
	out->se0_tx_level    = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinSe0Tx);
	out->se6_rx_level    = dby_bt_m0_pin_level(in, (unsigned)DbyBtM0PinSe6Rx);

	core  = dby_bt_m0_branch_running(in->gcc_vote, in->gcc_core_halt,  DBY_BT_M0_GCC_VOTE_CORE);
	m_ahb = dby_bt_m0_branch_running(in->gcc_vote, in->gcc_m_ahb_halt, DBY_BT_M0_GCC_VOTE_M_AHB);
	s_ahb = dby_bt_m0_branch_running(in->gcc_vote, in->gcc_s_ahb_halt, DBY_BT_M0_GCC_VOTE_S_AHB);
	out->qup_domain_on = (core && m_ahb && s_ahb) ? 1u : 0u;

	/* An SE register block is only readable when its wrapper (core + AHB) is up AND its own branch is
	 * out of halt.  Requiring all four is stricter than requiring the SE branch alone, and stricter is
	 * the only defensible direction on a block whose read fault freezes the SoC. */
	out->se0_clocked = (out->qup_domain_on != 0u) ?
	    dby_bt_m0_branch_running(in->gcc_vote, in->gcc_se0_halt, DBY_BT_M0_GCC_VOTE_SE0) : 0u;
	out->se6_clocked = (out->qup_domain_on != 0u) ?
	    dby_bt_m0_branch_running(in->gcc_vote, in->gcc_se6_halt, DBY_BT_M0_GCC_VOTE_SE6) : 0u;

	/* Did an engine answer at all?  Two canonical "no answer" words, and an all-zero page, which a
	 * refused window and an unclocked block both produce (DbyWifiCensus.c:152-160 makes the same
	 * point for the PCIe endpoint). */
	out->geni_answer = 0u;
	if ((in->geni_read_mask & 1u) != 0u) {
		const uint32_t init = in->geni_se0[DbyBtM0GeniInitCfg];
		if (init != 0u && init != 0xFFFFFFFFu) {
			out->geni_answer = 1u;
		}
	}
	if ((in->geni_read_mask & 2u) != 0u) {
		const uint32_t init = in->geni_se6[DbyBtM0GeniInitCfg];
		if (init != 0u && init != 0xFFFFFFFFu) {
			out->geni_answer = 1u;
		}
	}

	/* ---- the verdict ladder, pre-registered; the ORDER is the argument ---------------------------
	 *
	 * 1. rx-driven.  BT_EN is high (the chip is powered) AND the SoC's SE0 RX pin reads high AND all
	 *    four SE0 pins are plain GPIO, i.e. the serial engine is detached and cannot be the thing
	 *    driving that line.  gpio31's sleep-state bias is pull-down ($VD/kona-pinctrl.dtsi:238-242),
	 *    so an undriven line reads 0; a 1 there is a driver - and the only other device on that net
	 *    is the Bluetooth chip's UART TX.  Caveat, stated in the row and in M0-ANSWER.md: a pad
	 *    keeper can also hold a line high, so this verdict is only conclusive together with the
	 *    0 -> 1 transition that M9's BT_EN raise produces.
	 *
	 * 2. se0-mux: all four SE0 pins sit on function index 1, which pinctrl-kona.c:1474-1479 makes
	 *    qup0.  Somebody ran the pinctrl state machine for a UART on this engine and TLMM has not
	 *    been reset since.  Index 1 is qup0 for THIS pin group only because the group's funcs[] puts
	 *    gpio first; the same index means qup6 on gpio16-19, which is why the comparison is per
	 *    group and never against a global enum.
	 *
	 * 3. se6-mux: the same for the other candidate.  It ranks below SE0 only because the board's own
	 *    base (kona-qrd) leaves SE0 "ok" in the SoC dtsi ($VD/kona-qupv3.dtsi:124) and SE6 "disabled"
	 *    (:102) until kona-qrd.dtsi:30-36 turns it on; the two are otherwise the same shape, which is
	 *    the point of M0-ANSWER.md's "the device tree cannot separate them".
	 *
	 * 4/5. the clock vote.  Weaker than a pin mux: a branch can be left enabled by a driver that
	 *    suspended badly, so it only decides when exactly one engine is clocked.
	 *
	 * 6. both clocked -> no verdict.  Two engines clocked says nothing about which carries HCI.
	 *
	 * Nothing in this ladder can produce SLIMBUS.  Only the powered measurement can. */
	if (in->gcc_valid == DBY_BT_M0_GCC_VALID_ALL && in->tlmm_mapped != 0u) {
		if (se0_all_valid != 0u && se0_all_gpio != 0u && out->bt_en_level == 1u &&
		    out->se0_rx_level == 1u) {
			out->verdict = DbyBtM0VerdictUartSe0;
			out->why = "rx-driven";
			return;
		}
		if (out->se0_qup_pins == 4u) {
			out->verdict = DbyBtM0VerdictUartSe0;
			out->why = "se0-mux";
			return;
		}
		if (out->se6_qup_pins == 4u) {
			out->verdict = DbyBtM0VerdictUartSe6;
			out->why = "se6-mux";
			return;
		}
		if (out->se0_clocked != 0u && out->se6_clocked == 0u) {
			out->verdict = DbyBtM0VerdictUartSe0;
			out->why = "se0-clk";
			return;
		}
		if (out->se6_clocked != 0u && out->se0_clocked == 0u) {
			out->verdict = DbyBtM0VerdictUartSe6;
			out->why = "se6-clk";
			return;
		}
		if (out->se0_clocked != 0u && out->se6_clocked != 0u) {
			out->why = "both-clocked";
			return;
		}
		out->why = "no-static-evidence";
		return;
	}
	if (in->tlmm_mapped == 0u) {
		out->why = "no-tlmm";
		return;
	}
	out->why = "no-gcc";
	(void)se6_all_valid;
	(void)se6_all_gpio;
}

/* ------------------------------------------------------------------ */
/* Rows                                                               */
/* ------------------------------------------------------------------ */

static char *
dbyPutStr(char *at, const char *end, const char *text)
{
	while (*text != 0 && at < end) {
		*at++ = *text++;
	}
	return at;
}

static char *
dbyPutHex8(char *at, const char *end, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	for (shift = 28; shift >= 0 && at < end; shift -= 4) {
		*at++ = digits[(value >> (unsigned)shift) & 0xFu];
	}
	return at;
}

static char *
dbyPutDec(char *at, const char *end, uint32_t value)
{
	char tmp[11];
	unsigned n = 0;

	if (value == 0u) {
		if (at < end) {
			*at++ = '0';
		}
		return at;
	}
	while (value != 0u && n < sizeof tmp) {
		tmp[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	while (n != 0u && at < end) {
		*at++ = tmp[--n];
	}
	return at;
}

static void
dbyFinishRow(char *at)
{
	*at = 0;
}

/* One level cell: "label=<0|1>" when the pin was read, "label=-" when it was not. */
static char *
dbyPutLevel(char *at, const char *end, const char *label, const struct dby_bt_m0_inputs *in,
    unsigned id)
{
	at = dbyPutStr(at, end, label);
	if (dby_bt_m0_pin_valid(in, id) == 0u) {
		return dbyPutStr(at, end, "-");
	}
	return dbyPutDec(at, end, dby_bt_m0_pin_level(in, id));
}

/* Four "NN=word" cells of one engine, then the four function indices. */
static char *
dbyPutEngine(char *at, const char *end, const char *label, const struct dby_bt_m0_inputs *in,
    unsigned first)
{
	unsigned i;

	at = dbyPutStr(at, end, "DbyBt: M0 ");
	at = dbyPutStr(at, end, label);
	at = dbyPutStr(at, end, " ");
	for (i = 0; i < 4u; ++i) {
		const unsigned id = first + i;
		const struct dby_bt_m0_pin_fact *fact = &dby_bt_m0_pins[id];

		at = dbyPutDec(at, end, fact->id);
		at = dbyPutStr(at, end, "=");
		if (in->pin[id].valid == 0u) {
			at = dbyPutStr(at, end, "not-read ");
		} else {
			at = dbyPutHex8(at, end, in->pin[id].ctl);
			at = dbyPutStr(at, end, " ");
		}
	}
	at = dbyPutStr(at, end, "f=");
	for (i = 0; i < 4u; ++i) {
		const unsigned id = first + i;
		if (in->pin[id].valid == 0u) {
			at = dbyPutStr(at, end, "-");
		} else {
			at = dbyPutDec(at, end, dby_bt_m0_pin_func(in, id));
		}
	}
	return at;
}

void
dby_bt_m0_render(const struct dby_bt_m0_inputs *in, const struct dby_bt_m0_result *out,
    char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES])
{
	const char *end;
	char *at;
	unsigned q;

	/* A blank row is "this slot was not used", which package/BootLog.c already understands: it
	 * renders a row only when its first byte is non-zero (BootLog.c:667-671). */
	for (q = 0; q < (unsigned)DBY_BT_M0_ROWS; ++q) {
		rows[q][0] = 0;
	}

	/* row0 — the verdict, and nothing that could be mistaken for a measurement. */
	at = rows[0];
	end = rows[0] + (DBY_BT_M0_ROW_BYTES - 1);
	at = dbyPutStr(at, end, "DbyBt: M0 m=");
	at = dbyPutDec(at, end, in->mode);
	at = dbyPutStr(at, end, " tlmm=");
	at = dbyPutDec(at, end, in->tlmm_mapped != 0u ? 1u : 0u);
	at = dbyPutStr(at, end, " gcc=");
	at = dbyPutDec(at, end, in->gcc_mapped != 0u ? 1u : 0u);
	at = dbyPutStr(at, end, " geni=");
	at = dbyPutDec(at, end, in->geni_attempted != 0u ? 1u : 0u);
	at = dbyPutStr(at, end, " verdict=");
	at = dbyPutStr(at, end, dby_bt_m0_verdict_name(out->verdict));
	at = dbyPutStr(at, end, " why=");
	at = dbyPutStr(at, end, out->why != (const char *)0 ? out->why : "?");
	dbyFinishRow(at);

	/* row1, row2 — the two 4-wire UART candidates, raw config words and their function indices. */
	at = dbyPutEngine(rows[1], rows[1] + (DBY_BT_M0_ROW_BYTES - 1), "se0",
	    in, (unsigned)DbyBtM0PinSe0Cts);
	dbyFinishRow(at);

	at = dbyPutEngine(rows[2], rows[2] + (DBY_BT_M0_ROW_BYTES - 1), "se6",
	    in, (unsigned)DbyBtM0PinSe6Cts);
	dbyFinishRow(at);

	/* row3 — GCC: one vote word and the five branches it governs. */
	at = rows[3];
	end = rows[3] + (DBY_BT_M0_ROW_BYTES - 1);
	at = dbyPutStr(at, end, "DbyBt: M0 gcc vote=");
	if ((in->gcc_valid & DBY_BT_M0_GCC_VALID_VOTE) != 0u) { at = dbyPutHex8(at, end, in->gcc_vote); } else { at = dbyPutStr(at, end, "not-read"); }
	at = dbyPutStr(at, end, " core=");
	if ((in->gcc_valid & DBY_BT_M0_GCC_VALID_CORE) != 0u) { at = dbyPutHex8(at, end, in->gcc_core_halt); } else { at = dbyPutStr(at, end, "na"); }
	at = dbyPutStr(at, end, " se0=");
	if ((in->gcc_valid & DBY_BT_M0_GCC_VALID_SE0) != 0u) { at = dbyPutHex8(at, end, in->gcc_se0_halt); } else { at = dbyPutStr(at, end, "na"); }
	at = dbyPutStr(at, end, " se6=");
	if ((in->gcc_valid & DBY_BT_M0_GCC_VALID_SE6) != 0u) { at = dbyPutHex8(at, end, in->gcc_se6_halt); } else { at = dbyPutStr(at, end, "na"); }
	dbyFinishRow(at);

	/* row4 — the control pins and the two UART lines whose level separates the buses.  Levels are
	 * GPIO_IN_OUT bit 0 (pinctrl-kona.c:55), i.e. what the line is, not what a latch holds.
	 * A pin that was not read prints "-": the whole point of this row is to be chased to a scope, and
	 * a 0 that was never measured would send someone hunting a shorted line. */
	at = rows[4];
	end = rows[4] + (DBY_BT_M0_ROW_BYTES - 1);
	at = dbyPutLevel(at, end, "DbyBt: M0 pin bt_en=", in, (unsigned)DbyBtM0PinBtEn);
	at = dbyPutLevel(at, end, " wlan=", in, (unsigned)DbyBtM0PinWlanEn);
	at = dbyPutLevel(at, end, " sw=", in, (unsigned)DbyBtM0PinSwCtrl);
	at = dbyPutLevel(at, end, " ind=", in, (unsigned)DbyBtM0PinBtIndep);
	at = dbyPutLevel(at, end, " rx31=", in, (unsigned)DbyBtM0PinSe0Rx);
	at = dbyPutLevel(at, end, " tx30=", in, (unsigned)DbyBtM0PinSe0Tx);
	dbyFinishRow(at);

	/* row5 — the clock/engine summary and, in mode 2, the SE identity words.  "answered" says an
	 * engine's register block replied; it is a different question from which engine carries HCI, and
	 * the row never claims more than that. */
	at = rows[5];
	end = rows[5] + (DBY_BT_M0_ROW_BYTES - 1);
	at = dbyPutStr(at, end, "DbyBt: M0 qup=");
	if (in->gcc_valid != DBY_BT_M0_GCC_VALID_ALL) {
		/* Without all six words the derived clock fields were never computed, and printing the zeros
		 * the result struct was cleared with would be a fabricated measurement. */
		at = dbyPutStr(at, end, "? se0clk=? se6clk=?");
	} else {
		at = dbyPutStr(at, end, out->qup_domain_on != 0u ? "on" : "off");
		at = dbyPutStr(at, end, " se0clk=");
		at = dbyPutDec(at, end, out->se0_clocked);
		at = dbyPutStr(at, end, " se6clk=");
		at = dbyPutDec(at, end, out->se6_clocked);
	}
	at = dbyPutStr(at, end, " geni=");
	if (in->geni_read_mask == 0u) {
		at = dbyPutStr(at, end, "not-read");
	} else if (out->geni_answer != 0u) {
		at = dbyPutStr(at, end, "answered");
	} else {
		at = dbyPutStr(at, end, "silent");
	}
	if ((in->geni_read_mask & 1u) != 0u) {
		at = dbyPutStr(at, end, " init=");
		at = dbyPutHex8(at, end, in->geni_se0[DbyBtM0GeniInitCfg]);
		at = dbyPutStr(at, end, " fw=");
		at = dbyPutHex8(at, end, in->geni_se0[DbyBtM0GeniFwRevision]);
		at = dbyPutStr(at, end, " ios=");
		at = dbyPutHex8(at, end, in->geni_se0[DbyBtM0GeniIos]);
	}
	dbyFinishRow(at);
}

/* ------------------------------------------------------------------ */
/* Self-test (host only)                                              */
/* ------------------------------------------------------------------ */
#ifdef DBY_BT_M0_SELFTEST
#include <stdio.h>
#include <string.h>

static int failures;

static void
expect(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL %s: got %s want %s\n", what, got, want);
		failures++;
	} else {
		printf("ok   %s = %s\n", what, got);
	}
}

static void
set_pin(struct dby_bt_m0_inputs *in, unsigned id, uint32_t func, uint32_t level,
    uint32_t is_output)
{
	in->pin[id].ctl = (func << DBY_BT_M0_CTL_FUNC_SHIFT) |
	    (is_output != 0u ? (1u << DBY_BT_M0_CTL_OE_BIT) : 0u);
	in->pin[id].io = level & 1u;
	in->pin[id].valid = 1u;
}

static void
set_qup_pins(struct dby_bt_m0_inputs *in, unsigned first, uint32_t func)
{
	unsigned i;

	for (i = 0; i < 4u; ++i) {
		set_pin(in, first + i, func, 0u, 0u);
	}
}

static void
gcc_all(struct dby_bt_m0_inputs *in, uint32_t vote, uint32_t se0_halt, uint32_t se6_halt)
{
	in->gcc_mapped = 1u;
	in->gcc_valid = DBY_BT_M0_GCC_VALID_ALL;
	in->gcc_vote = vote;
	in->gcc_core_halt = 0u;
	in->gcc_m_ahb_halt = 0u;
	in->gcc_s_ahb_halt = 0u;
	in->gcc_se0_halt = se0_halt;
	in->gcc_se6_halt = se6_halt;
}

static void
test_case(const char *name, const struct dby_bt_m0_inputs *in, const char *want_verdict,
    const char *want_why)
{
	struct dby_bt_m0_result out;
	char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES];

	dby_bt_m0_evaluate(in, &out);
	expect(name, dby_bt_m0_verdict_name(out.verdict), want_verdict);
	expect("  why", out.why, want_why);
	dby_bt_m0_render(in, &out, rows);
	printf("     | %s\n", rows[0]);
}

int
main(void)
{
	struct dby_bt_m0_inputs in;
	struct dby_bt_m0_result out;

	/* The pins are all plain GPIO and undriven, the QUPv3 domain is off: the cold state. */
	memset(&in, 0, sizeof in);
	in.mode = 1u;
	in.tlmm_mapped = 1u;
	set_qup_pins(&in, (unsigned)DbyBtM0PinSe0Cts, DBY_BT_M0_FUNC_GPIO);
	set_qup_pins(&in, (unsigned)DbyBtM0PinSe6Cts, DBY_BT_M0_FUNC_GPIO);
	set_pin(&in, (unsigned)DbyBtM0PinWlanEn, DBY_BT_M0_FUNC_GPIO, 0u, 1u);
	set_pin(&in, (unsigned)DbyBtM0PinBtEn, DBY_BT_M0_FUNC_GPIO, 0u, 1u);
	set_pin(&in, (unsigned)DbyBtM0PinSwCtrl, DBY_BT_M0_FUNC_GPIO, 0u, 0u);
	set_pin(&in, (unsigned)DbyBtM0PinBtIndep, DBY_BT_M0_FUNC_GPIO, 0u, 0u);
	gcc_all(&in, 0u, DBY_BT_M0_CBCR_CLK_OFF, DBY_BT_M0_CBCR_CLK_OFF);

	/* 1. Nothing measured yet. */
	test_case("cold", &in, "UNKNOWN", "no-static-evidence");

	/* 2. A previous Linux boot left SE0 on qup0.  The strongest static evidence there is. */
	{
		struct dby_bt_m0_inputs se0 = in;
		set_qup_pins(&se0, (unsigned)DbyBtM0PinSe0Cts, DBY_BT_M0_FUNC_QUP);
		test_case("se0-mux", &se0, "UART-SE0", "se0-mux");
	}

	/* 3. The same for SE6, with SE0 untouched. */
	{
		struct dby_bt_m0_inputs se6 = in;
		set_qup_pins(&se6, (unsigned)DbyBtM0PinSe6Cts, DBY_BT_M0_FUNC_QUP);
		test_case("se6-mux", &se6, "UART-SE6", "se6-mux");
	}

	/* 4. SE0's branch is the only one voted and out of halt. */
	{
		struct dby_bt_m0_inputs clk = in;
		gcc_all(&clk, DBY_BT_M0_GCC_VOTE_CORE | DBY_BT_M0_GCC_VOTE_M_AHB |
		    DBY_BT_M0_GCC_VOTE_S_AHB | DBY_BT_M0_GCC_VOTE_SE0, 0u, DBY_BT_M0_CBCR_CLK_OFF);
		test_case("se0-clk", &clk, "UART-SE0", "se0-clk");
	}

	/* 5. Both clocked: the vote no longer decides anything. */
	{
		struct dby_bt_m0_inputs both = in;
		gcc_all(&both, DBY_BT_M0_GCC_VOTE_CORE | DBY_BT_M0_GCC_VOTE_M_AHB |
		    DBY_BT_M0_GCC_VOTE_S_AHB | DBY_BT_M0_GCC_VOTE_SE0 | DBY_BT_M0_GCC_VOTE_SE6,
		    0u, 0u);
		test_case("both-clocked", &both, "UNKNOWN", "both-clocked");
	}

	/* 6. A vote without the branch: enabled-but-halted is NOT running (the 0x80004221 trap the GPU
	 * probe documents at DbyGpuPowerProbe.cpp:37-39). */
	{
		struct dby_bt_m0_inputs halted = in;
		gcc_all(&halted, DBY_BT_M0_GCC_VOTE_CORE | DBY_BT_M0_GCC_VOTE_M_AHB |
		    DBY_BT_M0_GCC_VOTE_S_AHB | DBY_BT_M0_GCC_VOTE_SE0,
		    DBY_BT_M0_CBCR_CLK_OFF, DBY_BT_M0_CBCR_CLK_OFF);
		test_case("se0-voted-but-halted", &halted, "UNKNOWN", "no-static-evidence");
	}

	/* 7. rx-driven: BT_EN high, SE0 detached from the engine, and its RX line high. */
	{
		struct dby_bt_m0_inputs rx = in;
		set_pin(&rx, (unsigned)DbyBtM0PinBtEn, DBY_BT_M0_FUNC_GPIO, 1u, 1u);
		set_pin(&rx, (unsigned)DbyBtM0PinSe0Rx, DBY_BT_M0_FUNC_GPIO, 1u, 0u);
		test_case("rx-driven", &rx, "UART-SE0", "rx-driven");
	}

	/* 8. rx high but BT_EN low: the chip is unpowered, so the high line is not its TX. */
	{
		struct dby_bt_m0_inputs rx = in;
		set_pin(&rx, (unsigned)DbyBtM0PinSe0Rx, DBY_BT_M0_FUNC_GPIO, 1u, 0u);
		test_case("rx-high-without-bt_en", &rx, "UNKNOWN", "no-static-evidence");
	}

	/* 9. The GENI gate: a clocked SE0 is the only condition under which mode 2 may read it. */
	{
		struct dby_bt_m0_inputs clk = in;
		gcc_all(&clk, DBY_BT_M0_GCC_VOTE_CORE | DBY_BT_M0_GCC_VOTE_M_AHB |
		    DBY_BT_M0_GCC_VOTE_S_AHB | DBY_BT_M0_GCC_VOTE_SE0, 0u, DBY_BT_M0_CBCR_CLK_OFF);
		dby_bt_m0_evaluate(&clk, &out);
		expect("se0_clocked", out.se0_clocked != 0u ? "1" : "0", "1");
		expect("qup_domain_on", out.qup_domain_on != 0u ? "1" : "0", "1");
	}

	/* 10. An unmapped TLMM must not read as "all pins are plain GPIO". */
	{
		struct dby_bt_m0_inputs nomap = in;
		struct dby_bt_m0_result r;
		unsigned i;
		for (i = 0; i < (unsigned)DbyBtM0PinCount; ++i) {
			nomap.pin[i].valid = 0u;
		}
		nomap.tlmm_mapped = 0u;
		dby_bt_m0_evaluate(&nomap, &r);
		expect("no-tlmm verdict", dby_bt_m0_verdict_name(r.verdict), "UNKNOWN");
		expect("  why", r.why, "no-tlmm");
	}

	/* 11. The row renderer refuses to print an unmeasured word as a zero. */
	{
		struct dby_bt_m0_inputs blank;
		struct dby_bt_m0_result r;
		char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES];
		memset(&blank, 0, sizeof blank);
		blank.mode = 2u;
		dby_bt_m0_evaluate(&blank, &r);
		dby_bt_m0_render(&blank, &r, rows);
		expect("row1 says not-read", strstr(rows[1], "not-read") != NULL ? "1" : "0", "1");
		expect("row3 says not-read", strstr(rows[3], "not-read") != NULL ? "1" : "0", "1");
		expect("row4 says unmeasured", strstr(rows[4], "bt_en=-") != NULL ? "1" : "0", "1");
		expect("row5 says unknown clocks", strstr(rows[5], "se0clk=?") != NULL ? "1" : "0", "1");
	}

	/* 12. Every rendered row fits its 120-byte slot, with the terminator. */
	{
		struct dby_bt_m0_result r;
		char rows[DBY_BT_M0_ROWS][DBY_BT_M0_ROW_BYTES];
		unsigned i;
		dby_bt_m0_evaluate(&in, &r);
		dby_bt_m0_render(&in, &r, rows);
		for (i = 0; i < (unsigned)DBY_BT_M0_ROWS; ++i) {
			expect("row fits", strlen(rows[i]) < DBY_BT_M0_ROW_BYTES ? "1" : "0", "1");
		}
	}

	/* 13. SLIMBUS is unreachable from this ladder, whatever the input. */
	{
		struct dby_bt_m0_inputs any = in;
		struct dby_bt_m0_result r;
		set_qup_pins(&any, (unsigned)DbyBtM0PinSe0Cts, 3u);   /* an alternate function */
		set_qup_pins(&any, (unsigned)DbyBtM0PinSe6Cts, 3u);
		dby_bt_m0_evaluate(&any, &r);
		expect("never SLIMBUS", dby_bt_m0_verdict_name(r.verdict), "UNKNOWN");
	}

	if (failures == 0) {
		printf("PASS: all cases\n");
	} else {
		printf("FAIL: %d case(s)\n", failures);
	}
	return failures == 0 ? 0 : 1;
}
#endif /* DBY_BT_M0_SELFTEST */
