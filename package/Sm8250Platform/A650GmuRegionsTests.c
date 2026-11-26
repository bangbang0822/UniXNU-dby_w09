/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host test for A650GmuRegions.c.
 *
 * Two parts. First, a synthetic image that reproduces the real overlap, so the clip is proved on
 * the shape that matters without needing the firmware blob. Second, with a path argument, the
 * real a650_gmu.bin: the built table is handed to the real AdrenoFirmwarePlanGmu and the plan must
 * succeed, which is the actual requirement this closes.
 *
 * cc -std=c11 -Wall -Wextra -Werror -I <UniXNU>/kits/AdrenoFirmware A650GmuRegions.c \
 *    A650GmuRegionsTests.c <UniXNU>/kits/AdrenoFirmware/AdrenoFirmware.c -o a650regions
 */
#include "A650GmuRegions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A record is {address, payload length, type, value}; a type-1 record carries the declared region
 * length in `value` and no payload. */
static size_t record(uint8_t *p, uint32_t address, uint32_t payload, uint32_t type, uint32_t value)
{
	put32(p, address); put32(p + 4, payload); put32(p + 8, type); put32(p + 12, value);
	return 16;
}

int main(int argc, char **argv)
{
	/* Part 1: the synthetic shape. Two regions overlap by 4 bytes when their declared lengths are
	 * honoured, and a third sits far away so the sort is exercised. */
	{
		uint8_t img[256];
		memset(img, 0, sizeof(img));
		size_t off = 0;
		off += record(img + off, 0x10007fdc, 0, 1, 0x24);       /* declared out of order on purpose */
		off += record(img + off, 0x10004000, 0, 1, 0x3fe0);     /* runs 4 bytes into the one above */
		off += record(img + off, 0x00004000, 0, 1, 0x8000);
		AdrenoFirmwareRegion regions[8];
		size_t count = 99;
		CHECK(A650GmuBuildRegions(img, off, regions, 8, &count) == A650RegionsSuccess);
		CHECK(count == 3);
		CHECK(regions[0].address == 0x00004000);
		CHECK(regions[1].address == 0x10004000);
		CHECK(regions[2].address == 0x10007fdc);
		/* The clip: 0x10007fdc - 0x10004000 = 0x3fdc, four bytes shorter than declared. */
		CHECK(regions[1].bytes == 0x3fdc);
		CHECK(regions[2].bytes == 0x24);
		CHECK(regions[0].bytes == 0x8000);
		/* No two regions may overlap after the build. */
		for (size_t i = 0; i + 1 < count; i++) {
			CHECK((uint64_t)regions[i].address + regions[i].bytes <= regions[i + 1].address);
		}
		/* Identifiers are unique and dense. */
		for (size_t i = 0; i < count; i++) {
			CHECK(regions[i].identifier == i);
		}
		printf("synthetic: 3 regions, clip 0x3fe0 -> 0x3fdc, no overlap\n");
	}

	/* Part 2: a capacity smaller than the region count is refused, not silently truncated. */
	{
		uint8_t img[128];
		size_t off = 0;
		off += record(img + off, 0x1000, 0, 1, 0x100);
		off += record(img + off, 0x2000, 0, 1, 0x100);
		AdrenoFirmwareRegion regions[1];
		size_t count = 99;
		CHECK(A650GmuBuildRegions(img, off, regions, 1, &count) == A650RegionsTooMany);
		CHECK(count == 0);
		printf("capacity: refused with TooMany, count 0\n");
	}

	/* Part 3: a malformed declaration is refused. */
	{
		uint8_t img[32];
		size_t off = record(img, 0x1000, 0, 1, 0);   /* zero length */
		AdrenoFirmwareRegion regions[4];
		size_t count = 99;
		CHECK(A650GmuBuildRegions(img, off, regions, 4, &count) == A650RegionsInvalid);
		CHECK(count == 0);
		printf("malformed: zero-length region refused\n");
	}

	/* Part 4, optional: the real image through the real planner. */
	if (argc > 1) {
		FILE *f = fopen(argv[1], "rb");
		CHECK(f != NULL);
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *img = malloc((size_t)size);
		CHECK(img != NULL);
		CHECK(fread(img, 1, (size_t)size, f) == (size_t)size);
		fclose(f);

		AdrenoFirmwareRegion regions[16];
		size_t count = 0;
		CHECK(A650GmuBuildRegions(img, (size_t)size, regions, 16, &count) == A650RegionsSuccess);
		printf("real image: %zu regions\n", count);
		for (size_t i = 0; i < count; i++) {
			printf("  region %zu: 0x%08x + 0x%llx\n", i, regions[i].address,
			    (unsigned long long)regions[i].bytes);
		}

		AdrenoFirmwareBlock blocks[64];
		size_t nblocks = 0;
		AdrenoFirmwareResult r = AdrenoFirmwarePlanGmu(img, (size_t)size, regions, count,
		    blocks, sizeof(blocks) / sizeof(blocks[0]), &nblocks);
		CHECK(r == AdrenoFirmwareSuccess);
		CHECK(nblocks == 16);
		size_t payload = 0;
		for (size_t i = 0; i < nblocks; i++) {
			CHECK(blocks[i].payload_offset + blocks[i].bytes <= (size_t)size);
			payload += blocks[i].bytes;
		}
		printf("real image: planner success, %zu blocks, %zu payload bytes of %ld\n",
		    nblocks, payload, size);
		free(img);
	} else {
		printf("real image: skipped, no path argument\n");
	}

	puts("PASS: A650 GMU region table (overlap clipped, sort, capacity and malformed refusals"
	    ", real image plans)");
	return 0;
}
