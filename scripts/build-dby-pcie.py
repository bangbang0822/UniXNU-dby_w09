#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build libDbyPcie.a, the archive the DBY-W09 kernel links for the wireless PCIe0 probe.

`package/DbyPcie.cpp` is stage M1 of docs/wireless-spec-2026-11-20.md: a read-only census of PCIe0
and of the pads the WLAN endpoint needs.  It maps the always-on domains (GCC at 0x00100000, the three
TLMM tiles), reads GCC_PCIE_0_GDSCR and the six halt registers, reports which tile owns GPIO20 and
GPIO79, and only when GDSCR says the controller is powered does it read PARF, ELBI and - if the link
is already up - the DBI vendor/device ID.  It writes nothing, and it runs only when `dby-pcie=N` is on
the kernel command line.

It is one translation unit with no headers of its own, so the archive is one object:

    DbyPcie.cpp -> libDbyPcie.a

Compiled with a command the kernel build recorded for a C++ file, so the headers, defines and ABI
flags are exactly the ones the kernel was built with:

    DbyPcie.cpp <- build/xnu/ios-obj/.../iokit/DEVELOPMENT/*.json  (C++)

The same technique, and the same four warning demotions, as scripts/build-gpu-smmu-probe.py,
scripts/build-dby-touch.py and scripts/build-gop-legacy-display.py in this repository.

The kernel tree is only read (compile record, headers).  Every output lands under {tree}/build/dbypcie/.
The defconfig force-loads it through CONFIG_XNU_EXTRA_LD_ARCHIVES:

    -Wl,-force_load,{tree}/build/dbypcie/libDbyPcie.a

force_load, not a plain archive path: every reference to this probe is a WEAK import
(`package/BootProgress.c` declares `dby_pcie_probe` weak), so a plain archive path leaves the linker
with nothing undefined to pull and the object never enters the link.  That is the defect
scripts/build-gpu-smmu-probe.py:29-34 records for the GPU probe.

Usage:
  python3 scripts/build-dby-pcie.py --tree /root/unixnu-out/.../tree
  python3 scripts/build-dby-pcie.py --tree <tree> --source-dir package
  python3 scripts/build-dby-pcie.py --tree <tree> --no-verify    # skip the symbol check
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_NAME = 'build/dbypcie'
ARCH = 'DEVELOPMENT_ARM64_BCM2837'

SOURCE = 'DbyPcie.cpp'
RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
ARCHIVE = 'libDbyPcie.a'

# The symbols the kernel link must see defined by this archive.  A typo, a missing extern "C" or a
# renamed entry point would otherwise only show up as "the [dby-pcie] rows never appear" on hardware.
EXPORTS = ('dby_pcie_probe', 'native_dbypcie_census', 'native_dbypcie_stage', 'native_dbypcie_rc')

DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unnecessary-virtual-specifier',
]

DROPPED = {'-Weverything'}


def build_command(record_path, source, obj, source_dir):
    """Reuse the kernel's own recorded command, with -Werror in front and the demotions last."""
    record = json.loads(Path(record_path).read_text().strip().rstrip(','))
    args = list(record['arguments'])
    command = [args[0], '-Werror']
    i = 1
    skipped_includes = []
    while i < len(args):
        arg = args[i]
        if arg in DROPPED:
            i += 1
            continue
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-I' and i + 1 < len(args):
            directory_arg = args[i + 1]
            if Path(directory_arg).is_dir():
                command += [arg, directory_arg]
            else:
                skipped_includes.append(directory_arg)
            i += 1
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        else:
            command.append(arg)
        i += 1
    command += ['-Wall', '-Wextra', *DEMOTIONS, '-I', str(source_dir)]
    if '-o' not in command:
        command += ['-o', str(obj)]
    return command, record['directory'], skipped_includes


def nm_listing(archive):
    """A symbol listing from an nm that can read a Mach-O archive (see build-gpu-smmu-probe.py)."""
    attempts = (['llvm-nm', '-g', '-P'], ['xcrun', 'nm', '-g', '-P'], ['nm', '-g', '-P'])
    last = 'none tried'
    for prefix in attempts:
        try:
            listing = subprocess.check_output([*prefix, str(archive)], text=True, timeout=60,
                                              stderr=subprocess.DEVNULL)
        except (OSError, subprocess.SubprocessError) as error:
            last = f'{prefix[0]}: {error}'
            continue
        if listing.strip():
            return listing, ' '.join(prefix[:-3]) or prefix[0]
        last = f'{prefix[0]}: printed no symbols'
    raise SystemExit(f'no nm could read {archive} ({last}).')


def verify_archive(archive):
    listing, tool = nm_listing(archive)
    defined = {}
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(':'):
            continue
        name = fields[0][1:] if fields[0].startswith('_') else fields[0]
        defined[name] = fields[1]
    missing = [name for name in EXPORTS if defined.get(name) not in ('T', 'D', 'S', 'B', 't', 'd', 's', 'b')]
    if missing:
        raise SystemExit(f'archive does not define (listed by {tool}): ' + ', '.join(missing))
    return {name: defined[name] for name in EXPORTS}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='assembled kernel tree; defaults to this script\'s own tree root '
                             '(what `unixnu` uses when it runs an archive recipe) or $UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path,
                        help='directory holding DbyPcie.cpp; defaults to the documented lookup order')
    parser.add_argument('--no-verify', action='store_true',
                        help='skip the post-build symbol check (nm)')
    args = parser.parse_args()

    tree = (args.tree or ROOT).resolve()
    if not (tree / 'build/xnu/ios-obj' / ARCH).is_dir():
        raise SystemExit(f'{tree} has no build/xnu/ios-obj/{ARCH}: point --tree at the assembled tree')
    out = tree / OUT_NAME

    candidates = [args.source_dir] if args.source_dir else [
        tree / 'package',
        tree / 'kits/dby_w09',
        ROOT / 'package',
    ]
    source_dir = next((c.resolve() for c in candidates if c and (c / SOURCE).is_file()), None)
    if source_dir is None:
        raise SystemExit(f'{SOURCE} not found in: ' + ', '.join(str(c) for c in candidates))
    source = source_dir / SOURCE

    record_path = tree / 'build/xnu/ios-obj' / ARCH / RECORD
    if not record_path.is_file():
        raise SystemExit(f'{record_path} missing: build the kernel once before this archive')

    out.mkdir(parents=True, exist_ok=True)
    obj = out / (source.stem + '.o')
    command, directory, skipped = build_command(record_path, source, obj, source_dir)
    if skipped:
        print('note: skipped missing include dirs: ' + ', '.join(skipped))
    (out / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, cwd=directory, check=True, timeout=180)

    archive = out / ARCHIVE
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=60)

    symbols = {} if args.no_verify else verify_archive(archive)

    report = {
        'tree': str(tree),
        'source_dir': str(source_dir),
        'sources': {SOURCE: hashlib.sha256(source.read_bytes()).hexdigest()},
        'compile_record': str(record_path),
        'objects': {obj.name: obj.stat().st_size},
        'archive': archive.name,
        'archive_bytes': archive.stat().st_size,
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'exported_symbols': symbols,
        'dropped_flags': sorted(DROPPED),
        'added_flags': ['-Wall', '-Wextra', *DEMOTIONS],
        'skipped_include_dirs': skipped,
        # What this archive is, so a build report says which stage it carries without the source.
        'stage': 'M1 (docs/wireless-spec-2026-11-20.md §4.2)',
        'board_windows': {
            'gcc': '0x00100000 + 0x94000',
            'parf': '0x01c00000 + 0x3000',
            'dbi_elbi': '0x60000000 + 0x2000',
            'tlmm_tiles': '0x0f100000, 0x0f500000, 0x0f900000 (0x80000 each)',
        },
        'opt_in_arg': 'dby-pcie=N (N >= 1)',
        'writes': False,
        'hardware_execution': False,
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: ' + ARCHIVE + ' built from ' + str(source_dir))
    print(f'      archive {archive} ({report["archive_bytes"]} bytes)')
    if not args.no_verify:
        print('      symbols: ' + ', '.join(f'{n}={t}' for n, t in symbols.items()))
    print('      link it with CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,'
          f'{tree / OUT_NAME / ARCHIVE}"')
    print('      then re-run: ./unixnu defconfig dby_w09_defconfig -D ../UniXNU-dby_w09')


if __name__ == '__main__':
    main()
