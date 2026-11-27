# DBY-W09 QCA6390 Wi-Fi development

Snapshot: 2026-11-28, device tree `6992b639`. This plan follows the tablet's own PCIe and firmware observations. The current `dby-pcie=10` is a bring-up probe, not a Wi-Fi service.

## Current boundary

The tablet's endpoint has been read as Qualcomm `17cb:1101` over PCIe0. `package/DbyPcie.cpp` has brought up the controller, trained the link, mapped config/BAR windows, prepared an SMMU/DMA arena and read the tablet's FAT16 `modem` volume. The firmware path currently takes `image/qca6390/amss20.bin` (4,352,144 bytes) to BHI. The recorded device run returned `bhi status=0`, `ee=0` and no image transition; subsequent experiments reported BHI rejection `0xef128100`. No MHI mission mode, QMI, ath11k MAC, scan, association or iPadOS network service has been demonstrated. See `porting.toml` wireless findings/evidence and `docs/wireless-research-2026-11-24.md`.

Two same-tablet Linux ports use the public `ath11k/QCA6390/hw2.0/amss.bin` with matching `m3.bin`, taking only board data from the tablet. The vendor `amss20.bin` and public `amss.bin` are different, both structurally valid ELF images; the public image is a testable candidate, not a proven cure on this UniXNU build. Keep both files and their hashes outside Git.

## What upstream provides

The [UniXNU Wi-Fi guide](https://unixnu.com/porting/network) separates PCIe/MSI/DMA, firmware/MHI, and the OS network interface. Its public `Ath12kWifi` component lists ath12k PCI devices (QCC2072, QCN9274 and WCN7850); public `components.toml` does not list a QCA6390/ath11k implementation. Therefore its `nw_abi.h` callback contract and `WifiProbe.cpp`/`WifiNet.cpp` are useful interfaces, but selecting that component unchanged would misidentify this endpoint. [j716f](https://unixnu.com/devices/j716f), another SM8250 tablet on iPadOS 17.7.11, records a working QCA6390 path; inspect its exact fork, OS ABI and board inputs before borrowing code.

## Work packages and gates

### W0. Lock down the firmware experiment

Record SHA-256, length, ELF headers and source revision for public `amss.bin`, public `m3.bin` and this tablet's selected board file. Compare the BHI transfer contract and image length against the chosen source driver; do not assume a 512 KiB SBL slice merely because another MHI controller uses firmware download by segments. Host-check buffer bounds, physical address, cache synchronization, IOVA reachability and that the bytes at the bus-visible address equal the selected file. Make the firmware selection the only variable in the next reviewed image. Acceptance is a transition out of PBL with BHI status and execution environment recorded; rejection retains its error registers and does not get called a driver failure.

### W1. Prove endpoint DMA and interrupts

Before relying on an interrupt-driven worker, read the endpoint's real PCI capabilities and verify MSI delivery. Review the current identity/bypass SMMU path against the device tree's `qcom,smmu-s1-enable` and WLAN-owned IOVA/TCS registers; do not change translation mode without an address-by-address map and a bounded DMA test. Confirm BAR ownership, bus mastering, cache mode and cleanup on every timeout. This is where the reusable platform callback boundary belongs.

### W2. Reach firmware mission mode

Implement or adapt QCA6390-compatible MHI channel/event/ring setup, BHI/BHIe transition and the firmware reset path. Then prove QRTR/QMI readiness, the M3 handoff and the correct board-data variant. Record each state, timeout and received service identity separately. The `modem` and `bluetooth` FAT volumes are read-only firmware sources; never write or image `userdata`.

### W3. Expose Wi-Fi to iPadOS 17

Choose an ath11k-capable reusable driver source, pin it as a component/feed, and adapt the board's PCIe, SMMU, MSI, firmware and power callbacks. Implement an OS-owned ifnet plus the selected iPadOS 17 Apple80211 requests and link/event path; only publish a `wlan` node when a real provider answers. Test scan, WPA2 association, DHCP, sustained RX/TX and disconnect/reconnect in that order. A visible Settings switch, a trained PCIe link or firmware mission mode alone does not meet this gate.

## Build and device discipline

Build the kernel from a clean resolved tree and inspect the archive provider and final linked symbols. Record source pins, firmware hashes, kernel/payload SHA-256 and the last booted rollback payload. Review every code and boot-argument change with the four questions in `AGENTS.md`, update `porting.toml` wireless/network areas in the same push, and flash one variable at a time only after owner approval. The existing `wireless` record's `limits` and `next_action` still describe M1 even though its findings reach BHI; reconcile those fields before using the record as a gate.

References: [Wi-Fi adaptation](https://unixnu.com/porting/network), [j716f QCA6390 record](https://unixnu.com/devices/j716f), [component catalog](https://unixnu.com/reference/components), `docs/wireless-research-2026-11-24.md`, `porting.toml` wireless area.
