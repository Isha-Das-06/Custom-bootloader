#include "wdt.h"
#include "board.h"
#include "util.h"

#define PCI_VENDOR_INTEL    0x8086u
#define PCI_DEVICE_6300ESB  0x25abu

#define PCI_COMMAND         0x04
#define PCI_BAR0            0x10
#define PCI_CMD_MEM         0x0002u

#define ESB_CONFIG_REG      0x60    /* 16-bit */
#define ESB_LOCK_REG        0x68    /* 8-bit  */
#define ESB_TIMER1_REG      0x00
#define ESB_TIMER2_REG      0x04
#define ESB_RELOAD_REG      0x0c

#define ESB_WDT_FUNC        (1u << 2)
#define ESB_WDT_ENABLE      (1u << 1)
#define ESB_WDT_LOCK        (1u << 0)
#define ESB_WDT_RELOAD      (1u << 8)
#define ESB_WDT_TIMEOUT     (1u << 9)   /* previous-reboot flag, write 1 to clear */
#define ESB_UNLOCK1         0x80u
#define ESB_UNLOCK2         0x86u

static uint32_t ecam_fn(unsigned dev)
{
    return PCIE_ECAM_BASE + (dev << 15);   /* bus 0, function 0 */
}

static int wdt_find(void)
{
    for (unsigned dev = 0; dev < 32; dev++) {
        uint32_t id = REG32(ecam_fn(dev));
        if ((id & 0xffff) == PCI_VENDOR_INTEL && (id >> 16) == PCI_DEVICE_6300ESB)
            return (int)dev;
    }
    return -1;
}

static void unlock(void)
{
    REG8(WDT_BAR_ADDR + ESB_RELOAD_REG) = ESB_UNLOCK1;
    REG8(WDT_BAR_ADDR + ESB_RELOAD_REG) = ESB_UNLOCK2;
}

static uint32_t wdt_cfg;

int wdt_probe(void)
{
    int dev = wdt_find();
    if (dev < 0)
        return -1;
    wdt_cfg = ecam_fn((unsigned)dev);

    /* Map BAR0 into the PCIe MMIO window and enable memory decoding. */
    REG32(wdt_cfg + PCI_BAR0) = WDT_BAR_ADDR;
    REG16(wdt_cfg + PCI_COMMAND) = REG16(wdt_cfg + PCI_COMMAND) | PCI_CMD_MEM;
    return dev;
}

void wdt_start(uint32_t stage_ms, int lock)
{
    uint32_t cfg = wdt_cfg;
    if (!cfg)
        return;

    /* 1 kHz prescaler, reboot on stage-2 expiry, stage-1 "interrupt" disabled. */
    REG16(cfg + ESB_CONFIG_REG) = 0x0003;

    /* One tick at 1 kHz scale is 2^15 * 30 ns ~= 0.983 ms. */
    uint32_t ticks = (stage_ms * 1000u) / 983u;
    if (ticks > 0xfffff)
        ticks = 0xfffff;
    unlock();
    REG32(WDT_BAR_ADDR + ESB_TIMER1_REG) = ticks;
    unlock();
    REG32(WDT_BAR_ADDR + ESB_TIMER2_REG) = ticks;

    wdt_kick();
    REG8(cfg + ESB_LOCK_REG) = (uint8_t)(ESB_WDT_ENABLE | (lock ? ESB_WDT_LOCK : 0));
}

void wdt_kick(void)
{
    unlock();
    REG16(WDT_BAR_ADDR + ESB_RELOAD_REG) = ESB_WDT_RELOAD;
}

int wdt_caused_last_reset(void)
{
    return (REG16(WDT_BAR_ADDR + ESB_RELOAD_REG) & ESB_WDT_TIMEOUT) != 0;
}

void wdt_clear_reset_flag(void)
{
    unlock();
    REG16(WDT_BAR_ADDR + ESB_RELOAD_REG) = ESB_WDT_TIMEOUT;
}
