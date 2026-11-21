#!/usr/bin/env python3
"""Validate the packaged-app manifest and emit its kernel registration table."""

import argparse
import json
import re
from pathlib import Path

ROW = re.compile(
    r"^[A-Za-z0-9_.-]+\|[A-Za-z0-9_.-]+\|[A-Za-z0-9_]+\|"
    r"[A-Za-z0-9 -]+\|[A-Za-z0-9_]+\|[01]\|[01]$"
)


def read_manifest(path):
    rows = []
    seen_keys = set()
    seen_targets = set()
    for line_number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not ROW.fullmatch(line):
            raise SystemExit(f"{path}:{line_number}: invalid manifest row")
        source, target, key, name, app_class, hidden, system_component = line.split("|")
        if key.upper() in seen_keys or target.upper() in seen_targets:
            raise SystemExit(f"{path}:{line_number}: duplicate app key or destination")
        seen_keys.add(key.upper())
        seen_targets.add(target.upper())
        rows.append((source, target, key, name, app_class, hidden, system_component))
    if not rows:
        raise SystemExit(f"{path}: manifest has no app rows")
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()
    rows = read_manifest(args.manifest)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    items = ",\n".join(
        "    {%s, %s, %s, %s, %s, %s, %s}" % (
            *(json.dumps(value) for value in row[:5]), *row[5:]
        )
        for row in rows
    )
    args.header.write_text(
        "#ifndef BOOK8_GENERATED_APP_MANIFEST_H\n"
        "#define BOOK8_GENERATED_APP_MANIFEST_H\n"
        "struct book8_manifest_app { const char *source; const char *target; "
        "const char *key; const char *name; const char *app_class; "
        "uint8_t hidden; uint8_t system_component; };\n"
        "static const struct book8_manifest_app book8_manifest_apps[] = {\n"
        f"{items}\n"
        "};\n"
        "#define BOOK8_MANIFEST_APP_COUNT "
        "(sizeof(book8_manifest_apps) / sizeof(book8_manifest_apps[0]))\n"
        "#endif\n",
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
