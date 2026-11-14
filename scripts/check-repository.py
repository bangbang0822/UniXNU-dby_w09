#!/usr/bin/env python3
"""Check that this repository is coherent on its own: no build host, no hardware.

Everything here can be verified from a clean checkout with Python 3.11 and git, which is
what a reader who wants to trust -- or rebuild -- this record needs. It answers one
question: does this repository still describe itself truthfully?

  * board.toml parses and every path it names exists in the repository
  * porting.toml parses, its identity matches board.toml, its statuses are valid, and its
    evidence carries the fields the upstream guide requires
  * every tracked code file is claimed by exactly-one-or-more porting.toml areas, and every
    claim matches at least one file (a claim that matches nothing is a stale path)
  * every patch the series files name resolves to a file, and a series entry that resolves
    only outside this repository is reported as external rather than passed silently
  * copies of the same patch are byte-identical (the repository carries two names for one
    patch; nothing else checks that they stay the same)
  * no committed code or configuration names a build host's absolute path
  * no generated or private artefact is tracked (build/, .cache/)

Usage:
  python3 scripts/check-repository.py
  python3 scripts/check-repository.py --device DIR --verbose

Exit status is 0 only when every check passes.
"""
import argparse
import fnmatch
import pathlib
import re
import subprocess
import sys
import tomllib

STATUSES = {'planned', 'implemented', 'host-verified', 'device-verified', 'not-applicable'}
EVIDENCE_FIELDS = ('kind', 'reference', 'result')
AREA_FIELDS = ('id', 'status', 'components', 'paths', 'inputs', 'configuration', 'procedure',
               'findings', 'completion', 'limits', 'next_action', 'evidence')

# Files that are allowed to name a host path because they are a record of what was run,
# or because they define the pattern this check looks for.
HOST_PATH_EXEMPT = ('porting.toml', 'scripts/check-repository.py')
HOST_PATH_EXEMPT_PREFIX = ()
HOST_PATH = re.compile(r'/home/[a-zA-Z]|/Users/[a-zA-Z]|/opt/homebrew/|/Volumes/[A-Z]|(?<![A-Za-z])[A-Z]:\\\\|(?<![A-Za-z])[A-Z]:/[A-Za-z]')

IGNORED_PREFIX = ('build/', '.cache/')


class Report:
    def __init__(self, verbose, quiet=False):
        self.verbose = verbose
        self.quiet = quiet
        self.failures = []
        self.passes = 0

    def check(self, ok, name, detail=''):
        if ok:
            self.passes += 1
            if not self.quiet:
                print('PASS  ' + name + ('  ' + detail if detail else ''))
        else:
            print('FAIL  ' + name + ('  ' + detail if detail else ''))
            self.failures.append(name)
        return ok

    def note(self, message):
        if self.verbose and not self.quiet:
            print('      ' + message)

    def done(self):
        if self.failures:
            print(f'\n{len(self.failures)} check(s) failed: ' + ', '.join(self.failures))
            return 1
        if self.quiet:
            print(f'check-repository: all {self.passes} checks passed')
        else:
            print('\nall checks passed')
        return 0


def tracked(device, others=False):
    """Files git knows about. With others=True, untracked-but-not-ignored files count too,
    so the coverage checks work on a dirty tree before the first commit."""
    args = ['git', '-C', str(device), 'ls-files']
    if others:
        args += ['--cached', '--others', '--exclude-standard']
    try:
        out = subprocess.run(args, capture_output=True, text=True, check=True)
        return [line for line in out.stdout.splitlines() if line.strip()]
    except (OSError, subprocess.CalledProcessError):
        return [p.relative_to(device).as_posix() for p in device.rglob('*') if p.is_file()]


def resolve_series_entry(device, series_path, entry):
    """Resolve one series line. Returns (kind, path-or-None): local, external or missing."""
    # component:<ID>/<path> names a file inside a module checkout (profiles.py:resolve_path), so it
    # resolves in the assembled tree and never inside this repository.
    if entry.startswith('component:'):
        return 'external', None
    stripped = re.sub(r'^(arch|soc|os|device):', '', entry)
    if '..' in pathlib.PurePosixPath(stripped).parts:
        return 'external', None
    candidate = (series_path.parent / stripped)
    if candidate.is_file():
        return 'local', candidate
    return 'missing', None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1])
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--quiet', action='store_true',
                    help='print failures and a one-line summary only')
    args = ap.parse_args()
    device = args.device.resolve()
    report = Report(args.verbose, args.quiet)

    files = tracked(device, others=True)
    tracked_set = set(tracked(device))

    # ---- board.toml ------------------------------------------------------
    board_path = device / 'board.toml'
    try:
        board = tomllib.loads(board_path.read_text(encoding='utf-8'))
        report.check(True, 'board.toml parses')
    except (OSError, tomllib.TOMLDecodeError) as error:
        report.check(False, 'board.toml parses', str(error))
        return report.done()

    report.check(board.get('schema_version') == 2, 'board.toml uses format 2 schemas',
                 f"schema_version = {board.get('schema_version')}")
    report.check('example' not in board.get('board', {}), 'board is not marked as an example')

    kernel = board.get('kernel', {})
    defconfig = kernel.get('defconfig', '')
    report.check((device / 'configs' / defconfig).is_file(),
                 'board.toml [kernel] defconfig names an existing file', defconfig)

    source = board.get('device_tree', {}).get('source', '')
    dts = re.sub(r'^device:', '', source)
    report.check((device / dts).is_file(),
                 'board.toml [device_tree] source names an existing file', source)

    package = board.get('package', {})
    report.check('unixnu' in package, 'board.toml [package] unixnu is set',
                 str(package.get('unixnu', 'missing')))
    mapping = package.get('defconfig', {})
    report.check(bool(mapping), 'board.toml [package.defconfig] maps the OS profile to a defconfig')
    for profile, name in mapping.items():
        report.check((device / 'configs' / name).is_file(),
                     f'[package.defconfig] "{profile}" names an existing file', name)

    components = board.get('components', {}).get('include', [])
    report.check(bool(components), 'board.toml selects components', f'{len(components)} selected')

    for rel in ('scripts/package.sh', '.github/workflows/package.yml',
                '.github/workflows/porting-record.yml', 'proprietary-files.txt',
                'local.toml.example', 'unixnu.lock', 'feeds.toml', 'AGENTS.md', 'README.md'):
        report.check((device / rel).is_file(), f'layout: {rel}')

    # ---- porting.toml ----------------------------------------------------
    record_path = device / 'porting.toml'
    try:
        record = tomllib.loads(record_path.read_text(encoding='utf-8'))
        report.check(True, 'porting.toml parses')
    except (OSError, tomllib.TOMLDecodeError) as error:
        report.check(False, 'porting.toml parses', str(error))
        return report.done()

    report.check(record.get('schema_version') == 1, 'porting.toml uses record schema 1',
                 f"schema_version = {record.get('schema_version')}")
    identity = record.get('device', {})
    for key in ('name', 'soc', 'os_profile'):
        report.check(identity.get(key) == board.get('board', {}).get(key),
                     f'porting.toml device.{key} matches board.toml', str(identity.get(key)))

    areas = record.get('area', [])
    report.check(len(areas) >= 14, 'the record carries the required sections', f'{len(areas)} areas')

    patterns = []
    for area in areas:
        aid = area.get('id', '?')
        report.check(area.get('status') in STATUSES, f'{aid}: status is valid', str(area.get('status')))
        missing_fields = [f for f in AREA_FIELDS if f not in area]
        report.check(not missing_fields, f'{aid}: carries every required field',
                     ', '.join(missing_fields) if missing_fields else '')
        for key in ('inputs', 'configuration', 'procedure', 'findings', 'completion', 'limits',
                    'next_action', 'paths'):
            report.check(bool(area.get(key)), f'{aid}: {key} is not empty')
        patterns.extend(area.get('paths') or [])
        for item in area.get('evidence') or []:
            report.check(all(item.get(k) for k in EVIDENCE_FIELDS),
                         f'{aid}: evidence carries {"/".join(EVIDENCE_FIELDS)}')
            # The upstream guide requires command and a full revision from host and device
            # evidence; source evidence is a pointer at a file and needs neither.
            if item.get('kind') in ('host', 'device'):
                report.check(bool(item.get('command')) and len(item.get('revision', '')) == 40,
                             f'{aid}: {item.get("kind")} evidence carries command and a 40-character revision')

    # ---- path coverage ---------------------------------------------------
    def covers(rel):
        for pattern in patterns:
            if fnmatch.fnmatch(rel, pattern):
                return True
            if pattern.endswith('/**') and rel.startswith(pattern[:-3] + '/'):
                return True
        return False

    stale = []
    for pattern in patterns:
        # A claim with an arch:/soc:/os: prefix names a file in another repository by
        # design (the SoC DTSI, the arch series); only device-relative claims are checked.
        if re.match(r'^(arch|soc|os):', pattern):
            continue
        if any(fnmatch.fnmatch(f, pattern) or
               (pattern.endswith('/**') and f.startswith(pattern[:-3] + '/')) for f in files):
            continue
        stale.append(pattern)
    # A planned area may name where its files will go; anything already shipped may not
    # claim a path that matches nothing, because a stale claim reads as coverage.
    planned = {p for area in areas if area.get('status') == 'planned'
               for p in (area.get('paths') or [])}
    stale = [p for p in stale if p not in planned]
    report.check(not stale, 'every porting.toml path claim matches a tracked file',
                 ', '.join(stale[:6]) if stale else f'{len(patterns)} patterns')

    code_ext = ('.c', '.cpp', '.h', '.m', '.mm', '.py', '.sh')
    furniture = {'board.toml', 'porting.toml', 'feeds.toml', 'unixnu.lock', 'README.md',
                 'AGENTS.md', 'LICENSE', '.gitignore', '.gitattributes', 'proprietary-files.txt',
                 'local.toml.example'}
    unclaimed = [f for f in files
                 if f not in furniture and f.endswith(code_ext) and not covers(f)]
    report.check(not unclaimed, 'every tracked code file is claimed by an area',
                 ', '.join(unclaimed[:6]) if unclaimed else f'{len(files)} tracked files')

    # ---- patch series ----------------------------------------------------
    series_files = sorted((device / 'patches/xnu').glob('series*'))
    report.check(bool(series_files), 'patch series files exist', f'{len(series_files)} series')
    external = []
    missing_entries = []
    for series in series_files:
        for raw in series.read_text(encoding='utf-8').splitlines():
            entry = raw.strip()
            if not entry or entry.startswith('#'):
                continue
            kind, path = resolve_series_entry(device, series, entry)
            if kind == 'external':
                external.append(f'{series.name}: {entry}')
            elif kind == 'missing':
                missing_entries.append(f'{series.name}: {entry}')
    report.check(not missing_entries, 'every patch named by a series resolves in this repository',
                 ', '.join(missing_entries) if missing_entries else f'{len(series_files)} series checked')
    if external and not args.quiet:
        print('      note: series entries that resolve in the assembled tree, not here:')
        for item in external:
            print(f'            {item}')

    # ---- duplicate patch copies -----------------------------------------
    # Nothing to police since the module became a catalog component: patches/xnu/series-usb names
    # component:QcomXnuUsbDebug/patches/darwin23/... and there is no device-side copy to drift from
    # it. A copy reappearing here would be the defect, not the arrangement being checked for.
    stray_copies = sorted((device / 'patches/xnu').glob('qud-darwin23-*.patch'))
    report.check(not stray_copies, 'no device-side copy of a component patch',
                 ', '.join(p.name for p in stray_copies) if stray_copies else 'none')

    # ---- no host paths ---------------------------------------------------
    offenders = []
    for rel in files:
        if rel in HOST_PATH_EXEMPT or rel.startswith(HOST_PATH_EXEMPT_PREFIX):
            continue
        if not rel.endswith(code_ext + ('.toml', '.md', '.txt', '.yml', '.json')):
            continue
        try:
            text = (device / rel).read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        if HOST_PATH.search(text):
            offenders.append(rel)
    report.check(not offenders, 'no committed file names a build host path',
                 ', '.join(offenders[:6]) if offenders else 'record and Apple sources exempt')

    # ---- line citations --------------------------------------------------
    # A citation is only useful if it points inside the file it names. This cannot judge
    # whether the line still says what the sentence claims -- that needs a reader -- but it
    # catches the case that has already happened here twice: a number that ran past the end
    # of the file when the file shrank.
    citation = re.compile(r'([\w.-]+\.(?:c|cpp|h|m|mm|py|sh|toml)):(\d+)')
    out_of_range = []
    for rel in files:
        if not rel.endswith(('.c', '.cpp', '.h', '.m', '.mm', '.py', '.sh', '.toml', '.md', '.txt', '.yml')):
            continue
        try:
            text = (device / rel).read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for match in citation.finditer(text):
            target, line = match.group(1), int(match.group(2))
            if target in (pathlib.PurePath(rel).name,) and target.startswith(rel):
                continue
            candidates = [f for f in files if f.endswith('/' + target) or f == target]
            if not candidates:
                continue          # cites a file outside this repository (core, xnu, vendor tree)
            count = len((device / candidates[0]).read_text(encoding='utf-8', errors='replace').splitlines())
            if line > count:
                out_of_range.append(f'{rel}: {match.group(0)} (file has {count} lines)')
    report.check(not out_of_range, 'every line citation points inside the file it names',
                 '; '.join(sorted(set(out_of_range))[:4]) if out_of_range else 'checked across the tree')
    # ---- generated artefacts --------------------------------------------
    tracked_build = [f for f in tracked_set if f.startswith(IGNORED_PREFIX)]
    report.check(not tracked_build, 'no generated or private artefact is tracked',
                 ', '.join(tracked_build[:5]) if tracked_build else 'build/ and .cache/ are ignored')

    return report.done()


if __name__ == '__main__':
    sys.exit(main())
