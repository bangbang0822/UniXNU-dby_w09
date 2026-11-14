#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Bounded ASan/UBSan checks of production root layout and window reads.

A sanitizer diagnostic is a failure, not output: the binaries are built with
-fno-sanitize-recover=undefined so UBSan halts, and every log is scanned for "runtime error"
and "ERROR: AddressSanitizer" as well, because a recoverable report would otherwise be written
next to a PASS line and the exit status would stay 0.
"""
import argparse
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--openssl-prefix',type=Path)
parser.add_argument('--root-image',type=Path,help='Optional existing image: read only its first 4096 bytes')
args=parser.parse_args()
source=Path(__file__).resolve().parent
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
openssl=args.openssl_prefix or Path(subprocess.check_output(['brew','--prefix','openssl@3'],text=True).strip())
# SmmuTakeover, TransferPoll and RootCommands have a main() and were referenced by nothing in this
# repository, so their fault cases never ran here. None of the three needs OpenSSL or QEMU.
for name in ('RootLayout','RootWindowRead','RootDiagnostics','RootPublication',
             'SmmuTakeover','TransferPoll','RootCommands'):
    binary=out/name
    command=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-g',str(source/(name+'.cpp')),'-o',str(binary)]
    if name=='RootLayout':command+=['-I',str(openssl/'include'),'-L',str(openssl/'lib'),'-lcrypto']
    subprocess.run(command,check=True,timeout=60)
    command=[str(binary)]
    if name=='RootLayout' and args.root_image:command.append(str(args.root_image.resolve()))
    with (out/(name+'.log')).open('w') as log:subprocess.run(command,check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
    report=(out/(name+'.log')).read_text(errors='replace')
    for marker in ('runtime error:', 'ERROR: AddressSanitizer'):
        if marker in report:
            raise SystemExit(f'{name}: sanitizer reported {marker!r}, see {out/(name+".log")}')
    print('PASS:',name)
with (out/'media-diagnostics.txt').open('w') as log:
    subprocess.run([str(out/'RootDiagnostics'),'--media'],check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
report=(out/'media-diagnostics.txt').read_text(errors='replace')
for marker in ('runtime error:', 'ERROR: AddressSanitizer'):
    if marker in report:
        raise SystemExit(f'RootDiagnostics: sanitizer reported {marker!r}')
print('PASS: maximum-width diagnostics plus eight media entries prepared for actual renderer')
