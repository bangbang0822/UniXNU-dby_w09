"""Mach-O side of the accelerator recipe: strip __DATA,__mod_init_func and globalize the owned
initializer without llvm-objcopy --globalize-symbol (not supported for Mach-O).

The reference recipe used llvm-objcopy twice; on this host that flag exists only for ELF, so the
same two edits are made directly:
  * every section named __mod_init_func in __DATA is RENAMED to __mod_init_fcn, so the
    linker registers no initializer for this object while the section stays
    self-consistent (size, offset and relocation still describe bytes that exist);
  * the N_EXT bit of the symbol __GLOBAL__sub_I_NativeAdrenoAccelerator.cpp is set, so the board
    can call it by name.
Nothing else in the object is touched, and the caller still compares __text before and after."""
import struct
import sys
from pathlib import Path

INIT = b'__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp'
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2


def commands(raw):
    ncmds = struct.unpack_from('<I', raw, 16)[0]
    off, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', raw, off)
        out.append((cmd, off, size))
        off += size
    return out


def strip_and_globalize(src: Path, dst: Path):
    raw = bytearray(src.read_bytes())
    if struct.unpack_from('<I', raw, 0)[0] != 0xfeedfacf:
        sys.exit('%s is not a 64-bit Mach-O' % src)

    stripped = 0
    for cmd, off, size in commands(bytes(raw)):
        if cmd != LC_SEGMENT_64:
            continue
        nsects = struct.unpack_from('<I', raw, off + 64)[0]
        so = off + 72
        for _ in range(nsects):
            # section_64 layout: sectname[16], segname[16], addr, size, offset, ...
            sect = bytes(raw[so:so + 16]).rstrip(b'\0')
            if sect == b'__mod_init_func':
                # Rename, do not shrink: a zero-size section keeps its relocation, and ld64
                # would then fix up the first 8 bytes of whatever follows - the very
                # auto-registration this strip prevents. The board's Linux recipes rename it
                # (build-gop-legacy-display-linux.py:62, device-verified); the name keeps its
                # length so nothing else moves.
                # Exactly 16 bytes: '__mod_init_func' is 15 characters plus the terminator.
                raw[so:so + 16] = b'__mod_init_fcn' + b'\0\0'
                stripped += 1
            so += 80
    if not stripped:
        sys.exit('no __mod_init_func section found: the class would auto-register at kext load')

    symtab = next((off for cmd, off, _ in commands(bytes(raw)) if cmd == LC_SYMTAB), None)
    if symtab is None:
        sys.exit('no LC_SYMTAB')
    symoff, nsyms, stroff, _strsize = struct.unpack_from('<4I', raw, symtab + 8)
    globalized = 0
    for k in range(nsyms):
        base = symoff + k * 16
        strx, ntype, nsect, ndesc, nvalue = struct.unpack_from('<IBBHQ', raw, base)
        end = bytes(raw).index(b'\0', stroff + strx)
        name = bytes(raw[stroff + strx:end])
        if name == INIT:
            struct.pack_into('<B', raw, base + 4, ntype | 0x01)   # N_EXT
            globalized += 1
    if not globalized:
        sys.exit('the owned initializer symbol is not in the object')

    if len(raw) != len(src.read_bytes()):
        sys.exit('the object changed length: a partial write would corrupt every offset')
    dst.write_bytes(bytes(raw))
    return stripped, globalized


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: macho-late.py <in.o> <out.o>')
    n, g = strip_and_globalize(Path(sys.argv[1]), Path(sys.argv[2]))
    print('stripped %d mod_init section(s), globalized %d symbol(s)' % (n, g))
