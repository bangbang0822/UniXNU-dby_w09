#!/usr/bin/env python3
"""Produce a real verified-root policy (signed manifest + anchor) for one install-image volume.

The official producer is `build-owner-os-policy.py`, which needs macOS (`hdiutil`, `diskutil`) and
the macOS-only `build/verified-root-host/verified-root.dylib`. What it actually does is portable:

  1. take the exact logical media of one volume (here: expand the UDIF `.dmg` block map),
  2. hash it in 4096-byte blocks with `kits/VerifiedRoot/tools/manifest.py`,
  3. sign the 160-byte header with an RSA-3072 owner key,
  4. emit `manifest.bin` (header + signature + hash table), `trusted-key.bin` (the anchor) and
     `manifest.json` / `verification.json`.

The signature is made with *our* owner key and the matching anchor is embedded in the kernel we
build, so the chain is self-contained: no Apple key and no upstream author key is involved.

The media byte count is load-bearing: the kernel matches a policy to a block device by
`image_bytes` (`NativeUfs.cpp`), so it must be the same number `diskutil` reports for the whole
media. For the donor iPad7,1 build that is 6,077,580,800 B (OS) and 3,303,014,400 B
(Cryptex1,SystemOS).

Usage (inside the assembled tree):
    python3 scripts/make-verified-root-policy.py --component os \\
        --dmg ../../ipsw/21H461/044-76040-038.dmg

This is a one-time input step, not an archive step: run it before `unixnu build kernel`.
`build/verified-root-archive.py` picks the policies up from `build/verified-root-os/` and
`build/verified-root-systemos/`.
"""
import argparse
import bz2
import hashlib
import json
import plistlib
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_TOOL = ROOT / 'kits/VerifiedRoot/tools/manifest.py'
KEYS = ROOT / 'build/verified-root-host/keys'

# Whole-media sizes of the donor restore image (see docs/verified-root-stub.md).
EXPECTED_IMAGE_BYTES = {'os': 6077580800, 'systemos': 3303014400}
# Volume UUIDs of the donor volumes; the OS one is the constant build-owner-os-policy.py enforces.
# Volume UUIDs read with fsapfsinfo: the OS one is the constant build-owner-os-policy.py enforces,
# the SystemOS one is the single volume inside 044-76034-038.dmg.
VOLUME_UUID = {'os': '04769D26-2CA0-4825-B443-E2E5F88300F2',
               'systemos': '8846FE78-2910-4297-AF97-D59BB6F8FC16'}

TYPE_ZERO = 0x00000000
TYPE_RAW = 0x00000001
TYPE_IGNORE = 0x00000002
TYPE_COMMENT = 0x7FFFFFFE
TYPE_LAST = 0xFFFFFFFF
TYPE_LZFSE = 0x80000007          # libdmg-hfsplus names this LZFSE; it is what these images use
TYPE_LZFSE_ALT = 0x80000008
TYPE_ZLIB = 0x80000005
TYPE_BZIP2 = 0x80000006


def require(value, message):
    if not value:
        raise SystemExit(message)


def udif_maps(dmg):
    data = dmg.read_bytes()
    koly = data.rfind(b'koly')
    require(koly > 0, 'no koly trailer: not a UDIF image')
    start = data.find(b'<?xml', 0, koly)
    end = data.find(b'</plist>', start) + len(b'</plist>')
    resource = plistlib.loads(data[start:end])
    return data, resource['resource-fork']['blkx']


def block_runs(entry):
    blob = entry['Data']
    require(blob[:4] == b'mish', 'unexpected block map signature')
    base = struct.unpack_from('>Q', blob, 8)[0]
    count = struct.unpack_from('>I', blob, 0xC8)[0]
    runs = []
    for index in range(count):
        kind, _reserved, sector, sectors, offset, length = struct.unpack_from(
            '>IIQQQQ', blob, 0xCC + index * 40)
        if kind in (TYPE_COMMENT, TYPE_LAST):
            continue
        runs.append(dict(kind=kind, sector=base + sector, sectors=sectors,
                         offset=offset, length=length))
    return runs


def expand_media(dmg, media, expected):
    """Rebuild the whole logical media, every partition included, from the UDIF block maps."""
    if media.is_file() and media.stat().st_size == expected:
        print(f'reusing {media} ({expected} B)')
        return media
    if dmg.stat().st_size == expected:
        print(f'{dmg.name} is raw media already ({expected} B)')
        media.write_bytes(dmg.read_bytes())
        return media
    data, maps = udif_maps(dmg)
    runs, end = [], 0
    for entry in maps:
        entry_runs = block_runs(entry)
        runs.extend(entry_runs)
        end = max([end] + [run['sector'] + run['sectors'] for run in entry_runs])
    require(end * 512 == expected,
            f'the block maps cover {end * 512} bytes, expected {expected}')
    runs.sort(key=lambda run: run['sector'])
    print(f'expanding {dmg.name} -> {media.name} ({expected} B, {len(runs)} runs)')
    media.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with media.open('wb') as target:
        for run in runs:
            position, size = run['sector'] * 512, run['sectors'] * 512
            if position > written:
                target.write(b'\0' * (position - written))
                written = position
            if run['kind'] in (TYPE_ZERO, TYPE_IGNORE):
                target.write(b'\0' * size)
            else:
                raw = data[run['offset']:run['offset'] + run['length']]
                if run['kind'] == TYPE_RAW:
                    block = raw
                elif run['kind'] == TYPE_ZLIB:
                    block = zlib.decompress(raw)
                elif run['kind'] == TYPE_BZIP2:
                    block = bz2.decompress(raw)
                elif run['kind'] in (TYPE_LZFSE, TYPE_LZFSE_ALT):
                    try:
                        import lzfse
                    except ImportError:
                        raise SystemExit('python module "lzfse" is required for LZFSE runs')
                    block = lzfse.decompress(raw)
                else:
                    raise SystemExit(f'unsupported UDIF run type {run["kind"]:#x}')
                require(len(block) == size,
                        f'run at sector {run["sector"]} decoded {len(block)} B, expected {size} B')
                target.write(block)
            written += size
        if written < expected:
            target.write(b'\0' * (expected - written))
    actual = media.stat().st_size
    require(actual == expected, f'{media} is {actual} bytes, expected {expected}')
    return media


def owner_keys():
    """One RSA-3072 owner key pair; the anchor is embedded in the kernel we build."""
    private, public = KEYS / 'owner.pem', KEYS / 'owner-public.pem'
    KEYS.mkdir(parents=True, exist_ok=True)
    if not private.is_file():
        subprocess.run(['openssl', 'genrsa', '-out', str(private), '3072'], check=True,
                       capture_output=True)
        print(f'generated owner key {private}')
    if not public.is_file():
        subprocess.run(['openssl', 'rsa', '-in', str(private), '-pubout', '-out', str(public)],
                       check=True, capture_output=True)
        print(f'derived owner public key {public}')
    return private, public


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('os', 'systemos'), required=True)
    parser.add_argument('--dmg', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--volume-uuid', type=uuid.UUID)
    parser.add_argument('--image-bytes', type=int)
    parser.add_argument('--generation', type=int, default=1)
    args = parser.parse_args()

    require(MANIFEST_TOOL.is_file(), f'the kit manifest tool is missing: {MANIFEST_TOOL}')
    require(args.dmg.is_file(), f'{args.dmg} does not exist')
    image_bytes = args.image_bytes or EXPECTED_IMAGE_BYTES[args.component]
    volume_uuid = args.volume_uuid
    if volume_uuid is None:
        require(VOLUME_UUID[args.component], 'a --volume-uuid is required for this component')
        volume_uuid = uuid.UUID(VOLUME_UUID[args.component])

    out = args.out or ROOT / ('build/verified-root-os' if args.component == 'os'
                              else 'build/verified-root-systemos')
    out.mkdir(parents=True, exist_ok=True)
    media = expand_media(args.dmg, out / f'{args.component}-media.img', image_bytes)

    private, public = owner_keys()
    sys.path.insert(0, str(MANIFEST_TOOL.parent))
    import manifest as kit_manifest                       # noqa: E402  (path set above)

    with media.open('rb') as stream:
        record = kit_manifest.make(stream, image_bytes, private, public, volume_uuid,
                                   args.generation, out)

    verification = dict(record)
    verification['blocks_verified'] = record['block_count']
    verification['real_c_signature_and_block_verification'] = True
    (out / 'verification.json').write_text(json.dumps(verification, indent=1) + '\n')

    envelope = (out / 'manifest.bin').read_bytes()
    anchor = (out / 'trusted-key.bin').read_bytes()
    require(len(envelope) == 544 + record['block_count'] * 32, 'unexpected envelope size')
    require(len(anchor) == 395, 'unexpected anchor size')
    print(json.dumps(dict(component=args.component, manifest_bytes=len(envelope),
                          manifest_sha256=hashlib.sha256(envelope).hexdigest(),
                          anchor_sha256=hashlib.sha256(anchor).hexdigest(),
                          image_sha256=record['image_sha256'],
                          block_count=record['block_count'],
                          volume_uuid=record['volume_uuid']), indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
