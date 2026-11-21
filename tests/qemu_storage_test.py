#!/usr/bin/env python3
"""BOOK8 storage write test under QEMU.

Attaches scratch disks (sector 0 = "BOOK8SCRATCH"; the kernel's destructive
write self-test runs only on such disks) and checks:
  * read self-tests (bus-master DMA and AHCI multi-sector) pass,
  * write self-tests pass (DMA write + flush + read-back compare, injected
    bus-master/timeout/device faults recover via retry or PIO fallback, and a
    200-sector chunked write),
  * no unexpected DMA failure messages,
  * the scratch images, inspected from the host after QEMU exits, hold exactly
    the final pattern and untouched guard/neighbour sectors.

Profiles:
  generic    default machine, PIIX IDE (bus master) + ich9-ahci
  target-pc  i440fx ("pc") modelling the GA-78LMT/SB710 layout: PIIX IDE with
             bus master plus an explicit ICH9 AHCI controller at 00:11.0
             (the SB7x0 SATA device number)
  target-q35 q35: the ICH9 AHCI controller is the only storage controller, so
             every disk (including the scratch disks) goes through AHCI; the
             CD-ROM occupies ide.2

usage: qemu_storage_test.py <book8.iso> <data.img> [profile]
"""
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SECTOR = 512
SCRATCH_SECTORS = 2048
MAGIC = b"BOOK8SCRATCH"
BASE = 64
FULL = 200
FIRST = BASE + 1

# (regex, minimum number of matching lines)
IDE_AND_AHCI = [
    (r"STORAGE: IDE0-M ATA bus-master DMA selftest PASS", 1),
    (r"STORAGE: IDE0-S ATA bus-master DMA selftest PASS", 1),
    (r"STORAGE: IDE0-S write selftest PASS", 1),
    (r"STORAGE: SATA\d+ AHCI multi-sector selftest PASS", 1),
    (r"STORAGE: SATA\d+ write selftest PASS", 1),
]
AHCI_ONLY = [
    (r"STORAGE: SATA\d+ AHCI multi-sector selftest PASS", 3),
    (r"STORAGE: SATA\d+ write selftest PASS", 2),
]
FORBIDDEN = ["selftest FAIL", "DMA read failed", "DMA write failed"]


def pattern(sectors, seed):
    """Mirror of scratch_pattern() in src/kernel.c."""
    return bytes((i * 131 + (i >> 9) * 7 + seed) & 0xFF
                 for i in range(sectors * SECTOR))


def make_scratch(path):
    data = bytearray(SCRATCH_SECTORS * SECTOR)
    data[:len(MAGIC)] = MAGIC
    path.write_bytes(data)


def check_scratch(path):
    """Final on-disk state after the kernel's write self-test."""
    data = path.read_bytes()
    errors = []

    def sector(lba):
        return data[lba * SECTOR:(lba + 1) * SECTOR]

    if len(data) != SCRATCH_SECTORS * SECTOR:
        errors.append("image size changed")
    if sector(0) != MAGIC + bytes(SECTOR - len(MAGIC)):
        errors.append("sector 0 (magic) modified")
    if data[SECTOR:BASE * SECTOR] != bytes((BASE - 1) * SECTOR):
        errors.append("sectors below guard modified")
    if sector(BASE) != b"\xa5" * SECTOR:
        errors.append("low guard sector corrupted")
    if data[FIRST * SECTOR:(FIRST + FULL) * SECTOR] != pattern(FULL, 0x3C):
        errors.append("final data pattern mismatch")
    if sector(FIRST + FULL) != b"\x5a" * SECTOR:
        errors.append("high guard sector corrupted")
    tail = data[(FIRST + FULL + 1) * SECTOR:]
    if tail != bytes(len(tail)):
        errors.append("sectors above guard modified")
    return errors


def evaluate(text, required):
    """Returns (missing, forbidden) lists for a serial log."""
    missing = [f"{rx} x{n}" for rx, n in required
               if len(re.findall(rx, text)) < n]
    bad = [f for f in FORBIDDEN if f in text]
    return missing, bad


def storage_lines(text):
    return [l.strip() for l in text.splitlines() if l.startswith("STORAGE:")]


def qemu_args(profile, iso, serial, image, ide_scratch, sata_scratch):
    base = ["qemu-system-x86_64", "-m", "256", "-display", "none",
            "-cdrom", str(iso), "-boot", "d", "-serial", f"file:{serial}"]
    data = f"file={image},format=raw,snapshot=on"
    if profile == "generic":
        return base + [
            "-drive", f"{data},if=ide,index=0",
            "-drive", f"file={ide_scratch},format=raw,if=ide,index=1",
            "-drive", f"file={sata_scratch},format=raw,if=none,id=sata0",
            "-device", "ich9-ahci,id=ahci",
            "-device", "ide-hd,drive=sata0,bus=ahci.0"]
    if profile == "target-pc":
        return base + [
            "-machine", "pc",
            "-drive", f"{data},if=ide,index=0",
            "-drive", f"file={ide_scratch},format=raw,if=ide,index=1",
            "-drive", f"file={sata_scratch},format=raw,if=none,id=sata0",
            "-device", "ich9-ahci,id=ahci,bus=pci.0,addr=0x11",
            "-device", "ide-hd,drive=sata0,bus=ahci.0"]
    if profile == "target-q35":
        return base + [
            "-machine", "q35",
            "-drive", f"{data},if=none,id=d0",
            "-drive", f"file={ide_scratch},format=raw,if=none,id=d1",
            "-drive", f"file={sata_scratch},format=raw,if=none,id=d2",
            "-device", "ide-hd,drive=d0,bus=ide.0",
            "-device", "ide-hd,drive=d1,bus=ide.1",
            "-device", "ide-hd,drive=d2,bus=ide.3"]
    raise SystemExit(f"unknown profile {profile}")


def run_qemu(profile, iso, image, timeout=120):
    """Returns (serial text, missing, forbidden, image errors)."""
    required = AHCI_ONLY if profile == "target-q35" else IDE_AND_AHCI
    with tempfile.TemporaryDirectory(prefix="book8-storage-") as temp:
        temp = Path(temp)
        serial = temp / "serial.log"
        ide_scratch = temp / "scratch-ide.img"
        sata_scratch = temp / "scratch-sata.img"
        make_scratch(ide_scratch)
        make_scratch(sata_scratch)
        process = subprocess.Popen(
            qemu_args(profile, iso, serial, image, ide_scratch, sata_scratch),
            stderr=subprocess.DEVNULL)
        text = ""
        try:
            deadline = time.time() + timeout
            while time.time() < deadline:
                time.sleep(2)
                if serial.exists():
                    text = serial.read_text(errors="replace")
                    if not evaluate(text, required)[0]:
                        break
        finally:
            process.terminate()
            process.wait()
        missing, bad = evaluate(text, required)
        image_errors = [f"{p.name}: {e}"
                        for p in (ide_scratch, sata_scratch)
                        for e in check_scratch(p)]
    return text, missing, bad, image_errors


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: qemu_storage_test.py <book8.iso> <data.img> [profile]")
    iso = Path(sys.argv[1]).resolve()
    image = Path(sys.argv[2]).resolve()
    profile = sys.argv[3] if len(sys.argv) == 4 else "generic"
    text, missing, bad, image_errors = run_qemu(profile, iso, image)
    print("\n".join(storage_lines(text)))
    if missing or bad or image_errors:
        raise SystemExit(f"FAIL: storage test [{profile}] missing={missing} "
                         f"forbidden={bad} image={image_errors}")
    print(f"PASS: storage test [{profile}] "
          "(read, write, fault recovery, host image check)")


if __name__ == "__main__":
    main()
