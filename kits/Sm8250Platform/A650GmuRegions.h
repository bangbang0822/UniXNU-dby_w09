/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Build the region table a650_gmu.bin needs before AdrenoFirmwarePlanGmu will accept it.
 *
 * The image opens with a run of type-1 records whose `value` field is a region length. Two of
 * those declared lengths overlap by 4 bytes: 0x10004000 + 0x3fe0 = 0x10007fe0 runs past the next
 * region's 0x10007fdc. AdrenoFirmwarePlanGmu refuses overlapping regions, so the table has to be
 * built rather than taken verbatim. The payload blocks themselves do not overlap
 * (0x10004000 + 0xec0 = 0x10004ec0, far below 0x10007fdc), so the declared length is wider than
 * the data it carries and clipping it removes the conflict without touching a byte of firmware.
 *
 * This is what the A650 GMU cold start needs on alioth; PLAN.md records the overlap as an open
 * blocker, and this closes it. */
#ifndef A650_GMU_REGIONS_H
#define A650_GMU_REGIONS_H
#include <stddef.h>
#include <stdint.h>
#include "AdrenoFirmware.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	A650RegionsSuccess,
	A650RegionsInvalid,      /* malformed image, or more regions than the caller allows */
	A650RegionsTooMany,      /* the caller's array is smaller than the declared region count */
} A650RegionsStatus;

/* Read the type-1 region declarations from the head of `image`, sort them by address, and clip
 * each one to end at the next one's start. Writes at most `capacity` entries and reports how many
 * were written in `*count`. On any failure `*count` is zero and `regions` is untouched. */
A650RegionsStatus A650GmuBuildRegions(const uint8_t *image, size_t bytes,
    AdrenoFirmwareRegion *regions, size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
#endif
