# BHI reject `0xEF128100` on QCA6390 — upstream-source brief

All `file:line` are torvalds/master fetched 2026-11-24 via git.kernel.org `plain/`.

## 1. Probe → firmware download order (QCA6390 hw2.0)

`ath11k_pci_probe` (pci.c, line 949): `ath11k_pcic_init_msi_config` (:1083) → `ath11k_pci_alloc_msi` (:1089) → `ath11k_core_pre_init` (:1095, firmware fetch) → **`ath11k_mhi_register` (:1105)** → `ath11k_hal_srng_init` (:1111) → `ath11k_ce_alloc_pipes` (:1115) → `ath11k_pci_init_qmi_ce_config` (:1121) → `ath11k_pcic_config_irq` (:1123) → `ath11k_pci_config_msi_data` (:1134) → **`ath11k_core_init` (:1140)**.

`ath11k_core_init` (core.c, line 2683) → `ath11k_core_soc_create` → `ath11k_core_power_up` (core.c, line 1985): `ath11k_qmi_init_service` (:1993, *before* power-up) then `ath11k_hif_power_up` (:2005) = `ath11k_pci_power_up` (pci.c, line 818): `sw_reset(power_on)` (:825) → `aspm_disable` (:830) → `msi_enable` (:832) → **`ath11k_mhi_start` (:834)**.

`ath11k_mhi_start` (mhi.c, line 433) is the function that starts MHI: sets `timeout_ms = 20000` (:438; `MHI_TIMEOUT_DEFAULT_MS` mhi.c, line 14), `mhi_prepare_for_power_up` (:440), `mhi_sync_power_up` (:446).

`firmware` is handed over in `ath11k_mhi_register`: mhi.c, line 351-361 —
```c
if (ab->fw.amss_data && ab->fw.amss_len > 0) {
	mhi_ctrl->fw_data = ab->fw.amss_data;   /* firmware-N.bin */
	mhi_ctrl->fw_sz   = ab->fw.amss_len;
} else {
	ath11k_core_create_firmware_path(ab, ATH11K_AMSS_FILE, ab_pci->amss_path, ...);
	mhi_ctrl->fw_image = ab_pci->amss_path;
}
```
The path is `ath11k/<hw_params.fw.dir>/<file>` (core.h, line 1342-1359). QCA6390 hw2.0 is core.c, line 219-220 (`.name = "qca6390 hw2.0"`, `.hw_rev = ATH11K_HW_QCA6390_HW20`), so `amss.bin` resolves to `ath11k/QCA6390/hw2.0/amss.bin`; `MODULE_FIRMWARE(ATH11K_FW_DIR "/QCA6390/hw2.0/*")` pci.c, line 1331, `ATH11K_FW_DIR "ath11k"` hw.h, line 74.

Roles: **amss.bin → BHI then BHIe** (mhi.c, line 360 → boot.c, line 500-537, 569-588). **m3.bin → QMI, not BHI**: requested at qmi.c, line 2532, placed in a `dma_alloc_coherent` buffer (qmi.c, line 2545-2555) and announced with `QMI_WLANFW_M3_INFO_REQ_V01` (qmi.c, line 2613) carrying `req.addr = m3_mem->paddr; req.size` (qmi.c, line 2599-2600), only on the `ATH11K_QMI_EVENT_FW_MEM_READY` event (qmi.c, line 3252), i.e. *after* MHI READY. **board-2.bin → QMI** (`ATH11K_BOARD_API2_FILE` hw.h, line 77, core.c, line 1715/1835, sent by `ath11k_qmi_event_load_bdf` qmi.c, line 3246). There is no `bdwlan.bin` filename in ath11k (hw.h, line 74-82): bdwlan is carried inside the board file — **unverified** in detail, but no separate request exists.

## 2. `fbc_download` config

mhi.c, line 381-384:
```c
mhi_ctrl->rddm_size = RDDM_DUMP_SIZE;   /* 0x420000, mhi.c, line 15 */
mhi_ctrl->sbl_size  = SZ_512K;
mhi_ctrl->seg_len   = SZ_512K;
mhi_ctrl->fbc_download = true;
```
Only the SBL slice goes over BHI: `size = (mhi_cntrl->fbc_download) ? mhi_cntrl->sbl_size : firmware->size;` and `if (size > firmware->size) size = firmware->size;` (boot.c, line 530-534). `mhi_load_image_bhi` copies *only those bytes* into one coherent buffer and points `BHI_IMGADDR` at it (boot.c, line 444-451). After BHI success, boot.c, line 575-587 skips a second ELF header at `fw_data + sbl_size` and builds the BHIe vector table for the remainder; `mhi_download_amss_image` later pushes it (boot.c, line 617-629) from `DEV_ST_TRANSITION_SBL` (pm.c, line 825-826) once the device is READY. Host must pre-populate the coherent SBL buffer and, for fbc, the BHIe vector table before the doorbell.

## 3. Preconditions, in order

1. `ath11k_pci_sw_reset(ab, true)` (pci.c, line 380-396): `mdelay(100)` (:382) → `enable_ltssm` (:385, LTSSM=PARM_LTSSM_VALUE, `mdelay(5)` :352) → `clear_all_intrs` (:386) → `set_wlaon_pwr_ctrl` (:387) → `fix_l1ss` (:388-389) → `mhi_clear_vector` (:392) → `clear_dbg_registers` (:393: reads `PCIE_Q6_COOKIE_ADDR`, reads `WLAON_WARM_SW_ENTRY`, `mdelay(10)`, writes 0 to it, `mdelay(10)`, reads `WLAON_SOC_RESET_CAUSE_REG`, pci.c, line 233-255) → `soc_global_reset` (:394) → `mhi_set_mhictrl_reset` (:395).
2. ASPM off, pci.c, line 827-829: *"Disable ASPM during firmware download due to problems switching to AMSS state."* → `ath11k_pci_aspm_disable` (:830, clears `PCI_EXP_LNKCTL_ASPMC`, :622).
3. `ath11k_pci_msi_enable` (pci.c, line 832).
4. `mhi_prepare_for_power_up` (init.c, line 1109): reads `BHIOFF` (:1121) and `BHIEOFF` (:1136); each must be `< reg_len` (:1127, :1143) else `-ERANGE`; maps `bhi`/`bhie` (:1133, :1150); clears BHIE RX regs and builds the RDDM table (:1153-1175).
5. `mhi_async_power_up` (pm.c, line 1140): **writes `BHI_INTVEC = 0`** (:1166 — upstream always 0; re-programmed at :1198 after SYS_ERR reset), requires EE in PBL/PTHRU/EDL/AMSS (:1173, `MHI_POWER_UP_CAPABLE` internal.h, line 77), enables IRQs (:1202-1208), queues PBL transition (:1212-1215) → `mhi_fw_load_handler` (pm.c, line 813).
6. `mhi_fw_load_handler` (boot.c, line 474): reads `BHI_SERIALNU` (:491); `MHI_FW_LOAD_CAPABLE(ee)` = PBL or EDL only (internal.h, line 78, boot.c, line 497); writes `BHI_STATUS=0`, `BHI_IMGADDR_HIGH/LOW`, `BHI_IMGSIZE`, `BHI_IMGTXDB` (boot.c, line 276-280); polls `BHI_STATUS_MASK = GENMASK(31,30)` with a 20 s timeout (boot.c, line 284-288, mhi-common.h, line 68-71).
7. Link layer: PERST# must be asserted ≥100 ms before deassert (`msleep(PCIE_T_PVPERL_MS)`, pcie-qcom.c, line 358-359) plus `PERST_DELAY_US=1000` settle (:207). The "60 ms pwrseq" figure is **unverified** — not present in pci.c, mhi.c or the pcie-qcom paths read.

## 4. How upstream treats a BHI failure

boot.c, line 292-296:
```c
if (tx_status == BHI_STATUS_ERROR) {
	dev_err(dev, "Image transfer failed\n");
	mhi_fw_load_error_dump(mhi_cntrl);
	goto invalid_pm_state;
}
```
`mhi_fw_load_error_dump` (boot.c, line 180-208) prints `ERROR_CODE/ERROR_DBG1/2/3` (offsets 0x30/0x34/0x38/0x3c, mhi-common.h, line 54-57) and nothing else. **There is no retry and no BHI→BHIe fallback**: `fw_load_type` is chosen once (boot.c, line 540, `mhi_fw_load_type_get` :427-437) and any error goes to `error_fw_load` → `MHI_PM_FW_DL_ERR` (:609-614). `mhi_sync_power_up` then aborts because `MHI_PM_FATAL_ERROR` includes `FW_DL_ERR` (pm.c, line 1307-1312, internal.h, line 173) and returns `-ETIMEDOUT` (-110), exactly as ath11k logs "failed to power up :-110".

READY is a *separate* poll in `mhi_ready_state_transition` (pm.c, line 170-202): `MHICTRL_RESET` clear, then `MHISTATUS_READY_MASK` set, 25 ms interval. So "BHI_STATUS=ERROR + MHISTATUS state 0 + READY clear" means: the device rejected the SBL while still in PBL and never reached READY. State 0 is `MHI_STATE_RESET` (mhi.h, line 167). There is no other documented meaning.

## 5. The error code

The only authoritative-ish statement found: ath11k list, QCN9074, Jun 2022 — identical `Reg: ERROR_CODE value: 0xef128100`, `ERROR_DBG1 0xfffa0000`, `ERROR_DBG3 0x1ef13` ([post](https://lists.infradead.org/pipermail/ath11k/2022-June/003285.html)); QUIC's reply: *"it looks the ELF header is bad, so PBL rejected it"*, and asks for md5 of amss.bin plus `upper/lower_32_bits(dma_addr)`, `size`, `session_id` ([reply](https://lists.infradead.org/pipermail/ath11k/2022-July/003305.html)).

No kernel commit, no regression report, and no public register description document the `0xEFxxxxxx` encoding. The "PBL stage + code" reading is **unverified**. Note `0xEF128100 >> 30 == 3 == BHI_STATUS_ERROR`, so it cannot be distinguished from a status read at the wrong offset without knowing your read address.

## 6. Ranked hypotheses for this signature

1. **The device is not reading the bytes you verified** (address/SID mismatch). PBL reports "bad ELF header" while the CPU sees a valid ELF through an alias ⇒ only the device's view differs. Implied: SMMU stream 0x1c01 / bypass bank, or the address latched into `BHI_IMGADDR`. Test: put stream 0x1c01 in a *translated* domain mapping only that buffer; a fault at the buffer proves the device really reads it, no fault means the SID/ATU is wrong. Also retry the same 512 KiB at a 2 MiB-aligned frame — a changed ERRCODE confirms address dependence.
2. **Wrong slice or wrong artefact.** Upstream sends only the first 512 KiB of amss.bin over BHI (boot.c, line 530) — never the 4.3 MB file, and never m3.bin (QMI-only, qmi.c, line 2613). Test: assert `IMGSIZE == 512KiB`, `memcmp(buf,"\x7fELF",4)==0`, `EI_CLASS==2`, and every `p_offset+p_filesz <= 512KiB`; md5 against `ath11k/QCA6390/hw2.0/amss.bin`.
3. **Latency is a fixed internal step, not a transfer.** 414.5 ms identical for 266 KB and 4.3 MB is not byte-proportional. Test: sweep `BHI_IMGSIZE` 512 B → 512 KiB; constant time ⇒ the PBL fails before consuming the payload (points at auth/ELF-header stage), scaling ⇒ real transfer.
4. **Bad SBL image generation / secure-boot reject.** Fixed-latency signature check would explain constant time; **hypothesis**, untestable without a known-good dump from the same part.
5. **Skipped power-up step** (`WLAON_WARM_SW_ENTRY` not cleared, wrong LTSSM value) — low: link already trains. Test: read `WLAON_WARM_SW_ENTRY` and `WLAON_SOC_RESET_CAUSE_REG` after the error.
6. **BHI_INTVEC** — very low: upstream writes 0 (pm.c, line 1166); you already tried 0.

Highest-value next probe: 1 and 3 together — they separate "device can't see the buffer" from "device rejects the content".
