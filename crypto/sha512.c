/*
 * SHA-512 (FIPS 180-4), streaming, freestanding.
 * Constants generated from the prime cube/square roots, not hand-typed.
 */
#include "sha512.h"

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void compress(uint64_t st[8], const uint8_t *block)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = load_be64(block + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint64_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void sha512_init(sha512_ctx *ctx)
{
    static const uint64_t H0[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    for (int i = 0; i < 8; i++)
        ctx->state[i] = H0[i];
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha512_update(sha512_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    ctx->total_len += len;
    while (len) {
        if (ctx->buf_len == 0 && len >= 128) {
            compress(ctx->state, p);
            p += 128;
            len -= 128;
            continue;
        }
        uint32_t n = 128 - ctx->buf_len;
        if (n > len)
            n = (uint32_t)len;
        for (uint32_t i = 0; i < n; i++)
            ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += n;
        p += n;
        len -= n;
        if (ctx->buf_len == 128) {
            compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void sha512_final(sha512_ctx *ctx, uint8_t out[SHA512_DIGEST_SIZE])
{
    uint64_t bits = ctx->total_len * 8;
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < 128)
            ctx->buf[ctx->buf_len++] = 0;
        compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 120)
        ctx->buf[ctx->buf_len++] = 0;   /* upper 64 bits of the 128-bit length are zero */
    store_be64(ctx->buf + 120, bits);
    compress(ctx->state, ctx->buf);
    for (int i = 0; i < 8; i++)
        store_be64(out + 8 * i, ctx->state[i]);
}

void sha512(const void *data, size_t len, uint8_t out[SHA512_DIGEST_SIZE])
{
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}
