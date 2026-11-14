/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* DbyCamera 阶段 2 的门面：把 SM8250 的相机域按级开起来并把每一步读到什么报出来。结构照母本 PaganiCamera 的
 * PaganiCamera.cpp，落到本板的写法照本仓已经验收的 DbyTorch.cpp（窗口、线程、启动参数、一行一条 kprintf）。
 * 可移植的阶梯在 DbyCamBringup.c，本文件只做映射、ARC 投票和日志。
 *
 * 启动参数：**不新增任何一条**。命令行已经 50 条 998 字节，payload.py:35 的门是 1000，
 * 所以本模块默认就走满阶（4：cam_vio/开关轨、MCLK0 19.2 MHz（XO，PLL 未开：DbyCamBringup.c:393-400 只写 DBCAM_RCG_CFG_XO）、放开复位、CCI 初始化，再对
 * 0x10 / 0x20 / 0x34 各发一笔读 ID 的交易）。dby-cam 只是留给「往下调」的旁门：
 *   dby-cam=1 勘察；2 开 GCC 再只读 CAMCC；3 投 ARC、开 CAMCC 域、读 CCI_HW_VERSION；
 *   4 及以上按 4 走；显式传 0 则整段不动（不映射、不投、不写一个寄存器、不起线程）。
 * 不传这个参数不等于关——缺席即 4。
 *
 * 一次跑完就结束：这不是一个服务，没有请求面，也没有人再调它。跑完之后相机域保持上电（母本的理由：测试级不值得
 * 为省电去赌第一次 collapse）。任何一步失败都停在那里并写下负步号，之后的寄存器一个都不碰。 */
#ifndef DBY_CAMERA_H
#define DBY_CAMERA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 显示和输入起来之后由 BootProgress 的一次性钩子调用：读参数、起一个线程就返回，boot 不等总线。 */
void dby_camera_start(void);

/* [0] 状态（0 关或没起来，1 在跑，2 跑完），[1] 步号（失败是负的），[2] 错误码，[3] 参数要的级，
 * [4] 实际走的级，[5] 闸门拒绝的写次数，[6] CCI_HW_VERSION，[7] titan GDSCR，[8] ARC mm 档，[9] ARC mx 档，
 * [10] RSC id，[11] cmd-db 里两条 ARC 资源各找到没有（bit 0 mmcx，bit 1 mxc），[12] cam_vio 的 cmd-db 地址，
 * [13] cam_vio 投票的返回码，[14] 扫到应答的地址位图（bit n = kDbcamScanAddr[n]），[15] 第一个地址的读回值。 */
extern volatile int32_t dby_camera_status[16];

#ifdef __cplusplus
}
#endif
#endif
