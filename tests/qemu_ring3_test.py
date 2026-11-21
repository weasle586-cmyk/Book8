#!/usr/bin/env python3
"""Boot BOOK8 under QEMU (SB710 profile) and run RING3TEST.

The kernel maps the U/S=1 user pages, drops to Ring 3 with IRETQ, the task
traps back with SYSCALL (register preservation, getpid, write via the libc
__syscall wrapper) and exits through SYS_exit into the kernel.

usage: qemu_ring3_test.py <book8.iso>
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402

KEYS = ("r", "i", "n", "g", "3", "t", "e", "s", "t", "ret")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_ring3_test.py <book8.iso>")
    iso = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="book8-ring3-") as temp:
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
            for key in KEYS:
                amd.monitor(port, f"sendkey {key}")
            amd.wait_for(serial, "RING3TEST:", 30)
            time.sleep(1)
            log = serial.read_text(errors="replace")
        finally:
            process.terminate()
            process.wait()
    for line in log.splitlines():
        if "RING3" in line and "sendkey" not in line:
            print(line.strip())
    if ("RING3: hello from CPL3" not in log
            or "RING3: invalid syscall pointers rejected" not in log
            or "RING3TEST: PASS" not in log):
        raise SystemExit("FAIL: Ring 3 smoke test did not pass")
    print("PASS: Ring 3 smoke test")


if __name__ == "__main__":
    main()
