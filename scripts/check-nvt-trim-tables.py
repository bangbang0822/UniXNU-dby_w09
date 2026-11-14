#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare the two N9Z version/trim tables this repository carries.

`package/NvtProtocol.c` is a pinned copy of ShengNovatekTouch and holds `nvt_trim_table`;
`package/DbyTouch/DbyTouchProtocol.c` holds the touch path's own copy (`kN9zTrim` and `kN9zMask`).
They agreed byte for byte when they were last read, and nothing kept them agreeing: a row added or a mask
corrected in one left the other behind, while both feed the same panel's identify step. This compares the
initialisers, so it needs neither compiler nor hardware.

The two files sit in different places depending on what it runs against: this repository keeps both under
`package/`, while the assembled tree puts the board package under `kits/<kit>/` and the pinned component
under `drivers/ShengNovatekTouch/`. It looks for each in its own list of candidates and fails when one is
missing - a check that silently skips is the thing this file exists to prevent.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEX_ROW = re.compile(r"\{\s*" + r"\s*,\s*".join([r"(0x[0-9A-Fa-f]{1,2})"] * 6) + r"\s*\}")
DEC_ROW = re.compile(r"\{\s*" + r"\s*,\s*".join([r"(\d)"] * 6) + r"\s*\}")


def first_existing(candidates):
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def block(text, marker):
    start = text.index(marker)
    return text[start:text.index("};", start)]


def pairs(text, id_marker, mask_marker):
    ids = [tuple(int(v, 16) for v in row) for row in HEX_ROW.findall(block(text, id_marker))]
    masks = [tuple(int(v) for v in row) for row in DEC_ROW.findall(block(text, mask_marker))]
    return list(zip(ids, masks))


def main():
    board = next((path for path in (ROOT / 'kits' / 'dby_w09', ROOT / 'kits' / 'sheng', ROOT / 'package')
                  if path.is_dir()), ROOT / 'package')
    device = first_existing([board / 'DbyTouch' / 'DbyTouchProtocol.c', ROOT / 'package' / 'DbyTouch' / 'DbyTouchProtocol.c'])
    pinned = first_existing([board / 'NvtProtocol.c', ROOT / 'package' / 'NvtProtocol.c',
                             ROOT / 'drivers' / 'ShengNovatekTouch' / 'NvtProtocol.c'])
    if device is None or pinned is None:
        print(f'FAIL trim tables: not both found (device {device}, pinned {pinned})', file=sys.stderr)
        return 1
    left = pairs(device.read_text(encoding='utf-8'), 'static const uint8_t kN9zTrim', 'static const uint8_t kN9zMask')
    right = pairs(pinned.read_text(encoding='utf-8'),
                  'static const struct nvt_trim_entry nvt_trim_table',
                  'static const struct nvt_trim_entry nvt_trim_table')
    if len(left) != len(right):
        print(f'FAIL trim tables: {device.name} has {len(left)} rows, {pinned.name} has {len(right)}', file=sys.stderr)
        return 1
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            print(f'FAIL trim tables: row {index}: {device.name} {a}, {pinned.name} {b}', file=sys.stderr)
            return 1
    print(f'PASS trim tables: both copies carry the same {len(left)} rows and masks')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
