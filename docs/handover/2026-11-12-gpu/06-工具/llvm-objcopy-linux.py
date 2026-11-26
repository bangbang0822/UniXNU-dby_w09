#!/usr/bin/env python3
"""Linux shim for llvm-objcopy.

Ubuntu's llvm-objcopy refuses `--remove-section` and `--globalize-symbol` for Mach-O input, while the
board's macOS-facing recipes use exactly that pair to stop a C++ static initialiser from running in a
late-bound object. This shim performs the same byte-level edit (rename the __mod_init_func section so
the initialiser is not registered, mark the symbol external so it is still kept) and delegates every
other invocation to the real tool.
"""
import os, struct, subprocess, sys

REAL = '/usr/lib/llvm-18/bin/llvm-objcopy'


def patch(path_in, path_out, symbol):
    data = bytearray(open(path_in, 'rb').read())
    if struct.unpack_from('<I', data, 0)[0] not in (0xfeedfacf, 0xfeedface):
        raise SystemExit('shim: not a Mach-O file')
    ncmds = struct.unpack_from('<I', data, 16)[0]
    offset, renamed = 32, False
    symtab = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:                                  # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            at = offset + 72
            for _s in range(nsects):
                if bytes(data[at:at + 16]).rstrip(b'\0') == b'__mod_init_func':
                    data[at:at + 16] = b'__mod_init_fcn\0\0'
                    renamed = True
                at += 80
        elif cmd == 0x2:                                 # LC_SYMTAB
            symtab = struct.unpack_from('<IIII', data, offset + 8)
        offset += size
    if not renamed:
        raise SystemExit('shim: no __mod_init_func section')
    if symtab is None:
        raise SystemExit('shim: no LC_SYMTAB')
    symoff, nsyms, stroff, _ = symtab
    for index in range(nsyms):
        base = symoff + index * 16
        n_strx, n_type = struct.unpack_from('<IB', data, base)
        start = stroff + n_strx
        end = data.index(b'\0', start)
        if bytes(data[start:end]).decode('utf-8', 'replace') == symbol:
            data[base + 4] = n_type | 0x01               # N_EXT
            open(path_out, 'wb').write(bytes(data))
            sys.stderr.write('shim: patched %s (Mach-O)\n' % os.path.basename(path_in))
            return
    raise SystemExit('shim: symbol %s not found' % symbol)


def main(argv):
    remove = [a for a in argv if a.startswith('--remove-section=')]
    glob = [a for a in argv if a.startswith('--globalize-symbol=')]
    positional = [a for a in argv if not a.startswith('-')]
    if len(remove) == 1 and len(glob) == 1 and len(positional) == 2:
        patch(positional[0], positional[1], glob[0].split('=', 1)[1])
        return 0
    return subprocess.call([REAL] + argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
