# DBY-W09 Huawei private charging protocol preparation

Snapshot: 2026-11-28. This is preparation for a later implementation round. It records source findings, required evidence and host-side work. It does not add a charger write, request a higher voltage or change the current PMIC policy.

## Current conclusion

The DBY-W09 official rating is 10 V / 2.25 A, 22.5 W. Huawei's public specification also lists compatibility with 9 V / 2 A and 5 V / 2 A. A 40 W adapter rating does not show that this tablet accepts a 40 W contract. The first target is Huawei private protocol identification and safe negotiation up to the tablet's verified limit. A 40 W request stays unqualified until the tablet, charger path, battery limits and thermal behavior provide direct evidence.

The same-tablet Linux port is the best public preparation source found so far. Its dby-chg driver manages PM8150B watchdog health, input-current control and fixed PD 9 V. It reports a default 2.5 A input ceiling, a configurable pd9v_max_ma range of 1.5–3 A, thermal derating and a low-battery current cap. Its status says Huawei SCP/PPS is unsupported and that a PC USB source remains around 500 mA. The Linux implementation shows how to keep the charger watchdog alive and how to test fixed PD, but it does not provide a private Huawei protocol implementation.

## Material collected

- Official DBY-W09 rating and supported input modes are recorded in docs/charging-usb-pd-spec-2026-11-26.md.
- package/DbyBattery.cpp maps PM8150B SMB5 and fuel-gauge reads, APSD/AICL state, Type-C sink status, ICL limits and the existing guarded write tier.
- The current UniXNU code has no verified SCP/FCP state machine, protocol packet trace, Source Capabilities decoder, RDO/Accept/PS_RDY telemetry or negotiated VBUS field.
- The same-tablet Linux source is pinned for research at youzjuzi/dby-w09-linux@3d82fb7de89986520b5c0ce21aea88044b682a23.
- The Linux README says the watchdog is serviced through register 0x1643 and charging control uses 0x1042. Its recorded behavior includes a watchdog cutoff after about 128 seconds when the driver is unloaded. These values are research inputs, not a permission to copy writes into UniXNU.

References: https://consumer.huawei.com/cn/support/content/zh-cn15905569/, https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/drivers/dby-chg/README.md, https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/docs/status.md, https://github.com/youzjuzi/dby-w09-linux/blob/3d82fb7de89986520b5c0ce21aea88044b682a23/drivers/dby-chg/dby-chg.c.

## Preparation before the cold build finishes

### H0. Keep the device alive

Use the original charger or a known-good 22.5 W charger in the stock system when the battery is low. Do not use a low-current PC USB port as the recovery charger. Preserve the last booted payload and its hash before a later charging image is considered.

### H1. Prepare the source and evidence set

Keep the Linux repository revision, stock DT/DTBO revision, PM8150B register provenance, charger model, cable model, firmware version and battery state in one private experiment directory. Extracted firmware, calibration data, owner identity and raw protocol captures stay outside Git.

Prepare a USB-PD analyzer or meter that records VBUS and current, plus a source that advertises fixed 5 V, 9 V and the original Huawei charger. A power meter without CC or private-bus capture can measure the result but cannot identify the protocol.

### H2. Capture the stock protocol

In the stock OS, capture attach, source capabilities, sink request, accepted voltage and current, VBUS transitions, charger input current, battery current, temperature, SOC and detach recovery. Repeat with the original Huawei charger, a 40 W Huawei SCP charger and an ordinary USB host. The 40 W charger experiment answers whether the tablet requests 40 W or deliberately settles at its rated 22.5 W. It must not be treated as a target before the trace proves the request.

### H3. Add read-only UniXNU telemetry first

After the reproducible build, add only fields that can be sourced from PM8150B or a verified Type-C owner. Keep unknown distinct from zero. The minimum row is VBUS, input_ma, battery_ma, soc, temp, suspend, enable, APSD, AICL, ICL, sink_status, contract_voltage, contract_current, fault. Do not label APSD DCP as PD or SCP.

The first UniXNU charging image must prove sustained positive battery current at 5 V, then at a verified fixed 9 V contract. It must survive unplug, replug, refusal and temperature derating without a new PMIC write.

## Implementation gate after the build

1. Identify the PD/Type-C owner and the private protocol transport from stock DT, firmware and traces.
2. Match the transport to PM8150B and iPadOS 17.7.11. The public UniXNU sheng UCSI path is not a drop-in for this board.
3. Implement a read-only state machine and packet decoder with a 5 V fallback.
4. Add request logic only for a source and sink capability proven by the trace.
5. Clamp the request by the minimum of verified sink capability, PMIC input limit, cable/source limit, battery and thermal limits, and 22.5 W.
6. Test refusal, timeout, detach, VBUS collapse, low battery, hot battery, cold start and long-term charge. Record measured VBUS × input current separately from battery current.

The implementation is complete only when a trace, external meter, UniXNU row and SOC trend agree. A successful charger icon, a high ICL register or a 40 W adapter label is not sufficient.

## Files to add after the baseline

- package/DbyBattery.cpp or a matched shared charger component for the verified read-only telemetry and policy boundary.
- A private local capture manifest outside Git with hashes and equipment details.
- A porting.toml power evidence entry for each source revision, payload hash and hardware observation.
- A review entry that names the PMIC owner, shared register consumers, fallback behavior, host checks and exact charging row that decides success.