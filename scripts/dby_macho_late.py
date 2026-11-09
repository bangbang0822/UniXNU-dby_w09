#!/usr/bin/env python3
"""Mach-O "late object" patch, done on the bytes because llvm-objcopy cannot do it here.

The upstream macOS recipe runs

    llvm-objcopy --remove-section=__DATA,__mod_init_func \
                 --globalize-symbol=__GLOBAL__sub_I_<Something>.cpp in.o out.o

Neither llvm-objcopy shipped on this Linux host supports those two options for Mach-O input
("option is not supported for MachO"), so the kernel build died at the first archive recipe that
needs a late object. Both effects are easy on the bytes:

  * The loader only runs entries of a section literally named ``__mod_init_func``, so renaming the
    section to ``__mod_init_fcn`` has the same effect as removing it.
  * ``--globalize-symbol`` is a single N_EXT bit in the symbol table entry.

The caller passes the result to ``ar``; nothing else about the object changes.
"""
import struct
import sys
from pathlib import Path

# Import the kit's Mach-O reader; the caller has already put the prelink kit on sys.path.
from native_prelink import macho


def _globalize(data, name):
    ncmds = struct.unpack_from('<I', data, 16)[0]
    offset = 32
    for _ in range(ncmds):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x2:                                   # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, offset + 8)
            for index in range(nsyms):
                base = symoff + index * 16
                strx, ntype = struct.unpack_from('<IB', data, base)
                start = stroff + strx
                end = data.index(b'\0', start)
                if bytes(data[start:end]).decode('utf-8', 'replace') == name:
                    data[base + 4] = ntype | 0x01            # N_EXT
                    return True
            return False
        offset += size
    return False


def patch_late(source, destination, symbol):
    """Write `source` to `destination` with __mod_init_func renamed and `symbol` globalized."""
    data = bytearray(Path(source).read_bytes())
    meta = macho(bytes(data))
    renamed = False
    for segment in meta['segments']:
        for section in segment['sections']:
            if section['name'] == '__mod_init_func':
                start = section['command_offset']
                data[start:start + 16] = b'__mod_init_fcn\0\0'
                renamed = True
    if not renamed:
        raise SystemExit(f'{Path(source).name}: no __mod_init_func section')
    if not _globalize(data, symbol):
        raise SystemExit(f'{Path(source).name}: symbol {symbol} not found')
    Path(destination).write_bytes(bytes(data))
    print(f'patched {Path(source).name} -> {Path(destination).name} (Mach-O patch, no objcopy)')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: dby_macho_late.py <in.o> <out.o> <symbol>')
    patch_late(sys.argv[1], sys.argv[2], sys.argv[3])
