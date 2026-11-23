#!/usr/bin/env python3
"""Generate the fixed-layout Multiboot2 boot integrity manifest.

usage: gen_boot_hashes.py --root ROOT --kernel KERNEL --output OUTPUT [--check]

The manifest is intentionally not signed: it binds GRUB-loaded payloads to the
exact bytes produced by this build so a modified splash or user ELF is
rejected before it is executed. --check verifies an already written manifest
against the same inputs and is used by the build to catch stale files.
"""
import argparse
import hashlib
import struct
from pathlib import Path

MAGIC = 0x31483842  # "B8H1" in little-endian byte order
VERSION = 1
HEADER_SIZE = 16
ENTRY_SIZE = 64
KEY_SIZE = 32
DIGEST_SIZE = 32
MAX_ENTRIES = 128

ROOT_APPS = (
    ("cat.elf", "CAT.ELF"),
    ("hello.elf", "HELLO.ELF"),
    ("nvmap.elf", "NVMAP.ELF"),
)
KERNEL_KEYS = ("KERNEL", "BOOK8.ELF", "BOOT/BOOK8.ELF")


def sha256_file(path: Path) -> bytes:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.digest()


def manifest_entries(root: Path, kernel: Path) -> dict:
    entries = {}

    def add(key: str, source: Path) -> None:
        if not source.is_file():
            raise SystemExit(f"boot hash input is missing: {source}")
        if not key or len(key) >= KEY_SIZE:
            raise SystemExit(f"invalid boot hash key: {key!r}")
        if any(not (c.isupper() or c.isdigit() or c in "/_-.")
               for c in key):
            raise SystemExit(f"invalid boot hash key: {key!r}")
        entries[key] = sha256_file(source)

    add("SPLASH", root / "assets" / "book8-splash.b8s")
    kernel_digest = sha256_file(kernel)
    for key in KERNEL_KEYS:
        entries[key] = kernel_digest

    manifest = root / "installer" / "apps.manifest"
    with manifest.open("r", encoding="ascii") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split("|")
            if len(fields) < 2:
                raise SystemExit(f"invalid apps.manifest line: {line!r}")
            source_name, target_name = fields[0], fields[1]
            add("BIN/" + target_name.upper(),
                root / "build" / "user" / source_name)

    for source_name, target_name in ROOT_APPS:
        add(target_name, root / "build" / "user" / source_name)

    if len(entries) > MAX_ENTRIES:
        raise SystemExit(
            f"boot hash manifest has {len(entries)} entries; "
            f"maximum is {MAX_ENTRIES}"
        )
    return entries


def build_manifest(root: Path, kernel: Path) -> bytes:
    entries = manifest_entries(root, kernel)
    output = bytearray(struct.pack("<IIII", MAGIC, VERSION, len(entries), 0))
    for key in sorted(entries):
        key_bytes = key.encode("ascii")
        output += key_bytes.ljust(KEY_SIZE, b"\0")
        output += entries[key]
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = args.root.resolve()
    kernel = args.kernel if args.kernel.is_absolute() else root / args.kernel
    output = args.output if args.output.is_absolute() else root / args.output
    expected = build_manifest(root, kernel.resolve())

    if args.check:
        if not output.is_file() or output.read_bytes() != expected:
            raise SystemExit(f"boot hash manifest is stale: {output}")
        print(f"PASS: boot hash manifest is current ({len(expected)} bytes)")
        return

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(expected)
    print(f"Created {output} ({len(expected)} bytes)")


if __name__ == "__main__":
    main()
