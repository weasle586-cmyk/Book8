#!/usr/bin/env python3
"""Check bundled ELF launches and internal Programs scans with/without an external disk.

usage: qemu_static_programs_test.py <book8.iso> <data.img> [log-directory]
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke
import qemu_ui_lookup as ui
from qemu_desktop_test import QemuMonitor
from qemu_utilities_test import screenshot_region, type_text


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and disposable source image must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "static-programs-validation"
    logs.mkdir(parents=True, exist_ok=True)
    rendered = []
    with tempfile.TemporaryDirectory(prefix="book8-static-programs-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        binary = iso.parent / "user" / "hello.elf"
        for index in range(140):
            subprocess.run(["mmd", "-i", str(disk), f"::/HOST{index:03}"], check=True)
        subprocess.run(["mcopy", "-i", str(disk), str(binary), "::/HOSTAPP.ELF"], check=True)
        for attached in (False, True):
            name = "with-app-disk" if attached else "without-app-disk"
            serial = logs / f"{name}.serial.log"
            port = smoke.available_port()
            args = [os.environ.get("QEMU", "qemu-system-x86_64"),
                    "-machine", "pc", "-m", "256", "-vga", "std",
                    "-display", "none", "-cdrom", str(iso), "-boot", "d",
                    "-serial", "stdio", "-monitor",
                    f"tcp:127.0.0.1:{port},server,nowait", "-no-reboot"]
            if attached:
                args += ["-drive", f"file={disk},format=raw,if=ide,index=0"]
            with serial.open("wb") as out, (logs / f"{name}.stderr.log").open("wb") as err:
                process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=out, stderr=err)
            monitor = None
            try:
                log = smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
                width, height, _ = smoke.verify_framebuffer(log)
                monitor = QemuMonitor(port, process)
                monitor.set_screen_size(width, height)
                monitor.reset_pointer()
                smoke.wait_for(serial, process,
                               'WINDOW id=1 title="BOOK8 Sign In" registered')
                type_text(monitor, "default\n")
                smoke.wait_for(serial, process,
                               "BOOK8-SESSION: login accepted uid=0")
                # A:\HELLO.ELF is indexed too.  The external test disk is
                # reported unreadable and contributes no programs.
                expected = "Programs indexed: 14; metadata unavailable: 0; invalid names skipped: 0; unreadable drives skipped: %d." % int(attached)
                smoke.wait_for(serial, process, expected)
                rows = ui.await_program_rows(serial, process)
                menu_y = ui.panel_top(height, len(rows))
                monitor.click(20, height - 18)
                monitor.click(40, height - 90)
                pixels = screenshot_region(monitor, logs / f"{name}-menu.ppm",
                                           162, menu_y + 4, 160, 8 * 20)
                rendered.append(b"".join(pixels[row * 20 * 160 * 3:
                                               (row * 20 + 16) * 160 * 3]
                                         for row in range(8)))
                if not attached:
                    continue
                paths = ("A:\\BIN\\HEXVIEW.ELF", "A:\\BIN\\CODEEDIT.ELF",
                         "A:\\BIN\\NETSHARE.ELF", "A:\\CAT.ELF")
                titles = ("Hex Sector Viewer", "Code Editor",
                          "Local Network Share Browser")
                preferred_widths = (680, 640, 720)
                for index, path in enumerate(paths):
                    if index == 0:
                        # The Programs submenu is still open from the screenshot
                        # above; clicking Start again would close it.
                        monitor.click(200, ui.row_y(
                            height, ui.resolve_row(serial, path=path), len(rows)))
                    else:
                        ui.click_program(monitor, serial, height, process=process,
                                         path=path)
                    if index < 3:
                        smoke.wait_for(serial, process, f'title="{titles[index]}" registered')
                        window_width = min(preferred_widths[index], width - 8)
                        # The sign-in window consumed the first cascade step.
                        step = index + 1
                        x = min(40 + step * 24, width - window_width)
                        monitor.click(x + window_width - 14, 52 + step * 24)
                        smoke.wait_for(serial, process, f"WINDOW closed id={index + 2}")
                    else:
                        smoke.wait_for(serial, process, f"EXEC: {path} exited with status 7")
                ui.click_program(monitor, serial, height, process=process,
                                 path="A:\\HELLO.ELF")
                smoke.wait_for(serial, process, "EXEC: A:\\HELLO.ELF exited with status 7")
                smoke.wait_for(serial, process, "HELLO: argv[0]=A:\\HELLO.ELF")
                builtins = (("File Explorer", 300, 300),
                            ("Command Shell", 300, 300),
                            ("NoteEditor", 560, 360),
                            ("Task Manager", 560, 360))
                # launch_count includes the sign-in window and the three utility
                # windows above, so these cascade steps are 4, 5, 0 and 1 mod 6.
                builtin_steps = (96, 120, 0, 24)
                for order, (title, preferred_width, preferred_height) in enumerate(builtins):
                    ui.click_program(monitor, serial, height, process=process,
                                     label=title)
                    window_id = order + 5
                    smoke.wait_for(serial, process,
                                   f'WINDOW id={window_id} title="{title}" registered')
                    if title == "Command Shell":
                        # The GUI shell must execute commands, not merely create a window.
                        type_text(monitor, "exec a:/nvmap.elf\n")
                        smoke.wait_for(serial, process,
                                       "NVMAP: local diagnostics only; telemetry submission disabled")
                        smoke.wait_for(serial, process,
                                       "EXEC: a:/nvmap.elf exited with status 0")
                    step = builtin_steps[order]
                    window_width = min(preferred_width, width - 8)
                    window_height = min(preferred_height, height - 36 - 32 - 8)
                    x = min(40 + step, width - window_width)
                    y = min(40 + step, height - 36 - window_height)
                    monitor.click(x + window_width - 14, y + 12)
                    smoke.wait_for(serial, process, f"WINDOW closed id={window_id}")
                monitor.click(20, height - 18)
                monitor.click(40, height - 70)
                smoke.wait_for(serial, process, 'WINDOW id=9 title="System Settings" registered')
                monitor.command("sendkey pgdn", 0.3)
                monitor.command("sendkey pgdn", 0.3)
                smoke.wait_for(serial, process, expected, occurrences=2)
                screenshot_region(monitor, logs / f"{name}-programs.ppm", 160, 50, 440, 290)
                # System Settings consumed launch step 8; these are steps 9-11.
                trailing_steps = (72, 96, 120)
                for order, (title, preferred_width, preferred_height) in enumerate((
                        ("PaintBook", 760, 520),
                        ("Web Browser", 820, 560),
                        ("NV Diagnostic", 700, 480))):
                    ui.click_program(monitor, serial, height, process=process,
                                     label=title)
                    window_id = {"PaintBook": 10, "Web Browser": 11,
                                 "NV Diagnostic": 12}[title]
                    smoke.wait_for(serial, process,
                                   f'WINDOW id={window_id} title="{title}" registered')
                    window_width = min(preferred_width, width - 8)
                    window_height = min(preferred_height, height - 36 - 32 - 8)
                    step = trailing_steps[order]
                    x = min(40 + step, width - window_width)
                    y = min(40 + step, height - 36 - window_height)
                    monitor.click(x + window_width - 14, y + 12)
                    smoke.wait_for(serial, process, f"WINDOW closed id={window_id}")
                ui.click_program(monitor, serial, height, process=process,
                                 label="BOOK8 Player")
                smoke.wait_for(serial, process,
                               'WINDOW id=13 title="BOOK8 Player" registered')
                monitor.click(40 + 500 - 14, 40 + 12)
                smoke.wait_for(serial, process, "WINDOW closed id=13")
                log = smoke.read_serial(serial)
                if "Programs scan incomplete" in log or "HOSTAPP" in log:
                    raise AssertionError("Programs scan escaped internal volume or failed")
            finally:
                if monitor is not None:
                    monitor.close()
                panel.stop(process)
    if len(rendered) != 2:
        raise AssertionError("Programs menu screenshots were not captured for both disk profiles")
    print(f"PASS: registry-backed Programs menu scan, installed ELFs (including PaintBook "
          f"and NV Diagnostic), and no-disk menu startup; logs: {logs}")


if __name__ == "__main__":
    main()
