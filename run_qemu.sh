#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
iso="$root/build/book8.iso"
log="$root/qemu.log"

if [ ! -f "$iso" ]; then
    echo "Missing BOOK8 ISO: $iso" >&2
    echo "Build it first with: bash ./build.sh" >&2
    exit 1
fi

rm -f "$log"

qemu-system-i386 \
    -M pc \
    -m 512 \
    -no-reboot \
    -no-shutdown \
    -d int,cpu_reset \
    -D "$log" \
    -serial stdio \
    -cdrom "$iso"
