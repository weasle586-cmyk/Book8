#!/usr/bin/env python3
"""Verify diagnostic initialization logging on disposable FAT images only.

usage: qemu_boot_log_test.py <diagnostic.iso> <disposable-data.img> [logs]
"""
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("Diagnostic ISO and disposable data image must be regular files")
    kernel = iso.parent / "book8.elf"
    marker = iso.parent / "NVLOG.OK"
    if not kernel.is_file() or marker.read_bytes() != b"BOOK8-NVLOG-1\r\n":
        raise SystemExit("Expected diagnostic kernel and exact opt-in marker alongside ISO")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "boot-log-validation"
    logs.mkdir(parents=True, exist_ok=True)
    for opted_in in (False, True):
        name = "marked" if opted_in else "unmarked"
        with tempfile.TemporaryDirectory(prefix="book8-nvlog-") as temp:
            disk = Path(temp) / "scratch.img"
            shutil.copyfile(source, disk)
            subprocess.run(["mmd", "-i", str(disk), "::/BOOT"], check=True)
            subprocess.run(["mcopy", "-i", str(disk), str(kernel), "::/BOOT/BOOK8.ELF"], check=True)
            stale = Path(temp) / "nvinfo.txt"
            stale.write_bytes(b"STALE PREVIOUS BOOT\r\n")
            subprocess.run(["mcopy", "-i", str(disk), str(stale), "::/NVINFO.TXT"], check=True)
            if opted_in:
                subprocess.run(["mcopy", "-i", str(disk), str(marker), "::/NVLOG.OK"], check=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            process, _, serial = panel.boot(iso, disk, logs, name)
            try:
                smoke.wait_for(serial, process, "BOOK8-DIAG: M2 prompt printed")
                text = serial.read_text(errors="strict")
                expected = "BOOK8-NVLOG: saved A:\\nvinfo.txt" if opted_in else "BOOK8-NVLOG: ERROR No opted-in"
                if expected not in text:
                    raise AssertionError(f"Missing diagnostic outcome {expected}; see {serial}")
                if opted_in and "BOOK8-NVLOG: ERROR" in text:
                    raise AssertionError(f"Unexpected persistence error; see {serial}")
            finally:
                panel.stop(process)
            data = subprocess.run(["mtype", "-i", str(disk), "::/NVINFO.TXT"],
                                  check=True, capture_output=True).stdout
            if not opted_in:
                if data != stale.read_bytes() or hashlib.sha256(disk.read_bytes()).digest() != before:
                    raise AssertionError("Diagnostic boot modified an unmarked disk")
                print("PASS: unmarked FAT boot volume unchanged (whole-image SHA-256)")
                continue
            terminal = b"BOOK8-NVLOG: END initialization; entering shell\r\n"
            wire = serial.read_bytes()
            start = wire.index(b"BOOK8-BOOT: kernel_main entered\r\n")
            end = wire.index(terminal, start) + len(terminal)
            expected = bytes(c for c in wire[start:end] if c in (9, 10, 13) or 32 <= c < 127)
            if data != expected:
                raise AssertionError("nvinfo.txt does not exactly match the early serial initialization stream")
            for required in (b"PCIE1 INIT", b"PCIE1 ENUM", b"BEGIN PCIe target preparation",
                             b"BEGIN NVIDIA probe", b"END NVIDIA probe", terminal):
                if required not in data:
                    raise AssertionError(f"Missing initialization marker: {required!r}")
            (logs / "nvinfo.txt").write_bytes(data)
            print(f"PASS: marked FAT boot volume, exact {len(data)}-byte CRLF initialization log, old log replaced")
    print(f"PASS: QEMU diagnostic boot logging; logs: {logs}")


if __name__ == "__main__":
    main()
