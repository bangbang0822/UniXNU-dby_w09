// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// 阶段 1a 的内核读数：DbyCameraProvider.m 在 mediaserverd 里走到哪一步，从外面只看得到这一条路。mediaserverd 是
// mobile 用户、带沙盒，写不了 kern.pagani_diag_line（只收 root）和 root 的 spool，os_log 这台板子又读不出来，
// 所以测试图的状态需要一个只有它能打开的服务收一行文本，写成 "[cam] ios: ..."。
//
// 母本 refer/PaganiDrivers/drivers/PaganiCamera/PaganiCameraIos.cpp（105 行）逐条照搬，只有三处随本板改，每处都写在
// 这里，不留进编译日志：
//  1. 行数落点是 kprintf，不是 boot_progress_trace + IOLog 两遍：本板的 COM4 只镜像 kprintf 行，不镜像 IOLog 行
//     （DbyBattery.cpp:77-86，和 DbyTorchIos.cpp 同一判断）。
//  2. 计数数组：母本用 pagani_camera_status[9]/[10]，那是阶段 2 相机驱动的状态数组，本板没有移植那个驱动，所以
//     本文件自带 dby_camera_ios_status[2]，含义是母本那两个字的原义（收下的行、超过上限被拒的行）。
//  3. 文本 builder 用手电筒的 DbyTorchText.h：母本的 CamUtil.h 没移植，dbt_text_printable 是从它的
//     pcam_text_printable（CamUtil.h:71）逐字搬过来的那个函数。
//
// 类初始化由 scripts/build-dby-torch.py 从 __DATA,__mod_init_func 摘掉并全局化成 dby_camera_ios_static_init，
// 没有 dby-ti=1 的内核里没有这个类也没有这个服务；发布点是 zze-dby-torch-media-mount.patch 在 kern_exec.c 里的那个
// weak import（和手电筒那半同一个门、同一个停靠点），母本的调用点
// 在 PaganiCamera.cpp:242（同一道 pagani-torch-ios=1 门，只是那里由阶段 2 驱动持有）。NativeSandboxGrantProcess
// 让沙盒放行（zzw-native-adreno-sandbox-grant）。每次启动最多 DbcLogLines 行，失控的进程写不满日志环。不 panic。
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <mach/kmod.h>
extern "C" {
#include "DbyTorchText.h"
#include "ios/DbyCameraProvider.h"
void proc_selfname(char *buf, int size);
extern kmod_info_t g_kernel_kmod_info;
void dby_camera_ios_static_init(void) __asm__("__GLOBAL__sub_I_DbyCameraIos.cpp");
}

// 母本的 pagani_camera_status[9]/[10]，见开头第 2 条：那里由阶段 2 驱动 PaganiCamera.cpp 定义，本板没有那个
// 驱动，所以这个文件自己定义这两个计数（scripts/build-dby-camera.py 的 REQUIRED 就是查它在本归档里有定义）。
extern "C" volatile int32_t dby_camera_ios_status[2] = {0, 0};

namespace {
enum {DbcLogLines = 48};
}

class DbyCameraLog : public IOService {
    OSDeclareDefaultStructors(DbyCameraLog)
};
OSDefineMetaClassAndStructors(DbyCameraLog, IOService)

class DbyCameraLogClient : public IOUserClient {
    OSDeclareDefaultStructors(DbyCameraLogClient)
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        char name[17];
        dbt_zero(name, sizeof name);
        proc_selfname(name, sizeof name);
        static const char only[] = DBC_PROCESS;
        for (unsigned i = 0; i < sizeof only; ++i)
            if (name[i] != only[i]) return false;
        return IOUserClient::initWithTask(task, security, type, properties);
    }
    bool start(IOService *provider) override {
        return OSDynamicCast(DbyCameraLog, provider) && IOUserClient::start(provider);
    }
    IOReturn clientClose() override {
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
#pragma unused(dispatch, target, reference)
        if (selector != DBC_LOG_SELECTOR || !args->structureInput || !args->structureInputSize || args->structureInputSize > DbcLogMax)
            return kIOReturnBadArgument;
        if (__atomic_fetch_add(&dby_camera_ios_status[0], 1, __ATOMIC_RELAXED) >= DbcLogLines) {
            __atomic_fetch_add(&dby_camera_ios_status[1], 1, __ATOMIC_RELAXED);
            return kIOReturnNoSpace;
        }
        char line[160];
        DbtText t;
        dbt_text_start(&t, line, sizeof line);
        dbt_text_str(&t, "[cam] ios: ");
        dbt_text_printable(&t, static_cast<const uint8_t *>(args->structureInput), args->structureInputSize);
        kprintf("%s\n", line);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(DbyCameraLogClient, IOUserClient)

namespace {
volatile uint32_t gCameraPublished;
bool registerCameraClasses()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    dby_camera_ios_static_init();
    return OSMetaClass::postModLoad(handle) == kIOReturnSuccess;
}
}  // namespace

extern "C" void dby_camera_ios_publish(void)
{
    if (__atomic_exchange_n(&gCameraPublished, 1u, __ATOMIC_ACQ_REL)) return;
    if (!registerCameraClasses()) {
        kprintf("[cam] ios: classes not registered\n");
        return;
    }
    IOService *home = IOService::getResourceService();
    DbyCameraLog *service = OSTypeAlloc(DbyCameraLog);
    if (!service || !home || !service->init() || !service->attach(home)) {
        if (service) service->release();
        kprintf("[cam] ios: service not attached\n");
        return;
    }
    service->setProperty("NativeSandboxGrantProcess", DBC_PROCESS);
    service->setProperty("IOUserClientClass", "DbyCameraLogClient");
    if (!service->start(home)) {
        service->detach(home);
        service->release();
        kprintf("[cam] ios: service not started\n");
        return;
    }
    service->registerService();
    service->release();   // 资源服务的挂接留着它，到重启为止
    kprintf("[cam] ios: log service published\n");
}
