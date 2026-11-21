#!/usr/bin/env python3
"""Boot BOOK8 under QEMU, then restart it in RAM from a kernel ELF on A:.

Exercises the same path a pushed kernel takes (kernel/relaunch.c staging plan,
src/relaunch_stub.S trampoline, Multiboot2 re-entry with a rewritten MBI)
and checks the relaunched kernel still runs Ring 3 programs from disk.

The data image is copied to a temporary file first; the original is not
modified.

usage: qemu_relaunch_test.py <book8-cli.iso> <data.img> <book8.elf>
"""
import os
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
    for ch in text + "\n":
        monitor.command(f"sendkey {KEYMAP.get(ch, ch)}", 0.1)


def wait_new(serial, start, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in serial.read_text(errors="replace")[start:]:
            return True
        time.sleep(0.5)
    return False


def cpu_state(port):
    """Returns QEMU's register dump for CPU 0 (diagnostics for hangs)."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            time.sleep(0.3)
            sock.recv(65536)
            sock.sendall(b"info registers\n")
            time.sleep(1.0)
            sock.settimeout(1.0)
            chunks = []
            try:
                while True:
                    data = sock.recv(65536)
                    if not data:
                        break
                    chunks.append(data)
            except socket.timeout:
                pass
            return b"".join(chunks).decode(errors="replace")
    except OSError as error:
        return f"(monitor unavailable: {error})"


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: qemu_relaunch_test.py <book8-cli.iso> <data.img> <book8.elf>")
    iso, source_image, kernel = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="book8-relaunch-") as temp:
        image = Path(temp) / "data.img"
        shutil.copyfile(source_image, image)
        subprocess.run(["mcopy", "-o", "-i", str(image), str(kernel), "::/BOOK8.ELF"],
                       check=True)
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = amd.qemu_args(iso, serial, port, "none")
        args += ["-drive", f"file={image},format=raw,if=ide,index=0"]
        debug_log = os.environ.get("BOOK8_QEMU_DEBUG_LOG")
        if debug_log:
            args += ["-d", "int,cpu_reset,guest_errors", "-D", debug_log]
        exited_early = False
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        monitor = QemuMonitor(port, process)
        failures = []
        registers = ""
        try:
            if not amd.wait_for(serial, READY, 90):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            time.sleep(2)
            initial = serial.read_text(errors="replace")
            if "BOOK8-SMP: online CPUs=8" not in initial:
                failures.append("expected 8 online CPUs before relaunch")
            start = len(initial)
            type_line(monitor, "relaunch book8.elf")
            if not wait_new(serial, start, "BOOK8-RELAUNCH: relaunched kernel #1", 180):
                failures.append("relaunched kernel did not report relaunch #1")
            elif not wait_new(serial, start, READY, 180):
                failures.append("relaunched kernel did not reach the interpreter")
            else:
                time.sleep(2)
                exec_start = len(serial.read_text(errors="replace"))
                type_line(monitor, "exec hello.elf")
                if not wait_new(serial, exec_start, "EXEC: hello.elf exited with status 7", 120):
                    failures.append("Ring 3 exec failed after relaunch")
            log = serial.read_text(errors="replace")
            after = log[start:]
            if "BOOK8-SMP: online CPUs=8" not in after:
                failures.append("relaunched kernel did not restart 8 CPUs")
            for needle in ("RELAUNCH: A:\\book8.elf", "segments", "relaunch #1",
                           "BOOK8-RELAUNCH: quiescing DMA/interrupts"):
                if needle.lower() not in after.lower():
                    failures.append(f"missing {needle!r}")
            if after.count(READY) < 1:
                failures.append("interpreter marker missing after relaunch")
            for bad in ("RELAUNCH: cannot", "RELAUNCH: failed", "PANIC"):
                if bad in after:
                    failures.append(f"unexpected {bad!r}")
            registers = cpu_state(port) if failures and process.poll() is None else ""
            exited_early = process.poll() is not None
        finally:
            process.terminate()
            process.wait()
            tail = serial.read_text(errors="replace")[-1500:] if serial.exists() else ""
    if failures:
        print("--- serial tail:\n" + tail)
        if exited_early:
            print(f"--- QEMU exited by itself (status {process.returncode}); with "
                  "-no-reboot this usually means a triple fault. Set "
                  "BOOK8_QEMU_DEBUG_LOG=<path> to capture -d int,cpu_reset.")
        if registers:
            print("--- CPU 0 state:\n" + registers[-2500:])
        for failure in failures:
            print("FAIL:", failure)
        raise SystemExit("FAIL: relaunch test")
    print("PASS: relaunch test (in-RAM Multiboot2 restart + Ring 3 exec)")


if __name__ == "__main__":
    main()
