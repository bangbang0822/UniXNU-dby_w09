"""Rebuild 05-源码/src/board-stage179.toml from board.toml + the shipped image's command line.

The image's argument list is the ground truth: it is what the tablet was actually told. This script
copies board.toml and replaces only the `kernel_args = [ ... ]` array with exactly those arguments,
so the stage179 file reproduces the shipped image and the current board.toml stays the newest config.
"""
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
board = (root / '05-源码/src/board.toml').read_text(encoding='utf-8')
image = json.loads(subprocess.run(
    [sys.executable, str(root / '06-工具/read-payload-args.py'),
     str(root / '03-payload/payload-stage179-touch-gpu.img'), '--json'],
    capture_output=True, text=True, check=True).stdout)
args = image['args']

lines = board.split('\n')
start = next(i for i, line in enumerate(lines) if line.startswith('kernel_args = ['))
# The array closes on the first line that is nothing but a closing bracket. Counting brackets is not
# reliable here: the block is full of prose comments that mention them.
end = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == ']')

header = [
    'kernel_args = [',
    '  # ---------------------------------------------------------------------------',
    '  # 这份文件是 03-payload/payload-stage179-touch-gpu.img 逐字节对应的命令行。',
    '  # 它由 06-工具/read-payload-args.py 从镜像里读出来（偏移 0x%x），不是手抄。' % image['offset'],
    '  # 要用更新的配置（带背光 dby-bl / 音频 dby-audio / 电池 dby-battery / 手电筒 dby-torch',
    '  # / 命令行控制台 qcom-usb-debug，且 offline-legacy-unit-scale=2）就用 board.toml；',
    '  # 要 1:1 复现随包镜像就用这份。两者的差异见 00-先读我.md §5。',
    '  # ---------------------------------------------------------------------------',
]
body = ['  "%s",' % arg for arg in args]
tail = ['  # 以上 %d 条，拼接 %d 字节（上限 1000，余 %d）。' % (
    len(args), image['joined_bytes'], 1000 - image['joined_bytes']), ']']
new = lines[:start] + header + body + tail + lines[end + 1:]
out = root / '05-源码/src/board-stage179.toml'
out.write_text('\n'.join(new), encoding='utf-8')
print(f'wrote {out}: {len(args)} args, {image["joined_bytes"]} bytes')
