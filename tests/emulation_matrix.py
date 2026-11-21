#!/usr/bin/env python3
"""3x3 emulation matrix: {AMD SB710, Intel G41, stock} x {QEMU, Bochs, VirtualBox}.

Run under WSL (QEMU and Bochs are WSL packages, VBoxManage.exe is reached
through Windows interop):  python3 tests/emulation_matrix.py build/book8.iso

Only QEMU can approximate the G41 and SB710 topologies. Bochs (one controller
of each type, no companion modelling) and VirtualBox (OHCI/EHCI/xHCI only, no
UHCI) are mapped to the closest controller mix; for them the pass criterion is
"boots to the command interpreter", and the topology is reported against the
ideal board topology so the divergence is explicit. Keystroke injection is
only attempted on QEMU.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qemu_amd_sb710_profile as amd  # noqa: E402
import qemu_g41_profile as g41  # noqa: E402

VBOXMANAGE = "/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
BOOT_TIMEOUT = {"qemu": 90, "bochs": 420, "vbox": 150}
IDEAL = {"A": "8/0/5/2/1", "B": "5/4/0/1/0", "C": "1/1/0/0/0"}
NAMES = {"A": "AMD SB710", "B": "Intel G41", "C": "Stock"}


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def wait_for(path, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in read(path):
            return True
        time.sleep(1)
    return False


def monitor(port, command):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        time.sleep(0.2)
        sock.sendall(command.encode() + b"\n")
        time.sleep(0.2)


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def stop(process):
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()


def qemu_stock_args(iso, serial, port):
    return [
        os.environ.get("QEMU", "qemu-system-x86_64"),
        "-machine", "pc", "-m", "1024", "-cpu", "qemu64",
        "-device", "piix3-usb-uhci,id=uhci", "-device", "usb-kbd,bus=uhci.0",
        "-cdrom", str(iso), "-boot", "d", "-display", "none",
        "-serial", f"file:{serial}",
        "-monitor", f"tcp:127.0.0.1:{port},server,nowait", "-no-reboot",
    ]


def run_qemu(profile, iso, temp):
    serial = temp / "serial.log"
    port = free_port()
    if profile == "A":
        args = amd.qemu_args(iso, serial, port, "none")
    elif profile == "B":
        args = g41.qemu_args(iso, serial, port, "uhci4")
    else:
        args = qemu_stock_args(iso, serial, port)
    process = subprocess.Popen(args, stderr=subprocess.DEVNULL)
    echo = "n/a"
    try:
        booted = wait_for(serial, "BOOK8 x86-64 command interpreter",
                          BOOT_TIMEOUT["qemu"])
        if booted:
            wait_for(serial, "USB descriptor failures", 10)
            time.sleep(1)
            before = len(read(serial))
            for key in ("v", "e", "r", "ret"):
                monitor(port, f"sendkey {key}")
            time.sleep(3)
            echo = "yes" if "ver" in read(serial)[before:].lower() \
                or "a:\\> ver" in read(serial).lower() else "no"
            for key in ("r", "i", "n", "g", "3", "t", "e", "s", "t", "ret"):
                monitor(port, f"sendkey {key}")
            wait_for(serial, "RING3TEST:", 30)
            time.sleep(1)
    finally:
        stop(process)
    return read(serial), booted, echo


BOCHS_USB = {
    "A": ["usb_ohci: enabled=1", "usb_ehci: enabled=1", "usb_xhci: enabled=1"],
    "B": ["usb_uhci: enabled=1", "usb_ehci: enabled=1"],
    "C": ["usb_uhci: enabled=1"],
}


def run_bochs(profile, iso, temp):
    serial = temp / "serial.log"
    rc = temp / "bochsrc.txt"
    lines = [
        "romimage: file=/usr/share/seabios/bios-256k.bin",
        "vgaromimage: file=/usr/share/seabios/vgabios-stdvga.bin",
        "megs: 256", "cpu: ips=4000000", "boot: cdrom",
        "ata0: enabled=1",
        f"ata0-master: type=cdrom, path={iso}, status=inserted",
        'display_library: rfb, options="timeout=0"',
        f"log: {temp / 'bochs.log'}", "debug: action=ignore",
        f"com1: enabled=1, mode=file, dev={serial}",
    ] + BOCHS_USB[profile]
    rc.write_text("\n".join(lines) + "\n")
    script = temp / "bochs.rc"
    script.write_text("c\n")
    process = subprocess.Popen(["bochs", "-q", "-f", str(rc), "-rc",
                                str(script)],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        booted = wait_for(serial, "BOOK8 x86-64 command interpreter",
                          BOOT_TIMEOUT["bochs"])
        if booted:
            wait_for(serial, "USB descriptor failures", 20)
            time.sleep(1)
    finally:
        stop(process)
    return read(serial), booted, "n/a"


def vbox(*args, check=True):
    result = subprocess.run([VBOXMANAGE, *args], capture_output=True,
                            text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"VBoxManage {' '.join(args)}: "
                           f"{result.stdout}{result.stderr}")
    return result


def winpath(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True,
                          text=True).stdout.strip()


def run_vbox(profile, iso, temp):
    data = iso.parent / "book8-data16.img"
    name = f"book8-matrix-{profile}-{int(time.time())}"
    serial = temp / "serial.log"
    chipset = "ich9" if profile == "B" else "piix3"
    usb = {"A": ["--usb-ohci", "on", "--usb-ehci", "on", "--usb-xhci", "on"],
           "B": ["--usb-ohci", "on", "--usb-ehci", "on"],
           "C": ["--usb-ohci", "on"]}[profile]
    booted = False
    try:
        vbox("createvm", "--name", name, "--ostype", "Other_64", "--register")
        vbox("modifyvm", name, "--memory", "512", "--chipset", chipset,
             "--ioapic", "on", "--graphicscontroller", "vboxvga",
             "--vram", "16", "--mouse", "ps2", "--keyboard", "ps2", *usb)
        vbox("storagectl", name, "--name", "IDE", "--add", "ide")
        vbox("storageattach", name, "--storagectl", "IDE", "--port", "0",
             "--device", "0", "--type", "dvddrive", "--medium", winpath(iso))
        data = iso.parent / "book8-data16.img"
        if data.exists():
            vdi = temp / "data.vdi"
            vbox("convertfromraw", winpath(data), winpath(vdi), "--format", "VDI")
            vbox("storageattach", name, "--storagectl", "IDE", "--port", "1",
                 "--device", "0", "--type", "hdd", "--medium", winpath(vdi))
        vbox("modifyvm", name, "--uart1", "0x3f8", "4", "--uart-mode1",
             "file", winpath(serial))
        vbox("startvm", name, "--type", "headless")
        booted = wait_for(serial, "BOOK8 x86-64 command interpreter",
                          BOOT_TIMEOUT["vbox"])
        if booted:
            wait_for(serial, "USB descriptor failures", 20)
            time.sleep(1)
            vbox("controlvm", name, "keyboardputstring", "ring3test\n")
            wait_for(serial, "RING3TEST:", 30)
            time.sleep(1)
            if data.exists():
                vbox("controlvm", name, "keyboardputstring", "exec hello.elf a\n")
                wait_for(serial, "EXEC: hello.elf", 40)
                time.sleep(1)
        log = read(serial)
    finally:
        vbox("controlvm", name, "poweroff", check=False)
        time.sleep(3)
        vbox("unregistervm", name, "--delete", check=False)
    return log, booted, "n/a"


def exec_check(log):
    if "EXEC: hello.elf exited with status 7" in log and "HELLO: data/bss ok" in log:
        return "PASS"
    return "FAIL"


def ring3(log):
    return "PASS" if "RING3TEST: PASS" in log else "FAIL"


def parse(log):
    def find(pattern):
        match = re.search(pattern, log, re.IGNORECASE)
        return match.group(1) if match else "?"
    if not log.strip():
        return {k: "?" for k in ("counts", "enum", "ehci_handoffs",
                                 "companion", "ps2_mouse", "usb_kbd", "fb")}
    return {
        "counts": find(r"USB host controllers[^:]*:\s*([\d/]+)"),
        "enum": find(r"USB enumeration[^:]*:\s*([\d/]+)"),
        "ehci_handoffs": find(r"EHCI legacy handoffs:\s*(\d+)"),
        "companion": find(r"companion mappings/static fallbacks:\s*([\d/]+)"),
        "ps2_mouse": "unavailable" if re.search(
            r"PS/2 mouse unavailable", log, re.IGNORECASE) else "available",
        "usb_kbd": "unavailable" if re.search(
            r"USB HID keyboard unavailable", log, re.IGNORECASE) else "active",
        "fb": find(r"(framebuffer tag missing|\d+x\d+x\d+)"),
    }


ENGINES = [("qemu", run_qemu), ("bochs", run_bochs), ("vbox", run_vbox)]
ONLY = os.environ.get("MATRIX_ENGINES")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: emulation_matrix.py <book8.iso>")
    iso = Path(sys.argv[1]).resolve()
    out_dir = Path(os.environ.get("MATRIX_OUT", "/tmp/book8-matrix"))
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for engine, runner in ENGINES:
        if ONLY and engine not in ONLY.split(","):
            continue
        for profile in "ABC":
            started = time.time()
            with tempfile.TemporaryDirectory(prefix="book8-mx-") as temp:
                try:
                    log, booted, echo = runner(profile, iso, Path(temp))
                    error = ""
                except Exception as exc:  # harness failure, reported as such
                    log, booted, echo, error = "", False, "n/a", str(exc)[:200]
            data = parse(log)
            (out_dir / f"{engine}-{profile}.log").write_text(log)
            data.update(engine=engine, profile=profile, booted=booted,
                        echo=echo, error=error,
                        seconds=round(time.time() - started))
            data["topology_match"] = data["counts"] == IDEAL[profile]
            data["ring3"] = ring3(log) if engine != "bochs" else "n/a"
            data["exec"] = exec_check(log) if engine == "vbox" else "n/a"
            if engine == "qemu":
                data["verdict"] = "PASS" if booted and data["topology_match"] \
                    and data["ring3"] == "PASS" else "FAIL"
            elif engine == "vbox":
                data["verdict"] = "PASS" if booted and data["ring3"] == "PASS" \
                    and data["exec"] == "PASS" else "FAIL"
            else:
                data["verdict"] = "PASS" if booted else "FAIL"
            results.append(data)
            print(f"[{engine}/{profile}] {data['verdict']} booted={booted} ring3={data['ring3']} exec={data['exec']} "
                  f"counts={data['counts']} {error}", flush=True)
    (out_dir / "results.json").write_text(json.dumps(results, indent=1))
    print("\n| Engine | Profile | Boot | Counts T/U/O/E/X | Ideal | EHCI handoffs "
          "| Companion map/fallback | Enum c/r/a/hid/act | PS/2 mouse "
          "| USB kbd | Echo | Ring 3 | Exec | FB | Verdict |")
    print("|" + "---|" * 15)
    for r in results:
        print(f"| {r['engine']} | {NAMES[r['profile']]} | "
              f"{'yes' if r['booted'] else 'NO'} | {r['counts']} | "
              f"{IDEAL[r['profile']]} | {r['ehci_handoffs']} | "
              f"{r['companion']} | {r['enum']} | {r['ps2_mouse']} | "
              f"{r['usb_kbd']} | {r['echo']} | {r['ring3']} | {r['exec']} | {r['fb']} | {r['verdict']} |")
    if any(r["verdict"] == "FAIL" for r in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
