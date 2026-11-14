#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Derive the two original trust-cache modules from members `unixnu ipsw fetch` extracted.

Upstream reads the IPSW member by member and never downloads it whole; the two inputs
this board's trust bundle needs are among the members the OS profile fetches
(`StaticTrustCache`, `Cryptex1,SystemTrustCache`). This script takes those two IM4P
files, extracts their payloads, checks them against the pins in `proprietary-files.txt`
and the OS profile, writes the static one where the build reads it
(`.cache/firmware/target-volumes/static-trustcache.bin`) and builds the bundle
`scripts/build-original-system-trust.py` would have built - then checks the bundle
against `board.toml`'s `original_trust_cache_sha256`.

It writes nothing outside `.cache/firmware` and `build/original-static-system-trust/`,
and it refuses when a hash does not match rather than writing something plausible.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATIC_PIN = "da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2"
SYSTEM_PIN = "d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651"


def der(data, at):
    if at + 2 > len(data):
        raise SystemExit("truncated DER")
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 128:
        count = size & 127
        if not 0 < count <= 4 or at + count > len(data):
            raise SystemExit("invalid DER length")
        size = int.from_bytes(data[at:at + count], "big")
        at += count
    if at + size > len(data):
        raise SystemExit("DER exceeds its input")
    return tag, data[at:at + size], at + size


def module_of(path):
    """The trust module inside an IM4P: 0x16 0x16 0x16 0x04 fields, payload last."""
    data = path.read_bytes()
    tag, sequence, end = der(data, 0)
    fields = []
    cursor = 0
    while cursor < len(sequence):
        kind, payload, cursor = der(sequence, cursor)
        fields.append((kind, payload))
    if tag != 0x30 or end != len(data) or [kind for kind, _ in fields] != [0x16, 0x16, 0x16, 4]:
        raise SystemExit(f"{path.name}: not the original trust-cache IM4P")
    if fields[0][1] != b"IM4P":
        raise SystemExit(f"{path.name}: not an IM4P")
    return fields[3][1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ipsw-dir", type=Path, required=True,
                        help="directory `unixnu ipsw fetch` wrote (out/ipsw/<build>)")
    parser.add_argument("--write-cache", action="store_true",
                        help="also write .cache/firmware/target-volumes/static-trustcache.bin")
    args = parser.parse_args()

    static_member = args.ipsw_dir / "044-76040-038.dmg.trustcache"
    system_member = args.ipsw_dir / "044-76034-038.dmg.trustcache"
    for path in (static_member, system_member):
        if not path.is_file():
            raise SystemExit(f"{path} is missing; run `unixnu ipsw fetch` first")

    static = module_of(static_member)
    system = module_of(system_member)
    for name, module, pin in (("static", static, STATIC_PIN), ("system", system, SYSTEM_PIN)):
        digest = hashlib.sha256(module).hexdigest()
        if digest != pin:
            raise SystemExit(f"{name} module is {digest}, the build expects {pin}")
        if struct.unpack_from("<I", module)[0] != 2 or len(module) != 24 + struct.unpack_from("<I", module, 20)[0] * 24:
            raise SystemExit(f"{name} module is not a version-2 trust module")

    if args.write_cache:
        cache = ROOT / ".cache" / "firmware" / "target-volumes"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "static-trustcache.bin").write_bytes(static)
        print("wrote", cache / "static-trustcache.bin")

    bundle = struct.pack("<3I", 2, 12, 12 + len(static)) + static + system
    out = ROOT / "build" / "original-static-system-trust"
    out.mkdir(parents=True, exist_ok=True)
    (out / "original-static-system.bundle").write_bytes(bundle)
    (out / "inputs.json").write_text(json.dumps(dict(
        original_static_sha256=hashlib.sha256(static).hexdigest(),
        original_system_sha256=hashlib.sha256(system).hexdigest(),
        static_im4p_sha256=hashlib.sha256(static_member.read_bytes()).hexdigest(),
        system_im4p_sha256=hashlib.sha256(system_member.read_bytes()).hexdigest(),
        bundle_sha256=hashlib.sha256(bundle).hexdigest(),
        derived_from_fetched_members=True,
    ), indent=2) + "\n")

    import tomllib
    board = tomllib.loads((ROOT / "board.toml").read_text(encoding="utf-8"))
    pin = board.get("original_trust_cache_sha256")
    digest = hashlib.sha256(bundle).hexdigest()
    if pin and digest != pin:
        print(f"FAIL bundle is {digest}, board.toml pins {pin}", file=sys.stderr)
        return 1
    print(f"PASS trust bundle {digest} matches board.toml ({len(static)} + {len(system)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
