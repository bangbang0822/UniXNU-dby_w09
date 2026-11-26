# UEFI patches for this board

The images this port boots are built from the firmware tree (`WQSQW/uefi-dby_w09`), not from this
repository, so these are the changes to that tree that this port depends on, kept here because they
are part of the port's behaviour even though they compile elsewhere.

## darwinboot-addon-handover.patch

The GPU addon reaches the kernel through DarwinBoot: `unixnu build payload --gpu-addon` writes
`kernel.macho.gpuaddon` beside the kernel and declares a 16-byte `/chosen/memory-map/GpuAddon` node in
the ADT, and only the loader can fill that node in. The board's `DarwinBootPkg/Driver/DarwinBoot.c`
predates the feature, so on 2026-11-17 the tablet booted a payload that carried a 13 MB addon and the
boot row still read `A files=-1 mount=1/2` with `[metal askers] mediaserverd:1 backboardd:1
SpringBoard:1` — the clients were asking, the kernel had nothing to install.

The patch ports the handover from `UniXNU/DarwinBoot` at the core's pin `5243c68`: `AdtNode`
recognises and propagates the node, the loader reads and validates the payload file, and the copy
stores its address and size into the tree the way the trust cache is stored. It does not touch the
board's physical-framebuffer code (`DBY_PHYFB_*`).

Apply it with the firmware tree as the working directory:

```sh
git -C <firmware tree> apply <this repository>/patches/uefi/darwinboot-addon-handover.patch
```

and build with the **DEBUG** target — `Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc` selects
`BaseDebugLibSerialPort`/`FrameBufferSerialPortLib` only under `!if $(TARGET) == DEBUG`, while
`Silicon/Silicium/SiliciumPkg/Sec/Sec.c:184` calls `BaseDebugLibSerialPortConstructor`
unconditionally, so a RELEASE build fails at the Sec link with
`undefined symbol: BaseDebugLibSerialPortConstructor`. The image this board runs is a DEBUG build.

```sh
stuart_build -c Platforms/Huawei/dby_w09Pkg/DeviceBuild.py --SKIPPREBUILD \
    TARGET=DEBUG ENABLE_SECUREBOOT=1 \
    FD_BASE=0xce000000 FD_SIZE=0x800000 FD_BLOCKS=0x800 DEVICE_MODEL=dby_w09
```

`ENABLE_SECUREBOOT` and `DEVICE_MODEL` carry the values `board.toml` `[firmware.defines]` gives them;
the two `FD_` values are the ones `Resources/Configs/dby_w09.toml` `[uefi_fd]` declares, with
`FD_BLOCKS` its size divided by 4096.

`--SKIPPREBUILD` skips the DebugMacroCheck plugin, which cannot resolve a package for this platform
because `Platforms/Huawei/dby_w09Pkg` carries no `.dec`. Pack the result into the boot image with
`BootShim.bin` in front of the FD and the `[boot_image]` parameters of
`Resources/Configs/dby_w09.toml`, then `fastboot flash boot`.
