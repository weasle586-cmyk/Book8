#!/usr/bin/env bash
# No-regression configuration matrix for BOOK8 feature flags.
# Each combination compiles (freestanding and hosted) and runs the network
# feature test so a flag can never silently rot. QEMU is not involved here;
# use `BOOK8_SKIP_RING3_SMOKE=1 bash ./build.sh` for the full host suite.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/book8-build-matrix.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT

for tool in gcc; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'Missing %s\n' "$tool" >&2; exit 1; }
done

combos=(
    'default|'
    'firewall|-DBOOK8_NET_FIREWALL=1'
    'ipv6|-DBOOK8_NET_IPV6=1'
    'no-dns-cache|-DBOOK8_NET_DNS_CACHE=0'
    'no-tls-stub|-DBROOK8_TLS_EMPTY=0'
    'all|-DBOOK8_NET_FIREWALL=1 -DBOOK8_NET_IPV6=1 -DBOOK8_NET_DNS_CACHE=0 -DBROOK8_TLS_EMPTY=0'
)

for entry in "${combos[@]}"; do
    name="${entry%%|*}"
    flags="${entry#*|}"
    printf 'BUILD-MATRIX: %s (%s)\n' "$name" "${flags:-no flags}"
    # Hosted: exercises runtime behaviour of the same net/net.c translation unit.
    # shellcheck disable=SC2086
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
        -fsanitize=address,undefined \
        -o "$work/net_features-$name" \
        "$root/tests/net_features_host_test.c" "$root/net/net.c" $flags
    "$work/net_features-$name"
    # Freestanding: the kernel compiles this unit with different ABI flags.
    # shellcheck disable=SC2086
    gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
        -fPIC -fno-stack-protector -fno-builtin \
        -fno-asynchronous-unwind-tables -fno-unwind-tables \
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
        -c "$root/net/net.c" -o "$work/net-$name.o" $flags
done

printf 'PASS: no-regression build matrix (%d configurations)\n' "${#combos[@]}"
