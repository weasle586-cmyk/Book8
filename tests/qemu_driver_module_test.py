#!/usr/bin/env python3
"""Verify trusted NVIDIA, RTL8168, USB, and audio modules on FAT test disks."""

import socket
import subprocess
import sys
from pathlib import Path

import qemu_amd_sb710_profile as amd


SUCCESS_MARKER = "NVDRV: module loaded and initialized; no matching GPU found"
DIGEST_MARKER = "NVDRV: module digest verified, symbols resolved and init complete"
FAILURE_MARKER = "NVDRV: trusted module unavailable"
TRUSTED_MODULE_MARKERS = (
    "BOOK8-DRIVER: loaded A:\\DRIVERS\\NVMAXWEL.DRV",
    "BOOK8-DRIVER: loaded A:\\DRIVERS\\RTL8168.DRV",
    "BOOK8-DRIVER: loaded A:\\DRIVERS\\USBHOST.DRV",
    "BOOK8-DRIVER: loaded A:\\DRIVERS\\AC97.DRV",
    "BOOK8-USB: module initialized",
    "BOOK8-RTL8168: module loaded; no matching NIC found",
    "BOOK8-HDA: module loaded; no matching controller found",
)


def check_image(iso, image, log_path):
    log_path.write_text("", encoding="utf-8")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    args = amd.qemu_args(iso, log_path, port, "none")
    args += ["-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on"]
    process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
    try:
        if not amd.wait_for(log_path, SUCCESS_MARKER, 120):
            output = log_path.read_text(errors="replace") if log_path.exists() else ""
            raise SystemExit(
                f"FAIL: {image.name}: trusted NVIDIA module did not load\n"
                f"{output[-2000:]}"
            )
        output = log_path.read_text(errors="replace")
        if FAILURE_MARKER in output or DIGEST_MARKER not in output:
            raise SystemExit(
                f"FAIL: {image.name}: module verification markers are inconsistent\n"
                f"{output[-2000:]}"
            )
        missing = [marker for marker in TRUSTED_MODULE_MARKERS
            if marker not in output]
        if missing or "BOOK8-DRIVER: load failed" in output:
            raise SystemExit(
                f"FAIL: {image.name}: missing trusted module status: {missing}\n"
                f"{output[-3000:]}"
            )
        if output.index(DIGEST_MARKER) > output.index(SUCCESS_MARKER):
            raise SystemExit(f"FAIL: {image.name}: module success markers out of order")
        print(
            f"[{image.name}] trusted NVIDIA/NIC/USB/audio load and "
            "absent-hardware "
            "degradation: PASS"
        )
    finally:
        process.terminate()
        process.wait(timeout=10)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: qemu_driver_module_test.py <book8.iso> <fat16.img> <fat32.img>"
        )
    iso, fat16, fat32 = (Path(arg).resolve() for arg in sys.argv[1:])
    for path in (iso, fat16, fat32):
        if not path.is_file():
            raise SystemExit(f"FAIL: expected regular test file: {path}")
    logs = iso.parent / "qemu-driver-logs"
    logs.mkdir(parents=True, exist_ok=True)
    check_image(iso, fat16, logs / "fat16-serial.log")
    check_image(iso, fat32, logs / "fat32-serial.log")
    print("PASS: trusted driver module on FAT16 and FAT32")


if __name__ == "__main__":
    main()
