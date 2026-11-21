#!/usr/bin/env python3
"""Exercise the actual boot.S warm jump in an isolated, diskless QEMU image.

usage: python3 tests/qemu_netboot_jump_test.py
This tests the long-mode handoff, not RTL8168 wire transfers.
"""
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    output = root / "build" / "rtl8168-validation"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="book8-warm-jump-") as directory:
        temp = Path(directory)
        for name, source in (("boot", root / "src" / "boot.S"),
                             ("smoke", root / "tests" / "netboot_jump_smoke.S")):
            subprocess.run(["gcc", "-m64", "-ffreestanding", "-fno-pie",
                            "-c", str(source), "-o", str(temp / (name + ".o"))],
                           check=True)
        tree = temp / "iso" / "boot"
        (tree / "grub").mkdir(parents=True)
        subprocess.run(["ld", "-m", "elf_x86_64", "-T", str(root / "linker.ld"),
                        "-o", str(tree / "smoke.elf"),
                        str(temp / "boot.o"), str(temp / "smoke.o")], check=True)
        (tree / "grub" / "grub.cfg").write_text(
            "set timeout=0\nmenuentry warm {\n"
            "multiboot2 /boot/smoke.elf\nboot\n}\n")
        iso = temp / "smoke.iso"
        subprocess.run(["grub-mkrescue", "-o", str(iso), str(temp / "iso")],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        result = subprocess.run(
            ["qemu-system-x86_64", "-machine", "pc", "-m", "128",
             "-cdrom", str(iso), "-boot", "d", "-display", "none",
             "-serial", "stdio", "-monitor", "none", "-nic", "none",
             "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
            timeout=30, capture_output=True)
    log = result.stdout.decode(errors="replace")
    (output / "warm-jump-serial.log").write_text(log)
    (output / "warm-jump-qemu.log").write_bytes(result.stderr)
    if result.returncode != 1 or "PASS: NETBOOT long-mode warm handoff ABI" not in log:
        raise SystemExit(f"FAIL: warm jump (exit={result.returncode}): {log}")
    print(log.strip())


if __name__ == "__main__":
    main()
