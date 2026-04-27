#include "meta.h"
#include "board.h"
#include "flash.h"
#include "util.h"

#define RECS_PER_BANK (FLASH_SECTOR_SIZE / sizeof(boot_meta_t))

static const uint32_t banks[2] = { META_BANK0_ADDR, META_BANK1_ADDR };

static int rec_valid(const boot_meta_t *r)
{
    return r->magic == META_MAGIC &&
           r->crc == crc32(0, r, __builtin_offsetof(boot_meta_t, crc));
}

static int rec_blank(const boot_meta_t *r)
{
    const uint32_t *w = (const uint32_t *)r;
    for (unsigned i = 0; i < sizeof(*r) / 4; i++)
        if (w[i] != 0xFFFFFFFFu)
            return 0;
    return 1;
}

/* Newest valid record overall; also reports the write cursor. */
static int scan(boot_meta_t *best, int *best_bank, int *next_idx)
{
    int found = 0;
    for (int b = 0; b < 2; b++) {
        const boot_meta_t *recs = (const boot_meta_t *)(uintptr_t)banks[b];
        for (unsigned i = 0; i < RECS_PER_BANK; i++) {
            if (rec_blank(&recs[i]))
                break;           /* journal within a bank is contiguous */
            if (!rec_valid(&recs[i]))
                continue;        /* torn write: skip it */
            if (!found || recs[i].seq > best->seq) {
                *best = recs[i];
                *best_bank = b;
                found = 1;
            }
        }
    }
    if (found) {
        /* first blank slot after the newest record's bank contents */
        const boot_meta_t *recs = (const boot_meta_t *)(uintptr_t)banks[*best_bank];
        unsigned i = 0;
        while (i < RECS_PER_BANK && !rec_blank(&recs[i]))
            i++;
        *next_idx = (int)i;
    }
    return found;
}

void meta_defaults(boot_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = META_MAGIC;
    m->active = SLOT_A;
    m->pending = SLOT_NONE;
    m->last_event = EV_FACTORY_INIT;
    m->last_event_slot = SLOT_NONE;
}

int meta_load(boot_meta_t *out)
{
    int bank, idx;
    return scan(out, &bank, &idx) ? 0 : -1;
}

int meta_save(boot_meta_t *m)
{
    boot_meta_t cur;
    int bank = 0, idx = 0;
    uint32_t seq = 0;

    if (scan(&cur, &bank, &idx)) {
        seq = cur.seq + 1;
    } else {
        /* nothing valid anywhere: pretend bank 1 is full so bank 0 gets erased */
        bank = 1;
        idx = (int)RECS_PER_BANK;
    }

    uint32_t addr;
    if (idx >= (int)RECS_PER_BANK) {
        /* current bank full: erase the other (older) one and continue there */
        int nb = bank ^ 1;
        int rc = flash_erase_sector(banks[nb]);
        if (rc)
            return rc;
        addr = banks[nb];
    } else {
        addr = banks[bank] + (uint32_t)idx * sizeof(boot_meta_t);
    }

    m->magic = META_MAGIC;
    m->seq = seq;
    m->crc = crc32(0, m, __builtin_offsetof(boot_meta_t, crc));
    return flash_program(addr, m, sizeof(*m));
}

const char *meta_event_str(unsigned ev)
{
    switch (ev) {
    case EV_NONE:            return "none";
    case EV_FACTORY_INIT:    return "factory init";
    case EV_UPDATE_STAGED:   return "update staged";
    case EV_TRIAL_BOOT:      return "trial boot";
    case EV_CONFIRMED:       return "confirmed";
    case EV_ROLLBACK_TRIES:  return "ROLLBACK (trial never confirmed)";
    case EV_REJECTED:        return "REJECTED (verification failed)";
    case EV_ACTIVE_FAILOVER: return "FAILOVER (active image invalid)";
    case EV_APP_REJECTED:    return "ROLLBACK (app self-test failed)";
    default:                 return "?";
    }
}
