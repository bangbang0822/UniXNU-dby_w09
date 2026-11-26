# UniXNU charging baseline over USB CDC-ACM

- Date: 2026-11-26 14:51 Asia/Shanghai
- Host port: COM3, enumerated from USB `1209:316D` (`QCOMXNUUSBDEBUG`)
- Capture method: open the COM port with Win32 `CreateFile`, assert DTR and RTS using `EscapeCommFunction`, then read for 20 seconds; no bytes were written to the device.
- Capture size: 281,873 bytes; 110 battery-cycle rows, including four complete (tier 1) samples.
- Raw capture: `C:\Users\BangBang\Downloads\AndroidPlatformTools\Logs\charging-console-COM3-20261126-145155.log` (local, not committed)
- Running payload hash: not captured; do not treat this log as proof of the current source revision.

## Complete battery samples

```text
DbyBattery: #1 tier=1 op=1 tx=26 clean=26 to=0 err=0 why=0000000000000000 soc=6 mv=3661 ma=-127 plug=1 on=1 sus=0 en=1 st=3 cold=0 hot=0 chg=1 full=0 cfg=0x00 iclc=1500 iclm=500 icl=500 flt=4450 want=500 apst=17 aprd=0x01 rt=0x10 step=0 good=0 ph=2 latch=0 ceiling=500 aicl0=500 snk=0 reran=1 ib+=0 ib-=1 w[op=0 ops=1 ok=1 to=0 er=0 deny=0]
DbyBattery: #31 tier=1 op=15 tx=25 clean=25 to=0 err=0 why=0000000000000000 soc=6 mv=3573 ma=527 plug=1 on=1 sus=0 en=1 st=3 cold=0 hot=0 chg=1 full=0 cfg=0x00 iclc=1500 iclm=500 icl=500 flt=4450 want=500 apst=17 aprd=0x01 rt=0x10 step=0 good=0 ph=6 latch=0 ceiling=500 aicl0=500 snk=0 reran=1 ib+=30 ib-=1 w[op=0 ops=0 ok=0 to=0 er=0 deny=0]
DbyBattery: #62 tier=1 op=1 tx=25 clean=25 to=0 err=0 why=0000000000000000 soc=5 mv=3566 ma=528 plug=1 on=1 sus=0 en=1 st=7 cold=0 hot=0 chg=1 full=0 cfg=0x00 iclc=1500 iclm=500 icl=500 flt=4450 want=500 apst=17 aprd=0x01 rt=0x10 step=0 good=0 ph=6 latch=0 ceiling=500 aicl0=500 snk=0 reran=1 ib+=60 ib-=2 w[op=0 ops=0 ok=0 to=0 er=0 deny=0]
DbyBattery: #93 tier=1 op=1 tx=25 clean=25 to=0 err=0 why=0000000000000000 soc=5 mv=3559 ma=529 plug=1 on=1 sus=0 en=1 st=7 cold=0 hot=0 chg=1 full=0 cfg=0x00 iclc=1500 iclm=500 icl=500 flt=4450 want=500 apst=17 aprd=0x01 rt=0x10 step=0 good=0 ph=6 latch=0 ceiling=500 aicl0=500 snk=0 reran=1 ib+=91 ib-=2 w[op=0 ops=0 ok=0 to=0 er=0 deny=0]
```

## Reading

- `package/DbyBattery.cpp` defines negative `ma` as discharging; the three later complete samples therefore show positive battery charge current around 0.53 A after the boot transient.
- `[charger] port=sdp` and `iclm=500 icl=500 want=500` identify this as the host's 500 mA SDP path. It is not an SCP or 40 W observation.
- The complete rows have no field-read errors, but this does not identify the SCP/direct-charge controller or prove an SCP protocol implementation.
- The boot begins with `ma=-127 mA`; the current limit path reported one successful ICL operation on sample 1. This is existing behavior, not a change made in this capture session.

## Adapter excursion and post-reboot host baseline

The owner reports running UniXNU on the 40 W adapter for about one minute, then reconnecting the tablet to this PC. Windows failed to enumerate the CDC device on two host ports (`Code 43`, USB descriptor request failed); COM3 returned only after a device reboot. The adapter-period buffer was therefore not recovered. No SCP, negotiated voltage, or 40 W input measurement is claimed.

After reboot with the PC connected, a 20-second capture contained 15 current-boot battery rows. The first complete row was `soc=0 mv=3442 ma=-117` with `plug=1`, `chg=1`, `want=500`; rows #2–#15 reported `ma=+430..+690 mA`, still `port=sdp`, `want=500 mA`. A following 65-second capture contained touch/system lines but no further `DbyBattery` rows, so it does not provide a later full-tier sample. Raw files are local:

- `C:\Users\BangBang\Downloads\AndroidPlatformTools\Logs\charging-reboot-COM3-20261126-151003.log`
- `C:\Users\BangBang\Downloads\AndroidPlatformTools\Logs\charging-steady-COM3-20261126-151126.log`

The next 40 W measurement needs a way to keep USB CDC data connected while the adapter supplies power, or another supported log transport. Repeating a charger-to-PC cable swap risks losing the buffered interval again.

## Follow-up after the next reboot

After the owner reported another reboot, COM3 enumerated as `1209:316D` and a 75-second read-only capture ran with DTR and RTS asserted. It contained 55,297 bytes, 264 `DbyTouch` rows and 60 `DbyButtons` rows, but no `DbyBattery` or `[charger]` rows. The touch and button counters advanced throughout the capture (`pub=6336..23168`, `poll=11500..41000`), so the CDC stream was active. This confirms that the battery telemetry was absent from that capture; it does not distinguish a service/workloop stall from a different running image or a logging-path issue. The running payload hash is still unknown, so no source-level cause is assigned.

Raw capture (local, not committed): `C:\Users\BangBang\Downloads\AndroidPlatformTools\Logs\charging-after-reboot-COM3-20261126-151653.log`.

## Alternate host USB-C port capture

After the owner moved the tablet cable to another USB-C port on the PC, Windows enumerated the debug interface as COM3 (`1209:316D`, `QCOMXNUUSBDEBUG`) and the tablet USB composite interface (`12D1:10DB`). A 20-second read-only capture asserted DTR/RTS and sent no bytes. The raw capture is local, not committed:

- `C:\Users\BangBang\Downloads\AndroidPlatformTools\Logs\charging-new-port-COM3-20261126-154006.log` (397,303 bytes)

The stream contains replayed console history as well as current output; the current `DbyBattery` sequence starts at `#1`. Its first complete row reports `soc=0`, `mv=3330`, `ma=-993 mA`, `plug=1`, `chg=1`, and `aicl0=1400 mA`. Later current rows classify the input as APSD `dcp` (`aprd=0x08`), request `1500 mA` against an `aicl0=1400 mA` reading, and report battery current roughly `-560..-746 mA`. Most other fields in those later rows are unknown (`why=7700000777700077`); ICL readback is unavailable. This suggests the PC port did not keep the battery charging under the current system load, but the log does not include VBUS voltage or a PD contract, so it cannot establish actual input power or rule in/out an unreported Type-C contract.

No `PDO`, `RDO`, `TCPM`, or negotiated voltage/current line appeared. APSD's `DCP` label is not a USB-PD contract result. This capture is a host-port baseline only, not a PD charger test. With the battery at 0% in the first full sample and measured net discharge, use a suitable external charger rather than relying on this PC port to replenish the battery.
