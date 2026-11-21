#!/usr/bin/env python3
"""Verify raw swap page eviction and recovery through the real #PF path."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402

RAM_MIB = 256
SWAP_START_LBA = 2048
SECTORS_PER_MIB = 1024 * 1024 // 512
SWAP_SECTORS = RAM_MIB * SECTORS_PER_MIB


def create_swap_disk(path):
    total_sectors = SWAP_START_LBA + SWAP_SECTORS
    with path.open("wb") as disk:
        disk.truncate(total_sectors * 512)
        mbr = bytearray(512)
        entry = 446
        mbr[entry + 4] = 0x82
        mbr[entry + 8:entry + 12] = SWAP_START_LBA.to_bytes(4, "little")
        mbr[entry + 12:entry + 16] = SWAP_SECTORS.to_bytes(4, "little")
        mbr[510:512] = b"\x55\xaa"
        disk.seek(0)
        disk.write(mbr)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_swap_test.py <book8-cli.iso>")
    iso = Path(sys.argv[1]).resolve()
    if not iso.is_file():
        raise SystemExit(f"FAIL: ISO not found: {iso}")

    with tempfile.TemporaryDirectory(prefix="book8-swap-") as temp:
        temp_path = Path(temp)
        serial = temp_path / "serial.log"
        disk = temp_path / "raw-swap.img"
        create_swap_disk(disk)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        args = amd.qemu_args(iso, serial, port, "none")
        memory_index = args.index("-m") + 1
        args[memory_index] = str(RAM_MIB)
        args += ["-drive", f"file={disk},format=raw,if=ide,index=0"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        try:
            if not amd.wait_for(serial, "BOOK8-SWAP: raw partition enabled", 90):
                raise SystemExit("FAIL: BOOK8 did not discover the disposable swap partition")
            if not amd.wait_for(serial, "USB descriptor failures", 30):
                raise SystemExit("FAIL: BOOK8 did not reach command input")
            boot_log = serial.read_text(errors="replace")
            if "BOOK8-SWAP: raw partition enabled, slots=" not in boot_log:
                raise SystemExit("FAIL: swap initialization marker missing")
            for key in "swaptest":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            if not amd.wait_for(serial, "SWAPTEST:", 30):
                raise SystemExit("FAIL: SWAPTEST timed out")
            result = serial.read_text(errors="replace")
            if "SWAPTEST: PASS" not in result:
                raise SystemExit("FAIL: raw swap eviction/page-in failed:\n"
                    + result[-3000:])
            print("PASS: raw partition discovery, page eviction and #PF page-in")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
