#!/usr/bin/env python3
"""
End-to-end OTA / rollback demo + integration test.

Boots the real bootloader and signed images in QEMU, talks to the app over the
emulated UART, pushes updates exactly like a field OTA would, and asserts on
what the bootloader does. The same flash file is reused across QEMU restarts,
so "power cycles" are real: state only survives if it was written to flash.

  python3 tools/demo.py            run every scenario
  python3 tools/demo.py -k hang    only scenarios whose name contains "hang"
  python3 tools/demo.py -q         don't echo the serial console
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
FLASH0 = os.path.join(B, "flash0.bin")
FLASH1 = os.path.join(B, "demo-flash1.bin")
LOG = os.path.join(B, "demo.log")

SLOT_OFF = {"A": 0x100000, "B": 0x200000}
HDR = 256

TTY = sys.stdout.isatty()
def c(code, s): return f"\033[{code}m{s}\033[0m" if TTY else s
DIM, BOLD, GREEN, RED, CYAN, YELLOW = "2", "1", "32", "31", "36", "33"


class Board:
    """One QEMU process = one power-on of the board."""

    def __init__(self, echo, logf):
        self.echo, self.logf = echo, logf
        self.buf = ""
        self.cursor = 0
        self.cv = threading.Condition()
        self.proc = subprocess.Popen(
            ["qemu-system-arm",
             "-M", "virt,highmem=off,secure=off,virtualization=off",
             "-cpu", "cortex-a15", "-m", "128M", "-nic", "none", "-display", "none",
             "-device", "i6300esb",
             "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={FLASH0}",
             "-drive", f"if=pflash,format=raw,unit=1,file={FLASH1}",
             "-serial", "stdio", "-monitor", "none"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        line = ""
        while True:
            b = self.proc.stdout.read(1)
            if not b:
                break
            ch = b.decode("latin-1")
            with self.cv:
                self.buf += ch
                self.cv.notify_all()
            if ch == "\n":
                self.logf.write(line.rstrip("\r") + "\n")
                if self.echo:
                    print(c(DIM, "    │ " + line.rstrip("\r")), flush=True)
                line = ""
            else:
                line += ch

    def expect(self, pattern, timeout=15):
        """Wait for regex after the cursor; advance the cursor past it."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        with self.cv:
            while True:
                m = rx.search(self.buf, self.cursor)
                if m:
                    self.cursor = m.end()
                    return m
                left = deadline - time.time()
                if left <= 0 or self.proc.poll() is not None:
                    raise AssertionError(f"timed out waiting for /{pattern}/")
                self.cv.wait(min(left, 0.2))

    def send(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def cmd(self, line):
        self.expect(r"app> ")
        self.send(line.encode() + b"\r")

    def ota(self, image, cut_after=None):
        """Push an image through the app's 'update' command (the OTA client)."""
        with open(image, "rb") as f:
            data = f.read()
        self.cmd("update")
        self.expect(r"OTA: READY")
        pkt = b"OTA!" + struct.pack("<I", len(data)) + data + struct.pack("<I", zlib.crc32(data))
        if cut_after is not None:
            pkt = pkt[:8 + cut_after]
        self.send(pkt)

    def power_off(self):
        self.proc.kill()
        self.proc.wait()


class Demo:
    def __init__(self, echo):
        self.echo = echo
        self.logf = open(LOG, "w")
        self.board = None
        self.results = []

    def power_on(self):
        if self.board:
            self.board.power_off()
        self.board = Board(self.echo, self.logf)
        return self.board

    def power_cut(self):
        say(c(YELLOW, "  ⚡ power cut (QEMU killed, flash file kept)"))
        self.board.power_off()
        self.board = None

    def img(self, name):
        return os.path.join(B, name + ".img")

    def run(self, name, desc, fn):
        print()
        print(c(BOLD + ";" + CYAN, f"━━ {name}: {desc}"))
        t0 = time.time()
        try:
            fn()
            ok, err = True, ""
        except AssertionError as e:
            ok, err = False, str(e)
        dt = time.time() - t0
        self.results.append((name, desc, ok, dt, err))
        print(c(GREEN, f"  ✔ PASS ({dt:.1f}s)") if ok else c(RED, f"  ✘ FAIL: {err}"))
        return ok


def say(s):
    print(s, flush=True)


def corrupt_slot(slot, offset_in_payload=0x80):
    with open(FLASH1, "r+b") as f:
        f.seek(SLOT_OFF[slot] + HDR + offset_in_payload)
        b = f.read(1)
        f.seek(-1, 1)
        f.write(bytes([b[0] ^ 0x40]))
    say(c(YELLOW, f"  ☢ flipped one bit in slot {slot}'s payload (simulated flash bit rot)"))


def scenarios(d: Demo):
    img = d.img

    def factory():
        b = d.power_on()
        b.expect(r"first boot, initialising")
        b.expect(r"slot A: v1\.0\.0,.*Ed25519 ok")
        b.expect(r"APPLICATION v1\.0\.0\s+\[release\s*\]\s+slot A")

    def ota_good():
        b = d.board
        b.ota(img("app-1.1.0"))
        b.expect(r"slot B marked PENDING")
        b.expect(r"TRIAL boot of slot B \(attempt 1 of 3\)")
        b.expect(r"APPLICATION v1\.1\.0")
        b.expect(r"CONFIRMED: slot B is now the active image; rollback floor = v1\.1\.0")
        b.cmd("status")
        b.expect(r"active=B pending=- tries_left=0 floor=v1\.1\.0")

    def rollback(image, marker, reset_marker, final_event):
        def fn():
            b = d.board
            b.ota(img(image))
            b.expect(r"slot A marked PENDING")
            for i in (1, 2, 3):
                b.expect(rf"TRIAL boot of slot A \(attempt {i} of 3\)", timeout=20)
                b.expect(marker, timeout=20)
                b.expect(reset_marker, timeout=20)   # proves *what* reset the board
            b.expect(r"never confirmed after 3 boot attempts", timeout=20)
            b.expect(r">>> ROLLING BACK to slot B <<<")
            b.expect(r"APPLICATION v1\.1\.0\s+\[release\s*\]\s+slot B")
            b.expect(final_event)
        return fn

    def selftest_fail():
        b = d.board
        b.ota(img("app-1.2.2-selftest"))
        b.expect(r"TRIAL boot of slot A \(attempt 1 of 3\)")
        b.expect(r"self-test FAILED -> rejecting this image")
        b.expect(r"verifying ACTIVE slot B")
        b.expect(r"APPLICATION v1\.1\.0")
        b.expect(r"last boot-control event: ROLLBACK \(app self-test failed\) \(slot A\)")

    def rejected(image, reason):
        def fn():
            b = d.board
            b.ota(img(image))
            b.expect(r"slot A marked PENDING")
            b.expect(r"verifying PENDING slot A")
            b.expect(rf"slot A REJECTED: {reason} -> discarding update, it will not be run")
            b.expect(r"APPLICATION v1\.1\.0")
            b.expect(rf"REJECTED \(verification failed\) \(slot A\): {reason}")
        return fn

    def power_loss_mid_ota():
        b = d.board
        b.ota(img("app-1.3.0"), cut_after=6000)
        b.expect(r"OTA: 4096/")
        time.sleep(0.5)
        d.power_cut()
        b = d.power_on()
        b.expect(r"meta: .*active=B pending=-")
        b.expect(r"APPLICATION v1\.1\.0\s+\[release\s*\]\s+slot B")
        b.cmd("status")
        b.expect(r"slot A @0x04100000: v1\.3\.0")    # half-written, never marked bootable

    def ota_130():
        b = d.board
        b.ota(img("app-1.3.0"))
        b.expect(r"TRIAL boot of slot A \(attempt 1 of 3\)")
        b.expect(r"CONFIRMED: slot A .* rollback floor = v1\.3\.0")

    def persist():
        d.power_cut()
        b = d.power_on()
        b.expect(r"meta: .*active=A pending=- tries_left=0 floor=v1\.3\.0")
        b.expect(r"verifying ACTIVE slot A")
        b.expect(r"APPLICATION v1\.3\.0\s+\[release\s*\]\s+slot A")

    def reinstall_and_failover():
        b = d.board
        b.ota(img("app-1.3.0"))                      # same version into B is allowed (>= floor)
        b.expect(r"CONFIRMED: slot B")
        d.power_cut()
        corrupt_slot("B")
        b = d.power_on()
        b.expect(r"ACTIVE slot B INVALID: PAYLOAD HASH MISMATCH")
        b.expect(r">>> FAILOVER: slot A becomes active <<<")
        b.expect(r"APPLICATION v1\.3\.0\s+\[release\s*\]\s+slot A")

    def recovery():
        d.power_cut()
        corrupt_slot("A")
        b = d.power_on()
        b.expect(r"ACTIVE slot A INVALID: PAYLOAD HASH MISMATCH")
        b.expect(r"fallback slot B invalid too")
        b.expect(r"NO BOOTABLE IMAGE")

    return [
        ("factory",     "first boot: metadata created, factory v1.0.0 in slot A", factory),
        ("ota",         "OTA v1.1.0 -> slot B, trial boot, self-test, confirm", ota_good),
        ("hang",        "OTA v1.2.0 that deadlocks -> watchdog x3 -> auto-rollback",
         rollback("app-1.2.0-hang", r"deadlocks during init", r"previous reset was a WATCHDOG TIMEOUT",
                  r"last boot-control event: ROLLBACK \(trial never confirmed\)")),
        ("crash",       "OTA v1.2.1 that crashes -> CPU exception x3 -> auto-rollback",
         rollback("app-1.2.1-crash", r"CPU EXCEPTION: UNDEFINED INSTRUCTION", r"-- resetting",
                  r"last boot-control event: ROLLBACK \(trial never confirmed\)")),
        ("selftest",    "OTA v1.2.2 fails its own self-test -> immediate rollback", selftest_fail),
        ("tampered",    "attacker flips a byte after signing -> rejected, never runs",
         rejected("app-1.3.0-tampered", "PAYLOAD HASH MISMATCH")),
        ("wrongkey",    "image signed with an attacker's key -> rejected",
         rejected("app-1.3.0-wrongkey", "BAD SIGNATURE")),
        ("downgrade",   "validly-signed old v1.0.0 -> blocked by anti-rollback",
         rejected("app-1.0.0", "version below rollback floor")),
        ("powerloss",   "power cut halfway through an OTA -> still boots v1.1.0", power_loss_mid_ota),
        ("ota2",        "OTA v1.3.0 -> slot A, confirm, floor raised to v1.3.0", ota_130),
        ("persist",     "cold power-cycle: state lives in flash, not RAM", persist),
        ("failover",    "bit rot in the active slot -> fail over to the other slot", reinstall_and_failover),
        ("recovery",    "both slots corrupt -> refuse to boot anything (recovery halt)", recovery),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-q", "--quiet", action="store_true", help="don't echo serial output")
    ap.add_argument("-k", help="only run scenarios whose name contains this (factory always runs)")
    a = ap.parse_args()

    if not shutil.which("qemu-system-arm"):
        sys.exit("qemu-system-arm not found")
    for f in (FLASH0, os.path.join(B, "flash1-factory.bin")):
        if not os.path.exists(f):
            sys.exit(f"missing {f} -- run `make` first")
    shutil.copy(os.path.join(B, "flash1-factory.bin"), FLASH1)

    d = Demo(echo=not a.quiet)
    todo = scenarios(d)
    if a.k:
        # later scenarios depend on earlier state, so keep the prefix they need
        idx = max(i for i, s in enumerate(todo) if a.k in s[0])
        todo = todo[:idx + 1]

    failed = False
    try:
        for name, desc, fn in todo:
            if not d.run(name, desc, fn):
                failed = True
                break
    finally:
        if d.board:
            d.board.power_off()
        d.logf.close()

    print()
    print(c(BOLD, "━━ summary ━━"))
    for name, desc, ok, dt, err in d.results:
        mark = c(GREEN, "PASS") if ok else c(RED, "FAIL")
        print(f"  {mark}  {name:<10} {desc}  ({dt:.1f}s)")
    print(f"\n  full serial log: {os.path.relpath(LOG, ROOT)}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
