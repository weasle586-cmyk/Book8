#!/usr/bin/env python3
"""Emit kernel trust anchors for build-produced ring-0 driver modules."""

import argparse
import hashlib
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--entry", action="append", required=True,
        help="trusted on-disk path=build module path",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    entries = []
    for entry in args.entry:
        path, separator, module = entry.partition("=")
        if not separator or not path or not module:
            parser.error("--entry must be ON_DISK_PATH=MODULE_FILE")
        digest = hashlib.sha256(Path(module).read_bytes()).digest()
        values = ", ".join(f"0x{value:02x}u" for value in digest)
        c_path = path.replace("\\", "\\\\").replace('"', '\\"')
        entries.append(f'    {{"{c_path}", {{ {values} }}}},\n')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "#ifndef BOOK8_DRIVER_TRUST_H\n"
        "#define BOOK8_DRIVER_TRUST_H\n"
        "#include <stdint.h>\n"
        "struct book8_driver_trust_entry {\n"
        "    const char *path;\n"
        "    uint8_t digest[32];\n"
        "};\n"
        "static const struct book8_driver_trust_entry "
        "book8_driver_trust_table[] = {\n"
        + "".join(entries)
        + "};\n"
        "#endif\n",
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
