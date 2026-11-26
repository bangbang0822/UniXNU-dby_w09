# QCA6390 boot-loader reject `0xef128100`: upstream, community and board evidence

Research note, 2026-11-24. Compiled from three parallel investigations - upstream kernel source, community
and vendor reports, and this tablet's own Linux ports - because the hardware evidence had reached a boundary:
the device consumes the boot-host doorbell, transfers the image, and then refuses it with one invariant code.

## 1. The failure this note is about

Measured on the tablet (payload `be60dd08` lineage, `DbyPcie.cpp` M32-M48). Every attempt produces the same
three things, whatever is changed:

```
[dby-pcie] bhi wrote addr=00000000:<arena> size=<n> vec=<0|1> db=1a5a0001 | read back <identical>
[dby-pcie] bhi sts=ffffffff xfer=3 exec=00000000 ee=0 err=ef128100 waited=<414-416>000us
[dby-pcie] bhi post mhi_sts=00000000 state=0 ready=0 exec=00000000
[dby-pcie] m6l pbl log05 42100100 00000005 0080ead8 00000253 ef000008      (identical every boot)
```

`BHI_ERRDBG1` reads back the image address and `ERRDBG3` the size; `MHI_STATUS` answers with state 0 (RESET,
i.e. PBL) and READY clear; two seconds of polling change nothing. Already eliminated on hardware, one variable
per flash: image integrity in DRAM (ELF magic read back through an uncached alias), image file and size
(266,684 / 524,288 / 4,352,144 bytes), address width, every descriptor field including `BHI_INTVEC` 1 and 0,
the poll window (raised to 20 s), link rate (Gen2 and forced Gen1), the whole vendor power/reset pre-chain,
four PMIC clocks and four WLAN rails voted through RPMh, and the endpoint's inbound translation window. The
SMMU records no fault before or after the transfer.

## 2. What `0xef128100` means: it is a public code, and Qualcomm answered it

The value appears verbatim in an ath11k mailing-list report for a QCN9074 on kernel 5.15
([June 2022](https://lists.infradead.org/pipermail/ath11k/2022-June/003285.html)):

> `mhi mhi0: Image transfer failed`
> `mhi mhi0: Reg: ERROR_CODE value: 0xef128100`
> `mhi mhi0: Reg: ERROR_DBG1 value: 0xfffa0000`
> `mhi mhi0: Reg: ERROR_DBG2 value: 0x0`
> `mhi mhi0: Reg: ERROR_DBG3 value: 0x1ef13`
> `mhi mhi0: MHI did not load image over BHI, ret: -5`

A Qualcomm engineer replied in the
[July 2022 follow-up](https://lists.infradead.org/pipermail/ath11k/2022-July/003305.html):

> "From the below logs, it looks the ELF header is bad, so PBL rejected it."

and asked for exactly three things: the md5 of the `amss.bin` used, and the `dma_addr` upper/lower halves,
size and session id printed inside `mhi_fw_load_bhi`. The thread has no resolution, so the code's meaning is
vendor-stated but not independently explained; the `0xEF` prefix looking like a "PBL stage" tag is an
inference, not a documented encoding.

What the precedent does establish is the shape: **the same code on a different chip and a different host
means this is PBL's generic image-rejection, not a statement about our picture of the transport.** It also
matches the constant 414.5 ms across 266 KB and 4.3 MB: a failure that does not scale with the number of bytes
transferred is not a transfer that failed.

A CAF/cnss2 tree carries the QCA6390-specific dump that names the registers worth reading next
([hamjin/kernel_xiaomi_sm8250 commit a1fe641](https://github.com/hamjin/kernel_xiaomi_sm8250/commit/a1fe641482ee7a684550bd6ff83cf3ff221fe5e6)):
`TCSR_PBL_LOGGING 0x01b000f8`, `PBL_WLAN_BOOT_CFG 0x01e22b34`, `PBL_BOOTSTRAP_STATUS 0x01910008`, an 80-byte
PBL log at `0x01403d58`, plus `BHI_ERRDBG2/3`. This probe already reads all of those (stage M39) - which is
why the log row exists in section 1 above.

## 3. Upstream facts that reframe the failure

From `drivers/net/wireless/ath/ath11k/{pci,mhi,qmi,core}.c` and `drivers/bus/mhi/host/{boot,init,pm}.c`
(torvalds/master, fetched 2026-11-24; full `file:line` brief in `bhi-ef128100-brief.md` beside this document):

1. **Only the first 512 KiB of `amss.bin` ever goes over BHI.** `ath11k_mhi_register` hands MHI the
   `amss.bin` path, and `mhi_fw_load_handler` computes `size = fbc_download ? sbl_size : firmware->size`
   with `sbl_size = SZ_512K`, clamped to the file size (`boot.c, line 530-534`). The remainder is described by a
   BHIe vector table built at `boot.c, line 575-587` and pushed by `mhi_download_amss_image` (`boot.c, line 617`) only
   *after* the device reports READY.
2. **`m3.bin` is never a BHI image.** It is announced over QMI (`QMI_WLANFW_M3_INFO_REQ`, `qmi.c, line 2599-2616`)
   once the firmware is up (`qmi.c, line 3252`). One of this probe's experiments (M38) made `m3.bin` the BHI image;
   by construction that was the wrong picture, even though the outcome did not change.
3. **`BHI_INTVEC` is always 0 upstream** (`pm.c, line 1166`, reprogrammed at `:1198` after a SYS_ERR reset), and
   mainline's `mhi_fw_load_bhi` does not write it at all. The probe's M46 boot wrote 0; that is the upstream
   value.
4. **There is no retry and no BHI-to-BHIe fallback.** `fw_load_type` is chosen once (`boot.c, line 540`); an error
   goes to `MHI_PM_FW_DL_ERR` (`boot.c, line 609-614`), `mhi_sync_power_up` returns `-ETIMEDOUT`
   (`pm.c, line 1307-1312`), and ath11k logs "failed to power up :-110". So a device that answers BHI with an error
   and never sets READY is a device still in PBL, and there is no other documented meaning for
   "BHI error + state 0 + READY clear".
5. **The context/control-segment bases are programmed *after* the image load**, in
   `mhi_ready_state_transition`, not before the doorbell. Expecting the device to need CRCBAP/CCABAP/ECABAP
   pre-BHI looks like a dead end (this was one of the ideas in the earlier plan).
6. **`ath11k_pci_power_up` disables ASPM before starting MHI**, with the comment *"Disable ASPM during
   firmware download due to problems switching to AMSS state"* (`pci.c, line 827-830`), clearing
   `PCI_EXP_LNKCTL_ASPMC` on the endpoint (`pci.c, line 622`). The original patch is Carl Huang's
   ([Dec 2020](https://lists.infradead.org/pipermail/ath11k/2020-December/000914.html)): *"Sometimes QCA6390
   doesn't switch to amss state as device enters L1ss state, so disable L0sL1s during firmware
   downloading."* This probe has never touched the endpoint's LNKCTL.
7. **`fix_l1ss`** (`pci.c, line 284`, called at `:388`) is a real QCA6390 hardware workaround around L1 substates.

## 4. Community and sibling-board evidence

- The pwrseq timings for this exact part are in `drivers/power/sequencing/pwrseq-qcom-wcn.c`:
  `pwrseq_qca6390_of_data` lists eight supplies and **`pwup_delay_ms = 60`**, `gpio_enable_delay_ms = 100`,
  with the wlan target depending on *both* the regulator and clock units. The probe's own power vote (M48)
  now runs before WLAN_EN, but its post-enable delay is still 10 ms.
- The Xiaomi Pad 5 Pro (`sm8250-xiaomi-elish-common.dtsi`) wires the same PMU identically - s6a 0.95 V,
  s4a 1.8 V, s5a 1.9 V, s8c 1.35 V, WLAN_EN GPIO20, BT_EN GPIO21, `compatible = "pci17cb,1101"` - and adds
  nothing our configuration lacks except a `qcom,calibration-variant` string. It has no `xo-clk` GPIO (that
  is a wcn6855 target only) and no memory-region on the PCI child.
- A 2026 Xiaomi Pad 5 Pro record (`Huabin1010/dagu-mainline-linux`) notes that the board file must be
  `board.bin`, not `board-2.bin`, and that the QCA6390 antenna-switch rail `VDD_ASD` (PM8150 L16) is **not
  enabled by mainline**: until it was powered, Bluetooth paging failed ten times out of ten. That rail is a
  new item for the Bluetooth plan and possibly relevant to Wi-Fi transmit calibration, not to BHI.
- Bluetooth for the same chip: `&uart6` + `compatible = "qcom,qca6390-bt"`, `max-speed = <3000000>`,
  firmware `qca/htbtfw20.tlv` + `qca/htnv20.bin`, `local-bd-address` little-endian, and two gotchas - do not
  also give the BT node its own `enable-gpios` (BT_EN is driven by pwrseq through GPIO21) and do not set
  `firmware-name` on it.
- No public project drives QCA6390's BHI by hand: every SM8250+QCA6390 bring-up found goes through
  ath11k/MHI. The QCN9074 mailing-list case in section 2 is therefore the only public
  "transfer-completed-then-rejected" datapoint, and it is the same MHI/BHI block and the same code.

## 5. This tablet's own Linux port as a reference

`svesnav/linux-dby-w09` (mainline Linux 7.0, hardware table: "Wi-Fi, Bluetooth (QCA6390) - work") is the only
implementation known to work on this exact board. Its device tree (`sm8250-huawei-dby-w09.dts, line 137-160`) states
the wiring outright - "stock cnss-qca6390 uses WLAN_EN on GPIO 20, BT_EN on GPIO 21, and pm8150 s6 (0.95 V),
s4 (1.8 V), s5 (1.9 V), pm8150l s8 (1.35 V) - the same as the Xiaomi Pad 5 Pro" - and models the part as
`qcom,qca6390-pmu` with `wlan-enable-gpios = <&tlmm 20>` and the four rails. Its build enables
`POWER_SEQUENCING_QCOM_WCN` and `PCI_PWRCTRL_PWRSEQ` plus `MHI_BUS`, `QRTR`, `QRTR_MHI`, `ATH11K`,
`ATH11K_PCI` (`configure-and-build.sh:88-90`), and its README records that the QCA6390 stack is installed on
the SD card and that **firmware and vendor files are copied from the tablet itself**, i.e. it uses this
unit's own `modem` partition images - the same `amss20.bin` this probe hands to BHI.

**Correction, added after a second port was read.** That paragraph's conclusion was wrong, and the source was
the other DBY-W09 port (`youzjuzi/dby-w09-linux`, `docs/firmware-preparation.md`). Its firmware table says the
Wi-Fi main image comes from **public linux-firmware**, `ath11k/QCA6390/hw2.0/amss.bin`, with the matching
public `m3.bin`, and only the board file is taken from the tablet (`/vendor/etc/wifi/dby_fem_lx9_bdwlan.bin`
installed as `board.bin`). It then states the point outright: *"原厂 modem 内的 amss20.bin 也不等于已经验证
适用于当前 ath11k 的 amss.bin"* - the vendor `amss20.bin` inside the modem partition is **not** the `amss.bin`
verified for ath11k. The `svesnav` port is consistent: its Wi-Fi firmware comes from the distribution's
`linux-firmware` too, and the only files it copies from the tablet are GPU zap, SLPI, ADSP and Venus
(`kubuntu-dby-w09/device-files/firmware.txt`).

So the artefact this probe has been handing to BHI is not the one the working ports use - although, measured,
the difference turned out to be a **different build of the same schema, not a malformed file** (see below), so
"wrong artefact" is a weaker claim than it first looked and the vendor's "ELF header is bad" must be read as
"this is not the image I expected", not "this is not an ELF". Measured here: the tablet's `amss20.bin` is 4,352,144 bytes,
ELF32 ARM (e_machine 40), entry `0x16bfe00`, eighteen program headers, a single ELF magic at offset 0; its
`m3.bin` is 266,684 bytes, e_machine 3, entry `0x401`, one PT_LOAD at `0x4ff00000` of `0x40000`. Both are
self-consistent ELF files - which is why every byte-level check on this side passed - and neither is what
ath11k installs at `ath11k/QCA6390/hw2.0/amss.bin`.

**Measured comparison of the two artefacts** (public firmware fetched 2026-11-24 from
`pop-os/linux-firmware@4a0e59f9`, byte-identical at `endlessm/linux-firmware@master`, so unchanged since 2022):

| | public `ath11k/QCA6390/hw2.0/amss.bin` | this tablet's `modem:/image/qca6390/amss20.bin` |
|---|---|---|
| size | 3,628,672 | 4,352,144 |
| ELF | ET_EXEC, ARM, e_entry `0x16c0e00`, 18 phdrs | ET_EXEC, ARM, e_entry `0x16bfe00`, 18 phdrs, same skeleton |
| build | `WLAN.HST.1.0.1-05266`, 2021-11-04 | `WLAN.HST.1.0.1.r1-01083-SILICONZ-1.447466.2.472352.2` |
| `\x7fELF` offsets | `0x0` only | `0x0` only |
| bytes at `0x80000` | ordinary code | ordinary code |
| md5 | `682f7ca2e0b3ea16644c3585772a2cba` | `a828dd0bc8d821f8d1a25c4abf05d445` |

Three facts follow, and the first one is the important one: **neither file is an "SBL + firmware" container** -
there is no second ELF at `0x80000` in either, so mainline's fbc `sbl_size = 512 KiB` slice cannot be a
boot-loader image in the usual sense for this part. Second, **512 KiB is not enough to hold the image either**:
fifteen populated PT_LOAD segments run to the exact end of file, ten of them start at or beyond `0x80000`, and
one straddles it, so a loader that honours PT_LOAD needs the whole file. Third, the vendor image is the *newer*
build (ucode `2.0.15` against the public `2.0.7`, m3.bin differing in 196,920 of 266,684 bytes), which is what
one expects from a 2023 tablet firmware against a 2020 driver.

What remains untested on the host side is therefore the image itself: fetch the public QCA6390 `amss.bin` (and
the `m3.bin` that ships beside it), and hand its first 512 KiB to BHI. A sparse clone of
`git.kernel.org/.../linux-firmware` is the way to get it (git reaches that host from this WSL; plain HTTPS does
not), and the file is needed before any of the driver-side hypotheses below can be judged - although ASPM and
the pwrseq timing remain independently justified by upstream code.

## 6. Ranked next experiments, each with the observable that decides it

0. **Hand BHI the public QCA6390 `amss.bin`, not the tablet's `amss20.bin`.** This is now first because a
   second working port on the same tablet says the vendor image is not the artefact ath11k verifies, and
   because the vendor's reading of `0xef128100` is an image-header rejection. Observable: a different error
   code, or an accepted transfer followed by READY. Fetching the file needs a sparse clone of linux-firmware
   over git (HTTPS to git.kernel.org is blocked from this WSL; git is not).
1. **Disable ASPM and L1 substates on the endpoint before the doorbell.** Upstream does it with an explicit
   warning that not doing it causes firmware-download failures. Observable: the constant 414.5 ms becomes
   variable, or `BHI_ERRCODE` changes, or the transfer succeeds. This is the strongest single hypothesis on
   the table and the only one that is both upstream-mandated and untried here.
2. **Adopt the pwrseq timing**: rails, then clock, then WLAN_EN, then **60 ms**, with 100 ms spacing between
   GPIO enables. Observable: a different code or a different elapsed time; an unchanged 414.5 ms would mean
   the failure is downstream of power.
3. **Sweep `BHI_IMGSIZE`** from 512 bytes to 512 KiB and time each attempt. A constant elapsed time means PBL
   fails before consuming the payload (header check); a scaling one means it really is reading, which would
   put the fault in content or address translation rather than in the descriptor.
4. **Prove the device reads the buffer at all**, the way the sibling port did: put stream `0x1c01` in a
   translated domain that maps only the image buffer, or leave one page unmapped, and watch the SMMU fault
   registers (already instrumented, `m6n`). A fault at the buffer proves the read happened; silence means the
   SID/ATU path is not what the probe believes, and the "correct bytes in DRAM" evidence is about the host's
   copy only.
5. **Compare `BHI_ERRDBG2/3` across two very different images.** Identical values for a 266 KB and a 4.3 MB
   image would say PBL latches the descriptor and rejects on something common to both; differing values would
   say it parsed further into one of them.
6. **Compare against a stock boot.** `TCSR_PBL_LOGGING`, `PBL_BOOTSTRAP_STATUS` and `PBL_WLAN_BOOT_CFG` read
   after a HarmonyOS boot of the same tablet (which brings Wi-Fi up) versus after this probe's failure would
   separate "a host-programmed input is wrong" from "a hardware strap is wrong".

Deprioritised on this evidence: programming CRCBAP/CCABAP/ECABAP before BHI (upstream orders them after
READY), `BHI_INTVEC` (upstream is 0, already tested both), and image content (a working driver on this board
reads the same partition).

## 7. Bluetooth plan, from the same evidence

Order: the same power chain as Wi-Fi (rails, clock, 60 ms, BT_EN on GPIO21), then UART6 at 3 Mbaud, then the
`htbtfw20.tlv` download with `htnv20.bin` NVM from the `bluetooth` partition, then HCI, then the iOS
`AppleBluetooth` side. Two additions from the community record: check the `VDD_ASD` (PM8150 L16) rail, and do
not let two components drive BT_EN.

## 8. Sources

- ath11k list, QCN9074 `0xef128100`: https://lists.infradead.org/pipermail/ath11k/2022-June/003285.html
- ath11k list, Qualcomm reply "the ELF header is bad": https://lists.infradead.org/pipermail/ath11k/2022-July/003305.html
- ASPM/L1SS disable, Carl Huang: https://lists.infradead.org/pipermail/ath11k/2020-December/000914.html
- pwrseq driver (timings): https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/power/sequencing/pwrseq-qcom-wcn.c
- ath11k PCI/power-up: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/net/wireless/ath/ath11k/pci.c
- MHI boot/init/pm: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/bus/mhi/host/boot.c
- CAF QCA6390 PBL dump offsets: https://github.com/hamjin/kernel_xiaomi_sm8250/commit/a1fe641482ee7a684550bd6ff83cf3ff221fe5e6
- Xiaomi Pad 5 Pro DTS: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi
- Xiaomi Pad 5 Pro bring-up record (board.bin, VDD_ASD): https://github.com/Huabin1010/dagu-mainline-linux
- This tablet's working port: https://github.com/svesnav/linux-dby-w09 (local clone `/root/dby-linux`)
- Companion brief with upstream `file:line` for everything in section 3: `bhi-ef128100-brief.md`
