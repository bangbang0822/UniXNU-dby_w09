/**
  Board reset providers for the persistent log writer's unattended mode.

  package/BootLog.c carries the timer: with `sheng-autoreset=<seconds>` it asks boot_reset_probe()
  whether a reset is possible, calls boot_reset_mark_bootloader() and boot_reset_watchdog() once at
  start, and calls boot_reset_bootloader() when the deadline passes. On this board all four used to
  be weak stubs that returned -1, so the timer did nothing.

  The reset itself is PSCI SYSTEM_RESET through SMC: the device tree says
  `psci { compatible = "arm,psci-1.0"; method = "smc"; }`, and XNU's PEHaltRestart() cannot be used
  because PE_halt_restart is never assigned on this port (no AppleARMPE bound to the hardware), so it
  returns -1 without touching the platform.  A PSCI reset has been observed to reboot this board, so
  the reset half is proven; what is not proven is that the firmware then lands in fastboot.

  That second half is the board's own mechanism, four bytes of IMEM:

    kona.dtsi:1947-1962      qcom,msm-imem@146bf000 { restart_reason@65c { reg = <0x65c 4>; } }
    msm-poweroff.c:657       IMEM_RESTART_REASON_BOOTLOADER = 0x77665500  (`reboot bootloader`)
    msm-poweroff.c:689       IMEM_RESTART_REASON_NORMAL     = 0x77665501  (ordinary reboot)

  Two deliberate limits, both from work/tracks/autoreset-spec/SPEC.md:

    * S1 (default).  The probe only *reads* the word and reports it; it never writes and therefore
      never returns 0.  The first MMIO access from the first-second tick is the one thing that could
      kill the kernel before any log exists (SPEC.md:513), so it is measured on its own first.
      `sheng-autoreset-write=1` moves the board to S3 without rebuilding the kernel.

    * S3.  The probe writes 0x77665500, reads it back and only then returns 0.  If the word already
      holds 0x77665500, the previous boot asked for the bootloader and came back, so ABL did not
      honour it: the word is restored to the vendor's normal-reboot value and the caller is refused.
      That turns "ABL ignores the marker" into at most one extra boot instead of an endless reset
      loop nobody can interrupt (SPEC.md:463-490).  `sheng-autoreset-force=1` overrides the refusal
      for a one-off experiment and says so in the row.

  The probe is called three times per boot (BootLog.c:1090, BootLog.c:1155, BootProgress.c:749) and
  each call used to repeat the whole read/write/clear sequence, which made the fuse clear what another
  caller had just armed.  It now runs once per boot and replays its verdict, so all three agree.  The
  one exception is deliberate: a call from an exception vector is answered -1 and *not* recorded,
  because the mapping it needs is not interrupt-safe.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <stdint.h>
#include <stddef.h>

#include <mach/vm_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "BootResetSpmb.h"

/* Published for the diagnostics row: how the reboot-reason write went. */
uint32_t dby_reset_mark_rc = 0xFFFFFFFFu;
uint32_t dby_reset_mark_before = 0xFFu;
uint32_t dby_reset_mark_after = 0xFFu;
/* The whole word as found at this boot, before anything this code does. */
uint32_t dby_reset_imem_value = 0xFFFFFFFFu;

/* PSCI SYSTEM_RESET. The 32-bit SMC form is the one every PSCI implementation this board
 * could carry accepts; SYSTEM_RESET takes no arguments, so the SMC64 variant buys nothing. */
#define DBY_PSCI_SYSTEM_RESET   0x84000009ULL
#define DBY_PSCI_VERSION        0x84000000ULL
#define DBY_PSCI_FEATURES       0x8400000AULL

/* BASE: kona.dtsi:1947-1962  qcom,msm-imem@146bf000, restart_reason@65c, reg = <0x65c 4>.
 * Values: msm-poweroff.c:657 (0x77665500 bootloader) and :689 (0x77665501 normal reboot). */
#define DBY_IMEM_BASE           0x146BF000ULL
#define DBY_IMEM_SIZE           0x1000ULL
#define DBY_IMEM_RESTART_REASON 0x65CULL
#define DBY_IMEM_BOOTLOADER     0x77665500U
#define DBY_IMEM_NORMAL         0x77665501U

static uint64_t
dby_psci_call(uint64_t function)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile("smc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3)
	                 : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
	                   "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	return x0;
}

/* SMC with one argument, for PSCI_FEATURES. */
static uint64_t
dby_psci_call1(uint64_t function, uint64_t argument)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = argument;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile("smc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3)
	                 : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
	                   "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	return x0;
}

static void
dby_report(char *out, unsigned capacity, const char *text)
{
	unsigned i;

	if (out == NULL || capacity == 0) {
		return;
	}
	for (i = 0; i + 1 < capacity && text[i] != '\0'; i++) {
		out[i] = text[i];
	}
	out[i] = '\0';
}

/* The row text is assembled here rather than with snprintf: no translation unit in this package
 * calls snprintf (DbyTouch.cpp:319-323 explains why), and a wrong prototype in kernel code is a
 * silent stack corruption.  Every appender keeps the buffer NUL-terminated and stops at `last`,
 * which is the address of the final byte. */
static void
dby_append(char **cursor, char *last, const char *text)
{
	char *p = *cursor;

	while (p < last && *text != '\0') {
		*p++ = *text++;
	}
	*p = '\0';
	*cursor = p;
}

static void
dby_append_ch(char **cursor, char *last, char value)
{
	char one[2];

	one[0] = value;
	one[1] = '\0';
	dby_append(cursor, last, one);
}

static void
dby_append_hex(char **cursor, char *last, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	int shift;

	for (shift = 28; shift >= 0; shift -= 4) {
		dby_append_ch(cursor, last, digits[(value >> shift) & 0xFu]);
	}
}

static void
dby_append_dec(char **cursor, char *last, uint32_t value)
{
	char reversed[10];
	int count = 0;

	if (value == 0) {
		dby_append_ch(cursor, last, '0');
		return;
	}
	while (value != 0 && count < 10) {
		reversed[count++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	while (count > 0) {
		dby_append_ch(cursor, last, reversed[--count]);
	}
}

/* IMEM is always-on SRAM described by the device tree, but this kernel has no physmap: the address
 * is physical, so dereferencing it directly would trap.  ml_io_map() is the pmap-level mapping that
 * exists before IOKit does (SPEC.md:428), and the mapping is permanent - sheng notes ml_io_unmap
 * panics (SPEC.md:429) - so it is mapped at most once and never released. */
static volatile uint32_t *
dby_restart_reason(void)
{
	static volatile uint32_t *reason;
	static int tried;
	vm_offset_t va;

	if (reason != NULL) {
		return reason;
	}
	if (tried) {
		return NULL;
	}
	tried = 1;
	va = ml_io_map((vm_offset_t)DBY_IMEM_BASE, (vm_size_t)DBY_IMEM_SIZE);
	if (va == 0) {
		return NULL;
	}
	reason = (volatile uint32_t *)(uintptr_t)(va + DBY_IMEM_RESTART_REASON);
	return reason;
}

static char gProbeText[96];
static int  gProbeState;      /* 0 = not run yet, 1 = armed (0), -1 = refused */

/* The probe is the gate: BootLog.c:1154 arms the unattended autoreset only when this returns 0, and
 * it *ignores* boot_reset_mark_bootloader()'s result, so this is the only place that decides whether
 * the board ever resets itself.  It answers 0 only for S3 with a verified readback. */
int
boot_reset_probe(char *out, unsigned capacity)
{
	volatile uint32_t *reason;
	uint32_t before;
	uint32_t after;
	uint32_t arm = 0;
	uint32_t force = 0;
	uint64_t version;
	uint64_t features;
	char *p;
	char *last;

	/* Three call sites share one boot; the second and third replay this verdict so that the fuse
	 * cannot clear what another caller just armed. */
	if (gProbeState != 0) {
		dby_report(out, capacity, gProbeText);
		return (gProbeState == 1) ? 0 : -1;
	}

	/* The earliest caller is boot_progress_poll() from the IRQ tail (BootProgress.c:701 with the
	 * default boot-progress=2), and ml_io_map() below is not interrupt-safe: mapping from an
	 * exception vector can panic before this board has any log to leave behind (BootProgress.c:710-712
	 * is the same rule already learned here).  Refuse without recording a verdict, so the one
	 * mapping this code ever does still happens from a thread. */
	if (ml_at_interrupt_context()) {
		dby_report(out, capacity, "dby: probe deferred (interrupt context)");
		return -1;
	}

	p = gProbeText;
	last = gProbeText + sizeof gProbeText - 1;
	gProbeText[0] = '\0';

	PE_parse_boot_argn("sheng-autoreset-write", &arm, sizeof arm);
	PE_parse_boot_argn("sheng-autoreset-force", &force, sizeof force);

	version = dby_psci_call(DBY_PSCI_VERSION);
	features = dby_psci_call1(DBY_PSCI_FEATURES, DBY_PSCI_SYSTEM_RESET);
	dby_append(&p, last, "psci v");
	dby_append_dec(&p, last, (uint32_t)((version >> 16) & 0xFFFFu));
	dby_append_ch(&p, last, '.');
	dby_append_dec(&p, last, (uint32_t)(version & 0xFFFFu));
	dby_append(&p, last, ((features & 0xFFFFFFFFULL) == 0) ? " reset=ok " : " reset=? ");

	reason = dby_restart_reason();
	if (reason == NULL) {
		dby_append(&p, last, "imem: not mapped");
		gProbeState = -1;
		dby_report(out, capacity, gProbeText);
		return -1;
	}

	before = *reason;
	dby_reset_imem_value = before;
	dby_reset_mark_before = (uint8_t)(before & 0xFFu);
	dby_append(&p, last, "imem=");
	dby_append_hex(&p, last, before);

	/* S1: report only.  Nothing is written, so nothing on this board changes behaviour. */
	if (arm == 0 && force == 0) {
		dby_append(&p, last, " ro");
		gProbeState = -1;
		dby_report(out, capacity, gProbeText);
		return -1;
	}

	/* The marker this code wrote last boot is still there, so ABL did not consume it: refuse, and
	 * leave the vendor's normal-reboot value behind so a marker-free boot comes back clean. */
	if (before == DBY_IMEM_BOOTLOADER && force == 0) {
		*reason = DBY_IMEM_NORMAL;
		__asm__ volatile("dsb sy" ::: "memory");
		dby_reset_mark_rc = 2;
		dby_reset_mark_after = (uint8_t)(DBY_IMEM_NORMAL & 0xFFu);
		dby_append(&p, last, " fuse: leftover, refused");
		gProbeState = -1;
		dby_report(out, capacity, gProbeText);
		return -1;
	}

	*reason = DBY_IMEM_BOOTLOADER;
	__asm__ volatile("dsb sy" ::: "memory");
	after = *reason;
	dby_reset_mark_after = (uint8_t)(after & 0xFFu);
	if (after != DBY_IMEM_BOOTLOADER) {
		dby_reset_mark_rc = 1;
		dby_append(&p, last, " write failed, back=");
		dby_append_hex(&p, last, after);
		gProbeState = -1;
		dby_report(out, capacity, gProbeText);
		return -1;
	}

	dby_reset_mark_rc = 0;
	dby_append(&p, last, " -> 77665500 armed");
	if (force != 0) {
		dby_append(&p, last, " (forced)");
	}
	gProbeState = 1;
	dby_report(out, capacity, gProbeText);
	return 0;
}

/* The probe already wrote the word; this only reports that. */
int
boot_reset_mark_bootloader(char *out, unsigned capacity)
{
	dby_report(out, capacity, (dby_reset_mark_rc == 0) ? "dby: imem marked" : "dby: imem not marked");
	return (dby_reset_mark_rc == 0) ? 0 : -1;
}

/* No hypervisor watchdog exists on this platform, so answer -1: calling this "armed" would be a
 * lie. The caller keeps the text and still proceeds with the deadline, which is carried by the log
 * writer's timer and by the independent one in package/BootProgress.c. */
int
boot_reset_watchdog(uint32_t seconds, char *out, unsigned capacity)
{
	(void)seconds;
	dby_report(out, capacity, "dby: no hypervisor watchdog");
	return -1;
}

/* Called by the log writer when sheng-autoreset expires. Does not return on success. */
int
boot_reset_bootloader(char *out, unsigned capacity)
{
	volatile uint32_t *reason = dby_restart_reason();

	/* Re-assert the marker: the caller reaches here seconds after the probe, and this is the one
	 * moment the word must be right. */
	if (reason != NULL) {
		*reason = DBY_IMEM_BOOTLOADER;
		__asm__ volatile("dsb sy" ::: "memory");
	}
	dby_report(out, capacity, "dby: psci reset");
	dby_psci_call(DBY_PSCI_SYSTEM_RESET);
	/* Reaching here means the conduit refused; report that so the caller can log it. */
	dby_report(out, capacity, "dby: psci reset returned");
	return -1;
}
