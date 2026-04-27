#ifndef FLASH_H
#define FLASH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Intel/Sharp CFI command-set driver for the NOR flash on pflash1.
 * Same programming model as real parallel NOR: erase a whole sector to 0xFF,
 * then program 32-bit words, polling the status register after each op.
 *
 * NOTE: while a command is in flight the array stops being readable, so
 * nothing may execute from pflash1. The bootloader runs from pflash0 and
 * the application runs from RAM, which is why this is safe.
 */

int flash_erase_sector(uint32_t addr);
int flash_program(uint32_t addr, const void *data, size_t len);   /* addr, len 4-byte aligned */
int flash_is_erased(uint32_t addr, size_t len);

#define FLASH_OK        0
#define FLASH_ERR_ALIGN -1
#define FLASH_ERR_RANGE -2
#define FLASH_ERR_HW    -3
#define FLASH_ERR_VERIFY -4

#endif
