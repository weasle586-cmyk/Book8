#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/user}"
mkdir -p "$out"
flags=(-m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -nostdinc
    -I"$root/include" -fno-builtin -fno-stack-protector -fpie
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small)
gcc "${flags[@]}" -c "$root/user/crt0.S" -o "$out/nvmap_crt0.o"
gcc "${flags[@]}" -c "$root/lib/syscall.c" -o "$out/nvmap_syscall.o"
gcc "${flags[@]}" -c "$root/lib/syscall_stub.S" -o "$out/nvmap_syscall_stub.o"
gcc "${flags[@]}" -c "$root/user/nvmap.c" -o "$out/nvmap.o"
ld -m elf_x86_64 -z max-page-size=4096 -T "$root/user/user.ld" -nostdlib \
    -o "$out/nvmap.elf" "$out/nvmap_crt0.o" "$out/nvmap.o" \
    "$out/nvmap_syscall.o" "$out/nvmap_syscall_stub.o"
printf 'Created %s (kernel not rebuilt)\n' "$out/nvmap.elf"
