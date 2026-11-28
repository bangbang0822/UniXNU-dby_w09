#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
"""Build build/dby-camera/libDbyCamera.a: the kernel half of the camera log service.

Run by the UniXNU kernel step as an archive recipe (board.toml [kernel] archives, cwd = the tree), listed
right after scripts/build-dby-torch.py. One object: the mother's libPaganiCamera.a (refer/PaganiDrivers/
drivers/PaganiCamera/scripts/build-pagani-camera.py) also carries the stage-2 camera driver -
CamBringup.c, CamCci.c, CamPmic.c, PaganiCamera.cpp - and this board has not ported that driver (#212 still
has no SM8250/kona constant table), so what is left of that archive here is the service the H6ISP plug-in
writes its "[cam] ios:" lines through (package/DbyCamera/DbyCameraIos.cpp).

The compile-record replay, the Mach-O audit, the objcopy proof, the symbol tables and the archive-index trap
are imported from scripts/build-dby-torch.py, not copied a third time - the same rule the mother's camera
recipe states for its bluetooth and torch recipes. A refusal raised by that machinery keeps the torch
script's label, because that is the file that owns the check.

The class initializer is taken out of __DATA,__mod_init_func and globalized as dby_camera_ios_static_init, so
a kernel without dby-ti=1 has neither the DbyCameraLog class nor the service - the identical treatment
DbyTorchIos.cpp gets, and the reason it exists at all (an IOKit class registered from a static initializer
inside a board archive) is written up in README.md.

Link topology: the caller is the weak import in patches/xnu/zze-dby-torch-media-mount.patch, so the patched
kern_exec.c carries an undefined _dby_camera_ios_publish and ld64 pulls this archive to answer it, the same way
it pulls libDbyTorch.a for _dby_torch_ios_commit. CONFIG_XNU_EXTRA_LD_ARCHIVES
(configs/dby_w09_defconfig:143: appended to KERNEL_EXTRA_ARCHIVES, the plain archive path, pulled by an
undefined symbol, not force_load) lists it. No board archive imports a name defined here, which is why
scripts/build-dby-torch.py's import table is untouched: that gate requires every name libDbyTorch.a leaves
undefined to be a symbol the reference kernel defines, and a sibling board archive is not one - the mother keeps
the caller and the definition in the same component (PaganiCamera.cpp:242 calls pagani_camera_ios_publish()).

Gate before anything compiles: the three numbers the plug-in and this kernel file have to agree on
(DBC_PROCESS, DBC_LOG_SELECTOR, DbcLogMax) are read out of package/DbyCamera/ios/DbyCameraProvider.h, which
is the header the plug-in itself includes. They are names in one file and constants in the other, so a drift
is a silent refusal, not a link error.

  build-dby-camera.py [--tree TREE] [--source-dir DIR] [--out DIR]
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_dby_torch', ROOT / 'scripts/build-dby-torch.py')
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)

SOURCE_FILES = ['DbyCameraIos.cpp', 'DbyCamera.cpp', 'DbyCamBringup.c']
HEADERS = ['ios/DbyCameraProvider.h', 'DbyCamera.h', 'DbyCamBoard.h', 'DbyCamBringup.h', 'DbyCamText.h']
INITIALIZER = '__GLOBAL__sub_I_DbyCameraIos.cpp'
REQUIRED = ['_dby_camera_ios_publish', '_dby_camera_ios_status', '_dby_camera_start', '_dby_camera_status', INITIALIZER]
# The kernel names this archive takes, outside the IOKit and libkern mangled __Z shapes: the process-name gate
# and the kprintf sink (both already in the torch recipe's allowlist), plus the ones the stage-2 ladder adds -
# IOKit window mapping, the delay pair, kernel_thread_start and the thread-name setter. Everything else it
# imports (IODeviceMemory, OSSafeReleaseNULL, the IOKit base classes) comes in as a mangled __Z name, which the
# allowlist accepts by prefix and the kernel symbol table then has to answer.
EXTRA_IMPORTS = {'_kprintf', '_proc_selfname', '_g_kernel_kmod_info', '_IOMallocData', '_IOFreeData',
                 '_IODelay', '_IOSleep', '_kernel_thread_start', '_thread_deallocate', '_thread_set_thread_name',
                 '_current_thread', '_KHEAP_DATA_BUFFERS',
                 # The RPMh helper (package/QcomRpmh.c) is linked into the kernel by the audio probe's archive, so
                 # the shipping kernel defines these five - the same names AdspProbe.cpp and AudioProbe.cpp import.
                 '_qcom_cmd_db_find', '_qcom_rpmh_level_index', '_qcom_rpmh_active_request', '_qcom_rpmh_finish',
                 '_qcom_rsc_layout'}
# Those five are not kernel imports in a tree whose link predates the archive builders: the bootstrap pass links
# before any archive exists, so its kernel image cannot define them. They stay legal imports, but the kernel-symbol
# gate must not demand them from that kernel; the shipping link is what proves the audio archive supplies them.
SIBLING_ARCHIVE_IMPORTS = {'_qcom_cmd_db_find', '_qcom_rpmh_level_index', '_qcom_rpmh_active_request',
                           '_qcom_rpmh_finish', '_qcom_rsc_layout'}
# What the plug-in's header must say, compared as the macro text itself: DBC_PROCESS is a string literal,
# so the quotes are part of what the header says.
EXPECT = {'DBC_PROCESS': '"mediaserverd"', 'DBC_LOG_SELECTOR': '0u', 'DbcLogMax': '96'}


def fail(message):
    raise SystemExit('build-dby-camera: ' + message)


def check_contract(header):
    """The plug-in and the kernel file speak through three constants, not through a shared symbol."""
    text = header.read_text()
    found = {}
    for name in ('DBC_PROCESS', 'DBC_LOG_SELECTOR'):
        m = re.search(r'#define\s+' + name + r'\s+(.+?)\s*(?:/\*.*?\*/)?\n', text)
        if not m:
            fail(f'{header.name} does not define {name}')
        found[name] = m.group(1).strip()
    m = re.search(r'enum \{([^}]*)\}', text)
    if not m:
        fail(f'{header.name} has no enum for DbcLogMax')
    found['DbcLogMax'] = next((p.split('=')[1].strip() for p in m.group(1).split(',') if p.strip().startswith('DbcLogMax')), None)
    if found['DbcLogMax'] is None:
        fail(f'{header.name} does not define DbcLogMax')
    problems = [f'{name} is {found[name]}, expected {EXPECT[name]}' for name in EXPECT if found[name] != EXPECT[name]]
    if problems:
        fail('the plug-in contract moved without this recipe: ' + '; '.join(problems))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='assembled kernel tree; `unixnu` runs a recipe from the tree, so the default is $UNIXNU_TREE')
    parser.add_argument('--source-dir', type=Path, help='DbyCamera sources; defaults to the lookup order')
    parser.add_argument('--out', type=Path, help='output directory (default: TREE/build/dby-camera)')
    args = parser.parse_args()
    tree = (args.tree or ROOT).resolve()
    if not (tree / bt.RECORD_DIR).is_dir():
        fail(f'{tree} has no {bt.RECORD_DIR}: point --tree at the assembled tree')
    candidates = [args.source_dir] if args.source_dir else [tree / 'drivers/DbyCamera', tree / 'kits/dby_w09/DbyCamera',
                                                            ROOT / 'package/DbyCamera']
    source = next((c.resolve() for c in candidates if c and (c / SOURCE_FILES[0]).is_file()), None)
    if source is None:
        fail('DbyCameraIos.cpp not found in: ' + ', '.join(str(c) for c in candidates))
    missing = [n for n in SOURCE_FILES + HEADERS if not (source / n).is_file()]
    if missing:
        fail('missing sources: ' + ', '.join(missing))
    # The torch directory: this object is compiled with the torch text builder (DbyTorchText.h), because the
    # mother's CamUtil.h is not ported. Same lookup order the torch recipe uses.
    torch_candidates = [c.parent / 'DbyTorch' for c in candidates]
    torch_source = next((c.resolve() for c in torch_candidates if c and (c / 'DbyTorchText.h').is_file()), None)
    if torch_source is None:
        fail('DbyTorchText.h not found beside the camera sources: ' + ', '.join(str(c) for c in torch_candidates))
    contract = check_contract(source / HEADERS[0])
    out = (args.out or tree / 'build/dby-camera').resolve()
    shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    cxx_record = bt.load_record(tree, bt.CXX_RECORD)
    c_record = bt.load_record(tree, bt.C_RECORD)
    # -I source.parent: DbyCamera.cpp includes package/QcomRpmh.h (the RPMh helper the audio probe uses), which sits
    # one level above this package's directory, not inside it.
    extra = ['-I', str(source), '-I', str(source.parent), '-I', str(torch_source), '-mgeneral-regs-only'] + bt.WARNINGS
    # The ladder and the board constants are C; the text builder is header-only, so the C object takes the same
    # include paths and no C++ standard library. -mgeneral-regs-only stays off for the C object: XNU builds its C
    # half with the general registers available, and the C record already carries the tree's own flags.
    c_extra = ['-I', str(source), '-I', str(source.parent), '-I', str(torch_source)] + bt.WARNINGS
    objects = []
    skipped = []
    # The one object is the class-carrying one, so it is compiled once and only the staged copy enters the
    # archive - the same pair of names the torch recipe keeps apart with its .torch.o / .torch.late.o suffix.
    staged = out / 'obj' / 'DbyCameraIos_cpp.cam.o'
    skipped += bt.compile_one(cxx_record, source / SOURCE_FILES[0], staged, extra)
    late = out / 'obj' / 'DbyCameraIos_cpp.cam.late.o'
    late.unlink(missing_ok=True)
    subprocess.run([bt.tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
                    '--globalize-symbol=' + INITIALIZER, str(staged), str(late)], check=True, timeout=300)
    if bt.text(staged) != bt.text(late) or any(sect == '__mod_init_func' for _, sect in bt.sections(late)):
        fail('staging the class initializer changed its instructions or kept __mod_init_func')
    objects.append(late)
    # Stage 2: the glue (windows, ARC, RPMh rail vote, thread) and the portable ladder. These carry no static
    # initializer, so they go into the archive as compiled.
    glue = out / 'obj' / 'DbyCamera_cpp.cam.o'
    skipped += bt.compile_one(cxx_record, source / 'DbyCamera.cpp', glue, extra)
    objects.append(glue)
    ladder = out / 'obj' / 'DbyCamBringup_c.cam.o'
    skipped += bt.compile_one(c_record, source / 'DbyCamBringup.c', ladder, c_extra)
    objects.append(ladder)
    archive = out / 'libDbyCamera.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=300)
    index = bt.symdef(archive)
    if not index.startswith('__.SYMDEF'):
        fail(f'{archive}: symbol index is {index!r}, not __.SYMDEF; the same GNU-ar trap the torch recipe names '
             '(a GNU index of a Mach-O archive lists none of its symbols, so ld64 would pull nothing and the '
             'kernel-s undefined _dby_camera_ios_publish would fail the link or, worse, the service would be silently absent)')
    defined, undefined = bt.symbols(archive)
    problems = [f'{name} not defined' for name in REQUIRED if name not in defined]
    allowed = EXTRA_IMPORTS | {n for n in bt.KERNEL_IMPORTS}
    problems += ['unexpected kernel import ' + name for name in sorted(undefined) if name not in allowed and not name.startswith('__Z')]
    if problems:
        fail('archive symbol check failed:\n  ' + '\n  '.join(problems))
    kernel = tree / bt.KERNEL_IMAGE
    table = bt.kernel_symbols(kernel)
    deferred = []
    if table is not None:
        absent = sorted(name for name in undefined if name not in table and name not in SIBLING_ARCHIVE_IMPORTS)
        deferred = sorted(name for name in undefined if name in SIBLING_ARCHIVE_IMPORTS and name not in table)
        if absent:
            fail(f'{kernel} does not define {len(absent)} import(s): ' + ', '.join(absent[:12]))
    if deferred:
        print('note: sibling-archive imports deferred to the shipping link: ' + ', '.join(deferred))
    listed = [source / n for n in SOURCE_FILES + HEADERS] + [torch_source / 'DbyTorchText.h', Path(__file__).resolve()]
    report = {'tree': str(tree), 'source_dir': str(source), 'text_builder': str(torch_source), 'archive': str(archive),
              'archive_bytes': archive.stat().st_size, 'archive_sha256': bt.sha(archive), 'archive_index': index,
              'objects': {obj.name: obj.stat().st_size for obj in objects},
              'source_sha256': {p.name: bt.sha(p) for p in listed},
              'plugin_contract': contract, 'entry_points': REQUIRED, 'undefined': sorted(undefined),
              'warning_flags': bt.WARNINGS, 'skipped_include_dirs': skipped,
              'kernel_image': str(kernel), 'kernel_imports_checked_against_kernel': table is not None,
              'sibling_archive_imports': sorted(SIBLING_ARCHIVE_IMPORTS & undefined),
              'kernel_imports_deferred': deferred,
              'hardware_execution': False}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if skipped:
        print('note: skipped missing include dirs: ' + ', '.join(sorted(set(skipped))))
    print(f'PASS: {len(objects)} object in {archive} ({len(REQUIRED)} entry points, {len(undefined)} kernel imports, '
          f'contract {contract})')


if __name__ == '__main__':
    main()
