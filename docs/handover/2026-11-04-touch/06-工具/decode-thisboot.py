#!/usr/bin/env python3
"""Decode the current boot's archived pages out of a whole-window boot-log image.

Why not decode-window-2230.py
-----------------------------
That tool ranks slots by their highest r= and prints the newest three. On a device that
boots more than once into the same 509-slot window, the highest r= is usually a *previous*
boot: on 2026-11-03 12:05 the newest three slots were r=15915..15917 at t=32516 s, i.e. a
nine-hour-old session, while the boot being inspected had only written slots_written=121
(rounds=121, uptime_s=252). Reporting those as "this boot" is how the earlier round
concluded the touch reader had never run.

What this tool does instead
---------------------------
The writer stamps every slot with the round number, and it writes slot index r % slot_count.
So a slot belongs to the current boot exactly when its r= equals its slot index and
r < slots_written. That set is what this tool prints.

Usage:
  python3 work/verify/decode-thisboot.py evidence/edl-log-s19/bootlog-window.img
  python3 work/verify/decode-thisboot.py <img> --page all     # every this-boot slot
  python3 work/verify/decode-thisboot.py <img> --rows 42      # only the matching rows
"""

import argparse
import re
import struct
import sys

BLOCK = 4096
SLOT_FIRST = 33
SLOT_BLOCKS = 16

HEADER = [
    ('magic', 0, 'str8'), ('version', 8, 'u32'), ('header_bytes', 12, 'u32'),
    ('boot_id', 16, 'u64'), ('first_lba', 24, 'u64'), ('window_blocks', 32, 'u32'),
    ('msg_first', 36, 'u32'), ('msg_blocks', 40, 'u32'), ('msg_bytes', 44, 'u32'),
    ('slot_first', 48, 'u32'), ('slot_blocks', 52, 'u32'), ('slot_count', 56, 'u32'),
    ('slots_written', 60, 'u32'), ('uptime_s', 64, 'u32'), ('rounds', 68, 'u32'),
    ('write_errors', 72, 'u32'), ('gpt_open', 76, 'u32'), ('gpt_last', 80, 'u32'),
    ('state', 84, 'u32'),
]

ROUND = re.compile(r'#slot (\d+) t=(\d+)s')
LOGS = re.compile(r'LOGS st=(\d+) r=(\d+)')


def parse_header(blob):
    out = {}
    for name, off, kind in HEADER:
        if kind == 'str8':
            out[name] = blob[off:off + 8].decode('ascii', 'replace').rstrip('\x00')
        elif kind == 'u32':
            out[name] = struct.unpack_from('<I', blob, off)[0]
        else:
            out[name] = struct.unpack_from('<Q', blob, off)[0]
    out['tag'] = blob[88:120].decode('ascii', 'replace').rstrip('\x00')
    return out


def slot_text(data, index):
    off = (SLOT_FIRST + index * SLOT_BLOCKS) * BLOCK
    chunk = data[off:off + SLOT_BLOCKS * BLOCK]
    if len(chunk) < SLOT_BLOCKS * BLOCK:
        return None
    return chunk.replace(b'\x00', b'\n').decode('utf-8', 'replace')


def slot_round(text):
    m = ROUND.search(text) or LOGS.search(text)
    return int(m.group(1) if m.re is ROUND else m.group(2)) if m else None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', help='bootlog-window.img read from LBA 30684672')
    parser.add_argument('--page', choices=('newest', 'boot', 'all'), default='newest',
                        help='which this-boot page to print in full (default newest)')
    parser.add_argument('--rows', default=None,
                        help='comma-separated row prefixes to keep (e.g. 42,CPUFREQ,GPU)')
    args = parser.parse_args()

    data = open(args.image, 'rb').read()
    hdr = parse_header(data)
    if hdr['magic'] != 'SHLOG001':
        sys.exit(f'magic is {hdr["magic"]!r}: the writer never ran')
    if hdr['window_blocks'] < SLOT_FIRST + hdr['slot_count'] * SLOT_BLOCKS:
        print(f'NOTE: window_blocks={hdr["window_blocks"]} is smaller than '
              f'{SLOT_FIRST + hdr["slot_count"] * SLOT_BLOCKS}: the image is truncated')

    print(f'== header: boot_id={hdr["boot_id"]} tag={hdr["tag"]} state={hdr["state"]} ==')
    print(f'   rounds={hdr["rounds"]} slots_written={hdr["slots_written"]} '
          f'uptime_s={hdr["uptime_s"]} slot_count={hdr["slot_count"]} '
          f'write_errors={hdr["write_errors"]}')
    print(f'   first_lba={hdr["first_lba"]} window_blocks={hdr["window_blocks"]}')

    # The ring wraps: slot index = r % slot_count, so `r == index` only holds for the first slot_count
    # rounds. A boot that lives longer than slot_count * period (509 * 2 s = ~17 min) needs the modulo
    # form, otherwise every slot is filtered out and the tool claims "the writer never ran" (that is
    # exactly what happened to the 2.3-hour stage28 boot, 2026-11-03 18:20).
    mine = []
    count = hdr['slot_count'] or 1
    for index in range(count):
        text = slot_text(data, index)
        if text is None:
            break
        r = slot_round(text)
        if r is not None and r % count == index and r < hdr['slots_written']:
            mine.append((r, index, text))

    print(f'\n== slots that belong to this boot (r == index and r < {hdr["slots_written"]}): '
          f'{len(mine)} ==')
    if not mine:
        sys.exit('none: either the writer never ran this boot, or slots_written is stale')
    print(f'   r={mine[0][0]}..{mine[-1][0]}  '
          f'(stride {hdr["uptime_s"] / max(1, len(mine)):.1f} s per slot)')

    keep = None
    if args.rows:
        keep = tuple(part.strip() for part in args.rows.split(',') if part.strip())

    def wanted(line):
        if keep is None:
            return True
        return any(line.lstrip().startswith(k) or k in line.split(' ')[0] for k in keep)

    selection = {'newest': [mine[-1]], 'boot': [mine[0]],
                 'all': mine}[args.page]
    print(f'\n== pages: {args.page} ({len(selection)}) ==')
    for r, index, text in selection:
        print(f'\n--- slot {index}  r={r} ---')
        for line in (l.strip() for l in text.splitlines()):
            if line and wanted(line):
                print('   ' + line[:210])

    first, last = mine[0][2], mine[-1][2]

    def lines_with(text, prefix):
        return [l.strip() for l in text.splitlines() if l.strip().startswith(prefix)]

    print('\n== row 42 / TX over this boot ==')
    for label, text in (('first slot', first), ('newest slot', last)):
        found = lines_with(text, 'TX ') or lines_with(text, 'TOUCH ')
        for line in found:
            print(f'   {label:<12} {line[:150]}')

    print('\n== CPUFREQ census (as archived) ==')
    for line in lines_with(first, 'CPUFREQ') + lines_with(last, 'CPUFREQ'):
        print('   ' + line[:200])
    print('\n== GPU SMMU row (as archived) ==')
    for line in lines_with(first, 'GPU SMMU') + lines_with(last, 'GPU SMMU'):
        print('   ' + line[:200])

    print('\n== HID / touch counters over this boot ==')
    for label, text in (('first slot', first), ('newest slot', last)):
        for line in lines_with(text, 'TOUCH HID'):
            print(f'   {label:<12} {line[:200]}')


if __name__ == '__main__':
    main()
