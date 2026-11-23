#!/usr/bin/env bash
set -euo pipefail

kernel="${1:?usage: build-installer-image.sh <kernel> <output> [network-config] [target-disk-output]}"
output="${2:?usage: build-installer-image.sh <kernel> <output> [network-config] [target-disk-output]}"
network_config="${3:-}"
target_output="${4:-${output%.img}-target.img}"
helper="$(dirname -- "$kernel")/user/nvmap.elf"
utilities="$(dirname -- "$kernel")/user"
hash_image="$(dirname -- "$kernel")/book8-hashes.bin"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f "$hash_image" ]]; then
    printf 'Boot hash manifest is missing: %s\n' "$hash_image" >&2
    exit 1
fi
if [[ "$EUID" -ne 0 ]]; then
    printf 'Building the installer image requires root for loop-device setup.\n' >&2
    exit 1
fi
work="$(mktemp -d "$root/build/.installer.XXXXXX")"
mount_point="$work/mnt"
target_mount_point="$work/target-mnt"
loop_device=""
target_loop_device=""

cleanup() {
    if mountpoint -q "$mount_point"; then
        umount "$mount_point"
    fi
    if mountpoint -q "$target_mount_point"; then
        umount "$target_mount_point"
    fi
    if [[ -n "$loop_device" ]]; then
        losetup -d "$loop_device"
    fi
    if [[ -n "$target_loop_device" ]]; then
        losetup -d "$target_loop_device"
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT

for tool in grub-install grub-mkstandalone grub-file mkfs.vfat losetup mountpoint; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing installer-image dependency: %s\n' "$tool" >&2
        exit 1
    fi
done

if [[ ! -f "$kernel" ]]; then
    printf 'Kernel image not found: %s\n' "$kernel" >&2
    exit 1
fi
docs=(
    "README.md:README.MD"
    "DEVREADME.md:DEVREAD.MD"
    "KERNEL_NOTES.md:KERNELNO.MD"
    "CHECKPOINT.md:CHECKPT.MD"
    "OPENCODE_MAP.txt:OPENCODE.MAP"
    "INSTALLER.txt:INSTALL.TXT"
)
for file in "$helper" "$root/user/nvmap.cfg" "$utilities/cat.elf" "$utilities/hello.elf" "$utilities/msg.txt"; do
    if [[ ! -s "$file" ]]; then
        printf 'Required installer startup file missing: %s\n' "$file" >&2
        exit 1
    fi
done
for mapping in "${docs[@]}"; do
    source="${mapping%%:*}"
    if [[ ! -s "$root/$source" ]]; then
        printf 'Required BOOK8 documentation missing: %s\n' "$source" >&2
        exit 1
    fi
done
while IFS='|' read -r source target app_key display_name app_class hidden system_component; do
    [[ -n "$source" ]] || continue
    [[ "$source" == \#* ]] && continue
    if [[ ! -s "$utilities/$source" ]]; then
        printf 'Required installer app missing: %s\n' "$utilities/$source" >&2
        exit 1
    fi
done < "$root/installer/apps.manifest"
if [[ -n "$network_config" && ! -s "$network_config" ]]; then
    printf 'Installer network configuration missing: %s\n' "$network_config" >&2
    exit 1
fi

image="$work/book8-install.img"
mount_point="$work/mnt"
target_image="$work/book8-target.img"
target_mount_point="$work/target-mnt"
mkdir -p "$mount_point" "$target_mount_point"
truncate -s 68M "$image"

python3 - "$image" <<'PY'
import struct
import sys

image_path = sys.argv[1]
sectors = 68 * 1024 * 1024 // 512
start = 2048
with open(image_path, "r+b") as image:
    mbr = bytearray(512)
    mbr[446:462] = (
        b"\x80\xff\xff\xff\x0c\xff\xff\xff"
        + struct.pack("<II", start, sectors - start)
    )
    mbr[510:512] = b"\x55\xaa"
    image.seek(0)
    image.write(mbr)
PY

loop_device="$(losetup --find --show --partscan "$image")"
partition="${loop_device}p1"
for attempt in $(seq 1 50); do
    if [[ -b "$partition" ]]; then
        break
    fi
    sleep 0.1
done
if [[ ! -b "$partition" ]]; then
    printf 'Could not expose the installer FAT32 partition.\n' >&2
    exit 1
fi

mkfs.vfat -F 32 -n BOOK8 "$partition" >/dev/null
mount "$partition" "$mount_point"
mkdir -p "$mount_point/boot/grub" "$mount_point/EFI/BOOT" "$mount_point/BIN" "$mount_point/DATA" "$mount_point/DOC"
cp "$kernel" "$mount_point/boot/book8.elf"
cp "$hash_image" "$mount_point/boot/book8-hashes.bin"
cp "$helper" "$mount_point/NVMAP.ELF"
cp "$root/user/nvmap.cfg" "$mount_point/NVMAP.CFG"
while IFS='|' read -r source target app_key display_name app_class hidden system_component; do
    [[ -n "$source" ]] || continue
    [[ "$source" == \#* ]] && continue
    cp "$utilities/$source" "$mount_point/BIN/$target"
done < "$root/installer/apps.manifest"
cp "$utilities/cat.elf" "$mount_point/CAT.ELF"
cp "$utilities/hello.elf" "$mount_point/HELLO.ELF"
cp "$utilities/msg.txt" "$mount_point/DATA/MSG.TXT"
for mapping in "${docs[@]}"; do
    source="${mapping%%:*}"
    target="${mapping##*:}"
    cp "$root/$source" "$mount_point/DOC/$target"
    cmp "$root/$source" "$mount_point/DOC/$target"
done
if [[ -n "$network_config" ]]; then
    cp "$network_config" "$mount_point/BOOK8NET.CFG"
fi
cat > "$mount_point/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

insmod part_msdos
insmod fat
insmod all_video
set gfxmode=800x600x32
set gfxpayload=keep

menuentry "BOOK8 x86-64" {
    search --no-floppy --label BOOK8 --set=root
    multiboot2 /boot/book8.elf
    module2 /boot/book8-hashes.bin book8-hashes
    boot
}
EOF

grub-install --target=i386-pc --boot-directory="$mount_point/boot" \
    --no-floppy --recheck "$loop_device"
grub-mkstandalone --format=x86_64-efi \
    --output="$mount_point/EFI/BOOT/BOOTX64.EFI" \
    "boot/grub/grub.cfg=$mount_point/boot/grub/grub.cfg"
grub-file --is-x86_64-efi "$mount_point/EFI/BOOT/BOOTX64.EFI"
sync

truncate -s 8G "$target_image"
python3 - "$target_image" <<'PY'
import struct
import sys

image_path = sys.argv[1]
sectors = 8 * 1024 * 1024 * 1024 // 512
start = 2048
swap_sectors = 4 * 1024 * 1024 * 1024 // 512
root_sectors = sectors - start - swap_sectors
mbr = bytearray(512)
mbr[446:462] = (
    b"\x80\xff\xff\xff\x0c\xff\xff\xff"
    + struct.pack("<II", start, root_sectors)
)
mbr[462:478] = (
    b"\x00\xff\xff\xff\x82\xff\xff\xff"
    + struct.pack("<II", start + root_sectors, swap_sectors)
)
mbr[510:512] = b"\x55\xaa"
with open(image_path, "r+b") as image:
    image.seek(0)
    image.write(mbr)
PY

target_loop_device="$(losetup --find --show --partscan "$target_image")"
target_partition="${target_loop_device}p1"
for attempt in $(seq 1 50); do
    if [[ -b "$target_partition" ]]; then
        break
    fi
    sleep 0.1
done
if [[ ! -b "$target_partition" ]]; then
    printf 'Could not expose the target FAT32 partition.\n' >&2
    exit 1
fi

mkfs.vfat -F 32 -n BOOK8 "$target_partition" >/dev/null
mount "$target_partition" "$target_mount_point"
cp -a "$mount_point/." "$target_mount_point/"
grub-install --target=i386-pc --boot-directory="$target_mount_point/boot" \
    --no-floppy --recheck "$target_loop_device"
sync
umount "$target_mount_point"
losetup -d "$target_loop_device"
target_loop_device=""
umount "$mount_point"
losetup -d "$loop_device"
loop_device=""
mv -- "$image" "$output"
mv -- "$target_image" "$target_output"
printf 'Created BIOS/UEFI FAT32 installer payload: %s\n' "$output"
printf 'Created 8 GiB target disk (about 4 GiB FAT32 root + 4 GiB swap): %s\n' "$target_output"
