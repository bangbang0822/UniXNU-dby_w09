#!/usr/bin/env python3
"""Offline regression for the multi-contact reader (DbyTouch.cpp -03b, 2026-11-03).

Replays the SAME gates the kernel now uses and prints how many contacts each recorded frame would
publish, so the multi-contact splice can be judged before it reaches hardware:

  * reference: downward-only per node (dbyTouchSeedBase + the min() update)
  * delta cut at DBY_TOUCH_TOUCH_FLOOR (700), stored unclamped
  * every 4-connected component above the floor, not just the heaviest
  * per component: 2..80 nodes, peak (unclamped delta) >= 1200
  * centroid weights = clamp(delta, CEIL) - max(30 % of min(peak, CEIL), FLOOR)

Acceptance, from the single-contact round (work/tracks/touch-tuning-review/REPORT.md):
  untouched 791 s recording -> 0 contacts;  four-corner recording -> 18 of 39 frames.
The multi-contact reader must not change either number for a one-finger frame, and must report the
extra fingers in the press recording.

Usage: python emulate-multitouch.py [recording ...]
"""
import struct
import sys

BLOCK = 4096
SLOT = 3 * BLOCK
ROWS, COLS = 50, 32
NODES = ROWS * COLS
HDR = 4
NF = 40
PANEL_W, PANEL_H = 1600, 2560
FLOOR = 700
CEIL = 8000
PEAK = 1200
MINN, MAXN = 2, 80
MAX_CONTACTS = 10

DEFAULT = [
    'evidence/edl-frames40-2230/frames40.img',    # untouched boot, 791 s
    'evidence/edl-frames40-press/frames40.img',   # owner pressed, device reported h=13
    'evidence/edl-frames40/frames40.img',         # four corner presses
]


def load(path):
    raw = open(path, 'rb').read()
    if len(raw) < NF * SLOT:
        raise SystemExit(f'{path}: {len(raw)} bytes, need {NF * SLOT}')
    out = []
    for i in range(NF):
        off = (NF - 1 - i) * SLOT
        out.append(struct.unpack_from('<%dH' % NODES, raw[off + 2:off + 2 + 8192], HDR))
    return out


def components(delta):
    """Yield (nodes, weight, peak, wx, wy) for each 4-connected component of non-zero delta."""
    seen = [False] * NODES
    for seed in range(NODES):
        if delta[seed] == 0 or seen[seed]:
            continue
        seen[seed] = True
        stack = [seed]
        nodes = weight = peak = wx = wy = 0
        while stack:
            cur = stack.pop()
            r, c = divmod(cur, COLS)
            d = delta[cur]
            if d > peak:
                peak = d
            weight += min(d, CEIL)
            wx += min(d, CEIL) * c
            wy += min(d, CEIL) * r
            nodes += 1
            for nb, ok in ((cur + 1, c + 1 < COLS), (cur - 1, c > 0),
                           (cur + COLS, r + 1 < ROWS), (cur - COLS, r > 0)):
                if ok and delta[nb] != 0 and not seen[nb]:
                    seen[nb] = True
                    stack.append(nb)
        yield nodes, weight, peak, wx, wy


def centroid(seed_delta, nodes):
    """Second pass over one component with the per-blob floor (the kernel's dbyTouchCentroid)."""
    # Find this component again by flooding from any of its nodes: simpler to recompute over the whole
    # frame and match on node count, which is unique enough at these sizes.
    raise NotImplementedError


def frame_contacts(delta):
    """Contacts this frame would publish, applying the kernel's gates to every component."""
    out = []
    for nodes, weight, peak, _wx, _wy in components(delta):
        if nodes < MINN or nodes > MAXN or weight == 0 or peak < PEAK:
            continue
        out.append((nodes, weight, peak))
    out.sort(key=lambda t: -t[1])
    return out[:MAX_CONTACTS]


def frame_centroids(delta, keep):
    """Centroid of each accepted component, with its own floor (needs a second flood per component)."""
    seen = [False] * NODES
    result = []
    for seed in range(NODES):
        if delta[seed] == 0 or seen[seed]:
            continue
        seen[seed] = True
        stack = [seed]
        nodes = peak = 0
        members = []
        while stack:
            cur = stack.pop()
            r, c = divmod(cur, COLS)
            d = delta[cur]
            if d > peak:
                peak = d
            nodes += 1
            members.append((r, c, d))
            for nb, ok in ((cur + 1, c + 1 < COLS), (cur - 1, c > 0),
                           (cur + COLS, r + 1 < ROWS), (cur - COLS, r > 0)):
                if ok and delta[nb] != 0 and not seen[nb]:
                    seen[nb] = True
                    stack.append(nb)
        if (nodes, min(peak, CEIL)) not in keep:
            continue
        floor = max(min(peak, CEIL) * 3 // 10, FLOOR)
        wsum = wx = wy = 0
        for r, c, d in members:
            w = min(d, CEIL) - floor
            if w < 0:
                w = 0
            wsum += w
            wx += w * c
            wy += w * r
        if wsum == 0:
            continue
        x = ((wx * 2 + wsum) * PANEL_W) // (wsum * 2 * COLS)
        y = ((wy * 2 + wsum) * PANEL_H) // (wsum * 2 * ROWS)
        result.append((x, y, nodes, wsum))
    return result


def replay(frames):
    base = None
    hits = 0
    stroke = []
    max_contacts = 0
    for f, fr in enumerate(frames):
        if base is None:
            base = list(fr)
            continue
        delta = [0] * NODES
        for n in range(NODES):
            if fr[n] < base[n]:
                base[n] = fr[n]
            d = fr[n] - base[n]
            delta[n] = 0 if d < FLOOR else d
        accepted = frame_contacts(delta)
        max_contacts = max(max_contacts, len(accepted))
        if not accepted:
            stroke.append((f, None))
            continue
        keep = {(nodes, min(peak, CEIL)) for nodes, _w, peak in accepted}
        pts = frame_centroids(delta, keep)
        if not pts:
            stroke.append((f, None))
            continue
        hits += 1
        stroke.append((f, pts))
    return hits, max_contacts, stroke


def main():
    paths = sys.argv[1:] or DEFAULT
    for path in paths:
        try:
            frames = load(path)
        except (OSError, SystemExit) as exc:
            print(f'== {path}: unavailable ({exc})')
            continue
        hits, max_contacts, stroke = replay(frames)
        multi = [s for s in stroke if s[1] and len(s[1]) > 1]
        print(f'\n== {path}')
        print(f'   frames with a contact : {hits:2d}/39')
        print(f'   max simultaneous      : {max_contacts}')
        if multi:
            for f, pts in multi[:6]:
                desc = '  '.join(f'({x},{y} n={n})' for x, y, n, _w in pts)
                print(f'      f{f:02d}: {len(pts)} contacts  {desc}')
        if hits and hits <= 8:
            for f, pts in stroke:
                if pts:
                    x, y, n, w = pts[0]
                    print(f'      f{f:02d}: TX {x},{y} n={n} w={w}'
                          + (f'  (+{len(pts) - 1} more)' if len(pts) > 1 else ''))


if __name__ == '__main__':
    main()
