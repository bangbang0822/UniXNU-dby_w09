/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "A650GmuRegions.h"

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

A650RegionsStatus A650GmuBuildRegions(const uint8_t *image, size_t bytes,
    AdrenoFirmwareRegion *regions, size_t capacity, size_t *count)
{
	if (!count) {
		return A650RegionsInvalid;
	}
	*count = 0;
	if (!image || !regions || !capacity || bytes < 16) {
		return A650RegionsInvalid;
	}

	/* The type-1 run at the head of the image is the region declaration. Any other type ends it;
	 * a zero address with a zero length would be padding, which the format does not define, so it
	 * is treated as the end too. */
	AdrenoFirmwareRegion built[64];
	size_t n = 0, off = 0;
	while (off + 16 <= bytes) {
		uint32_t address = le32(image + off);
		uint32_t length = le32(image + off + 4);
		uint32_t type = le32(image + off + 8);
		uint32_t value = le32(image + off + 12);
		if (type != 1) {
			break;
		}
		if (n == sizeof(built) / sizeof(built[0])) {
			return A650RegionsInvalid;
		}
		if (!value) {
			return A650RegionsInvalid;
		}
		built[n].address = address;
		built[n].bytes = value;   /* the declared length is the `value` field */
		built[n].identifier = (uint32_t)n;
		n++;
		off += 16;
		if (length) {
			if (length > bytes - off) {
				return A650RegionsInvalid;
			}
			off += length;
		}
	}
	if (!n) {
		return A650RegionsInvalid;
	}
	if (n > capacity) {
		return A650RegionsTooMany;
	}

	/* Sort by address: the declarations are not guaranteed to arrive in order, and clipping only
	 * works on a sorted table. */
	for (size_t i = 0; i + 1 < n; i++) {
		for (size_t j = 0; j + 1 < n - i; j++) {
			if (built[j].address > built[j + 1].address) {
				AdrenoFirmwareRegion swap = built[j];
				built[j] = built[j + 1];
				built[j + 1] = swap;
			}
		}
	}

	/* Clip each region to end at the next one's start. A region that already ends at or before
	 * the next start is left alone. */
	for (size_t i = 0; i + 1 < n; i++) {
		uint64_t end = (uint64_t)built[i].address + built[i].bytes;
		if (end > built[i + 1].address) {
			uint64_t clipped = (uint64_t)built[i + 1].address - built[i].address;
			if (!clipped) {
				/* Two declarations at the same address cannot both be honoured. */
				return A650RegionsInvalid;
			}
			built[i].bytes = clipped;
		}
	}

	/* Identifiers must stay unique after the sort; renumber so they match the table order. */
	for (size_t i = 0; i < n; i++) {
		built[i].identifier = (uint32_t)i;
		regions[i] = built[i];
	}
	*count = n;
	return A650RegionsSuccess;
}
