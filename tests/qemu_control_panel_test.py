#!/usr/bin/env python3
"""Exercise control-panel navigation and disk persistence on a disposable copy.

usage: qemu_control_panel_test.py <book8-cli.iso> <data.img> [log-directory]
Runtime modesetting is intentionally tested by the host fake-backend suite,
not inferred from QEMU firmware framebuffer support.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import qemu_graphics_input_smoke as smoke


def click_at(port, process, x, y, button=1):
    commands = ["mouse_move -100 -100"] * 10
    while x > 0 or y > 0:
        dx, dy = min(x, 80), min(y, 80)
        commands.append(f"mouse_move {dx} {dy}")
        x -= dx
        y -= dy
    smoke.monitor(port, commands + [f"mouse_button {button}", "mouse_button 0"], process)


def boot(iso, disk, logs, name):
    port = smoke.available_port()
    serial = logs / f"{name}.serial.log"
    stderr = logs / f"{name}.stderr.log"
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


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def config_from_disk(disk):
    data = subprocess.run(
        ["mtype", "-i", str(disk), "::/BOOK8.CFG"],
        check=True, capture_output=True,
    ).stdout
    if len(data) != 76:
        raise AssertionError(f"Unexpected config size: {len(data)}")
    magic, version, count, checksum = struct.unpack_from("<4I", data)
    if (magic, version, count) != (0x384B4F42, 1, 15):
        raise AssertionError("Invalid configuration header")
    actual = 2166136261
    for byte in data[16:]:
        actual = ((actual ^ byte) * 16777619) & 0xFFFFFFFF
    if checksum != actual:
        raise AssertionError("Invalid configuration checksum")
    return struct.unpack_from("<15I", data, 16)


def wait_page(port, process, path, page, width, height):
    deadline = time.monotonic() + 15
    expected_size = len(f"P6\n{width} {height}\n255\n".encode("ascii")) + width * height * 3
    while time.monotonic() < deadline:
        path.unlink(missing_ok=True)
        smoke.monitor(port, [f"screendump {path}"], process)
        while not path.is_file() or path.stat().st_size < expected_size:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise AssertionError("QEMU did not finish its control-panel screenshot")
            time.sleep(0.1)
        color, image_width, image_height = smoke.ppm_pixel(path, 180, 74 + page * 22)
        if (image_width, image_height) != (width, height):
            raise AssertionError("Control-panel screenshot dimensions changed")
        if color == (0, 120, 215):
            return
        time.sleep(0.2)
    raise AssertionError(f"Sidebar did not select category {page}")


def wait_space_graph(port, process, path, width, height):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        wait_page(port, process, path, 5, width, height)
        color, _, _ = smoke.ppm_pixel(path, 600, 142)
        if color == (192, 192, 192):
            return
        time.sleep(0.2)
    raise AssertionError("FAT space sampling did not publish its used/free graph")


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and source image must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "control-panel-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-control-panel-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        process, port, serial = boot(iso, disk, logs, "first-boot")
        try:
            log = smoke.wait_for(serial, process, "A:\\> ")
            width, height, _ = smoke.verify_framebuffer(log)
            smoke.type_line(port, "book8gui", process)
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
            click_at(port, process, 20, height - 18)
            click_at(port, process, 40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered')
            for page in range(9):
                wait_page(port, process, logs / f"page-{page}.ppm", page, width, height)
                if page == 5:
                    wait_space_graph(port, process, logs / "page-5.ppm", width, height)
                smoke.monitor(port, ["sendkey pgdn"], process)
            # Add/Remove Programs is appended after About; preserve the older
            # categories' indices while exercising the new page in its own test.
            smoke.monitor(port, ["sendkey pgdn"], process)
            wait_page(port, process, logs / "returned-to-date.ppm", 0, width, height)
            smoke.monitor(port, ["sendkey pgdn"] * 3, process)
            wait_page(port, process, logs / "display-before-apply.ppm", 3, width, height)
            smoke.monitor(port, ["sendkey right", "sendkey ret"], process)
            smoke.wait_for(serial, process, "BOOK8-CONFIG: saved", occurrences=1)
            # Mouse page: speed, double click, then swap primary buttons.
            smoke.monitor(port, ["sendkey pgup"], process)
            wait_page(port, process, logs / "mouse-before-apply.ppm", 2, width, height)
            smoke.monitor(port, ["sendkey right", "sendkey down",
                                 "sendkey right", "sendkey down", "sendkey right",
                                 "sendkey ret"], process)
            smoke.wait_for(serial, process, "BOOK8-CONFIG: saved", occurrences=2)
            for x, y in ((20, height - 18), (40, height - 50),
                         (200, height - 50)):
                click_at(port, process, round(x * 100 / 105),
                         round(y * 100 / 105), button=2)
            smoke.wait_for(serial, process, "BOOK8GUI closed.")
            # Reopen the desktop to exercise the shared configuration lifetime.
            smoke.type_line(port, "book8gui", process)
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR", occurrences=2)
            time.sleep(0.5)
        finally:
            stop(process)
        config = config_from_disk(disk)
        if (config[4], config[5], config[6], config[7]) != (105, 450, 1, 1):
            raise AssertionError(f"Unexpected saved pointer/display preferences: {config}")
        process, port, serial = boot(iso, disk, logs, "second-boot")
        try:
            smoke.wait_for(serial, process, "A:\\> ")
            smoke.type_line(port, "book8gui", process)
            smoke.wait_for(serial, process, "BOOK8-CONFIG: Loaded A:\\BOOK8.CFG.")
            smoke.wait_for(
                serial, process,
                "BOOK8-CONFIG: active resolution=1 pointer-speed=105 double-click=450 swapped=1",
            )
        finally:
            stop(process)
        if config_from_disk(disk) != config:
            raise AssertionError("Boot changed persisted preferences")
    print(f"PASS: control panel navigation, config save and reboot restore; logs: {logs}")


if __name__ == "__main__":
    main()
