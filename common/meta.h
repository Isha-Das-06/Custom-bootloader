#ifndef META_H
#define META_H

#include <stdint.h>
#include "board.h"

/*
 * Boot-control metadata, shared between bootloader and application.
 *
 * Stored as an append-only journal of 32-byte CRC-protected records across
 * two flash sectors. A write appends a new record with seq+1; when the current
 * sector fills, the *other* sector is erased and the journal continues there.
 * The newest record with a valid CRC wins, so a power cut in the middle of any
 * write leaves the previous state intact.
 */

#define META_MAGIC      0x4154454du   /* "META" */

#define SLOT_A          0u
#define SLOT_B          1u
#define SLOT_NONE       0xffu

/* last_event: why the previous boot decision happened (for diagnostics). */
enum meta_event {
    EV_NONE = 0,
    EV_FACTORY_INIT,       /* metadata created on first boot */
    EV_UPDATE_STAGED,      /* app installed an image into the inactive slot */
    EV_TRIAL_BOOT,         /* bootloader started a trial of the pending slot */
    EV_CONFIRMED,          /* app confirmed the trial image as good */
    EV_ROLLBACK_TRIES,     /* trial image never confirmed -> rolled back */
    EV_REJECTED,           /* pending image failed verification -> discarded */
    EV_ACTIVE_FAILOVER,    /* active image failed verification -> other slot */
    EV_APP_REJECTED,       /* app's own self-test failed; it asked for rollback */
};

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  active;        /* confirmed-good slot */
    uint8_t  pending;       /* slot on trial, or SLOT_NONE */
    uint8_t  tries_left;    /* boot attempts remaining for pending */
    uint8_t  last_event;
    uint32_t min_version;   /* anti-rollback floor */
    uint32_t boot_count;
    uint32_t rollback_count;
    uint8_t  last_event_slot;
    uint8_t  last_event_detail;  /* e.g. enum img_status for rejections */
    uint16_t reserved;
    uint32_t crc;           /* CRC-32 of the preceding 28 bytes */
} boot_meta_t;

_Static_assert(sizeof(boot_meta_t) == 32, "meta record size");

int meta_load(boot_meta_t *out);          /* 0 if a valid record was found */
int meta_save(boot_meta_t *m);            /* bumps seq, fixes crc, appends */
void meta_defaults(boot_meta_t *m);

static inline uint32_t slot_addr(unsigned slot) { return slot == SLOT_A ? SLOT_A_ADDR : SLOT_B_ADDR; }
static inline char slot_name(unsigned slot) { return slot == SLOT_A ? 'A' : slot == SLOT_B ? 'B' : '-'; }

const char *meta_event_str(unsigned ev);

/* Handed from bootloader to app in RAM at BOOT_INFO_ADDR (and in r0). */
#define BOOT_INFO_MAGIC 0x4f464e49u   /* "INFO" */
typedef struct {
    uint32_t magic;
    uint32_t slot;          /* slot the running image came from */
    uint32_t trial;         /* 1 if this is an unconfirmed trial boot */
    uint32_t tries_left;
    uint32_t version;
    uint32_t boot_count;
    uint32_t wdt_reset;     /* 1 if the previous reset was a watchdog bite */
    uint32_t bl_version;
} boot_info_t;

#endif
