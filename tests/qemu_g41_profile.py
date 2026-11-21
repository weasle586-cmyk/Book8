#!/usr/bin/env python3
"""Boot BOOK8 on a QEMU profile approximating the Foxconn G41T-DM (G41 + ICH7).

QEMU has no ICH7/G41 model, so this uses Q35 (ICH9): four UHCI functions at
00:1d.0-3 plus one EHCI at 00:1d.7 (the ICH7 USB layout; three UHCI are
companions of the EHCI, the fourth is standalone because QEMU's EHCI has six
ports), the i8042 disabled to mimic the board's USB-only rear panel, 4 GiB
RAM, and AHCI SATA. It does not emulate SMM, BIOS USB-legacy trapping, or the
ICH7 USB_LEGKEY semantics beyond QEMU's default PCI 0xC0 register.

Bare-metal G41 sequence (latest capture): 1280x1024x32 framebuffer handoff
and command interpreter come up, telemetry reads 5/4/0/1/0 controllers, EHCI
legacy handoffs 1, companion mappings/static fallbacks 4/1, then the machine
freezes dead as soon as the "A:" prompt is drawn. The freeze is therefore
after USB init, in the first interactive-loop iteration (keyboard/USB poll,
interrupt or SMI/PIRQ activity); QEMU cannot reproduce it. Use this profile
only to compare the controller topology and the 'ehci' companion-routing
telemetry (0/0/0/0/0), which is an open, undiagnosed gap.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def qemu_args(iso, serial_path, monitor_port, keyboard="ehci"):
    args = [
        os.environ.get("QEMU", "qemu-system-x86_64"),
        "-machine", "q35,i8042=off",
        "-m", "1024",
        "-cpu", "Penryn",
        "-smp", "2",
        "-device", "ich9-usb-ehci1,id=ehci,bus=pcie.0,addr=0x1d.0x7,"
                   "multifunction=on",
        "-device", "ich9-usb-uhci1,masterbus=ehci.0,firstport=0,"
                   "bus=pcie.0,addr=0x1d.0x0,multifunction=on,id=uhci1",
        "-device", "ich9-usb-uhci2,masterbus=ehci.0,firstport=2,"
                   "bus=pcie.0,addr=0x1d.0x1,multifunction=on",
        "-device", "ich9-usb-uhci3,masterbus=ehci.0,firstport=4,"
                   "bus=pcie.0,addr=0x1d.0x2,multifunction=on",
        "-device", "ich9-usb-uhci4,bus=pcie.0,addr=0x1d.0x3,"
                   "multifunction=on,id=uhci4",
    ]
    if keyboard == "ehci":
        args += ["-device", "usb-kbd,bus=ehci.0,port=1"]
    elif keyboard == "uhci4":
        args += ["-device", "usb-kbd,bus=uhci4.0,port=1"]
    elif keyboard == "companion":
        args += ["-device", "usb-kbd,bus=ehci.0,port=1,usb_version=1"]
    args += [
        "-cdrom", str(iso), "-boot", "d",
        "-display", "none", "-vga", "std",
        "-serial", f"file:{serial_path}",
        "-monitor", f"tcp:127.0.0.1:{monitor_port},server,nowait",
        "-no-reboot",
    ]
    return args


def monitor(port, command):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        time.sleep(0.2)
        sock.sendall(command.encode() + b"\n")
        time.sleep(0.2)


def wait_for(path, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and text in path.read_text(errors="replace"):
            return True
        time.sleep(0.5)
    return False


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(
            "usage: qemu_g41_profile.py <book8.iso> [ehci|companion|uhci4|none]")
    iso = Path(sys.argv[1]).resolve()
    keyboard = sys.argv[2] if len(sys.argv) == 3 else "ehci"
    with tempfile.TemporaryDirectory(prefix="book8-g41-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        process = subprocess.Popen(qemu_args(iso, serial, port, keyboard))
        try:
            if not wait_for(serial, "BOOK8 x86-64 command interpreter", 90):
                raise SystemExit("FAIL: BOOK8 did not reach the command interpreter")
            wait_for(serial, "USB descriptor failures", 10)
            time.sleep(1)
            for key in ("v", "e", "r", "ret"):
                monitor(port, f"sendkey {key}")
            time.sleep(2)
            log = serial.read_text(errors="replace")
            print(log[log.find("USB host controllers"):][:900])
            print(f"keyboard={keyboard} typed-command echoed on serial:",
                  "A:\\> ver" in log.lower())
            print("G41 profile boot: reached shell")
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    main()
