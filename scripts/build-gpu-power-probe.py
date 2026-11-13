#!/usr/bin/env python3
"""Build libDbyGpuPowerProbe.a: the board probe plus the upstream SM8250 GPU power kit.

What it compiles
----------------
    package/DbyGpuPowerProbe.cpp                    (C++, board probe)
    {tree}/kits/Sm8250Platform/A650GmuPower.c       (C, upstream kit, compiled verbatim)

The kit is the Adreno 650 power/clocks/AOP parser from FFT-infra/UniXNU-sm8250-common@52bab45
(component `Sm8250Platform`, catalogue entry added locally because upstream never listed it).
It is the owner of the step the ad-hoc SMMU probe lacked: mode 2 writes gcc_gpu_cfg_ahb_clk's
enable bit, mode 3 brings up gpu_cx_gdsc and the hlos1 SMMU vote clock, and the SMMU registers
are only read when CX is PWR_ON and that clock runs (A650GmuPower.c:258-263). See
work/tracks/gpu-smmu-read-hang/REPORT.md for why that ordering is mandatory on this SoC.

Why the compile commands come from the kernel
---------------------------------------------
Same technique as scripts/build-dby-touch.py and build-gpu-smmu-probe.py: a command the kernel
build recorded is replayed, so headers, defines and ABI flags are exactly the kernel's. The C TU
uses the recorded C command, the C++ TU the recorded C++ command.

Outputs
-------
    {tree}/build/gpupower/libDbyGpuPowerProbe.a
    {tree}/build/gpupower/compile-command-<stem>.json      (one per TU, for the review trail)

force-load, because every reference to this probe is a weak import (BootProgress.c,
BootLog.c), so a plain archive member would never be pulled in.

Usage:
  python3 scripts/build-gpu-power-probe.py --tree /root/unixnu-out/.../tree
  python3 scripts/build-gpu-power-probe.py --tree <tree> --no-verify
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCH = 'DEVELOPMENT_ARM64_BCM2837'
OUT_DIR = 'build/gpupower'
ARCHIVE = 'libDbyGpuPowerProbe.a'
# The feed source pins path = "kits/Sm8250Platform" (feeds.toml), so the tree links the module directory
# itself and the kit file sits exactly here. An unpinned source would link the repository root and the
# file would be one level deeper; that mismatch is what pinned the path (review 2026-11-03 15:30).
KIT_CANDIDATES = (Path('kits/Sm8250Platform/A650GmuPower.c'),)
# Round 2 (cold start + GX) needs two more kit TUs, and the AdrenoFirmware headers they include.
# The cold start is board-owned: package/DbyA650Gmu.c plus package/DbyA650Gmu.h (see their headers:
# 100 ms poll, CM3 fault BIT(15), GBIF clear before the release through a fourth GPU window).  The
# feed's copies are no longer compiled; the other kit TUs still come from the feed.
KIT_REGIONS_PATH = Path('kits/Sm8250Platform/A650GmuRegions.c')
FIRMWARE_SOURCES = ('AdrenoFirmware.c', 'AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c')
FIRMWARE = 'FIRM:'
FIRMWARE_CANDIDATES = (
    Path('kits/AdrenoFirmware/AdrenoFirmware.h'),
    Path('..') / 'UniXNU' / 'kits' / 'AdrenoFirmware' / 'AdrenoFirmware.h',
)
# Round 2a (GMU SMMU context) drives the SMMU through the ArmSmmu4K kit rather than by hand. That kit is
# not materialised in this tree either, so it is compiled from the checkout beside us.
SMMU_CANDIDATES = (
    Path('kits/ArmSmmu4K/ArmSmmu4K.c'),
    Path('..') / 'UniXNU' / 'kits' / 'ArmSmmu4K' / 'ArmSmmu4K.c',
)
SMMU_SOURCES = (
    'ArmSmmu4K.c', 'ArmSmmu4KContext.c', 'ArmSmmu4KStreams.c', 'ArmSmmu4KMmio.c',
    'ArmSmmu4KTableMemory.c', 'ArmSmmu4KProgram.c', 'ArmSmmu4KLive.c',
)
# Mode 14 emits PM4 packets; the encoder is the pinned AdrenoPM4 kit, not a local copy.
PM4_CANDIDATES = (
    Path('kits/AdrenoPM4/AdrenoPM4.c'),
    Path('..') / 'UniXNU' / 'kits' / 'AdrenoPM4' / 'AdrenoPM4.c',
)
PM4_SOURCES = ('AdrenoPM4.c',)

# (source path relative to the tree, or a KIT marker, language, object suffix)
KIT = 'KIT'
KIT_GMU = 'KIT_GMU'
KIT_REGIONS = 'KIT_REGIONS'
SMMU = 'SMMU:'          # prefix for an ArmSmmu4K source: 'SMMU:ArmSmmu4KLive.c'
PM4 = 'PM4:'            # prefix for an AdrenoPM4 source: 'PM4:AdrenoPM4.c'
SOURCES = (
    ('package/DbyGpuPowerProbe.cpp', 'cxx', '.o'),
    (KIT, 'c', '.kit.o'),
    (KIT_GMU, 'c', '.gmu.o'),
    (KIT_REGIONS, 'c', '.regions.o'),
    # The 12 shared kit TUs (ArmSmmu4K x7, AdrenoFirmware/Hfi x4, AdrenoPM4 x1) are NOT
    # compiled here since 2026-11-06: they live in build/adreno-kits/libAdrenoKits.a,
    # which the kernel link places after both force_load'ed archives so one copy serves
    # the driver and this probe (work/tracks/gpu-r3-wiring/REPORT.md Q1, option ii).
    # Compiling them here again is what produced 41 duplicate symbols.
)

EXPORTS = ('sheng_gpu_power_probe', 'native_gpupower_rows', 'native_gpupower_stage',
           'A650GmuColdStart', 'A650GmuPowerGx', 'A650GmuBuildStartup', 'A650GmuBuildRegions')

DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-missing-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unused-parameter',
    '-Wno-deprecated-enum-enum-conversion',
]

# The kernel records one compile command per language under iokit; the C++ one is the same record
# build-dby-touch.py and build-gpu-smmu-probe.py use, and iokit also has a C record for the kit TU.
# Records are per source LANGUAGE and per originating make target, so the C record is a real C build
# (bsd_init.o) and the C++ record is the one build-dby-touch.py uses. Using the C++ record for a .c
# file compiles it as C++ and trips -Wc++11-narrowing inside the upstream kit (hit 2026-11-03 14:35).
CXX_RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORDS = ('bsd/DEVELOPMENT/bsd_init.o.json', 'osfmk/DEVELOPMENT/startup.o.json')


def board_kit(tree):
    """[board] kit from the tree's board.toml, so the package directory is read not guessed."""
    import re

    manifest = tree / 'board.toml'
    if manifest.is_file():
        match = re.search(r'^\s*kit\s*=\s*"([^"]+)"', manifest.read_text(), re.M)
        if match:
            return match.group(1)
    return 'dby_w09'


def nm_listing(archive):
    """A symbol listing from an nm that can read Mach-O (binutils nm exits 0 with an empty table)."""
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
            return listing, prefix[0]
        last = f'{prefix[0]}: printed no symbols'
    raise SystemExit(f'no nm could read {archive} ({last}). A Mach-O archive needs llvm-nm; '
                     f'binutils nm exits 0 with an empty listing, so this is a tool problem, '
                     f'not a missing-symbol problem.')


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


def read_record(path):
    return json.loads(Path(path).read_text().strip().rstrip(','))


def command_from(record, source, obj, extra_includes):
    """Replay a recorded compile command for one source, with this script's warnings."""
    args = record['arguments']
    command = [args[0]]
    skipped = []
    i = 1
    while i < len(args):
        arg = args[i]
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif arg in ('-MF', '-MT', '-MJ'):
            i += 1
        elif arg == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif arg == '-I' and i + 1 < len(args):
            directory = args[i + 1]
            if Path(directory).is_dir():
                command += ['-I', directory]
            else:
                skipped.append(directory)
            i += 1
        elif not arg.startswith('-W') and arg not in ('-MD', '-MP', '-Weverything'):
            command.append(arg)
        i += 1
    command += ['-Wall', '-Wextra', *DEMOTIONS]
    for include in extra_includes:
        if Path(include).is_dir():
            command += ['-I', str(include)]
        else:
            skipped.append(str(include))
    if '-o' not in command:
        command += ['-o', str(obj)]
    return command, record['directory'], skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None)
    parser.add_argument('--no-verify', action='store_true', help='skip the symbol check')
    args = parser.parse_args()

    tree = (args.tree or ROOT).resolve()
    records = tree / 'build/xnu/ios-obj' / ARCH
    if not records.is_dir():
        raise SystemExit(f'{tree} has no build/xnu/ios-obj/{ARCH}: build the kernel once first '
                         f'(this recipe replays the commands the kernel build recorded)')

    c_record = next((records / name for name in C_RECORDS if (records / name).is_file()), None)
    cxx_record = records / CXX_RECORD
    if c_record is None or not cxx_record.is_file():
        raise SystemExit(f'no compile records under {records}')
    records_by_lang = {'c': read_record(c_record), 'cxx': read_record(cxx_record)}

    kit = next((tree / candidate for candidate in KIT_CANDIDATES if (tree / candidate).is_file()), None)
    if kit is None:
        raise SystemExit('the Sm8250Platform kit is not materialised under '
                         + ' or '.join(str(tree / c) for c in KIT_CANDIDATES)
                         + ': it comes from the component of that name; select it in board.toml '
                           '[components] include and run `unixnu fetch` + `unixnu tree`.')
    kit_dir = kit.parent

    # A650Gmu.c includes AdrenoFirmware.h and AdrenoHfiVotes.h, which live in the AdrenoFirmware kit.
    # That kit is not materialised in this tree (a manual copy into kits/ broke the fetch step), so the
    # headers are read from the checkout when it is beside us.
    firmware = next((tree / candidate for candidate in FIRMWARE_CANDIDATES
                     if (tree / candidate).is_file()), None)
    if firmware is None:
        firmware = next((candidate for candidate in FIRMWARE_CANDIDATES
                         if candidate.is_file()), None)
    if firmware is None:
        raise SystemExit('the AdrenoFirmware kit is not available: expected '
                         + ' or '.join(str(c) for c in FIRMWARE_CANDIDATES))

    smmu = next((tree / c for c in SMMU_CANDIDATES if (tree / c).is_file()), None)
    if smmu is None:
        smmu = next((c for c in SMMU_CANDIDATES if c.is_file()), None)
    if smmu is None:
        raise SystemExit('the ArmSmmu4K kit is not available: expected '
                         + ' or '.join(str(c) for c in SMMU_CANDIDATES))
    smmu_dir = smmu.parent
    pm4 = next((tree / c for c in PM4_CANDIDATES if (tree / c).is_file()), None)
    if pm4 is None:
        pm4 = next((c for c in PM4_CANDIDATES if c.is_file()), None)
    if pm4 is None:
        raise SystemExit('the AdrenoPM4 kit is not available: expected '
                         + ' or '.join(str(c) for c in PM4_CANDIDATES))
    pm4_dir = pm4.parent

    out = tree / OUT_DIR
    out.mkdir(parents=True, exist_ok=True)
    includes = [kit_dir, firmware.parent, smmu_dir, pm4_dir, tree / '.cache/xnu-work/osfmk']
    # mode 17 drives the A650 archive's C entry points, so the probe includes its headers.
    # Three places to look: the assembled tree (only when the feed is materialised), the core
    # checkout beside this repository (running the recipe from the device repo), and the core
    # checkout beside the device repository the board kit symlink resolves to (running from
    # {tree}/scripts, where ROOT is the tree).
    for _driver in (tree / 'drivers/Adreno740',
                    ROOT / '..' / 'UniXNU' / 'drivers' / 'Adreno740',
                    (tree / 'kits' / board_kit(tree)).resolve().parent.parent / 'UniXNU' / 'drivers' / 'Adreno740'):
        if _driver.is_dir():
            includes.append(_driver)
            break

    objects, skipped_all = [], []
    # The board package is materialised as kits/<kit> inside the tree, or read from this repository
    # when the recipe runs from the checkout. Under `unixnu` this file is copied into {tree}/scripts, so
    # ROOT is the tree there -- look in all three places (build-gpu-smmu-probe.py does the same).
    # Order matters (2026-11-03 21:45): the tree copies are materialised snapshots and go stale, so a
    # source edit in package/ silently missed the build once already. This repository is the truth.
    # A candidate only counts when it actually holds the probe (2026-11-03 22:15): under `unixnu build
    # kernel` ROOT is the tree, where `package/` exists but holds no probe, so a bare is_dir() test picked
    # it and the archive failed with "missing source".
    package = next((candidate for candidate in (ROOT / 'package', tree / 'kits' / board_kit(tree),
                                                tree / 'package')
                    if (candidate / 'DbyGpuPowerProbe.cpp').is_file()), None)
    if package is None:
        raise SystemExit('no board package: expected {tree}/kits/<kit>, {tree}/package or package/')

    for relative, language, suffix in SOURCES:
        # The board package is read from THIS repository (the tree links it as kits/<kit>); the kit
        # comes from the materialised feed. Same lookup shape as build-dby-touch.py.
        if relative == KIT:
            source = kit
        elif relative == KIT_GMU:
            # The board owns the cold start since 2026-11-04: 100 ms poll (vendor GMU_START_TIMEOUT)
            # and the CM3 fault bit at BIT(15).  Read from THIS repository, not the tree snapshot.
            source = package / 'DbyA650Gmu.c' 
        elif relative == KIT_REGIONS:
            source = tree / KIT_REGIONS_PATH
        elif relative.startswith(SMMU):
            source = smmu_dir / relative[len(SMMU):]
        elif relative.startswith(FIRMWARE):
            source = firmware.parent / relative[len(FIRMWARE):]
        elif relative.startswith(PM4):
            source = pm4_dir / relative[len(PM4):]
        else:
            source = package / Path(relative).name
        if relative not in (KIT, KIT_GMU, KIT_REGIONS) and not relative.startswith((SMMU, FIRMWARE, PM4)):
            repo_copy = ROOT / relative
            if repo_copy.is_file() and source != repo_copy and source.is_file():
                if repo_copy.read_bytes() != source.read_bytes():
                    raise SystemExit(f'stale package copy: {source} differs from {repo_copy}; '
                                     f'refusing to compile the tree snapshot')
            print(f'compiling {source}')
        if not source.is_file():
            raise SystemExit(f'missing source: {source}')
        obj = out / (source.stem + suffix)
        if os.environ.get('PROBE_DEBUG'):
            print('  source  :', source)
            print('  source md5:', __import__('hashlib').md5(source.read_bytes()).hexdigest())
            print('  has raw marker:', b'GPU SMMU raw' in source.read_bytes())
            print('  argv    :', ' '.join(command_from(records_by_lang[language], source, obj, includes)[0]))
            open('/tmp/probe-argv-' + source.stem + '.txt','w').write(repr((command_from(records_by_lang[language], source, obj, includes)[0], command_from(records_by_lang[language], source, obj, includes)[1])))
        command, directory, skipped = command_from(records_by_lang[language], source, obj, includes)
        skipped_all += skipped
        (out / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            print(result.stdout[-1500:], file=sys.stderr, end='')
            raise SystemExit(f'{source}:\n{result.stderr[-3000:]}')
        objects.append(obj)
        print(f'  compiled {source}')
    archive = out / ARCHIVE
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]], check=True)

    exported = verify_archive(archive) if not args.no_verify else {}
    print(f'PASS: {len(objects)} objects archived into {archive}')
    print(f'      {archive.stat().st_size} bytes, sha256 '
          f'{hashlib.sha256(archive.read_bytes()).hexdigest()}')
    if exported:
        print('      symbols: ' + ', '.join(f'{k}={v}' for k, v in exported.items()))
    if skipped_all:
        print('      skipped missing include dirs: ' + ', '.join(sorted(set(skipped_all))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
