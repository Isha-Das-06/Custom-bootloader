/* Host test: run the exact same crypto sources the bootloader uses against
 * vectors produced by Python's `cryptography` / hashlib. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ed25519.h"
#include "sha512.h"

static unsigned char *buf;
static size_t pos, size;

static void need(size_t n)
{
    if (pos + n > size) {
        fprintf(stderr, "truncated vector file\n");
        exit(2);
    }
}

static unsigned rd32(void)
{
    need(4);
    unsigned v = buf[pos] | buf[pos + 1] << 8 | buf[pos + 2] << 16 | (unsigned)buf[pos + 3] << 24;
    pos += 4;
    return v;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s vectors.bin\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    rewind(f);
    buf = malloc(size);
    if (fread(buf, 1, size, f) != size)
        return 2;
    fclose(f);

    int hashes = 0, sigs = 0, fails = 0;
    while (pos < size) {
        char kind = (char)buf[pos++];
        if (kind == 'H') {
            unsigned n = rd32();
            need(n + 64);
            unsigned char d[64];
            /* feed in odd-sized pieces to exercise the streaming path */
            sha512_ctx c;
            sha512_init(&c);
            for (unsigned off = 0; off < n;) {
                unsigned step = 1 + (off * 7 + 13) % 300;
                if (step > n - off) step = n - off;
                sha512_update(&c, buf + pos + off, step);
                off += step;
            }
            sha512_final(&c, d);
            if (memcmp(d, buf + pos + n, 64)) {
                printf("FAIL sha512 len=%u\n", n);
                fails++;
            }
            pos += n + 64;
            hashes++;
        } else if (kind == 'E') {
            need(1 + 32 + 64);
            int expect = buf[pos];
            const unsigned char *pub = buf + pos + 1, *sig = buf + pos + 33;
            pos += 97;
            unsigned n = rd32();
            need(n);
            int ok = ed25519_verify(sig, buf + pos, n, pub) == 0;
            if (ok != expect) {
                printf("FAIL ed25519 #%d len=%u expected %s got %s\n", sigs, n,
                       expect ? "valid" : "invalid", ok ? "valid" : "invalid");
                fails++;
            }
            pos += n;
            sigs++;
        } else {
            fprintf(stderr, "bad record\n");
            return 2;
        }
    }
    printf("crypto_test: %d SHA-512 vectors, %d Ed25519 vectors, %d failures\n", hashes, sigs, fails);
    return fails ? 1 : 0;
}
