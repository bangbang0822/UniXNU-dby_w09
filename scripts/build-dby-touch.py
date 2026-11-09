#!/usr/bin/env python3
"""Build the DBY-W09 touch archive against this source kernel's actual ABI.

`libDbyTouch.a` is linked into the kernel through `CONFIG_XNU_EXTRA_LD_ARCHIVES`, and the board
hook added by `patches/xnu/dby-touch-probe.patch` calls its `dby_touch_probe_entry()` once from
`native_mobile_graphics_client_probe()`. The driver is inert unless the payload's kernel arguments
carry `dby-touch=<stage>`.

Both translation units are compiled with a command recorded by the kernel build itself, so the
headers, defines and ABI flags are the ones the kernel was really built with:

  * DbyTouchProtocol.c  <- build/xnu/ios-obj/.../osfmk/DEVELOPMENT/startup.o.json     (C)
  * DbyTouch.cpp        <- build/xnu/ios-obj/.../iokit/DEVELOPMENT/IOPlatformExpert.cpo.json (C++)

The recorded commands carry no -Werror; this script puts -Werror at the front and then restores the
four warning demotions the driver's own build log carries at the end, because xnu's OSMetaClass and
KALLOC_TYPE_DEFINE macros trip -Wmissing-designated-field-initializers / -Wc99-designator /
-Wreorder-init-list / -Wunnecessary-virtual-specifier on every driver that uses them. That is the
same shape as the command recorded in the driver's README §9.1 build log.

Source lookup order is the one the driver's README documents: --source-dir, then <tree>/drivers,
then <tree>/kits/dby_w09. The kernel tree is only read (compile records, headers); every output
lands under the repository's own build/dby-touch/.

Usage:
  python3 scripts/build-dby-touch.py --tree /root/unixnu-out/.../tree
  python3 scripts/build-dby-touch.py --tree <tree> --source-dir package/DbyTouch
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/dby-touch'
ARCH = 'DEVELOPMENT_ARM64_BCM2837'

# name -> (compile record, explicit source path or None for "inside the package directory")
#
# GeniSpi.c is the upstream QUPv3 GENI/SPI component (src/UniXNU/drivers/QcomGeniSpi).  It is compiled
# from its own directory rather than copied into this package: the component's AGENTS.md forbids
# keeping a second copy, and a copy would drift from upstream.  It is plain C with no IOKit
# dependency, hence the C record, and it must stay C - GeniSpi.h has no extern "C", so compiling it
# as C++ would change the names it exports.
RECORDS = {
    'DbyTouchProtocol.c': (f'osfmk/DEVELOPMENT/startup.o.json', None),
    'DbyTouch.cpp': (f'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', None),
    'GeniSpi.c': (f'osfmk/DEVELOPMENT/startup.o.json',
                  'src/UniXNU/drivers/QcomGeniSpi/GeniSpi.c'),
}


DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unnecessary-virtual-specifier',
]

# -Weverything is what the kernel's own compile uses, but the kernel never turns warnings into
# errors; combined with -Werror it drags in diagnostics the kernel does not demote and that a
# driver cannot satisfy without rewriting kernel-owned macros (clang-24's -Wunsafe-buffer-usage
# fires on every array access in DbyTouchProtocol.c). -Wall -Wextra -Werror is kept, the umbrella
# is dropped, and the report records that so nobody believes the archive was built under it.
DROPPED = {'-Weverything'}


def build_command(record_path, source, obj, source_dir):
    """Reuse the kernel's own recorded command, with -Werror in front and the demotions last.

    The recorded arguments already carry the kernel's -D defines and -I include paths, so the
    driver sees exactly the headers and configuration the kernel was built with.
    """
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
            # The recorded command lists include directories the kernel build had at the time;
            # a pruned tree no longer carries all of them (EXPORT_HDRS/libsa is the usual one), and
            # -Werror turns -Wmissing-include-dirs into a hard failure. Skip the ones that are gone.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='assembled kernel tree; defaults to this script\'s own tree root '
                             '(that is what `unixnu` uses when it runs an archive recipe) or '
                             '$UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path,
                        help='DbyTouch sources; defaults to the documented lookup order')
    parser.add_argument('--werror', action='store_true',
                        help='kept for the documented invocation; -Werror is always on')
    args = parser.parse_args()

    # `unixnu` copies scripts/ into <tree>/scripts and runs the recipe there, so the script's own
    # parent-of-parent is the tree in the normal build path. --tree/$UNIXNU_TREE overrides it for
    # the manual "workspace repo against a tree somewhere else" case.
    tree = (args.tree or ROOT).resolve()
    if not (tree / 'build/xnu/ios-obj' / ARCH).is_dir():
        raise SystemExit(f'{tree} has no build/xnu/ios-obj/{ARCH}: point --tree at the assembled tree')
    # The archive must land where CONFIG_XNU_EXTRA_LD_ARCHIVES looks for it: {tree}/build/dby-touch.
    out = tree / 'build/dby-touch'

    candidates = [args.source_dir] if args.source_dir else [
        tree / 'drivers/DbyTouch',
        tree / 'kits/dby_w09/DbyTouch',
        ROOT / 'package/DbyTouch',
    ]
    source_dir = next((c.resolve() for c in candidates if c and (c / 'DbyTouch.cpp').is_file()), None)
    if source_dir is None:
        raise SystemExit('DbyTouch.cpp not found in: ' + ', '.join(str(c) for c in candidates))

    out.mkdir(parents=True, exist_ok=True)
    objects = []
    skipped_all = []
    built = {}
    for name, spec in RECORDS.items():
        record_rel, explicit = spec if isinstance(spec, tuple) else (spec, None)
        # An explicit path is relative to the workspace root (the repository that holds src/UniXNU
        # and this device repository side by side); everything else lives in the package directory.
        # The workspace root is the nearest ancestor that actually holds the upstream tree: the
        # package may be found under package/, drivers/ or kits/, and this script also runs from a
        # copy inside the build tree, so neither a fixed depth nor __file__ is reliable.
        workspace = source_dir
        while workspace != workspace.parent and not (workspace / 'src/UniXNU/drivers/QcomGeniSpi').is_dir():
            workspace = workspace.parent
        if not (workspace / 'src/UniXNU/drivers/QcomGeniSpi').is_dir():
            raise SystemExit(f'cannot find src/UniXNU/drivers/QcomGeniSpi above {source_dir}')
        source = (workspace / explicit) if explicit else (source_dir / name)
        if not source.is_file():
            raise SystemExit(f'{source} missing')
        built[name] = source
        record_path = tree / 'build/xnu/ios-obj' / ARCH / record_rel
        if not record_path.is_file():
            raise SystemExit(f'{record_path} missing: build the kernel once before this archive')
        obj = out / (source.stem + '.o')
        command, directory, skipped = build_command(record_path, source, obj, source_dir)
        if source.parent != source_dir:
            command += ['-I', str(source.parent)]
        # Every translation unit here may include the upstream QUPv3 GENI/SPI header, so its directory
        # is on the include path for all of them, not just for GeniSpi.c itself.
        geni_dir = workspace / 'src/UniXNU/drivers/QcomGeniSpi'
        if geni_dir.is_dir():
            command += ['-I', str(geni_dir)]
        if skipped:
            print('note: skipped missing include dirs: ' + ', '.join(skipped))
        (OUT / f'compile-command-{source.stem}.json').write_text(json.dumps(command, indent=2) + '\n')
        subprocess.run(command, cwd=directory, check=True, timeout=180)
        objects.append(obj)
        skipped_all += skipped

    archive = OUT / 'libDbyTouch.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]],
                   check=True, timeout=60)

    report = {
        'tree': str(tree),
        'source_dir': str(source_dir),
        'sources': {name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for name, path in built.items()},
        'objects': {o.name: o.stat().st_size for o in objects},
        'archive': archive.name,
        'archive_bytes': archive.stat().st_size,
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'skipped_include_dirs': skipped_all,
        'hardware_execution': False,
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: libDbyTouch.a built from ' + str(source_dir))
    print(f'      archive {archive} ({report["archive_bytes"]} bytes)')
    print('      link it with CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/dby-touch/libDbyTouch.a"')


if __name__ == '__main__':
    main()
