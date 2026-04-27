#!/usr/bin/env python3
"""Generate cross-check vectors for the C SHA-512 / Ed25519 code (see crypto_test.c).

Record format (little-endian):
  'H' u32 len  msg  digest[64]                         SHA-512 vector
  'E' u8 expect  pub[32] sig[64] u32 len  msg          Ed25519 vector (expect 1 = valid)
"""
import hashlib
import os
import random
import struct
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

L = 2**252 + 27742317777372353535851937790883648493
rng = random.Random(1234)
out = bytearray()


def raw(pub):
    return pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def H(msg):
    out.extend(b"H" + struct.pack("<I", len(msg)) + msg + hashlib.sha512(msg).digest())


def E(expect, pub, sig, msg):
    out.extend(b"E" + bytes([expect]) + pub + sig + struct.pack("<I", len(msg)) + msg)


# SHA-512: every length around the block/padding boundaries, plus a big one
for n in list(range(0, 300)) + [1000, 4096, 65537, 1 << 20]:
    H(bytes(rng.getrandbits(8) for _ in range(n)) if n < 70000 else os.urandom(n))

# RFC 8032 test 1 (empty message) -- checks we agree with the spec, not just with ourselves
sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"))
sig = sk.sign(b"")
assert sig.hex().startswith("e5564300c360ac729086e2cc806e828a")
E(1, raw(sk.public_key()), sig, b"")

other = raw(Ed25519PrivateKey.generate().public_key())
for i in range(40):
    sk = Ed25519PrivateKey.generate()
    pub = raw(sk.public_key())
    msg = os.urandom(rng.choice([0, 1, 31, 64, 127, 128, 192, 1000]))
    sig = sk.sign(msg)
    E(1, pub, sig, msg)
    if msg:
        bad = bytearray(msg); bad[rng.randrange(len(bad))] ^= 1 << rng.randrange(8)
        E(0, pub, sig, bytes(bad))                             # tampered message
    bad = bytearray(sig); bad[rng.randrange(32)] ^= 1 << rng.randrange(8)
    E(0, pub, bytes(bad), msg)                                 # tampered R
    bad = bytearray(sig); bad[32 + rng.randrange(31)] ^= 1 << rng.randrange(8)
    E(0, pub, bytes(bad), msg)                                 # tampered S
    E(0, other, sig, msg)                                      # wrong key
    s = int.from_bytes(sig[32:], "little") + L                 # malleated S+L
    if s < 2**256:
        E(0, pub, sig[:32] + s.to_bytes(32, "little"), msg)

with open(sys.argv[1], "wb") as f:
    f.write(out)
