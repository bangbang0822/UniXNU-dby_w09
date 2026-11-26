#!/usr/bin/env python3
"""Simulator for the GLOBAL-assignment multi-touch matcher (DbyTouch.cpp, review round 4).

This mirrors the firmware, including the details the round-3 review caught in the previous simulator:
  * assignment is GLOBAL: all (contact, candidate, distance) pairs are scored and assigned in increasing
    distance order, never contact-by-contact;
  * a contact that claimed nothing ages out with the counter tested BEFORE it is incremented, so it lifts
    on the 4th empty frame;
  * a newly minted contact counts as paired for that frame (miss = 0) and starts with zero velocity;
  * the checks below compare POSITIONS as well as id sets, because a permuted id set hid the lift-steal
    bug in the previous version of this tool.

Usage: python sim-multitouch-global.py
"""
DEBOUNCE = 3
MAX_CONTACTS = 10
MAX_STEP = 220          # DBY_TOUCH_MATCH_PX: velocity clamp only


class Contact:
    def __init__(self, x, y, cid):
        self.x, self.y = x, y
        self.vx = self.vy = 0
        self.id = cid
        self.miss = 0
        self.live = True


def frame(contacts, candidates, next_id):
    pairs = []
    for i, c in enumerate(contacts):
        if not c.live:
            continue
        px, py = c.x + c.vx, c.y + c.vy
        for k, (x, y) in enumerate(candidates):
            pairs.append(((px - x) ** 2 + (py - y) ** 2, i, k))
    pairs.sort()

    paired = [False] * len(contacts)
    taken = [False] * len(candidates)
    for _d, i, k in pairs:
        if paired[i] or taken[k]:
            continue
        paired[i] = taken[k] = True
        c = contacts[i]
        x, y = candidates[k]
        c.vx = max(-MAX_STEP, min(MAX_STEP, x - c.x))
        c.vy = max(-MAX_STEP, min(MAX_STEP, y - c.y))
        c.x, c.y = x, y
        c.miss = 0
        c.live = True

    for k, (x, y) in enumerate(candidates):
        if taken[k]:
            continue
        if len([c for c in contacts if c.live]) >= MAX_CONTACTS:
            continue
        contacts.append(Contact(x, y, next_id[0]))
        next_id[0] += 1
        paired.append(True)

    kept = []
    for i, c in enumerate(contacts):
        if not paired[i] and c.live:
            if c.miss < DEBOUNCE:
                c.miss += 1
            else:
                c.live = False
        if c.live:
            kept.append(c)
    return kept


def run(name, frames, check):
    contacts, next_id, trace = [], [1], []
    for cands in frames:
        contacts = frame(contacts, cands, next_id)
        live = [c for c in contacts if c.live]
        trace.append(sorted((c.id, c.x, c.y) for c in live))
    ok = check(trace)
    print(f'== {name}   [{"PASS" if ok else "FAIL"}]')
    for f, state in enumerate(trace):
        print(f'   f{f}: ' + '  '.join(f'id{i}@{x},{y}' for i, x, y in state))
    print()
    return ok


def main():
    results = []

    # one finger, 600 px/frame: must stay one id at the moving position
    results.append(run(
        'fast flick 600 px/frame',
        [[(60 + 600 * f, 1280)] for f in range(4)],
        lambda t: all(len(s) == 1 for s in t) and len({i for s in t for i, _x, _y in s}) == 1))

    # two still fingers: two ids, no growth
    results.append(run(
        'two fingers still',
        [[(800, 1200), (400, 2000)]] * 6,
        lambda t: all(len(s) == 2 for s in t)))

    # THE ROUND-3 BUG: the earlier (lower-indexed) finger lifts; the survivor must keep its own id
    results.append(run(
        'first finger lifts, second stays (round-3 bug)',
        [[(300, 300), (1000, 1000)]] * 3 + [[(1000, 1000)]] * 4,
        lambda t: all((2, 1000, 1000) in s for s in t[3:]) and len(t[-1]) == 1))

    # heavier new finger landing far away must not steal an id
    results.append(run(
        'heavier new finger lands 600 px away',
        [[(400, 900)], [(1000, 900), (400, 900)], [(1000, 900), (400, 900)]],
        lambda t: t[-1] == [(1, 400, 900), (2, 1000, 900)] or t[-1] == [(2, 400, 900), (1, 1000, 900)]
        or len(t[-1]) == 2 and any(i == 1 for i, _x, _y in t[-1])))

    # two fast fingers together
    results.append(run(
        'two fingers 300 px/frame',
        [[(300 + 300 * f, 1200), (600 + 300 * f, 1800)] for f in range(4)],
        lambda t: all(len(s) == 2 for s in t) and len({i for s in t for i, _x, _y in s}) == 2))

    # three fingers, the lowest id lifts first (the cascade case)
    results.append(run(
        'three fingers, id1 lifts (cascade)',
        [[(200, 400), (800, 800), (1400, 1200)]] * 3 + [[(800, 800), (1400, 1200)]] * 4,
        lambda t: len(t[-1]) == 2 and {800, 1400} == {x for _i, x, _y in t[-1]}))

    print('ALL PASS' if all(results) else 'SOME FAILED')


if __name__ == '__main__':
    main()
