#!/usr/bin/env python3
"""Run the dual QEMU baseline: Intel G41/ICH7 and AMD 760G/SB710 profiles.

usage: run_baseline_profiles.py <book8.iso>
G41 (standalone-UHCI keyboard) must enumerate an active boot-HID keyboard;
SB710 must report 8/0/5/2/1 controllers with the PS/2-translated keyboard
working and no BOOK8-driven USB HID (OHCI/EHCI is unimplemented). The G41
EHCI-companion keyboard variant is a tracked, undiagnosed gap and is not
asserted here.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(script, iso, variant):
    result = subprocess.run(
        [sys.executable, str(HERE / script), iso, variant],
        capture_output=True, text=True, timeout=300)
    return result.returncode, result.stdout


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_baseline_profiles.py <book8.iso>")
    iso = str(Path(sys.argv[1]).resolve())
    failures = []
    code, out = run("qemu_g41_profile.py", iso, "uhci4")
    if code != 0 or "2/2/1/1/1" not in out:
        failures.append("G41 profile: standalone-UHCI keyboard not active")
    print("G41/ICH7 baseline:", "FAIL" if failures else "PASS")
    for pointer in ("none", "mouse", "tablet"):
        code, out = run("qemu_amd_sb710_profile.py", iso, pointer)
        ok = code == 0 and "PASS" in out
        print(f"AMD SB710 baseline ({pointer}):", "PASS" if ok else "FAIL")
        if not ok:
            failures.append(f"SB710 profile ({pointer})")
    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("dual baseline: PASS")


if __name__ == "__main__":
    main()
