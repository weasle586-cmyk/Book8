#!/usr/bin/env python3
"""Verify MADT/AP startup and affinity-pinned scheduling at 2, 4, 6 and 8 vCPUs."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_amd_sb710_profile as amd  # noqa: E402


def send_command(port, command):
    for key in command.lower():
        if key == " ":
            amd.monitor(port, "sendkey spc")
        else:
            amd.monitor(port, f"sendkey {key}")
    amd.monitor(port, "sendkey ret")


def run_configuration(iso, cpu_count):
    with tempfile.TemporaryDirectory(prefix=f"book8-smp-{cpu_count}-") as temp:
        serial = Path(temp) / "serial.log"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        args = amd.qemu_args(iso, serial, port, "none")
        args[args.index("-smp") + 1] = str(cpu_count)
        process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
        try:
            if not amd.wait_for(serial, "BOOK8-SMP: online CPUs=", 90):
                raise SystemExit(
                    f"FAIL: BOOK8 did not initialize SMP at -smp {cpu_count}")
            log = serial.read_text(errors="replace")
            expected_startup = (
                f"BOOK8-SMP: online CPUs={cpu_count} detected={cpu_count}")
            if expected_startup not in log:
                raise SystemExit(
                    f"FAIL: expected '{expected_startup}' at -smp {cpu_count}; "
                    f"startup log: {log[-2500:]}")
            start = len(log)
            try:
                send_command(port, "smptest")
            except OSError as error:
                log = serial.read_text(errors="replace")
                raise SystemExit(
                    f"FAIL: QEMU monitor unavailable at -smp {cpu_count}; "
                    f"exit={process.poll()} error={error}; "
                    f"serial log: {log[-4000:]}") from error
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                result = serial.read_text(errors="replace")[start:]
                if "BOOK8-SMP-TEST:" in result:
                    break
                time.sleep(0.1)
            else:
                log = serial.read_text(errors="replace")
                raise SystemExit(
                    f"FAIL: SMP scheduler test timed out at -smp {cpu_count}; "
                    f"serial log: {log[-4000:]}")
            expected_test = (
                f"BOOK8-SMP-TEST: PASS cores={cpu_count} online={cpu_count}")
            if expected_test not in result:
                raise SystemExit(
                    f"FAIL: expected '{expected_test}' at -smp {cpu_count}; "
                    f"test log: {result[-2500:]}")
            print(expected_startup)
            print(expected_test)
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: qemu_smp_test.py <book8.iso>")
    iso = Path(sys.argv[1]).resolve()
    for cpu_count in (2, 4, 6, 8):
        run_configuration(iso, cpu_count)
    print("PASS: QEMU SMP startup and affinity scheduling at -smp 2, 4, 6 and 8")


if __name__ == "__main__":
    main()
