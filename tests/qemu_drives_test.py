#!/usr/bin/env python3
"""Boot BOOK8 with two FAT data disks and check multi-drive registration.

usage: qemu_drives_test.py <book8.iso> <disk-a.img> <disk-b.img>
Checks that DRIVES lists A: and B:, that B: switches the shell drive, that
an unassigned letter reports a clean error, and that DIR still works.
"""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "\n": "ret",
          ":": "shift-semicolon"}

CASES = [
    ("drives", ["A: FAT", "B: FAT"]),
    ("b:", ["B:\\"]),
    ("dir", ["Volume"]),
    ("a:", ["A:\\"]),
    ("z:", ["No such drive."]),
]


def type_line(port, text):
    for ch in text + "\n":
        amd.monitor(port, f"sendkey {KEYMAP.get(ch, ch)}")


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    iso = Path(sys.argv[1]).resolve()
    images = [Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()]
    failures = []
    with tempfile.TemporaryDirectory(prefix="book8-drives-") as temp:
        serial = Path(temp) / "serial.log"
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        args = amd.qemu_args(iso, serial, port, "none")
        for index, image in enumerate(images):
            args += ["-drive", f"file={image},format=raw,if=ide,"
                     f"index={index},snapshot=on"]
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        try:
            if not amd.wait_for(serial, "USB descriptor failures", 90):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                log = serial.read_text(errors="replace")
                if "BOOK8-DIAG: M2 prompt printed" in log:
                    break
                if "BOOK8-DESKTOP: TASKBAR" in log:
                    amd.exit_desktop(port, serial)
                    if not amd.wait_for(serial, "BOOK8GUI closed.", 20):
                        raise SystemExit("FAIL: GUI did not return to prompt")
                    break
                time.sleep(0.2)
            else:
                raise SystemExit("FAIL: neither shell nor desktop became ready")
            for command, expected in CASES:
                start = len(serial.read_text(errors="replace"))
                type_line(port, command)
                time.sleep(4)
                new = serial.read_text(errors="replace")[start:]
                missing = [e for e in expected if e not in new]
                print(f"{command}: "
                      f"{'PASS' if not missing else 'FAIL ' + str(missing)}")
                if missing:
                    failures.append((command, new[-600:]))
        finally:
            process.terminate()
            process.wait()
    for command, tail in failures:
        print(f"--- output after '{command}':\n{tail}")
    if failures:
        raise SystemExit("FAIL: drives test")
    print("PASS: multi-drive test")


if __name__ == "__main__":
    main()
