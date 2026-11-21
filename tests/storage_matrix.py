#!/usr/bin/env python3
"""Storage write-path matrix: DMA write, flush, read-back and fault-recovery
self-tests across four emulation profiles.

  1 qemu-generic   default QEMU machine, PIIX IDE bus master + ich9-ahci
  2 qemu-target    GA-78LMT/SB710 approximation: i440fx with PIIX IDE and an
                   explicit ICH9 AHCI at 00:11.0 (target-pc), plus a q35 run
                   where AHCI is the only controller (target-q35)
  3 vbox-generic   PIIX3 chipset, PIIX4 IDE, Intel AHCI
  4 vbox-target    ICH9 chipset, ICH6 IDE, Intel AHCI (closest VirtualBox
                   match to the bench's IDE/AHCI controller mix; VirtualBox
                   cannot model the AMD SB710 itself)

Run under WSL (VBoxManage.exe is reached through Windows interop):
    python3 tests/storage_matrix.py build/book8.iso build/book8-data16.img

Every profile must show the read and write self-tests passing, no DMA failure
messages, and a host-side check of the scratch images (guard sectors intact,
final pattern exact). Results go to
build/runtime-validation/storage-matrix/results.json.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qemu_storage_test as st  # noqa: E402

VBOXMANAGE = "/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
VBOX_PROFILES = {
    "vbox-generic": {"chipset": "piix3", "ide": "PIIX4"},
    "vbox-target": {"chipset": "ich9", "ide": "ICH6"},
}


def vbox(*args, check=True):
    result = subprocess.run([VBOXMANAGE, *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"VBoxManage {' '.join(args)}: "
                           f"{result.stdout}{result.stderr}")
    return result


def winpath(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True,
                          text=True).stdout.strip()


def run_vbox(profile, iso, image, work, timeout=240):
    """Returns (serial text, missing, forbidden, image errors)."""
    cfg = VBOX_PROFILES[profile]
    name = f"book8-storage-{profile}-{int(time.time())}"
    serial = work / f"{profile}.serial.log"
    serial.unlink(missing_ok=True)
    scratch = {"ide": work / f"{profile}-ide.img",
               "sata": work / f"{profile}-sata.img"}
    vdi = {k: scratch[k].with_suffix(".vdi") for k in scratch}
    data_vdi = work / f"{profile}-data.vdi"
    raw_out = {k: work / f"{profile}-{k}.out.img" for k in scratch}
    for p in [*vdi.values(), data_vdi, *raw_out.values()]:
        vbox("closemedium", "disk", winpath(p), check=False)
        p.unlink(missing_ok=True)
    for kind, path in scratch.items():
        st.make_scratch(path)
        vbox("convertfromraw", winpath(path), winpath(vdi[kind]),
             "--format", "VDI")
    vbox("convertfromraw", winpath(image), winpath(data_vdi), "--format", "VDI")

    text, image_errors = "", []
    try:
        vbox("createvm", "--name", name, "--ostype", "Other_64", "--register")
        vbox("modifyvm", name, "--memory", "512", "--chipset", cfg["chipset"],
             "--ioapic", "on", "--graphicscontroller", "vboxvga",
             "--vram", "16", "--boot1", "dvd", "--boot2", "disk",
             "--uart1", "0x3f8", "4", "--uart-mode1", "file", winpath(serial))
        vbox("storagectl", name, "--name", "IDE", "--add", "ide",
             "--controller", cfg["ide"])
        vbox("storagectl", name, "--name", "SATA", "--add", "sata",
             "--controller", "IntelAhci", "--portcount", "2")
        for port, device, kind, medium in (
                (0, 0, "hdd", data_vdi), (0, 1, "hdd", vdi["ide"]),
                (1, 0, "dvddrive", iso)):
            vbox("storageattach", name, "--storagectl", "IDE",
                 "--port", str(port), "--device", str(device),
                 "--type", kind, "--medium", winpath(medium))
        vbox("storageattach", name, "--storagectl", "SATA", "--port", "0",
             "--device", "0", "--type", "hdd", "--medium", winpath(vdi["sata"]))
        vbox("startvm", name, "--type", "headless")
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(2)
            text = serial.read_text(errors="replace") if serial.exists() else ""
            if not st.evaluate(text, st.IDE_AND_AHCI)[0]:
                break
    finally:
        vbox("controlvm", name, "poweroff", check=False)
        time.sleep(4)
        text = serial.read_text(errors="replace") if serial.exists() else text
        for kind in scratch:
            res = vbox("clonemedium", "disk", winpath(vdi[kind]),
                       winpath(raw_out[kind]), "--format", "RAW", check=False)
            if res.returncode != 0 or not raw_out[kind].exists():
                image_errors.append(f"{kind}: could not export image: "
                                    f"{res.stderr.strip()}")
            else:
                image_errors += [f"{kind}: {e}"
                                 for e in st.check_scratch(raw_out[kind])]
        vbox("unregistervm", name, "--delete", check=False)
        for p in [*vdi.values(), data_vdi]:
            vbox("closemedium", "disk", winpath(p), "--delete", check=False)
        for p in raw_out.values():
            vbox("closemedium", "disk", winpath(p), check=False)
        for p in [*scratch.values(), *raw_out.values()]:
            p.unlink(missing_ok=True)
    missing, bad = st.evaluate(text, st.IDE_AND_AHCI)
    return text, missing, bad, image_errors


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: storage_matrix.py <book8.iso> <data.img>")
    iso = Path(sys.argv[1]).resolve()
    image = Path(sys.argv[2]).resolve()
    work = iso.parent / "runtime-validation" / "storage-matrix"
    work.mkdir(parents=True, exist_ok=True)

    runs = [("qemu-generic", "QEMU generic", lambda: st.run_qemu("generic", iso, image)),
            ("qemu-target/pc", "QEMU target (i440fx + ICH9 AHCI)",
             lambda: st.run_qemu("target-pc", iso, image)),
            ("qemu-target/q35", "QEMU target (q35, AHCI only)",
             lambda: st.run_qemu("target-q35", iso, image)),
            ("vbox-generic", "VirtualBox generic (PIIX3/PIIX4/AHCI)",
             lambda: run_vbox("vbox-generic", iso, image, work)),
            ("vbox-target", "VirtualBox target (ICH9/ICH6/AHCI)",
             lambda: run_vbox("vbox-target", iso, image, work))]
    results, failed = [], False
    for key, label, run in runs:
        try:
            text, missing, bad, image_errors = run()
        except Exception as exc:  # report and continue with the other profiles
            text, missing, bad, image_errors = "", [], [], [f"run error: {exc}"]
        passed = not (missing or bad or image_errors)
        failed |= not passed
        (work / f"{key.replace('/', '-')}.storage.log").write_text(
            "\n".join(st.storage_lines(text)) + "\n")
        print(f"[{key}] {label}: {'PASS' if passed else 'FAIL'}")
        for line in st.storage_lines(text):
            print(f"    {line}")
        if not passed:
            print(f"    missing={missing} forbidden={bad} image={image_errors}")
        results.append({"profile": key, "label": label, "pass": passed,
                        "storage_lines": st.storage_lines(text),
                        "missing": missing, "forbidden": bad,
                        "image_errors": image_errors})
    (work / "results.json").write_text(json.dumps(results, indent=2))
    if failed:
        raise SystemExit("FAIL: storage matrix")
    print("PASS: storage matrix (all profiles)")


if __name__ == "__main__":
    main()
