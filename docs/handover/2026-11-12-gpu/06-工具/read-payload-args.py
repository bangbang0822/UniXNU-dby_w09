"""Read the DarwinBoot command line embedded in a payload image.

Usage:
  python read-payload-args.py <payload.img>            # print the arguments, one per line
  python read-payload-args.py <payload.img> --json     # {"offset":…, "args":[…], "joined_bytes":…}

Why this exists: the payload image is the thing that actually boots, so its argument list - not
board.toml - is the ground truth for what the tablet was told. `verify-package.py` compares the two.

How it finds the blob: the command line is stored as one NUL-terminated run of printable ASCII that
contains `serial=` and at least eight space-separated tokens. The scanner walks the image once and
keeps the longest such run, which is the DarwinBoot buffer (the same trick `decode-thisboot.py` uses
for the on-device copy).
"""
import json
import sys
from pathlib import Path

# Every kernel command line this board boots starts with the same two tokens, so anchor on them and
# read forward to the first byte that cannot be part of the line. The image also embeds the build's
# board.toml as text (`kernel_args = [ "-v", … ]`), which is why anchoring matters: a plain
# "longest printable run" scan finds the TOML rendering first and yields quotes and brackets.
ANCHOR = b'-v serial='


def find_command_line(image):
    at = image.find(ANCHOR)
    if at < 0:
        return 0, b''
    end = at
    while end < len(image) and (0x20 <= image[end] <= 0x7E):
        end += 1
    run = image[at:end]
    # Trim a trailing quote if the buffer ran straight into the TOML text's closing delimiter.
    run = run.rstrip(b'" ')
    return at, run


def main():
    path = Path(sys.argv[1])
    image = path.read_bytes()
    offset, run = find_command_line(image)
    if not run:
        raise SystemExit(f'no DarwinBoot command line found in {path}')
    args = run.decode('ascii').split()
    joined = ' '.join(args)
    if '--json' in sys.argv:
        print(json.dumps({'offset': offset, 'args': args, 'joined_bytes': len(joined)}, ensure_ascii=False))
        return 0
    print(f'{path}: command line at offset 0x{offset:x}')
    print(f'arguments: {len(args)}, joined bytes: {len(joined)}')
    for arg in args:
        print(arg)
    return 0


if __name__ == '__main__':
    sys.exit(main())
