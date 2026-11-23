#!/usr/bin/env python3
"""Verify boot hash enforcement under QEMU.

Boots the CLI ISO with its generated book8-hashes module, executes an
unmodified ELF, then executes a deliberately corrupted ELF and checks that the
kernel rejects the bad bytes before parsing them.

usage: qemu_boot_hash_test.py <book8-cli.iso> <data.img>
"""
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402
from qemu_desktop_test import QemuMonitor  # noqa: E402

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "\n": "ret"}
READY = "USB descriptor failures"


def type_line(monitor, text):
    for character in text + "\n":
        monitor.command(f"sendkey {KEYMAP.get(character, character)}", 0.1)


def wait_new(serial, start, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in serial.read_text(errors="replace")[start:]:
            return True
        time.sleep(0.5)
    return False


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_boot_hash_test.py <book8-cli.iso> <data.img>")
    iso, source_image = (Path(value).resolve() for value in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="book8-boot-hash-") as temp:
        temp_path = Path(temp)
        image = temp_path / "data.img"
        shutil.copyfile(source_image, image)
        elf = temp_path / "HELLO.ELF"
        subprocess.run(
            ["mcopy", "-o", "-i", str(image), "::/HELLO.ELF", str(elf)],
            check=True,
        )
        corrupted = bytearray(elf.read_bytes())
        if len(corrupted) < 64:
            raise SystemExit("FAIL: HELLO.ELF is too small to corrupt safely")
        corrupted[32] ^= 0xff
        elf.write_bytes(corrupted)
        subprocess.run(
            ["mcopy", "-o", "-i", str(image), str(elf), "::/HELLO.ELF"],
            check=True,
        )

        serial = temp_path / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = amd.qemu_args(iso, serial, port, "none")
        args += ["-drive", f"file={image},format=raw,if=ide,index=0"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        monitor = QemuMonitor(port, process)
        failures = []
        try:
            if not amd.wait_for(serial, READY, 90):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            log = serial.read_text(errors="replace")
            match = re.search(r"BOOK8-HASH: manifest loaded (\d+) entries", log)
            if match is None or int(match.group(1)) == 0:
                failures.append("boot hash manifest was not loaded")
            if "BOOK8-HASH: no usable manifest module" in log:
                failures.append("boot hash manifest module missing")

            start = len(log)
            type_line(monitor, "exec nvmap.elf")
            if not wait_new(serial, start,
                            "EXEC: nvmap.elf exited with status 0", 60):
                failures.append("unmodified ELF did not execute")

            start = len(serial.read_text(errors="replace"))
            type_line(monitor, "exec hello.elf")
            if not wait_new(serial, start,
                            "BOOK8-HASH: verification failed for HELLO.ELF", 60):
                failures.append("corrupted ELF did not report a hash failure")
            if not wait_new(serial, start,
                            "EXEC: cannot execute hello.elf (error 1)", 60):
                failures.append("corrupted ELF was not rejected")
            tail = serial.read_text(errors="replace")[start:]
            if "HELLO: running at CPL3" in tail:
                failures.append("corrupted ELF executed despite hash failure")
        finally:
            process.terminate()
            process.wait()

    if failures:
        print("--- failures:")
        for failure in failures:
            print(f"  {failure}")
        raise SystemExit("FAIL: boot hash QEMU test")
    print("PASS: boot hash manifest, clean exec and corrupted-ELF rejection")


if __name__ == "__main__":
    main()
