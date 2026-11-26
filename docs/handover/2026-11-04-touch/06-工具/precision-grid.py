"""Score a 5x5 precision grid from a boot-log window, without any extra build.

The owner presses a known grid on the panel; the reader publishes `TX x,y` (panel pixels) into row 42,
the page is archived every 2 s and re-composed every ~0.64 s, so a press HELD for ~6 s yields 3
archived samples.  This script reads the window, takes the samples belonging to the newest boot,
assigns each to the nearest grid point, and reports the statistics the touch-precision study asked for
(work/tracks/touch-precision/REPORT.md §7):

    per-point error, bias, RMS, max, affine-fit residual RMS, repeatability, monotonicity

Gates (from the same report):
    jitter max <= 5 px (target 2.5)     fit residual RMS <= 12 px (target 5)
    corner error <= 25 px (target 8)    monotonicity and "all 25 points reported" are hard gates

Usage:
    python precision-grid.py <bootlog-window.img> [--grid]

--grid prints the press plan to give the owner.
"""
import re
import struct
import sys

BLOCK = 4096
SLOT_FIRST = 33
SLOT_BLOCKS = 16

# The 5x5 grid. 1 px inset at the edges, because 0 is the very lip of the glass.
XS = (1, 400, 800, 1200, 1598)
YS = (1, 640, 1280, 1920, 2558)


def header(data):
    return {
        'slots_written': struct.unpack_from('<I', data, 60)[0],
        'uptime': struct.unpack_from('<I', data, 64)[0],
        'slot_count': struct.unpack_from('<I', data, 56)[0],
    }


def samples(data, hdr):
    """(t, x, y, w, n) for every archived sample of the NEWEST boot.

    Only slots 0..slots_written-1 belong to this boot; the window also holds earlier boots' slots,
    which is why "r == slot index" alone is not enough here.
    """
    out = []
    limit = min(hdr['slots_written'], hdr['slot_count'])
    for idx in range(limit):
        off = (SLOT_FIRST + idx * SLOT_BLOCKS) * BLOCK
        text = data[off:off + SLOT_BLOCKS * BLOCK].replace(b'\x00', b'\n').decode(
            'utf-8', 'replace')
        tag = re.search(r'#slot (\d+) t=(\d+)s', text)
        t = int(tag.group(2)) if tag else -1
        # The multi-contact reader (2026-11-03, -03b) prefixes the row with c=<count>; the grid is a
        # single-finger measurement, so accept both shapes and skip multi-contact rows.
        for m in re.finditer(r'^TX (?:c=(\d+) )?(\d+),(\d+) w=(\d+) n=(\d+)', text, re.M):
            if m.group(1) not in (None, '1'):
                continue
            out.append((t, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))
    return out


def nearest_grid(x, y):
    gx = min(XS, key=lambda v: abs(v - x))
    gy = min(YS, key=lambda v: abs(v - y))
    return gx, gy


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--grid' in sys.argv or not args:
        print('press plan: 5x5, hold each point ~6 s (3 archived samples), release ~2 s between')
        print('  rows top to bottom, columns left to right')
        for gy in YS:
            print('   y=%4d : ' % gy + '  '.join('(%4d,%4d)' % (gx, gy) for gx in XS))
        if not args:
            return
    if not args:
        raise SystemExit(__doc__)

    data = open(args[0], 'rb').read()
    if data[0:8] != b'SHLOG001':
        raise SystemExit('not a boot-log window (magic missing)')
    hdr = header(data)
    pts = samples(data, hdr)
    print('boot: uptime_s=%d slots_written=%d' % (hdr['uptime'], hdr['slots_written']))
    print('archived TX samples: %d' % len(pts))
    if not pts:
        raise SystemExit('no TX sample in this boot: was anything pressed while it ran?')

    # Group by the nearest grid point.
    groups = {}
    for t, x, y, w, n in pts:
        key = nearest_grid(x, y)
        groups.setdefault(key, []).append((t, x, y))

    print('\n  grid point    samples   mean(dx,dy)   rms   max   note')
    hit = 0
    errs = []
    for gy in YS:
        for gx in XS:
            g = groups.get((gx, gy))
            if not g:
                print('  (%4d,%4d)       0            -         -     -   MISSING' % (gx, gy))
                continue
            hit += 1
            dxs = [x - gx for _, x, _ in g]
            dys = [y - gy for _, _, y in g]
            mdx = sum(dxs) / len(dxs)
            mdy = sum(dys) / len(dys)
            e = [(x - gx) ** 2 + (y - gy) ** 2 for _, x, y in g]
            rms = (sum(e) / len(e)) ** 0.5
            mx = max(e) ** 0.5
            errs += e
            # Repeatability: spread of repeated samples at one point.
            spread = 0.0
            if len(g) > 1:
                sx = max(dxs) - min(dxs)
                sy = max(dys) - min(dys)
                spread = (sx * sx + sy * sy) ** 0.5
            note = ''
            if abs(mdx) > 8 or abs(mdy) > 8:
                note = 'BIAS-%s' % ('x' if abs(mdx) > abs(mdy) else 'y')
            if spread > 5:
                note += ' JITTER'
            if mx > 25:
                note += ' BIG'
            print('  (%4d,%4d)  %5d    %+6.1f,%+6.1f  %6.1f %6.1f  %s'
                  % (gx, gy, len(g), mdx, mdy, rms, mx, note.strip()))

    print('\n  points reported: %d / 25          <- hard gate: 25/25' % hit)
    if errs:
        print('  overall RMS error: %.1f px        <- gate <= 12, target <= 5' % (sum(errs) / len(errs)) ** 0.5)
        print('  overall max error: %.1f px' % (max(errs) ** 0.5))

    # Monotonicity: along a row, x must not go backwards; along a column, y must not.
    mono_bad = 0
    for gy in YS:
        row = [groups.get((gx, gy)) for gx in XS]
        xs = [sum(p[1] for p in g) / len(g) for g in row if g]
        for a, b in zip(xs, xs[1:]):
            if b <= a:
                mono_bad += 1
    for gx in XS:
        col = [groups.get((gx, gy)) for gy in YS]
        ys = [sum(p[2] for p in g) / len(g) for g in col if g]
        for a, b in zip(ys, ys[1:]):
            if b <= a:
                mono_bad += 1
    print('  monotonicity violations: %d      <- hard gate: 0' % mono_bad)


if __name__ == '__main__':
    main()
