#!/usr/bin/env python3
"""Exercise CPL0 safe MSR reads and #GP recovery without attaching disks.

usage: qemu_msr_guard_test.py <book8-cli.iso>
"""
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

import qemu_amd_sb710_profile as profile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_msr_guard_test.py <book8-cli.iso>")
    iso = Path(sys.argv[1]).resolve()
    if not iso.is_file():
        raise SystemExit("FAIL: expected a regular CLI ISO")
    for name, cpu in (("amd15h", "Opteron_G5"), ("intel", "Nehalem")):
        serial = iso.parent / f"msr-guard-{name}.log"
        serial.unlink(missing_ok=True)
        with tempfile.TemporaryDirectory(prefix="book8-msr-guard-") as temp:
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            args = profile.qemu_args(iso, serial, port, "none")
            args[args.index("-cpu") + 1] = cpu
            with (Path(temp) / "qemu.log").open("w") as errors:
                process = subprocess.Popen(args, stderr=errors)
                try:
                    if not profile.wait_for(serial, "BOOK8-DIAG: M2 prompt printed", 90):
                        raise SystemExit(f"FAIL: {name}: no shell; see {serial}")
                    for char in "msrguardtest":
                        profile.monitor(port, f"sendkey {char}")
                    profile.monitor(port, "sendkey ret")
                    marker = ("MSRGUARDTEST: PASS valid EFER read, #GP recovery, "
                              "output/IF preserved")
                    if not profile.wait_for(serial, marker, 30):
                        raise SystemExit(f"FAIL: {name}: #GP recovery; see {serial}")
                    log = serial.read_text(errors="replace")
                    if "MSRGUARDTEST: FAIL" in log:
                        raise SystemExit(f"FAIL: {name}: state preservation; see {serial}")
                    if ("PCIE1 PMC SKIP unverified MSR c0012121; no PMC prerequisite"
                            not in log or "PCIE1 PMC MSR c0012121" in log):
                        raise SystemExit(f"FAIL: {name}: boot PMC probe not skipped; see {serial}")
                    if "PCIE1 PULSE skipped: NIC unavailable" not in log:
                        raise SystemExit(f"FAIL: missing explicit pulse skip; see {serial}")
                    # Recovery must return to a functional interpreter.
                    profile.monitor(port, "sendkey v")
                    profile.monitor(port, "sendkey e")
                    profile.monitor(port, "sendkey r")
                    profile.monitor(port, "sendkey ret")
                    if not profile.wait_for(serial, "experimental kernel 0.2", 15):
                        raise SystemExit(f"FAIL: {name}: shell unusable after #GP; see {serial}")
                    print(f"PASS: {name} boot PMC skip, #GP recovery, shell continuation; {serial}")
                finally:
                    process.terminate()
                    process.wait(timeout=10)
    print("PASS: QEMU exception-safe MSR guards")


if __name__ == "__main__":
    main()
