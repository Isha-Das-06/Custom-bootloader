#ifndef ED25519_H
#define ED25519_H

#include <stddef.h>
#include <stdint.h>

/*
 * Verify an Ed25519 (RFC 8032, pure) signature.
 * Returns 0 if valid, -1 otherwise. Constant-time is not required for
 * verification (all inputs are public), but the code is still branch-light.
 */
int ed25519_verify(const uint8_t sig[64], const void *msg, size_t len,
                   const uint8_t pubkey[32]);

#endif
