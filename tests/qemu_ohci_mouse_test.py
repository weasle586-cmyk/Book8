#!/usr/bin/env python3
"""OHCI HID mouse test on the SB710-like profile (no UHCI, EHCI companions).

Boots BOOK8, checks the OHCI-attached QEMU usb-mouse enumerates, then starts
BOOK8GUI, injects relative motion through the QEMU monitor and requires
INPUTSTAT to report USB_REPORTS > 0 with no queue drops.
"""
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import qemu_amd_sb710_profile as profile  # noqa: E402


def keys(port, text):
    for char in text:
        profile.monitor(port, f"sendkey {'ret' if char == chr(10) else char}")
        time.sleep(0.1)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: qemu_ohci_mouse_test.py <book8.iso> [disposable-data.img]")
    iso = Path(sys.argv[1]).resolve()
    image = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    if image is not None and not image.is_file():
        raise SystemExit("FAIL: expected a regular disposable data image")
    with tempfile.TemporaryDirectory(prefix="book8-ohci-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = profile.qemu_args(iso, serial, port, "mouse")
        if image is not None:
            args += ["-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on"]
        process = subprocess.Popen(
            args,
            stderr=subprocess.DEVNULL)
        try:
            if not profile.wait_for(serial, "BOOK8 x86-64 command interpreter", 90):
                raise SystemExit("FAIL: shell not reached")
            profile.wait_for(serial, "USB descriptor failures", 10)
            time.sleep(1)
            log = serial.read_text(errors="replace")
            if "BOOK8-NVMAP:" in log or "NVMAP: snapshot accepted" in log:
                raise SystemExit("FAIL: NVMAP unexpectedly ran during boot")
            if "USB HID mouse active" not in log:
                raise SystemExit("FAIL: OHCI mouse not active")
            if not re.search(r"OHCI started/ports/connected/HID/hubs/reports/errors: 5/15/1/1/", log):
                raise SystemExit("FAIL: OHCI enumeration counters unexpected")
            keys(port, "book8gui\n")
            if not profile.wait_for(
                    serial, 'WINDOW id=1 title="BOOK8 Sign In" registered', 20):
                raise SystemExit("FAIL: sign-in window did not appear")
            keys(port, "default\n")
            if not profile.wait_for(
                    serial, "BOOK8-SESSION: login accepted uid=0", 30):
                raise SystemExit("FAIL: could not authenticate admin")
            if not profile.wait_for(serial, "BOOK8-DESKTOP: TASKBAR", 20):
                raise SystemExit("FAIL: desktop did not start after sign-in")
            time.sleep(3)
            for step in range(40):
                profile.monitor(port, f"mouse_move {5 + step % 3} {3}")
                time.sleep(0.05)
            time.sleep(1)
            keys(port, "")
            profile.exit_desktop(port, serial)
            if not profile.wait_for(serial, "BOOK8GUI closed.", 20):
                output = serial.read_text(errors="replace") if serial.exists() else ""
                raise SystemExit(
                    "FAIL: GUI did not return to prompt\n" + output[-4000:]
                )
            keys(port, "inputstat\n")
            time.sleep(2)
            log = serial.read_text(errors="replace")
            match = re.search(r"USB_REPORTS=(\d+) USB_QUEUED=(\d+) USB_QUEUE_DROPS=(\d+)", log)
            if not match:
                raise SystemExit("FAIL: no INPUTSTAT output")
            reports, queued, drops = map(int, match.groups())
            if reports == 0 or queued == 0 or drops != 0:
                raise SystemExit(f"FAIL: reports={reports} queued={queued} drops={drops}")
            print(f"PASS: OHCI mouse test (USB_REPORTS={reports} "
                  f"USB_QUEUED={queued} drops={drops})")
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    main()
