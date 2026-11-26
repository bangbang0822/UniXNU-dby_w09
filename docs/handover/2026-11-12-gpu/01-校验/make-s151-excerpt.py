"""Cut the s151 panel-page excerpt down to the pages that actually carry the judgement rows.

Usage: python make-s151-excerpt.py <decoded-all.txt> <out.txt>

Every row the handover docs quote must be present in the excerpt, otherwise "see the evidence" is
just a claim. The script finds each quoted row, maps its line to the slot (page) that contains it,
and writes the header plus those pages, unmodified.
"""
import re
import sys
from pathlib import Path

# Rows quoted in 00-先读我.md, 03-payload/说明 and 07-证据/gpu-实测摘要.md.
NEEDLES = [
    '[gpu zap] st=0 step=30',
    '[gpu latch] irq=100000',
    '[gop] arg=1 step=7',
    '[metal gpu7] backboard s34 e0 v4/4 t27bd241c7 a256/0',
    '[metal gpu7] PF 1/0 r0 v1/0 t38 a38',
    '[metal gpu7] PF 6/0 r0 v4/0 t30 a31',
    '[fb] scanout=9c000000 1600x2560 rb=6400 gop=9c000000 match=1',
    'TX c=2 1068,2505 w=6634 n=8 hid=22096/1',
    'TOUCH HID started=1 drv=1 key-reports=0 touch-reports=116 res=0',
    '[touch edge] installed=1',
    'SWAP  calls=21 accepted=21 rejected=0',
    '[scanout prepare]',
]

source = Path(sys.argv[1])
target = Path(sys.argv[2])
lines = source.read_text(encoding='utf-8', errors='replace').split('\n')

# Slot boundaries: a slot starts at its "--- slot N  r=N ---" marker and ends before the next one.
markers = [(i, int(re.search(r'slot (\d+)', line).group(1)))
           for i, line in enumerate(lines) if line.startswith('--- slot ')]
bounds = {}
for k, (index, slot) in enumerate(markers):
    end = markers[k + 1][0] - 1 if k + 1 < len(markers) else len(lines) - 1
    bounds[slot] = (index, end)


def slot_of(line_number):
    for slot, (start, end) in bounds.items():
        if start <= line_number <= end:
            return slot
    return None


wanted = {}
missing = []
for needle in NEEDLES:
    hit = next((i for i, line in enumerate(lines) if needle in line), None)
    if hit is None:
        missing.append(needle)
        continue
    slot = slot_of(hit)
    wanted.setdefault(slot, []).append(needle)

out = [
    '# s151 原始面板页摘录 —— 取自 evidence/edl-log-s151/decoded-all.txt，未改一个字。',
    f'# 原文件 {len(lines)} 行 / {len(markers)} 页（rounds=444，uptime_s=894，约 15 分钟）。',
    '# 下面保留的是**判定行所在的那几页**，不是抽样：文档里引用的每一行都能在这里找到。',
    '# 抓取方式：EDL 读 userdata 尾部 8192 块（first_lba=30684672），再用 06-工具/decode-thisboot.py 解码。',
    '',
]
out.extend(lines[0:markers[0][0]])
for slot in sorted(wanted):
    start, end = bounds[slot]
    out.append('')
    out.extend(lines[start:end + 1])

target.write_text('\n'.join(out) + '\n', encoding='utf-8')
print(f'pages kept: {sorted(wanted)} -> {target} ({len(out)} lines, {target.stat().st_size} B)')
for slot in sorted(wanted):
    print(f'  slot {slot}: {wanted[slot]}')
if missing:
    print('NOT FOUND (fix the docs, they quote rows that do not exist):')
    for needle in missing:
        print(f'  {needle}')
    sys.exit(1)
