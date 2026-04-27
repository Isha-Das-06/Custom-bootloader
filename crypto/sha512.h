#ifndef SHA512_H
#define SHA512_H

#include <stddef.h>
#include <stdint.h>

#define SHA512_DIGEST_SIZE 64

typedef struct {
    uint64_t state[8];
    uint64_t total_len;   /* bytes hashed so far (images are < 4 GiB, so no 128-bit counter needed) */
    uint8_t  buf[128];
    uint32_t buf_len;
} sha512_ctx;

void sha512_init(sha512_ctx *ctx);
void sha512_update(sha512_ctx *ctx, const void *data, size_t len);
void sha512_final(sha512_ctx *ctx, uint8_t out[SHA512_DIGEST_SIZE]);
void sha512(const void *data, size_t len, uint8_t out[SHA512_DIGEST_SIZE]);

#endif
