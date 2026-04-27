#ifndef WDT_H
#define WDT_H

#include <stdint.h>

/*
 * Intel 6300ESB watchdog (a PCI function on QEMU's PCIe root bus).
 * Two-stage: stage 1 expiry would raise an IRQ, stage 2 expiry resets the
 * machine. Once enabled with the LOCK bit set it cannot be disabled until
 * the next reset -- the application can only keep feeding it.
 */

int wdt_probe(void);                         /* bootloader: discover on PCI, map BAR0 */
void wdt_start(uint32_t stage_ms, int lock); /* bootloader: arm it just before handing off */
void wdt_kick(void);                         /* app: feed it */
int wdt_caused_last_reset(void);             /* "previous reboot" flag */
void wdt_clear_reset_flag(void);

#endif
