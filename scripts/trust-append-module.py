#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""trust-append-module.py IN OUT CDHASH [CDHASH ...]: copy a trust bundle (bundle-ipsw-trust.py layout) and append one version-2
module with the given CDHashes (hash type 2). The IPSW modules stay byte for byte; the kernel loads the new one as an engineering module."""
import struct
import sys
import uuid
from pathlib import Path

if len(sys.argv) < 4:
    sys.exit(__doc__)
data = Path(sys.argv[1]).read_bytes()
# The input bundle is external: its count and offset table used to be trusted outright, so a truncated or
# foreign file either raised inside struct or produced modules sliced out of unrelated bytes. Every field the
# tool slices with is checked first.
if len(data) < 8:
    sys.exit('input bundle is too short to hold a module count and one offset')
count = struct.unpack_from('<I', data)[0]
if not 1 <= count <= 64:
    sys.exit(f'input bundle declares {count} modules, which is not a bundle this tool can append to')
if len(data) < 4 + 4 * count:
    sys.exit('input bundle is shorter than its own offset table')
starts = list(struct.unpack_from(f'<{count}I', data, 4)) + [len(data)]
if any(b <= a for a, b in zip(starts, starts[1:])):
    sys.exit('input bundle offsets are not strictly ascending')
if starts[0] < 4 + 4 * count:
    sys.exit('input bundle\'s first module starts inside its own offset table')
modules = [data[starts[i]:starts[i + 1]] for i in range(count)]
if any(len(module) < 24 for module in modules):
    sys.exit('input bundle has a module shorter than its own header')
hashes = sorted({bytes.fromhex(h) for h in sys.argv[3:]})
assert all(len(h) == 20 for h in hashes), 'CDHashes are 20 bytes'
ident = uuid.uuid5(uuid.NAMESPACE_URL, 'org.unixnu.owner:' + ''.join(h.hex() for h in hashes)).bytes
modules.append(struct.pack('<I16sI', 2, ident, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes))
cursor, offsets = 4 + 4 * len(modules), []
for m in modules:
    offsets.append(cursor)
    cursor += len(m)
Path(sys.argv[2]).write_bytes(struct.pack(f'<{len(modules) + 1}I', len(modules), *offsets) + b''.join(modules))
print(f'{sys.argv[2]}: {count} -> {len(modules)} modules, last has {len(hashes)} hashes')
