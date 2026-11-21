#!/usr/bin/env python3
"""Exercise timer-driven kernel context switches and return to the shell."""
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402


def run_vm_test(serial, port):
    start = len(serial.read_text(errors="replace"))
    for key in "vmtest":
        amd.monitor(port, f"sendkey {key}")
    amd.monitor(port, "sendkey ret")
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        log = serial.read_text(errors="replace")[start:]
        if "VMTEST:" in log:
            if "VMTEST: PASS" not in log:
                raise SystemExit(f"FAIL: address-space test: {log[-2000:]}")
            return
        time.sleep(0.1)
    raise SystemExit("FAIL: address-space test timed out")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_scheduler_test.py <book8-cli.iso>")
    iso = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="book8-scheduler-") as temp:
        serial = Path(temp) / "serial.log"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen(amd.qemu_args(iso, serial, port, "none"),
                                   stderr=subprocess.DEVNULL)
        try:
            if not amd.wait_for(serial, "USB descriptor failures", 90):
                raise SystemExit("FAIL: BOOK8 did not reach the interpreter")
            run_vm_test(serial, port)
            run_vm_test(serial, port)
            for attempt in range(2):
                start = len(serial.read_text(errors="replace"))
                for key in "schedtest":
                    amd.monitor(port, f"sendkey {key}")
                amd.monitor(port, "sendkey ret")
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    log = serial.read_text(errors="replace")[start:]
                    if "SCHEDTEST:" in log:
                        break
                    time.sleep(0.1)
                else:
                    raise SystemExit("FAIL: scheduler test timed out")
                if "SCHEDTEST: PASS" not in log:
                    raise SystemExit(f"FAIL: scheduler test {attempt + 1}: {log[-2000:]}")
            for attempt in range(2):
                start = len(serial.read_text(errors="replace"))
                for key in "scheduser":
                    amd.monitor(port, f"sendkey {key}")
                amd.monitor(port, "sendkey ret")
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    log = serial.read_text(errors="replace")[start:]
                    if "SCHEDUSER:" in log:
                        break
                    time.sleep(0.1)
                else:
                    raise SystemExit("FAIL: user preemption test timed out")
                if "SCHEDUSER: PASS" not in log:
                    raise SystemExit(f"FAIL: user preemption test {attempt + 1}: {log[-2000:]}")
            for key in "ring3test":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            if not amd.wait_for(serial, "RING3TEST: PASS", 30):
                raise SystemExit("FAIL: Ring 3 did not work after scheduling")
            run_vm_test(serial, port)
            for key in "inputstat":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            if not amd.wait_for(serial, "BOOK8-INPUT:", 20):
                raise SystemExit("FAIL: initial input status unavailable")
            before_log = serial.read_text(errors="replace")
            before = re.findall(r"BOOK8-INPUT:.*IRQ0=(\d+)", before_log)
            if not before:
                raise SystemExit("FAIL: timer IRQ counter missing")
            for key in "book8gui":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            if not amd.wait_for(serial, "Launching BOOK8GUI.", 20):
                raise SystemExit("FAIL: GUI failed to start")
            if not amd.wait_for(serial, 'title="BOOK8 Sign In" registered', 20):
                raise SystemExit("FAIL: sign-in window missing")
            for key in "default":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            if not amd.wait_for(serial, "BOOK8-SESSION: login accepted uid=0", 20):
                raise SystemExit("FAIL: GUI sign-in failed")
            time.sleep(2)
            amd.monitor(port, "mouse_move 80 50")
            amd.monitor(port, "mouse_button 1")
            amd.monitor(port, "mouse_button 0")
            amd.exit_desktop(port, serial)
            if not amd.wait_for(serial, "BOOK8GUI closed.", 20):
                log = serial.read_text(errors="replace")
                raise SystemExit(f"FAIL: GUI failed to close: {log[-3000:]}")
            for key in "inputstat":
                amd.monitor(port, f"sendkey {key}")
            amd.monitor(port, "sendkey ret")
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                log = serial.read_text(errors="replace")
                lines = [line for line in log.splitlines() if "BOOK8-INPUT:" in line]
                if len(lines) >= 2:
                    break
                time.sleep(0.1)
            else:
                raise SystemExit("FAIL: final input status unavailable")
            values = dict(re.findall(r"([A-Z0-9_]+)=(\d+)", lines[-1]))
            if (int(values.get("IRQ0", "0")) <= int(before[-1])
                    or int(values.get("IRQ12", "0")) == 0
                    or int(values.get("PS2_QUEUED", "0")) == 0
                    or int(values.get("PS2_QUEUE_DROPS", "-1")) != 0):
                raise SystemExit("FAIL: GUI timer/mouse IRQs: " + lines[-1])
            print(lines[-1])
        finally:
            process.terminate()
            process.wait()
    print("PASS: address-space isolation, timer preemption, Ring 3 and GUI timer/mouse IRQs")


if __name__ == "__main__":
    main()
