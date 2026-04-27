/*
 * A/B bootloader with signed images and automatic rollback.
 *
 * Boot decision (every reset):
 *
 *   1. Load boot metadata (journaled, CRC-protected) from flash.
 *   2. If a slot is PENDING (freshly installed, not yet confirmed):
 *        - tries_left == 0  -> it had its chances and never confirmed: ROLL BACK.
 *        - otherwise verify it; if valid, burn one try and boot it as a TRIAL.
 *          If invalid, discard it immediately (never executed).
 *   3. Else boot the ACTIVE (confirmed) slot after verifying it.
 *   4. If the active slot is invalid (bit rot, interrupted write...), fail over
 *      to the other slot if *it* verifies. If nothing verifies: recovery halt.
 *
 * Verification: header is copied to RAM, its Ed25519 signature is checked
 * against the public key compiled into this ROM, bounds/version are checked,
 * then the payload is copied to its RAM load address and the SHA-512 of the
 * RAM copy is compared with the signed hash. We execute exactly the bytes we
 * hashed, so there is no time-of-check/time-of-use window on the flash.
 *
 * Just before jumping, the hardware watchdog is armed and locked. The app must
 * feed it; a hung trial image is reset by the watchdog and eventually rolled back.
 */
#include "board.h"
#include "ed25519.h"
#include "flash.h"
#include "image.h"
#include "meta.h"
#include "sha512.h"
#include "util.h"
#include "wdt.h"

#include "pubkey.h"   /* generated: signing_pubkey[32], signing_key_id[8] */

#define BL_VERSION 0x010000u

#define LOG(...) kprintf("[BL] " __VA_ARGS__)

static boot_info_t *const boot_info = (boot_info_t *)BOOT_INFO_ADDR;

/* ------------------------------------------------------------------ */

static int verify_slot(unsigned slot, uint32_t min_version, image_header_t *hdr, int quiet)
{
    const uint32_t base = slot_addr(slot);
    char vbuf[16];

    memcpy(hdr, (const void *)(uintptr_t)base, sizeof(*hdr));

    if (flash_is_erased(base, sizeof(*hdr)))
        return IMG_EMPTY;
    if (hdr->magic != IMAGE_MAGIC)
        return IMG_BAD_MAGIC;

    /* Authenticate the header before trusting any field in it. */
    uint32_t t0 = timer_ms();
    if (ed25519_verify(hdr->signature, hdr, IMAGE_SIGNED_BYTES, signing_pubkey) != 0)
        return IMG_BAD_SIGNATURE;
    uint32_t t1 = timer_ms();

    if (hdr->header_size != IMAGE_HEADER_SIZE ||
        hdr->image_size == 0 || hdr->image_size > SLOT_SIZE - IMAGE_HEADER_SIZE ||
        hdr->load_addr < APP_LOAD_ADDR || hdr->load_addr >= APP_RAM_END ||
        hdr->image_size > APP_RAM_END - hdr->load_addr ||
        hdr->entry_addr < hdr->load_addr || hdr->entry_addr >= hdr->load_addr + hdr->image_size ||
        (hdr->load_addr & 3) || (hdr->entry_addr & 3))
        return IMG_BAD_HEADER;

    if (hdr->version < min_version)
        return IMG_ROLLBACK;

    /* Copy to the execution address, then hash what we will actually run. */
    uint8_t digest[64];
    memcpy((void *)(uintptr_t)hdr->load_addr,
           (const void *)(uintptr_t)(base + IMAGE_HEADER_SIZE), hdr->image_size);
    sha512((const void *)(uintptr_t)hdr->load_addr, hdr->image_size, digest);
    uint32_t t2 = timer_ms();

    uint8_t diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= digest[i] ^ hdr->payload_sha512[i];
    if (diff)
        return IMG_BAD_HASH;

    if (!quiet) {
        version_str(hdr->version, vbuf);
        LOG("  slot %c: v%s, %u bytes, Ed25519 ok (%u ms), SHA-512 ok (%u ms)\n",
            slot_name(slot), vbuf, hdr->image_size, t1 - t0, t2 - t1);
    }
    return IMG_OK;
}

static void save_meta(boot_meta_t *m)
{
    int rc = meta_save(m);
    if (rc)
        LOG("WARNING: metadata write failed (%d)\n", rc);
}

static void set_event(boot_meta_t *m, unsigned ev, unsigned slot, unsigned detail)
{
    m->last_event = (uint8_t)ev;
    m->last_event_slot = (uint8_t)slot;
    m->last_event_detail = (uint8_t)detail;
}

extern void jump_to_app(uint32_t entry, void *info) __attribute__((noreturn));

static void __attribute__((noreturn))
boot(const image_header_t *hdr, unsigned slot, const boot_meta_t *m, int trial, int wdt_bite)
{
    boot_info->magic = BOOT_INFO_MAGIC;
    boot_info->slot = slot;
    boot_info->trial = (uint32_t)trial;
    boot_info->tries_left = m->tries_left;
    boot_info->version = hdr->version;
    boot_info->boot_count = m->boot_count;
    boot_info->wdt_reset = (uint32_t)wdt_bite;
    boot_info->bl_version = BL_VERSION;

    wdt_start(WDT_STAGE_MS, 1);
    LOG("watchdog armed (%u ms, locked). jumping to %p\n\n", 2 * WDT_STAGE_MS, hdr->entry_addr);
    jump_to_app(hdr->entry_addr, boot_info);
}

static void __attribute__((noreturn)) recovery_halt(void)
{
    LOG("\n");
    LOG("!!! NO BOOTABLE IMAGE -- entering recovery (halted) !!!\n");
    LOG("    reflash a signed image into slot A or B and reset.\n");
    for (;;)
        __asm__ volatile("wfi");
}

void bl_main(void)
{
    boot_meta_t m;
    image_header_t hdr;
    char vbuf[16];

    uart_init();
    kprintf("\n");
    LOG("==================================================\n");
    LOG(" A/B secure bootloader v1.0.0   (QEMU virt, A15)\n");
    LOG(" key id ");
    hexdump_short(signing_key_id, 8);
    kprintf("   max tries %u\n", BOOT_MAX_TRIES);
    LOG("==================================================\n");

    int wdt_bite = 0;
    if (wdt_probe() < 0) {
        LOG("WARNING: i6300esb watchdog not found (run QEMU with -device i6300esb)\n");
    } else {
        wdt_bite = wdt_caused_last_reset();
        wdt_clear_reset_flag();
        if (wdt_bite)
            LOG("*** previous reset was a WATCHDOG TIMEOUT ***\n");
    }

    if (meta_load(&m) != 0) {
        LOG("no valid boot metadata: first boot, initialising (active = A)\n");
        meta_defaults(&m);
        save_meta(&m);
    }
    m.boot_count++;

    version_str(m.min_version, vbuf);
    LOG("meta: seq=%u active=%c pending=%c tries_left=%u floor=v%s boots=%u rollbacks=%u\n",
        m.seq, slot_name(m.active), slot_name(m.pending), m.tries_left, vbuf,
        m.boot_count, m.rollback_count);

    /* ---- 1. pending (trial) image ---- */
    if (m.pending != SLOT_NONE) {
        unsigned p = m.pending;
        if (m.tries_left == 0) {
            LOG("slot %c was never confirmed after %u boot attempts\n", slot_name(p), BOOT_MAX_TRIES);
            LOG(">>> ROLLING BACK to slot %c <<<\n", slot_name(m.active));
            m.pending = SLOT_NONE;
            m.rollback_count++;
            set_event(&m, EV_ROLLBACK_TRIES, p, 0);
            save_meta(&m);
        } else {
            LOG("verifying PENDING slot %c ...\n", slot_name(p));
            int st = verify_slot(p, m.min_version, &hdr, 0);
            if (st == IMG_OK) {
                m.tries_left--;
                set_event(&m, EV_TRIAL_BOOT, p, 0);
                save_meta(&m);
                LOG("TRIAL boot of slot %c (attempt %u of %u)\n",
                    slot_name(p), BOOT_MAX_TRIES - m.tries_left, BOOT_MAX_TRIES);
                boot(&hdr, p, &m, 1, wdt_bite);
            }
            LOG("slot %c REJECTED: %s -> discarding update, it will not be run\n",
                slot_name(p), img_status_str(st));
            m.pending = SLOT_NONE;
            set_event(&m, EV_REJECTED, p, (unsigned)st);
            save_meta(&m);
        }
    }

    /* ---- 2. active (confirmed) image ---- */
    LOG("verifying ACTIVE slot %c ...\n", slot_name(m.active));
    int st = verify_slot(m.active, m.min_version, &hdr, 0);
    if (st == IMG_OK) {
        if (m.last_event == EV_TRIAL_BOOT || m.last_event == EV_CONFIRMED)
            set_event(&m, EV_NONE, SLOT_NONE, 0);
        save_meta(&m);   /* persists boot_count */
        boot(&hdr, m.active, &m, 0, wdt_bite);
    }

    /* ---- 3. failover ---- */
    unsigned other = m.active ^ 1u;
    LOG("ACTIVE slot %c INVALID: %s\n", slot_name(m.active), img_status_str(st));
    LOG("trying fallback slot %c ...\n", slot_name(other));
    int st2 = verify_slot(other, m.min_version, &hdr, 0);
    if (st2 == IMG_OK) {
        LOG(">>> FAILOVER: slot %c becomes active <<<\n", slot_name(other));
        set_event(&m, EV_ACTIVE_FAILOVER, m.active, (unsigned)st);
        m.active = (uint8_t)other;
        save_meta(&m);
        boot(&hdr, other, &m, 0, wdt_bite);
    }
    LOG("fallback slot %c invalid too: %s\n", slot_name(other), img_status_str(st2));
    recovery_halt();
}

/* Exceptions taken while the app runs land here (vectors live at 0x0). */
void fault_handler(uint32_t kind, uint32_t pc)
{
    static const char *const names[] = {
        "?", "UNDEFINED INSTRUCTION", "SVC", "PREFETCH ABORT", "DATA ABORT", "IRQ", "FIQ",
    };
    uart_init();
    kprintf("\n[BL] !!! CPU EXCEPTION: %s at pc=%p", names[kind < 7 ? kind : 0], pc);
    if (kind == 4) {
        uint32_t dfar;
        __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
        kprintf(" addr=%p", dfar);
    }
    kprintf(" -- resetting\n");
    system_reset();
}
