#!/usr/bin/env python3
"""Validate netshare ELF, GUI/offline alerts and uninstall on a disposable disk.

Stock QEMU lacks a BOOK8-supported RTL8168/8169 NIC. Real SMB protocol testing
is provided separately by smb_integration_test.py, not inferred from this VM.
usage: qemu_netshare_test.py <book8-cli.iso> <data.img> [log-directory]
"""
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import qemu_control_panel_test as panel
import qemu_graphics_input_smoke as smoke
import qemu_ui_lookup as ui
from qemu_utilities_test import (
    exit_desktop,
    screenshot_region,
    type_text,
)
from qemu_desktop_test import QemuMonitor


def authenticate(monitor, serial, process, height):
    """Open a throwaway desktop session so later ELF windows can access files."""
    type_text(monitor, "book8gui\n")
    smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR")
    smoke.wait_for(serial, process,
                   'WINDOW id=1 title="BOOK8 Sign In" registered')
    monitor.reset_pointer()
    type_text(monitor, "default\n")
    smoke.wait_for(serial, process, "BOOK8-SESSION: login accepted uid=0")
    exit_desktop(monitor, serial, process, height)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    iso, source = (Path(arg).resolve() for arg in sys.argv[1:3])
    if not iso.is_file() or not source.is_file():
        raise SystemExit("ISO and disposable source image must be regular files")
    logs = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else iso.parent / "netshare-qemu-validation"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-netshare-") as temp:
        disk = Path(temp) / "scratch.img"
        shutil.copyfile(source, disk)
        # Model an installed system volume, not an excluded external data disk.
        subprocess.run(["mmd", "-i", str(disk), "::/BOOT"], check=True)
        subprocess.run(["mcopy", "-i", str(disk), str(iso.parent / "book8.elf"),
                        "::/BOOT/BOOK8.ELF"], check=True)
        process, port, serial = panel.boot(iso, disk, logs, "netshare")
        monitor = None
        try:
            log = smoke.wait_for(serial, process, "A:\\> ")
            width, height, _ = smoke.verify_framebuffer(log)
            monitor = QemuMonitor(port, process)
            monitor.set_screen_size(width, height)
            authenticate(monitor, serial, process, height)
            type_text(monitor, "exec a:/bin/netshare.elf\n")
            smoke.wait_for(serial, process, 'title="Local Network Share Browser" registered')
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR", occurrences=2)
            monitor.reset_pointer()
            w = min(720, width - 8)
            x = min(40, width - w)
            monitor.click(x + w - 38, 52)
            screenshot_region(monitor, logs / "browser.ppm", 4, 30, width - 8, 160)
            for _ in range(16):
                monitor.command("sendkey backspace", 0.12)
            type_text(monitor, "bad\n")
            smoke.wait_for(serial, process, "NETSHARE: Invalid IPv4")
            screenshot_region(monitor, logs / "invalid-ip.ppm", (width - 520) // 2,
                              (height - 36 - 128) // 2, 520, 128)
            monitor.command("sendkey esc", 0.3)
            for _ in range(16):
                monitor.command("sendkey backspace", 0.12)
            type_text(monitor, "192.168.7.1\n")
            smoke.wait_for(serial, process, "NETSHARE: Network offline")
            monitor.command("sendkey esc", 0.3)
            # Use only the test's disposable FAT copy for local operations.
            client_x, client_y, client_width = 4, 30, width - 8
            monitor.click(client_x + 110, client_y + 56)
            for _ in range(20):
                monitor.command("sendkey backspace", 0.05)
            type_text(monitor, "A:\\\n")
            monitor.click(client_x + client_width * 5 // 7 + 12, client_y + 112)
            type_text(monitor, "NSHARE\n")
            smoke.wait_for(serial, process, "NETSHARE: local folder created")
            monitor.click(client_x + 110, client_y + 56)
            for _ in range(4):
                monitor.command("sendkey backspace", 0.05)
            type_text(monitor, "A:\\NSHARE\n")
            monitor.click(client_x + client_width * 5 // 7 + 12, client_y + 112)
            type_text(monitor, "INNER\n")
            smoke.wait_for(serial, process, "NETSHARE: local folder created", occurrences=2)
            monitor.command("sendkey down", 0.3)
            monitor.click(client_x + client_width * 3 // 7 + 12, client_y + 112)
            for _ in range(5):
                monitor.command("sendkey backspace", 0.05)
            type_text(monitor, "RENAMED\n")
            smoke.wait_for(serial, process, "NETSHARE: local entry renamed")
            monitor.command("sendkey down", 0.3)
            monitor.click(client_x + client_width * 2 // 7 + 12, client_y + 112)
            screenshot_region(monitor, logs / "local-properties.ppm", (width - 520) // 2,
                              (height - 36 - 300) // 2, 520, 300)
            monitor.command("sendkey esc", 0.3)
            monitor.click(client_x + client_width * 4 // 7 + 12, client_y + 112)
            monitor.command("sendkey esc", 0.3)
            monitor.click(client_x + client_width * 4 // 7 + 12, client_y + 112)
            monitor.command("sendkey ret", 0.3)
            smoke.wait_for(serial, process, "NETSHARE: local entry deleted")
            monitor.click(client_x + client_width * 6 // 7 + 12, client_y + 112)
            dialog_x, dialog_y = (width - 520) // 2, (height - 36 - 300) // 2
            monitor.click(dialog_x + 150, dialog_y + 280)
            smoke.wait_for(serial, process, "NETSHARE: profiles saved without passwords")
            screenshot_region(monitor, logs / "profiles.ppm", dialog_x, dialog_y, 520, 300)
            monitor.command("sendkey esc", 0.3)
            expected_host = screenshot_region(monitor, logs / "profile-host.ppm",
                                              client_x + 3, client_y + 20, 88, 16)
            monitor.click(width - 14, 12)
            smoke.wait_for(serial, process, "WINDOW closed id=1")
            ui.click_program(monitor, serial, height, process=process,
                             path="A:\\BIN\\NETSHARE.ELF")
            smoke.wait_for(serial, process, 'title="Local Network Share Browser" registered', occurrences=2)
            monitor.click(20, height - 18)
            monitor.click(40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered')
            # Settings has eight categories; Apps is category 2.
            monitor.command("sendkey pgdn", 0.3)
            monitor.command("sendkey pgdn", 0.3)
            monitor.command("sendkey home", 0.3)
            for _ in range(8):
                monitor.command("sendkey down", 0.15)
            screenshot_region(monitor, logs / "netshare-details.ppm", 160, 50, 440, 290)
            monitor.command("sendkey delete", 0.3)
            monitor.command("sendkey ret", 0.3)
            smoke.wait_for(serial, process, "Close this utility's windows before removing its executable.")
            monitor.command("sendkey esc", 0.3)
            # Settings is launch #3 (step 48); close then close the still-running browser.
            settings_width = min(760, width - 8)
            monitor.click(min(88, width - settings_width) + settings_width - 14, 100)
            monitor.click(min(64, width - w) + w - 14, 76)
            smoke.wait_for(serial, process, "WINDOW closed id=2")
            monitor.click(20, height - 18)
            monitor.click(40, height - 70)
            smoke.wait_for(serial, process, 'title="System Settings" registered', occurrences=2)
            # Settings has eight categories; Apps is category 2.
            monitor.command("sendkey pgdn", 0.3)
            monitor.command("sendkey pgdn", 0.3)
            monitor.command("sendkey home", 0.3)
            for _ in range(8):
                monitor.command("sendkey down", 0.15)
            monitor.command("sendkey delete", 0.3)
            monitor.command("sendkey ret", 0.3)
            smoke.wait_for(serial, process, "Program removed; Programs shortcut unregistered.")
            screenshot_region(monitor, logs / "uninstalled.ppm", 160, 50, 440, 290)
        finally:
            if monitor:
                monitor.close()
            panel.stop(process)
        if subprocess.run(["mtype", "-i", str(disk), "::/BIN/NETSHARE.ELF"],
                          capture_output=True).returncode == 0:
            raise AssertionError("Removed NETSHARE.ELF is still present")
        for target in ("::/BIN/HEXVIEW.ELF", "::/BIN/CODEEDIT.ELF"):
            subprocess.run(["mtype", "-i", str(disk), target],
                           check=True, stdout=subprocess.DEVNULL)
        profile = subprocess.run(["mtype", "-i", str(disk), "::/NETSHARE.CFG"],
                                 check=True, capture_output=True).stdout
        if len(profile) != 3732 or profile[:8] != b"B8NSH01\0" or struct.unpack_from("<I", profile, 8)[0] != 1:
            raise AssertionError("Server profile did not persist with the expected password-free format")
        if profile[12:28].rstrip(b"\0") != b"192.168.7.1":
            raise AssertionError("Persisted profile has the wrong host")
        subprocess.run(["mdir", "-i", str(disk), "::/NSHARE"], check=True, stdout=subprocess.DEVNULL)
        if subprocess.run(["mdir", "-i", str(disk), "::/NSHARE/RENAMED"], capture_output=True).returncode == 0:
            raise AssertionError("Deleted local folder still exists")
        # Reinstall only on this scratch image to test profile decoding in a new VM.
        subprocess.run(["mcopy", "-i", str(disk), str(iso.parent / "user" / "netshare.elf"),
                        "::/BIN/NETSHARE.ELF"], check=True)
        process, port, serial = panel.boot(iso, disk, logs, "profile-restore")
        monitor = None
        try:
            log = smoke.wait_for(serial, process, "A:\\> ")
            width, height, _ = smoke.verify_framebuffer(log)
            monitor = QemuMonitor(port, process)
            monitor.set_screen_size(width, height)
            authenticate(monitor, serial, process, height)
            type_text(monitor, "exec a:/bin/netshare.elf\n")
            smoke.wait_for(serial, process, 'title="Local Network Share Browser" registered')
            smoke.wait_for(serial, process, "BOOK8-DESKTOP: TASKBAR", occurrences=2)
            monitor.reset_pointer()
            monitor.click(min(40, width - min(720, width - 8)) + min(720, width - 8) - 38, 52)
            before = screenshot_region(monitor, logs / "profile-before-restore.ppm", 7, 50, 88, 16)
            monitor.click(4 + (width - 8) * 6 // 7 + 12, 142)
            monitor.click((width - 520) // 2 + 20, (height - 36 - 300) // 2 + 60)
            monitor.command("sendkey ret", 0.3)
            restored = screenshot_region(monitor, logs / "profile-restored.ppm", 7, 50, 88, 16)
            if restored != expected_host or restored == before:
                raise AssertionError("Cold-boot profile load did not restore the saved host field")
        finally:
            if monitor:
                monitor.close()
            panel.stop(process)
    print(f"PASS: netshare ELF/menu, local create/rename/properties/delete, profile persistence/cold restore, offline alerts and uninstall; logs: {logs}")


if __name__ == "__main__":
    main()
