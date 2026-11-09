#!/usr/bin/env python3
"""Build libDbyWifiProbe.a, the archive the DBY-W09 kernel links for the Wi-Fi/Bluetooth M0 census.

package/DbyWifiProbe.cpp is the IOKit half: it parses `dby-bt-m0=<1|2>`, maps three or five windows
and reads 30 (mode 1) or 46 (mode 2) words.  package/DbyWifiProbeBoard.c is the IOKit-free half: it
decodes GPIO_CFG words, decides whether a GCC branch is really running, picks the M0 verdict and
renders the six rows.  Design and evidence: work/tracks/wifi-bt/M0-ANSWER.md and
work/tracks/wifi-bt/IMPLEMENTATION.md.

Two translation units, one archive:

    DbyWifiProbeBoard.c -> libDbyWifiProbe.a <- DbyWifiProbe.cpp

Compiled with the commands the kernel build itself recorded, so headers, defines and ABI flags are
exactly the ones the kernel was built with:

    DbyWifiProbeBoard.c <- build/xnu/ios-obj/.../osfmk/DEVELOPMENT/startup.o.json             (C)
    DbyWifiProbe.cpp    <- build/xnu/ios-obj/.../iokit/DEVELOPMENT/IOPlatformExpert.cpo.json  (C++)

That is the same technique, and the same warning demotions, as scripts/build-gpu-smmu-probe.py in
this repository.  The C record is the one the previous Wi-Fi round used for its IOKit-free census
(work/tracks/wifi/repo/scripts/build-dby-wifi.py:51-58).

The kernel tree is only read (compile records, headers).  Every output lands under
{tree}/build/wifiprobe/.  The defconfig force-loads it through CONFIG_XNU_EXTRA_LD_ARCHIVES:

    -Wl,-force_load,{tree}/build/wifiprobe/libDbyWifiProbe.a

force_load, not a plain archive path, because every reference to this probe is a WEAK import
(package/BootProgress.c declares dby_wifi_probe_entry weak, package/BootLog.c reads native_bt_rows
weak), so an ordinary archive member is never pulled in by the linker and the probe would silently
never run.

THREE PHASES, and why the other two exist
  build       the archive, from the kernel's recorded commands            (needs --tree)
  --audit     re-derives every constant in package/DbyWifiProbeBoard.h from the vendor kernel tree
              and fails if the header and the vendor source disagree      (needs --vendor)
  --self-test compiles package/DbyWifiProbeBoard.c with a HOST compiler, runs its cases and prints
              the rows those register values would produce                 (needs a host cc)
--audit and --self-test are what make this recipe useful before a device is involved: the first
proves the magic numbers still match the file:line comments that justify them, the second proves the
verdict ladder still behaves the way M0-ANSWER.md documents.  Neither touches a tablet.

Usage:
  python3 scripts/build-wifi-probe.py --tree /root/unixnu-out/.../tree
  python3 scripts/build-wifi-probe.py --tree <tree> --no-verify
  python3 scripts/build-wifi-probe.py --vendor _dby/robyy10__DBY-W09.tar/DBY-W09-main --audit
  python3 scripts/build-wifi-probe.py --self-test --cc /usr/bin/cc
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_NAME = 'build/wifiprobe'
ARCH = 'DEVELOPMENT_ARM64_BCM2837'
ARCHIVE = 'libDbyWifiProbe.a'

RECORD_C = 'osfmk/DEVELOPMENT/startup.o.json'
RECORD_CXX = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

# name -> (record, language).  The C record builds the decision core, the C++ record the driver half,
# because that is what each file is.
SOURCES = {
    'DbyWifiProbeBoard.c': RECORD_C,
    'DbyWifiProbe.cpp': RECORD_CXX,
}

# The symbols the kernel link must see defined by this archive.  A typo, a mangled name or a missing
# extern "C" would otherwise only show up as "no row ever appears" on the device.
EXPORTS = ('dby_wifi_probe_entry', 'native_bt_rows', 'native_bt_stage',
           'dby_bt_m0_evaluate', 'dby_bt_m0_render', 'dby_bt_m0_verdict_name')

DEMOTIONS = [
    '-Wno-missing-designated-field-initializers',
    '-Wno-c99-designator',
    '-Wno-reorder-init-list',
    '-Wno-unnecessary-virtual-specifier',
]
# -Weverything is what the kernel's own compile uses, but the kernel never turns warnings into
# errors; combined with -Werror it drags in diagnostics the kernel does not demote.  The umbrella is
# dropped and the report records that, so nobody believes the archive was built under it.
DROPPED = {'-Weverything'}

# --------------------------------------------------------------------------------------------------
# --audit: every constant that came from the vendor tree, the file it came from, and how to find the
# number there.  The captured number is interpreted by `kind` and must then equal the header's value.
# A constant in the header with no entry here and not in INTERNAL is a magic number with no
# provenance, and the audit fails on it.
# --------------------------------------------------------------------------------------------------
DTS = 'arch/arm64/boot/dts/vendor/qcom/'
AUDIT = [
    # (macro, vendor file, regex with exactly one numeric capture, kind)
    ('DBY_BT_M0_TLMM_BASE',   DTS + 'kona-pinctrl.dtsi', r'tlmm:\s*pinctrl@([0-9a-f]+)\s*\{', 'hex'),
    ('DBY_BT_M0_TLMM_BYTES',  DTS + 'kona-pinctrl.dtsi', r'reg = <0x0F000000 (0x[0-9a-fA-F]+)>', 'hex'),
    ('DBY_BT_M0_TLMM_NORTH',  'drivers/pinctrl/qcom/pinctrl-kona.c', r'#define NORTH\s+(0x[0-9a-fA-F]+)', 'hex'),
    ('DBY_BT_M0_TLMM_WEST',   'drivers/pinctrl/qcom/pinctrl-kona.c', r'#define WEST\s+(0x[0-9a-fA-F]+)', 'hex'),
    ('DBY_BT_M0_TLMM_STRIDE', 'drivers/pinctrl/qcom/pinctrl-kona.c', r'#define REG_SIZE (0x[0-9a-fA-F]+)', 'hex'),
    ('DBY_BT_M0_TLMM_IO_OFF', 'drivers/pinctrl/qcom/pinctrl-kona.c', r'\.io_reg = base \+ (0x4)', 'hex'),
    ('DBY_BT_M0_CTL_OE_BIT',  'drivers/pinctrl/qcom/pinctrl-kona.c', r'\.oe_bit = (\d+)', 'hex'),
    ('DBY_BT_M0_IO_IN_BIT',   'drivers/pinctrl/qcom/pinctrl-kona.c', r'\.in_bit = (\d+)', 'hex'),
    ('DBY_BT_M0_CTL_FUNC_SHIFT', 'drivers/pinctrl/qcom/pinctrl-kona.c', r'\.mux_bit = (\d+)', 'hex'),
    ('DBY_BT_M0_CTL_DRIVE_SHIFT', 'drivers/pinctrl/qcom/pinctrl-kona.c', r'\.drv_bit = (\d+)', 'hex'),
    # The three field widths come from the vendor's own debugfs decoder, not from a register manual.
    ('DBY_BT_M0_CTL_FUNC_MASK',  'drivers/pinctrl/qcom/pinctrl-msm.c', r'func = \(ctl_reg >> g->mux_bit\) & (\d+)', 'hex'),
    ('DBY_BT_M0_CTL_DRIVE_MASK', 'drivers/pinctrl/qcom/pinctrl-msm.c', r'drive = \(ctl_reg >> g->drv_bit\) & (\d+)', 'hex'),
    ('DBY_BT_M0_CTL_PULL_MASK',  'drivers/pinctrl/qcom/pinctrl-msm.c', r'pull = \(ctl_reg >> g->pull_bit\) & (\d+)', 'hex'),

    ('DBY_BT_M0_GCC_BASE',   DTS + 'kona.dtsi', r'clock_gcc: qcom,gcc@([0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_BYTES',  DTS + 'kona.dtsi',
     r'clock_gcc: qcom,gcc@[0-9a-f]+ \{\s*\n\s*compatible = [^\n]*\n\s*reg = <0x100000 (0x[0-9a-fA-F]+)>', 'hex'),
    ('DBY_BT_M0_SE0_BASE',   DTS + 'kona-qupv3.dtsi', r'qupv3_se0_4uart: qcom,qup_uart@([0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_SE6_BASE',   DTS + 'kona-qupv3.dtsi', r'qupv3_se6_4uart: qcom,qup_uart@([0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_SE_BYTES',   DTS + 'kona-qupv3.dtsi',
     r'qupv3_se0_4uart: qcom,qup_uart@980000 \{\s*\n\s*compatible = [^\n]*\n\s*reg = <0x980000 (0x[0-9a-fA-F]+)>', 'hex'),

    ('DBY_BT_M0_GCC_VOTE_REG',   'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_s0_clk = \{[\s\S]{0,80}?\.enable_reg = (0x52008)', 'hex'),
    ('DBY_BT_M0_GCC_CORE_HALT',  'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_core_clk = \{\s*\n\s*\.halt_reg = (0x[0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_M_AHB_HALT', 'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap_0_m_ahb_clk = \{\s*\n\s*\.halt_reg = (0x[0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_S_AHB_HALT', 'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap_0_s_ahb_clk = \{\s*\n\s*\.halt_reg = (0x[0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_SE0_HALT',   'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_s0_clk = \{\s*\n\s*\.halt_reg = (0x[0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_SE6_HALT',   'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_s6_clk = \{\s*\n\s*\.halt_reg = (0x[0-9a-f]+)', 'hex'),
    ('DBY_BT_M0_GCC_VOTE_CORE',  'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_core_clk = \{[\s\S]{0,140}?\.enable_mask = BIT\((\d+)\)', 'shift'),
    ('DBY_BT_M0_GCC_VOTE_M_AHB', 'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap_0_m_ahb_clk = \{[\s\S]{0,140}?\.enable_mask = BIT\((\d+)\)', 'shift'),
    ('DBY_BT_M0_GCC_VOTE_S_AHB', 'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap_0_s_ahb_clk = \{[\s\S]{0,140}?\.enable_mask = BIT\((\d+)\)', 'shift'),
    ('DBY_BT_M0_GCC_VOTE_SE0',   'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_s0_clk = \{[\s\S]{0,140}?\.enable_mask = BIT\((\d+)\)', 'shift'),
    ('DBY_BT_M0_GCC_VOTE_SE6',   'drivers/clk/qcom/gcc-kona.c',
     r'gcc_qupv3_wrap0_s6_clk = \{[\s\S]{0,140}?\.enable_mask = BIT\((\d+)\)', 'shift'),
    ('DBY_BT_M0_CBCR_CLK_OFF',   'drivers/clk/qcom/clk-branch.c',
     r'#define BRANCH_CLK_OFF\s+BIT\((\d+)\)', 'shift'),

    ('DBY_BT_M0_GENI_INIT_CFG_REVISION', 'include/linux/qcom-geni-se.h',
     r'#define GENI_INIT_CFG_REVISION\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_STATUS', 'include/linux/qcom-geni-se.h',
     r'#define SE_GENI_STATUS\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_SER_M_CLK_CFG', 'include/linux/qcom-geni-se.h',
     r'#define GENI_SER_M_CLK_CFG\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_SER_S_CLK_CFG', 'include/linux/qcom-geni-se.h',
     r'#define GENI_SER_S_CLK_CFG\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_CLK_CTRL_RO', 'include/linux/qcom-geni-se.h',
     r'#define GENI_CLK_CTRL_RO\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_IF_FIFO_DISABLE', 'include/linux/qcom-geni-se.h',
     r'#define GENI_IF_FIFO_DISABLE_RO\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_FW_REVISION_RO', 'include/linux/qcom-geni-se.h',
     r'#define GENI_FW_REVISION_RO\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_FW_S_REVISION_RO', 'include/linux/qcom-geni-se.h',
     r'#define GENI_FW_S_REVISION_RO\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_CLK_SEL', 'include/linux/qcom-geni-se.h',
     r'#define SE_GENI_CLK_SEL\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_RX_FIFO_STATUS', 'include/linux/qcom-geni-se.h',
     r'#define SE_GENI_RX_FIFO_STATUS\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
    ('DBY_BT_M0_GENI_IOS', 'include/linux/qcom-geni-se.h',
     r'#define SE_GENI_IOS\s+\((0x[0-9a-fA-F]+)\)', 'hex'),
]

# Constants that are this probe's own design decisions, not hardware facts: row geometry, the
# gcc_valid bit assignment, and the pingroup function INDEX (which comes from the order of the
# pingroup's funcs[] array, audited by the self-test's mux cases instead).
INTERNAL = {
    'DBY_BT_M0_ROWS', 'DBY_BT_M0_ROW_BYTES', 'DBY_BT_M0_GENI_WORDS',
    'DBY_BT_M0_GCC_VALID_VOTE', 'DBY_BT_M0_GCC_VALID_CORE', 'DBY_BT_M0_GCC_VALID_M_AHB',
    'DBY_BT_M0_GCC_VALID_S_AHB', 'DBY_BT_M0_GCC_VALID_SE0', 'DBY_BT_M0_GCC_VALID_SE6',
    'DBY_BT_M0_GCC_VALID_ALL',
    'DBY_BT_M0_FUNC_GPIO', 'DBY_BT_M0_FUNC_QUP',
}


# --------------------------------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------------------------------

def build_command(record_path, source, obj, source_dir):
    """Reuse the kernel's own recorded command, with -Werror in front and the demotions last."""
    record = json.loads(Path(record_path).read_text(encoding='utf-8').strip().rstrip(','))
    args = list(record['arguments'])
    command = [args[0], '-Werror']
    i = 1
    skipped_includes = []
    while i < len(args):
        arg = args[i]
        if arg in DROPPED:
            i += 1
            continue
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-I' and i + 1 < len(args):
            # A pruned tree no longer carries every include directory the kernel build had, and
            # -Werror turns -Wmissing-include-dirs into a hard failure.  Skip the ones that are gone.
            directory_arg = args[i + 1]
            if Path(directory_arg).is_dir():
                command += [arg, directory_arg]
            else:
                skipped_includes.append(directory_arg)
            i += 1
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        else:
            command.append(arg)
        i += 1
    command += ['-Wall', '-Wextra', *DEMOTIONS, '-I', str(source_dir)]
    if '-o' not in command:
        command += ['-o', str(obj)]
    return command, record['directory'], skipped_includes


def nm_listing(archive):
    """A symbol listing from an nm that can actually read this archive.

    Not `xcrun nm` alone: outside the kernel build's PATH that shim resolves to binutils nm, which
    exits 0 on a Mach-O object after printing "file format not recognized" to stderr only, so the
    listing comes back empty and every export looks missing (2026-11-03 12:45).
    """
    attempts = (['llvm-nm', '-g', '-P'], ['xcrun', 'nm', '-g', '-P'], ['nm', '-g', '-P'])
    last = 'none tried'
    for prefix in attempts:
        try:
            listing = subprocess.check_output([*prefix, str(archive)], text=True, timeout=60,
                                              stderr=subprocess.DEVNULL)
        except (OSError, subprocess.SubprocessError) as error:
            last = f'{prefix[0]}: {error}'
            continue
        if listing.strip():
            return listing, ' '.join(prefix[:-3]) or prefix[0]
        last = f'{prefix[0]}: printed no symbols'
    raise SystemExit(f'no nm could read {archive} ({last}). A Mach-O archive needs llvm-nm; '
                     f'binutils nm exits 0 with an empty listing, so this is a tool problem, '
                     f'not a missing-symbol problem.')


def verify_archive(archive):
    """Refuse an archive that does not define the symbols the board's consumers name."""
    listing, tool = nm_listing(archive)
    defined = {}
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(':'):
            continue
        # Mach-O prints one leading underscore for an external C name.  Strip exactly one, so the
        # C++ form keeps its own leading underscore (`__Z19...` -> `_Z19...`).
        name = fields[0][1:] if fields[0].startswith('_') else fields[0]
        defined[name] = fields[1]
    missing = [name for name in EXPORTS if defined.get(name) not in ('T', 'D', 'S', 'B', 't', 'd', 's', 'b')]
    if missing:
        raise SystemExit(f'archive does not define (listed by {tool}): ' + ', '.join(missing))
    return {name: defined[name] for name in EXPORTS}


def header_constants(header):
    """The numeric constants in the board header, as Python ints.  Three spellings are understood:
    `#define X 0x123u`, `#define X 7`, and `#define X (1u << 31)`, each optionally followed by the
    `/* vendor file:line */` comment that gives it its provenance."""
    text = header.read_text(encoding='utf-8')
    found = {}
    tail = r'\s*(?:/\*[^\n]*\*/)?\s*$'
    for match in re.finditer(r'^#define\s+(DBY_BT_M0_[A-Z0-9_]+)\s+\(?\s*(0x[0-9a-fA-F]+|\d+)u?\s*\)?'
                             + tail, text, re.M):
        found[match.group(1)] = int(match.group(2), 0)
    for match in re.finditer(r'^#define\s+(DBY_BT_M0_[A-Z0-9_]+)\s+\(\s*1u?\s*<<\s*(\d+)\s*\)' + tail,
                             text, re.M):
        found[match.group(1)] = 1 << int(match.group(2))
    return found


def audit(vendor, header):
    """Every audited constant must still equal the vendor source it cites, and every constant must be
    either audited or declared internal - an unaudited constant is a magic number with no provenance."""
    constants = header_constants(header)
    audited = {macro for macro, _, _, _ in AUDIT}
    rows = []
    failures = 0
    for macro, relative, pattern, kind in AUDIT:
        path = Path(vendor) / relative
        if not path.is_file():
            rows.append((macro, relative, 'missing-file', None))
            failures += 1
            continue
        match = re.search(pattern, path.read_text(encoding='utf-8', errors='replace'), re.M)
        if match is None:
            rows.append((macro, relative, 'no-match', None))
            failures += 1
            continue
        raw = match.group(1)
        # Two spellings to survive: device-tree node names and cell values have no 0x prefix
        # (pinctrl@f000000, reg = <0x980000 0x4000> is prefixed but qcom,gcc@100000 is not), and a
        # `shift` capture is a plain BIT() index.  Interpreting a prefix-less capture as hex is safe
        # for every decimal capture in AUDIT, because those are single digits 0..9.
        if kind == 'shift':
            captured = int(raw, 10)
        else:
            captured = int(raw[2:], 16) if raw.lower().startswith('0x') else int(raw, 16)
        value = (1 << captured) if kind == 'shift' else captured
        have = constants.get(macro)
        status = 'ok' if have == value else f'header={have if have is None else hex(have)}'
        if have != value:
            failures += 1
        rows.append((macro, relative, status, value))
    width = max((len(r[0]) for r in rows), default=0)
    for macro, relative, status, value in rows:
        shown = 'n/a' if value is None else hex(value)
        print(f'{status:>16}  {macro:<{width}}  {shown:>12}  {relative}')
    unaudited = sorted(set(constants) - audited - INTERNAL)
    for macro in unaudited:
        print(f'{"NO-PROVENANCE":>16}  {macro:<{width}}  (add it to AUDIT or INTERNAL)')
    missing_header = sorted(audited - set(constants))
    for macro in missing_header:
        print(f'{"HEADER-MISSING":>16}  {macro}')
    if failures or unaudited or missing_header:
        raise SystemExit(f'audit FAILED: {failures} mismatch(es), {len(unaudited)} unaudited, '
                         f'{len(missing_header)} missing from the header')
    print(f'audit PASS: {len(AUDIT)} constants re-derived from {vendor}, '
          f'{len(INTERNAL)} declared internal')
    return {'audited': len(AUDIT), 'internal': len(INTERNAL), 'failures': 0}


def self_test(cc, package_dir, out_dir):
    """Compile the IOKit-free half with a host compiler, run its cases, print the rows they produce.

    This is the only part of this recipe that runs on a machine with no kernel tree and no iPadOS
    SDK, which is the point: the verdict ladder is exercised before anyone flashes anything.
    """
    compiler = cc or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        raise SystemExit('no host C compiler (--cc / cc / gcc / clang).  The self-test stays UNRUN '
                         'and must not be reported as passing.')
    out = Path(out_dir) / 'DbyWifiProbeBoard-selftest'
    command = [compiler, '-std=c99', '-Wall', '-Wextra', '-Werror', '-DDBY_BT_M0_SELFTEST=1',
               '-I', str(package_dir), str(package_dir / 'DbyWifiProbeBoard.c'), '-o', str(out)]
    print('self-test compile: ' + ' '.join(command))
    subprocess.run(command, check=True, timeout=120)
    result = subprocess.run([str(out)], text=True, capture_output=True, timeout=120)
    print(result.stdout, end='')
    if result.returncode != 0:
        raise SystemExit(f'self-test FAILED (exit {result.returncode})')
    return {'compiler': compiler, 'command': command, 'output': result.stdout.splitlines()}


def find_source_dir(explicit, tree):
    candidates = []
    if explicit is not None:
        candidates.append(Path(explicit))
    if tree is not None:
        candidates.append(Path(tree) / 'package')
    candidates += [ROOT / 'package', Path.cwd() / 'package']
    for candidate in candidates:
        if (candidate / 'DbyWifiProbe.cpp').is_file():
            return candidate.resolve()
    raise SystemExit('DbyWifiProbe.cpp not found in: ' + ', '.join(str(c) for c in candidates))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help="assembled kernel tree; defaults to this script's own tree root "
                             "(that is what `unixnu` uses when it runs an archive recipe) or "
                             '$UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path,
                        help='directory holding the two sources; defaults to the documented lookup order')
    parser.add_argument('--vendor', type=Path,
                        help='vendor kernel tree (_dby/robyy10__DBY-W09.tar/DBY-W09-main) for --audit')
    parser.add_argument('--audit', action='store_true', help='re-derive the header constants')
    parser.add_argument('--self-test', action='store_true', help='build and run the decision cases')
    parser.add_argument('--cc', help='host C compiler for --self-test')
    parser.add_argument('--no-verify', action='store_true', help='skip the post-build symbol check')
    args = parser.parse_args()

    # `unixnu` copies scripts/ into <tree>/scripts and runs the recipe there, so the script's own
    # parent-of-parent is the tree in the normal build path.  --tree/$UNIXNU_TREE overrides it.
    tree = (args.tree or ROOT).resolve()
    source_dir = find_source_dir(args.source_dir, tree)
    header = source_dir / 'DbyWifiProbeBoard.h'
    if not header.is_file():
        raise SystemExit(f'{header} is missing; the constants and the decision core ship together')

    report = {'sources': {}, 'hardware_execution': False}

    if args.audit:
        if args.vendor is None:
            raise SystemExit('--audit needs --vendor <vendor kernel tree>')
        report['vendor'] = str(Path(args.vendor).resolve())
        report['audit'] = audit(args.vendor, header)
    if args.self_test:
        with tempfile.TemporaryDirectory() as keep:
            report['self_test'] = self_test(args.cc, source_dir, keep)

    obj_root = tree / 'build/xnu/ios-obj' / ARCH
    if not obj_root.is_dir():
        if args.audit or args.self_test:
            print(json.dumps(report, indent=2))
            print('note: no assembled kernel tree, so only the requested phase(s) ran')
            return
        raise SystemExit(f'{tree} has no build/xnu/ios-obj/{ARCH}: point --tree at the assembled tree')

    for name in SOURCES:
        if not (source_dir / name).is_file():
            raise SystemExit(f'{source_dir / name} is missing')
    out = tree / OUT_NAME
    out.mkdir(parents=True, exist_ok=True)

    objects = []
    for name, record in SOURCES.items():
        record_path = obj_root / record
        if not record_path.is_file():
            raise SystemExit(f'{record_path} missing: build the kernel once before this archive')
        obj = out / (Path(name).stem + '.o')
        command, directory, skipped = build_command(record_path, source_dir / name, obj, source_dir)
        if skipped:
            print('note: skipped missing include dirs: ' + ', '.join(skipped))
        (out / f'compile-command-{Path(name).stem}.json').write_text(json.dumps(command, indent=2) + '\n', encoding='utf-8')
        subprocess.run(command, cwd=directory, check=True, timeout=300)
        objects.append(obj)
        report['sources'][name] = {
            'sha256': hashlib.sha256((source_dir / name).read_bytes()).hexdigest(),
            'object_bytes': obj.stat().st_size,
            'compile_record': str(record_path),
            'skipped_include_dirs': skipped,
        }

    archive = out / ARCHIVE
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]],
                   check=True, timeout=60)

    report['tree'] = str(tree)
    report['archive'] = archive.name
    report['archive_bytes'] = archive.stat().st_size
    report['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
    report['exported_symbols'] = {} if args.no_verify else verify_archive(archive)
    report['dropped_flags'] = sorted(DROPPED)
    report['added_flags'] = ['-Wall', '-Wextra', *DEMOTIONS]
    # The probe's own board facts, repeated so a build report says what the archive carries without
    # opening the source.
    report['board_deltas'] = {
        'opt_in_arg': 'dby-bt-m0=1|2',
        'mode1_windows': ['tlmm-north 0xf900000+0x100000', 'tlmm-west 0xf100000+0x100000',
                          'gcc 0x100000+0x1f0000'],
        'mode2_windows': ['se0 0x980000+0x4000', 'se6 0x998000+0x4000'],
        'geni_gate': 'mode 2 reads an SE only when the wrapper (core + m_ahb + s_ahb) and that SE\'s '
                     'own branch read enabled-and-CLK_OFF-clear',
        'writes': False,
        'verdicts': ['UART-SE0', 'UART-SE6', 'UNKNOWN'],
        'slimbus_reachable': False,
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: ' + ARCHIVE + ' built from ' + str(source_dir))
    print(f'      archive {archive} ({report["archive_bytes"]} bytes)')
    if report['exported_symbols']:
        print('      symbols: ' + ', '.join(f'{n}={t}' for n, t in report['exported_symbols'].items()))
    print('      link it with CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,'
          f'{tree / OUT_NAME / ARCHIVE}"')
    print('      then re-run: ./unixnu defconfig dby_w09_defconfig -D ../UniXNU-dby_w09')


if __name__ == '__main__':
    main()
