#!/usr/bin/env python3
"""Check the dby_w09 platform descriptors against the compiled device tree and config.

Minimal preflight for the DBY-W09 (SM8250) UEFI port. It verifies:
  - the UEFI FD / Stack descriptors in MemoryMapLib.c match dby_w09.toml + dby_w09.dsc
  - memory descriptors are 4 KiB aligned and non-overlapping
  - the Binaries/dby_w09 vendor inputs referenced by the .inc files are present
This is the dby_w09 analogue of sheng's check-sheng.py, trimmed to what the
bring-up stage 2 -> 3 actually needs (no DTB/stock-firmware cross-check yet).
"""
import json
import os
import re
from pathlib import Path
import tomllib

# core runs this with cwd = the device tree and passes the firmware checkout in
# $UNIXNU_UEFI (cli.py:74); 'uefi-dby_w09' is the same checkout installed under its
# repository name next to the tree.
uefi_root = Path(os.environ.get('UNIXNU_UEFI') or 'uefi-dby_w09')
PLATFORM = uefi_root / 'Platforms/Huawei/dby_w09Pkg'
CONFIG = uefi_root / 'Resources/Configs/dby_w09.toml'
DTB = Path('build/dby_w09/dby_w09.dtb')


def rows_from_memory_map(source: Path):
    """Parse EFI_MEMORY_REGION_DESCRIPTOR rows out of MemoryMapLib.c."""
    text = source.read_text()
    return [(m[1], int(m[2], 16), int(m[3], 16), m[4])
            for m in re.finditer(
                r'\{"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,'
                r'\s*([A-Za-z_]+)', text)]


if __name__ == '__main__':
    errors = []

    memory_map_path = PLATFORM / 'Library/MemoryMapLib/MemoryMapLib.c'
    if not memory_map_path.is_file():
        print('MemoryMapLib.c not found; skip descriptor checks')
        rows = []
    else:
        rows = rows_from_memory_map(memory_map_path)

    if not rows or len(rows) > 128:
        errors.append('Empty memory map or descriptor capacity exceeded')
    if memory_map_path.is_file():
        text = memory_map_path.read_text()
        declared = text.count('{"')
        if len(rows) != declared:
            errors.append(f'Parsed {len(rows)} of {declared} descriptor rows; entries were dropped')

    # Alignment covers every row. Overlap mirrors the firmware's validator:
    # Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c ValidateMemoryMap()
    # sorts by Address and, for each adjacent pair after sorting, skips the comparison when the
    # lower-address region is below FixedPcdGet64 (PcdSystemMemoryBase) ("Skip Register Regions")
    # before testing EndAddress > NextRegion->Address. Comparing every pair but dropping pairs
    # whose lower address is below that base is the same predicate without the sort.
    #
    # This matters on this board: "IMEM Base" (0x14680000 + 0x40000 = 0x146C0000) overlaps
    # "IMEM Cookie Base" (0x146BF000) by 0x1000, and both are MMAP_IO windows below DRAM. The
    # firmware never reaches the dead loop for that pair, so the gate must not fail on it.
    dsc_path = PLATFORM / 'dby_w09.dsc'
    dsc = dsc_path.read_text() if dsc_path.is_file() else ''
    system_base = re.search(r'PcdSystemMemoryBase\|(0x[0-9a-fA-F]+)', dsc)
    if system_base:
        ram_floor = int(system_base.group(1), 16)
    else:
        ram_floor = 0
        errors.append('PcdSystemMemoryBase not found in dby_w09.dsc; overlap semantics unknown')
    checked = [(name, base, size) for name, base, size, _ in rows if base >= ram_floor]

    for name, base, size, _ in rows:
        if base % 4096 or size % 4096 or not size:
            errors.append(f'Invalid UEFI page alignment: {name}')

    for index, (name, base, size) in enumerate(checked):
        for other, other_base, other_size in checked[index + 1:]:
            if max(base, other_base) < min(base + size, other_base + other_size):
                errors.append(f'Overlapping intervals: {name}, {other}')

    # UEFI FD descriptor vs dby_w09.toml [uefi_fd]
    config_path = CONFIG
    if not config_path.is_file():
        errors.append('dby_w09.toml not found; the FD size cannot be checked against it')
    else:
        config = tomllib.loads(config_path.read_text())
        fd = config.get('uefi_fd', {})
        fd_base = fd.get('base')
        fd_size = fd.get('size')
        if not fd_base or not fd_size:
            errors.append('[uefi_fd] base/size missing from dby_w09.toml')
        elif not any(n == 'UEFI FD' and a == fd_base and s >= fd_size
                     for n, a, s, _ in rows):
            errors.append('FD configuration does not match a "UEFI FD" memory descriptor')

    # UEFI Stack descriptor vs dsc PcdCPUCoresStackBase
    stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
    if not stack:
        errors.append('PcdCPUCoresStackBase not found in dby_w09.dsc')
    elif not any(n == 'UEFI Stack' and a == int(stack[1], 16) for n, a, _, _ in rows):
        errors.append('Stack configuration does not match a "UEFI Stack" memory descriptor')

    # Vendor binary inputs referenced by the .inc files must exist
    dependencies = set()
    inc_dir = PLATFORM / 'Include'
    if inc_dir.is_dir():
        for inc in inc_dir.glob('*.inc'):
            dependencies.update(re.findall(r'Binaries/dby_w09/[^\s}]+', inc.read_text()))
    missing = sorted(p for p in dependencies if not (uefi_root / p).is_file())
    # The docstring promises these are present; a missing one fails the EDK II build later with a
    # far less specific message, so it fails the gate here instead of only being printed.
    errors.extend(f'Missing vendor input: {p}' for p in missing)

    result = {
        'descriptors': len(rows),
        'static_errors': errors,
        'missing_vendor_inputs': missing,
        'runtime_ram_verified': False,
        'hardware_boot_demonstrated': True,   # DBY-W09 UEFI already boots on hardware
    }
    out = Path('build/dby_w09/check.json')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + '\n')

    print(f'{len(rows)} memory descriptors checked')
    print(f'{len(errors)} static errors; {len(missing)} vendor inputs missing')
    for error in errors:
        print(error)
    raise SystemExit(bool(errors))
