#!/usr/bin/env python3
"""Build libDbyGpuSmmuProbe.a, the archive the DBY-W09 kernel links for the read-only GPU SMMU probe.

`package/DbyGpuSmmuProbe.cpp` maps the GPU's Arm MMU-500 at physical 0x03da0000 (this board's own vendor
tree: kona-gpu.dtsi:263-266, qcom,kgsl-iommu@3da0000, reg = <0x03da0000 0x10000>) and reads ID0/ID1/ID2
at 0x20/0x24/0x28.  Its purpose is the GPU track's first gate (G-1): the Adreno 650 driver refuses every
non-kernel client with e00002be when the SMMU reports input_bits <= 32.  It writes nothing, and it runs
only when `dby-gpu-smmu=1` is on the kernel command line.  Design and risk:
work/tracks/gpu-smmu-probe-review/REPORT.md, work/tracks/gpu/SPEC.md.

It is one translation unit with no headers of its own, so the archive is one object:

    DbyGpuSmmuProbe.cpp -> libDbyGpuSmmuProbe.a

Compiled with a command the kernel build recorded for a C++ file, so the headers, defines and ABI
flags are exactly the ones the kernel was built with:

    DbyGpuSmmuProbe.cpp <- build/xnu/ios-obj/.../iokit/DEVELOPMENT/*.json  (C++)

That is the same technique, and the same four warning demotions, as scripts/build-dby-touch.py and
scripts/build-cpufreq.py in this repository.

The kernel tree is only read (compile record, headers). Every output lands under {tree}/build/gpusmmu/.
The defconfig force-loads it through CONFIG_XNU_EXTRA_LD_ARCHIVES:

    -Wl,-force_load,{tree}/build/gpusmmu/libDbyGpuSmmuProbe.a

force_load, not a plain archive path, because every reference to this probe is a WEAK import
(`package/BootProgress.c` declares `sheng_gpu_smmu_probe` weak, `package/BootLog.c` reads
`native_gpusmmu_census` weak), so an ordinary archive member is never pulled in by the linker and the
probe would silently never run. (`package/BootProgressStubs.c` does NOT provide stubs for this probe's
symbols - it carries the CPU-frequency ones; the weak references are the whole reason for force_load.)

Usage:
  python3 scripts/build-gpu-smmu-probe.py --tree /root/unixnu-out/.../tree
  python3 scripts/build-gpu-smmu-probe.py --tree <tree> --source-dir package
  python3 scripts/build-gpu-smmu-probe.py --tree <tree> --no-verify    # skip the symbol check
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_NAME = 'build/gpusmmu'
ARCH = 'DEVELOPMENT_ARM64_BCM2837'

# The one translation unit, and the recorded command it is compiled with. A C++ record on purpose:
# the file is C++ and its entry points are `extern "C"`; compiling it as C would change nothing it
# exports but would lose the static_assert and the namespace.
SOURCE = 'DbyGpuSmmuProbe.cpp'
RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
ARCHIVE = 'libDbyGpuSmmuProbe.a'

# The symbols the kernel link must see defined by this archive. A typo, a mangled name or a
# missing extern "C" would otherwise only show up as "the page row never appears" on the device.
# Three names, all extern "C": the probe entry point and the two globals BootLog.c reads.
EXPORTS = ('sheng_gpu_smmu_probe', 'native_gpusmmu_census', 'native_gpusmmu_stage')

DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unnecessary-virtual-specifier',
]

# -Weverything is what the kernel's own compile uses, but the kernel never turns warnings into
# errors; combined with -Werror it drags in diagnostics the kernel does not demote. -Wall -Wextra
# -Werror is kept, the umbrella is dropped, and the report records that so nobody believes the
# archive was built under it.
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
            # A pruned tree no longer carries every include directory the kernel build had
            # (EXPORT_HDRS/libsa is the usual one), and -Werror turns -Wmissing-include-dirs into a
            # hard failure. Skip the ones that are gone, and say which.
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
    """A symbol listing from an nm that can actually read this archive.

    Not `xcrun nm` alone any more (2026-11-03 12:45): outside the kernel build's PATH that shim
    resolves to binutils nm, which exits 0 on a Mach-O object after printing "file format not
    recognized" to stderr only, so the listing came back empty and every export looked missing.
    llvm-nm first, then the macOS shim, then plain nm, and an empty listing is a failure rather
    than a result.
    """
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
    raise SystemExit(f'no nm could read {archive} ({last}). A Mach-O archive needs llvm-nm; '
                     f'binutils nm exits 0 with an empty listing, so this is a tool problem, '
                     f'not a missing-symbol problem.')


def verify_archive(archive):
    """Refuse an archive that does not define the three symbols the board's consumers name.

    nm -g -P prints `<name> <type> <value>` per symbol, with a leading underscore for external C
    names on Mach-O. T/D/S/B are definitions; U is the failure this check is for.
    """
    listing, tool = nm_listing(archive)
    defined = {}
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(':'):
            continue
        # Mach-O prints one leading underscore for an external C name. Strip exactly one, so the
        # C++ form keeps its own leading underscore (`__Z19...` -> `_Z19...`).
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
                        help="assembled kernel tree; defaults to this script's own tree root "
                             "(that is what `unixnu` uses when it runs an archive recipe) or "
                             '$UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path,
                        help='directory holding DbyGpuSmmuProbe.cpp; defaults to the documented '
                             'lookup order')
    parser.add_argument('--no-verify', action='store_true',
                        help='skip the post-build symbol check (nm)')
    args = parser.parse_args()

    # `unixnu` copies scripts/ into <tree>/scripts and runs the recipe there, so the script's own
    # parent-of-parent is the tree in the normal build path. --tree/$UNIXNU_TREE overrides it.
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
        # The probe's own constants, repeated here so a build report says which board deltas of
        # SPEC §3 the archive actually carries without opening the source.
        'board_deltas': {
            'smmu_base': '0x03da0000',
            'smmu_span': '0x10000',
            'id0_offset': '0x20',
            'id1_offset': '0x24',
            'id2_offset': '0x28',
            'input_bits_decode': 'code=(id2>>8)&15; {32,36,40,42,44}[code] else 48',
            'gate': 'input_bits <= 32 -> publish refuses user clients (e00002be)',
            'opt_in_arg': 'dby-gpu-smmu=1',
            'writes': False,
        },
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
