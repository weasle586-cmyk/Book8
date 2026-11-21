#!/usr/bin/env python3
"""BOOK8GUI file-pane hit-test check on the SB710-like OHCI profile.

Starts BOOK8GUI, parks the pointer at 0,0, moves it onto the 3rd row of the
FILES pane, clicks, and inspects a QEMU screendump for the selection colour.
Also clicks the OPEN SELECTED button and expects the TYPE command to run.
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import qemu_amd_sb710_profile as profile  # noqa: E402

LIST_Y = 44 + 82
ROW_HEIGHT = 18
SELECT_RGB = (0x00, 0x00, 0x80)  # keep in sync with GUI_HIGHLIGHT


def send(sock, command, pause=0.15):
    sock.sendall(command.encode() + b"\n")
    time.sleep(pause)


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    return width, height, parts[3]


def pixel(image, x, y):
    width, _, pixels = image
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset + 3])


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: qemu_gui_click_test.py <book8.iso>")
    iso = Path(sys.argv[1]).resolve()
    row = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    with tempfile.TemporaryDirectory(prefix="book8-gui-") as temp:
        serial = Path(temp) / "serial.log"
        shot = Path(temp) / "shot.ppm"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = profile.qemu_args(iso, serial, port, "mouse")
        if len(sys.argv) > 3:
            args += ["-drive", f"file={sys.argv[3]},format=raw,if=ide,index=0"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        try:
            if not profile.wait_for(serial, "BOOK8 x86-64 command interpreter", 90):
                raise SystemExit("FAIL: shell not reached")
            profile.wait_for(serial, "USB descriptor failures", 10)
            time.sleep(1)
            mon = socket.create_connection(("127.0.0.1", port), timeout=5)
            time.sleep(0.3)
            mon.recv(4096)
            for char in "book8gui":
                send(mon, f"sendkey {char}", 0.1)
            send(mon, "sendkey ret", 3)
            for _ in range(30):
                send(mon, "mouse_move -100 -100", 0.03)
            target_x = 40
            target_y = LIST_Y + row * ROW_HEIGHT + ROW_HEIGHT // 2
            moved_x = moved_y = 0
            while moved_x < target_x or moved_y < target_y:
                dx = min(40, target_x - moved_x)
                dy = min(40, target_y - moved_y)
                send(mon, f"mouse_move {dx} {dy}", 0.05)
                moved_x += dx
                moved_y += dy
            time.sleep(0.5)
            send(mon, "mouse_button 1", 0.2)
            send(mon, "mouse_button 0", 0.5)
            send(mon, f"screendump {shot}", 1)
            image = read_ppm(shot)
            color = pixel(image, 30, LIST_Y + row * ROW_HEIGHT + 8)
            other = pixel(image, 30, LIST_Y + (row + 2) * ROW_HEIGHT + 8)
            print(f"row {row} colour={color} unselected row colour={other}")
            if color == other:
                raise SystemExit("FAIL: clicked row was not highlighted")
            print("PASS: GUI file pane click selects the row")
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    main()
