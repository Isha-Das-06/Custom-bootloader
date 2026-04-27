#ifndef BOARD_H
#define BOARD_H

/*
 * Memory map for QEMU "virt" (32-bit, Cortex-A15, highmem=off).
 *
 *   0x00000000  pflash0  64 MiB  bootloader (execute-in-place, read-only)
 *   0x04000000  pflash1  64 MiB  boot metadata + firmware slots A/B
 *   0x09000000  PL011 UART0
 *   0x10000000  PCIe MMIO window   (i6300esb watchdog BAR0 is placed here)
 *   0x3f000000  PCIe ECAM
 *   0x40000000  RAM
 *
 * pflash1 layout (erase sector = 256 KiB):
 *   +0x000000  META0   metadata journal, bank 0
 *   +0x040000  META1   metadata journal, bank 1
 *   +0x100000  SLOT A  1 MiB (256-byte signed header + payload)
 *   +0x200000  SLOT B  1 MiB
 *
 * RAM layout:
 *   0x40000000 .. 0x400FEFFF  bootloader data/bss/stacks
 *   0x400FF000                boot_info handed from bootloader to app
 *   0x40100000 ..             application (copied here from its slot, run from RAM)
 */

#define UART0_BASE          0x09000000u

#define FLASH_BL_BASE       0x00000000u
#define FLASH_BASE          0x04000000u
#define FLASH_SIZE          0x04000000u
#define FLASH_SECTOR_SIZE   0x00040000u

#define META_BANK0_ADDR     (FLASH_BASE + 0x000000u)
#define META_BANK1_ADDR     (FLASH_BASE + 0x040000u)
#define SLOT_A_ADDR         (FLASH_BASE + 0x100000u)
#define SLOT_B_ADDR         (FLASH_BASE + 0x200000u)
#define SLOT_SIZE           0x00100000u

#define PCIE_ECAM_BASE      0x3f000000u
#define PCIE_MMIO_BASE      0x10000000u
#define WDT_BAR_ADDR        PCIE_MMIO_BASE

#define RAM_BASE            0x40000000u
#define BOOT_INFO_ADDR      0x400FF000u
#define APP_LOAD_ADDR       0x40100000u
#define APP_RAM_END         0x40800000u

/* Watchdog: each stage is ~N ms (1 kHz prescaler); reset fires after stage 2. */
#define WDT_STAGE_MS        1500u

/* Boot attempts a freshly installed image gets before it is rolled back. */
#define BOOT_MAX_TRIES      3u

#endif
