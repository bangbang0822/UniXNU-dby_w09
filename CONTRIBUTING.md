# Contributing to UniXNU-dby_w09

This is the device repository of one tablet: the Huawei MatePad 11 (DBY-W09, SM8250 / kona). It
owns the board DTS, the kernel defconfig, the board package, the partition layout, the firmware
selection, the boot arguments and the porting record.

Shared XNU, driver and kit implementations belong to `UniXNU`; SoC operations belong to
`UniXNU-sm8250-common`; a driver or protocol a second board could use belongs to a component
repository. [`AGENTS.md`](AGENTS.md#where-code-belongs) states the test that decides it -- would a
second SM8250 board need this file, unchanged? -- and carries the commit convention, the tag rule
and the hardware rules this port learned the hard way. Read it before your first change.

## Before your first change

Install the hooks once per clone. They run everything that does not need a build host:

```sh
git config core.hooksPath .githooks
```

Read the `porting.toml` area your change touches. The record is the only place this board remembers
why something is the way it is; a change that contradicts it is a change that has to update it.

Nothing in this document needs a build host or the tablet. The checks below run from a clean
checkout with Python 3.11 and git.

## Branches, review and merge

`main` is protected: only repository administrators push to it or merge into it. Every other branch
name is covered by a rule that also admits developers, so you push your branch to this repository
and open a pull request against `main`. Forking works and is not required.

Two gates stand between a pull request and `main`, and neither is optional:

| Gate | What it means |
| --- | --- |
| Review | A reviewer with write access approves the change. |
| Test | The test marker is set once the checks below have been run. |

Only an administrator merges, and only after both gates are recorded. This was verified on
2026-11-13 by running the chain end to end: the merge was refused with "this pull request has not
passed the configured review", then with "has not passed the configured test", and succeeded only
after both were recorded.

One thing worth stating because it is easy to get wrong: Gitee will accept an approval from the
author of a pull request, but that records a review nobody performed. The gate asks for a second
person; when there is no second person, say so in the pull request rather than approving your own
work.

## What review checks

Six questions, in this order. The first is answered before the diff is read; the rest decide the
verdict.

1. **Does the change belong in this repository?** A file a second SM8250 board would need unchanged
   belongs in a component or a feed. Board policy stays here.
2. **Does the record move with the code?** The affected `porting.toml` area's findings or evidence
   is updated in the same pull request, as its own commit.
3. **Does every hardware value name its source?** A register address, a timing, a partition
   boundary or a pin comes with the backup, the vendor DTS line or the official record that
   produced it, written next to the value.
4. **Does the documentation describe current behaviour?** Text that describes what the code is
   meant to do, rather than what it does, is a defect: it is how a record stops being true.
5. **Did anything private or generated enter Git?** No IPSW member, Data seed, key, vendor blob,
   firmware binary or build output. `proprietary-files.txt` lists what stays out, and
   `scripts/check-repository.py` fails when a tracked file looks like any of them.
6. **Do the checks pass?** Both commands under "Checks to run" exit 0.

A verdict is one of three, and the difference is the review:

- **approved** -- the change is right, and so is the record beside it;
- **changes requested** -- the code is right but something is missing: the record, the source of a
  value, a subject line, the comment that explains why;
- **blocked** -- the change is wrong in behaviour or in principle.

Say which. A request to "fix it" without naming the category is what makes a review thread
unreadable a month later.

A review cannot verify the tablet. A claim about what the device does needs a run on the device
recorded in `porting.toml`; a reviewer who has not seen that run should say so rather than approve
the claim.

## Code style

There is no formatter and no style gate, so the file you are editing is the specification: read the
function above the one you are adding and match it. The table below is what a new file should look
like here; where the tree disagrees, the disagreement is older than the rule.

| Convention | What the tree does |
| --- | --- |
| Indentation | Four spaces per level; tabs survive only in files that arrived from elsewhere. |
| Braces | Blocks open on the same line; function definitions use either form -- match the file. |
| Headers | `#ifndef` guards in upper snake case ending in `_H`; no `#pragma once`. |
| Licence header | `// SPDX-License-Identifier: BSD-2-Clause-Patent` first, or `#` in scripts. |
| Includes | `<...>` for SDK and kernel headers, `"..."` for headers in this tree. |
| Null | `NULL` in C, `nullptr` in C++: match the file instead of mixing the two. |
| Constants | A `k` prefix on constants that are not local to one function (`kOpForms`). |
| Line endings | LF, enforced by `.gitattributes`; patches are marked `-whitespace`. |
| Line length | No hard limit; comment prose is wrapped by hand at about 95 columns. |
| Comments | English, explaining why; a claim about upstream code cites file and line. |

The tree does not match this table everywhere, because several hands wrote it. Twelve files carry
Chinese notes, `NULL` and `nullptr` both appear in C++ written on different days, and a few older
functions put their brace on its own line. That is history, not precedent: new code follows the
table, and a file is not converted just because it is being touched.

Two things the table does not license:

- **Do not restyle a file that came from somewhere else.** The fixtures under
  `package/QcomXnuUsbDebug/fixture/` carry XNU's own `bsd/`, `osfmk/` and `pexpert/` paths, and they
  keep their tabs, their brace placement and the header they arrived with, because the next reader
  has to diff them against the original.
- **Do not reformat and change behaviour in the same commit.** If the tree ever adopts a formatter,
  it arrives as its own commit that touches only whitespace, so that a review can read it with
  `git diff -w` and know there is nothing else in it.

## Scripts

The Python scripts under `scripts/` are the checking half of this repository. They run anywhere
Python 3.11 does:

- `#!/usr/bin/env python3`, a module docstring that says what the script proves, standard library
  only;
- `argparse` for arguments, `if __name__ == "__main__"` for the entry point;
- one line of output on success and a non-zero exit when a check fails; a script the `pre-push` hook
  calls takes `--quiet`, because the hook prints one line per push.

Shell scripts start `#!/usr/bin/env bash` and set `-e`.

## Checks to run

```sh
python3 scripts/check-repository.py --quiet     # one line: the repository still describes itself
python3 scripts/check-repository.py --verbose   # plus what it skipped and why
```

The upstream record validator is stronger and needs a UniXNU checkout beside this one:

```sh
python3 scripts/porting-record.py check --device ../UniXNU-dby_w09
```

The first proves that `board.toml` and `porting.toml` parse and agree, that every path either file
names exists, that every tracked code file is claimed by a record area and every claim matches a
file, that each patch named by a series resolves, that no committed file names a build host path
and that nothing generated or private is tracked. The second proves the record passes the shape
upstream's guide requires.

The `pre-push` hook runs the first before every push; `.github/workflows/porting-record.yml`
calls the second as a reusable workflow. Both also run by hand.

## Commit messages and tags

The convention, including the prefix table and the scope vocabulary, is in
[`AGENTS.md`](AGENTS.md#commit-convention); `scripts/check-commit-messages.py` enforces it and the
`commit-msg` hook calls that. A pushed tag is immutable -- the rule and its reason are in
[`AGENTS.md`](AGENTS.md#tags).

## Private inputs

Apple restore-image members, the Data seed, any owner key, vendor firmware and the images built
from them stay out of Git. `proprietary-files.txt` lists the inputs and their hashes; the files
live under `.cache/`. A pull request that adds one of them is rejected, not discussed.

## License

Contributions are licensed under BSD-2-Clause-Patent, the root `LICENSE`. Code that arrived from
somewhere else keeps the licence it came with, as `package/QcomXnuUsbDebug/usb/LICENSE.m1n1` does.
