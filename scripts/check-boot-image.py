#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check that a boot image contains what the build reports said it would.

The boot and payload steps each write a report, and a flash is the first time anyone finds out whether
the image on disk matches it. This reads the image itself: the Android header, the appended device tree,
and the payload volume's contents - the kernel, the trust cache, the data seed and the boot arguments -
and compares them with the hashes and the arguments the reports recorded.

    scripts/check-boot-image.py --image out/boot.img --report out/boot.json --payload-report <payload>.json
"""
import argparse
import zlib
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

BOOT_MAGIC = b"ANDROID!"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--report", type=Path, help="the boot step's boot.json")
    parser.add_argument("--payload-report", type=Path, help="the payload step's payload.json")
    parser.add_argument("--base-dtb", type=Path,
                        help="the base device tree the UEFI device config names ([boot_image] dtb)")
    parser.add_argument("--mdir", default="mdir", help="mtools entry point used to read the payload volume")
    parser.add_argument("--mcopy", default="mcopy")
    args = parser.parse_args()

    problems = []
    image = args.image.read_bytes()
    if image[:8] != BOOT_MAGIC:
        print("FAIL not an Android boot image", file=sys.stderr)
        return 1
    kernel_size = struct.unpack_from("<I", image, 8)[0]
    ramdisk_size = struct.unpack_from("<I", image, 16)[0]
    header_version = struct.unpack_from("<I", image, 40)[0]
    page = 2048 if header_version < 2 else 4096

    def page_up(value):
        return (value + page - 1) // page * page

    offset = page
    kernel = image[offset:offset + kernel_size]
    offset += page_up(kernel_size)
    ramdisk = image[offset:offset + ramdisk_size]
    offset += page_up(ramdisk_size)
    print(f"header v{header_version}: kernel {kernel_size} bytes, ramdisk {ramdisk_size} bytes")

    # The device tree sits after the ramdisk, page aligned. Its size field moves between mkbootimg versions (this
    # one keeps it at 1648, not at the 1624 of the header version 2 in AOSP), so the magic decides where it starts
    # and the FDT's own total-size field decides how long it is.
    start = offset
    found = image.find(b"\xd0\x0d\xfe\xed", start - page, len(image))
    if found < 0:
        problems.append("no device tree follows the ramdisk")
    else:
        dtb_size = struct.unpack_from(">I", image, found + 4)[0]
        dtb = image[found:found + dtb_size]
        if dtb_size < 64 or len(dtb) != dtb_size:
            problems.append("the device tree after the ramdisk is truncated")
        else:
            digest = sha256(dtb)
            print(f"device tree at {found}: {dtb_size} bytes, {digest[:16]}")
            # Two device trees exist in a build: the board's own compiled tree, which the step appends to the
            # kernel field, and the base tree the UEFI device config names for the header. Either is legitimate
            # here; which one the image carries is worth printing, and neither may be a third thing.
            expected = []
            if args.report:
                report = json.loads(args.report.read_text())
                if report.get("dtb_sha256"):
                    expected.append(report["dtb_sha256"])
            base = sha256(args.base_dtb.read_bytes()) if args.base_dtb and args.base_dtb.is_file() else None
            if base:
                expected.append(base)
            if expected and digest not in expected:
                problems.append("the device tree is neither the board's compiled tree nor the base tree the "
                                "device config names")
            elif digest == base and base != (expected[0] if expected else None):
                print("note: the header carries the base device tree from the UEFI device config, not the board's "
                      "compiled tree (which is appended to the kernel field instead)")

    # the kernel field is the boot shim plus the firmware volume, gzipped, with the device tree appended
    body = kernel
    try:
        # the field is gzip(shim + firmware volume) followed by the device tree the bootloader reads
        decompressor = zlib.decompressobj(31)
        body = decompressor.decompress(kernel) + decompressor.flush()
        trailing = len(kernel) - len(decompressor.unused_data) - (len(kernel) - len(body) - len(decompressor.unused_data))
    except zlib.error:
        problems.append("the kernel field is not gzip, so the shim + firmware volume could not be checked")
    else:
        print(f"shim + firmware volume {len(body)} bytes")
        if args.report and args.report.exists():
            report = json.loads(args.report.read_text())
            if report.get("fd_sha256"):
                fd = body[192:]
                if sha256(fd) != report["fd_sha256"]:
                    problems.append("the firmware volume inside the image does not match the boot report")

    # the ramdisk is the payload volume
    temp = Path("/tmp/check-boot-image") if Path("/tmp").is_dir() else Path(".")
    temp.mkdir(exist_ok=True)
    volume = temp / "payload.img"
    volume.write_bytes(ramdisk)
    listing = subprocess.run([args.mdir, "-i", str(volume), "-/", "::/"], capture_output=True, text=True)
    if listing.returncode:
        problems.append(f"the ramdisk is not a readable payload volume: {listing.stderr.strip()[:120]}")
    payload_report = json.loads(args.payload_report.read_text()) if args.payload_report else {}

    wanted = ["EFI/UniXNU/Payloads/kernel.macho", "EFI/UniXNU/unixnu.toml"]
    copied = {}
    for entry in wanted:
        target = temp / Path(entry).name
        result = subprocess.run([args.mcopy, "-i", str(volume), f"::{entry}", str(target)],
                                capture_output=True, text=True)
        if result.returncode:
            problems.append(f"{entry} is missing from the payload volume")
        else:
            copied[entry] = target

    if "EFI/UniXNU/Payloads/kernel.macho" in copied:
        digest = sha256(copied["EFI/UniXNU/Payloads/kernel.macho"].read_bytes())
        print("payload kernel", digest[:16])
        expected = payload_report.get("payload_kernel_sha256")
        if expected and digest != expected:
            problems.append("the kernel in the payload does not match the payload report")

    if "EFI/UniXNU/unixnu.toml" in copied:
        configuration = copied["EFI/UniXNU/unixnu.toml"].read_text(errors="replace")
        arguments = payload_report.get("kernel_args")
        if arguments:
            missing = [token for token in arguments.split() if token not in configuration]
            if missing:
                problems.append(f"the payload's unixnu.toml is missing arguments: {missing[:4]}")
            else:
                print(f"all {len(arguments.split())} boot arguments are in the payload's unixnu.toml")
        if "pmgr=2" not in configuration:
            problems.append("the payload does not carry pmgr=2, which the payload step requires")
        if "sheng-pmgr=2" in configuration:
            problems.append("the payload still carries the pre-rename sheng-pmgr marker")

    # the two inputs the payload embeds verbatim: the packed Data seed and the trust cache bundle
    for entry, digest, what in (("EFI/UniXNU/Payloads/scratch-data.img", payload_report.get("data_ramdisk_sha256"),
                                 "the Data seed"),
                                ("EFI/UniXNU/Payloads/kernel.macho.trustcache",
                                 payload_report.get("trust_cache_sha256"), "the trust cache")):
        if not digest:
            continue
        target = temp / Path(entry).name
        result = subprocess.run([args.mcopy, "-i", str(volume), f"::{entry}", str(target)],
                                capture_output=True, text=True)
        if result.returncode:
            problems.append(f"{what} is missing from the payload volume ({entry})")
        elif sha256(target.read_bytes()) != digest:
            problems.append(f"{what} in the payload does not match the payload report")
        else:
            print(f"{what}: {target.stat().st_size} bytes, matches the report")

    if problems:
        for problem in problems:
            print("FAIL", problem, file=sys.stderr)
        return 1
    print("PASS boot image: header, device tree, payload kernel and boot arguments agree with the reports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
