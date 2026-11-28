# DBY-W09 standard USB-PD development

Snapshot: 2026-11-28, device tree `6992b639`. The owner's selected scope is standard USB Power Delivery with fixed PDOs and an input ceiling of 22.5 W. The active detailed safety and acceptance specification is `docs/charging-usb-pd-spec-2026-11-26.md`; this document adds the upstream fit and an execution order. It does not authorize a charge-control change or a flash.

## Current implementation and upstream boundary

`package/DbyBattery.cpp` reads PM8150B SMB5/fuel-gauge and Type-C status, publishes an `IOPMPowerSource`, and applies an APSD/AICL-based input-current ladder under `dby-battery=2`. An ICL request is not a PD Request Data Object. No current DBY-W09 log identifies Source_Capabilities, Sink_Capabilities, RDO, Accept, PS_RDY or a negotiated VBUS voltage/current. The 2026-11-26 CDC baseline saw a 500 mA SDP host path and a later DCP-labelled host-port path with incomplete readings; neither is evidence of PD charging. The running payload hash was not captured for those logs.

The [UniXNU power guide](https://unixnu.com/porting/power) says firmware ownership must be established before host charger control. Its sheng example uses an SM8550 DSP battery manager for PD/PPS and authentication. Public `QcomDwc3` selects a PM8550B/eUSB2/UCSI path for iPadOS 27. Neither implementation is an established PM8150B/SM8250/iPadOS 17 policy engine. Do not transplant its rail, UCSI or authentication commands. The [same-tablet Linux record](https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/docs/status.md) is a source for candidate ownership and measured behavior, subject to matching this tablet's stock DT and firmware.

## Execution sequence

### First milestone: sustained battery charging

Give this milestone priority over GPU or radio bring-up. A prior USB host capture showed
net discharge with the cable attached, despite an asserted charging flag. If the device is
already low, recover charge with the original charger in the stock OS or while powered off.
Confirm that the battery level rises before resuming UniXNU testing.

Use a known good charger and cable and a safe state of charge for the first comparison.
Match screen load and temperature between runs. Capture external VBUS and input current,
battery voltage and signed net current, SOC, input suspend, charge enable, temperature
and faults over at least ten minutes at idle, then repeat at normal screen load. Keep the
running payload hash. Charge acceptance requires sustained positive battery net current
and a rising SOC trend, including after reconnect. Repeat longer if the SOC display is too
coarse for ten minutes. An attach flag, a single current sample, and a charger icon are
insufficient. Use existing read-only telemetry where possible. Capture the external
charger interval directly because a previous charger-to-PC cable swap lost its CDC logs.

Use the 5 V path to establish a fallback before adding a higher-voltage request. If the
5 V test fails, compare the stock and UniXNU input suspend, current-limit readbacks and
battery current on the same source. Verify the charger classification and confirmed input
limit before changing the current policy.

### P0. Identify the PD owner and limits

Inspect this tablet's stock DT/DTBO and official firmware for the Type-C/PD PHY, TCPM, IRQ, charger manager, sink PDO table and thermal policy. Compare the public DTS record's 5 V/2 A and 9 V/1.5 A sink PDOs with the running stock policy and Linux port's 9 V behavior. Do not infer a 22.5 W contract from the 10 V/2.25 A nameplate. Gate: one source-backed owner diagram from attach through Request/Accept/PS_RDY to SMB5 ICL, with a verified safe fallback.

### P1. Add read-only contract telemetry

Using the owner's official or verified interface, observe attach, source PDOs, sink capability, selected PDO/RDO, contract voltage/current, VBUS, ICL/AICL, battery voltage/current, SOC, temperature and fault flags. Unknown and failed reads must be distinct from zero. Capture unplugged, ordinary host USB and a fixed-PD source, with an external USB-PD meter on the same cable. Gate: contract fields agree with the meter and no charge behavior changes.

### P2. Request a bounded fixed PDO

Only after P0/P1 pass, let the established policy engine choose a PDO within the **verified** sink capability, cable/source limit and 22.5 W ceiling. Set SMB5 ICL no higher than the contract and the tablet's independently verified input limit. Keep PPS/AVS and proprietary protocols outside this scope. Test refusal, detach, timeout, thermal limit and VBUS collapse; each must return to the measured safe 5 V/automatic behavior. A 9 V/1.5 A active sink table caps the defensible request at 13.5 W until a different valid table is proven.

### P3. Qualify power and OS reporting

Measure input voltage/current and battery net current simultaneously under a fixed screen load, battery range and temperature range. Verify charging status, thermal response, low-SOC boot, hotplug and host SDP regression. Keep the existing read-only `dby-battery=1` path as a diagnostic control. Report actual watts only from measured VBUS and input current; PDO advertised power and battery current are different quantities.

## Build and flash gate

Use an identified, previously booted image for read-only baseline measurements. A changed
image needs the reproducible kernel/addon/Data-seed baseline first. Track it in the
[cold build failure log](reviews/cold-build-failure-log.md). For each later PD change,
review the register owner and other consumers, host checks, expected tablet rows and
rollback payload with SHA-256. Update `porting.toml` power evidence in the same push.
Obtain owner approval before flashing. Keep logs off the tablet's `userdata` partition.

References: [UniXNU power adaptation](https://unixnu.com/porting/power), [component catalog](https://unixnu.com/reference/components), `docs/charging-usb-pd-spec-2026-11-26.md`, `docs/charging-baseline-2026-11-26.md`, `package/DbyBattery.cpp`.
