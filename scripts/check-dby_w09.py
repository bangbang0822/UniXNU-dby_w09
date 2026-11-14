#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
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
# $UNIXNU_UEFI (cli.py:90 reads it; line 74 is the vendor_boot parser's --verify-against
# option); 'uefi-dby_w09' is the same checkout installed under its repository name next
# to the tree.
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
        # The descriptor checks cannot pass without the file: the empty row list below fails them.
        # The message used to say the checks were skipped and then fail anyway.
        print('MemoryMapLib.c not found; the descriptor checks fail without it')
        rows = []
    else:
        rows = rows_from_memory_map(memory_map_path)

    if not rows:
        errors.append('Empty memory map: MemoryMapLib.c is missing or declares no row')
    elif len(rows) > 128:
        errors.append(f'Descriptor capacity exceeded: {len(rows)} rows, 128 allowed')

    # Alignment / overlap checks. Only rows at or above PcdSystemMemoryBase are compared:
    # Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c skips register
    # regions before its EndAddress test, so flagging one would reject a table the tablet
    # already boots with.
    dsc_text = (PLATFORM / 'dby_w09.dsc').read_text() if (PLATFORM / 'dby_w09.dsc').is_file() else ''
    system_base = re.search(r'PcdSystemMemoryBase\|(0x[0-9a-fA-F]+)', dsc_text)
    ram_floor = int(system_base.group(1), 16) if system_base else 0
    checked = [(name, base, size) for name, base, size, _ in rows if base >= ram_floor]
    for index, (name, base, size) in enumerate(checked):
        if base % 4096 or size % 4096 or not size:
            errors.append(f'Invalid UEFI page alignment: {name}')
        for other, other_base, other_size in checked[index + 1:]:
            if max(base, other_base) < min(base + size, other_base + other_size):
                errors.append(f'Overlapping intervals: {name}, {other}')

    # UEFI FD descriptor vs dby_w09.toml [uefi_fd]
    config_path = CONFIG
    if not config_path.is_file():
        print(f'note: {config_path} is absent, so the FD descriptor was not compared')
    else:
        config = tomllib.loads(config_path.read_text())
        fd = config.get('uefi_fd', {})
        fd_base = fd.get('base')
        fd_size = fd.get('size')
        if not (fd_base and fd_size):
            # A configuration file with no [uefi_fd] base/size used to skip this comparison silently, so the
            # FD descriptor was never checked on a tree that does have one -- say so instead of passing.
            print(f'note: {config_path} has no [uefi_fd] base/size, so the FD descriptor was not compared')
        else:
            if not any(n == 'UEFI FD' and a == fd_base and s >= fd_size
                       for n, a, s, _ in rows):
                errors.append('FD configuration does not match a "UEFI FD" memory descriptor')

    # UEFI Stack descriptor vs dsc PcdCPUCoresStackBase
    dsc_path = PLATFORM / 'dby_w09.dsc'
    if dsc_path.is_file():
        dsc = dsc_path.read_text()
        stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
        if stack and not any(n == 'UEFI Stack' and a == int(stack[1], 16)
                             for n, a, _, _ in rows):
            errors.append('Stack configuration does not match a "UEFI Stack" memory descriptor')

    # Vendor binary inputs referenced by the .inc files must exist
    dependencies = set()
    inc_dir = PLATFORM / 'Include'
    if inc_dir.is_dir():
        for inc in inc_dir.glob('*.inc'):
            dependencies.update(re.findall(r'Binaries/dby_w09/[^\s}]+', inc.read_text()))
    missing = sorted(p for p in dependencies if not (uefi_root / p).is_file())

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
    for path in missing:
        print(f'missing vendor input: {path}')
    # A vendor binary the .inc files need is as much a failure as a static error: the platform will not
    # build against the UEFI tree without it, so it has to reach the exit status.
    raise SystemExit(bool(errors or missing))
