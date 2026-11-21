#!/usr/bin/env python3
"""Move a FAT directory tree through Explorer on a disposable disk copy."""
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
        raise SystemExit("usage: qemu_clipboard_tree_test.py <cli.iso> <disposable-data.img>")
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("Expected regular ISO and disposable data image")
    with tempfile.TemporaryDirectory(prefix="book8-tree-") as temp:
        disk = Path(temp) / "scratch.img"
        serial = Path(temp) / "serial.log"
        shutil.copyfile(source, disk)
        subprocess.run(["mmd", "-i", str(disk), "::/TARGET"], check=True)
        long_file = Path(temp) / "long-example-file.txt"
        long_file.write_bytes(b"BOOK8 VFAT copy test\n")
        subprocess.run(["mcopy", "-i", str(disk), str(long_file),
                        "::/DATA/long-example-file.txt"], check=True)
        subprocess.run(["mmd", "-i", str(disk),
                        "::/DATA/long-example-directory"], check=True)
        subprocess.run(["mcopy", "-i", str(disk), str(long_file),
                        "::/DATA/long-example-directory/long-example-file.txt"],
                       check=True)
        expected = subprocess.run(
            ["mtype", "-i", str(disk), "::/DATA/MSG.TXT"],
            check=True, capture_output=True,
        ).stdout
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
            monitor.click(65, 207)
            monitor.click(60, 221)
            monitor.click(60, 221)
            wait_for_text(serial, "BOOK8-EXPLORER: path=A:\\TARGET\\", 10)
            monitor.move_to(60, 200)
            monitor.command("mouse_button 2", 0.1)
            monitor.command("mouse_button 0", 0.1)
            monitor.click(65, 299)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if serial.read_text(errors="replace").count(
                    "BOOK8-EXPLORER: path=A:\\TARGET\\"
                ) >= 2:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Paste did not refresh the target directory")
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            process.wait(timeout=10)
        actual = subprocess.run(
            ["mtype", "-i", str(disk), "::/TARGET/DATA/MSG.TXT"],
            check=True, capture_output=True,
        ).stdout
        original = subprocess.run(
            ["mdir", "-i", str(disk), "::/DATA"], capture_output=True,
        )
        if actual != expected or original.returncode == 0:
            raise AssertionError("Move failed to preserve bytes or remove the source")
        moved_long = subprocess.run(
            ["mtype", "-i", str(disk), "::/TARGET/DATA/long-example-file.txt"],
            check=True, capture_output=True,
        ).stdout
        if moved_long != long_file.read_bytes():
            raise AssertionError("Long-name file was not preserved")
        nested_long = subprocess.run(
            ["mtype", "-i", str(disk),
             "::/TARGET/DATA/long-example-directory/long-example-file.txt"],
            check=True, capture_output=True,
        ).stdout
        if nested_long != long_file.read_bytes():
            raise AssertionError("Long-name directory was not preserved")
        print("PASS: Explorer moved a FAT directory tree and preserved its file bytes")


if __name__ == "__main__":
    main()
