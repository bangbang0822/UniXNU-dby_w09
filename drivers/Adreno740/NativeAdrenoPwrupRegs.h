/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PWRUP_REGS_H
#define NATIVE_ADRENO_PWRUP_REGS_H
#include <stdint.h>
/* The A7XX entries of the pinned a6xx.xml/adreno_common.xml register definitions and of a6xx_catalog.c, in the
 * order the power-up list walks them. Dword register offsets.
 *
 * PROVENANCE: the generator that produced them (scripts/derive-a740-pwrup.py) is NOT part of this repository --
 * only the value list was carried over. These sixteen addresses therefore cannot be re-derived here: doing so
 * means running that script against the pinned a6xx tree, and a hand edit cannot be checked against anything in
 * this tree. Treat the file as read-only for the same reason. */
static const uint32_t NativeAdrenoA740PwrupRegs[]={
    0x00000e09,
    0x00000e0a,
    0x00000e07,
    0x00000e08,
    0x00000e0b,
    0x00000e0c,
    0x00000e0d,
    0x00000e0e,
    0x00000e17,
    0x00000e01,
    0x00008e08,
    0x00008e28,
    0x00008e50,
    0x00000e3a,
    0x00000e19,
    0x0000b602,
};
#define NATIVE_ADRENO_A740_PWRUP_COUNT \
    (sizeof(NativeAdrenoA740PwrupRegs)/sizeof(NativeAdrenoA740PwrupRegs[0]))
#endif
