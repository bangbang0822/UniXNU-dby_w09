#!/usr/bin/env python3
"""Host tests of the N9Z protocol layer against the vendor's register map (package/NvtN9ZHostTests.c). Model tests only.

The test lived in the tree with no runner, so nothing ran it -- and when it was finally compiled it aborted
on its first assertion: its bus model asserted that a write carries no receive buffer, which the fork's
`bus_write()` has not been true of since it was made full-duplex. This gate exists so the next drift in
either direction is noticed by a run instead of by a reader.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'nvt-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'NvtProtocol.c'), str(PACKAGE / 'NvtN9ZHostTests.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and 'PASS' in result.stdout else 1)
