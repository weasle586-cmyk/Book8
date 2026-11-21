#!/usr/bin/env python3
"""Boot BOOK8 under QEMU with a FAT data disk and run user programs from it.

Checks the whole storage path: IDE block device -> FAT reader -> ELF loader ->
Ring 3 -> sys_open/read/close/execve -> exit status back in the shell.

usage: qemu_exec_test.py <book8.iso> <data.img>
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402
import qemu_graphics_input_smoke as smoke  # noqa: E402
from qemu_desktop_test import QemuMonitor  # noqa: E402
from qemu_utilities_test import type_text  # noqa: E402

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "\n": "ret",
          ":": "shift-semicolon"}


def type_line(monitor, text):
    for ch in text + "\n":
        monitor.command(f"sendkey {KEYMAP.get(ch, ch)}", 0.1)


# (command, strings that must appear in the new serial output)
CASES = [
    ("netstat", ["PUSH not built accepted/rejected/challenges/busy="]),
    ("exec nvmap.elf",
     ["NVMAP: target absent; PMC register not read",
      "NVMAP: local diagnostics only; telemetry submission disabled",
      "EXEC: nvmap.elf exited with status 0"]),
    ("exec a:/nvmap.elf",
     ["NVMAP: local diagnostics only; telemetry submission disabled",
      "EXEC: a:/nvmap.elf exited with status 0"]),
    ("exec hello.elf alpha beta",
     ["HELLO: running at CPL3 from an ELF on disk",
      "HELLO: interrupts enabled", "HELLO: framebuffer syscalls ok", "HELLO: argc=3 pid=",
      "HELLO: argv[0]=hello.elf", "HELLO: argv[1]=alpha",
      "HELLO: argv[2]=beta", "HELLO: data/bss ok",
      "EXEC: hello.elf exited with status 7"]),
    ("exec cat.elf",
     ["CAT: MSG: read from a FAT subdirectory via sys_open/sys_read",
      "CAT: failed package execve preserved image",
      "CAT: execve hello", "HELLO: interrupts enabled", "HELLO: framebuffer syscalls ok",
      "HELLO: argc=2 pid=", "HELLO: argv[1]=from-cat",
      "EXEC: cat.elf exited with status 7"]),
    ("exec hello.elf a b c d e f g h i j k l m n o p",
     ["EXEC: too many arguments (maximum 16)"]),
    ("exec data/msg.txt", ["EXEC: cannot execute data/msg.txt"]),
    ("exec nothere.elf", ["EXEC: nothere.elf: file not found"]),
]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: qemu_exec_test.py <book8.iso> <data.img>")
    iso = Path(sys.argv[1]).resolve()
    image = Path(sys.argv[2]).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="book8-exec-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = amd.qemu_args(iso, serial, port, "none")
        args += ["-drive", f"file={image},format=raw,if=ide,index=0,snapshot=on"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        monitor = None
        try:
            if not amd.wait_for(serial,
                    'WINDOW id=1 title="BOOK8 Sign In" registered', 90):
                raise SystemExit("FAIL: BOOK8 did not reach sign-in")
            width, height, _ = smoke.verify_framebuffer(
                serial.read_text(errors="replace"))
            monitor = QemuMonitor(port, process)
            monitor.set_screen_size(width, height)
            monitor.reset_pointer()
            type_text(monitor, "default\n")
            if not amd.wait_for(serial, "BOOK8-SESSION: login accepted uid=0", 30):
                raise SystemExit("FAIL: QEMU exec test could not authenticate admin")
            if not amd.wait_for(serial, "BOOK8-DESKTOP: TASKBAR", 20):
                raise SystemExit("FAIL: BOOK8 desktop did not start after sign-in")
            monitor.click(20, height - 18)
            monitor.click(40, height - 50)
            monitor.click(200, height - 50)
            if not amd.wait_for(serial, "BOOK8GUI closed.", 20):
                raise SystemExit("FAIL: could not return to the authenticated shell")
            if not amd.wait_for(serial, "USB descriptor failures", 10):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            time.sleep(1)
            boot = serial.read_text(errors="replace")
            if "BOOK8-NVMAP:" in boot or "NVMAP: snapshot accepted" in boot:
                failures.append(("unexpected nvmap autostart", boot[-1200:]))
            else:
                print(f"[{image.name}] no nvmap autostart: PASS")
            for command, expected in CASES:
                start = len(serial.read_text(errors="replace"))
                type_line(monitor, command)
                deadline = time.time() + 40
                while time.time() < deadline:
                    result = serial.read_text(errors="replace")[start:]
                    if all(marker in result for marker in expected):
                        break
                    time.sleep(1)
                time.sleep(1)
                new = serial.read_text(errors="replace")[start:]
                missing = [e for e in expected if e not in new]
                print(f"[{image.name}] {command}: "
                      f"{'PASS' if not missing else 'FAIL ' + str(missing)}")
                if missing:
                    failures.append((command, new[-800:]))
        finally:
            if monitor is not None:
                monitor.close()
            process.terminate()
            process.wait()
    for command, tail in failures:
        print(f"--- output after '{command}':\n{tail}")
    if failures:
        raise SystemExit("FAIL: exec test")
    print(f"PASS: exec test ({image.name})")


if __name__ == "__main__":
    main()
