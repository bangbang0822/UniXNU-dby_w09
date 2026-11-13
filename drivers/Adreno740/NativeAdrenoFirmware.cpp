/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoSha256.h"
#include <string.h>
IOReturn NativeAdrenoFirmware::validate(const uint8_t *image,uint64_t length,
    const NativeAdrenoFirmwareRules *rules,NativeAdrenoFirmwareView *view)
{
    if(!view)return kIOReturnBadArgument;
    *view={};
    /* `length > SIZE_MAX` used to be tested here as well. size_t is 64 bits on this target and length is a
     * uint64_t, so that comparison is never true and the cast below cannot truncate; what bounds the image is
     * the rule's own byte count, which is checked first. */
    if(!rules || !image || !length || length!=rules->bytes)return kIOReturnBadArgument;
    uint8_t digest[32];
    NativeAdrenoSha256::hash(image,size_t(length),digest);
    if(memcmp(digest,rules->sha256,32))return kIOReturnIOError;
    view->bytes=image;view->length=size_t(length);
    memcpy(view->sha256,digest,32);
    return kIOReturnSuccess;
}
