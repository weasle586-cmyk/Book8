#!/usr/bin/env python3
"""Verify the flat boot splash is decoded, drawn and held on a real QEMU boot.

usage: qemu_splash_test.py <book8.iso> <data.img> [log-directory]
"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke


def wait_for_stamp(path, process, expected, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited while waiting for {expected!r}")
        if expected in smoke.read_serial(path):
            return time.monotonic()
        time.sleep(0.05)
    raise TimeoutError(f"Timed out waiting for {expected!r}")


def count_nonblack(path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.is_file():
            data = path.read_bytes()
            if data.startswith(b"P6"):
                tokens = []
                pos = 2
                while len(tokens) < 3:
                    while pos < len(data) and data[pos:pos + 1].isspace():
                        pos += 1
                    start = pos
                    while pos < len(data) and not data[pos:pos + 1].isspace():
                        pos += 1
                    tokens.append(int(data[start:pos]))
                pos += 1
                width, height, _ = tokens
                expected = width * height * 3
                if len(data) - pos >= expected:
                    pixels = data[pos:pos + expected]
                    return sum(1 for i in range(0, expected, 3)
                               if pixels[i] or pixels[i + 1] or pixels[i + 2])
        time.sleep(0.1)
    raise AssertionError(f"QEMU did not finish writing screenshot: {path}")


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and disposable data image must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "splash-validation"
    logs.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="book8-splash-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        process, port, serial = panel.boot(iso, disk, logs, "splash")
        try:
            played_at = wait_for_stamp(serial, process, "BOOK8-SPLASH: playing")
            time.sleep(0.8)
            shot = logs / "splash.ppm"
            smoke.monitor(port, [f"screendump {shot}"], process)
            if not shot.is_file() or shot.stat().st_size == 0:
                raise AssertionError(f"QEMU did not write a splash screenshot: {shot}")
            nonblack = count_nonblack(shot)
            if nonblack < 10000:
                raise AssertionError(f"Splash screenshot is blank ({nonblack} lit pixels)")
            smoke.wait_for(serial, process, "BOOK8-SPLASH: complete", timeout=30)
            held = time.monotonic() - played_at
            if held < 2.5 or held > 6.0:
                raise AssertionError(f"Splash hold was {held:.2f}s, expected about 3s")
            text = serial.read_text(errors="replace")
            if "BOOK8-SPLASH: invalid image module" in text:
                raise AssertionError("Boot splash module failed to decode")
        finally:
            panel.stop(process)
    print(f"PASS: QEMU boot splash held for {held:.2f}s; screenshot: {shot}")


if __name__ == "__main__":
    main()
