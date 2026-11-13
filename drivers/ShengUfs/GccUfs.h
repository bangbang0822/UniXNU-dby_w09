// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8550 GCC registers used by UFS, transcribed as facts from the public
// Linux gcc-sm8550.c clock driver. No write is performed by NativeUfs yet.
//
// These offsets are SM8550's, and this board is SM8250 (kona). SM8250's GCC has its own layout and
// nothing in this repository records kona's UFS branch offsets, so the values NativeUfs::inspect()
// reads through this map are logged for a build that runs on an SM8550 board and are only a
// heuristic on dby_w09: the probe gate they feed (powered && busClocks) decides whether the UFS
// host-controller registers are touched, and on kona that decision rests on unrelated registers.
// Do not treat an inspect() dump from this board as a statement about its UFS clock tree until the
// kona offsets are recorded with a source.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the sheng board DT (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

#define GCC_UFS_PHY_AXI_ENABLE        0x77018
#define GCC_UFS_PHY_AHB_ENABLE        0x77024
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x77028
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x7702c
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x77068
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x77074
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770b0
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770cc
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x770e4, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770e4

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8550.c, so CLK_OFF=1 on
// an idle branch is normal (hardware 2026-10-17 read ahb=80000003).
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif
