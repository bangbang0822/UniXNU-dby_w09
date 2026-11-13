#!/bin/sh
# Linux shim for the one macOS call the board scripts make: `brew --prefix llvm`.
# The UniXNU Linux toolchain ships its LLVM at /opt/unixnu-linux/llvm.
if [ "$1" = "--prefix" ]; then
    case "$2" in
        llvm) echo /opt/unixnu-linux/llvm ;;
        *)    echo /opt/unixnu-linux ;;
    esac
    exit 0
fi
echo "brew shim: unsupported arguments: $*" >&2
exit 1
