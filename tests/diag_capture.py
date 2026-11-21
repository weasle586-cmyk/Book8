#!/usr/bin/env python3
"""Boot a QEMU profile and print the BOOK8 register-level DIAG serial lines.

usage: diag_capture.py <book8.iso> <g41|sb710> [variant]
"""
import sys
import tempfile
import subprocess
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402
import qemu_g41_profile as g41  # noqa: E402


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: diag_capture.py <book8.iso> <g41|sb710> "
                         "[variant]")
    iso = Path(sys.argv[1]).resolve()
    module = g41 if sys.argv[2] == "g41" else amd
    variant = sys.argv[3] if len(sys.argv) > 3 else (
        "ehci" if module is g41 else "none")
    serial = Path(tempfile.mkdtemp(prefix="book8-diag-")) / "serial.log"
    process = subprocess.Popen(module.qemu_args(iso, serial, 45555, variant),
                               stderr=subprocess.DEVNULL)
    try:
        module.wait_for(serial, "BOOK8-DIAG: P ps2 polled", 90)
        time.sleep(2)
    finally:
        process.terminate()
        process.wait()
    for line in serial.read_text(errors="replace").splitlines():
        if "DIAG" in line:
            print(line)


if __name__ == "__main__":
    main()
