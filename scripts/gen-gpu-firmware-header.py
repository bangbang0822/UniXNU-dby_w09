#!/usr/bin/env python3
"""Generate package/DbyGpuFirmware.h: the DBY-W09 pin table plus the three A650 images, compiled in.

`package/DbyGpuFirmware.h` used to carry three `nullptr` pins because the images were
not in this repository. They are in the reference now: the A650 fork's board package
keeps them as C arrays (`package/GpuFirmwareA650.c`, alias board at f1883782), with the
lengths the upstream A650 catalog records -- gmu 41548, sqe 31632, zap 13964. This
script reads that file, verifies those three lengths, and writes the board header with
the pins filled and the same bytes behind them.

  python3 scripts/gen-gpu-firmware-header.py \
      --source ../gpu-official-route/scratch/alioth/package/GpuFirmwareA650.c

By default the header is written next to this repository's package directory. The
canonical build-tree location is `kits/dby_w09/DbyGpuFirmware.h` (this repository's
`package/` materialises as `kits/dby_w09`), and the file must sit beside
`DbyGpuRegs.h`, which it includes -- so if the DbyGpu group is kept as its own
directory, pass `--out package/DbyGpu/DbyGpuFirmware.h` and delete that directory's
old placeholder instead of keeping both.

--check regenerates into memory and compares, so a reviewer (or CI) can prove the
committed header still matches its source and that nobody hand-edited a byte.
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / 'package/DbyGpuFirmware.h'

# The three pins, in the order kDbyGpuFirmwarePins indexes them (ZAP, GMU, SQE), with
# the C array name the reference uses, the length the A650 catalog records, the file
# name the device tree / upstream catalog gives the part, and the provenance line.
PINS = (
    dict(kind='ZAP', macro='DBY_GPU_ZAP_FIRMWARE_BYTES', symbol='native_gpu_zap_firmware',
         source_name='native_gpu_zap_firmware', bytes=13964, file='a650_zap.elf',
         provenance='reference board package/GpuFirmwareA650.c (A650 fork images), a '
                    'complete ELF32 little-endian ARM image, entry 0x5000; PAS 13'),
    dict(kind='GMU', macro='DBY_GPU_GMU_FIRMWARE_BYTES', symbol='native_gpu_gmu_firmware',
         source_name='native_gpu_gmu_firmware', bytes=41548, file='a650_gmu.bin',
         provenance='reference board package/GpuFirmwareA650.c (A650 fork images), the '
                    'image the 2026-10-31 cold start on the reference board loaded'),
    dict(kind='SQE', macro='DBY_GPU_SQE_FIRMWARE_BYTES', symbol='native_gpu_sqe_firmware',
         source_name='native_gpu_sqe_firmware', bytes=31632, file='a650_sqe.fw',
         provenance='reference board package/GpuFirmwareA650.c (A650 fork images); the '
                    'probe drops its first dword before upload'),
)

ARRAY_RE = re.compile(r'const\s+uint8_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;', re.S)
BYTE_RE = re.compile(r'\b(\d{1,3})\b')
PER_LINE = 16


def read_arrays(path):
    """name -> bytes for every `const uint8_t NAME[] = {...};` in the file."""
    text = Path(path).read_text()
    arrays = {}
    for name, body in ARRAY_RE.findall(text):
        values = [int(v) for v in BYTE_RE.findall(body)]
        bad = [v for v in values if v > 255]
        if bad:
            raise SystemExit(f'{path}: {name} has a value above 255: {bad[0]}')
        arrays[name] = bytes(values)
    if not arrays:
        raise SystemExit(f'{path}: no `const uint8_t <name>[] = {{...}}` array found')
    return arrays


def array_lines(name, data, macro):
    lines = [f'extern const uint8_t {name}[{macro}] __attribute__((weak)) = {{']
    for start in range(0, len(data), PER_LINE):
        chunk = data[start:start + PER_LINE]
        lines.append('\t' + ''.join('0x%02x,' % value for value in chunk))
    lines.append('};')
    return lines


def describe_source(source):
    """How the header names its input: repo-relative when inside the repo, else basename.

    The reference checkout lives outside this repository, and a machine-specific
    absolute path in a committed header would be wrong for every other checkout, so
    the identity of the input is its own digest rather than its location."""
    source = Path(source)
    try:
        shown = source.resolve().relative_to(ROOT.resolve()).as_posix()
    except ValueError:
        shown = source.name
    return shown, hashlib.sha256(source.read_bytes()).hexdigest()


def build_header(source, arrays, pins):
    shown, source_sha = describe_source(source)
    total = sum(p['bytes'] for p in pins)
    columns = []
    for pin in pins:
        data = arrays[pin['source_name']]
        columns.append(dict(pin,
                            sha256=hashlib.sha256(data).hexdigest(),
                            image=pin['symbol'],
                            elf=data[:4] == b'\x7fELF'))
    zap = next(c for c in columns if c['kind'] == 'ZAP')
    text = []
    add = text.append
    add('/* SPDX-License-Identifier: MIT */')
    add('/* DBY-W09 GPU firmware pin table and the three A650 images, compiled in.')
    add(' *')
    add(' * GENERATED by scripts/gen-gpu-firmware-header.py from')
    add(f' *   {shown}')
    add(f' *   sha256 {source_sha}')
    add(' * Regenerate; do not edit. The generator refuses the three recorded lengths')
    add(' * (%s) rather than trusting whatever file it was pointed at, so a truncated'
        % ', '.join(str(p['bytes']) for p in pins))
    add(' * or wrong-part blob cannot quietly become "almost right".')
    add(' *')
    add(' * WHY EMBEDDED: the Adreno bring-up path validates each image by exact byte')
    add(' * count plus SHA-256 before use, and this board has no vendor-firmware reader')
    add(' * in the kernel. The reference board does the same: its GpuProbe.cpp:58-63')
    add(' * weak-imports native_gpu_{gmu,sqe,zap}_firmware and their _size words, so this')
    add(' * header defines both spellings and a port of that probe links unchanged. The')
    add(f' * {total} bytes below cost that much kernel image and nothing at runtime.')
    add(' *')
    add(' * MEASURED, and reproducible from the file above:')
    for column in columns:
        add(' *   %-3s %-30s %6d bytes  sha256 %s'
            % (column['kind'], column['file'], column['bytes'], column['sha256']))
    if zap['elf']:
        add(' *   The ZAP image is one complete ELF32 ARM image (e_phnum 3, entry 0x5000,')
        add(' *   highest segment end 13964), i.e. the split .mdt/.bNN pair already')
        add(' *   re-laid-out by its own program headers. It is NOT a concatenation of')
        add(' *   the two vendor files: the reference file\'s own comment still says')
        add(' *   "zap.mdt 6860 + zap.b02 1676", which sums to 8536 and does not')
        add(' *   describe these 13964 bytes. Trust the ELF, not that comment.')
    add(' *')
    add(' * STILL NOT MEASURED ON THIS DEVICE:')
    add(' *   These bytes come from the reference board, not from this tablet\'s vendor')
    add(' *   partition, so they are the same part number and the same A650 catalog')
    add(' *   entry, but not proof that DBY-W09 ships identical images. The cross-check')
    add(' *   against /vendor/firmware (or the SUPER logical partition of the OTA) is')
    add(' *   still TODO and belongs in porting.toml before this path is called')
    add(' *   device-verified. Until then `provenance` says where each byte came from.')
    add(' *')
    add(' * The A740 values must never be substituted: 0x13000 for the SQE length,')
    add(' * "a740_sqe.fw", "gmu_gen70200.bin" and "a740_zap.mbn" are all SM8550 images.')
    add(' *')
    add(' * The device tree gives this board\'s PAS id and firmware name only')
    add(' *   DTB:/soc/qcom,kgsl-hyp:23878 qcom,pas-id = <0x0d>')
    add(' *   DTB:/soc/qcom,kgsl-hyp:23879 qcom,firmware-name = "a650_zap"')
    add(' * and no GPU node stays enabled (the reference board\'s dts/alioth.dts:35 says')
    add(' * "Leave the GPU disabled"), so nothing here is a device-tree property. */')
    add('#ifndef DBY_GPU_FIRMWARE_H')
    add('#define DBY_GPU_FIRMWARE_H')
    add('')
    add('#include <stdint.h>')
    add('#include <stddef.h>')
    add('')
    add('#include "DbyGpuRegs.h"')
    add('')
    add('enum DbyGpuFirmwareKind : uint32_t {')
    add('\tDBY_GPU_FW_ZAP = 0, /* signed secure-world payload, PAS 13 */')
    add('\tDBY_GPU_FW_GMU = 1, /* GMU core image loaded by the CP boot path */')
    add('\tDBY_GPU_FW_SQE = 2, /* CP microcode, uploaded to a GPU address */')
    add('\tDBY_GPU_FW_COUNT = 3')
    add('};')
    add('')
    add('/* Exact lengths of the embedded images. These are properties of the images,')
    add(' * not policy constants: DBY_GPU_SQE_BYTES is what a CP upload must move. */')
    for pin in pins:
        add('#define %-28s %du' % (pin['macro'], pin['bytes']))
    add('')
    add('/* The images themselves. `extern const` and `__attribute__((weak))` so every')
    add(' * translation unit that includes this header emits one candidate copy and the')
    add(' * linker keeps exactly one: no board source file has to own the storage, which')
    add(' * matters because [kernel] archives has no step that would compile one.')
    add(' * A caller that only wants the pin table defines DBY_GPU_FIRMWARE_NO_EMBED. */')
    add('#ifndef DBY_GPU_FIRMWARE_NO_EMBED')
    for pin in pins:
        add('/* %s: %s */' % (pin['kind'], pin['file']))
        text.extend(array_lines(pin['symbol'], arrays[pin['source_name']], pin['macro']))
        add('')
    add('/* The length words the reference probe reads beside the arrays (uint32_t, as')
    add(' * GpuProbe.cpp:59-63 declares them). */')
    for pin in pins:
        add('extern const uint32_t %s_size __attribute__((weak)) = %s;'
            % (pin['symbol'], pin['macro']))
    add('#else')
    for pin in pins:
        add('extern const uint8_t %s[%s] __attribute__((weak));'
            % (pin['symbol'], pin['macro']))
        add('extern const uint32_t %s_size __attribute__((weak));' % pin['symbol'])
    add('#endif /* DBY_GPU_FIRMWARE_NO_EMBED */')
    add('')
    add('struct DbyGpuFirmwarePin {')
    add('\tconst char *name;       /* file name the vendor image carries, as the')
    add('\t                         * upstream catalog and the reference probe spell it */')
    add('\tuint64_t bytes;         /* exact length of the embedded image */')
    add('\tconst char *sha256;     /* lowercase hex over exactly those bytes */')
    add('\tconst char *provenance; /* where the bytes came from, verbatim */')
    add('\tconst uint8_t *image;   /* the embedded bytes; nullptr = not pinned */')
    add('};')
    add('')
    add('/* Every field is measured. A caller still has to treat a null image as "this')
    add(' * board has no GPU firmware" and stop at the stage that needs it, instead of')
    add(' * uploading a zero-length image -- dby_gpu_firmware_pin_ready() is that test. */')
    add('static const DbyGpuFirmwarePin kDbyGpuFirmwarePins[DBY_GPU_FW_COUNT] = {')
    for column in columns:
        add('\t{ /* %s */ "%s", %d, "%s",'
            % (column['kind'], column['file'], column['bytes'], column['sha256']))
        add('\t  "%s",' % column['provenance'].replace('"', '\\"'))
        add('\t  %s },' % column['image'])
    add('};')
    add('')
    add('/* PAS id of the ZAP payload as the secure world expects it.')
    add(' * DTB:/soc/qcom,kgsl-hyp:23878 qcom,pas-id = <0x0d> */')
    add('#define DBY_GPU_ZAP_PAS_ID 13u')
    add('')
    add('/* Returns 1 when every field of the pin is present, so a caller can prove it is')
    add(' * looking at a pinned image rather than at a placeholder. */')
    add('static inline int dby_gpu_firmware_pin_ready(const DbyGpuFirmwarePin *pin)')
    add('{')
    add('\treturn pin != nullptr && pin->name != nullptr && pin->bytes != 0 &&')
    add('\t    pin->sha256 != nullptr && pin->provenance != nullptr &&')
    add('\t    pin->image != nullptr;')
    add('}')
    add('')
    add('/* Number of pins that are ready. The bring-up plan requires 3 before M4. */')
    add('static inline unsigned dby_gpu_firmware_ready_count(void)')
    add('{')
    add('\tunsigned ready = 0;')
    add('\tfor (unsigned i = 0; i < DBY_GPU_FW_COUNT; ++i) {')
    add('\t\tif (dby_gpu_firmware_pin_ready(&kDbyGpuFirmwarePins[i])) {')
    add('\t\t\t++ready;')
    add('\t\t}')
    add('\t}')
    add('\treturn ready;')
    add('}')
    add('')
    add('/* The image for one kind, or nullptr. Same table, so a caller cannot ask for a')
    add(' * part the pin table does not name. */')
    add('static inline const DbyGpuFirmwarePin *dby_gpu_firmware_pin(unsigned kind)')
    add('{')
    add('\treturn kind < DBY_GPU_FW_COUNT ? &kDbyGpuFirmwarePins[kind] : nullptr;')
    add('}')
    add('')
    add('/* Fixed GPU addresses the command processor needs, mirroring the layout the')
    add(' * Adreno740 driver uses so the client ABI stays the same. The values are')
    add(' * policy, not hardware discovery, so they are stated here and not measured. */')
    add('#define DBY_GPU_SQE_IOVA 0x00100000ull')
    add('#define DBY_GPU_RING_IOVA 0x00200000ull')
    add('#define DBY_GPU_MEMPTRS_IOVA 0x00300000ull')
    add('#define DBY_GPU_PWRUP_IOVA 0x00301000ull')
    add('#define DBY_GPU_SCRATCH_IOVA 0x00302000ull')
    add('#define DBY_GPU_RING_BYTES 0x8000u')
    add('/* 2 GiB of GPU VA per address space, as the client ABI assumes. The reference')
    add(' * driver derives each client window from the SMMU ID2 input size instead')
    add(' * ((1 << input_bits) - GpuClientVaStart), which is why the address-space')
    add(' * scheme, not this constant, decides how much a process may map. */')
    add('#define DBY_GPU_APERTURE_BYTES 0x80000000ull')
    add('')
    add('#endif /* DBY_GPU_FIRMWARE_H */')
    return '\n'.join(text) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path,
                        help='the reference file that carries the three A650 arrays')
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT,
                        help=f'header to write (default {DEFAULT_OUT})')
    parser.add_argument('--check', action='store_true',
                        help='compare instead of writing; exit 1 when the file differs')
    args = parser.parse_args()

    source = args.source
    if source is None:
        # No guessed absolute path: the reference checkout lives wherever the research
        # tree lives, and a stale hard-coded one would silently pin the wrong blob.
        candidates = [ROOT / 'package/GpuFirmwareA650.c']
        source = next((c.resolve() for c in candidates if c.is_file()), None)
        if source is None:
            raise SystemExit('no GpuFirmwareA650.c in: '
                             + ', '.join(str(c) for c in candidates)
                             + '; pass --source <reference>/package/GpuFirmwareA650.c')
    if not source.is_file():
        raise SystemExit(f'{source} missing')
    arrays = read_arrays(source)

    for pin in PINS:
        data = arrays.get(pin['source_name'])
        if data is None:
            raise SystemExit(f'{source} has no array {pin["source_name"]}')
        if len(data) != pin['bytes']:
            raise SystemExit('%s: %s is %d bytes, the A650 part needs %d'
                             % (source, pin['source_name'], len(data), pin['bytes']))

    header = build_header(source, arrays, PINS)
    out = args.out if args.out.is_absolute() else (ROOT / args.out)
    if args.check:
        if not out.is_file():
            raise SystemExit(f'{out} missing; run the generator')
        if out.read_text() != header:
            raise SystemExit(f'{out} differs from what the generator produces: '
                             f'{source} has changed or the header was edited by hand')
        print(f'PASS: {out.name} matches {source.name}')
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(header)
    print(f'PASS: wrote {out} ({len(header.splitlines())} lines) from {source}')
    for pin in PINS:
        data = arrays[pin['source_name']]
        print('      %-3s %6d bytes sha256 %s'
              % (pin['kind'], len(data), hashlib.sha256(data).hexdigest()))


if __name__ == '__main__':
    main()
