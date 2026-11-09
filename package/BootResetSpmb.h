/* SPMI PMIC arbiter reboot-reason writer.  Implemented in DbyTouch.cpp (it needs IOKit to map the
 * four arbiter windows) and called from the C reset provider. */
#ifndef DBY_BOOT_RESET_SPMB_H
#define DBY_BOOT_RESET_SPMB_H

#include <stdint.h>

extern int dby_spmi_set_bootloader(char *out, unsigned capacity, uint8_t *before, uint8_t *after,
                                   unsigned *detail);

#endif /* DBY_BOOT_RESET_SPMB_H */
