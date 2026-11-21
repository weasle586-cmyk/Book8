#!/usr/bin/env python3
"""Delete a populated FAT directory with Explorer on a disposable disk."""
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
        raise SystemExit("usage: qemu_delete_tree_test.py <cli.iso> <disposable-data.img>")
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("Expected regular ISO and disposable data image")
    with tempfile.TemporaryDirectory(prefix="book8-delete-") as temp:
        disk = Path(temp) / "scratch.img"
        serial = Path(temp) / "serial.log"
        shutil.copyfile(source, disk)
        subprocess.run(["mdir", "-i", str(disk), "::/DATA/MSG.TXT"],
                       check=True, capture_output=True)
        long_file = Path(temp) / "long-example-file.txt"
        long_file.write_bytes(b"BOOK8 VFAT deletion test\n")
        subprocess.run(["mcopy", "-i", str(disk), str(long_file),
                        "::/DATA/long-example-file.txt"], check=True)
        port = available_port()
        args = amd.qemu_args(iso, serial, port, "none")
        args += ["-drive", f"file={disk},format=raw,if=ide,index=0"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
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
            monitor.click(65, 153)
            monitor.command("sendkey ret", 0.2)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if serial.read_text(errors="replace").count(
                    "BOOK8-EXPLORER: path=A:\\"
                ) >= 2:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Deletion did not refresh Explorer")
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            process.wait(timeout=10)
        result = subprocess.run(["mdir", "-i", str(disk), "::/DATA"],
                                capture_output=True)
        if result.returncode == 0:
            raise AssertionError("Deleted directory is still present")
        print("PASS: Explorer deleted a populated FAT directory with a long-name file")


if __name__ == "__main__":
    main()
