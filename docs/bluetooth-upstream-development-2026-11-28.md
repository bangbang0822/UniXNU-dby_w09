# DBY-W09 QCA6390 Bluetooth development

Snapshot: 2026-11-28, device tree `6992b639`. The radio identity is supported by this tablet's QCA6390/CNSS device tree and vendor image. Bluetooth transport wiring on **this** tablet is not yet device-verified.

## Upstream fit and local evidence

The [UniXNU Bluetooth guide](https://unixnu.com/porting/bluetooth) uses a GENI UART driver, Qualcomm firmware initialization, H4/IBS framing and Apple's UART.SK transport. The current public `QcaBluetooth` catalog entry names WCN7850/GENI and iPadOS 27/macOS 27 ABIs, with sheng as its verified device. It is a reference for lifecycle, DMA ownership and the native-stack boundary, not an unchanged iPadOS 17/QCA6390 component.

The [j716f record](https://unixnu.com/devices/j716f) reports QCA6390 Bluetooth on SM8250 and iPadOS 17.7.11 using a GENI UART, BlueTool and `bluetoothd`. Its UART base, interrupt, pins, board data and firmware must not be copied into DBY-W09 without this tablet's DTS or a measured register response.

This tablet's `bluetooth` FAT volume was read in the existing wireless bring-up. It contains `htbtfw10.tlv`, `htbtfw20.tlv` and several `*htnv20.bin` board variants, including `dby_htnv20.bin`, `dby_ddr_htnv20.bin` and `dby_fem_htnv20.bin`. The tablet's source evidence identifies BT_EN on GPIO21, but the current `porting.toml` says the Bluetooth transport itself is unidentified. The UART6/3 Mbaud suggestion in `docs/wireless-research-2026-11-24.md` is a hypothesis from related ports, not a DBY-W09 hardware result. No Bluetooth component, node, firmware downloader or HCI transport is currently selected in `board.toml`.

## Execution sequence

### B0. Identify the board's actual transport

Read the tablet's full stock DT/DTBO, UART ownership, pinmux, clock, IRQ, SMMU stream and wake line. Reconcile the UART6 hypothesis with the active vendor DT and stock driver. Pin the exact firmware and NVM variant by board identity and SHA-256, keeping files outside Git. Identify where the Bluetooth address is stored; do not synthesize an address while the `oeminfo` partition remains only partly inspected. Gate: a source-backed resource sheet and a read-only controller probe, with no power or pin writes.

### B1. Establish shared power and UART

Model Wi-Fi and Bluetooth as separate owners of common WCN rails. A reference-counted vote must leave the supplies on while either radio is active; BT_EN has one owner. Confirm reset/power sequencing, UART protocol microcode, serial clock, pin pulls and IRQ before firmware download. Implement bounded RX/TX DMA with explicit cache sync, backpressure and cleanup on every failure. Gate: stable H4 bytes and repeatable power off/on, with no Wi-Fi regression.

### B2. Load firmware and prove HCI

Select the `htbtfw20.tlv`/matching `htnv20.bin` pair only after checking the controller's reported revision. Record the vendor initialization response, negotiated baud, controller address and HCI reset/version commands. Test packet framing, RX flow control, timeout and reconnect. A successful firmware transfer without HCI command/response is an intermediate result.

### B3. Connect iPadOS 17

Qualify the iPadOS 17 UART.SK and AppleBluetooth ABI against the selected 21H461 kernelcache and userland. Publish a `bluetooth` node, address and service only after B2 passes. Stage the matching signed BlueTool/`bluetoothd` pieces and owner trust inputs from the same build. Check Settings visibility, scan, pairing, reboot reconnection and coexistence with active Wi-Fi. Audio profiles require a working audio route and are a separate acceptance gate.

## Review and acceptance

For each flash, change one variable; retain the last booted payload hash. Review the power vote's other owner, DMA cache mode, UART IRQ lifecycle and boot arguments before building. Run repository, record and pre-flash regression checks, then record host results and tablet observations separately in `porting.toml`. Do not declare Bluetooth working from a node, a toggle or a firmware file listing.

References: [Bluetooth adaptation](https://unixnu.com/porting/bluetooth), [QcaBluetooth](https://unixnu.com/drivers/qca-bluetooth), [j716f](https://unixnu.com/devices/j716f), `docs/wireless-research-2026-11-24.md`, `porting.toml` wireless area.
