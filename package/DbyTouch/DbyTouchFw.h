// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Novatek NT36523 THP firmware selection for the DBY-W09 panel.
 *
 * The image bytes are vendor data taken from this unit's stock package
 * (libafehalW2599Z<project>.so, listed in proprietary-files.txt) and therefore never
 * enter Git. They are extracted into .cache/firmware/novatek/ and turned into
 * build/dby-touch/DbyTouchFwImages.h by scripts/gen-touch-firmware-multi.py, which the
 * touch archive recipe runs before it compiles this package.
 *
 * What stays here is policy: the table layout, which project is selected by default, and
 * how a boot argument or the boot-page picker names one. What is generated is the bytes
 * and the table that points at them, including each image's own ILM/DLM length and CRC --
 * the uploader reads those from the selected image, never from a macro baked at build time.
 *
 * With no firmware present the generated table carries length 0 for every row. The upload
 * path bounds every write by that length, so the driver stops before the download stage and
 * logs the reason instead of programming the panel with bytes it made up.
 */
#ifndef DBY_W09_DBY_TOUCH_FW_H
#define DBY_W09_DBY_TOUCH_FW_H

#include <stdint.h>

/* Every NT36523 THP project image this driver can run is 0x30000 bytes. */
#define DBY_TOUCH_FW_LEN 0x30000U

struct dbyTouchFwSel {
	const char *project;      /* lcd-kit,default-project-id of the panel this image drives */
	const char *source;       /* where the bytes came from, for the log line */
	uint32_t len;             /* image length, always DBY_TOUCH_FW_LEN; 0 = no image present */
	const unsigned char *image;
	uint32_t ilm_off, ilm_sram, ilm_len, ilm_crc;
	uint32_t dlm_off, dlm_sram, dlm_len, dlm_crc;
};

/* Defines dbyTouchFwImage<project> for every image present and gDbyTouchFwAll[]. */
#include "DbyTouchFwImages.h"

#define DBY_TOUCH_FW_COUNT (sizeof gDbyTouchFwAll / sizeof gDbyTouchFwAll[0])

/* Selected image.  Default is the last entry, which is this board's own project; a boot
 * argument or the boot-page picker can change it before the uploader runs.  Nothing here
 * is const so the picker can repoint it, and no uploader constant is baked from a macro:
 * lengths and CRCs always come from the selected image's own header. */
static const struct dbyTouchFwSel *gDbyTouchFw = &gDbyTouchFwAll[DBY_TOUCH_FW_COUNT - 1];

/* Pick by project string, full id or the trailing four digits ("1100"/"1300"/"2700"), or
 * by 1-based index.  Returns 0 on success, -1 when nothing matched. */
static int dbyTouchFwSelect(const char *name)
{
	if (name == nullptr || *name == '\0') {
		return -1;
	}
	if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0') {
		unsigned idx = (unsigned)(name[0] - '1');
		if (idx >= DBY_TOUCH_FW_COUNT) {
			return -1;
		}
		gDbyTouchFw = &gDbyTouchFwAll[idx];
		return 0;
	}
	for (unsigned i = 0; i < DBY_TOUCH_FW_COUNT; i++) {
		const char *p = gDbyTouchFwAll[i].project;
		const char *q = name;
		if (q[0] == 'W' && q[1] == '2') {
			/* full id given */
		} else {
			/* short form: compare the trailing four digits */
			p = p + 6;   /* "W2599Z" is six characters; compare "1100" etc. */
		}
		while (*p && *q && *p == *q) { p++; q++; }
		if (*p == '\0' && *q == '\0') {
			gDbyTouchFw = &gDbyTouchFwAll[i];
			return 0;
		}
	}
	return -1;
}

#endif /* DBY_W09_DBY_TOUCH_FW_H */
