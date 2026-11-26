import json, pathlib, subprocess, sys, tomllib

root = pathlib.Path(sys.argv[1])
board = tomllib.loads((root / '05-源码/src/board.toml').read_text(encoding='utf-8'))
board_args = board['payload']['kernel_args']
img = json.loads(subprocess.run(
    [sys.executable, str(root / '06-工具/read-payload-args.py'),
     str(root / '03-payload/payload-stage179-touch-gpu.img'), '--json'],
    capture_output=True, text=True, check=True).stdout)
img_args = img['args']

print(f'board.toml : {len(board_args)} args, {len(" ".join(board_args))} bytes')
print(f'payload img: {len(img_args)} args, {img["joined_bytes"]} bytes')
print()
print('only in board.toml :', [a for a in board_args if a not in img_args])
print('only in payload img:', [a for a in img_args if a not in board_args])
print()
common = [a for a in board_args if a in img_args]
print('common:', len(common))
print('order identical for common prefix:', board_args[:len(common)] == img_args[:len(common)])
