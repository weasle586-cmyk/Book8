#!/usr/bin/env python3
"""Exercise real GUI utility ELFs and FAT saves/removal on a temporary image.

usage: qemu_utilities_test.py <book8-cli.iso> <data.img> [log-directory]
"""
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke
import qemu_ui_lookup as ui
from qemu_desktop_test import QemuMonitor


def screenshot_region(monitor, path, x, y, width, height):
    time.sleep(1)
    with tempfile.TemporaryDirectory(prefix="book8-utility-screen-") as temp:
        screenshot = Path(temp) / "frame.ppm"
        monitor.command(f"screendump {screenshot}", 0.3)
        expected = len(f"P6\n{monitor.width} {monitor.height}\n255\n".encode("ascii")) \
            + monitor.width * monitor.height * 3
        deadline = time.monotonic() + 10
        while not screenshot.is_file() or screenshot.stat().st_size < expected:
            if time.monotonic() >= deadline:
                raise AssertionError("Utility screenshot did not finish")
            time.sleep(0.1)
        data = screenshot.read_bytes()
        shutil.copyfile(screenshot, path)
    ppm = data.split(b"\n", 3)
    screen_width, screen_height = map(int, ppm[1].split())
    if x + width > screen_width or y + height > screen_height:
        raise AssertionError("Utility screenshot region is offscreen")
    return b"".join(ppm[3][(row * screen_width + x) * 3:
                           (row * screen_width + x + width) * 3]
                    for row in range(y, y + height))


def type_text(monitor, value):
    mapping = {":": "shift-semicolon", "/": "slash", "\\": "backslash",
               ".": "dot", " ": "spc", "_": "shift-minus", "\n": "ret", "\t": "tab"}
    for char in value:
        key = mapping.get(char, f"shift-{char.lower()}" if char.isupper() else char)
        monitor.command(f"sendkey {key}", 0.15)


def exit_desktop(monitor, serial, process, height, occurrences=1):
    monitor.click(20, height - 18)
    monitor.click(40, height - 50)
    monitor.click(200, height - 50)
    smoke.wait_for(serial, process, "BOOK8GUI closed.", occurrences=occurrences)


def program_menu_rows(serial):
    """Backward-compatible wrapper; row identity now comes from book8gui."""
    return ui.menu_row_count(serial)


def program_row_y(height, row, rows):
    return ui.row_y(height, row, rows)


def launch_hex(monitor, serial, process, height):
    """Launch the Hex Viewer by its installed path, not a shifting row index."""
    ui.click_program(monitor, serial, height, process=process,
                     path="A:\\BIN\\HEXVIEW.ELF")


def remove_program_at(monitor, index):
    monitor.command("sendkey home", 0.3)
    for _ in range(index):
        monitor.command("sendkey down", 0.15)
    monitor.command("sendkey delete", 0.3)
    monitor.command("sendkey ret", 0.3)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and source must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "utilities-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-utilities-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        # Model an installed system volume, not an excluded external data disk.
        subprocess.run(["mmd", "-i", str(disk), "::/BOOT"], check=True)
        subprocess.run(["mcopy", "-i", str(disk), str(iso.parent / "book8.elf"),
                        "::/BOOT/BOOK8.ELF"], check=True)
        document = Path(temp) / "test.c"
        original = b"hello\n" + b"line of text\n" * 80
        document.write_bytes(original)
        binary = Path(temp) / "raw.bin"
        binary.write_bytes(bytes(range(256)) * 16)
        for host, target in ((document, "::/DATA/TEST.C"), (binary, "::/DATA/RAW.BIN")):
            subprocess.run(["mcopy", "-i", str(disk), str(host), target], check=True)
        process, port, serial = panel.boot(iso, disk, logs, "utilities")
        monitor = None
        try:
            log = smoke.wait_for(serial, process, "A:\\> ")
            width, height, _ = smoke.verify_framebuffer(log)
            monitor = QemuMonitor(port, process)
            monitor.set_screen_size(width, height)
            monitor.reset_pointer()
            type_text(monitor, "exec a:/bin/codeedit.elf a:/data/test.c\n")
            smoke.wait_for(serial, process, 'title="Code Editor" registered')
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
            monitor.reset_pointer()
            # The document request is queued before sign-in; authenticate first,
            # then load the file explicitly so the editor has scrollable content.
            type_text(monitor, "default\n")
            smoke.wait_for(serial, process, "BOOK8-SESSION: login accepted uid=0")
            window_width = min(640, width - 8)
            window_x = min(40, width - window_width)
            monitor.click(window_x + window_width - 38, 52)
            monitor.command("sendkey ctrl-o", 0.3)
            type_text(monitor, "a:/data/test.c\n")
            before = screenshot_region(monitor, logs / "code-before.ppm", 65, 61, 200, 16)
            monitor.move_to(150, 120)
            monitor.command("mouse_move 0 0 -1", 0.3)
            after = screenshot_region(monitor, logs / "code-down.ppm", 65, 61, 200, 16)
            if before == after:
                raise AssertionError("Code editor wheel did not change the rendered first line")
            monitor.command("mouse_move 0 0 1", 0.3)
            restored = screenshot_region(monitor, logs / "code-up.ppm", 65, 61, 200, 16)
            if restored != before:
                raise AssertionError("Code editor positive wheel did not restore content")
            type_text(monitor, "edit\n\t")
            monitor.command("sendkey ctrl-s", 0.3)
            smoke.wait_for(serial, process, "Code Editor: file saved.")
            monitor.command("sendkey ctrl-shift-s", 0.3)
            for _ in range(30):
                monitor.command("sendkey backspace", 0.12)
            type_text(monitor, "a:/data/saved.cfg\n")
            smoke.wait_for(serial, process, "Code Editor: file saved.", occurrences=2)
            screenshot_region(monitor, logs / "code-saved.ppm", 8, 7, 360, 16)
            monitor.click(width - 14, 12)
            smoke.wait_for(serial, process, 'title="Code Editor - a:/data/saved.cfg"')
            exit_desktop(monitor, serial, process, height)
            type_text(monitor, "book8gui\n")
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR", occurrences=2)
            monitor.reset_pointer()
            ui.click_program(monitor, serial, height, process=process,
                             label="File Explorer")
            smoke.wait_for(serial, process, 'title="File Explorer" registered')
            monitor.click(302, 52)
            monitor.click(60, 92)
            monitor.command("sendkey ret", 0.3)
            monitor.click(80, 110)
            monitor.click(80, 110)
            smoke.wait_for(serial, process, 'title="Code Editor" registered', occurrences=2)
            code_x = min(64, width - window_width)
            monitor.click(code_x + window_width - 14, 76)
            smoke.wait_for(serial, process, 'title="Code Editor - A:\\DATA\\TEST.C"')
            monitor.move_to(80, 110)
            monitor.command("mouse_button 2", 0.15)
            monitor.command("mouse_button 0", 0.15)
            monitor.click(100, 246)
            monitor.click(100, 156)
            smoke.wait_for(serial, process, 'title="Code Editor" registered', occurrences=3)
            code_x = min(88, width - window_width)
            monitor.click(code_x + window_width - 14, 100)
            monitor.click(width - 14, 12)
            exit_desktop(monitor, serial, process, height, occurrences=2)
            type_text(monitor, "book8gui\n")
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR", occurrences=3)
            monitor.reset_pointer()
            launch_hex(monitor, serial, process, height)
            smoke.wait_for(serial, process, 'title="Hex Sector Viewer" registered')
            hex_width = min(680, width - 8)
            hex_x = min(40, width - hex_width)
            monitor.click(hex_x + hex_width - 38, 52)
            monitor.click(20, 42)
            type_text(monitor, "a:/data/raw.bin\n")
            hex_before = screenshot_region(monitor, logs / "hex-before.ppm", 7, 61, 560, 16)
            monitor.move_to(150, 120)
            monitor.command("mouse_move 0 0 -1", 0.3)
            hex_after = screenshot_region(monitor, logs / "hex-down.ppm", 7, 61, 560, 16)
            if hex_before == hex_after:
                raise AssertionError("Hex viewer wheel did not change the offset row")
            monitor.command("mouse_move 0 0 1", 0.3)
            hex_restored = screenshot_region(monitor, logs / "hex-up.ppm", 7, 61, 560, 16)
            if hex_before != hex_restored:
                raise AssertionError("Hex viewer upward wheel did not restore the offset")
            monitor.click(100, 42)
            monitor.command("sendkey ret", 0.3)
            raw = screenshot_region(monitor, logs / "hex-cluster.ppm", 7, 61, 560, 16)
            if raw == hex_before:
                raise AssertionError("Raw FAT cluster load did not replace the file view")
            monitor.click(width - 14, 12)
            monitor.click(20, height - 18)
            monitor.click(40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered')
            # Settings now has eight categories; Apps is category 2.
            monitor.command("sendkey pgdn", 0.3)
            monitor.command("sendkey pgdn", 0.3)
            for occurrence, program_index in ((1, 7), (2, 3)):
                remove_program_at(monitor, program_index)
                smoke.wait_for(serial, process, "Program removed; Programs shortcut unregistered.",
                               occurrences=occurrence)
            screenshot_region(monitor, logs / "utilities-removed.ppm", 164, 48, 440, 300)
            exit_desktop(monitor, serial, process, height, occurrences=3)
        finally:
            if monitor:
                monitor.close()
            panel.stop(process)
        for target in ("::/DATA/TEST.C", "::/DATA/SAVED.CFG"):
            saved = subprocess.run(["mtype", "-i", str(disk), target],
                                   check=True, capture_output=True).stdout
            if saved != b"edit\n\t" + original:
                raise AssertionError(f"FAT Save/Save As content mismatch: {target}")
        for target in ("::/BIN/CODEEDIT.ELF", "::/BIN/HEXVIEW.ELF"):
            if subprocess.run(["mtype", "-i", str(disk), target],
                              capture_output=True).returncode == 0:
                raise AssertionError(f"Uninstalled utility ELF remains: {target}")
        subprocess.run(["mtype", "-i", str(disk), "::/BIN/NETSHARE.ELF"],
                       check=True, stdout=subprocess.DEVNULL)
        if subprocess.run(["mtype", "-i", str(disk), "::/DATA/RAW.BIN"],
                          check=True, capture_output=True).stdout != binary.read_bytes():
            raise AssertionError("Read-only hex viewer changed its binary")
    print(f"PASS: real utility ELFs, Explorer association/Open With, rendering/wheel, raw cluster, FAT Save/Save As and uninstall; logs: {logs}")


if __name__ == "__main__":
    main()
