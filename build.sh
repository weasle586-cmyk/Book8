#!/usr/bin/env bash
set -euo pipefail

for tool in gcc ld grub-file grub-mkrescue grub-install grub-mkstandalone \
    mformat mcopy mmd mkfs.vfat losetup mountpoint python3 xorriso nm cmp mktemp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing build dependency: %s\n' "$tool" >&2
        printf 'Install the packages listed in README.md and try again.\n' >&2
        exit 1
    fi
done

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Optional diagnostic switches, e.g. BOOK8_DIAG_CFLAGS="-DBOOK8_DIAG_SKIP_LEGSUP=1"
BOOK8_DIAG_CFLAGS="${BOOK8_DIAG_CFLAGS:-}"
build="$root/build"
run_test() {
    if [ "${BOOK8_BUILD_ONLY:-0}" = 1 ]; then
        return 0
    fi
    "$@"
}
if [ "${BOOK8_DIAGNOSTIC:-0}" = 1 ]; then
    build="$root/build/nv-diagnostic"
    BOOK8_DIAG_CFLAGS="$BOOK8_DIAG_CFLAGS -DBOOK8_DIAG_BOOT_LOG=1"
    iso_name="book8-nv-diagnostic.iso"
else
    iso_name="book8.iso"
fi
iso_root="$build/iso"
mkdir -p "$iso_root/boot/grub"
python3 "$root/tools/gen_app_manifest.py" \
    --manifest "$root/installer/apps.manifest" \
    --header "$build/generated/app_manifest.h"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fPIC -fno-stack-protector -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -DBOOK8_NV_DRIVER_MODULE -shared -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,max-page-size=4096 -Wl,-z,notext \
    -o "$build/nv_maxwell.drv" "$root/drivers/nv_maxwell.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fPIC -fno-stack-protector -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -DBOOK8_RTL8168_DRIVER_MODULE -shared -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,max-page-size=4096 -Wl,-z,notext \
    -o "$build/rtl8168.drv" "$root/drivers/rtl8168.c" \
    "$root/drivers/rtl8168_module.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fPIC -fno-stack-protector -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -DBOOK8_USB_DRIVER_MODULE -shared -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,max-page-size=4096 -Wl,-z,notext \
    -o "$build/usbhost.drv" "$root/src/usb.c" "$root/src/ohci.c" \
    "$root/drivers/usb_host_module.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fPIC -fno-stack-protector -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -shared -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,max-page-size=4096 -Wl,-z,notext \
    -o "$build/ac97.drv" "$root/drivers/hda.c" \
    "$root/drivers/hda_stream.c" "$root/drivers/hda_module.c" \
    "$root/lib/string.c"
python3 "$root/tools/gen_driver_trust.py" \
    --entry "A:\DRIVERS\NVMAXWEL.DRV=$build/nv_maxwell.drv" \
    --entry "A:\DRIVERS\RTL8168.DRV=$build/rtl8168.drv" \
    --entry "A:\DRIVERS\USBHOST.DRV=$build/usbhost.drv" \
    --entry "A:\DRIVERS\AC97.DRV=$build/ac97.drv" \
    --output "$build/generated/driver_trust.h"

# DHCP is the boot default; these are timeout fallback/static editor defaults. Set
# BOOK8_NET_IP=none to build an image with no default address.
BOOK8_NET_IP="${BOOK8_NET_IP-192.168.50.208}"
BOOK8_NET_MASK="${BOOK8_NET_MASK-255.255.255.0}"
BOOK8_NET_GATEWAY="${BOOK8_NET_GATEWAY-192.168.50.1}"
BOOK8_TELEMETRY_TARGET="${BOOK8_TELEMETRY_TARGET-192.168.50.17}"
BOOK8_PUSH="${BOOK8_PUSH:-0}"
if [ "$BOOK8_PUSH" != 0 ]; then
    printf 'BOOK8_PUSH is disabled; only BOOK8_PUSH=0 is supported\n' >&2
    exit 1
fi
ipv4_ok() {
    [[ "$1" =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]] || return 1
    for octet in "${BASH_REMATCH[@]:1}"; do
        [ "$((10#$octet))" -le 255 ] || return 1
    done
}
for setting in BOOK8_NET_IP BOOK8_NET_MASK BOOK8_NET_GATEWAY BOOK8_TELEMETRY_TARGET; do
    if [ "${!setting}" = none ]; then
        printf -v "$setting" '%s' ''
    fi
    value="${!setting}"
    if [ -n "$value" ] && ! ipv4_ok "$value"; then
        printf '%s=%s is not a dotted IPv4 address\n' "$setting" "$value" >&2
        exit 1
    fi
done
if [ -n "$BOOK8_NET_IP" ] && [ -z "$BOOK8_NET_MASK" ]; then
    printf 'BOOK8_NET_MASK is required when BOOK8_NET_IP is set\n' >&2
    exit 1
fi
build_id="$(date +%s)"
printf '%s\n' "$build_id" > "$build/build-id.txt"
net_cflags=(-DBOOK8_NET_DEFAULT_IP="\"$BOOK8_NET_IP\""
    -DBOOK8_NET_DEFAULT_MASK="\"$BOOK8_NET_MASK\""
    -DBOOK8_NET_DEFAULT_GATEWAY="\"$BOOK8_NET_GATEWAY\""
    -DBOOK8_NET_DEFAULT_TELEMETRY="\"$BOOK8_TELEMETRY_TARGET\""
    -DBOOK8_BUILD_ID="${build_id}u")
net_cflags+=(-DBOOK8_PUSH=0)
printf 'BOOK8 network mode=DHCP, 30s timeout fallback: IP=%s MASK=%s GATEWAY=%s TELEMETRY=%s PUSH=%s BUILD=%s\n' \
    "${BOOK8_NET_IP:-none}" "${BOOK8_NET_MASK:-none}" "${BOOK8_NET_GATEWAY:-none}" \
    "${BOOK8_TELEMETRY_TARGET:-none}" "$BOOK8_PUSH" "$build_id"

gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/boot.o" "$root/src/boot.S"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small $BOOK8_DIAG_CFLAGS "${net_cflags[@]}" \
    -I"$root/lib" -I"$build/generated" -c -o "$build/kernel.o" "$root/src/kernel.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/filesystem.o" "$root/src/filesystem.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/registry.o" "$root/src/registry.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/session.o" "$root/src/session.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -I"$root/include" \
    -c -o "$build/b8pkg.o" "$root/src/b8pkg.c"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/session_host_test" "$root/tests/session_host_test.c" \
    "$root/src/session.c" "$root/src/registry.c" "$root/net/sha256.c"
run_test "$build/session_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/include" \
    -fsanitize=address,undefined -o "$build/test_b8pkg" \
    "$root/tests/b8pkg_host_test.c" "$root/src/b8pkg.c"
run_test "$build/test_b8pkg"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/boot_log.o" "$root/src/boot_log.c"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/boot_log_host_test" "$root/tests/boot_log_host_test.c" "$root/src/boot_log.c"
run_test "$build/boot_log_host_test"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8gui.o" "$root/src/book8gui.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/system_config.o" "$root/src/system_config.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_explorer.o" \
    "$root/src/apps/book8_explorer.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_shell.o" \
    "$root/src/apps/book8_shell.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_editor.o" \
    "$root/src/apps/book8_editor.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_settings.o" \
    "$root/src/apps/book8_settings.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_login.o" \
    "$root/src/apps/book8_login.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/book8_taskmgr.o" \
    "$root/src/apps/book8_taskmgr.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_utilities.o" "$root/src/apps/book8_utilities.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_netshare.o" "$root/src/apps/book8_netshare.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -U_FORTIFY_SOURCE \
    -c -o "$build/book8_image.o" "$root/src/apps/book8_image.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_paintbook.o" "$root/src/apps/book8_paintbook.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_browser.o" "$root/src/apps/book8_browser.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_media_extension.o" "$root/src/apps/book8_media_extension.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_player.o" "$root/src/apps/book8_player.c"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
    -c -o "$build/book8_nvdiag.o" "$root/src/apps/book8_nvdiag.c"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/syscall_entry.o" "$root/src/syscall_entry.S"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/ap_trampoline.o" "$root/src/ap_trampoline.S"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/interrupt_entry.o" "$root/src/interrupt_entry.S"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/user_smoke.o" "$root/src/user_smoke.S"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/relaunch_stub.o" "$root/src/relaunch_stub.S"
gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
    -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -mcmodel=small -c -o "$build/syscall.o" "$root/src/syscall.c"
# Freestanding libc (lib/ + include/). -fno-tree-loop-distribute-patterns stops
# GCC turning loops in memcpy/memset into calls to themselves.
libc_flags="-m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -nostdinc \
    -I$root/include -I$root/lib -fno-builtin -fno-strict-aliasing \
    -fno-tree-loop-distribute-patterns -fno-stack-protector -fno-pic -fno-pie \
    -fno-asynchronous-unwind-tables -fno-unwind-tables"
libc_objects=()
for unit in string malloc stdlib stdio libc_selftest syscall; do
    gcc $libc_flags -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
        -c -o "$build/lib_$unit.o" "$root/lib/$unit.c"
    libc_objects+=("$build/lib_$unit.o")
done
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/lib_syscall_stub.o" "$root/lib/syscall_stub.S"
libc_objects+=("$build/lib_syscall_stub.o")

# Build-time check: run the libc self-test as a standalone host program.
gcc $libc_flags -DBOOK8_HOST_TEST -nostdlib -static -nostartfiles -Wl,--build-id=none \
    -o "$build/libc_host_test" "$root/tests/libc_host_test.c" \
    "$root"/lib/string.c "$root"/lib/malloc.c "$root"/lib/stdlib.c \
    "$root"/lib/stdio.c "$root"/lib/libc_selftest.c
run_test "$build/libc_host_test"

# Build-time check: ATA LBA28/LBA48 DMA taskfile encoding.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -o "$build/ata_dma_host_test" \
    "$root/tests/ata_dma_host_test.c" "$root/drivers/ata_dma.c"
run_test "$build/ata_dma_host_test"

# Build-time check: framebuffer primitives, font and text console.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/fb_host_test" "$root/tests/fb_host_test.c" \
    "$root/drivers/fb.c" "$root/drivers/font8x16.c"
run_test "$build/fb_host_test"

# Build-time check: NVIDIA Maxwell BAR discovery, push-buffer ring and 2D streams.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/nv_maxwell_host_test" "$root/tests/nv_maxwell_host_test.c" \
    "$root/drivers/nv_maxwell.c"
run_test "$build/nv_maxwell_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/driver_loader_host_test" "$root/tests/driver_loader_host_test.c" \
    "$root/kernel/driver_loader.c" "$root/net/sha256.c"
run_test env BOOK8_TEST_DRIVER="$build/nv_maxwell.drv" \
    BOOK8_TEST_DRIVER_PATH='A:\DRIVERS\NVMAXWEL.DRV' \
    BOOK8_TEST_DRIVER_KIND=nvidia "$build/driver_loader_host_test"
run_test env BOOK8_TEST_DRIVER="$build/rtl8168.drv" \
    BOOK8_TEST_DRIVER_PATH='A:\DRIVERS\RTL8168.DRV' \
    BOOK8_TEST_DRIVER_KIND=rtl8168 "$build/driver_loader_host_test"
run_test env BOOK8_TEST_DRIVER="$build/usbhost.drv" \
    BOOK8_TEST_DRIVER_PATH='A:\DRIVERS\USBHOST.DRV' \
    BOOK8_TEST_DRIVER_KIND=usb "$build/driver_loader_host_test"
run_test env BOOK8_TEST_DRIVER="$build/ac97.drv" \
    BOOK8_TEST_DRIVER_PATH='A:\DRIVERS\AC97.DRV' \
    BOOK8_TEST_DRIVER_KIND=hda "$build/driver_loader_host_test"

# Realtek PCI BAR sizing, reset/MAC, DMA rings and polling packet I/O.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/rtl8168_host_test" "$root/tests/rtl8168_host_test.c" \
    "$root/drivers/rtl8168.c"
run_test "$build/rtl8168_host_test"

# Zero-allocation ARP/IPv4/ICMP/UDP and packed telemetry serialization/dispatch.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/net_host_test" "$root/tests/net_host_test.c" "$root/net/net.c" \
    "$root/drivers/rtl8168.c"
run_test "$build/net_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/dhcp_host_test" "$root/tests/dhcp_host_test.c" \
    "$root/net/dhcp.c" "$root/net/network_settings.c" "$root/net/net.c"
run_test "$build/dhcp_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOOK8_HOST_TEST \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -Wl,--gc-sections \
    -o "$build/scheduler_host_test" "$root/tests/scheduler_host_test.c" \
    "$root/kernel/exec.c" "$root/kernel/sched.c" "$root/drivers/block.c" \
    "$root/src/session.c" "$root/src/registry.c" "$root/net/sha256.c"
run_test "$build/scheduler_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/smp_host_test" "$root/tests/smp_host_test.c" \
    "$root/kernel/smp.c"
run_test "$build/smp_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOOK8_HOST_TEST \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -Wl,--gc-sections \
    -o "$build/elf_b8s_host_test" "$root/tests/elf_b8s_host_test.c" \
    "$root/kernel/exec.c" "$root/src/session.c" "$root/src/registry.c" \
    "$root/src/b8pkg.c" "$root/net/sha256.c"
run_test "$build/elf_b8s_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOOK8_HOST_TEST \
    -fsanitize=address,undefined -o "$build/swap_host_test" \
    "$root/tests/swap_host_test.c" "$root/drivers/block.c"
run_test "$build/swap_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/tftp_host_test" "$root/tests/tftp_host_test.c" "$root/net/net.c"
run_test "$build/tftp_host_test"

# SHA-256/HMAC vectors, authenticated push-update agent, in-RAM kernel relaunch.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/sha256_host_test" "$root/tests/sha256_host_test.c" "$root/net/sha256.c"
run_test "$build/sha256_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/push_host_test" "$root/tests/push_host_test.c" "$root/net/push.c" \
    "$root/net/net.c" "$root/net/sha256.c"
run_test "$build/push_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/push_interop_host" "$root/tests/push_interop_host.c" "$root/net/push.c" \
    "$root/net/net.c" "$root/net/sha256.c"
run_test python3 "$root/tests/push_client_test.py" "$build/push_interop_host"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOOK8_HOST_TEST \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -Wl,--gc-sections \
    -o "$build/hwdiag_host_test" "$root/tests/hwdiag_host_test.c" \
    "$root/kernel/hwdiag.c" "$root/drivers/nv_maxwell.c" \
    "$root/src/syscall.c" "$root/net/net.c"
run_test "$build/hwdiag_host_test"
run_test python3 "$root/tests/telemetry_decode_test.py" "$build/hwdiag_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$root/include" \
    -fsanitize=address,undefined -o "$build/nvmap_host_test" \
    "$root/tests/nvmap_host_test.c"
run_test "$build/nvmap_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/relaunch_host_test" "$root/tests/relaunch_host_test.c" \
    "$root/kernel/relaunch.c"
run_test "$build/relaunch_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/pcie_host_test" "$root/tests/pcie_host_test.c" \
    "$root/drivers/pcie.c" "$root/drivers/amd_host_bridge.c" \
    "$root/drivers/pcie_probe.c" "$root/drivers/rs780.c" "$root/net/net.c" \
    "$root/src/registry.c"
run_test "$build/pcie_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/hda_host_test" "$root/tests/hda_host_test.c" \
    "$root/drivers/hda.c"
run_test "$build/hda_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/hda_stream_host_test" "$root/tests/hda_stream_host_test.c" \
    "$root/drivers/hda_stream.c" "$root/drivers/hda.c"
run_test "$build/hda_stream_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/rs780_manual_audit_test" "$root/tests/rs780_manual_audit_test.c" \
    "$root/drivers/rs780_manual_audit.c"
run_test "$build/rs780_manual_audit_test"

# Headless desktop regression: app registration, taskbar, input routing and
# tick-based Explorer directory navigation.
gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -o "$build/book8gui_host_test" "$root/tests/book8gui_host_test.c" \
    "$root/src/book8gui.c" "$root/src/apps/book8_explorer.c" \
    "$root/src/apps/book8_shell.c" "$root/src/apps/book8_editor.c" \
    "$root/src/apps/book8_settings.c" "$root/src/apps/book8_taskmgr.c" \
    "$root/src/apps/book8_login.c" "$root/src/session.c" \
    "$root/src/apps/book8_utilities.c" "$root/src/apps/book8_netshare.c" \
    "$root/src/apps/book8_paintbook.c" "$root/src/apps/book8_image.c" \
    "$root/src/apps/book8_browser.c" "$root/src/apps/book8_media_extension.c" \
    "$root/src/apps/book8_player.c" "$root/src/apps/book8_nvdiag.c" \
    "$root/net/smb.c" "$root/net/ntlm.c" "$root/net/sha256.c" "$root/net/tcp.c" "$root/net/net.c" \
    "$root/src/system_config.c" "$root/src/registry.c"
run_test "$build/book8gui_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/gui_programs_host_test" "$root/tests/gui_programs_host_test.c" \
    "$root/src/book8gui.c" "$root/src/apps/book8_explorer.c" \
    "$root/src/apps/book8_shell.c" "$root/src/apps/book8_editor.c" \
    "$root/src/apps/book8_settings.c" "$root/src/apps/book8_taskmgr.c" \
    "$root/src/apps/book8_login.c" "$root/src/session.c" \
    "$root/src/apps/book8_utilities.c" "$root/src/apps/book8_netshare.c" \
    "$root/src/apps/book8_paintbook.c" "$root/src/apps/book8_image.c" \
    "$root/src/apps/book8_browser.c" "$root/src/apps/book8_media_extension.c" \
    "$root/src/apps/book8_player.c" "$root/src/apps/book8_nvdiag.c" \
    "$root/net/smb.c" "$root/net/ntlm.c" "$root/net/sha256.c" "$root/net/tcp.c" "$root/net/net.c" "$root/src/system_config.c" "$root/src/registry.c"
run_test "$build/gui_programs_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/control_panel_host_test" "$root/tests/control_panel_host_test.c" \
    "$root/src/book8gui.c" "$root/src/system_config.c" \
    "$root/src/apps/book8_explorer.c" "$root/src/apps/book8_shell.c" \
    "$root/src/apps/book8_editor.c" "$root/src/apps/book8_settings.c" \
    "$root/src/apps/book8_login.c" "$root/src/session.c" \
    "$root/src/apps/book8_taskmgr.c" "$root/src/apps/book8_utilities.c" \
    "$root/src/apps/book8_netshare.c" "$root/src/apps/book8_paintbook.c" \
    "$root/src/apps/book8_image.c" "$root/src/apps/book8_browser.c" \
    "$root/src/apps/book8_media_extension.c" "$root/src/apps/book8_player.c" \
    "$root/src/apps/book8_nvdiag.c" \
    "$root/net/smb.c" "$root/net/ntlm.c" "$root/net/sha256.c" "$root/net/tcp.c" "$root/net/net.c" "$root/src/registry.c"
run_test "$build/control_panel_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/utilities_host_test" "$root/tests/utilities_host_test.c" \
    "$root/src/apps/book8_utilities.c" "$root/src/system_config.c"
run_test "$build/utilities_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/book8_image_host_test" "$root/tests/book8_image_host_test.c" \
    "$root/src/apps/book8_image.c"
run_test "$build/book8_image_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/book8_apps_host_test" "$root/tests/book8_apps_host_test.c" \
    "$root/src/apps/book8_paintbook.c" "$root/src/apps/book8_image.c" \
    "$root/src/apps/book8_browser.c" "$root/src/apps/book8_media_extension.c" \
    "$root/src/apps/book8_player.c" "$root/net/tcp.c" "$root/net/net.c"
run_test "$build/book8_apps_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/book8_nvdiag_host_test" "$root/tests/book8_nvdiag_host_test.c" \
    "$root/src/apps/book8_nvdiag.c" "$root/src/system_config.c"
run_test "$build/book8_nvdiag_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/fs_space_host_test" "$root/tests/fs_space_host_test.c" \
    "$root/src/filesystem.c"
run_test "$build/fs_space_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/tcp_host_test" "$root/tests/tcp_host_test.c" "$root/net/tcp.c" "$root/net/net.c"
run_test "$build/tcp_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/ntlm_host_test" "$root/tests/ntlm_host_test.c" "$root/net/ntlm.c"
run_test "$build/ntlm_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/smb_host_test" "$root/tests/smb_host_test.c" "$root/net/smb.c" "$root/net/ntlm.c" "$root/net/sha256.c" "$root/net/net.c"
run_test "$build/smb_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/netshare_host_test" "$root/tests/netshare_host_test.c" \
    "$root/src/apps/book8_netshare.c" "$root/net/ntlm.c" "$root/src/system_config.c" \
    "$root/src/session.c" "$root/src/registry.c" "$root/net/sha256.c" "$root/net/net.c"
run_test "$build/netshare_host_test"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/smb_integration_host" "$root/tests/smb_integration_host.c" \
    "$root/net/net.c" "$root/net/tcp.c" "$root/net/smb.c" "$root/net/ntlm.c" "$root/net/sha256.c"

# Storage stack: block layer, FAT reader, ELF loader / process execution.
for unit in drivers/block drivers/ata_dma drivers/ahci drivers/fb drivers/font8x16 drivers/pcie drivers/amd_host_bridge drivers/pcie_probe drivers/rs780 drivers/rs780_manual_audit net/net net/tcp net/smb net/ntlm net/dhcp net/network_settings net/sha256 net/push fs/fs kernel/exec kernel/sched kernel/smp kernel/apic kernel/vmm kernel/relaunch kernel/hwdiag kernel/driver_loader; do
    gcc -m64 -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
        -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
        -fno-asynchronous-unwind-tables -fno-unwind-tables \
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
        -mcmodel=small -c -o "$build/${unit//\//_}.o" "$root/$unit.c"
done

# User programs (linked into the 0x8000000000 user window by user/user.ld) and
# the FAT data disks the loader reads them from.
mkdir -p "$build/user"
gcc -m64 -c -ffreestanding -fno-pic -fno-pie -mno-red-zone \
    -o "$build/user/crt0.o" "$root/user/crt0.S"
for prog in hello cat; do
    gcc $libc_flags -fpie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
        -c -o "$build/user/$prog.o" "$root/user/$prog.c"
    ld -m elf_x86_64 -z max-page-size=4096 -T "$root/user/user.ld" -nostdlib \
        -o "$build/user/$prog.elf" "$build/user/crt0.o" "$build/user/$prog.o" \
        "$build/lib_syscall.o" "$build/lib_syscall_stub.o"
done
for utility in hexview codeedit netshare paintbook browser nvdiag; do
    kind=1
    if [ "$utility" = codeedit ]; then kind=2; fi
    if [ "$utility" = netshare ]; then kind=3; fi
    if [ "$utility" = paintbook ]; then kind=4; fi
    if [ "$utility" = browser ]; then kind=5; fi
    if [ "$utility" = nvdiag ]; then kind=6; fi
    gcc $libc_flags -fpie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=small \
        -DBOOK8_UTILITY_KIND="$kind" -c -o "$build/user/$utility.o" "$root/user/utility.c"
    ld -m elf_x86_64 -z max-page-size=4096 -T "$root/user/user.ld" -nostdlib \
        -o "$build/user/$utility.elf" "$build/user/crt0.o" "$build/user/$utility.o" \
        "$build/lib_syscall.o" "$build/lib_syscall_stub.o"
done
bash "$root/user/build-nvmap.sh" "$build/user"
printf 'MSG: read from a FAT subdirectory via sys_open/sys_read\n' > "$build/user/msg.txt"
make_data_image() { # <image> <size MiB> <FAT bits>
    rm -f "$1"
    truncate -s "${2}M" "$1"
    mkfs.vfat -F "$3" -S 512 -n BOOK8DATA "$1" >/dev/null
    mmd -i "$1" ::/DATA
    mmd -i "$1" ::/BIN
    mmd -i "$1" ::/DRIVERS
    mmd -i "$1" ::/APPS
    mmd -i "$1" ::/APPS/EXECROLLBACK
    while IFS='|' read -r source target app_key display_name app_class hidden system_component; do
        [ -n "$source" ] || continue
        case "$source" in \#*) continue ;; esac
        mcopy -i "$1" "$build/user/$source" "::/BIN/$target"
    done < "$root/installer/apps.manifest"
    mcopy -i "$1" "$build/nv_maxwell.drv" ::/DRIVERS/NVMAXWEL.DRV
    mcopy -i "$1" "$build/rtl8168.drv" ::/DRIVERS/RTL8168.DRV
    mcopy -i "$1" "$build/usbhost.drv" ::/DRIVERS/USBHOST.DRV
    mcopy -i "$1" "$build/ac97.drv" ::/DRIVERS/AC97.DRV
    mcopy -i "$1" "$build/user/hello.elf" ::/HELLO.ELF
    mcopy -i "$1" "$build/user/cat.elf" ::/CAT.ELF
    mcopy -i "$1" "$build/user/nvmap.elf" ::/NVMAP.ELF
    mcopy -i "$1" "$root/user/nvmap.cfg" ::/NVMAP.CFG
    mcopy -i "$1" "$build/user/msg.txt" ::/DATA/MSG.TXT
    mcopy -i "$1" "$build/user/hello.elf" ::/APPS/EXECROLLBACK/APP.ELF
    printf '[Package]\nname=ExecRollback\nversion=1.0.0\nentry=app.elf\n[Dependencies]\nlibs=missing.b8s\n' \
        > "$build/exec-rollback-manifest.ini"
    mcopy -i "$1" "$build/exec-rollback-manifest.ini" \
        ::/APPS/EXECROLLBACK/MANIFEST.INI
}
make_data_image "$build/book8-data16.img" 16 16
make_data_image "$build/book8-data32.img" 40 32
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/fs_loader_host_test" "$root/tests/fs_loader_host_test.c" \
    "$root/src/filesystem.c" "$root/fs/fs.c" "$root/drivers/block.c"
run_test "$build/fs_loader_host_test" "$build/book8-data16.img"
run_test "$build/fs_loader_host_test" "$build/book8-data32.img"
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -o "$build/boot_update_host_test" "$root/tests/boot_update_host_test.c" \
    "$root/src/filesystem.c" "$root/fs/fs.c" "$root/drivers/block.c"
ld -m elf_x86_64 -T "$root/linker.ld" -nostdlib -o "$build/book8.elf" \
    "$build/boot.o" "$build/kernel.o" "$build/filesystem.o" "$build/registry.o" "$build/session.o" "$build/boot_log.o" "$build/system_config.o" \
    "$build/syscall.o" "$build/syscall_entry.o" "$build/ap_trampoline.o" \
    "$build/user_smoke.o" \
    "$build/interrupt_entry.o" "$build/book8gui.o" \
    "$build/book8_explorer.o" "$build/book8_shell.o" \
    "$build/book8_editor.o" "$build/book8_settings.o" "$build/book8_login.o" \
    "$build/book8_taskmgr.o" "$build/book8_utilities.o" "$build/book8_netshare.o" \
    "$build/book8_image.o" "$build/book8_paintbook.o" "$build/book8_browser.o" \
    "$build/book8_media_extension.o" "$build/book8_player.o" \
    "$build/book8_nvdiag.o" \
    "$build/b8pkg.o" "$build/drivers_block.o" "$build/drivers_ata_dma.o" "$build/drivers_ahci.o" \
    "$build/drivers_fb.o" "$build/drivers_font8x16.o" \
    "$build/drivers_pcie.o" \
    "$build/drivers_amd_host_bridge.o" \
    "$build/drivers_pcie_probe.o" \
    "$build/drivers_rs780.o" \
    "$build/drivers_rs780_manual_audit.o" \
    "$build/net_net.o" "$build/net_tcp.o" "$build/net_smb.o" "$build/net_ntlm.o" "$build/net_dhcp.o" "$build/net_network_settings.o" "$build/net_sha256.o" "$build/net_push.o" \
    "$build/kernel_relaunch.o" "$build/relaunch_stub.o" \
    "$build/fs_fs.o" "$build/kernel_exec.o" "$build/kernel_sched.o" \
    "$build/kernel_smp.o" "$build/kernel_apic.o" "$build/kernel_vmm.o" \
    "$build/kernel_hwdiag.o" "$build/kernel_driver_loader.o" "${libc_objects[@]}"

grub-file --is-x86-multiboot2 "$build/book8.elf"
kernel_symbols="$(nm --defined-only "$build/book8.elf")"
for symbol in book8_pcie_enumerate book8_pcie_bind_drivers \
    book8_pcie_map_bar book8_pcie_prepare_target \
    book8_pcie_enable_target_command \
    book8_amd_host_bridge_probe book8_amd_root_link_ready \
    book8_gp_entry book8_pf_entry book8_read_msr_safe \
    book8_read32_safe book8_write32_safe book8_probe_begin book8_probe_ack \
    book8_rs780_read_index book8_rs780_read_link book8_probe_begin_link \
    rs780_audit_request_next rs780_audit_request_linkcap rs780_audit_request_linkstatus \
    rs780_audit_init rs780_audit_confirm_collector \
    rs780_audit_ack_intent rs780_audit_ack_result rs780_audit_finish; do
    if ! awk -v symbol="$symbol" \
        '$2 ~ /^[Tt]$/ && $3 == symbol { found = 1 } END { exit !found }' \
        <<< "$kernel_symbols"; then
        printf 'Required PCIe/MSR boot code missing from linked kernel: %s\n' "$symbol" >&2
        exit 1
    fi
done
printf 'PASS: linked kernel contains PCIe initialization and exception-safe MSR/#GP handler\n'

verify_boot_kernel() (
    local image="$1" format="$2"
    local work
    work="$(mktemp -d "$build/.boot-verify.XXXXXX")"
    trap 'rm -f -- "$work/book8.elf"; rmdir -- "$work"' EXIT
    if [ "$format" = fat ]; then
        # build-installer-image.sh places its FAT partition at LBA 2048.
        mcopy -i "$image@@1048576" ::/boot/book8.elf "$work/book8.elf"
    else
        xorriso -osirrox on -indev "$image" \
            -extract /boot/book8.elf "$work/book8.elf"
    fi
    if ! cmp -s "$build/book8.elf" "$work/book8.elf"; then
        printf 'Boot image does not contain the current PCIe/MSR kernel: %s\n' "$image" >&2
        exit 1
    fi
    printf 'PASS: current PCIe/MSR kernel packaged in %s\n' "$image"
)

# Validate the linked kernel is relaunchable (Multiboot2 header, segments, MBI slot).
run_test "$build/relaunch_host_test" "$build/book8.elf"
mkdir -p "$build"
installer_network_config=""
rm -f "$build/BOOK8NET.CFG"
bash "$root/build-installer-image.sh" "$build/book8.elf" "$build/book8-install.img" \
    "$installer_network_config" "$build/book8-target.img"
verify_boot_kernel "$build/book8-install.img" fat
cp "$build/book8.elf" "$iso_root/boot/book8.elf"
cp "$build/book8-data16.img" "$build/book8-apps.img"
mmd -i "$build/book8-apps.img" ::/BOOT
mcopy -i "$build/book8-apps.img" "$build/book8.elf" ::/BOOT/BOOK8.ELF
for app in hexview codeedit netshare paintbook browser nvdiag cat hello nvmap; do
    target="::/BIN/${app^^}.ELF"
    if [[ "$app" = cat || "$app" = hello || "$app" = nvmap ]]; then
        target="::/${app^^}.ELF"
    fi
    if [[ "$app" = paintbook ]]; then
        target="::/BIN/PAINTBK.ELF"
    fi
    if [[ "$app" = nvdiag ]]; then
        target="::/BIN/NVDIAG.ELF"
    fi
    mcopy -i "$build/book8-apps.img" "$target" "$build/.verify-app.elf"
    cmp "$build/user/$app.elf" "$build/.verify-app.elf"
    rm -f "$build/.verify-app.elf"
done
cp "$build/book8-apps.img" "$iso_root/boot/book8-apps.img"
printf 'PASS: all application ELFs packaged and verified in internal boot volume\n'
for driver in nv_maxwell rtl8168 usbhost ac97; do
    target="::/DRIVERS/${driver^^}.DRV"
    if [[ "$driver" = nv_maxwell ]]; then target="::/DRIVERS/NVMAXWEL.DRV"; fi
    verify="$build/.verify-$driver.drv"
    mcopy -i "$build/book8-apps.img" "$target" "$verify"
    cmp "$build/$driver.drv" "$verify"
    rm -f "$verify"
done
printf 'PASS: all trusted driver modules packaged and verified in internal boot volume\n'
cp "$build/book8-install.img" "$iso_root/boot/book8-install.img"
cp "$root/grub/grub.cfg" "$iso_root/boot/grub/grub.cfg"
if [ "${BOOK8_DIAGNOSTIC:-0}" = 1 ]; then
    cp "$root/grub/grub-nv-diagnostic.cfg" "$iso_root/boot/grub/grub.cfg"
    printf 'BOOK8-NVLOG-1\r\n' > "$build/NVLOG.OK"
    cp "$build/NVLOG.OK" "$iso_root/NVLOG.OK"
fi
grub-mkrescue -o "$build/$iso_name" "$iso_root"
verify_boot_kernel "$build/$iso_name" iso
printf 'Created %s\n' "$build/$iso_name"

# Same kernel, but GRUB passes book8.autostart=0 so CLI-driven tests land at
# the command prompt instead of the autostarted desktop.
rm -rf "$build/iso-cli"
cp -r "$iso_root" "$build/iso-cli"
cp "$root/grub/grub-cli.cfg" "$build/iso-cli/boot/grub/grub.cfg"
grub-mkrescue -o "$build/book8-cli.iso" "$build/iso-cli"
verify_boot_kernel "$build/book8-cli.iso" iso
printf 'Created %s\n' "$build/book8-cli.iso"
cli_iso="$build/book8-cli.iso"

# Ring 3 smoke test (U/S pages, IRETQ entry, SYSCALL trap, exit path) under QEMU.
# Set BOOK8_SKIP_RING3_SMOKE=1 to skip; BOOK8_RING3_VBOX=1 also runs it in VirtualBox.
if [ "${BOOK8_BUILD_ONLY:-0}" != 1 ] \
    && [ "${BOOK8_SKIP_RING3_SMOKE:-0}" != 1 ]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        if [ "${BOOK8_DIAGNOSTIC:-0}" = 1 ]; then
            python3 "$root/tests/qemu_boot_log_test.py" "$build/$iso_name" "$build/book8-data16.img"
        else
            python3 "$root/tests/qemu_static_programs_test.py" "$build/$iso_name" "$build/book8-data16.img"
            python3 "$root/tests/qemu_installer_smoke.py" "$cli_iso" --installer-boot-only
            python3 "$root/tests/qemu_utilities_test.py" "$cli_iso" "$build/book8-data16.img"
            python3 "$root/tests/qemu_netshare_test.py" "$cli_iso" "$build/book8-data16.img"
            python3 "$root/tests/qemu_ring3_test.py" "$cli_iso"
            python3 "$root/tests/qemu_scheduler_test.py" "$cli_iso"
            python3 "$root/tests/qemu_msr_guard_test.py" "$cli_iso"
            python3 "$root/tests/qemu_netboot_jump_test.py"
            python3 "$root/tests/qemu_exec_test.py" "$build/$iso_name" "$build/book8-data16.img"
            python3 "$root/tests/qemu_exec_test.py" "$build/$iso_name" "$build/book8-data32.img"
            python3 "$root/tests/qemu_nvmap_startup_test.py" "$cli_iso" "$build/book8-data16.img"
            python3 "$root/tests/qemu_relaunch_test.py" "$cli_iso" "$build/book8-data16.img" "$build/book8.elf"
            python3 "$root/tests/qemu_drives_test.py" "$cli_iso" "$build/book8-data16.img" "$build/book8-data32.img"
            python3 "$root/tests/qemu_storage_test.py" "$cli_iso" "$build/book8-data32.img" generic
            python3 "$root/tests/qemu_storage_test.py" "$cli_iso" "$build/book8-data32.img" target-pc
            python3 "$root/tests/qemu_storage_test.py" "$cli_iso" "$build/book8-data32.img" target-q35
            python3 "$root/tests/qemu_driver_module_test.py" "$build/$iso_name" \
                "$build/book8-data16.img" "$build/book8-data32.img"
            python3 "$root/tests/qemu_ohci_mouse_test.py" "$cli_iso" \
                "$build/book8-data16.img"
        fi
    else
        printf 'WARNING: qemu-system-x86_64 not found; Ring 3 smoke test skipped\n' >&2
    fi
    if [ "${BOOK8_DIAGNOSTIC:-0}" != 1 ] && [ "${BOOK8_RING3_VBOX:-0}" = 1 ]; then
        MATRIX_ENGINES=vbox python3 "$root/tests/emulation_matrix.py" "$build/book8.iso"
        python3 "$root/tests/storage_matrix.py" "$build/book8.iso" "$build/book8-data16.img"
    fi
fi
