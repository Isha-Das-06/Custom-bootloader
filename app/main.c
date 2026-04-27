/*
 * Demo application image.
 *
 * On a TRIAL boot it runs a self-test and, if that passes, confirms itself
 * (marks its slot active in the boot metadata). Until that happens the
 * bootloader treats it as unproven and will roll it back if it doesn't
 * confirm within BOOT_MAX_TRIES boots.
 *
 * Build-time fault injection (APP_FAULT) produces deliberately broken
 * releases so rollback can be demonstrated:
 *   FAULT_NONE      healthy image
 *   FAULT_HANG      hangs before confirming  -> watchdog resets -> rollback
 *   FAULT_CRASH     executes an undefined instruction -> exception -> rollback
 *   FAULT_SELFTEST  self-test fails, app requests rollback itself
 *
 * Console commands: help, status, update, reboot, hang, crash
 */
#include "board.h"
#include "flash.h"
#include "image.h"
#include "meta.h"
#include "util.h"
#include "wdt.h"

#define FAULT_NONE     0
#define FAULT_HANG     1
#define FAULT_CRASH    2
#define FAULT_SELFTEST 3

#ifndef APP_FAULT
#define APP_FAULT FAULT_NONE
#endif
#ifndef APP_TAG
#define APP_TAG "release"
#endif

static boot_info_t info;

#define OTA_MAGIC    0x2141544fu   /* "OTA!" */
#define OTA_CHUNK    4096u
#define OTA_TIMEOUT_MS 5000u

static uint8_t ota_buf[OTA_CHUNK] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */

static void print_meta(void)
{
    boot_meta_t m;
    char v[16];
    if (meta_load(&m) != 0) {
        kprintf("  metadata: none\n");
        return;
    }
    version_str(m.min_version, v);
    kprintf("  metadata: seq=%u active=%c pending=%c tries_left=%u floor=v%s\n",
            m.seq, slot_name(m.active), slot_name(m.pending), m.tries_left, v);
    kprintf("            boots=%u rollbacks=%u last_event=\"%s\"",
            m.boot_count, m.rollback_count, meta_event_str(m.last_event));
    if (m.last_event_slot != SLOT_NONE)
        kprintf(" (slot %c)", slot_name(m.last_event_slot));
    if (m.last_event == EV_REJECTED || m.last_event == EV_ACTIVE_FAILOVER)
        kprintf(" reason=\"%s\"", img_status_str(m.last_event_detail));
    kprintf("\n");
}

static void print_slot(unsigned slot)
{
    const image_header_t *h = (const image_header_t *)(uintptr_t)slot_addr(slot);
    char v[16];
    kprintf("  slot %c @%p: ", slot_name(slot), slot_addr(slot));
    if (flash_is_erased(slot_addr(slot), 64)) {
        kprintf("empty\n");
        return;
    }
    if (h->magic != IMAGE_MAGIC) {
        kprintf("garbage (no image magic)\n");
        return;
    }
    version_str(h->version, v);
    kprintf("v%s, %u bytes, key ", v, h->image_size);
    hexdump_short(h->key_id, 4);
    kprintf("%s\n", slot == info.slot ? "   <== running" : "");
}

static void cmd_status(void)
{
    char v[16];
    version_str(info.version, v);
    kprintf("  running: v%s (%s) from slot %c, %s boot\n", v, APP_TAG,
            slot_name(info.slot), info.trial ? "TRIAL" : "confirmed");
    print_meta();
    print_slot(SLOT_A);
    print_slot(SLOT_B);
}

/* ---- confirm / reject ---- */

static int confirm_self(void)
{
    boot_meta_t m;
    if (meta_load(&m) != 0)
        return -1;
    if (m.pending != info.slot)
        return -1;
    m.active = (uint8_t)info.slot;
    m.pending = SLOT_NONE;
    m.tries_left = 0;
    if (info.version > m.min_version)
        m.min_version = info.version;   /* anti-rollback floor only ever moves up */
    m.last_event = EV_CONFIRMED;
    m.last_event_slot = (uint8_t)info.slot;
    m.last_event_detail = 0;
    int rc = meta_save(&m);
    if (rc == 0)
        info.trial = 0;
    return rc;
}

static void reject_self(void)
{
    boot_meta_t m;
    if (meta_load(&m) == 0 && m.pending == info.slot) {
        m.pending = SLOT_NONE;
        m.rollback_count++;
        m.last_event = EV_APP_REJECTED;
        m.last_event_slot = (uint8_t)info.slot;
        meta_save(&m);
    }
}

/* ---- self-test ---- */

static int self_test(void)
{
    boot_meta_t m;
    kprintf("  self-test: boot info ........ %s\n", info.magic == BOOT_INFO_MAGIC ? "ok" : "FAIL");
    if (info.magic != BOOT_INFO_MAGIC)
        return -1;
    int meta_ok = meta_load(&m) == 0 && m.pending == info.slot;
    kprintf("  self-test: boot metadata .... %s\n", meta_ok ? "ok" : "FAIL");
    if (!meta_ok)
        return -1;
    wdt_kick();
    kprintf("  self-test: watchdog feed .... ok\n");

#if APP_FAULT == FAULT_SELFTEST
    kprintf("  self-test: sensor subsystem . FAIL (simulated regression in this build)\n");
    return -1;
#else
    kprintf("  self-test: sensor subsystem . ok\n");
    return 0;
#endif
}

/* ---- OTA receive ---- */

static int rx_byte(void)
{
    uint32_t start = timer_ms();
    for (;;) {
        int c = uart_getc_nonblock();
        if (c >= 0)
            return c;
        wdt_kick();
        if (timer_ms() - start > OTA_TIMEOUT_MS)
            return -1;
    }
}

static int rx_u32(uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int c = rx_byte();
        if (c < 0)
            return -1;
        v |= (uint32_t)c << (8 * i);
    }
    *out = v;
    return 0;
}

static int flush_chunk(uint32_t addr, uint32_t len)
{
    while (len & 3)
        ota_buf[len++] = 0xff;
    if ((addr & (FLASH_SECTOR_SIZE - 1)) == 0) {
        int rc = flash_erase_sector(addr);
        if (rc)
            return rc;
    }
    return flash_program(addr, ota_buf, len);
}

/*
 * Wire format from host:  "OTA!" | u32 length | <length bytes> | u32 crc32
 * Target is always the slot we are NOT running from, so the known-good
 * image is never touched. Nothing is marked bootable until the whole
 * image has landed and its CRC checks out; a power cut mid-transfer
 * just leaves a half-written inactive slot that nobody will boot.
 */
static void cmd_update(void)
{
    if (info.trial) {
        kprintf("OTA: refused -- current image is still on trial; it must confirm first\n");
        return;
    }
    unsigned target = info.slot ^ 1u;
    uint32_t base = slot_addr(target);
    uint32_t magic, len, crc_host;

    kprintf("OTA: target slot %c, max %u bytes\n", slot_name(target), SLOT_SIZE);
    kprintf("OTA: READY\n");

    if (rx_u32(&magic) || magic != OTA_MAGIC) {
        kprintf("OTA: ERROR bad/missing preamble\n");
        return;
    }
    if (rx_u32(&len) || len < IMAGE_HEADER_SIZE || len > SLOT_SIZE) {
        kprintf("OTA: ERROR bad length\n");
        return;
    }

    uint32_t crc = 0, got = 0, fill = 0;
    while (got < len) {
        int c = rx_byte();
        if (c < 0) {
            kprintf("OTA: ERROR timeout after %u/%u bytes (slot %c left unbootable)\n",
                    got, len, slot_name(target));
            return;
        }
        ota_buf[fill++] = (uint8_t)c;
        got++;
        if (fill == OTA_CHUNK || got == len) {
            crc = crc32(crc, ota_buf, fill);
            int rc = flush_chunk(base + got - fill, fill);
            if (rc) {
                kprintf("OTA: ERROR flash write failed (%d)\n", rc);
                return;
            }
            fill = 0;
            kprintf("OTA: %u/%u written\n", got, len);
        }
    }
    /* make sure any stale data from an older, larger image can't linger in trailing sectors */
    uint32_t used_end = (len + 3u) & ~3u;
    uint32_t next_sector = (used_end + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    for (uint32_t off = next_sector; off < SLOT_SIZE; off += FLASH_SECTOR_SIZE)
        if (!flash_is_erased(base + off, FLASH_SECTOR_SIZE))
            flash_erase_sector(base + off);

    if (rx_u32(&crc_host) || crc_host != crc) {
        kprintf("OTA: ERROR transport CRC mismatch (host %08x, got %08x)\n", crc_host, crc);
        return;
    }
    if (crc32(0, (const void *)(uintptr_t)base, len) != crc) {
        kprintf("OTA: ERROR flash readback mismatch\n");
        return;
    }

    const image_header_t *h = (const image_header_t *)(uintptr_t)base;
    char v[16];
    version_str(h->version, v);
    kprintf("OTA: received %u bytes, CRC ok, header says v%s\n", len, v);
    kprintf("OTA: (signature is NOT checked here -- the bootloader is the only authority)\n");

    boot_meta_t m;
    if (meta_load(&m) != 0)
        meta_defaults(&m);
    m.pending = (uint8_t)target;
    m.tries_left = BOOT_MAX_TRIES;
    m.last_event = EV_UPDATE_STAGED;
    m.last_event_slot = (uint8_t)target;
    m.last_event_detail = 0;
    if (meta_save(&m) != 0) {
        kprintf("OTA: ERROR metadata write failed\n");
        return;
    }
    kprintf("OTA: slot %c marked PENDING (%u tries). rebooting...\n", slot_name(target), BOOT_MAX_TRIES);
    system_reset();
}

/* ---- console ---- */

static int read_line(char *buf, int max)
{
    int n = 0;
    for (;;) {
        wdt_kick();
        int c = uart_getc_nonblock();
        if (c < 0)
            continue;
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            buf[n] = 0;
            return n;
        }
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            uart_puts("\b \b");
            continue;
        }
        if (c >= 32 && c < 127 && n < max - 1) {
            buf[n++] = (char)c;
            uart_putc((char)c);
        }
    }
}

static void __attribute__((noinline)) do_crash(void)
{
    __asm__ volatile(".word 0xe7f000f0");   /* permanently undefined (UDF) */
}

static void do_hang(void)
{
    for (;;)
        ;   /* spin without feeding the watchdog */
}

void app_main(const boot_info_t *bi)
{
    char v[16];
    uart_init();

    if (bi && bi->magic == BOOT_INFO_MAGIC)
        info = *bi;
    version_str(info.version, v);

    kprintf("\n+------------------------------------------------+\n");
    kprintf("|  APPLICATION v%-8s  [%-8s]  slot %c     |\n", v, APP_TAG, slot_name(info.slot));
    kprintf("+------------------------------------------------+\n");
    if (info.wdt_reset)
        kprintf("  (note: last reset was caused by the watchdog)\n");

    if (info.trial) {
        kprintf("  TRIAL boot -- %u attempt(s) left after this one. running self-test\n", info.tries_left);
#if APP_FAULT == FAULT_HANG
        kprintf("  [bug] this build deadlocks during init... (watchdog will bite)\n");
        do_hang();
#elif APP_FAULT == FAULT_CRASH
        kprintf("  [bug] this build dereferences garbage during init...\n");
        do_crash();
#endif
        if (self_test() != 0) {
            kprintf("  self-test FAILED -> rejecting this image, requesting rollback\n");
            reject_self();
            system_reset();
        }
        if (confirm_self() != 0) {
            kprintf("  ERROR: could not confirm image\n");
        } else {
            kprintf("  CONFIRMED: slot %c is now the active image; rollback floor = v%s\n",
                    slot_name(info.slot), v);
        }
    } else {
        boot_meta_t m;
        if (meta_load(&m) == 0 && m.last_event != EV_NONE && m.last_event != EV_CONFIRMED) {
            kprintf("  last boot-control event: %s", meta_event_str(m.last_event));
            if (m.last_event_slot != SLOT_NONE)
                kprintf(" (slot %c)", slot_name(m.last_event_slot));
            if (m.last_event == EV_REJECTED || m.last_event == EV_ACTIVE_FAILOVER)
                kprintf(": %s", img_status_str(m.last_event_detail));
            kprintf("\n");
        }
    }

    kprintf("  type 'help' for commands\n");
    char line[64];
    for (;;) {
        kprintf("app> ");
        read_line(line, sizeof(line));
        if (!strcmp(line, "help")) {
            kprintf("  status  - show slots and boot metadata\n");
            kprintf("  update  - receive a signed image over UART into the inactive slot\n");
            kprintf("  reboot  - reset the board\n");
            kprintf("  hang    - stop feeding the watchdog\n");
            kprintf("  crash   - execute an undefined instruction\n");
        } else if (!strcmp(line, "status")) {
            cmd_status();
        } else if (!strcmp(line, "update")) {
            cmd_update();
        } else if (!strcmp(line, "reboot")) {
            system_reset();
        } else if (!strcmp(line, "hang")) {
            kprintf("  hanging; watchdog should reset us in ~%u ms\n", 2 * WDT_STAGE_MS);
            do_hang();
        } else if (!strcmp(line, "crash")) {
            do_crash();
        } else if (line[0]) {
            kprintf("  unknown command '%s'\n", line);
        }
    }
}
