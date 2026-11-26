"""Check that this handover package is complete, or write its manifest.

Usage:
  python verify-package.py <package-root>            # verify every file + every cross-reference
  python verify-package.py <package-root> --write    # (re)generate 01-校验/MANIFEST.json

What "complete" means here, in the order the checks run:
  1. every file listed in MANIFEST.json exists, with the recorded byte size and sha256;
  2. every file on disk is in the manifest (nothing was added after it was written);
  3. the required set exists - the pieces the 2026-11-10 package was missing (UEFI, the touch
     headers, the touch protocol companion, the GENI source) and the pieces a rebuild needs;
  4. every package-relative path quoted in *any* shipped .md exists on disk;
  5. every recipe the board.toml names is either shipped in src/scripts/ or on the explicit list of
     scripts that belong to the UniXNU core tree;
  6. every device:patches/... entry of patch_series exists;
  7. the payload image's embedded command line equals board-stage179.toml's kernel_args, token by
     token - this is the check that catches "the docs describe a config the image does not carry";
  8. SILICIUM_UEFI.fd is a structurally valid EFI firmware volume of the declared size.

Exit code 0 = complete; 1 = something is missing or changed, with the reason printed.
"""
import hashlib
import importlib.util
import json
import re
import struct
import sys
import tomllib
from pathlib import Path

MANIFEST = Path('01-校验/MANIFEST.json')
MANIFEST_SELF = MANIFEST.as_posix()
# The manifest cannot certify itself, so its own digest lives one level up, in a file that the
# manifest deliberately does not list. --write refreshes both; verify checks the pair.
MANIFEST_DIGEST = Path('01-校验/MANIFEST.sha256')
MANIFEST_DIGEST_SELF = MANIFEST_DIGEST.as_posix()
SRC = Path('05-源码/src')
PAYLOAD = Path('03-payload/payload-stage179-touch-gpu.img')
STAGE179 = SRC / 'board-stage179.toml'
BOARD = SRC / 'board.toml'
UEFI_FD = Path('02-UEFI/SILICIUM_UEFI.fd')

# The pieces whose absence made the 2026-11-10 package unusable or incomplete.
REQUIRED = [
    '00-先读我.md',
    '02-UEFI/SILICIUM_UEFI.fd',
    '02-UEFI/uefi-dby_w09-改动.patch',
    '02-UEFI/说明-如何重建UEFI.md',
    '03-payload/payload-stage179-touch-gpu.img',
    '03-payload/说明-payload与换机重打.md',
    '04-GPU插件/gpu-addon.bin',
    '04-GPU插件/owner-trust.bundle',
    '04-GPU插件/NativeAdrenoMetal.bundle/Info.plist',
    '05-源码/00-源码说明.md',
    '05-源码/src/board.toml',
    '05-源码/src/board-stage179.toml',
    '05-源码/src/feeds.toml',
    '05-源码/src/porting.toml',
    '05-源码/src/configs/dby_w09_defconfig',
    '05-源码/src/dts/dby_w09.dts',
    '05-源码/src/package/DbyTouch/DbyTouch.cpp',
    '05-源码/src/package/DbyTouch/DbyTouchBoard.h',
    '05-源码/src/package/DbyTouch/DbyTouchFw.h',
    '05-源码/src/package/DbyTouch/DbyTouchProtocol.h',
    '05-源码/src/package/DbyTouch/DbyTouchProtocol.c',
    '05-源码/src/drivers/QcomGeniSpi/GeniSpi.h',
    '05-源码/src/drivers/QcomGeniSpi/GeniSpi.c',
    '05-源码/src/drivers/Adreno740/NativeAdrenoGpu.cpp',
    '05-源码/src/drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    '05-源码/src/package/LegacyDisplayProbe.cpp',
    '05-源码/src/package/BootLog.c',
    '05-源码/src/package/BootProgress.c',
    '05-源码/src/package/BootProcessProbe.c',
    '05-源码/src/package/BootUiTraceProbe.c',
    '05-源码/src/package/CpuFreqProbe.cpp',
    '05-源码/src/package/ShengPlatformPower.cpp',
    '05-源码/src/package/DbyGpuPowerProbe.cpp',
    '05-源码/src/package/DbyGpuSmmuProbe.cpp',
    '05-源码/src/package/DbyGpuFirmware.h',
    '05-源码/src/package/DbyGpuRegs.h',
    '05-源码/src/package/DbyA650Gmu.c',
    '05-源码/src/package/LinkStubs.c',
    '05-源码/src/package/DbyTorch/DbyTorch.cpp',
    '05-源码/src/package/QcomXnuUsbDebug/kernel/QcomXnuUsbDebug.c',
    '05-源码/src/patches/xnu/series',
    '05-源码/src/patches/xnu/series-usb',
    '05-源码/src/patches/xnu/series-audio',
    '05-源码/src/patches/xnu/series-torch',
    '05-源码/src/scripts/build-boot-progress.py',
    '05-源码/src/scripts/build-dby-touch.py',
    '05-源码/src/scripts/build-adreno650.py',
    '05-源码/src/scripts/build-adreno650-accelerator.py',
    '05-源码/src/scripts/build-gop-legacy-display-linux.py',
    '05-源码/src/scripts/build-storage-bindings.py',
    '05-源码/src/scripts/build-verified-root-archive.py',
    '05-源码/src/scripts/build-gpu-power-probe.py',
    '05-源码/src/scripts/build-gpu-smmu-probe.py',
    '05-源码/src/scripts/build-cpufreq.py',
    '05-源码/src/scripts/build-link-stubs.py',
    '05-源码/src/scripts/build-dby-torch.py',
    '05-源码/src/scripts/build-qcom-usb-debug.py',
    '05-源码/src/scripts/build-platform-power-linux.py',
    '05-源码/src/scripts/seed-mobile-graphics-record.py',
    '06-工具/read-window-8192-keep-edl.ps1',
    '06-工具/decode-thisboot.py',
    '06-工具/read-payload-args.py',
    '06-工具/flash-preload-edl.ps1',
    '06-工具/edl/readback-window.xml',
    '07-证据/gpu-实测摘要.md',
    '07-证据/s151-关键页摘录.txt',
    '07-证据/lun0-partitions.txt',
]

# Sanity bounds for the files whose size or content is itself part of the contract.
SIZE_CHECKS = {
    '02-UEFI/SILICIUM_UEFI.fd': 8388608,
    '03-payload/payload-stage179-touch-gpu.img': 218103808,
    '04-GPU插件/gpu-addon.bin': 12154024,
    '05-源码/src/package/DbyTouch/DbyTouch.cpp': 178752,      # the device-verified v3, 4298 lines
    '05-源码/src/package/DbyTouch/DbyTouchFw.h': 1192808,
    '05-源码/src/configs/dby_w09_defconfig': 14438,
}

# Recipes board.toml names that belong to the UniXNU core tree, not to the board repository.  Every
# other .py it names must be shipped in 05-源码/src/scripts/.
CORE_RECIPES = {
    'build-corecrypto-14.py',
    'build-software-aes-bindings.py',
    'build-virtio-mmio-bindings.py',
    'build-arm-driver.py',
    'build-verified-root-probe.py',
    'build-libpthread-kernel.py',
    'seed-data-skip-setup.py',
}

# Paths the READMEs quote that live inside the package (top-level directories only).
PACKAGE_DIRS = ('00-', '01-', '02-', '03-', '04-', '05-', '06-', '07-')

# Side effects of running the shipped tools. They are not part of the deliverable, so they are never
# required and never reported as "not in manifest" after the recipient has used a tool once.
GENERATED_FILES = {
    '06-工具/edl/program-preload.xml',
    '06-工具/edl/power-reset.xml',
}

FV_LENGTH_OFFSET = 0x20
FV_SIGNATURE_OFFSET = 0x28
FV_SIGNATURE = b'_FVH'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def files_on_disk(root):
    """Every file that is part of the deliverable.

    Three kinds of file are deliberately not part of it and must not make the check fail:
      * Python bytecode and __pycache__ - written by whichever interpreter the recipient runs, so a
        cpython-311.pyc produced on their machine would otherwise read as "not in manifest";
      * the XML that 06-工具/flash-preload-edl.ps1 writes into 06-工具/edl/ as a side effect.
    """
    out = []
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if '__pycache__' in rel or path.suffix == '.pyc':
            continue
        if rel in GENERATED_FILES:
            continue
        out.append(rel)
    return sorted(out)


def write_manifest(root):
    entries = []
    for rel in files_on_disk(root):
        if rel in (MANIFEST_SELF, MANIFEST_DIGEST_SELF):
            continue
        path = root / rel
        entries.append({'path': rel, 'bytes': path.stat().st_size, 'sha256': sha256(path)})
    target = root / MANIFEST
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(entries, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'manifest: {len(entries)} files -> {target}')
    digest = sha256(target)
    (root / MANIFEST_DIGEST).write_text(digest + '  MANIFEST.json\n', encoding='utf-8')
    print(f'manifest sha256: {digest}  -> {MANIFEST_DIGEST}')
    return 0


def load_payload_reader(root):
    """Import 06-工具/read-payload-args.py so the arg comparison uses the shipped scanner."""
    path = root / '06-工具/read-payload-args.py'
    spec = importlib.util.spec_from_file_location('read_payload_args', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_recipes(root, board_path, problems):
    """Every non-component recipe board.toml names must be shipped or be a known core script.

    Counts distinct recipe names, not archive entries: the same script legitimately appears more
    than once with different arguments (build-corecrypto-14.py, build-storage-bindings.py).
    """
    if not board_path.is_file():
        problems.append(f'missing board file: {board_path.relative_to(root).as_posix()}')
        return 0, 0
    data = tomllib.loads(board_path.read_text(encoding='utf-8'))
    archives = data.get('kernel', {}).get('archives', [])
    scripts_dir = root / SRC / 'scripts'
    seen = set()
    core_used = set()
    for entry in archives:
        if entry.startswith('component:'):
            continue
        recipe = entry.split()[0]
        if not recipe.endswith('.py'):
            problems.append(f'archive entry names something that is not a recipe: {entry}')
            continue
        seen.add(recipe)
        if (scripts_dir / recipe).is_file():
            continue
        if recipe in CORE_RECIPES:
            core_used.add(recipe)
            continue
        problems.append(f'board.toml archive recipe is neither shipped nor a known core script: {recipe}')
    return len(seen), len(core_used)


def check_patch_series(root, board_path, problems):
    """Every device: series must exist, and every patch file a shipped series lists must exist.

    Checking only the series files themselves is not enough: a series is a list of patch files, and
    dropping one of those leaves a build that applies half a series.
    """
    if not board_path.is_file():
        return 0
    data = tomllib.loads(board_path.read_text(encoding='utf-8'))
    series = data.get('kernel', {}).get('patch_series', [])
    checked = 0
    parsed = set()
    for entry in series:
        if not entry.startswith('device:'):
            continue  # arch:/soc:/os: live in the core tree
        rel = entry.split(':', 1)[1]
        checked += 1
        path = root / SRC / rel
        if not path.is_file():
            problems.append(f'patch_series names a device patch file that is not shipped: {rel}')
            continue
        parsed.add(path)
    for path in sorted(parsed):
        directory = path.parent
        for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            if ':' in line:
                continue  # arch:/soc:/os:-prefixed entries resolve in the core tree
            if not (directory / line).is_file():
                problems.append(
                    f'{path.relative_to(root).as_posix()} lists a patch that is not shipped: {line}')
    return checked


def check_board_budget(root, board_path, problems):
    """board.toml's own kernel_args must fit the 1000-byte command line."""
    if not board_path.is_file():
        return 0, 0
    data = tomllib.loads(board_path.read_text(encoding='utf-8'))
    args = data.get('payload', {}).get('kernel_args', [])
    joined = ' '.join(args)
    if len(joined) > 1000:
        problems.append(
            f'{board_path.relative_to(root).as_posix()}: command line is {len(joined)} bytes, '
            f'over the 1000-byte cap')
    return len(args), len(joined)


def check_doc_paths(root, problems):
    """Every package-relative path quoted in any shipped .md must exist.

    Fenced code blocks are removed first: their ``` delimiters would otherwise pair with the inline
    backticks and turn the scan into a search for prose (that bug made the first version of this
    checker report "4 paths checked" while the docs quote dozens).
    """
    checked = 0
    fence = re.compile(r'```.*?```', re.S)
    for doc in sorted(root.rglob('*.md')):
        text = fence.sub('', doc.read_text(encoding='utf-8', errors='replace'))
        for item in sorted(set(re.findall(r'`([^`]+)`', text))):
            if not item.startswith(PACKAGE_DIRS):
                continue
            if any(token in item for token in ('*', ' ', '<', '>', '…')):
                continue
            cleaned = item.rstrip('/')
            checked += 1
            if not (root / cleaned).exists():
                rel = doc.relative_to(root).as_posix()
                problems.append(f'{rel} quotes a path that does not exist: {item}')
    return checked


def check_payload_args(root, problems):
    """The image's command line must equal board-stage179.toml's kernel_args, token by token."""
    if not (root / PAYLOAD).is_file() or not (root / STAGE179).is_file():
        return None
    reader = load_payload_reader(root)
    offset, run = reader.find_command_line((root / PAYLOAD).read_bytes())
    if not run:
        problems.append(f'{PAYLOAD.as_posix()}: no DarwinBoot command line found')
        return None
    image_args = run.decode('ascii').split()
    data = tomllib.loads((root / STAGE179).read_text(encoding='utf-8'))
    board_args = data.get('payload', {}).get('kernel_args', [])
    if image_args != board_args:
        problems.append(
            f'{STAGE179.name} does not reproduce the payload: '
            f'image has {len(image_args)} args, file has {len(board_args)}')
        only_file = [a for a in board_args if a not in image_args]
        only_image = [a for a in image_args if a not in board_args]
        if only_file:
            problems.append(f'  only in {STAGE179.name}: {only_file}')
        if only_image:
            problems.append(f'  only in the image: {only_image}')
        if not only_file and not only_image:
            problems.append('  same arguments, different order')
    joined = ' '.join(board_args)
    if len(joined) > 1000:
        problems.append(f'{STAGE179.name}: command line is {len(joined)} bytes, over the 1000-byte cap')
    if len(' '.join(image_args)) > 1000:
        problems.append(f'the image command line is {len(" ".join(image_args))} bytes, over the cap')
    return offset


def check_uefi_fd(root, problems):
    path = root / UEFI_FD
    if not path.is_file():
        return
    with path.open('rb') as handle:
        header = handle.read(FV_SIGNATURE_OFFSET + len(FV_SIGNATURE))
    if len(header) < FV_SIGNATURE_OFFSET + len(FV_SIGNATURE):
        problems.append(f'{UEFI_FD.as_posix()}: too short to hold an FV header')
        return
    if header[FV_SIGNATURE_OFFSET:FV_SIGNATURE_OFFSET + len(FV_SIGNATURE)] != FV_SIGNATURE:
        problems.append(f'{UEFI_FD.as_posix()}: no {FV_SIGNATURE!r} signature at 0x{FV_SIGNATURE_OFFSET:x}')
        return
    length = struct.unpack_from('<Q', header, FV_LENGTH_OFFSET)[0]
    if length != path.stat().st_size:
        problems.append(f'{UEFI_FD.as_posix()}: FvLength 0x{length:x} != file size 0x{path.stat().st_size:x}')


def verify(root):
    problems = []

    manifest_path = root / MANIFEST
    if not manifest_path.is_file():
        problems.append(f'missing {MANIFEST} (run with --write once)')
        return problems
    entries = json.loads(manifest_path.read_text(encoding='utf-8'))
    recorded = {entry['path']: entry for entry in entries}

    # The manifest cannot certify itself; MANIFEST.sha256 is the anchor above it.
    digest_path = root / MANIFEST_DIGEST
    if not digest_path.is_file():
        problems.append(f'missing {MANIFEST_DIGEST} (run with --write once)')
    else:
        recorded_digest = digest_path.read_text(encoding='utf-8').split()[0]
        actual_digest = sha256(manifest_path)
        if recorded_digest != actual_digest:
            problems.append(
                f'{MANIFEST_DIGEST} does not match MANIFEST.json: '
                f'recorded {recorded_digest}, actual {actual_digest}')

    for rel, entry in sorted(recorded.items()):
        path = root / rel
        if not path.is_file():
            problems.append(f'missing file: {rel}')
            continue
        size = path.stat().st_size
        if size != entry['bytes']:
            problems.append(f'size differs: {rel} (manifest {entry["bytes"]}, disk {size})')
            continue
        if sha256(path) != entry['sha256']:
            problems.append(f'sha256 differs: {rel}')

    for rel in files_on_disk(root):
        if rel in (MANIFEST_SELF, MANIFEST_DIGEST_SELF) or rel in recorded:
            continue
        problems.append(f'not in manifest: {rel}')

    for rel in REQUIRED:
        if not (root / rel).is_file():
            problems.append(f'required but missing: {rel}')

    for rel, expected in SIZE_CHECKS.items():
        path = root / rel
        if path.is_file() and path.stat().st_size != expected:
            problems.append(f'unexpected size: {rel} (expected {expected}, disk {path.stat().st_size})')

    recipes, core_used = check_recipes(root, root / BOARD, problems)
    series = check_patch_series(root, root / BOARD, problems)
    budget = check_board_budget(root, root / BOARD, problems)
    quoted = check_doc_paths(root, problems)
    offset = check_payload_args(root, problems)
    check_uefi_fd(root, problems)

    print(f'board.toml recipes checked: {recipes} ({core_used} of them resolve to the core-tree allowlist)')
    print(f'device patch series checked: {series} (and every patch file they list)')
    if budget is not None:
        count, size_b = budget
        verdict = 'under' if size_b <= 1000 else 'OVER'
        print(f'board.toml command line: {count} args, {size_b} bytes ({verdict} the 1000-byte cap)')
    print(f'doc-quoted paths checked: {quoted}')
    if offset is not None:
        print(f'payload command line: matches board-stage179.toml (blob at 0x{offset:x})')
    return problems


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    if not root.is_dir():
        raise SystemExit(f'not a directory: {root}')
    if '--write' in sys.argv:
        return write_manifest(root)

    problems = verify(root)
    if problems:
        print(f'INCOMPLETE: {len(problems)} problem(s)')
        for problem in problems:
            print(f'  - {problem}')
        return 1
    total = len(files_on_disk(root))
    size = sum((root / rel).stat().st_size for rel in files_on_disk(root))
    print(f'OK: {total} files, {size / (1024 * 1024):.1f} MiB, manifest and required set all match')
    print(f'MANIFEST.json sha256: {sha256(root / MANIFEST)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
