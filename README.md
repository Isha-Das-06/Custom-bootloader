# A/B Secure Bootloader with OTA Updates and Automatic Rollback

A bare-metal bootloader for ARMv7-A (Cortex-A15) that runs on QEMU's `virt` board. It has:

- **Signed images.** Ed25519 signature over the image header, and a SHA-512 of the payload inside that signed header. The public key is compiled into the bootloader ROM.
- **Two firmware slots (A/B).** Updates always go into the slot that isn't running, so the known-good image is never overwritten.
- **Trial boots with automatic rollback.** A new image gets 3 boot attempts to confirm itself. If it hangs, crashes or fails its own self-test, the bootloader goes back to the last good image.
- **A hardware watchdog.** It is armed and locked by the bootloader before the app starts, so a deadlocked image can't avoid being reset.
- **Anti-rollback.** Once v1.3.0 has been confirmed, a validly signed v1.0.0 is refused.
- **Power-fail-safe metadata.** It is stored as a CRC-protected append-only journal across two flash sectors.
- **Real flash programming.** The slots live on an emulated Intel CFI NOR flash chip (sector erase, word program, status polling), backed by a file. State survives QEMU restarts the same way it survives a power cycle.

The C code has no dependencies: no libc and no crypto library. The Ed25519 and SHA-512 code is written for this project and checked against Python's `cryptography` package.

```
[BL] meta: seq=8 active=B pending=A tries_left=0 floor=v1.1.0 boots=6 rollbacks=0
[BL] slot A was never confirmed after 3 boot attempts
[BL] >>> ROLLING BACK to slot B <<<
[BL] verifying ACTIVE slot B ...
[BL]   slot B: v1.1.0, 10104 bytes, Ed25519 ok (16 ms), SHA-512 ok (1 ms)
[BL] watchdog armed (3000 ms, locked). jumping to 0x40100000
|  APPLICATION v1.1.0     [release ]  slot B     |
  (note: last reset was caused by the watchdog)
  last boot-control event: ROLLBACK (trial never confirmed) (slot A)
```

---

## Quick start

You need `arm-none-eabi-gcc`, `qemu-system-arm`, `make`, host `gcc`, and Python 3 with `cryptography`. On Windows, run everything inside WSL (Ubuntu):

```bash
sudo apt install gcc-arm-none-eabi qemu-system-arm make gcc python3-cryptography
```

```bash
make            # generates a dev signing key, builds the bootloader, 6 app releases and the flash images
make test       # host test: C crypto vs Python cryptography (541 vectors)
make demo       # 13 end-to-end scenarios in QEMU, with assertions
make run        # interactive console (commands: help/status/reboot/hang/crash; Ctrl-A X to quit)
```

From Windows PowerShell you can run a target without opening a WSL shell:

```bash
wsl make demo
```

## Demo scenarios (`make demo`)

Each scenario boots the real bootloader in QEMU, drives the app over the emulated UART, and asserts on what happens. Later scenarios build on the flash state that earlier ones left behind.

| # | Scenario | What it proves |
|---|---|---|
| 1 | factory | First boot creates metadata. Factory v1.0.0 in slot A passes verification and boots |
| 2 | ota | v1.1.0 goes over UART into slot B, then trial boot, self-test and confirm. The rollback floor becomes v1.1.0 |
| 3 | hang | v1.2.0 deadlocks. The watchdog resets the board 3 times and the bootloader rolls back to v1.1.0 |
| 4 | crash | v1.2.1 executes an undefined instruction. The bootloader's fault vector resets the board 3 times, then rolls back |
| 5 | selftest | v1.2.2 fails its self-test, marks itself bad and resets. Rollback happens on the first attempt |
| 6 | tampered | One payload byte is flipped after signing. The bootloader reports a payload hash mismatch and never runs it |
| 7 | wrongkey | An image signed with an attacker's key fails the signature check and never runs |
| 8 | downgrade | A validly signed v1.0.0 is below the anti-rollback floor, so it is refused |
| 9 | powerloss | QEMU is killed halfway through an OTA. The device still boots v1.1.0 and the half-written slot is ignored |
| 10 | ota2 | v1.3.0 goes into slot A and is confirmed. The floor becomes v1.3.0 |
| 11 | persist | Cold power cycle (new QEMU process): the state comes back from flash alone |
| 12 | failover | A bit flip in the active slot's flash makes the bootloader boot the other valid slot |
| 13 | recovery | Both slots are corrupt, so the bootloader refuses to run anything and halts in recovery |

`python3 tools/demo.py -k crash` runs the scenarios up to and including `crash`. The full serial log is written to `build/demo.log`.

---

## Architecture

```
            QEMU virt (Cortex-A15, 32-bit)
 ┌──────────────────────────────────────────────────────────────┐
 │ 0x00000000  pflash0  BOOTLOADER (XIP, read-only, root of trust) │
 │             vectors @0 stay installed while the app runs   │
 │ 0x04000000  pflash1  ┌ +0x000000 META bank 0   (256 KiB)    │
 │                      ├ +0x040000 META bank 1   (256 KiB)    │
 │                      ├ +0x100000 SLOT A        (1 MiB)      │
 │                      └ +0x200000 SLOT B        (1 MiB)      │
 │ 0x09000000  PL011 UART  ── serial console / OTA transport   │
 │ 0x3f000000  PCIe ECAM   ── i6300esb watchdog (BAR0 @0x10000000) │
 │ 0x40000000  RAM   bootloader data/stacks                    │
 │ 0x400FF000        boot_info (bootloader → app handoff)      │
 │ 0x40100000        APPLICATION (copied here, runs from RAM)  │
 └──────────────────────────────────────────────────────────────┘
```

### Boot decision

```
reset
  │
  ├─ load newest valid metadata record (CRC) ── none? → init: active=A
  │
  ├─ pending slot set?
  │     ├─ tries_left == 0 ───────────────► ROLLBACK: pending=none, rollbacks++
  │     └─ verify(pending)
  │           ├─ ok  → tries_left--, persist, TRIAL BOOT ──► app
  │           └─ bad → REJECT: pending=none (the image never executes)
  │
  ├─ verify(active)  ok → BOOT ──► app
  │
  ├─ verify(other)   ok → FAILOVER: active=other → BOOT
  │
  └─ RECOVERY HALT (nothing trustworthy to run)
```

On a trial boot, the app runs its self-test and then **confirms** itself by writing `active=<its slot>, pending=none, floor=max(floor, version)`. If it never confirms, for any reason, each reset uses up one try. That covers hangs (watchdog), crashes (fault vector, then PSCI reset), brownouts and bugs we didn't think of. The bootloader doesn't need to know *why* the image failed.

### Verification (bootloader/boot.c)

1. Copy the 256-byte header from flash to RAM.
2. Check the **Ed25519 signature** over header bytes `[0,192)` before trusting any field in it.
3. Bounds-check the size, load address and entry point, and check `version >= floor`.
4. Copy the payload to its RAM load address, then **SHA-512 the RAM copy** and compare it with the signed hash.
5. Arm and lock the watchdog, then jump.

The bootloader hashes the copy it is about to execute, not the flash. That closes the time-of-check/time-of-use gap where flash could change between verifying and running.

### Image format (common/image.h)

| offset | field |
|---|---|
| 0 | magic, header_size, image_size, load_addr, entry_addr, version, flags, reserved |
| 32 | SHA-512 of payload (64 B) |
| 96 | key id (8 B), reserved (88 B) |
| **192** | **Ed25519 signature** over bytes 0..191 |
| 256 | payload |

### Metadata journal (common/meta.c)

Each metadata record is 32 bytes (`magic, seq, active, pending, tries_left, floor, counters, crc32`). An update **appends** a record with `seq+1`. When a sector fills, the other sector is erased and the journal continues there. On boot, the newest record with a valid CRC wins. Because of this:
- A torn write fails its CRC and the previous record is used.
- An interrupted erase only hits the *older* bank.
- One sector holds 8192 records before it needs an erase (flash wear).

### OTA transport (app/main.c)

`update` means the app receives `"OTA!" | len | image | crc32` over UART. It writes the image to the **inactive** slot (erase, program, read back and compare), and only after the CRC matches does it set `pending=<slot>, tries_left=3` and reset. The app deliberately does **not** check signatures: the bootloader is the only authority. The app also refuses to start an OTA while it is itself still on trial, because the slot it would overwrite is the known-good fallback.

### Watchdog

The i6300esb is a PCI device. The bootloader finds it through ECAM, maps BAR0, sets two 1.5 s stages, then enables and **locks** it, so the app cannot turn it off. The app feeds it from its main loop. The device's "previous reboot was a watchdog timeout" flag is reported on the next boot.

---

## Layout

```
bootloader/   start.S (vectors, stacks, fault entry), boot.c (A/B logic), linker script
app/          start.S, main.c (self-test, confirm, OTA receiver, console), linker script
common/       board.h (memory map), flash.c (CFI NOR driver), wdt.c (i6300esb), meta.c (journal), util.c
crypto/       sha512.c, ed25519.c (verify-only, streaming, rejects non-canonical S)
tools/        imgtool.py (keygen/sign/info/mkflash), demo.py (end-to-end harness)
test/         crypto_test.c + gen_vectors.py (RFC 8032 + tamper/malleability vectors)
```

## Honest limitations and what production would add

- **The rollback floor and metadata live in ordinary flash.** Someone with physical flash access could rewrite them. Real parts keep the security counter in OTP, eFuses or RPMB.
- **The bootloader isn't measured or verified by anything below it.** Here it is the root of trust by assumption. On silicon it would be ROM, or verified by a boot ROM (secure boot / HAB / TF-M BL1).
- **Only one signing key, and no revocation.** Production would support a key list with revocation, and ideally sign with a key kept in an HSM.
- **Images are not encrypted.** Signing gives integrity and authenticity, not confidentiality.
- **Ed25519 verification is not constant-time.** This is fine for verification, since every input is public, but this code must not be reused for signing.
- `keys/signing_key.pem` is a dev key that the build generates locally. Never commit a real one.
