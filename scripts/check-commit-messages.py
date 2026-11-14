#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check commit subjects against the UniXNU commit convention.

The convention is the table in UniXNU's CONTRIBUTING.md:

    feat(<area>)   New capability
    fix(<area>)    Defect repair
    docs           Documentation
    build(<area>)  Build system
    chore          Maintenance

    "Commit messages describe changes. Documentation describes current behavior.
     Keep one subject per commit."

This repository adds one rule of its own, because its records and its code are supposed to
move together: within a range, if an area's files change, porting.toml must change too --
the upstream record guide asks for exactly that, and it asks for the two to be separate
commits inside the same pull request, which is why the check is scoped to the range rather
than to each commit.

Usage:
  python3 scripts/check-commit-messages.py                    # @{u}..HEAD, or HEAD~1..HEAD with no upstream
  python3 scripts/check-commit-messages.py --range main..HEAD
  python3 scripts/check-commit-messages.py --message-file .git/COMMIT_EDITMSG
  python3 scripts/check-commit-messages.py --strict-scopes    # scope must be an area id

Exit status is 0 only when every commit passes.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tomllib

TYPES = ('feat', 'fix', 'docs', 'build', 'chore')
# The table spells feat/fix/build with an area and docs/chore without one. A scope on
# docs or chore is accepted when it adds information (docs(readme)), because forbidding it
# would only make people write less specific subjects.
SCOPED = ('feat', 'fix', 'build')
SUBJECT = re.compile(r'^(feat|fix|docs|build|chore)(?:\(([a-z][a-z0-9-]*)\))?: (\S.*)$')
MAX_SUBJECT = 72

# Scopes that describe the repository itself rather than one board area.
REPO_SCOPES = {'repo', 'ci', 'tests'}


def read_areas(device):
    """The record's area ids, or None when the record cannot be read.

    An unreadable or unparsable porting.toml used to look exactly like a record with no areas, and under
    --strict-scopes that turned every scope into "not a porting.toml area id" -- a failure invented by the
    missing file rather than found in the commits.
    """
    try:
        record = tomllib.loads((device / 'porting.toml').read_text(encoding='utf-8'))
    except (OSError, tomllib.TOMLDecodeError):
        return None
    return {area.get('id') for area in record.get('area', []) if area.get('id')}


def check_subject(subject, areas, strict):
    """One problem per broken rule, so a fixable message gets all of them at once."""
    problems, notes = [], []
    if len(subject) > MAX_SUBJECT:
        notes.append(f'subject is {len(subject)} characters; {MAX_SUBJECT} keeps it readable in logs')
    if subject.rstrip().endswith('.'):
        notes.append('subject ends with a period')
    match = SUBJECT.match(subject)
    if not match:
        problems.append('subject must be "<type>(<area>): <what changed>" with type in '
                        + '/'.join(TYPES))
        return problems, notes
    kind, scope = match.group(1), match.group(2)
    if kind in SCOPED and not scope:
        problems.append(f'"{kind}" needs an area scope, for example {kind}(display)')
    if scope:
        known = scope in areas or scope in REPO_SCOPES
        if not known and strict:
            problems.append(f'scope "{scope}" is not a porting.toml area id '
                            f'(known: {", ".join(sorted(areas | REPO_SCOPES))})')
    return problems, notes


def iter_commits(rev_range):
    out = subprocess.run(['git', 'log', '--format=%H%x09%P%x09%s%x09%b%x00', rev_range],
                         capture_output=True, text=True, check=True)
    for chunk in out.stdout.split('\x00'):
        chunk = chunk.strip('\n')
        if not chunk:
            continue
        parts = chunk.split('\t')
        sha = parts[0].strip()
        parents = parts[1].split() if len(parts) > 1 else []
        subject = parts[2].strip() if len(parts) > 2 else ''
        body = parts[3].strip() if len(parts) > 3 else ''
        yield sha, subject, body, parents


def changed_files(sha):
    out = subprocess.run(['git', 'show', '--name-only', '--format=', sha],
                         capture_output=True, text=True, check=True)
    return [line for line in out.stdout.splitlines() if line.strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1])
    ap.add_argument('--range', dest='rev_range', default=None,
                    help='commit range to check; defaults to upstream..HEAD')
    ap.add_argument('--message-file', type=pathlib.Path,
                    help='check one message file, as the commit-msg hook passes it')
    ap.add_argument('--strict-scopes', action='store_true',
                    help='also require every scope to be a porting.toml area id')
    args = ap.parse_args()

    areas = read_areas(args.device.resolve())
    if areas is None:
        if args.strict_scopes:
            raise SystemExit(f'FAIL: {args.device}/porting.toml could not be read, so --strict-scopes cannot '
                             f'check any scope; fix the record or drop the flag')
        areas = set()
    failures = 0

    if args.message_file:
        text = args.message_file.read_text(encoding='utf-8', errors='replace')
        lines = [l for l in text.splitlines()]
        subject = lines[0] if lines else ''
        problems, notes = check_subject(subject, areas, args.strict_scopes)
        if len(lines) > 1 and lines[1].strip():
            problems.append('the body must start after a blank line')
        for note in notes:
            print(f'NOTE  {note}')
        for problem in problems:
            print(f'FAIL  {problem}')
        if problems:
            print('\nThe convention is in UniXNU CONTRIBUTING.md; this repository also '
                  'expects a porting.toml change in the same push when code changes.')
            return 1
        print(f'PASS  {subject}')
        return 0

    rev_range = args.rev_range
    if rev_range is None:
        upstream = subprocess.run(['git', 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{u}'],
                                  capture_output=True, text=True)
        rev_range = f'{upstream.stdout.strip()}..HEAD' if upstream.returncode == 0 else 'HEAD~1..HEAD'
    print(f'range {rev_range}')
    touched_areas = set()
    record_touched = False
    for sha, subject, _body, parents in iter_commits(rev_range):
        if len(parents) > 1:
            # Written by the hosting platform, not by a person. Holding it to the
            # convention would block every push that follows a pull-request merge.
            print(f'SKIP  {sha[:8]}  merge commit, subject authored by the host')
            continue
        problems, notes = check_subject(subject, areas, args.strict_scopes)
        for note in notes:
            print(f'NOTE  {sha[:8]}  {note}')
        for problem in problems:
            print(f'FAIL  {sha[:8]}  {problem}')
            print(f'      subject: {subject}')
            failures += len(problems)
        if not problems:
            print(f'PASS  {sha[:8]}  {subject}')
        files = changed_files(sha)
        if 'porting.toml' in files:
            record_touched = True
        touched_areas.update(a for a in areas if any(a in f for f in files))
        # An approximate attribution: a file belongs to the area whose id appears in its path.
        # The exact per-file ownership lives in porting.toml paths; this is the cheap version.

    if touched_areas and not record_touched:
        print(f'\nWARN  this range changes files that look like areas {sorted(touched_areas)} '
              f'but does not touch porting.toml')
        print('      the record guide wants the affected area updated in the same push')

    if failures:
        print(f'\n{failures} commit subject(s) do not follow the convention')
        return 1
    print('\nall commit subjects follow the convention')
    return 0


if __name__ == '__main__':
    sys.exit(main())
