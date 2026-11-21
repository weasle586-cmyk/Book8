#!/usr/bin/env python3
"""Validate BOOK8 framebuffer syscalls and PS/2/USB mouse event delivery in QEMU.

usage: qemu_graphics_input_smoke.py <book8.iso> <data.img> [log-directory]
"""
import errno
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

from qemu_desktop_test import QemuMonitor

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus"}
def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_serial(path):
    try:
        return path.read_text(errors="replace") if path.exists() else ""
    except OSError as error:
        if error.errno != errno.ENODATA:
            raise
        return ""


def wait_for(
    path, process, expected, timeout=60, occurrences=1, complete_line=False
):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited while waiting for {expected!r}")
        text = read_serial(path)
        found = text.find(expected)
        line_complete = not complete_line or (
            found >= 0 and "\n" in text[found:]
        )
        if text.count(expected) >= occurrences and line_complete:
            return text
        time.sleep(0.1)
    raise TimeoutError(f"Timed out waiting for {expected!r}")


def monitor(port, commands, process):
    deadline = time.monotonic() + 10
    while True:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before its monitor became available")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)
    with sock:
        sock.settimeout(1)
        try:
            sock.recv(4096)
        except TimeoutError:
            pass
        for command in commands:
            sock.sendall(command.encode("ascii") + b"\n")
            time.sleep(0.2)


def type_line(port, text, process):
    commands = [f"sendkey {KEYMAP.get(char, char)}" for char in text]
    commands.append("sendkey ret")
    monitor(port, commands, process)


class SlowDesktopMonitor(QemuMonitor):
    def command(self, command, pause=0.02):
        super().command(command, max(pause, 0.15))


def exit_desktop(serial, process, port, width, height):
    log = wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
    closed_count = log.count("BOOK8GUI closed.")
    desktop = SlowDesktopMonitor(port, process)
    try:
        desktop.set_screen_size(width, height)
        desktop.reset_pointer()
        desktop.click(20, height - 18)
        desktop.click(40, height - 50)
        desktop.click(200, height - 50)
        wait_for(serial, process, "BOOK8GUI closed.", timeout=10,
                 occurrences=closed_count + 1)
    finally:
        desktop.close()


def enter_prompt(serial, process, port, width, height):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited while waiting for startup")
        log = read_serial(serial)
        if "BOOK8-DESKTOP: TASKBAR" in log:
            exit_desktop(serial, process, port, width, height)
            wait_for(serial, process, "A:\\> ")
            return
        if "A:\\> " in log:
            return
        time.sleep(0.1)
    raise TimeoutError("Timed out waiting for the desktop or shell prompt")


def verify_framebuffer(log):
    match = re.search(
        r"BOOK8 framebuffer handoff:\s*(\d+)x(\d+)x(24|32) pitch=(\d+) address=0x",
        log,
    )
    if not match:
        raise AssertionError("Framebuffer handoff was not accepted")
    if "BOOK8: VGA fallback active" in log:
        raise AssertionError("Kernel fell back to VGA text mode")
    width, height, bpp, pitch = map(int, match.groups())
    if width < 640 or height < 300 or pitch < width * (bpp // 8):
        raise AssertionError(f"Invalid framebuffer geometry: {match.group(0)}")
    return width, height, bpp


def ppm_pixel(path, x, y):
    data = path.read_bytes()
    offset = 0

    def token():
        nonlocal offset
        while offset < len(data):
            if data[offset] in b" \t\r\n":
                offset += 1
            elif data[offset] == ord("#"):
                while offset < len(data) and data[offset] not in b"\r\n":
                    offset += 1
            else:
                break
        start = offset
        while offset < len(data) and data[offset] not in b" \t\r\n#":
            offset += 1
        if start == offset:
            raise AssertionError("Malformed PPM screenshot header")
        return data[start:offset]

    if token() != b"P6":
        raise AssertionError("QEMU screendump was not a binary PPM image")
    width, height, maximum = int(token()), int(token()), int(token())
    if maximum != 255 or not (0 <= x < width and 0 <= y < height):
        raise AssertionError("Unexpected screenshot dimensions or color depth")
    if data[offset:offset + 2] == b"\r\n":
        offset += 2
    elif data[offset:offset + 1] in (b"\n", b"\r", b" ", b"\t"):
        offset += 1
    expected_size = offset + width * height * 3
    if len(data) < expected_size:
        raise AssertionError("Truncated QEMU screenshot")
    pixel = offset + (y * width + x) * 3
    return tuple(data[pixel:pixel + 3]), width, height


def run_case(iso, data, log_dir, name, usb_mouse):
    serial = log_dir / f"{name}.serial.log"
    stderr_path = log_dir / f"{name}.qemu.stderr.log"
    screenshot = log_dir / f"{name}.ppm"
    serial.unlink(missing_ok=True)
    screenshot.unlink(missing_ok=True)
    monitor_port = available_port()
    args = [
        os.environ.get("QEMU", "qemu-system-x86_64"),
        "-machine", "pc", "-m", "256",
        "-device", "piix3-usb-uhci,id=uhci",
    ]
    if usb_mouse:
        args += ["-device", "usb-mouse,bus=uhci.0"]
    args += [
        "-cdrom", str(iso), "-boot", "d",
        "-drive", f"file={data},format=raw,if=ide,index=0,snapshot=on",
        "-display", "none", "-vga", "std",
        "-serial", "stdio",
        "-monitor", f"tcp:127.0.0.1:{monitor_port},server,nowait",
        "-no-reboot",
    ]
    with stderr_path.open("w", encoding="utf-8") as stderr, \
            serial.open("wb") as serial_output:
        process = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=serial_output, stderr=stderr
        )
        try:
            log = wait_for(serial, process, "BOOK8 framebuffer handoff:")
            width, height, bpp = verify_framebuffer(log)
            print(f"[{name}] framebuffer {width}x{height}x{bpp}: PASS")
            wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
            print(f"[{name}] automatic desktop startup: PASS")
            startup = read_serial(serial).split("BOOK8-DESKTOP: TASKBAR", 1)[0]
            if any(marker in startup for marker in
                   ("BOOK8-NV:", "PCIE1 ", "BOOK8-NVLOG:")):
                raise AssertionError("Verbose hardware diagnostics leaked into quiet desktop startup")
            print(f"[{name}] quiet graphical handoff: PASS")
            enter_prompt(serial, process, monitor_port, width, height)
            log = read_serial(serial)
            if "BOOK8-NVMAP:" in log or "NVMAP: snapshot accepted" in log:
                raise AssertionError("NVMAP unexpectedly ran during boot")
            print(f"[{name}] prompt reached without NVMAP startup: PASS")
            prompt_count = log.count("A:\\> ")
            type_line(monitor_port, "cls", process)
            wait_for(serial, process, "A:\\> ", occurrences=prompt_count + 1)
            type_line(monitor_port, "exec hello.elf", process)
            log = wait_for(
                serial, process, "EXEC: hello.elf exited with status 7"
            )
            wait_for(serial, process, "A:\\> ", occurrences=prompt_count + 2)
            if "HELLO: framebuffer syscalls ok" not in log:
                raise AssertionError("Ring 3 framebuffer syscalls did not pass")
            monitor(monitor_port, [f"screendump {screenshot}"], process)
            deadline = time.monotonic() + 10
            screenshot_size = (
                len(f"P6\n{width} {height}\n255\n".encode("ascii"))
                + width * height * 3
            )
            while time.monotonic() < deadline and (
                    not screenshot.is_file()
                    or screenshot.stat().st_size < screenshot_size):
                time.sleep(0.1)
            if not screenshot.is_file() \
                    or screenshot.stat().st_size < screenshot_size:
                raise AssertionError("QEMU did not finish the framebuffer screenshot")
            pixel, image_width, image_height = ppm_pixel(
                screenshot, width - 2, height - 2
            )
            if (image_width, image_height) != (width, height):
                raise AssertionError(
                    f"Framebuffer/screenshot size mismatch: "
                    f"{width}x{height} vs {image_width}x{image_height}"
                )
            if pixel != (13, 37, 251):
                raise AssertionError(
                    f"Ring 3 pixel was {pixel}, expected (13, 37, 251)"
                )
            print(f"[{name}] Ring 3 framebuffer syscalls and rendered pixel: PASS")

            if usb_mouse:
                if "USB HID mouse active" not in log:
                    raise AssertionError("UHCI USB mouse did not enumerate")
            elif "PS/2 mouse ready." not in log:
                raise AssertionError("PS/2 auxiliary mouse did not initialize")

            current_log = wait_for(serial, process, "EXEC: hello.elf exited with status 7")
            desktop_count = current_log.count("BOOK8-DESKTOP: TASKBAR")
            prompt_count = current_log.count("A:\\> ")
            type_line(monitor_port, "book8gui", process)
            wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR",
                     occurrences=desktop_count + 1)
            monitor(monitor_port, [
                "mouse_move 110 75 0",
                "mouse_button 1",
                "mouse_button 0",
                "mouse_move 0 0 1",
                "mouse_move 0 0 -1",
            ], process)
            wait_for(serial, process, " WHEEL=1 ")
            wait_for(serial, process, " WHEEL=-1 ")
            wait_for(serial, process, " WHEEL_DELTA=120 ")
            wait_for(serial, process, " WHEEL_DELTA=-120 ")
            print(f"[{name}] standard +/-120 wheel-only events delivered: PASS")
            time.sleep(1)
            exit_desktop(serial, process, monitor_port, width, height)
            wait_for(serial, process, "A:\\> ", occurrences=prompt_count + 1)
            time.sleep(1)
            if process.stdin is None:
                raise RuntimeError("QEMU serial input is unavailable")
            process.stdin.write(b"inputstat\r")
            process.stdin.flush()
            log = wait_for(
                serial, process, "BOOK8-INPUT:", complete_line=True
            )
            line = next(
                line for line in log.splitlines() if "BOOK8-INPUT:" in line
            )
            print(f"[{name}] {line.strip()}")
            values = {
                key: int(value)
                for key, value in re.findall(r"([A-Z0-9_]+)=(\d+)", line)
            }
            if values.get("PS2_QUEUE_DROPS") != 0 \
                    or values.get("USB_QUEUE_DROPS") != 0:
                raise AssertionError(f"Unified mouse queue dropped events: {line}")
            if usb_mouse:
                if values.get("USB_REPORTS", 0) == 0 \
                        or values.get("USB_QUEUED", 0) == 0:
                    raise AssertionError(f"USB mouse input was not queued: {line}")
            elif values.get("IRQ12", 0) == 0 \
                    or values.get("PS2_PACKETS", 0) == 0 \
                    or values.get("PS2_QUEUED", 0) == 0:
                raise AssertionError(f"PS/2 IRQ12 input was not queued: {line}")
            print(f"[{name}] mouse events delivered without queue drops: PASS")
            return log
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: qemu_graphics_input_smoke.py <book8.iso> <data.img> "
            "[log-directory]"
        )
    iso, data = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if not iso.is_file() or not data.is_file():
        raise SystemExit("BOOK8 ISO and FAT data image must both exist")
    log_dir = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 \
        else iso.parent / "runtime-validation"
    log_dir.mkdir(parents=True, exist_ok=True)

    run_case(iso, data, log_dir, "qemu-ps2", usb_mouse=False)
    run_case(iso, data, log_dir, "qemu-usb", usb_mouse=True)
    print(f"PASS: QEMU graphics/input smoke tests; logs: {log_dir}")


if __name__ == "__main__":
    main()
