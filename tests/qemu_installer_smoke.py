#!/usr/bin/env python3
import os
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import qemu_amd_sb710_profile as amd


def wait_for_text(log_path, process, expected, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"QEMU exited early while waiting for {expected!r}:\n"
                + log_path.read_text(errors="replace")
            )
        log = log_path.read_text(errors="replace")
        if expected in log:
            return log
        time.sleep(0.1)
    raise TimeoutError(
        f"Timed out waiting for {expected!r}:\n"
        + log_path.read_text(errors="replace")
    )


def wait_for_occurrences(log_path, process, expected, count, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"QEMU exited early while waiting for {expected!r}:\n"
                + log_path.read_text(errors="replace")
            )
        log = log_path.read_text(errors="replace")
        if log.count(expected) >= count:
            return log
        time.sleep(0.1)
    raise TimeoutError(
        f"Timed out waiting for {count} occurrences of {expected!r}:\n"
        + log_path.read_text(errors="replace")
    )


def monitor_command(port, command):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as monitor:
        monitor.settimeout(1)
        try:
            monitor.recv(4096)
        except TimeoutError:
            pass
        monitor.sendall(command.encode("ascii") + b"\n")
        time.sleep(0.1)


class QmpClient:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.settimeout(5)
        self.buffer = b""
        self.next_id = 1
        self._read_message()
        self.command("qmp_capabilities")

    def _read_message(self):
        while b"\n" not in self.buffer:
            data = self.socket.recv(4096)
            if not data:
                raise ConnectionError("QMP closed the connection.")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        if not line:
            return self._read_message()
        return json.loads(line)

    def command(self, execute, arguments=None):
        command_id = self.next_id
        self.next_id += 1
        payload = {"execute": execute, "id": command_id}
        if arguments is not None:
            payload["arguments"] = arguments
        self.socket.sendall(json.dumps(payload).encode("ascii") + b"\n")
        while True:
            response = self._read_message()
            if response.get("id") != command_id:
                continue
            if "error" in response:
                raise RuntimeError(f"QMP {execute} failed: {response['error']}")
            return response.get("return")

    def key(self, qcode, down):
        self.command(
            "input-send-event",
            {
            "events": [
                    {
                        "type": "key",
                        "data": {
                            "key": {"type": "qcode", "data": qcode},
                            "down": down,
                        },
                    }
                ],
            },
        )

    def type_text(self, text):
        for character in text:
            qcode = "space" if character == " " else character.lower()
            self.key(qcode, True)
            self.key(qcode, False)
        self.key("ret", True)
        self.key("ret", False)

    def pointer_absolute(self, x, y, width, height):
        x_value = round(x * 32767 / (width - 1))
        y_value = round(y * 32767 / (height - 1))
        self.command(
            "input-send-event",
            {
                "events": [
                    {"type": "abs", "data": {"axis": "x", "value": x_value}},
                    {"type": "abs", "data": {"axis": "y", "value": y_value}},
                ]
            },
        )

    def mouse_button(self, button, down):
        self.command(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "btn",
                        "data": {"button": button, "down": down},
                    }
                ]
            },
        )

    def close(self):
        self.socket.close()


def start_qemu(args, log_path):
    log = log_path.open("wb")
    process = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    process._book8_log_file = log
    return process


def stop_qemu(process, monitor_port=None):
    if process.poll() is None and monitor_port is not None:
        try:
            monitor_command(monitor_port, "quit")
        except OSError:
            pass
    if process.poll() is None:
        process.terminate()
    process.wait(timeout=10)
    process._book8_log_file.close()


def wait_for_shell(log_path, process, timeout=60):
    wait_for_text(log_path, process, "A:\\> ", timeout)


def verify_framebuffer_handoff(log_path, process):
    log = wait_for_text(log_path, process, "BOOK8 framebuffer handoff:", 60)
    match = re.search(
        r"BOOK8 framebuffer handoff: (\d+)x(\d+)x(24|32) "
        r"pitch=(\d+) address=0x([0-9a-fA-F]+)",
        log,
    )
    if match is None:
        raise RuntimeError(f"Installer did not initialize a linear framebuffer:\n{log}")
    width, height, bpp, pitch = (int(value) for value in match.groups()[:4])
    address = int(match.group(5), 16)
    if (width < 640 or height < 480 or pitch < width * (bpp // 8)
            or address == 0):
        raise RuntimeError(
            f"Installer framebuffer is below the supported minimum: "
            f"{width}x{height}x{bpp}, pitch={pitch}, address=0x{address:x}"
        )
    return width, height, bpp, pitch


def available_monitor_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def verify_ps2_keyboard(qemu, iso, temp_path):
    log_path = temp_path / "ps2-keyboard.log"
    monitor_port = available_monitor_port()
    process = start_qemu(
        [
            qemu,
            "-machine",
            "pc",
            "-m",
            "256",
            "-cdrom",
            str(iso),
            "-boot",
            "d",
            "-display",
            "none",
            "-vga",
            "std",
            "-serial",
            "stdio",
            "-monitor",
            f"tcp:127.0.0.1:{monitor_port},server,nowait",
            "-no-reboot",
        ],
        log_path,
    )
    try:
        wait_for_shell(log_path, process)
        wait_for_text(
            log_path,
            process,
            "PS/2 keyboard controller configured for polling.",
            10,
        )
        for key in ("h", "e", "l", "p", "ret"):
            monitor_command(monitor_port, f"sendkey {key}")
        wait_for_text(log_path, process, "BOOK8 x86-64 commands:", 10)
    finally:
        stop_qemu(process, monitor_port)


def verify_usb_keyboard(qemu, iso, temp_path):
    log_path = temp_path / "usb-keyboard.log"
    qmp_port = available_monitor_port()
    process = start_qemu(
        [
            qemu,
            "-machine",
            "pc,i8042=off",
            "-m",
            "256",
            "-device",
            "piix3-usb-uhci,id=uhci",
            "-device",
            "usb-kbd,id=keyboard,bus=uhci.0",
            "-cdrom",
            str(iso),
            "-boot",
            "d",
            "-display",
            "none",
            "-vga",
            "std",
            "-serial",
            "stdio",
            "-qmp",
            f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
            "-no-reboot",
        ],
        log_path,
    )
    qmp = None
    try:
        wait_for_text(
            log_path, process, "USB HID keyboard active.", timeout=60
        )
        wait_for_text(
            log_path,
            process,
            "USB host controllers (total/UHCI/OHCI/EHCI/xHCI): 1/1/0/0/0;",
            timeout=10,
        )
        wait_for_shell(log_path, process)
        qmp = QmpClient(qmp_port)
        qmp.type_text("help")
        wait_for_text(log_path, process, "BOOK8 x86-64 commands:", 10)
        qmp.type_text("USBSTAT")
        wait_for_text(
            log_path, process, "USB HID poll (active/completions/stalls):", 10
        )
        wait_for_text(log_path, process, "UHCI[0] PCI=", 10)
        wait_for_text(log_path, process, "  io=", 10)
        qmp.type_text("BOOK8GUI")
        wait_for_text(log_path, process, "Launching BOOK8GUI.", 10)
        qmp.type_text("help")
        wait_for_occurrences(
            log_path, process, "BOOK8 x86-64 commands:", 2, 10
        )
        qmp.key("esc", True)
        qmp.key("esc", False)
        wait_for_text(log_path, process, "BOOK8GUI closed.", 10)
    finally:
        if qmp is not None:
            qmp.close()
        stop_qemu(process)


def verify_usb_mouse(qemu, iso, temp_path):
    log_path = temp_path / "usb-mouse.log"
    monitor_port = available_monitor_port()
    process = start_qemu(
        [
            qemu,
            "-machine",
            "pc",
            "-m",
            "256",
            "-device",
            "piix3-usb-uhci,id=uhci",
            "-device",
            "usb-mouse,bus=uhci.0",
            "-cdrom",
            str(iso),
            "-boot",
            "d",
            "-display",
            "none",
            "-vga",
            "std",
            "-serial",
            "stdio",
            "-monitor",
            f"tcp:127.0.0.1:{monitor_port},server,nowait",
            "-no-reboot",
        ],
        log_path,
    )
    try:
        wait_for_text(
            log_path, process, "USB HID mouse active", timeout=60
        )
        wait_for_shell(log_path, process)
        process.stdin.write(b"BOOK8GUI\r")
        process.stdin.flush()
        wait_for_text(log_path, process, "Launching BOOK8GUI.", 10)
        monitor_command(monitor_port, "mouse_move 350 250 0")
        monitor_command(monitor_port, "mouse_move 0 0 1")
        wait_for_text(log_path, process, " WHEEL=", 10)
        monitor_command(monitor_port, "mouse_button 1")
        monitor_command(monitor_port, "mouse_button 0")
        amd.exit_desktop_with(lambda command: monitor_command(monitor_port, command),
                              log_path)
        wait_for_text(log_path, process, "BOOK8GUI closed.", 10)
    finally:
        stop_qemu(process, monitor_port)


def verify_usb_tablet(qemu, iso, temp_path):
    log_path = temp_path / "usb-tablet.log"
    qmp_port = available_monitor_port()
    process = start_qemu(
        [
            qemu,
            "-machine",
            "pc",
            "-m",
            "256",
            "-device",
            "piix3-usb-uhci,id=uhci",
            "-device",
            "usb-tablet,bus=uhci.0",
            "-cdrom",
            str(iso),
            "-boot",
            "d",
            "-display",
            "none",
            "-vga",
            "std",
            "-serial",
            "stdio",
            "-qmp",
            f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
            "-no-reboot",
        ],
        log_path,
    )
    qmp = None
    try:
        wait_for_text(
            log_path, process, "USB HID mouse active", timeout=60
        )
        wait_for_shell(log_path, process)
        process.stdin.write(b"BOOK8GUI\r")
        process.stdin.flush()
        wait_for_text(log_path, process, "Launching BOOK8GUI.", 10)
        qmp = QmpClient(qmp_port)
        qmp.pointer_absolute(730, 52, 800, 600)
        time.sleep(0.25)
        qmp.mouse_button("left", True)
        time.sleep(0.1)
        qmp.mouse_button("left", False)
        wait_for_text(log_path, process, "BOOK8GUI closed.", 10)
    finally:
        if qmp is not None:
            qmp.close()
        stop_qemu(process)


def verify_reboot(qemu, iso, temp_path):
    log_path = temp_path / "reboot.log"
    process = start_qemu(
        [
            qemu,
            "-machine",
            "pc",
            "-m",
            "256",
            "-cdrom",
            str(iso),
            "-boot",
            "d",
            "-display",
            "none",
            "-vga",
            "std",
            "-serial",
            "stdio",
        ],
        log_path,
    )
    try:
        boot_message = "BOOK8 x86-64 command interpreter 0.2"
        wait_for_text(log_path, process, boot_message, 60)
        process.stdin.write(b"REBOOT\r")
        process.stdin.flush()
        wait_for_text(log_path, process, "BOOK8 reboot requested.", 10)
        wait_for_occurrences(log_path, process, boot_message, 2, 60)
    finally:
        stop_qemu(process)


def main():
    mode = sys.argv[2] if len(sys.argv) == 3 else None
    valid_modes = (None, "--install-only", "--installer-boot-only")
    if len(sys.argv) not in (2, 3) or mode not in valid_modes:
        raise SystemExit(
            "usage: qemu_installer_smoke.py <book8-cli.iso> "
            "[--install-only|--installer-boot-only]"
        )
    install_only = mode in ("--install-only", "--installer-boot-only")
    boot_only = mode == "--installer-boot-only"
    iso = Path(sys.argv[1]).resolve()
    if not iso.is_file():
        raise SystemExit(f"BOOK8 ISO not found: {iso}")

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    qemu_img = os.environ.get("QEMU_IMG", "qemu-img")
    with tempfile.TemporaryDirectory(prefix="book8-installer-qemu-") as temp:
        temp_path = Path(temp)
        target = temp_path / "target.img"
        subprocess.run(
            [qemu_img, "create", "-f", "raw", str(target), "8G"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        if not install_only:
            verify_ps2_keyboard(qemu, iso, temp_path)
            verify_usb_keyboard(qemu, iso, temp_path)
            verify_usb_mouse(qemu, iso, temp_path)
            verify_usb_tablet(qemu, iso, temp_path)
            verify_reboot(qemu, iso, temp_path)
        monitor_port = available_monitor_port()

        installer_log = temp_path / "installer.log"
        process = start_qemu(
            [
                qemu,
                "-machine",
                "pc",
                "-m",
                "256",
                "-cdrom",
                str(iso),
                "-boot",
                "d",
                "-drive",
                f"file={target},format=raw,if=ide,cache=unsafe",
                "-display",
                "none",
                "-vga",
                "std",
                "-serial",
                "stdio",
                "-monitor",
                f"tcp:127.0.0.1:{monitor_port},server,nowait",
                "-no-reboot",
            ],
            installer_log,
        )
        bios = None
        efi = None
        try:
            time.sleep(1)
            monitor_command(monitor_port, "sendkey down")
            monitor_command(monitor_port, "sendkey ret")
            framebuffer = verify_framebuffer_handoff(installer_log, process)
            wait_for_text(
                installer_log,
                process,
                "Installer payload loaded",
                timeout=60,
            )
            wait_for_text(
                installer_log,
                process,
                "BOOK8 x86-64 command interpreter",
                timeout=10,
            )
            wait_for_shell(installer_log, process)

            if boot_only:
                process.stdin.write(b"INSTALL\r")
                process.stdin.flush()
                wait_for_text(installer_log, process, "Available disks:", 10)
                wait_for_text(installer_log, process, "or Q to cancel):", 10)
                process.stdin.write(b"Q\r")
                process.stdin.flush()
                wait_for_text(installer_log, process, "Installation cancelled", 10)
                with target.open("rb") as untouched:
                    if untouched.read(512) != bytes(512):
                        raise RuntimeError(
                            "Installer boot-only regression modified its disposable target."
                        )
                print(
                    "PASS: GRUB Installer framebuffer "
                    f"{framebuffer[0]}x{framebuffer[1]}x{framebuffer[2]} "
                    f"pitch={framebuffer[3]}, installer UI and safe cancellation"
                )
                return

            process.stdin.write(b"DISK\r")
            process.stdin.flush()
            log = wait_for_text(installer_log, process, "IDE0-M:", 10)
            match = re.search(
                r"\[(\d+)\]\s+(IDE0-M): .*?\((\d+) sectors,", log
            )
            if match is None:
                raise RuntimeError(f"Could not identify the QEMU target disk:\n{log}")
            disk_index, disk_name, sectors = match.groups()

            process.stdin.write(b"INSTALL\r")
            process.stdin.flush()
            wait_for_text(installer_log, process, "Available disks:", 10)
            menu = wait_for_text(installer_log, process, "or Q to cancel):", 10)
            menu_match = re.search(r"(\d+)\) " + disk_name + r"\s", menu)
            if menu_match is None:
                raise RuntimeError(f"Target disk missing from menu:\n{menu}")
            choice = menu_match.group(1).encode("ascii")
            prompt = "Is this the correct disk? (Y/N):"
            process.stdin.write(choice + b"\r")
            process.stdin.flush()
            wait_for_text(installer_log, process, prompt, 10)
            process.stdin.write(b"N\r")
            process.stdin.flush()
            wait_for_text(installer_log, process, "Installation cancelled", 10)
            with target.open("rb") as installed:
                if installed.read(512) != bytes(512):
                    raise RuntimeError("A rejected confirmation modified the target disk.")

            process.stdin.write(b"INSTALL\r")
            process.stdin.flush()
            wait_for_occurrences(installer_log, process, "or Q to cancel):", 2, 10)
            process.stdin.write(choice + b"\r")
            process.stdin.flush()
            wait_for_occurrences(installer_log, process, prompt, 2, 10)
            process.stdin.write(b"Y\r")
            process.stdin.flush()
            wait_for_text(
                installer_log, process, "Delete the existing partitions", 10
            )
            process.stdin.write(b"Y\r")
            process.stdin.flush()
            wait_for_text(
                installer_log,
                process,
                "[INSTALL] copying installer image to target...",
                10,
            )
            wait_for_text(
                installer_log,
                process,
                "BOOK8 installed and verified",
                timeout=600,
            )
            stop_qemu(process, monitor_port)
            process = None

            root = Path(__file__).resolve().parent.parent
            for name, source in (
                ("boot/book8.elf", root / "build" / "book8.elf"),
                ("NVMAP.ELF", root / "build" / "user" / "nvmap.elf"),
                ("NVMAP.CFG", root / "user" / "nvmap.cfg"),
                ("BIN/BROWSER.ELF", root / "build" / "user" / "browser.elf"),
            ):
                extracted = subprocess.run(
                    ["mtype", "-i", f"{target}@@1048576", f"::/{name}"],
                    check=True, stdout=subprocess.PIPE,
                ).stdout
                if extracted != source.read_bytes():
                    raise RuntimeError(f"Installed {name} differs from build input")
            for name in ("BOOK8.CFG", "BOOK8REG.INI"):
                extracted = subprocess.run(
                    ["mtype", "-i", f"{target}@@1048576", f"::/{name}"],
                    check=True, stdout=subprocess.PIPE,
                ).stdout
                if not extracted:
                    raise RuntimeError(f"Installed {name} is empty")
            network_config = root / "build" / "BOOK8NET.CFG"
            if network_config.is_file():
                extracted = subprocess.run(
                    ["mtype", "-i", f"{target}@@1048576", "::/BOOK8NET.CFG"],
                    check=True, stdout=subprocess.PIPE,
                ).stdout
                if extracted != network_config.read_bytes():
                    raise RuntimeError("Installed BOOK8NET.CFG differs from build input")
                extracted.decode("ascii")

            bios_log = temp_path / "bios-installed.log"
            bios = start_qemu(
                [
                    qemu,
                    "-machine",
                    "pc",
                    "-m",
                    "256",
                    "-drive",
                    f"file={target},format=raw,if=ide",
                    "-boot",
                    "c",
                    "-display",
                    "none",
                    "-vga",
                    "std",
                    "-serial",
                    "stdio",
                    "-monitor",
                    "none",
                    "-no-reboot",
                ],
                bios_log,
            )
            verify_framebuffer_handoff(bios_log, bios)
            wait_for_text(
                bios_log, bios, "BOOK8-CONFIG: Loaded A:\\BOOK8.CFG.", timeout=30
            )
            wait_for_text(bios_log, bios, "BOOK8-DESKTOP: TASKBAR", timeout=30)
            wait_for_text(bios_log, bios, "Programs indexed: 16;", timeout=10)
            if "NVMAP:" in bios_log.read_text(errors="replace") or "BOOK8-PUSH:" in bios_log.read_text(errors="replace"):
                raise RuntimeError("installed BIOS boot started disabled diagnostics or push")
            stop_qemu(bios)
            bios = None

            efi_log = temp_path / "uefi-installed.log"
            efi = start_qemu(
                [
                    qemu,
                    "-machine",
                    "pc",
                    "-m",
                    "256",
                    "-bios",
                    "/usr/share/ovmf/OVMF.fd",
                    "-drive",
                    f"file={target},format=raw,if=ide",
                    "-boot",
                    "c",
                    "-display",
                    "none",
                    "-vga",
                    "std",
                    "-serial",
                    "stdio",
                    "-monitor",
                    "none",
                    "-no-reboot",
                ],
                efi_log,
            )
            verify_framebuffer_handoff(efi_log, efi)
            wait_for_text(
                efi_log, efi, "BOOK8-CONFIG: Loaded A:\\BOOK8.CFG.", timeout=30
            )
            wait_for_text(efi_log, efi, "BOOK8-DESKTOP: TASKBAR", timeout=30)
            wait_for_text(efi_log, efi, "Programs indexed: 16;", timeout=10)
            if "NVMAP:" in efi_log.read_text(errors="replace") or "BOOK8-PUSH:" in efi_log.read_text(errors="replace"):
                raise RuntimeError("installed UEFI boot started disabled diagnostics or push")
            stop_qemu(efi)
            efi = None
        finally:
            if process is not None:
                stop_qemu(process, monitor_port)
            if bios is not None:
                stop_qemu(bios)
            if efi is not None:
                stop_qemu(efi)

    if not install_only:
        print("PASS: PS/2 and USB keyboard input, USB mouse GUI input, REBOOT command")
    print(
        "PASS: QEMU "
        "installer cancellation/install, exact packaged files, "
        "and automatic desktop without NVMAP/push on BIOS/UEFI cold boot."
    )


if __name__ == "__main__":
    main()
