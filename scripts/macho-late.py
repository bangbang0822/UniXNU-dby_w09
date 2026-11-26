# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Mechanical Mach-O edits that turn a compiled accelerator object into its "late" form.

scripts/build-adreno650-accelerator.py stages NativeAdrenoAccelerator.cpp into the kernel's
driver archive, but that class must not run its static initializer when the kernel image is
linked: the board calls it at the moment the GPU is actually brought up (mode 25).  The
reference board gets this from llvm-objcopy:

    llvm-objcopy --remove-section=__DATA,__mod_init_func \
                 --globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp obj late

On this host llvm-objcopy (LLVM 11/14) only implements those options for ELF, so the same two
edits are made directly on the Mach-O here.  Both are in-place byte patches: the file keeps its
size, and no section contents move, which is what the caller re-checks by comparing __text of
the input and the output.

Usage:
  python3 scripts/macho-late.py <object.o> <late.o>
"""
import struct
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
MH_OBJECT = 0x1
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_TYPE = 0x0E
N_EXT = 0x01
N_SECT = 0x0E

SECTNAME = '__mod_init_func'


def commands(raw):
    magic = struct.unpack_from('<I', raw, 0)[0]
    if magic != MH_MAGIC_64:
        raise SystemExit('not a 64-bit Mach-O')
    filetype = struct.unpack_from('<I', raw, 12)[0]
    if filetype != MH_OBJECT:
        raise SystemExit('not a Mach-O object (filetype %#x)' % filetype)
    ncmds = struct.unpack_from('<I', raw, 16)[0]
    out, off = [], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', raw, off)
        if size < 8:
            raise SystemExit('truncated load command at %#x' % off)
        out.append((cmd, off, size))
        off += size
    return out


def sections(raw, commands_):
    """Offsets of every section_64 header, with its segment and section names."""
    found = []
    for cmd, off, size in commands_:
        if cmd != LC_SEGMENT_64:
            continue
        nsects = struct.unpack_from('<I', raw, off + 64)[0]
        base = off + 72
        if base + nsects * 80 > off + size:
            raise SystemExit('segment %#x claims %d sections beyond its size' % (off, nsects))
        for k in range(nsects):
            at = base + k * 80
            sect = raw[at:at + 16].split(b'\0')[0].decode()
            seg = raw[at + 16:at + 32].split(b'\0')[0].decode()
            found.append((seg, sect, at))
    return found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    source, target = Path(sys.argv[1]), Path(sys.argv[2])

    raw = source.read_bytes()
    commands_ = commands(raw)
    buffer = bytearray(raw)

    # 1. Drop the initializer registration: a zero-size section emits nothing, and the bytes it
    #    used to describe stay where they are so no other section offset moves.
    dropped = []
    for seg, sect, at in sections(raw, commands_):
        if sect != SECTNAME:
            continue
        size = struct.unpack_from('<Q', raw, at + 40)[0]
        if size:
            struct.pack_into('<Q', buffer, at + 40, 0)          # size
            struct.pack_into('<I', buffer, at + 48, 0)          # offset
            struct.pack_into('<I', buffer, at + 56, 0)          # reloff
            struct.pack_into('<I', buffer, at + 60, 0)          # nreloc
            dropped.append((seg, sect, size))
    if not dropped:
        raise SystemExit('no non-empty %s section: nothing to strip' % SECTNAME)

    # 2. Export the initializer so the board can call it by name at the right moment.  A C++
    #    static-initializer function is emitted as a local text symbol, and a local symbol cannot
    #    satisfy an undefined reference from another object.
    symtab = next((off for cmd, off, _ in commands_ if cmd == LC_SYMTAB), None)
    if symtab is None:
        raise SystemExit('no LC_SYMTAB')
    symoff, nsyms, stroff = struct.unpack_from('<3I', raw, symtab + 8)
    exported = []
    for k in range(nsyms):
        at = symoff + k * 16
        strx = struct.unpack_from('<I', raw, at)[0]
        n_type = raw[at + 4]
        if n_type & N_TYPE != N_SECT or n_type & N_EXT:
            continue
        name = raw[stroff + strx:raw.index(b'\0', stroff + strx)].decode()
        if name.startswith('__GLOBAL__sub_I_'):
            buffer[at + 4] = n_type | N_EXT
            exported.append(name)
    if not exported:
        raise SystemExit('no local __GLOBAL__sub_I_ symbol to export')

    if len(buffer) != len(raw):
        raise SystemExit('the patches changed the file size')
    target.write_bytes(bytes(buffer))

    for seg, sect, size in dropped:
        print('stripped %s,%s (%d B)' % (seg, sect, size))
    for name in exported:
        print('exported %s' % name)


if __name__ == '__main__':
    main()
