#!/usr/bin/env python3
"""Validate program removal on a disposable FAT copy, never the source image.

usage: qemu_programs_test.py <book8-cli.iso> <data.img> [log-directory]
"""
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke
from qemu_desktop_test import QemuMonitor


def screenshot(monitor, path):
    time.sleep(1)
    monitor.command(f"screendump {path}", 0.3)
    expected = len(f"P6\n{monitor.width} {monitor.height}\n255\n".encode("ascii")) \
        + monitor.width * monitor.height * 3
    deadline = time.monotonic() + 10
    while not path.is_file() or path.stat().st_size < expected:
        if time.monotonic() >= deadline:
            raise AssertionError("Programs applet screenshot did not finish")
        time.sleep(0.1)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    binary = Path(__file__).resolve().parent.parent / "build" / "user" / "hello.elf"
    if not all(path.is_file() for path in (iso, source, binary)):
        raise SystemExit("ISO, disposable source image and built hello.elf are required")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "programs-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-programs-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        subprocess.run(["mmd", "-i", str(disk), "::/BOOT"], check=True)
        for target in ("::/REMOVE.ELF", "::/KEEP.ELF", "::/BOOT/BOOK8.ELF"):
            subprocess.run(["mcopy", "-i", str(disk), str(binary), target], check=True)
        for boot in range(2):
            process, port, serial = panel.boot(iso, disk, logs, f"boot-{boot}")
            monitor = None
            try:
                log = smoke.wait_for(serial, process, "A:\\> ")
                width, height, _ = smoke.verify_framebuffer(log)
                smoke.type_line(port, "book8gui", process)
                smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
                monitor = QemuMonitor(port, process)
                monitor.set_screen_size(width, height)
                monitor.reset_pointer()
                monitor.click(20, height - 18)
                monitor.click(40, height - 70)
                smoke.wait_for(serial, process, 'title="System Settings" registered')
                monitor.command("sendkey pgup", 0.3)
                screenshot(monitor, logs / f"programs-{boot}.ppm")
                if boot == 0:
                    # Built-in entries are immutable even when invoked by keyboard.
                    monitor.command("sendkey delete")
                    for _ in range(7):
                        monitor.command("sendkey down", 0.12)
                    screenshot(monitor, logs / "hexview-details.ppm")
                    color, _, _ = smoke.ppm_pixel(logs / "hexview-details.ppm", 194, 232)
                    if color != (0, 120, 215):
                        raise AssertionError("User utility is not selected in the program list")
                    monitor.command("sendkey delete", 0.3)
                    screenshot(monitor, logs / "confirmation.ppm")
                    monitor.command("sendkey esc", 0.3)
                    monitor.command("sendkey delete", 0.3)
                    monitor.command("sendkey ret", 0.3)
                    smoke.wait_for(serial, process, "Program removed; Programs shortcut unregistered.")
                    screenshot(monitor, logs / "after-removal.ppm")
                else:
                    # Rebuilt registry must omit REMOVE and keep the other executable.
                    # Launch it through the live, scrollable Programs menu.
                    monitor.click(20, height - 18)
                    monitor.click(40, height - 90)
                    # Eight fixed rows followed by HELLO and KEEP.
                    monitor.click(200, height - 50)
                    smoke.wait_for(serial, process, "EXEC: A:\\KEEP.ELF exited with status 7")
            finally:
                if monitor is not None:
                    monitor.close()
                panel.stop(process)
            deleted = subprocess.run(
                ["mtype", "-i", str(disk), "::/REMOVE.ELF"], capture_output=True,
            )
            if deleted.returncode == 0:
                raise AssertionError("Removed executable still exists on disk")
            for target in ("::/KEEP.ELF", "::/BOOT/BOOK8.ELF"):
                content = subprocess.run(
                    ["mtype", "-i", str(disk), target], check=True, capture_output=True,
                ).stdout
                if content != binary.read_bytes():
                    raise AssertionError(f"Removal changed a retained/core binary: {target}")
    print(f"PASS: program confirmation/removal, core protection, reboot persistence and menu launch; logs: {logs}")


if __name__ == "__main__":
    main()
