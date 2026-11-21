#!/usr/bin/env python3
"""Check that NVMAP.CFG cannot trigger boot diagnostics on disposable disks.

usage: qemu_nvmap_startup_test.py <book8-cli.iso> <book8-data16.img>
"""
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import zlib

import qemu_amd_sb710_profile as amd


def partition_image(image, layout):
    """Wrap only this temporary FAT image, never a physical drive."""
    data = image.read_bytes()
    start = 2048
    sectors = len(data) // 512
    total = start + sectors + 2048
    mbr = bytearray(512)
    mbr[510:] = b"\x55\xaa"
    if layout == "efi-mbr":
        mbr[446:462] = struct.pack("<B3sB3sII", 0x80, bytes(3), 0xef, bytes(3),
                                  start, sectors)
    else:
        mbr[446:462] = struct.pack("<B3sB3sII", 0, bytes(3), 0xee, bytes(3),
                                  1, total - 1)
    with image.open("wb") as disk:
        disk.truncate(total * 512)
        disk.write(mbr)
        if layout == "gpt":
            entries = bytearray(128 * 128)
            # EFI system partition GUID, little-endian GPT byte order.
            entries[:16] = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
            entries[16:32] = bytes(range(1, 17))
            struct.pack_into("<QQ", entries, 32, start, start + sectors - 1)
            header = bytearray(512)
            struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0,
                             b"EFI PART", 0x10000, 92, 0, 0, 1, total - 1,
                             34, total - 34, bytes(range(16)), 2, 128, 128,
                             zlib.crc32(entries))
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            disk.seek(512)
            disk.write(header)
            disk.write(entries)
            backup = bytearray(header)
            struct.pack_into("<I", backup, 16, 0)
            struct.pack_into("<QQ", backup, 24, total - 1, 1)
            struct.pack_into("<Q", backup, 72, total - 33)
            struct.pack_into("<I", backup, 16, zlib.crc32(backup[:92]))
            disk.seek((total - 33) * 512)
            disk.write(entries)
            disk.write(backup)
        disk.seek(start * 512)
        disk.write(data)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_nvmap_startup_test.py <book8-cli.iso> <data.img>")
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("FAIL: expected regular ISO and disposable generated FAT image")
    cases = (
        ("lf-enabled", b"1\n", False),
        ("crlf-enabled", b"1\r\n", False),
        ("disabled", b"0\n", False),
        ("absent", None, False),
        ("invalid", b"1\nextra\n", False),
        ("missing-elf", b"1\n", True),
        ("efi-mbr", b"1\n", False),
        ("gpt", b"1\n", False),
    )
    with tempfile.TemporaryDirectory(prefix="book8-nvmap-startup-") as temp:
        root = Path(temp)
        for name, config, missing_elf in cases:
            image = root / f"{name}.img"
            shutil.copyfile(source, image)
            if config is None:
                subprocess.run(["mdel", "-i", str(image), "::/NVMAP.CFG"], check=True)
            else:
                setting = root / "nvmap.cfg"
                setting.write_bytes(config)
                subprocess.run(["mcopy", "-o", "-i", str(image),
                                str(setting), "::/NVMAP.CFG"], check=True)
            if missing_elf:
                subprocess.run(["mdel", "-i", str(image), "::/NVMAP.ELF"], check=True)
            if name in ("efi-mbr", "gpt"):
                partition_image(image, name)
            serial = root / f"{name}.log"
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            args = amd.qemu_args(iso, serial, port, "none")
            args += ["-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on"]
            process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
            try:
                if not amd.wait_for(serial, "BOOK8-DIAG: M2 prompt printed", 90):
                    raise SystemExit(f"FAIL: {name}: did not reach prompt")
                log = serial.read_text(errors="replace")
                if ("BOOK8-NVMAP:" in log or "NVMAP:" in log or "BOOK8-PUSH:" in log
                        or "BOOK8-DESKTOP: TASKBAR" in log):
                    raise SystemExit(f"FAIL: {name}: boot started disabled diagnostics or push")
                print(f"NVMAP startup {name}: PASS")
            finally:
                process.terminate()
                process.wait(timeout=10)
    print("PASS: NVMAP startup disabled for all external configurations")


if __name__ == "__main__":
    main()
