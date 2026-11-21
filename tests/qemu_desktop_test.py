#!/usr/bin/env python3
"""Exercise BOOK8 desktop registration, taskbar focus, and Explorer navigation.

usage: qemu_desktop_test.py <book8.iso> /tmp/d.img
"""
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BOOT_MESSAGE = "BOOK8 x86-64 command interpreter"
FRAMEBUFFER_PATTERN = re.compile(
    r"BOOK8 framebuffer handoff:\s*(\d+)x(\d+)x(?:24|32)"
)


def wait_for_text(path, expected, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(errors="replace") if path.exists() else ""
        if expected in text:
            return text
        time.sleep(0.1)
    text = path.read_text(errors="replace") if path.exists() else ""
    raise TimeoutError(f"Timed out waiting for {expected!r}:\n{text[-2000:]}")


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]

def screenshot_region(monitor, path, x, y, width, height):
    time.sleep(1)
    monitor.command(f"screendump {path}", 0.3)
    ppm = path.read_bytes().split(b"\n", 3)
    screen_width, screen_height = map(int, ppm[1].split())
    if x + width > screen_width or y + height > screen_height:
        raise AssertionError("Screenshot region is outside the active display")
    return b"".join(
        ppm[3][(row * screen_width + x) * 3:
               (row * screen_width + x + width) * 3]
        for row in range(y, y + height)
    )


class QemuMonitor:
    def __init__(self, port, process):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before the monitor was ready")
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                self.sock.settimeout(0.05)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise TimeoutError("QEMU monitor did not become available")
        try:
            self.sock.recv(4096)
        except TimeoutError:
            pass
        self.x = None
        self.y = None
        self.width = None
        self.height = None

    def command(self, command, pause=0.02):
        self.sock.sendall(command.encode("ascii") + b"\n")
        time.sleep(pause)
        try:
            self.sock.recv(4096)
        except TimeoutError:
            pass

    def type_text(self, text):
        for character in text:
            self.command(f"sendkey {character}", 0.025)
        self.command("sendkey ret", 0.05)

    def move_to(self, x, y):
        if self.x is None or self.y is None:
            raise RuntimeError("Mouse position must be reset before moving")
        dx = x - self.x
        dy = y - self.y
        while dx != 0 or dy != 0:
            step_x = max(-40, min(40, dx))
            step_y = max(-40, min(40, dy))
            self.command(f"mouse_move {step_x} {step_y}", 0.05)
            dx -= step_x
            dy -= step_y
            self.x += step_x
            self.y += step_y
            if self.width is not None:
                self.x = max(0, min(self.width - 1, self.x))
            if self.height is not None:
                self.y = max(0, min(self.height - 1, self.y))

    def reset_pointer(self):
        for _ in range(30):
            self.command("mouse_move -100 -100", 0.03)
        self.x = 0
        self.y = 0
        # Let the guest drain queued relative moves before absolute targeting.
        time.sleep(1.5)

    def set_screen_size(self, width, height):
        self.width = width
        self.height = height

    def click(self, x, y):
        self.move_to(x, y)
        self.command("mouse_button 1", 0.06)
        self.command("mouse_button 0", 0.06)

    def close(self):
        self.sock.close()


def validate_inputs(iso, image):
    if not iso.is_file():
        raise SystemExit(f"BOOK8 ISO does not exist: {iso}")
    if str(image) != "/tmp/d.img" or image.is_symlink():
        raise SystemExit("QEMU disk target must be a regular disposable /tmp/d.img")
    try:
        image_mode = image.stat().st_mode
    except OSError as error:
        raise SystemExit(f"Disposable image is not available: {error}") from error
    if not stat.S_ISREG(image_mode) or image.stat().st_size == 0:
        raise SystemExit("/tmp/d.img must be a non-empty regular disposable image")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_desktop_test.py <book8.iso> /tmp/d.img")
    iso = Path(sys.argv[1]).resolve()
    image = Path(sys.argv[2])
    validate_inputs(iso, image)

    failures = []
    with tempfile.TemporaryDirectory(prefix="book8-desktop-") as temp:
        temp_path = Path(temp)
        serial = temp_path / "serial.log"
        stderr = temp_path / "qemu.stderr.log"
        screenshot = temp_path / "desktop.ppm"
        port = available_port()
        args = [
            os.environ.get("QEMU", "qemu-system-x86_64"),
            "-machine", "pc", "-m", "512M",
            "-device", "piix3-usb-uhci,id=uhci",
            "-device", "usb-mouse,bus=uhci.0",
            "-cdrom", str(iso), "-boot", "d",
            "-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on",
            "-display", "none", "-vga", "std",
            "-serial", f"file:{serial}",
            "-monitor", f"tcp:127.0.0.1:{port},server,nowait",
            "-no-reboot",
        ]
        stderr_file = stderr.open("wb")
        process = subprocess.Popen(args, stderr=stderr_file)
        monitor = None
        try:
            wait_for_text(serial, BOOT_MESSAGE, 90)
            wait_for_text(serial, "USB HID mouse active", 20)
            monitor = QemuMonitor(port, process)

            # Clean boot: only the taskbar is reserved, no windows autostart.
            log = wait_for_text(serial, "BOOK8-DESKTOP: TASKBAR height=36 reserved")
            if "BOOK8-DESKTOP: WINDOW id=" in log:
                raise AssertionError("A window auto-started on the clean desktop")
            monitor.reset_pointer()

            log = wait_for_text(serial, "BOOK8 framebuffer handoff:")
            match = FRAMEBUFFER_PATTERN.search(log)
            if match is None:
                raise AssertionError("Framebuffer handoff dimensions not found")
            width, height = map(int, match.groups())
            monitor.set_screen_size(width, height)
            taskbar_y = height - 18

            monitor.command(f"screendump {screenshot}")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not screenshot.is_file():
                time.sleep(0.1)
            if not screenshot.is_file():
                raise AssertionError("QEMU desktop screenshot was not created")
            header = screenshot.read_bytes()[:64]
            if not header.startswith(b"P6"):
                raise AssertionError("QEMU desktop screenshot is not PPM")

            # Start menu geometry mirrors src/book8gui.c (20 px rows, 2 px gaps).
            top = height - 36
            root_y = top - 2 - 64
            programs_y = root_y + 2 + 10
            settings_y = root_y + 2 + 20 + 10
            # Data fixture: four built-ins + two root apps + three utilities.
            sub_y = min(root_y, top - 2 - (9 * 20 + 4))
            explorer_y = sub_y + 2 + 10
            shell_y = sub_y + 2 + 20 + 10
            editor_y = sub_y + 2 + 2 * 20 + 10
            taskmgr_y = sub_y + 2 + 3 * 20 + 10
            monitor.click(20, taskbar_y)
            monitor.click(40, programs_y)
            monitor.click(200, explorer_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW id=1 title="File Explorer" registered', 5)
            monitor.click(20, taskbar_y)
            monitor.click(40, programs_y)
            monitor.click(200, editor_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW id=2 title="NoteEditor" registered', 5)
            monitor.click(20, taskbar_y)
            monitor.click(40, programs_y)
            monitor.click(200, shell_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW id=3 title="Command Shell" registered', 5)
            monitor.click(300, 150)
            wait_for_text(serial,
                "BOOK8-DESKTOP: input routed id=3 type=mouse-click", 5)

            monitor.type_text("inputstat")
            log = wait_for_text(serial, "BOOK8-INPUT:")
            monitor.click(104, taskbar_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: focus id=1 title="File Explorer"')
            monitor.click(60, 131)
            time.sleep(0.08)
            monitor.click(60, 131)
            log = wait_for_text(serial, "BOOK8-EXPLORER: path=A:\\DATA\\")
            if "BOOK8-DESKTOP: input routed id=1 type=mouse-click" not in log:
                raise AssertionError("Explorer click was not routed to its window")

            input_lines = re.findall(r"BOOK8-INPUT:[^\r\n]+", log)
            if not input_lines:
                raise AssertionError("Input status serial line was not captured")
            values = {
                key: int(value)
                for key, value in re.findall(
                    r"([A-Z0-9_]+)=(\d+)", input_lines[-1]
                )
            }
            if values.get("USB_REPORTS", 0) <= 0 \
                    or values.get("USB_QUEUE_DROPS") != 0:
                raise AssertionError(
                    f"USB mouse input counters are not clean: {input_lines[-1]}"
                )

            # Explorer sits at (40,40), 300 px wide: min/max/close buttons.
            monitor.click(285, 55)
            wait_for_text(serial, "BOOK8-DESKTOP: WINDOW minimized id=1", 5)
            monitor.click(104, taskbar_y)
            wait_for_text(serial, "BOOK8-DESKTOP: WINDOW restored id=1", 5)
            monitor.click(305, 55)
            wait_for_text(serial, "BOOK8-DESKTOP: WINDOW maximized id=1", 5)
            time.sleep(1)
            monitor.command(f"screendump {temp_path / 'maximized.ppm'}", 0.3)
            ppm = (temp_path / "maximized.ppm").read_bytes().split(b"\n", 3)
            shot_width, shot_height = map(int, ppm[1].split())
            assert (shot_width, shot_height) == (width, height)
            pixels = ppm[3]
            def color_at(x, y):
                offset = (y * width + x) * 3
                return tuple(pixels[offset:offset + 3])
            if color_at(width // 2, 0) != (0, 0, 0) \
                    or color_at(width - 1, height - 37) != (0, 0, 0):
                raise AssertionError(
                    "Maximized frame does not fill the work area: "
                    f"top={color_at(width // 2, 0)}, "
                    f"bottom={color_at(width - 1, height - 37)}"
                )
            monitor.click(width - 35, 15)

            # Unsaved scratch text only: prove USB wheel delivery changes the
            # actual editor viewport in both directions, not just telemetry.
            monitor.click(240, taskbar_y)
            for character in "abcdefghijklmnopqrstuvwx":
                monitor.command(f"sendkey {character}", 0.12)
                monitor.command("sendkey ret", 0.12)
            monitor.move_to(100, 150)
            before = screenshot_region(monitor, temp_path / "wheel-before.ppm",
                                       71, 117, 520, 272)
            monitor.command("mouse_move 0 0 1", 0.3)
            wait_for_text(serial, " WHEEL=1 ", 5)
            scrolled = screenshot_region(monitor, temp_path / "wheel-up.ppm",
                                         71, 117, 520, 272)
            if scrolled == before:
                raise AssertionError("Wheel up did not move editor content")
            monitor.command("mouse_move 0 0 -1", 0.3)
            wait_for_text(serial, " WHEEL=-1 ", 5)
            restored = screenshot_region(monitor, temp_path / "wheel-down.ppm",
                                         71, 117, 520, 272)
            if restored != before:
                differing_rows = [
                    row for row in range(272)
                    if restored[row * 520 * 3:(row + 1) * 520 * 3]
                    != before[row * 520 * 3:(row + 1) * 520 * 3]
                ]
                raise AssertionError(
                    "Opposite wheel delta did not restore content: "
                    f"differing rows={differing_rows[:20]}"
                )

            monitor.click(20, taskbar_y)
            monitor.click(40, settings_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW id=4 title="System Settings" registered',
                5)
            # Time and Date is the initial page; increment the hour and apply.
            monitor.command("sendkey right")
            monitor.command("sendkey ret")
            wait_for_text(serial, "BOOK8-SETTINGS: time set ", 5)

            # Task Manager: Start > Programs menu, then the Ctrl+Alt+Esc hotkey.
            monitor.click(20, taskbar_y)
            monitor.click(40, programs_y)
            monitor.click(200, taskmgr_y)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW id=5 title="Task Manager" registered',
                5)
            monitor.command("sendkey ctrl-alt-esc")
            wait_for_text(serial,
                'BOOK8-DESKTOP: hotkey Task Manager id=5', 5)
            monitor.command("sendkey right", 0.15)
            monitor.command("sendkey left", 0.15)
            # Row 0 is the desktop itself; the second Down selects File Explorer.
            monitor.command("sendkey down", 0.15)
            monitor.command("sendkey down", 0.15)
            monitor.command("sendkey delete", 0.15)
            wait_for_text(serial,
                'BOOK8-DESKTOP: WINDOW closed id=1 title="File Explorer"', 5)
            log = serial.read_text(errors="replace")
            if 'id=6 title="Task Manager"' in log:
                raise AssertionError("Task Manager opened a second instance")
            monitor.click(20, taskbar_y)
            monitor.click(40, height - 50)
            monitor.click(200, height - 50)
            wait_for_text(serial, "BOOK8GUI closed.")
            print("PASS: BOOK8 desktop test")
            print(f"Serial markers: windows=5, taskbar={height - 36}..{height - 1}, "
                  "clean boot, Start menu launches, focus routing, "
                  "min/max/restore, Settings time set, "
                  "maximized viewport bounds, bidirectional USB wheel rendering, "
                  "Task Manager hotkey + End Task, "
                  "Explorer A:\\DATA\\, USB queue drops=0")
        except (AssertionError, RuntimeError, TimeoutError) as error:
            failures.append(str(error))
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            stderr_file.close()
        if failures:
            full_log = serial.read_text(errors="replace") if serial.exists() else ""
            markers = "\n".join(
                line for line in full_log.splitlines()
                if "BOOK8-DESKTOP:" in line or "BOOK8-EXPLORER:" in line
                or "BOOK8-INPUT:" in line or "USB HID" in line
            )
            tail = full_log[-2000:]
            errors = stderr.read_text(errors="replace")[-2000:] if stderr.exists() else ""
            raise SystemExit(
                "FAIL: BOOK8 desktop test\n"
                + "\n".join(failures)
                + f"\n--- desktop/input markers ---\n{markers[-3000:]}"
                + f"\n--- serial tail ---\n{tail}\n--- QEMU stderr ---\n{errors}"
            )


if __name__ == "__main__":
    main()
