#!/usr/bin/env python3
"""Verify the real FAT update code, then cold-boot its disposable output disk.

usage: qemu_persistent_kernel_test.py <old-install.img> <new-kernel.elf>
"""
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def boot(image, serial, expected, bios=None):
    args = [os.environ.get("QEMU", "qemu-system-x86_64"),
            "-m", "256", "-display", "none", "-vga", "std",
            "-drive", f"file={image},format=raw,if=ide,snapshot=on",
            "-serial", f"file:{serial}", "-monitor", "none", "-no-reboot"]
    if bios is not None:
        args += ["-bios", str(bios)]
    process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            log = serial.read_text(errors="replace") if serial.exists() else ""
            if process.poll() is not None:
                raise AssertionError(f"QEMU exited early:\n{log[-2000:]}")
            match = re.search(r"BOOK8-NET: build (\d+)", log)
            if match:
                if int(match.group(1)) != expected:
                    raise AssertionError(f"Cold boot loaded {match.group(1)}, expected {expected}")
                if "USB descriptor failures" in log or "BOOK8-DESKTOP: TASKBAR" in log:
                    return
            time.sleep(0.2)
        raise AssertionError(f"Cold boot did not reach new kernel:\n{log[-2000:]}")
    finally:
        process.terminate()
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_persistent_kernel_test.py <old-install.img> <new-kernel.elf>")
    seed, kernel = (Path(arg).resolve() for arg in sys.argv[1:])
    if not seed.is_file() or not kernel.is_file():
        raise SystemExit("FAIL: expected regular generated installer image and kernel ELF")
    root = Path(__file__).resolve().parent.parent
    expected = int((root / "build" / "build-id.txt").read_text().strip())
    with tempfile.TemporaryDirectory(prefix="book8-persistent-kernel-") as temp:
        disk = Path(temp) / "updated.img"
        subprocess.run([str(root / "build" / "boot_update_host_test"),
                        str(seed), str(kernel), str(disk)], check=True)
        boot(disk, Path(temp) / "bios.log", expected)
        print(f"PASS: BIOS cold boot from persisted kernel build {expected}")
        ovmf = Path("/usr/share/OVMF/OVMF_CODE.fd")
        if not ovmf.is_file():
            raise SystemExit("FAIL: OVMF_CODE.fd is required for UEFI persistence validation")
        boot(disk, Path(temp) / "uefi.log", expected, ovmf)
        print(f"PASS: UEFI cold boot from persisted kernel build {expected}")
    print("PASS: persistent boot kernel update and BIOS/UEFI cold boot")


if __name__ == "__main__":
    main()
