#!/usr/bin/env python3
"""Add the compositor's 8-bit framebuffer preferences to a UniXNU Data seed.

This is the upstream repair for the 10-bit path: profiles and drivers assume an 8-bit BGRA
scanout, and the render server only asks for one when it is told the framebuffer is not
"deep". scripts/make-data-seed.py:88-91 and scripts/add-gpu-plugin-data.py:88-93 both write
the three switches for the process's own domain and the global one, because the preference is
read when backboardd starts - before any plugin or kernel code can run.

This script does only that, against an already-produced seed, so a board whose seed came from
elsewhere can be brought in line without regenerating it.

Gates, all of which exit non-zero rather than proceeding:
  * the APFS volume UUID must read as a UUID and must equal --expect-uuid, because the kernel
    is compiled against that value (configs/*_defconfig CONFIG_XNU_OWNER_DATA_VOLUME_UUID);
  * every existing key of every plist must survive the rewrite (a plist rewrite regenerates
    the whole file, so this is asserted rather than assumed) and the key names are logged;
  * the raw image must still carry the NXSB container magic before it is packed.

  macOS only: the APFS container is attached with hdiutil. Everything else is portable.

Usage:
  python3 patch-data-seed-prefs.py BASE.dbr OUTPUT.dbr [--packed-ramdisk PATH]
      [--expect-uuid UUID]
"""
import argparse
import json
import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SWITCHES = {
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}
HOMES = ('root', 'mobile')
NAMES = ('backboardd.plist', '.GlobalPreferences.plist')
UUID_RE = re.compile(r'^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$')
# The value configs/dby_w09_defconfig pins; overridable so the script is not board-locked.
DEFAULT_EXPECT_UUID = '2138CDAC-F86A-4DED-9EF1-09F27363DB2A'


def load_packed_ramdisk(explicit):
    """Import the core's packed_ramdisk module from an explicit path or the usual places."""
    import importlib.util
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    here = Path(__file__).resolve().parent
    candidates += [
        here / 'packed_ramdisk.py',
        here.parent / 'drivers/DarwinBoot/Test/packed_ramdisk.py',
    ]
    for cand in candidates:
        if cand.is_file():
            spec = importlib.util.spec_from_file_location('packed_ramdisk', cand)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            print(f'packed_ramdisk: {cand}')
            return mod
    sys.exit('packed_ramdisk.py not found; pass --packed-ramdisk')


def volume_uuid(mount):
    """Read the APFS volume UUID. Returns None when it cannot be read - never a placeholder,
    because a placeholder would make the before/after comparison pass vacuously."""
    try:
        info = plistlib.loads(subprocess.check_output(
            ['diskutil', 'info', '-plist', str(mount)]))
    except Exception:                                   # noqa: BLE001
        return None
    return info.get('VolumeUUID') or info.get('APFSVolumeUUID')


def require_uuid(value, when):
    if not value:
        sys.exit(f'cannot read the volume UUID {when}; refusing to continue')
    if not UUID_RE.match(value):
        sys.exit(f'volume UUID {value!r} {when} is not a UUID; refusing to continue')
    return value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('output')
    ap.add_argument('--packed-ramdisk')
    ap.add_argument('--expect-uuid', default=DEFAULT_EXPECT_UUID,
                    help='the UUID the kernel was compiled against; required, because the kernel '
                         'looks the seed up by it. Pass --allow-any-uuid to bypass with a warning.')
    ap.add_argument('--allow-any-uuid', action='store_true',
                    help='skip the UUID comparison; only for inspecting a foreign seed')
    args = ap.parse_args()

    base, output = Path(args.base).resolve(), Path(args.output).resolve()
    if not base.is_file():
        sys.exit(f'{base} is not a file')
    if output.exists():
        sys.exit(f'refusing to overwrite {output}')
    if sys.platform != 'darwin':
        sys.exit('this script needs macOS: hdiutil attaches the APFS container')
    if args.allow_any_uuid:
        print('WARNING: --allow-any-uuid, the volume UUID is NOT compared against the kernel pin')
        args.expect_uuid = ''
    elif not args.expect_uuid:
        sys.exit('--expect-uuid is required: the kernel is compiled against that value '
                 '(configs/*_defconfig CONFIG_XNU_OWNER_DATA_VOLUME_UUID). '
                 'Use --allow-any-uuid only to inspect a foreign seed.')

    packed_ramdisk = load_packed_ramdisk(args.packed_ramdisk)
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    print(f'seed {base.name}: {len(blob)} bytes packed, {logical} bytes logical, {len(extents)} extents')

    report = {'base': str(base), 'output': str(output), 'expect_uuid': args.expect_uuid,
              'plists': []}

    with tempfile.TemporaryDirectory(prefix='data-seed-prefs-') as td:
        raw = Path(td) / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset)
                f.write(data)

        mount = Path(td) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse',
             '-owners', 'off', '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                sys.exit('the seed has no /mobile; wrong base image?')

            uuid_before = require_uuid(volume_uuid(mount), 'before the write')
            print(f'volume uuid before: {uuid_before}')
            if args.expect_uuid and uuid_before.upper() != args.expect_uuid.upper():
                sys.exit(f'volume uuid {uuid_before} does not match the expected '
                         f'{args.expect_uuid}; the kernel is compiled against that value')
            report['volume_uuid'] = uuid_before

            for home in HOMES:
                directory = mount / home / 'Library/Preferences'
                directory.mkdir(parents=True, exist_ok=True)
                for name in NAMES:
                    file = directory / name
                    existed = file.exists()
                    values = plistlib.loads(file.read_bytes()) if existed else {}
                    keys_before = sorted(values)
                    values.update(SWITCHES)
                    keys_after = sorted(values)
                    lost = sorted(set(keys_before) - set(keys_after))
                    if lost:
                        sys.exit(f'{home}/{name}: the rewrite would drop {lost}; refusing')
                    file.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
                    file.chmod(0o644)
                    print(f'  wrote {home}/Library/Preferences/{name}: {len(keys_after)} keys'
                          f'{" (created)" if not existed else ""}')
                    print(f'    keys before: {keys_before}')
                    print(f'    keys after : {keys_after}')
                    report['plists'].append({'home': home, 'name': name, 'existed': existed,
                                             'keys_before': keys_before, 'keys_after': keys_after})

            # Prove the write landed, and that nothing else was lost, before detaching.
            for entry in report['plists']:
                path = mount / entry['home'] / 'Library/Preferences' / entry['name']
                back = plistlib.loads(path.read_bytes())
                missing_keys = [k for k in entry['keys_before'] if k not in back]
                bad = [k for k in SWITCHES if back.get(k) is not False]
                if missing_keys or bad:
                    sys.exit(f"{entry['home']}/{entry['name']}: lost {missing_keys} "
                             f"or the switches are not False ({bad})")
                if sorted(back) != entry['keys_after']:
                    sys.exit(f"{entry['home']}/{entry['name']}: key set drifted after the write")

            uuid_after = require_uuid(volume_uuid(mount), 'after the write')
            print(f'volume uuid after:  {uuid_after}')
            if uuid_before != uuid_after:
                sys.exit('the volume UUID changed; the kernel is compiled against it')
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)

        # The container must still be an APFS container before it is packed. Read a page, not
        # the whole image: this file is the full logical size.
        with raw.open('rb') as f:
            head = f.read(4096)
        if head[32:36] != b'NXSB':
            sys.exit(f'the image no longer starts with an APFS container (bytes 32..36 = '
                     f'{head[32:36]!r}); refusing to pack')
        print('container magic after detach: NXSB')

        metadata = packed_ramdisk.pack(raw, output)
    report['metadata'] = metadata
    print(json.dumps(report, indent=2, default=str))


if __name__ == '__main__':
    main()
