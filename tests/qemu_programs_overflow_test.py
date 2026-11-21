#!/usr/bin/env python3
"""Test expanded Programs menus, deep paths and scrollbar capture on a FAT copy.

usage: qemu_programs_overflow_test.py <book8.iso|book8-cli.iso> <data.img> [log-directory]
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke
from qemu_desktop_test import QemuMonitor
from qemu_utilities_test import screenshot_region

APP_COUNT = 300


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    binary = iso.parent / "user" / "hello.elf"
    if not all(path.is_file() for path in (iso, source, binary)):
        raise SystemExit("Built ISO, regular disposable source image and hello ELF required")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "programs-overflow-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-programs-overflow-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        volume = str(disk)
        if subprocess.run(["mdir", "-i", volume, "::/"], capture_output=True).returncode != 0:
            volume += "@@1048576"
        root = subprocess.run(["mdir", "-b", "-i", volume, "::/"],
                              check=True, capture_output=True, text=True).stdout.splitlines()
        excluded = {"NVMAP.ELF", "BOOK8.ELF", "KERNEL.ELF", "BOOK8GUI.ELF", "SHELL.ELF"}
        root_names = [Path(path).name.upper() for path in root
                      if Path(path).name.upper().endswith(".ELF")
                      and Path(path).name.upper() not in excluded]
        root_apps = len(root_names)
        if "CAT.ELF" not in root_names:
            raise AssertionError("Built image must contain the registered CAT executable")
        # Raw infrastructure objects must never enter the application list.
        subprocess.run(["mmd", "-i", volume, "::/DRIVERS"], check=True)
        for target in ("::/BIN/NVMAP.ELF", "::/BIN/KERNEL.ELF", "::/DRIVERS/RAW.ELF"):
            subprocess.run(["mcopy", "-i", volume, str(binary), target], check=True)
        for index in range(APP_COUNT):
            subprocess.run(["mcopy", "-i", volume, str(binary),
                            f"::/BIN/APP{index:02}.ELF"], check=True)
        for index in range(140):
            subprocess.run(["mmd", "-i", volume, f"::/DIR{index:03}"], check=True)
        deep = ""
        for index in range(32):
            deep += f"/DLEVEL{index:02}"
            subprocess.run(["mmd", "-i", volume, f"::{deep}"], check=True)
        subprocess.run(["mcopy", "-i", volume, str(binary), f"::{deep}/DEEP.ELF"],
                       check=True)
        deep_binary = "A:" + deep.replace("/", "\\") + "\\DEEP.ELF"
        if len(deep_binary) <= 256:
            raise AssertionError("Deep executable fixture must exceed the old path limit")
        indexed = 9 + APP_COUNT + root_apps
        process, port, serial = panel.boot(iso, disk, logs, "overflow")
        monitor = None
        try:
            log = smoke.wait_for(serial, process, "BOOK8 framebuffer handoff:",
                                 complete_line=True)
            width, height, _ = smoke.verify_framebuffer(log)
            smoke.enter_prompt(serial, process, port, width, height)
            initial_scans = smoke.read_serial(serial).count("Programs indexed:")
            initial_desktops = smoke.read_serial(serial).count("BOOK8-DESKTOP: TASKBAR")
            smoke.type_line(port, "book8gui", process)
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR",
                           occurrences=initial_desktops + 1)
            smoke.wait_for(serial, process,
                           f"Programs indexed: {indexed}; metadata unavailable: 0;",
                           occurrences=initial_scans + 1)
            monitor = QemuMonitor(port, process)
            monitor.set_screen_size(width, height)
            monitor.reset_pointer()
            bottom = height - 36
            visible = (bottom - 6) // 20
            menu_y = bottom - 2 - (visible * 20 + 4)
            utility_y = menu_y + 2 + 4 * 20
            titles = ("Hex Sector Viewer", "Code Editor", "Local Network Share Browser")
            sizes = (680, 640, 720)
            for index, (title, preferred_width) in enumerate(zip(titles, sizes)):
                monitor.click(20, height - 18)
                monitor.click(40, height - 90)
                monitor.click(200, utility_y + 10 + index * 20)
                smoke.wait_for(serial, process, f'title="{title}" registered')
                window_width = min(preferred_width, width - 8)
                x = min(40 + index * 24, width - window_width)
                monitor.click(x + window_width - 14, 52 + index * 24)
                smoke.wait_for(serial, process, f"WINDOW closed id={index + 1}")
            monitor.click(20, height - 18)
            monitor.click(40, height - 90)
            monitor.click(200, menu_y + 2 + 7 * 20 + 10)
            smoke.wait_for(serial, process, "CAT: execve hello")
            smoke.wait_for(serial, process, "EXEC: A:\\CAT.ELF exited with status 7")
            monitor.click(20, height - 18)
            monitor.click(40, height - 90)
            screenshot_region(monitor, logs / "menu-expanded.ppm", 152, menu_y, 224,
                              visible * 20 + 4)
            monitor.move_to(365, menu_y + 3)
            monitor.command("mouse_button 1", 0.2)
            monitor.move_to(430, bottom - 4)
            monitor.command("mouse_button 0", 0.2)
            screenshot_region(monitor, logs / "menu-bottom.ppm", 152, menu_y, 224,
                              visible * 20 + 4)
            monitor.click(200, menu_y + 2 + (visible - 1) * 20 + 10)
            smoke.wait_for(serial, process, f"EXEC: {deep_binary} exited with status 7")
            monitor.click(20, height - 18)
            monitor.click(40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered')
            settings_width = min(620, width - 8)
            x = min(112, width - settings_width)
            monitor.click(x + settings_width - 38, 124)
            monitor.command("sendkey pgup", 0.3)
            smoke.wait_for(serial, process,
                           f"Programs indexed: {indexed}; metadata unavailable: 0;",
                           occurrences=initial_scans + 2)
            content_width = width - 8 - 148
            list_width = content_width // 2
            list_x, list_y = 152, 30
            list_height = height - 36 - 34 - 54
            track_x = list_x + list_width - 13
            before = screenshot_region(monitor, logs / "list-before.ppm",
                                       list_x + 4, list_y + 6, 144, 16)
            monitor.move_to(track_x + 2, list_y + 3)
            monitor.command("mouse_button 1", 0.2)
            grabbed = screenshot_region(monitor, logs / "thumb-grabbed.ppm",
                                        list_x + 4, list_y + 6, 144, 16)
            if grabbed != before:
                raise AssertionError("Grabbing the thumb changed the list offset")
            monitor.move_to(100, list_y + list_height - 1)
            monitor.command("mouse_button 0", 0.2)
            after = screenshot_region(monitor, logs / "list-bottom.ppm",
                                      list_x + 4, list_y + 6, 144, 16)
            if after == before:
                raise AssertionError("Scrollbar drag across the sidebar was swallowed")
            monitor.move_to(list_x + 20, list_y + 50)
            # QEMU HMP generates one notch regardless of the supplied magnitude.
            for _ in range(indexed + 1):
                monitor.command("mouse_move 0 0 1", 0.15)
            monitor.move_to(100, list_y + 10)
            restored = screenshot_region(monitor, logs / "list-restored.ppm",
                                         list_x + 4, list_y + 6, 144, 16)
            if restored != before:
                raise AssertionError("Wheel-up did not restore the first registry row")
            log = serial.read_text(errors="replace")
            if "Programs scan incomplete" in log:
                raise AssertionError("Valid expanded FAT BIN registry reported an incomplete scan")
        finally:
            if monitor:
                monitor.close()
            panel.stop(process)
    print(f"PASS: {APP_COUNT} BIN fixtures, 140 sibling directories, 32-level long-path launch, "
          f"direct Hex/Code/Netshare/CAT launches, no incomplete boot scan, "
          f"menu overflow/drag, Settings thumb capture/wheel; logs: {logs}")


if __name__ == "__main__":
    main()
