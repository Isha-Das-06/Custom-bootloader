#include "flash.h"
#include "board.h"
#include "util.h"

#define CMD_READ_ARRAY   0xFFu
#define CMD_READ_STATUS  0x70u
#define CMD_CLEAR_STATUS 0x50u
#define CMD_ERASE_SETUP  0x20u
#define CMD_ERASE_CONFIRM 0xD0u
#define CMD_PROGRAM      0x40u

#define SR_READY         0x80u
#define SR_ERASE_ERR     0x20u
#define SR_PROG_ERR      0x10u

static int in_range(uint32_t addr, size_t len)
{
    return addr >= FLASH_BASE && len <= FLASH_SIZE && addr - FLASH_BASE <= FLASH_SIZE - len;
}

static uint32_t wait_ready(uint32_t addr)
{
    uint32_t sr;
    REG32(addr) = CMD_READ_STATUS;
    do {
        sr = REG32(addr);
    } while (!(sr & SR_READY));
    return sr;
}

static void back_to_array(uint32_t addr)
{
    REG32(addr) = CMD_CLEAR_STATUS;
    REG32(addr) = CMD_READ_ARRAY;
}

int flash_erase_sector(uint32_t addr)
{
    if (addr & (FLASH_SECTOR_SIZE - 1))
        return FLASH_ERR_ALIGN;
    if (!in_range(addr, FLASH_SECTOR_SIZE))
        return FLASH_ERR_RANGE;

    REG32(addr) = CMD_CLEAR_STATUS;
    REG32(addr) = CMD_ERASE_SETUP;
    REG32(addr) = CMD_ERASE_CONFIRM;
    uint32_t sr = wait_ready(addr);
    back_to_array(addr);
    if (sr & SR_ERASE_ERR)
        return FLASH_ERR_HW;
    return flash_is_erased(addr, FLASH_SECTOR_SIZE) ? FLASH_OK : FLASH_ERR_VERIFY;
}

int flash_program(uint32_t addr, const void *data, size_t len)
{
    if ((addr | len | (uintptr_t)data) & 3)
        return FLASH_ERR_ALIGN;
    if (!in_range(addr, len))
        return FLASH_ERR_RANGE;

    const uint32_t *src = data;
    int rc = FLASH_OK;
    for (size_t i = 0; i < len / 4; i++) {
        uint32_t a = addr + 4 * i;
        REG32(a) = CMD_PROGRAM;
        REG32(a) = src[i];
        uint32_t sr = wait_ready(a);
        if (sr & SR_PROG_ERR) {
            rc = FLASH_ERR_HW;
            break;
        }
    }
    back_to_array(addr);
    if (rc == FLASH_OK && memcmp((const void *)(uintptr_t)addr, data, len) != 0)
        rc = FLASH_ERR_VERIFY;
    return rc;
}

int flash_is_erased(uint32_t addr, size_t len)
{
    for (size_t i = 0; i < len; i += 4)
        if (REG32(addr + i) != 0xFFFFFFFFu)
            return 0;
    return 1;
}
