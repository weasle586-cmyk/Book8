#!/usr/bin/env python3
"""Verify the Settings About box and its packaged documentation links.

usage: qemu_about_test.py <book8-cli.iso> <data.img> [log-directory]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_graphics_input_smoke as smoke


def click_at(port, process, x, y):
    commands = ["mouse_move -100 -100"] * 10
    while x > 0 or y > 0:
        dx, dy = min(x, 80), min(y, 80)
        commands.append(f"mouse_move {dx} {dy}")
        x -= dx
        y -= dy
    smoke.monitor(port, commands + ["mouse_button 1", "mouse_button 0"], process)


def boot(iso, disk, logs):
    port = smoke.available_port()
    serial = logs / "about.serial.log"
    stderr = logs / "about.stderr.log"
    args = [
        os.environ.get("QEMU", "qemu-system-x86_64"),
        "-machine", "pc", "-m", "256", "-vga", "std", "-display", "none",
        "-cdrom", str(iso), "-boot", "d",
        "-drive", f"file={disk},format=raw,if=ide,index=0",
        "-serial", "stdio", "-monitor",
        f"tcp:127.0.0.1:{port},server,nowait", "-no-reboot",
    ]
    with serial.open("wb") as out, stderr.open("wb") as err:
        process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=out, stderr=err)
    return process, port, serial


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and source image must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "about-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-about-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        process, port, serial = boot(iso, disk, logs)
        try:
            log = smoke.wait_for(serial, process, "A:\\> ")
            width, height, _ = smoke.verify_framebuffer(log)
            smoke.type_line(port, "book8gui", process)
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
            smoke.wait_for(serial, process,
                           'WINDOW id=1 title="BOOK8 Sign In" registered')
            smoke.monitor(port, ["sendkey d", "sendkey e", "sendkey f",
                                 "sendkey a", "sendkey u", "sendkey l",
                                 "sendkey t", "sendkey ret"], process)
            smoke.wait_for(serial, process, "BOOK8-SESSION: login accepted uid=0")
            click_at(port, process, 20, height - 18)
            click_at(port, process, 40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered')
            smoke.monitor(port, ["sendkey f1"], process)
            smoke.wait_for(serial, process, "BOOK8-ABOUT: opened")
            shot = logs / "about.ppm"
            smoke.monitor(port, [f"screendump {shot}"], process)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if shot.is_file() and shot.stat().st_size > 1024:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError(f"QEMU did not write an About screenshot: {shot}")
            smoke.monitor(port, ["sendkey esc"], process)
            smoke.wait_for(serial, process, "BOOK8-ABOUT: closed")
            smoke.monitor(port, ["sendkey f1"], process)
            smoke.wait_for(serial, process, "BOOK8-ABOUT: opened", occurrences=2)
            smoke.monitor(port, ["sendkey ret"], process)
            smoke.wait_for(serial, process, "BOOK8-ABOUT: open A:\\DOC\\README.MD")
            smoke.wait_for(serial, process, 'title="Code Editor" registered')
            assert "BOOK8-ABOUT: open-failed" not in smoke.read_serial(serial)
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    print(f"PASS: Settings About box opened, closed and linked to Code Editor; "
          f"screenshot: {shot}")


if __name__ == "__main__":
    main()
