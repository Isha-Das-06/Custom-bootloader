#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

/*
 * Signed firmware image = 256-byte header + payload, stored at the start of a slot.
 *
 *   The Ed25519 signature covers header bytes [0, 192): every field below,
 *   including the SHA-512 of the payload. So one signature check authenticates
 *   the metadata (version, load address, size) and, through the hash, the code.
 *
 * All multi-byte fields are little-endian. tools/imgtool.py builds these.
 */

#define IMAGE_MAGIC         0x4d494f41u   /* "AOIM" in memory: 'A','O','I','M' */
#define IMAGE_HEADER_SIZE   256u
#define IMAGE_SIGNED_BYTES  192u

typedef struct {
    uint32_t magic;
    uint32_t header_size;     /* == IMAGE_HEADER_SIZE */
    uint32_t image_size;      /* payload bytes following the header */
    uint32_t load_addr;       /* where the bootloader copies the payload */
    uint32_t entry_addr;      /* where it jumps */
    uint32_t version;         /* 0x00MMmmpp; also the anti-rollback counter */
    uint32_t flags;
    uint32_t reserved0;
    uint8_t  payload_sha512[64];
    uint8_t  key_id[8];       /* first 8 bytes of SHA-512(pubkey); diagnostics only */
    uint8_t  reserved1[88];
    /* ---- end of signed region (offset 192) ---- */
    uint8_t  signature[64];
} image_header_t;

_Static_assert(sizeof(image_header_t) == IMAGE_HEADER_SIZE, "header layout");
_Static_assert(__builtin_offsetof(image_header_t, signature) == IMAGE_SIGNED_BYTES, "signed region");

/* Result codes from image validation, shared so the app can report them. */
enum img_status {
    IMG_OK = 0,
    IMG_EMPTY,          /* slot is erased */
    IMG_BAD_MAGIC,
    IMG_BAD_HEADER,     /* sizes/addresses out of bounds */
    IMG_BAD_SIGNATURE,
    IMG_BAD_HASH,       /* signature fine but payload doesn't match */
    IMG_ROLLBACK,       /* version below the anti-rollback floor */
};

static inline const char *img_status_str(int s)
{
    switch (s) {
    case IMG_OK:            return "ok";
    case IMG_EMPTY:         return "empty";
    case IMG_BAD_MAGIC:     return "bad magic";
    case IMG_BAD_HEADER:    return "bad header";
    case IMG_BAD_SIGNATURE: return "BAD SIGNATURE";
    case IMG_BAD_HASH:      return "PAYLOAD HASH MISMATCH";
    case IMG_ROLLBACK:      return "version below rollback floor";
    default:                return "?";
    }
}

#endif
