/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuConfig.h"
#include <IOKit/IOLib.h>
/* The five payload sizes used to be restated here as their own enum. They have one definition --
 * NativeAdrenoGmuDefaultSizes() in NativeAdrenoGmuMemory.cpp, which is what the allocator actually reserves
 * -- so this file reads them from there instead of carrying a second copy that could drift. */
static bool matches(const NativeAdrenoMemory &memory,uint64_t bytes,uint64_t iova,
    ShengGmuMemory *out)
{
    if(!memory.mapping() || memory.bytes()!=size_t(bytes) || memory.spanCount()!=1)
        return false;
    out->cpu=static_cast<volatile uint32_t *>(memory.mapping());
    out->bytes=size_t(bytes);
    out->iova=iova;
    return true;
}
IOReturn NativeAdrenoAssembleGmuConfig(const NativeAdrenoFirmwareView *firmware,
    const NativeAdrenoGmuAllocator *allocator,uint32_t chip_id,unsigned poll_budget,
    ShengGmuConfig *config)
{
    if(!config)return kIOReturnBadArgument;
    *config={};
    if(!firmware || !firmware->bytes || !firmware->length || !allocator ||
        !allocator->ready() || !chip_id || !poll_budget || poll_budget>1024)
        return kIOReturnBadArgument;
    const NativeAdrenoGmuMemoryLayout &layout=allocator->layout();
    const NativeAdrenoGmuSizes sizes=NativeAdrenoGmuDefaultSizes();
    ShengGmuConfig built{};
    if(!matches(allocator->icache(),sizes.icache,layout.icache_iova,&built.icache) ||
        !matches(allocator->dummy(),sizes.dummy,layout.dummy_iova,&built.dummy) ||
        !matches(allocator->debug(),sizes.debug,layout.debug_iova,&built.debug) ||
        !matches(allocator->log(),sizes.log,layout.log_iova,&built.log) ||
        !matches(allocator->hfi(),sizes.hfi,layout.hfi_iova,&built.hfi))
        return kIOReturnBadArgument;
    built.chip_id=chip_id;
    built.firmware=firmware->bytes;
    built.firmware_bytes=firmware->length;
    built.poll_budget=poll_budget;
    *config=built;
    return kIOReturnSuccess;
}
