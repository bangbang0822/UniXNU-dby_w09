# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Keep archive warnings strict on Clang versions that lack this option."""

from functools import lru_cache
from pathlib import Path
import subprocess
import sys

OPTION = '-Wno-unnecessary-virtual-specifier'


@lru_cache
def supports_option(compiler, target, cwd):
    base = [*compiler, *target, '-Werror=unknown-warning-option']
    source = ['-x', 'c++', '-fsyntax-only', '-']
    probe = subprocess.run(
        [*base, OPTION, *source],
        cwd=cwd, input='', text=True, capture_output=True,
    )
    if probe.returncode == 0:
        return True
    if 'unknown warning option' in probe.stderr.lower() and OPTION in probe.stderr:
        # Clang can report the unknown option before validating the target.
        probe = subprocess.run([*base, *source], cwd=cwd, input='', text=True, capture_output=True)
        if probe.returncode == 0:
            print(f'{compiler[0]} does not support {OPTION}; omitting it', file=sys.stderr)
            return False
    raise RuntimeError(f'archive warning probe failed: {probe.stderr.strip()}')


def filter_virtual_specifier_warning(command, cwd):
    if OPTION not in command:
        return command
    compiler = tuple(command[:2] if Path(command[0]).name == 'xcrun' else command[:1])
    target = tuple(arg for arg in command if arg.startswith(('--target=', '-target=')))
    if not target and '-target' in command:
        index = command.index('-target')
        target = tuple(command[index:index + 2])
    if supports_option(compiler, target, cwd):
        return command
    return [arg for arg in command if arg != OPTION]
