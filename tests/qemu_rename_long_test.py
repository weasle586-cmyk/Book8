#!/usr/bin/env python3
"""Rename a FAT directory to an ASCII long name on a disposable disk."""
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_amd_sb710_profile as amd
from qemu_desktop_test import QemuMonitor, available_port, wait_for_text


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_rename_long_test.py <cli.iso> <disposable-data.img>")
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("Expected regular ISO and disposable data image")
    with tempfile.TemporaryDirectory(prefix="book8-rename-") as temp:
        disk = Path(temp) / "scratch.img"
        serial = Path(temp) / "serial.log"
        shutil.copyfile(source, disk)
        expected = subprocess.run(
            ["mtype", "-i", str(disk), "::/DATA/MSG.TXT"],
            check=True, capture_output=True,
        ).stdout
        port = available_port()
        process = subprocess.Popen(
            amd.qemu_args(iso, serial, port, "none")
            + ["-drive", f"file={disk},format=raw,if=ide,index=0"],
            stderr=subprocess.DEVNULL,
        )
        monitor = None
        try:
            log = wait_for_text(serial, "USB descriptor failures", 90)
            match = re.search(r"BOOK8 framebuffer handoff:\s*(\d+)x(\d+)x", log)
            if match is None:
                raise AssertionError("framebuffer dimensions unavailable")
            width, height = map(int, match.groups())
            monitor = QemuMonitor(port, process)
            monitor.type_text("book8gui")
            wait_for_text(serial, "BOOK8-DESKTOP: TASKBAR", 20)
            monitor.set_screen_size(width, height)
            monitor.reset_pointer()
            monitor.click(20, height - 18)
            monitor.click(40, height - 90)
            monitor.click(200, height - 110)
            wait_for_text(serial, 'title="File Explorer" registered', 10)
            monitor.move_to(60, 131)
            monitor.command("mouse_button 2", 0.1)
            monitor.command("mouse_button 0", 0.1)
            monitor.click(65, 171)
            for _ in range(4):
                monitor.command("sendkey backspace", 0.15)
            for key in ["l", "o", "n", "g", "minus", "f", "o", "l", "d", "e", "r"]:
                monitor.command(f"sendkey {key}", 0.15)
            monitor.command("sendkey ret", 0.2)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if serial.read_text(errors="replace").count("BOOK8-EXPLORER: path=A:\\") >= 2:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Rename did not refresh Explorer")
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            process.wait(timeout=10)
        result = subprocess.run(
            ["mtype", "-i", str(disk), "::/long-folder/MSG.TXT"],
            check=True, capture_output=True,
        ).stdout
        old = subprocess.run(["mdir", "-i", str(disk), "::/DATA"], capture_output=True)
        if result != expected or old.returncode == 0:
            raise AssertionError("Rename did not preserve the directory contents")
        print("PASS: Explorer renamed a FAT directory to a long name")


if __name__ == "__main__":
    main()
