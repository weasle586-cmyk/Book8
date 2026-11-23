#!/usr/bin/env python3
"""Open the test FAT image's NVMAP.CFG from Explorer's context menu."""
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_amd_sb710_profile as amd
import qemu_ui_lookup as ui
from qemu_desktop_test import QemuMonitor, available_port, wait_for_text


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_explorer_context_test.py <cli.iso> <disposable-data.img>")
    iso, image = (Path(arg).resolve() for arg in sys.argv[1:])
    if not iso.is_file() or not image.is_file():
        raise SystemExit("FAIL: expected regular ISO and disposable data image")
    with tempfile.TemporaryDirectory(prefix="book8-context-") as temp:
        serial = Path(temp) / "serial.log"
        port = available_port()
        args = amd.qemu_args(iso, serial, port, "none")
        args += ["-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on"]
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
            wait_for_text(serial,
                'WINDOW id=1 title="BOOK8 Sign In" registered', 20)
            monitor.type_text("default")
            wait_for_text(serial, "BOOK8-SESSION: login accepted uid=0", 20)
            monitor.set_screen_size(width, height)
            monitor.reset_pointer()
            ui.click_program(monitor, serial, height,
                             label="File Explorer", process=process)
            wait_for_text(serial, 'title="File Explorer" registered', 10)
            # The sign-in modal consumes cascade slot 0, so Explorer is at
            # (64,64) rather than its nominal (40,40). Row 8 is NVMAP.CFG
            # at list_y 147 + 8 * 18 px.
            monitor.move_to(89, 299)
            monitor.command("mouse_button 2", 0.1)
            monitor.command("mouse_button 0", 0.1)
            monitor.click(94, 220)
            log = wait_for_text(serial, "BOOK8-NOTEEDITOR: opened path=A:\\NVMAP.CFG", 10)
            if 'title="Command Shell" registered' in log:
                raise AssertionError("Explorer launched the command shell")
            monitor.command("sendkey esc")
            time.sleep(0.5)
            if "BOOK8GUI closed." in serial.read_text(errors="replace"):
                raise AssertionError("Escape unexpectedly closed the desktop")
            monitor.click(20, height - 18)
            monitor.click(40, height - 50)
            monitor.click(200, height - 50)
            wait_for_text(serial, "BOOK8GUI closed.", 10)
            print("PASS: Explorer context Open, editor fallback and explicit desktop exit")
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
