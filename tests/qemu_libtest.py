#!/usr/bin/env python3
"""Boot BOOK8 on the SB710 QEMU profile (PS/2 keyboard) and run LIBTEST.

Verifies the freestanding libc self-test passes inside the real kernel
(no SSE, kernel memory layout), not just in the host build check.

usage: qemu_libtest.py <book8.iso>
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_libtest.py <book8.iso>")
    iso = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="book8-libtest-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        process = subprocess.Popen(amd.qemu_args(iso, serial, port, "none"),
                                   stderr=subprocess.DEVNULL)
        try:
            if not amd.wait_for(serial, "USB descriptor failures", 90):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            time.sleep(2)
            for key in ("l", "i", "b", "t", "e", "s", "t", "ret"):
                amd.monitor(port, f"sendkey {key}")
            ok = amd.wait_for(serial, "libc self-test:", 30)
            time.sleep(1)
            log = serial.read_text(errors="replace")
        finally:
            process.terminate()
            process.wait()
    line = next((l for l in log.splitlines() if "libc self-test:" in l), "")
    print(line or "(no self-test output)")
    for l in log.splitlines():
        if "FAIL line" in l:
            print(l)
    if not ok or not line.rstrip().endswith("0 failed"):
        raise SystemExit("FAIL: in-kernel libc self-test did not pass")
    print("PASS: in-kernel libc self-test")


if __name__ == "__main__":
    main()
