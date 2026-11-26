# 复刻构建脚本（参考版）
#
# 用法：把下面四个路径改成你自己的，然后在 WSL/Ubuntu 里执行：
#     bash reproduce-build.sh
#
# 前提：UniXNU core 与本板仓库已经 clone 到本地；iPadOS 17.7.11 (21H461) 的 Restore.ipsw 在位。
# 说明：这不是"一键脚本"——每个工程的 --out / IPSW / trust bundle / data seed 路径都不一样，
#       所以这里只固定**步骤与顺序**，路径留给你填。

set -euo pipefail

WS=/mnt/c/Users/HW/Documents/DSH2ZONE          # 工作区根
CORE="$WS/src/UniXNU"                          # UniXNU core
DEV="$WS/src/UniXNU-dby_w09"                   # 本板仓库
UEFI="$WS/src/uefi-dby_w09"                    # UEFI 固件仓库
OUT=/root/unixnu-out                           # 构建输出根
IPSW=/root/dby/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw
TRUST=/root/unixnu-out/trust-bundle/original-static-system.bundle
SEED=/path/to/data-final.dbr                    # Data 种子（不入库，自己准备）

export PYTHONUTF8=1
export UNIXNU_IPSW="$IPSW"
cd "$CORE"

# 注意 1：下面用的是**本板**的 defconfig 名（dby_w09_defconfig）与**本板**的 kernel_args。
#        换板时改成你自己的 defconfig，并**不要**沿用包里 02-配置片段/board.toml-kernel-args-全文.txt
#        （46 条里只有 4 条触摸参数可移植，其余会改显示/存储/复位）。
# 注意 2：整段脚本会重建内核；同一工作区若有别的会话在构建，先看 work/COORDINATION.md。

echo "== 1/5 生成配置（改过 defconfig 必须重跑，内核读的是生成的 .config）=="
./unixnu defconfig dby_w09_defconfig -D "$DEV" --uefi "$UEFI" --out "$OUT"

echo "== 2/5 组装树（把 kits/ scripts/ drivers/ 物化出来）=="
./unixnu tree -D "$DEV" --uefi "$UEFI" --out "$OUT"

TREE="$OUT/dby_w09/ipados-17.7.11-21H461/dby_w09/tree"
echo "   tree = $TREE"
ls "$TREE/kits/dby_w09" | head -5

echo "== 3/5 内核第一遍（会打印 Build the kernel again to link the bridge.）=="
./unixnu build kernel -j 16 -D "$DEV" --uefi "$UEFI" --out "$OUT"

echo "== 4/5 内核第二遍（真正的 PMGR 桥）=="
./unixnu build kernel -j 16 -D "$DEV" --uefi "$UEFI" --out "$OUT"

echo "   触摸归档："
ls -l "$TREE/build/dby-touch/libDbyTouch.a"
llvm-nm -gP "$TREE/build/dby-touch/libDbyTouch.a" | grep -E "native_touch_(seq|xs|ys|set_sink)" | head   # 不要吞掉错误：llvm-nm 缺失必须看得见

echo "== 5/5 payload =="
./unixnu build payload -D "$DEV" --uefi "$UEFI" --edk2 "$UEFI/Mu_Basecore" \
    --unique-chip-id 0x1 --trust-cache "$TRUST" --data-ramdisk "$SEED" --out "$OUT"

echo "   产物：$OUT/dby_w09/ipados-17.7.11-21H461/dby_w09/payload/payload.img"
ls -l "$OUT/dby_w09/ipados-17.7.11-21H461/dby_w09/payload/payload.img"
sha256sum "$OUT/dby_w09/ipados-17.7.11-21H461/dby_w09/payload/payload.img"

echo "== 刷机 =="
echo "   fastboot flash preload <payload.img> && fastboot continue"
echo "   （只有 continue 会真正启动；reboot 会回到 fastboot）"
