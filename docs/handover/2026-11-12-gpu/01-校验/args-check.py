"""Print the resolved kernel_args of a board.toml and the joined command-line size.

Usage: python args-check.py <board.toml>

The DarwinBoot command line has a hard 1000-byte cap (scripts/unixnu/payload.py:33-36), and the
joined string is what the cap measures: each argument plus one separator space.
"""
import pathlib
import sys
import tomllib

CAP = 1000

path = pathlib.Path(sys.argv[1])
data = tomllib.loads(path.read_text(encoding='utf-8'))
# kernel_args lives under [payload] in this board's schema; the older [kernel] spelling is kept as a
# fallback so the script also reads pre-schema board files.
args = (data.get('payload') or {}).get('kernel_args')
if args is None:
    args = (data.get('kernel') or {}).get('kernel_args', data.get('kernel_args', []))
joined = ' '.join(args)
print(f'{path}')
print(f'arguments: {len(args)}')
print(f'joined bytes: {len(joined)} / {CAP}  (headroom {CAP - len(joined)})')
if len(joined) > CAP:
    print(f'OVER BY {len(joined) - CAP} bytes: payload packing will refuse this board')
for i, arg in enumerate(args, 1):
    print(f'{i:3d}  {len(arg):4d}  {arg}')
