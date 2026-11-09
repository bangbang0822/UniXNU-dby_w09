#!/usr/bin/env python3
"""Build libVerifiedRootProbe.a on Linux from the kernel's own compile record.

Two modes:

  * `--policy-dir DIR [--policy-dir DIR2]` — link the real signed envelopes and the anchor that
    `kits/VerifiedRoot/tools/manifest.py` produced (see `_scratch/make_verified_root_policy.py`).
    This is the shape `build-verified-root-probe.py --include-systemos` produces: one
    `_verified_root_manifest` (plus `_verified_root_systemos_manifest`), `_verified_root_key`, and a
    `_verified_root_policies` table of (address, length) records with `_verified_root_policy_count`.
    The owner key is ours and the matching anchor is embedded here, so the chain is self-contained.
  * no policy — link a **LINK-ONLY STUB** instead: `verified_root_policy_count = 0`, an all-zero
    `verified_root_key` and an empty probe. Code that gates on the count (`NativeUfs`,
    `NativeVirtioBlock::configureVerification()`) then refuses to start, which means no storage and
    no root mount. That is documented in `docs/verified-root-stub.md` of the project repository.

Why this exists: `board.toml`'s archive list needs
`CONFIG_XNU_VERIFIED_ROOT_ARCHIVE=build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a`,
but the official producer (`build-verified-root-probe.py --component os --include-systemos`)
cannot run here:

  * it replays `bsd_init.o.json` verbatim, and that record carries `-Weverything` without
    `-Wno-unsafe-buffer-usage`, which the pinned clang-24 turns into 39 errors in
    `kits/VerifiedRoot/src/verified_root.c` (documented in the board.toml archive comment);
  * it calls `xcrun clang` / `xcrun ar`, which do not exist on Linux;
  * it drives `build-owner-os-policy.py`, which needs `hdiutil`/`diskutil` plus the macOS-only
    `build/verified-root-host/verified-root.dylib`;
  * it also compiles the QEMU probe harness, which is test scaffolding, not kernel code.

This script keeps the part that ships in the kernel — the real `verified_root.c` plus the
`xnu_crypto.c` adapter from the same kit — compiles them with the record's flags minus every
`-W*` (the approach `build-arm-driver.py:66-79` already uses), and archives the objects with
`llvm-ar`.

Usage (inside the assembled tree, as an archive step):
    python3 scripts/build-verified-root-archive.py --component os
    python3 scripts/build-verified-root-archive.py --component os \\
        --policy-dir build/verified-root-os --policy-dir build/verified-root-systemos
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

ROOT = Path(__file__).resolve().parents[1]
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
SOURCES = (
    ROOT / 'kits/VerifiedRoot/src/verified_root.c',
    ROOT / 'kits/VerifiedRoot/adapters/xnu_crypto.c',
)
INCLUDE = ROOT / 'kits/VerifiedRoot/include'
AR = 'llvm-ar'
CLANG = os.environ.get('UNIXNU_CLANG') or shutil.which('clang') or 'clang'
# The record's warning set is dropped, so pick a small explicit one (same spirit as build-arm-driver.py).
WARNINGS = ('-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier', '-Wno-unsafe-buffer-usage')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('os', 'systemos'), default='os')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--policy-dir', action='append', default=[], type=Path, metavar='DIR',
                        help='real policy directory (repeat for OS then SystemOS); when omitted the '
                             'archive links the zero-policy stub')
    args = parser.parse_args()

    if not RECORD.is_file():
        raise SystemExit(f'compile record missing: {RECORD}; run one kernel build first')
    output = args.output or ROOT / ('build/verified-root-os-xnu-bundle' if args.component == 'os'
                                    else 'build/verified-root-xnu-bundle') / 'libVerifiedRootProbe.a'
    output.parent.mkdir(parents=True, exist_ok=True)

    if not args.policy_dir:
        # The official build expects the policies at these two paths; use them when they exist so a
        # tree that has them links the real bundle without a board.toml change, and a fresh tree
        # still builds (with the stub).
        args.policy_dir = [directory for directory in (ROOT / 'build/verified-root-os',
                                                       ROOT / 'build/verified-root-systemos')
                           if (directory / 'manifest.bin').is_file()]
    if args.policy_dir:
        return build_with_policies(args, output)
    return build_stub(output)


def build_with_policies(args, output):
    """Link the real manifest(s) and the anchor, the shape build-verified-root-probe.py produces."""
    policies = []
    anchor = None
    for index, directory in enumerate(args.policy_dir, 1):
        envelope = directory / 'manifest.bin'
        key = directory / 'trusted-key.bin'
        if not envelope.is_file() or not key.is_file():
            raise SystemExit(f'{directory} is not a policy directory (manifest.bin / trusted-key.bin)')
        blob = envelope.read_bytes()
        # Envelope = 160-byte header + 384-byte signature + one 32-byte digest per 4096-byte block.
        if len(blob) < 544:
            raise SystemExit(f'{envelope} is shorter than a verified-root envelope')
        blocks = struct.unpack_from('<Q', blob, 32)[0]
        if len(blob) != 544 + blocks * 32:
            raise SystemExit(f'{envelope} is not a verified-root envelope '
                             f'(header says {blocks} blocks)')
        key_bytes = key.read_bytes()
        if len(key_bytes) != 395 or not key_bytes.startswith(b'VROOTKEY'):
            raise SystemExit(f'{key} is not a verified-root anchor')
        if anchor is not None and key_bytes != anchor:
            raise SystemExit('every policy must share one anchor')
        anchor = key_bytes
        policies.append((f'_verified_root_{"" if index == 1 else "systemos_"}manifest', envelope, len(blob)))
    if len(policies) > 16:
        raise SystemExit('the driver accepts at most 16 policies')

    assembly = output.parent / 'verified-root-inputs.S'
    lines = ['.section __DATA_CONST,__const', '.balign 8',
             '.globl _verified_root_manifest, _verified_root_manifest_bytes',
             '_verified_root_manifest:']
    lines += ['.incbin "' + str(policies[0][1]) + '"', '_verified_root_manifest_bytes:',
              '.quad . - _verified_root_manifest', '.balign 8', '.globl _verified_root_key',
              '_verified_root_key:',
              '.incbin "' + str(args.policy_dir[0] / 'trusted-key.bin') + '"']
    if len(policies) > 1:
        lines += ['.balign 8', '.globl _verified_root_systemos_manifest',
                  '_verified_root_systemos_manifest:',
                  '.incbin "' + str(policies[1][1]) + '"']
    lines += ['.balign 8', '.globl _verified_root_policies, _verified_root_policy_count',
              '_verified_root_policies:']
    lines += [f'.quad {label}, {length}' for label, _, length in policies]
    lines += ['_verified_root_policy_count:', f'.quad {len(policies)}']
    assembly.write_text('\n'.join(lines) + '\n')

    probe = output.parent / 'verified-root-probe-stub.c'
    probe.write_text('/* The QEMU probe harness is test scaffolding; the kernel links only this entry. */\n'
                     'struct vr_verified_state;\nvoid native_verified_root_probe(void) { }\n')

    return compile_and_archive(output, [*SOURCES, probe, assembly],
                               {'link_only_stub': False, 'verified_root_policy_count': len(policies),
                                'policies': [{'manifest': str(path), 'bytes': length,
                                              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                                             for _, path, length in policies],
                                'anchor_sha256': hashlib.sha256(anchor).hexdigest(),
                                'note': 'Real verified-root policies signed with the owner key whose anchor '
                                        'is embedded here; no Apple or author key is involved.'})


def build_stub(output):
    """Link the zero-policy stub so a kernel can be built without any policy input."""
    defaults = output.parent / 'verified-root-defaults.c'
    defaults.write_text(
        '/* Local defaults: no policy input was supplied, so verified root stays disabled. */\n'
        '#include <stdint.h>\n'
        '#define VR_KEY_BYTES 395u\n'
        'const uint8_t verified_root_key[VR_KEY_BYTES] = {0};\n'
        'const uint64_t verified_root_policy_count = 0;\n'
        'const uint8_t verified_root_policies[16] = {0};\n'
        'void native_verified_root_probe(void) { }\n'
    )
    return compile_and_archive(output, [*SOURCES, defaults],
                               {'link_only_stub': True, 'verified_root_policy_count': 0,
                                'note': 'LINK-ONLY STUB: kit sources compiled, but the signed policy bundle '
                                        'is absent, so verified_root_policy_count=0 and the anchor key is all '
                                        'zeros. Verified root is NOT enabled; components that gate on the '
                                        'policy count refuse to start. See docs/verified-root-stub.md.'})


def compile_and_archive(output, sources, extra_manifest):
    record = json.loads(RECORD.read_text().strip().rstrip(','))
    objects = []
    for source in sources:
        if not source.is_file():
            raise SystemExit(f'missing source: {source}')
        destination = output.parent / (source.stem + '.o')
        if source.suffix.lower() in ('.s', '.asm'):
            # The compile record belongs to a C file, so it forces a language; assemble directly with
            # the same target instead (the kit's data blob is `.incbin` and needs the assembler).
            target = next((arg.split('=', 1)[1] for arg in record['arguments']
                           if arg.startswith('--target=')), 'arm64-apple-ios17.0')
            command = [CLANG, '-target', target, '-c', str(source), '-o', str(destination)]
            result = subprocess.run(command, capture_output=True, text=True)
        else:
            arguments, command, index = record['arguments'], [], 0
            while index < len(arguments):
                value = arguments[index]
                if value == record['file']:
                    command.append(str(source))
                elif value == '-o':
                    command += ['-o', str(destination)]
                    index += 1
                elif value in ('-MF', '-MJ'):
                    index += 1
                elif value in ('-MD', '-MP'):
                    pass
                elif value.startswith('-W'):
                    pass
                else:
                    command.append(value)
                index += 1
            command += ['-I', str(INCLUDE), *WARNINGS]
            result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
        (output.parent / (source.stem + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise SystemExit(f'{source.name} failed:\n{(result.stderr or result.stdout)[-3000:]}')
        data = destination.read_bytes()
        if struct.unpack_from('<I', data)[0] != 0xFEEDFACF:
            raise SystemExit(f'{destination.name} is not an ARM64 Mach-O object')
        objects.append(destination)

    output.unlink(missing_ok=True)
    subprocess.run([AR, 'rcs', str(output), *map(str, objects)], check=True)
    manifest = {
        'archive': str(output),
        'archive_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'record': str(RECORD),
        'sources': {str(s): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
        'warnings': list(WARNINGS),
        **extra_manifest,
    }
    (output.parent / 'archive.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'{output} ({output.stat().st_size} bytes, {len(objects)} objects)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
