#!/usr/bin/env python3
"""Boot BOOK8 on a QEMU profile approximating the GIGABYTE GA-78LMT-USB3.

Board: AMD 760G northbridge + SB710 southbridge, AM3+ (FX), DDR3.
QEMU has no AMD 760G/SB710 model, so this uses the i440fx machine and
reproduces what BOOK8 can observe:
  * no UHCI at all: five OHCI functions, one standalone (SB7xx OHCI4),
    two behind each EHCI (QEMU OHCI has 3 ports) (OHCI is the SB7xx full/low-speed path; EHCI
    ports are routed to OHCI companions);
  * keyboard input only through the i8042 (PS/2 port 0x60/0x64), which is what
    the BIOS USB-legacy SMM translation presents on the real board;
  * one xHCI (the board's add-on USB 3.0 controller); bare-metal counts\n    are 8/0/5/2/1 total/UHCI/OHCI/EHCI/xHCI;
  * USB pointing devices attached to the OHCI side, which BOOK8 detects but
    does not drive (OHCI/EHCI HID is unimplemented);
  * Piledriver-class CPU (Opteron_G5 is QEMU's closest) with 8 vCPUs and DDR3
    sized RAM (4 GiB).
It does not emulate SMM, the BIOS legacy USB trap, SB7xx PCI config
registers or the SB710 PCI config registers. PS/2 keystrokes injected via
the monitor therefore stand in for the BIOS-translated USB keyboard.
"""
import errno
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXPECTED_COUNTS = "8/0/5/2/1"


def qemu_args(iso, serial_path, monitor_port, pointer="none"):
    args = [
        os.environ.get("QEMU", "qemu-system-x86_64"),
        "-machine", "pc",
        "-m", "4096",
        "-cpu", "Opteron_G5",
        "-smp", "8",
        "-device", "ich9-usb-ehci1,id=ehci1,bus=pci.0,addr=0x12.0x2,"
                   "multifunction=on",
        "-device", "pci-ohci,masterbus=ehci1.0,firstport=0,bus=pci.0,"
                   "addr=0x12.0x0,multifunction=on,id=ohci0",
        "-device", "pci-ohci,masterbus=ehci1.0,firstport=3,bus=pci.0,"
                   "addr=0x12.0x1,multifunction=on,id=ohci1",
        "-device", "ich9-usb-ehci2,id=ehci2,bus=pci.0,addr=0x13.0x2,"
                   "multifunction=on",
        "-device", "pci-ohci,masterbus=ehci2.0,firstport=0,bus=pci.0,"
                   "addr=0x13.0x0,multifunction=on,id=ohci2",
        "-device", "pci-ohci,masterbus=ehci2.0,firstport=3,bus=pci.0,"
                   "addr=0x13.0x1,multifunction=on,id=ohci3",
        "-device", "pci-ohci,bus=pci.0,addr=0x14.0x0,id=ohci4",
        "-device", "qemu-xhci,bus=pci.0,addr=0x15.0x0,id=xhci",
    ]
    if pointer == "mouse":
        args += ["-device", "usb-mouse,bus=ehci1.0,port=1"]
    elif pointer == "tablet":
        args += ["-device", "usb-tablet,bus=ehci1.0,port=1"]
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


def exit_desktop_with(send, serial):
    log = serial.read_text(errors="replace")
    match = re.search(r"BOOK8 framebuffer handoff:\s*(\d+)x(\d+)x", log)
    if not match:
        raise RuntimeError("framebuffer dimensions unavailable")
    height = int(match.group(2))
    for _ in range(30):
        send("mouse_move -100 -100")
    x = y = 0
    for target_x, target_y in ((20, height - 18), (40, height - 50),
                               (200, height - 50)):
        while x != target_x or y != target_y:
            dx = max(-40, min(40, target_x - x))
            dy = max(-40, min(40, target_y - y))
            send(f"mouse_move {dx} {dy}")
            x += dx
            y += dy
        send("mouse_button 1")
        send("mouse_button 0")


def exit_desktop(port, serial):
    exit_desktop_with(lambda command: monitor(port, command), serial)


def wait_for(path, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if path.exists() and text in path.read_text(errors="replace"):
                return True
        except OSError as error:
            if error.errno != errno.ENODATA:
                raise
        time.sleep(0.5)
    return False


def main():
    if len(sys.argv) not in (2, 3) or (
            len(sys.argv) == 3 and sys.argv[2] not in ("none", "mouse", "tablet")):
        raise SystemExit(
            "usage: qemu_amd_sb710_profile.py <book8.iso> [none|mouse|tablet]")
    iso = Path(sys.argv[1]).resolve()
    pointer = sys.argv[2] if len(sys.argv) == 3 else "none"
    with tempfile.TemporaryDirectory(prefix="book8-sb710-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        process = subprocess.Popen(qemu_args(iso, serial, port, pointer))
        try:
            if not wait_for(serial, "BOOK8 x86-64 command interpreter", 90):
                raise SystemExit("FAIL: BOOK8 did not reach the command interpreter")
            wait_for(serial, "USB descriptor failures", 10)
            time.sleep(1)
            for key in ("v", "e", "r", "ret"):
                monitor(port, f"sendkey {key}")
            time.sleep(2)
            log = serial.read_text(errors="replace")
            start = log.find("USB host controllers")
            print(log[start:][:900] if start >= 0 else "(no USB telemetry)")
            failures = []
            if EXPECTED_COUNTS not in log:
                failures.append(
                    f"controller counts total/UHCI/OHCI/EHCI/XHCI "
                    f"!= {EXPECTED_COUNTS}")
            clean = re.sub(r"book8-diag:[^\n]*\n", "", log.lower())
            if "a:\\> ver" not in clean:
                failures.append("PS/2 (BIOS-legacy stand-in) keyboard echo "
                                "of 'ver' not seen")
            if failures:
                raise SystemExit("FAIL: " + "; ".join(failures))
            print(f"SB710 profile ({pointer}): PASS "
                  f"(counts {EXPECTED_COUNTS}, PS/2 keyboard works)")
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    main()
