"""Build the staged AGXAccelerator class into this board's driver archive.

NativeAdrenoAccelerator.cpp derives from the ORIGINAL kernel's IOAcceleratorES, and the base
class is only reachable through three absolute addresses recorded from that kernel
(scripts/data/ioaccelerator-base-abi.json): the protected-compatible constructor alias, the
non-deleting destructor and the metaclass object. Those addresses belong to the donor build
`ipados-17.7.11-21H461`, which is also this board's donor, so the binding is legitimate here -
but it is still only valid if the live class really is the one that was audited, which is why
the board probe verifies the metaclass address and class size before it ever calls into this
object (mode 25).

The object is built as a "late" archive exactly the way scripts/build-adreno740-accelerator.py
does it for the reference board:
  * compile with the kernel's own recorded command, so the layout matches the kernel's headers;
  * refuse if the undefined IOAcceleratorES imports are anything other than the three audited ones;
  * bind the three by `.set` in a small assembly file;
  * remove __mod_init_func (the initializer must NOT run at kext load) and globalize
    __GLOBAL__sub_I_NativeAdrenoAccelerator.cpp so the board can call it at the right moment;
  * prove only that registration changed (compare __text before and after the strip);
  * add both objects to build/adreno740/libAdreno740.a, which the kernel force-loads.

Usage (from the assembled tree, i.e. what `unixnu` does for an archive recipe):
  python3 scripts/build-adreno650-accelerator.py
  python3 scripts/build-adreno650-accelerator.py --tree <tree>
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

OUT_DIR = Path('build/adreno740-accelerator')
DRIVER_ARCHIVE = Path('build/adreno740/libAdreno740.a')
SOURCE = 'drivers/Adreno740/NativeAdrenoAccelerator.cpp'
RECORDED_ABI = 'scripts/data/ioaccelerator-base-abi.json'
ARCHIVER = shutil.which('llvm-ar') or shutil.which('ar') or 'ar'


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=600, **kw)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def macho(raw):
    magic = struct.unpack_from('<I', raw, 0)[0]
    if magic != 0xfeedfacf:
        raise SystemExit('not a 64-bit Mach-O')
    ncmds = struct.unpack_from('<I', raw, 16)[0]
    off = 32
    commands = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', raw, off)
        commands.append((cmd, off, size))
        off += size
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None)
    args = parser.parse_args()
    tree = args.tree.resolve() if args.tree else Path(__file__).resolve().parents[1]
    # The driver sources are not in the assembled tree (schema_version 2 keeps only the board
    # package there), so fall back the way build-adreno650.py does: beside the device, then beside
    # this script's parent.
    candidates = [tree / 'drivers/Adreno740']
    # tree/kits/dby_w09 is a symlink to the board package once the kernel has assembled the tree;
    # the core checkout (and therefore drivers/Adreno740) sits beside the device directory.
    link = tree / 'kits' / 'dby_w09'
    if link.exists():
        device = link.resolve().parent
        candidates.append(device.parent / 'UniXNU' / 'drivers' / 'Adreno740')
    candidates.append(Path(__file__).resolve().parents[2] / 'UniXNU' / 'drivers' / 'Adreno740')
    driver = next((p for p in candidates if (p / 'NativeAdrenoAccelerator.cpp').is_file()), None)
    if driver is None:
        sys.exit('cannot find drivers/Adreno740/NativeAdrenoAccelerator.cpp; tried %s'
                 % ', '.join(str(p) for p in candidates))
    source = driver / 'NativeAdrenoAccelerator.cpp'

    out = tree / OUT_DIR
    out.mkdir(parents=True, exist_ok=True)

    abi_path = tree / RECORDED_ABI
    if not abi_path.is_file():
        sys.exit('missing %s: the audited base-class addresses are the whole point' % RECORDED_ABI)
    abi = json.loads(abi_path.read_text(encoding='utf-8'))
    if abi.get('operational_overrides') or abi.get('instance_bytes') != 136:
        sys.exit('the recorded base class is not a clean 136-byte IOAcceleratorES')
    bindings = {
        '__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(abi['protected_compatible_constructor_aliases'][0], 16),
        '__ZN15IOAcceleratorESD2Ev': int(abi['nondeleting_destructor'], 16),
        '__ZN15IOAcceleratorES10gMetaClassE': int(abi['metaclass'], 16),
    }

    # The kernel's own recorded C++ compile command gives the matching ABI and headers.
    record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not record_path.is_file():
        sys.exit('missing the kernel compile record: build the kernel once first')
    record = json.loads(record_path.read_text().strip().rstrip(','))
    cmd, i = [], 0
    while i < len(record['arguments']):
        v = record['arguments'][i]
        if v == record['file']:
            cmd.append(str(source))
        elif v == '-o':
            cmd += ['-o', str(out / 'NativeAdrenoAccelerator.o')]
            i += 1
        elif v in ('-MF', '-MJ'):
            i += 1
        elif not v.startswith('-W') and v not in ('-MD', '-MP', '-Xclang'):
            cmd.append(v)
        i += 1
    cmd += ['-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-overloaded-virtual',
            '-I', str(tree / '.cache/xnu-work/osfmk')]
    subprocess.run(cmd, cwd=record['directory'], check=True)

    obj = out / 'NativeAdrenoAccelerator.o'
    raw = obj.read_bytes()
    commands = macho(raw)
    st = next(off for c, off, _ in commands if c == 2)
    at, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    undefined = set()
    for k in range(count):
        ix, kind, _section, _res, _va = struct.unpack_from('<IBBHQ', raw, at + k * 16)
        name = raw[strings + ix:raw.index(b'\0', strings + ix)].decode()
        if kind == 1 and 'IOAcceleratorES' in name:
            undefined.add(name)
    extra = undefined - set(bindings)
    if extra or not undefined:
        sys.exit('unexpected original-base imports: %s' % sorted(extra or undefined))

    asm = out / 'original-bindings.S'
    asm.write_text(''.join('.globl %s\n.set %s, %#x\n' % (n, n, a) for n, a in sorted(bindings.items())
                           if n in undefined))
    # The binding file needs the same target triple the kernel objects use; the compiler that
    # compiled NativeAdrenoAccelerator.cpp above is the right one, not a macOS xcrun.
    run(['clang', '-target', 'arm64-apple-ios17.0', '-c', asm,
         '-o', out / 'original-bindings.o'])

    late = out / 'NativeAdrenoAccelerator.late.o'
    # llvm-objcopy's --globalize-symbol exists only for ELF on this host, and both edits are
    # mechanical, so they are made directly on the Mach-O (scripts/macho-late.py).
    run([sys.executable, str(Path(__file__).resolve().parent / 'macho-late.py'), obj, late])

    def text(path):
        b = path.read_bytes()
        for c, off, _ in macho(b):
            if c == 0x19:                      # LC_SEGMENT_64
                segname = b[off + 8:off + 24].rstrip(b'\0')
                nsects = struct.unpack_from('<I', b, off + 64)[0]
                so = off + 72
                for _ in range(nsects):
                    sect = b[so:so + 16].rstrip(b'\0')
                    if sect == b'__text':
                        size = struct.unpack_from('<Q', b, so + 40)[0]
                        foff = struct.unpack_from('<I', b, so + 48)[0]
                        return b[foff:foff + size]
                    so += 80
        raise SystemExit('no __text')

    if text(obj) != text(late):
        sys.exit('only the owned initializer registration may change')

    archive = out / 'libAdrenoAccelerator.late.a'
    archive.unlink(missing_ok=True)
    run([ARCHIVER, 'rcs', archive, late, out / 'original-bindings.o'])

    driver = tree / DRIVER_ARCHIVE
    if not driver.is_file():
        # A warning here would leave the kernel linking a probe that references symbols
        # nobody defines, so a missing archive is fatal: board.toml orders this recipe
        # after the driver one.
        sys.exit('missing %s: run build-adreno650.py first' % DRIVER_ARCHIVE)
    run([ARCHIVER, 'rcs', driver, late, out / 'original-bindings.o'])
    members = run([ARCHIVER, 't', driver], text=True).split()
    if members.count('NativeAdrenoAccelerator.late.o') != 1:
        sys.exit('the accelerator member is not in %s exactly once' % DRIVER_ARCHIVE)
    print('PASS: staged AGXAccelerator added to %s (%d B, sha256 %s, %d members)'
          % (DRIVER_ARCHIVE, driver.stat().st_size, sha(driver), len(members)))
    (out / 'build-report.json').write_text(json.dumps(dict(
        source_sha256=sha(source),
        abi_sha256=sha(abi_path),
        bindings={n: hex(a) for n, a in bindings.items()},
        archive_sha256=sha(archive),
        original_base_bytes=abi['instance_bytes'],
        original_instructions_modified=False,
        runtime_qualified=False,
    ), indent=2) + '\n')


if __name__ == '__main__':
    main()
