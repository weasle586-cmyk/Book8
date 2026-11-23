# BOOK8

BOOK8 (Flight of Icarus) is an experimental, freestanding x86-64 operating system with a command
shell, ATA IDE PIO and AHCI SATA drivers, and BIOS/UEFI boot support through a
hybrid GRUB ISO. This is original BOOK8 code, not a DOS batch file or Windows
application.

Created by Dan Ellis with assistance from Copilot.

## Documentation map

`README.md` is the canonical, current-state guide for building, booting and using BOOK8.
[DEVREADME.md](./DEVREADME.md) is the development companion (implementation state, source map
and app-work guidance), and [KERNEL_NOTES.md](./KERNEL_NOTES.md) is the kernel reference.
Sections in the companion documents are dated investigation logs; where an older section
conflicts with a later one or with this README, this README and the latest dated note win.

## Build on Windows

Install WSL and Ubuntu, then install the build dependencies in Ubuntu:

```sh
sudo apt update
sudo apt install build-essential dosfstools grub-pc-bin grub-efi-amd64-bin \
  mtools ovmf python3 qemu-system-x86 qemu-utils util-linux xorriso
```

In PowerShell, from the BOOK8 project directory, run:

```powershell
.\build.ps1
```

The hybrid installer ISO is written to `build/book8.iso`; the prepared raw
FAT32 disk image is `build/book8-install.img`. The Windows build script runs the
WSL build as root because creating the disposable loop-backed install image
requires loop-device and mount access. The build checks for missing
dependencies and reports what is needed.

## Boot and test safely

Boot the ISO in a BIOS or x86-64 UEFI virtual machine. For QEMU, use a PS/2
keyboard and serial console (for example, `-serial stdio`). Attach only
disposable virtual disk images when testing disk operations. Do not use
production disks: partitioning and formatting erase data, and raw `WRITE`
can overwrite filesystem metadata.

To test USB mouse input in QEMU with its legacy UHCI controller, attach a
virtual USB mouse:

```powershell
qemu-system-x86_64 -machine pc -m 256 `
  -device piix3-usb-uhci,id=uhci `
  -device usb-mouse,bus=uhci.0 `
  -cdrom .\build\book8.iso -boot d -serial stdio
```

The startup message should report an active UHCI USB HID mouse. Run
`BOOK8GUI` to verify pointer movement and clicking.

To test USB keyboard input independently of PS/2, disable the emulated i8042
and attach a boot-protocol USB keyboard:

```powershell
qemu-system-x86_64 -machine pc,i8042=off -m 256 `
  -device piix3-usb-uhci,id=uhci `
  -device usb-kbd,bus=uhci.0 `
  -cdrom .\build\book8.iso -boot d -serial stdio
```

The startup message should report `USB HID keyboard active.` The keyboard can
enter shell commands and control BOOK8GUI.

#### Boot integrity manifest

`build.sh` generates `boot/book8-hashes.bin` after the final kernel link and GRUB
loads it as the Multiboot2 module `book8-hashes`. The manifest is an unsigned
build-integrity record (`B8H1` version 1: a 16-byte header followed by 64-byte
key/SHA-256 entries), not secure boot. The current build contains 13 entries
covering the splash asset, the final kernel, the packaged `A:\BIN` payloads and
the root `CAT.ELF`/`HELLO.ELF`/`NVMAP.ELF` files. The kernel logs
`BOOK8-HASH: manifest loaded 13 entries` and refuses a trusted payload whose
bytes no longer match its manifest, printing
`BOOK8-HASH: verification failed for <KEY>` before rejecting execution. A cold
GRUB-loaded kernel cannot hash its own running image, so the build binds the
manifest to the exact linked `book8.elf`; a stale manifest fails the build.

To validate experimental multi-core startup and affinity scheduling, run the
focused QEMU regression from WSL. It boots disposable ISO-only VMs at 2, 4, 6,
and 8 vCPUs and runs the `SMPTEST` command in each:

```sh
python3 tests/qemu_smp_test.py build/book8-cli.iso
```

### Cross-checking with Bochs and VirtualBox

Use the same `build/book8.iso` when comparing emulators. Record the controller
and companion telemetry printed at startup, then run `USBSTAT` at the prompt
to sample active HID devices, completed reports, and stalled schedules. The
UHCI HID polling path never waits for a TD; if the controller's frame number
stops advancing for an extended run of polls, BOOK8 disables that HID queue
instead of blocking the shell.

For Bochs, create a VM with 256 MiB RAM, BIOS boot, and the BOOK8 ISO as its
CD-ROM. Save the settings in a Bochs configuration file (for example,
`bochs-book8.bxrc`), including the `boot: cdrom` setting and an
`ata0-master: type=cdrom, path="...", status=inserted` entry for the ISO.
Use ROM paths and USB settings provided by the installed Bochs build. Enable
COM1 file output separately from Bochs' own diagnostic log; for example:

```text
log: /tmp/book8-bochs-process.log
com1: enabled=1, mode=file, dev=/tmp/book8-bochs-serial.log
```

The COM1 file is created when the guest writes serial data, so its absence
means no guest serial output was captured; it does not by itself establish
whether the guest booted. Keep both paths writable by the Bochs process. If
Bochs starts at its debugger prompt, continue execution before waiting for BOOK8
startup output. Then launch the VM:

```powershell
bochs -q -f .\bochs-book8.bxrc
```

Enable the Bochs UHCI USB controller and attach a USB keyboard or mouse if
that Bochs build provides USB UHCI support. Bochs configuration syntax and
USB device options vary by release; verify the controller and attached devices
in the Bochs startup log before interpreting BOOK8 telemetry. Save the Bochs log
and COM1 output separately, and compare its PCI/controller and BOOK8 startup
reports with QEMU. Kernel COM1 output uses bounded transmitter polling: if the
UART never becomes ready, the affected byte is dropped rather than hanging
kernel execution. If USB UHCI is unavailable, Bochs can still be used to
compare boot behavior, but it does not validate UHCI input or companion
routing.

**Bochs 2.8 USB keyboard: use the 256 KB SeaBIOS image.** With the default
128 KB `bios.bin` (SeaBIOS 1.17.0), an attached UHCI USB keyboard makes SeaBIOS
reset the CPU repeatedly through port `0xCF9` before GRUB runs. BOOK8 never starts
(no `BOOK8-BOOT` marker, no COM1 file) and Bochs logs thousands of UHCI warnings
that come from SeaBIOS's own descriptors, not from BOOK8. Select the 256 KB image
instead (path shown is the Debian/Ubuntu `seabios` package layout; adjust for
your install):

```text
romimage: file=/usr/share/seabios/bios-256k.bin
usb_uhci: enabled=1, port1=keyboard
```

With this image the keyboard-attached boot reaches `kernel_main()` with the
marker, no CPU resets, and no UHCI warnings, and BOOK8 enumerates the keyboard
and processes its HID reports (`USBSTAT` completions rise as keys are typed).
The `speed:` options, moving the keyboard to port 2, and adding a mouse did not
help with the default image. Do not attribute warnings from a run that lacks
the guest marker and `UHCI-DESC` records to BOOK8 descriptors. Bochs normally
needs a headless or RFB display setup (for example,
`display_library: rfb, options="timeout=0"`). Bochs's default VGA mode gives
BOOK8 a text-mode fallback; the VBE configuration tested below also failed to
pass a framebuffer tag, so BOOK8GUI pointer input remains unvalidated there.

### Hardware-profile baselines (Intel G41/ICH7 and AMD SB710)

Two QEMU profiles approximate the bare-metal test boards. Run both under WSL:

```
python3 tests/run_baseline_profiles.py build/book8.iso
```

- `tests/qemu_g41_profile.py <iso> [ehci|uhci4|none]`: Q35/ICH9 with 4 UHCI + 1 EHCI, no i8042.
- `tests/qemu_amd_sb710_profile.py <iso> [none|mouse|tablet]`: no UHCI, 5 OHCI + 2 EHCI, PS/2 keyboard standing in for BIOS legacy translation.

QEMU cannot emulate SMM or BIOS USB legacy trapping, so these do not reproduce the bare-metal freezes. See KERNEL_NOTES.md sections 29-30.
### Pointer input validation

The QEMU regression suite (`python3 tests/qemu_installer_smoke.py
build/book8.iso`) passes PS/2 and USB keyboard input, UHCI relative USB mouse,
absolute USB tablet, GUI click, and wheel checks. Use `usb-mouse` for relative
movement or `usb-tablet` for absolute coordinates:

```text
-device piix3-usb-uhci,id=uhci -device usb-mouse,bus=uhci.0
-device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0
```

GRUB and the Multiboot2 header prefer `1024x768x32`, falling back to
`800x600x32`, `640x480x32`, or the bootloader's automatic mode. GRUB preserves
the selected mode for the Multiboot2 handoff. The kernel accepts the firmware's
RGB framebuffer and uses the descriptor's byte pitch. Absolute USB
coordinates are scaled from the HID logical range to the active desktop canvas,
while relative reports update by displacement. USB wheel events and four-byte PS/2 IntelliMouse wheel packets
are surfaced as `BOOK8-MOUSE` telemetry and scroll the GUI pane under the
pointer. PS/2 IntelliMouse ID 3 packets use a signed wheel byte; Explorer ID 4
packets use a signed nibble and separate extra-button bits.

For VirtualBox, use a PS/2 mouse and a graphics adapter that provides a
framebuffer, e.g. `--graphicscontroller vboxvga --vram 16 --mouse ps2`. The
Windows helper injects relative movement, wheel, and optional left-click
through VirtualBox's COM API (the VM must already be running):

```powershell
.\tests\vbox_mouse_inject.ps1 -VmName "BOOK8-Input-Validation" `
  -DeltaX 100 -DeltaY 60 -Wheel 1
```

This was verified on VirtualBox 7.2.22 with BOOK8GUI active: the cursor moved and
the guest serial log reported `BOOK8-MOUSE` coordinate and wheel values. That
VirtualBox run used PS/2 input; use `tests/qemu_ohci_mouse_test.py` for the
SB710-like QEMU OHCI USB-mouse coverage.

Bochs 2.8 remains **inconclusive for GUI pointer input**. With the 256 KB
SeaBIOS, `vga: extension=vbe`, the Bochs display VBE ROM, RFB, and the GRUB
`800x600x24` request, BOOK8 reaches the kernel and enumerates a UHCI mouse but
still receives no Multiboot framebuffer tag. BOOK8GUI therefore cannot run in
that tested configuration. See [KERNEL_NOTES.md](./KERNEL_NOTES.md) section
28 for exact test outcomes and limitations.

### Graphics and mouse runtime smoke tests

Run the QEMU framebuffer, Ring 3 draw-syscall, PS/2 IRQ12, and UHCI USB mouse
checks under WSL:

```sh
python3 tests/qemu_graphics_input_smoke.py \
  build/book8-cli.iso build/book8-data16.img
```

For VirtualBox on Windows, run:

```powershell
.\tests\vbox_graphics_input_smoke.ps1 `
  -Iso .\build\book8.iso `
  -OutputDirectory .\build\runtime-validation
```

The VirtualBox smoke VM uses BIOS boot, `vboxvga` with 16 MiB VRAM, and a PS/2
mouse. This smoke path validates PS/2 input; use the focused QEMU UHCI graphics
test or the SB710-like OHCI mouse test for USB HID coverage.
Both scripts retain serial logs and QEMU screenshots under
`build/runtime-validation`.

Validated with QEMU 10.2.1: framebuffer handoff `800x600x24`, Ring 3 sentinel
pixel matched `(13, 37, 251)`, PS/2 reported `IRQ12=12`, `PS2_PACKETS=3`,
`PS2_QUEUED=3`, and zero drops; UHCI USB reported three queued reports and zero
drops. VirtualBox 7.2.22 reported `640x480x32`; PS/2 reported `IRQ12=24`,
`PS2_PACKETS=6`, `PS2_QUEUED=6`, and zero drops. `tests/emulation_matrix.py`
also passed VirtualBox BIOS boot, Ring 3, and `EXEC hello.elf` on all three
chipset profiles. These results validate the current emulated framebuffer and
mouse paths; they do not replace physical-hardware testing.

GRUB requests `1024x768x32` first and falls back to `800x600x32` or the
bootloader's automatic mode when a requested mode is unavailable. The kernel
consumes GRUB's Multiboot2 framebuffer handoff; it does not call VBE BIOS
services or query the NVIDIA VBIOS after entering 64-bit mode.

VirtualBox can provide a second boot/PCI comparison. For example, with
`VBoxManage` installed:

```powershell
VBoxManage createvm --name "BOOK8-PCI-Test" --ostype Other_64 --register
VBoxManage modifyvm "BOOK8-PCI-Test" --memory 256 --firmware bios --boot1 dvd
VBoxManage storagectl "BOOK8-PCI-Test" --name IDE --add ide
VBoxManage storageattach "BOOK8-PCI-Test" --storagectl IDE --port 0 `
  --device 0 --type dvddrive `
  --medium "C:\Users\DCM\Documents\OS_PROJECT\book8\build\book8.iso"
VBoxManage startvm "BOOK8-PCI-Test"
VBoxManage showvminfo "BOOK8-PCI-Test" --log 0
```

USB controller availability depends on the selected chipset and VirtualBox
version. VirtualBox's USB 1.1/2.0 controller models are typically OHCI/EHCI,
which is not equivalent to a UHCI keyboard/mouse test. BOOK8 has a polled OHCI
boot-HID path, but validate it from the startup and `INPUTSTAT` telemetry; the
focused automated OHCI regression uses QEMU's SB710-like profile. Compare
detected-controller and EHCI handoff telemetry, and retain the VM log with the
BOOK8 startup output. QEMU and VirtualBox have focused automated
graphics/input smoke tests; Bochs remains a manual comparison run.

The kernel discovers legacy IDE and PCI AHCI controllers. IDE supports ATA
IDENTIFY and LBA28/LBA48 PIO reads and writes; AHCI supports IDENTIFY and
LBA48 DMA reads and writes. Sector operations use the existing disk drivers.

### Installing BOOK8 to a hard drive

Boot the ISO and select **BOOK8 Installer** in the GRUB menu. The installer
payload is loaded into memory only for this menu entry. At the shell, run
`DISK` to see numbered ATA/AHCI devices, then `INSTALL <disk-number>`. There is
no automatic or default target selection. Before writing, BOOK8 prints the
selected disk model, capacity, and exact confirmation phrase; installation
continues only when that phrase is entered exactly. A rejected confirmation
does not write to the target.

After disk confirmation the installer lists the packaged applications and asks
`Install applications ([A]ll default, [N]one, [S]elect, Q to cancel)`. `A` keeps
the default selection, `N` excludes all packaged ELFs, and `S` prompts for each
application with `Y`/`N`/`Q`. Every application is selected by default. `Q` at
either prompt cancels before writing. Deselected ELFs are removed from the
installed `A:\BIN` volume and are not registered for the Start menu.

The bundled image installs an active MBR EFI System Partition (type `0xEF`)
formatted FAT32, BIOS GRUB boot code, and the UEFI fallback loader at
`EFI/BOOT/BOOTX64.EFI`. It does not change UEFI NVRAM variables. The EFI loader
is unsigned; Secure Boot must be disabled. NTFS formatting and NTFS boot
volumes are not supported. BOOK8GUI is linked into `boot/book8.elf`, not a
separate executable.

The installer includes `NVMAP.ELF` for manual local diagnostics. The legacy
`NVMAP.CFG` is ignored at boot. New installer images do not include a
forced-static network file: DHCP is the default.
Telemetry uses UDP 8080 after an address is acquired. `BOOK8.CFG` remains
binary GUI preferences and is not generated as a text boot configuration.

The installer copies a fixed 68 MiB image: it requires a target disk at least
that large, overwrites only the first 68 MiB, and leaves later sectors
unchanged. The FAT32 boot partition is 67 MiB; the remainder of larger disks
is left unpartitioned. This initial installer is intended for disposable
QEMU images and lab disks, not production or host-development drives. BOOK8
cannot identify a Windows host disk or determine which disk supplied a
chainloaded installer ISO; carefully verify the disk index, model, and
capacity before confirming. For physical-media boot, USB mass-storage support
is not implemented; use optical media or another supported boot path.

Run the end-to-end disposable-disk test from WSL after building:

```sh
python3 ./tests/qemu_installer_smoke.py ./build/book8-cli.iso --install-only
```

For a focused check of the GRUB Installer entry's framebuffer handoff and
installer prompt, run:

```sh
python3 ./tests/qemu_installer_smoke.py ./build/book8-cli.iso --installer-boot-only
```

`--install-only` verifies confirmation cancellation, application deselection,
the exact packaged file set, installation to a temporary QEMU drive, and cold
boot through both BIOS and OVMF UEFI. `--installer-boot-only` uses that same
temporary target but cancels before writing. Omitting the mode adds the legacy
PS/2 and USB keyboard/mouse plus reboot checks; those standalone input checks
require the trusted USB driver media and are not part of the build's QEMU suite.

The command prompt accepts PS/2 or UHCI HID boot-keyboard input (and
serial-console input in QEMU); mouse input is handled in BOOK8GUI, not at the
shell prompt. UHCI mice support boot protocol or the supported non-boot HID
relative X/Y report subset. A polled OHCI driver supports boot-protocol HID
devices on the SB710-like profile, including the tested USB mouse path. Generic
HID keyboards, xHCI, and USB hubs remain unsupported.

## Shell and filesystem commands

The shell includes `HELP`, `VER`, `CLS`, `ECHO`, `DATE`, `TIME`, `SET`, `PATH`,
`PROMPT`, `REM`, `CD`/`CHDIR`, `DISK`, `READ`, `WRITE`, `INSTALL`, `HALT`,
`REBOOT`, and `RESET`. `HALT` requests ACPI S5 power-off; `REBOOT` and `RESET`
request an 8042 reset with a triple-fault fallback.

### BOOK8GUI

Run `BOOK8GUI` from the shell to open the on-demand desktop. It boots to a clean
desktop: only the "BOOK8 DESKTOP" header text and the taskbar are shown, and no
application windows auto-start. Launch File Explorer, NoteEditor, Command Shell
or System Settings from Start > Programs / Settings. The taskbar occupies every
pixel from `SCREEN_HEIGHT - 36` through `SCREEN_HEIGHT - 1`. It has a Start
launcher, active-app buttons, a time/date clock stack and a dual-terminal NIC
activity indicator. The front terminal flashes for transmitted packets and the
back terminal for received packets. Every window title bar has
minimize `[_]`, maximize/restore and close `[X]` buttons: minimizing hides the
window to its taskbar button (click it to restore), and maximizing fills the
entire screen from `(0, 0)` to the taskbar's top edge, using the active display
dimensions rather than cached launch geometry. Restoring retains the previous
frame, fitted to the display if its dimensions changed. Clicking the focused window's
taskbar button minimizes it; `Tab` also cycles focus. Use Start > Shutdown >
Exit to Prompt to leave the desktop; Escape no longer exits it.

#### Add/Remove Programs

Open **Start > Settings > Add/Remove Programs** in the Control Panel sidebar
(or press Page Up from Date & Time). The Win2K-style page has a wheel/drag-
scrollable program list on the left, with name, absolute executable target,
filesystem allocation in bytes, and System Core/User Installed status on the
right. The five kernel-linked desktop apps share `A:\BOOT\BOOK8.ELF` as their
target; its size is unavailable when that file is not on the mounted drive.

BOOK8 has no separate package database. The Programs registry index is rebuilt
from built-in shortcuts and `.ELF` files discovered on mounted drives, including
`A:\BIN\HEXVIEW.ELF`, `A:\BIN\CODEEDIT.ELF`, and `A:\BIN\NETSHARE.ELF`, included in the data and installer
images. These have friendly utility names; other filenames serve as
display names. Copying an ELF onto a mounted volume and selecting **Refresh**
registers it; no imaginary utilities are listed when their binaries are absent.
The installer also includes `A:\CAT.ELF` and its `HELLO.ELF` / `DATA\MSG.TXT`
dependencies; CAT runs from its direct Programs entry and prints to the console.
The Start > Programs submenu expands upward to show as many complete rows as
fit above the taskbar. Larger registries scroll with the wheel or a draggable
scrollbar. Every installed application has a direct row beside File Explorer
and NoteEditor, with no Utilities child popup. The static Programs array always
includes HexViewer, CodeEditor, NetShare and CAT, even without a readable
application disk. These shortcuts execute the corresponding ELFs in `A:\BIN`
(CAT uses `A:\CAT.ELF`); missing binaries report a launch error.
Normal ISO boot loads the bundled `book8-apps.img` FAT16 module as the internal
`A:` volume. It includes HexViewer, CodeEditor, NetShare, CAT, HELLO and NVMAP,
plus the kernel and sample document. No separate application disk is required.
Add/Remove Programs scans only this internal volume; attached disks remain
available in Explorer but are not searched for applications. Changes to this
RAM-backed volume (including uninstall and saves) last only until reboot.
An installed boot without the module scans only its `A:` system volume when
`A:\BOOT\BOOK8.ELF` exists. The CLI test ISO intentionally omits the module
so disk-based filesystem and persistence tests keep their original drive paths.
Discovered applications follow these fixed rows without duplicate targets.
Scrollbar drags retain the point grabbed inside the thumb, including when
the pointer moves outside the list or across the Settings sidebar.
Discovery supports 512 registry entries, 512 directories, 32 directory levels
and paths up to 511 characters. Genuine scan limits or directory read failures
still produce an explicit incomplete-scan warning.

#### Hex Viewer and Code Editor

**Start > Programs** contains **HexViewer** and **CodeEditor** directly.
Both are independently
linked ELF entrypoints installed in `A:\BIN`, registered as removable user
applications. They request kernel-hosted BOOK8GUI windows through
`SYS_gui_utility`; the ELF exits after the request. This follows the existing
kernel-native window architecture, not a separate user-mode window manager.
Close their windows before uninstalling. Removing their ELFs immediately
removes their discovered registry entries and Explorer Open With associations.
The four static Programs shortcuts remain visible after removal; launching
one without its executable reports an error.

**Hex Sector Viewer** is read-only. Open a binary file to view 16-byte rows
with eight-digit offsets, hex bytes and printable ASCII (`.` for other bytes).
Wheel input scrolls three rows per notch; use the vertical scrollbar or
Up/Down/Page Up/Page Down/Home/End. Left/Right pan horizontally when the full
row cannot fit. **FAT Cluster** reads a mounted drive's data cluster, e.g.
`A:2`; it does not write storage or interpret a physical-sector address.
Files are bounded to 1 MiB.

**Code Editor** is a plain text editor with a line-number margin, typing,
Backspace/Delete, Enter, four-column tabs, arrows, Home/End and page navigation.
There is no autocomplete, syntax autofill, AI assistance or IDE popup machinery.
**Ctrl+S** saves to the active file; **Save As** / **Ctrl+Shift+S** opens an
absolute-path modal and updates the title after successful writing.
**Ctrl+O** opens another file. Text is bounded to 64 KiB, and failures retain
unsaved text. Save changes before opening another file or closing the window;
Task Manager can explicitly force termination. Four combined Hex Viewer/Code
Editor windows can be open concurrently, plus one Network Share Browser.

FAT16/FAT32 saves support root or existing subdirectories, e.g.
`A:\SYS\CONFIG.CFG`; the parent must already exist. Existing files use verified
copy-on-write replacement, and read-only/system targets are refused. A directory
commit I/O failure may be indeterminate; new-file creation is not transactional.
From the shell, `EXEC A:/BIN/CODEEDIT.ELF A:/DATA/MSG.TXT` or
`EXEC A:/BIN/HEXVIEW.ELF A:/DATA/MSG.TXT` opens the document in BOOK8GUI.

#### Local Network Share Browser

Launch **Start > Programs > NetShare**, or
`EXEC A:/BIN/NETSHARE.ELF`. The standalone ELF requests a kernel-hosted window
titled **Local Network Share Browser**, and is removable through Add/Remove
Programs after its window closes.

The IP field defaults to the current IPv4 gateway (or `0.0.0.0` when offline).
Enter the host IP and a **Share** name (default `public`), then **Connect**.
Choose **Guest**, or enter a username, optional domain and masked password.
Login failures reopen the login dialog with a clear explanation and allow retry
or cancellation; share/file permission errors are reported separately.
Directories appear on the left; files and byte sizes appear on the right.
Click a directory to enter it, use **Up** to return, and scroll either pane
using its scrollbar or mouse wheel. A downloaded file also has a short
read-only ASCII preview. **Cancel** aborts pending connections/transfers.
Invalid addresses, offline networking, timeouts and server errors appear in
a dismissible status modal.

**Local**/**Remote** switches the two panes between the local folder and the
connected share. Edit **Local:** and press Enter to browse an absolute drive
path; **Up** returns to its parent. **Upload** sends a selected local file to the
current remote folder. Uploads and downloads are limited to 1 MiB and never
overwrite existing destination files. Cancelled/failed uploads can leave a
partial remote file: check the destination before retrying.
**Rename**, **Delete** (with confirmation), and **New folder** operate only on
local entries using the existing filesystem protections. Up/Down selects files
or folders without entering them; Enter opens a selected folder.
**Properties** shows local/remote size, allocation, attributes, location and
available timestamps (remote dates are UTC). **Edit** opens a selected local
text file; **Explorer** opens its local folder.

**Profiles** saves up to eight server/share profiles in `A:\NETSHARE.CFG`,
including local folder and username/domain but **never passwords**. Choose a
profile row and **Load**, **Save current**, or **Remove**. Saved profiles survive
reboots and application removal. With **Remember password for this session**
checked, successful profile logins are reused across reconnects/window closes.
The first connection after a reboot prompts again. Uncheck it for a transient
login; authentication failures forget the cached password and prompt again.

Select a file and choose **Download to Local**. The local field defaults to
`A:\DOWNLOADS\`; this directory is created on FAT if absent. Other destinations
are supported when their parent already exists. Downloads are read-only on
the server, limited to 1 MiB, and **never overwrite an existing local file**.
**Edit** immediately launches the installed Code Editor; binary/non-text input
is explicitly rejected there. **Explorer** opens a new window in the download
directory, where downloaded files use the existing associations/Open With.
New-file/directory creation is not transactional: a disk I/O failure may leave
partial local state and is always reported.

Protocol scope is deliberately bounded: one TCP connection to port 445,
SMB2.0.2/SMB2.1 negotiation, anonymous NTLM guest/null or NTLMv2 username/password
session setup, SMB2 HMAC-SHA256 signing, named disk tree connect, directory
queries, create/read/write/close. Only ASCII names/login fields and up to
128 entries per directory are supported. Supply the share name explicitly:
automatic share-name discovery, DNS hostnames, SMB1/SMB3, required encryption,
NTLM MIC/channel-binding policies, DFS referrals and arbitrary user-mode sockets
are not implemented. Unsupported server policies fail explicitly; this is a
bounded LAN file-transfer client, not a general Windows domain client.
Signing-required servers need a credential login, not Guest.
Networking requires BOOK8's supported RTL8168/8169 NIC
and a configured IPv4 address.

**Remove** (or Delete/Enter on the selected entry) asks
“Are you sure you want to remove [App Name] from BOOK8OS?” Yes/Enter confirms;
No/Escape cancels. Successful disk deletion immediately removes the launch
shortcut from the active index. Discovery on subsequent launches/boots omits
the deleted file, so removal persists without a second registry file.
Deletion failures retain the shortcut and show an error; Refresh rescans disk.
Uninstall removes only the executable, not its documents or configuration.

Built-in desktop/shell components remain visible as read-only System Components.
Raw `NVMAP.ELF`, `BOOK8.ELF`, `KERNEL.ELF`, `SHELL.ELF` and `BOOK8GUI.ELF`
are excluded from application discovery. The scanner does not descend into
root `BOOT`, `DRIVERS`, `SYSTEM`, `EFI`, `MODULES` or `KERNEL` trees; those are
OS infrastructure, not installed applications. This also avoids exhausting
application scan limits on bootloader/module directories. File Explorer and
NoteEditor retain their built-in registrations, while `EXPLORER.ELF`,
`EDITOR.ELF`/`TEXTEDIT.ELF` and `IMGVIEW.ELF`, when installed outside those trees,
have friendly application names. Files marked read-only/system, or whose
attributes cannot be read, are also protected. The destructive API rechecks
file attributes before deletion; protection does not rely on button color.
Discovery supports 512 registry entries (including built-ins), 512 directories,
32 directory levels and paths up to 511 characters; a directory I/O error or
reached limit is explicitly reported as incomplete, identifying the failing
directory and filesystem error.
Unreadable/unformatted drives are skipped before the
walk. Empty names, dot entries and invalid path components cannot consume an
index; duplicate targets are registered once. FAT short-name null padding and
missing creation timestamps are tolerated. Missing allocation/attribute metadata
does not hide an ELF: its size is unavailable and removal stays protected when
the metadata backend cannot read its attributes. The scan reports these counts
separately from directory failures. This is
ELF registration, not a package installer, dependency manager or ELF signature
validation service.

Directory enumeration accepts drive roots in either `A:` or `A:\` form.
Empty and root-relative paths (`/`, `\`, `/BIN/`) resolve on the current command
drive, mounting it as needed. Only implicit paths may fall back to the first
readable filesystem volume if that drive is unavailable, with a diagnostic.
An explicit missing drive or directory still fails rather than scanning a
different volume silently.

System Settings is a categorized control panel with eight categories: Network &
Internet, Input & Devices, Apps & Default Apps, Display, Sounds, Storage,
System and User Profile. Use the sidebar (wheel-scrollable in small windows),
or PgUp/PgDn to switch categories. Tab/Up/Down selects a field; Left/Right
changes it. Sliders also support pointer dragging, and toggles can be clicked.
Enter or Apply saves edits from all pages; Reset discards unsaved edits. The
`About` button in the header, or F1 from any category, opens the About BOOK8
dialog with the GUI version and links to the packaged documentation in
`A:\DOC`. Enter or `Open` launches the selected document in Code Editor.

Preferences live in a shared, range-checked system configuration and are saved
to `A:\BOOK8.CFG` with a versioned little-endian header and checksum. They are
loaded when the desktop first starts after boot and survive closing/reopening
Settings or BOOK8GUI. A writable root volume is required; a failed save leaves
active preferences unchanged and reports an error. Missing or invalid files
are reported and retain defaults. The underlying filesystem's replace-write
is not power-loss atomic; a corrupt/truncated file is rejected on the next boot.

Date & Time includes calendar fields, fixed UTC offsets in 15-minute steps
(no daylight-saving rules), and an RTC-sync toggle. The RTC is assumed to hold
UTC. With sync off, manual changes use a session-only software clock rather
than rewriting CMOS; the chosen offset and sync policy still persist.
Pointer speed and button orientation apply to PS/2 and relative USB input;
absolute tablets retain their absolute coordinates. Explorer uses the selected
double-click threshold. Accent colors use an eight-color Windows-style palette,
and Dark/Light affects desktop chrome and the shared native app widgets.

Hardware requests are distinct from live telemetry: repeat/delay, Vsync, network
connection and mixer-volume preferences remain editable and persist even where
the hardware service is absent. Display choices switch the software logical
canvas (800x600, 1024x768 or 1440x900), including its drawing pitch, compositor
layout and mouse-coordinate range; the canvas is scaled into the firmware-owned
boot scanout. This does not reprogram the monitor or perform NVIDIA/VESA
modesetting. The display page shows the active logical canvas and boot
framebuffer separately.
Network readouts show PCI detection (or the actual RTL8168/RTL8111 MAC address
after successful bring-up), unsampled link state and configured IPv4 addresses
(explicitly labeled zero-address defaults before configuration), not a simulated
connection. PC speaker test tones are
bounded to 200 ms and honor the applied mute/zero-volume preference; there is
no PCM mixer driver.

### Realtek PCIe GbE bring-up

The kernel scans all PCI buses/functions for `10EC:8168` Ethernet controllers
(RTL8168/RTL8111), including the onboard adapter at `3:0.0`. It sizes and restores
the BARs with decoding disabled, handles 32/64-bit memory BARs, and maps each
assigned MMIO aperture uncached through the existing low-4-GiB identity mapping.
Addresses are taken from PCI configuration, not hardcoded: an aperture such as
`0xFDDFC000–0xFDDFFFFF` and I/O BAR `0xCE00–0xCEFF` remain firmware-assigned.
The I/O BAR is retained as a port range; x86 I/O ports use IN/OUT instructions,
not kernel virtual-memory mappings. The first register-sized memory BAR supplies
the register window.

Bring-up enables BAR decoding, masks interrupts, issues the CR (`0x37`) software
reset (`0x10`), polls for completion with a bounded one-million-iteration limit,
unlocks CFG9346 (`0x50`) with `0xC0`, and reads MAC ID bytes `0x00–0x05`.
Protected configuration is relocked afterward. Serial `BOOK8-RTL8168:` lines
report discovery, physical BAR addresses/sizes, reset errors and the actual MAC.
Invalid/unassigned BARs, memory above 4 GiB, PCI command readback failures,
reset timeout and invalid MAC addresses are reported explicitly.
After successful reset/MAC discovery, the kernel starts polled packet I/O:
eight RX and eight normal-priority TX descriptors in persistent, contiguous
kernel BSS, with each ring 256-byte aligned (stricter than 256-bit alignment).
Each 16-byte descriptor contains OWN/EOR/status/length, an options word and a
64-bit physical buffer address. EOR terminates each ring; the length field is
the buffer/frame size, not the descriptor count.

Each descriptor has a dedicated 1536-byte buffer. The physical RX/TX ring
addresses, including their upper 32 bits, are written to RDSAR (`0xE4–0xEB`)
and TDSAR (`0x20–0x27`). Bus mastering and CR TX/RX bits are enabled only after
the rings and receive filters are ready. INTx and device interrupts stay masked;
checksum/VLAN offloads are disabled. The receive filter accepts own-MAC unicast
and broadcast packets. No PHY-revision-specific tuning or link negotiation
driver is implemented yet.

[rtl8168_transmit](./drivers/rtl8168.c) copies a 14–1514-byte Ethernet frame
without FCS into its dedicated TX buffer, zero-pads short frames to 60 bytes,
publishes FS/LS/OWN after a DMA memory barrier, and rings TxPoll (`0x38`, `0x40`).
It returns immediately after enqueueing, without polling for completion.
`tx_packets` counts enqueued frames, not delivery to a peer. A full ring returns
`RTL8168_TX_BUSY` immediately; hardware-owned buffers remain untouched until
ownership returns. DHCP, ARP and telemetry use this same non-blocking transport.

[rtl8168_receive](./drivers/rtl8168.c) polls one RX descriptor, copies a complete
error-free frame without its four-byte FCS, then rearms the descriptor with
OWN/EOR and the buffer capacity. Empty rings return `RTL8168_RX_EMPTY`;
error/fragmented/oversized frames are dropped with `RTL8168_RX_DROPPED`.
`RTL8168_BUFFER_SMALL` drops the frame and reports the required output size.
The APIs are serialized, single-caller kernel APIs, not interrupt-safe services.
The DMA allocation must remain alive and coherent while the NIC is running;
live ring reinitialization is rejected. The kernel network pump described below
consumes RX frames; no DHCP or userspace network API is implemented.

The build runs [rtl8168_host_test.c](./tests/rtl8168_host_test.c) with ASan/UBSan
to cover discovery (including multifunction devices), BAR restoration/sizing,
mapping failures, reset completion/timeout, MAC validation, 64-bit DMA addresses,
ring programming/alignment, TX padding/full-ring backpressure/late completion and RX
recycling/FCS removal/errors/wraparound. QEMU currently
provides `rtl8139`, not RTL8168/RTL8111; QEMU boot checks cover the absent-device
path, not real RTL8168 reset timing or PHY behavior.

### Minimal kernel network stack

[net/net.h](./net/net.h) defines packed Ethernet II, Ethernet/IPv4 ARP, IPv4,
ICMP echo and UDP headers, plus `book8_ntohs`/`book8_ntohl` and host-to-network
helpers. All API addresses/ports use host-order integers; for example
`BOOK8_NET_IP(192, 168, 7, 10)`. [net/net.c](./net/net.c) uses only caller-owned
fixed storage: two frame buffers, eight ARP cache entries and eight UDP bindings.
There are no heap allocations, pending-datagram queues or hidden socket objects.

The stack starts **unconfigured** and begins non-blocking DHCP on UDP 67/68.
Saved DHCP/static settings can override this default (see
[Baked network baseline](#baked-network-baseline-push-updates-and-relaunch)).
To change it at runtime from either the CLI or GUI Command Shell:

```text
NETCONFIG 192.168.7.10 255.255.255.0 192.168.7.1
NETCONFIG DHCP
NETCONFIG RENEW
NETSTAT
NETCONFIG OFF
```

These addresses are examples. A `0.0.0.0` gateway permits only
local-subnet sends. `NETCONFIG` is session-only (use `NETSAVE` to keep it), accepts contiguous /1–/30
masks, rejects network/broadcast/invalid host addresses and off-subnet gateways,
and clears the ARP cache. No supported NIC means an explicit configuration
error. `NETSTAT` reports actual configuration, RX/TX, malformed/ignored frames,
NIC I/O errors, ARP replies, ICMP replies and delivered UDP datagrams.

System Settings > Network exposes DHCP/forced-static mode, static fallback
IP/mask/gateway, collector IP, discovery retry interval (1000--16000 ms),
assigned addresses, DNS, remaining lease and state. Apply is runtime-only;
Save persists mode and fallback settings, not a DHCP lease; Renew explicitly
renews/restarts DHCP. The network-enable toggle suspends IPv4/DHCP/telemetry
and restarts the selected mode when re-enabled. `book8.net=off` remains a boot
override. Static configuration stops DHCP before applying the new address.

[net/dhcp.c](./net/dhcp.c) processes DISCOVER/OFFER/REQUEST/ACK, NAK,
exponential retries (capped at 64 seconds), T1 renewal, T2 broadcast rebinding
and lease expiry without blocking. Each poll makes at most one TX attempt;
RX records replies and applies addresses only after callback completion.
Transactions validate the XID, MAC, server identity, bounded options (including
overload), mask, router, DNS and lease timers. IPv4 headers and nonzero UDP
checksums are verified against the actual destination, including broadcasts;
IPv4 UDP checksum zero is permitted by the protocol. Fragments and IPv4
options are not supported. DNS is displayed, not a new DNS resolver.
This lightweight client does not yet implement ARP duplicate-address detection
or DHCP DECLINE/RELEASE; server reservation/conflict management is recommended.
The old static values are used **only after a 30-second DHCP acquisition
timeout**, never at DHCP startup. DHCP continues in the background while that
fallback is active. A subsequently validated ACK replaces all three active
interface values (IP, subnet mask and gateway), even when different from the
fallback. Forced-static mode is only used when explicitly selected.
Acquisition retries and NAK-triggered rediscovery do not restart the fallback
deadline; losing an established lease starts a new 30-second acquisition window.

The CLI input loop and desktop input pump service up to eight RX frames per
poll via `rtl8168_receive`; the returned FCS-free frames are dispatched before
the next poll. Valid ARP requests for the assigned IP receive automatic replies,
including address-conflict probes. Targeted valid ARP replies/requests populate
the bounded round-robin cache. Entries have no time-based expiry; reconfiguration
flushes them. ICMP echo requests to the assigned unicast address receive replies
with preserved identifier/sequence/data and recomputed ICMP/IPv4 checksums.
Malformed lengths/checksums, fragmented IPv4, multicast sources, unsupported
IPv4 options, VLANs, TCP and other protocols are rejected or counted as ignored.
IPv4 Ethernet padding is not treated as payload. No fragment reassembly, TCP,
DHCP, DNS, ICMP error generation, multicast or broadcast UDP is implemented.

Kernel subsystems can get the ready context with `book8_kernel_network()` (NULL
when unavailable), then bind/send through the functions in [net/net.h](./net/net.h):

```c
static void on_udp(void *ctx, uint32_t peer, uint16_t port,
    const uint8_t *payload, size_t size)
{
    struct book8_net *net = ctx;
    enum book8_net_result result =
        book8_udp_send(net, 9000, peer, port, payload, size);
    /* Handle result explicitly; ARP_PENDING means poll and retry, not sent. */
    handle_network_result(result);
}

/* Run in serialized kernel context; check the getter and every result. */
struct book8_net *net = book8_kernel_network();
if (net != NULL) {
    enum book8_net_result result = book8_udp_bind(net, 9000, on_udp, net);
    handle_network_result(result);
}
```

`handle_network_result` above stands for the subsystem's own error handler.
Callbacks borrow RX payload bytes only until they return and may send replies;
recursive polling/configuration is rejected. Unbind with `book8_udp_unbind`.
Sends require a bound source port, support 0–1472 payload bytes, always generate
a UDP pseudo-header checksum and route through the gateway for off-subnet peers.
On a cache miss, an ARP request is sent and `BOOK8_NET_ARP_PENDING` is returned:
the caller must poll and retry, retaining its own payload and controlling retry
timing. A nonzero incoming UDP checksum is verified; zero is legal for IPv4.
NIC failures propagate explicitly; malformed/ignored frames are counted rather
than printing unbounded per-packet console messages.

The build runs [net_host_test.c](./tests/net_host_test.c) under ASan/UBSan. Tests
use an independent byte-oriented wire oracle for serialization, checksums,
unaligned input, truncation boundaries, ARP learning/replies/cache replacement,
ICMP echo/padding, UDP callbacks/routing/checksums/limits/binding exhaustion,
poll budgets and error propagation. A simulated RTL8168 DMA test exercises
RX descriptor consumption through protocol dispatch into actual TX descriptors.
QEMU still cannot validate real RTL8168 wire traffic or PHY behavior.

### UDP telemetry

Default builds stream telemetry after DHCP acquisition to the configured target
(192.168.50.17:8080, see [Baked network baseline](#baked-network-baseline-push-updates-and-relaunch)).
To change or stop it at runtime:

```text
TELEMETRY 192.168.7.20
NETSTAT
TELEMETRY OFF
```

A build with `BOOK8_TELEMETRY_TARGET=none` (and no saved collector target)
starts with telemetry disabled. It uses UDP source/destination port **8080**
and normally samples at most once per second from the CLI/desktop service pump,
never from an interrupt handler. Queued `PCIE1` boot events temporarily use up
to 100 packets/second (see the PCIe/chipset prerequisite below).
Long-running commands can delay samples; missed
intervals are skipped rather than sent in a burst. `NETCONFIG OFF` also stops
telemetry and releases its UDP binding. Runtime `TELEMETRY` changes persist only
through `NETSAVE`. Only explicitly supplied log text and kernel event strings
(boot config source, relaunch, PCIe enumeration/power events) are sent;
console logs are not captured.
UDP telemetry is unauthenticated/unencrypted: use a trusted network.

The zero-allocation singleton dispatcher lives in [net/net.c](./net/net.c).
`net_telemetry_init(net, host_order_target)` validates the destination and
reserves port 8080, or returns an explicit binding/configuration error.
`net_telemetry_init(net, 0)` stops it without disturbing another port owner.
The network context must outlive the stream. `net_telemetry_send(timestamp_ms,
&metrics, log)` serializes host-order metrics into a static 204-byte packet
and uses the normal UDP dispatcher. Calls must be serialized; reentry is
rejected. ARP misses return `BOOK8_NET_ARP_PENDING`; samples are not queued,
and periodic servicing tries again with fresh metrics on the next interval.
The sequence starts at zero and advances only on a successful UDP submission;
failed/ARP-pending sends do not consume it. A successful submission is not
proof that the collector received the UDP packet. Errors are reported.

All integer fields below are **big-endian**, with `#pragma pack(push, 1)` /
`#pragma pack(pop)` protecting the wire types in [net/net.h](./net/net.h):

| Byte offset | Type | Field |
|---|---|---|
| 0 | uint32 | Magic `0x424B3801` |
| 4 | uint32 | Sequence (wraps modulo 2^32) |
| 8 | uint64 | Monotonic uptime timestamp, milliseconds |
| 16 | uint32 | CPU flags: bit 0 desktop active, bit 1 interrupts enabled, bit 2 NIC running |
| 20 | uint32 | Calibrated TSC MHz (not CPU utilization or live core frequency) |
| 24 | uint64 | Firmware-reported usable RAM, bytes |
| 32 | uint64 | Reserved low-memory extent through kernel end, bytes |
| 40 | uint64 | Heap used bytes, including allocator tags |
| 48 | uint64 | Heap total bytes |
| 56 | uint32 | RTL8168 TX frames enqueued (not peer delivery) |
| 60 | uint32 | RTL8168 accepted RX packets |
| 64 | uint32 | RTL8168 TX submissions rejected for invalid packet or busy ring |
| 68 | uint32 | RTL8168 RX frames dropped, including undersized output buffers |
| 72 | uint32 | RTL8168 TX timeouts (separate: DMA may still complete later) |
| 76 | char[128] | Fixed log text, at most 127 bytes plus NUL; unused bytes zero |

Memory values are separately labeled: heap usage is not total physical RAM
usage, and the heap is already contained in the reserved kernel extent.
Counters are uint32 snapshots and wrap modulo 2^32. The periodic log is
`CLI service` or `desktop service`; CPU flags describe sampling context,
not inferred CPU load.

**Telemetry v2 (current kernels).** The kernel now sends 292-byte packets
with magic `0x424B3802` (`net_telemetry_send_v2`). Bytes 0–75 are identical
to v1. Bytes 76–163 add 22 big-endian uint32 diagnostics, and the
128-byte log moves to offset 164:

| Byte offset | Field | Meaning |
|---|---|---|
| 76 | build_id | `date +%s` at build time (also `build/build-id.txt`) |
| 80 | health_flags | bit 0 IPv4, 1 baked config, 2 file config, 3 push agent, 4 PHY link, 5 storage, 6 USB kbd, 7 USB mouse, 8 PS/2 kbd, 9 PS/2 mouse, 10 framebuffer, 11 PCI audio present, 12 ACPI S5, 13 relaunched |
| 84 | relaunch_count | In-RAM relaunches since the last cold boot |
| 88 | phy_status | RTL8168 PHYstatus (0x6C): FDX 0x01, LINK 0x02, 10M 0x04, 100M 0x08, 1000M 0x10, RX/TX flow 0x20/0x40 |
| 92, 96 | tx_ring_pending, rx_ring_ready | Descriptors owned by the NIC / filled and awaiting the driver |
| 100–112 | net_rx/tx/invalid/ignored_frames | Protocol-stack frame counters |
| 116, 120 | net_rx_errors, net_tx_errors | Stack RX poll / TX submission errors |
| 124–132 | arp_replies, echo_replies, udp_received | Responder and dispatcher counters |
| 136 | arp_entries | Valid ARP cache entries |
| 140–148 | poll_iterations, poll_avg_us, poll_max_us | Network service loop iterations and TSC-timed avg/max cost, all reset after each sample (per-interval) |
| 152 | telemetry_failures | Failed or ARP-pending telemetry sends |
| 156, 160 | push_accepted, push_rejected | Push-agent authenticated commands / dropped datagrams |

A collector decodes v2 with `struct.unpack("!IIQIIQQQQIIIII" + "I" * 22 +
"128s", datagram)`. The bundled
[telemetry_listener.py](telemetry_listener.py) decodes v1 and v2
(`python telemetry_listener.py --source 192.168.50.128`), prints the decoded
health/PHY flags, announces build or relaunch changes, and logs to
`book8_telemetry.log`. On Windows, allow inbound UDP 8080 in the firewall.
Start the existing listener before booting the target machine:

```powershell
python .\telemetry_listener.py --bind 0.0.0.0 --port 8080 --log book8_telemetry.log --raw-out boot-capture.jsonl
```

The collector default is `192.168.50.17`. The last reported BOOK8 DHCP lease
is `192.168.50.128`; `192.168.50.208` is only the timeout fallback. Use the
collector's actual address as `BOOK8_TELEMETRY_TARGET` when building, and check
`NETSTAT` before setting `--source` because a DHCP lease can change. Only one listener/profile
process should bind port 8080 at a time.
For a structured hardware profile, run
[telemetry_profile.py](telemetry_profile.py) either live
(`--live 60 --source 192.168.50.128`, with the listener stopped) or on a
`telemetry_listener.py --raw-out capture.jsonl` capture
(`--raw capture.jsonl`). Add `--serial-log <COM1 capture>` for the NIC PCI
location, BARs, MAC, driver status, NETSTAT counters, and ACPI details,
which are not in the UDP packet. The profiler handles uint32 wraparound,
sequence loss, and reboots. With v2 packets it reports PHY link/speed and adds a
v2 diagnostics section (health flags, ring and poll statistics, build/relaunch
history). It labels audio, C-states, and power-management registers as not
reported because BOOK8 does not measure them.
The 64-bit `book8_htonll`/`book8_ntohll` helpers complement the existing helpers.

Host tests verify one-byte struct alignment, exact sizes/offsets, independent
wire decoding, UDP checksums, all metric fields, log truncation/zero filling,
unaligned metric input, sequence behavior, ARP pending, reentry and port ownership.

### Baked network baseline, push updates and RELAUNCH

**DHCP defaults.** `build.sh` compiles static editor defaults into the kernel;
a fresh image acquires its active IPv4 configuration from DHCP:

| Build variable | Default | Meaning |
|---|---|---|
| `BOOK8_NET_IP` | `192.168.50.208` | DHCP timeout fallback/editor IPv4 (`none` = unset) |
| `BOOK8_NET_MASK` | `255.255.255.0` | Subnet mask |
| `BOOK8_NET_GATEWAY` | `192.168.50.1` | Gateway (`none` = on-link only) |
| `BOOK8_TELEMETRY_TARGET` | `192.168.50.17` | Telemetry collector, UDP 8080 (`none` = off) |
| `BOOK8_PUSH` | `0` | Disabled; any other value is rejected |

Override them for one build from PowerShell, for example
`$env:BOOK8_NET_IP='10.0.0.5'; .\build.ps1`. `build.ps1` forwards only variables
that are set, and only `[0-9A-Za-z.]` values. Use `none` rather than an empty
string. These values do not force static mode. The build prints the effective defaults and writes
`build/build-id.txt`.

Boot precedence: `book8.net=off` disables networking, then saved
`A:\BOOK8DHCP.CFG` (36-byte versioned binary settings), then legacy
`A:\BOOK8NET.CFG` as DHCP timeout fallback values, otherwise DHCP.
A malformed settings file is reported and retains DHCP defaults, not a silent
static fallback. Configuration is applied only when an RTL8168 is running.
`NETSAVE` stores mode, retry, static fallback and collector in `BOOK8DHCP.CFG`,
not the current DHCP lease. Delete both saved files to restore DHCP defaults.
The legacy file remains readable as fallback configuration (not an automatic
static-mode selection), one `KEY value` per line:

```text
BOOK8NET 1
IP 192.168.50.208
MASK 255.255.255.0
GATEWAY 192.168.50.1
TELEMETRY 192.168.50.17
```

`NETSTAT` shows DHCP state/lease/rejections, active source, static editor
defaults, build id, relaunch count and push-agent
counters.

**Push agent disabled.** BOOK8 does not start the UDP 8081 push agent or
generate a push key. `BOOK8_PUSH=1` is rejected by the build. The retained
client and protocol host tests do not imply that a running kernel accepts
remote pushes. `RELAUNCH <file.elf>` remains available for a kernel already
on `A:`.

### TFTP client and warm NETBOOT

The zero-allocation TFTP client in [net/net.c](./net/net.c) supports RFC 1350
`octet` RRQ, 512-byte DATA, ACK and server ERROR packets. Packed headers use
network byte order. It binds a free ephemeral UDP port, sends RRQ to port 69,
then locks to the configured server IP and the first valid DATA block's transfer
port. Wrong peers/ports, out-of-order blocks and malformed packets are ignored.
Duplicates are re-ACKed without copying twice. A short block completes a file;
an exact multiple of 512 requires a terminating zero-byte DATA block.

`tftp_client_init(net, server_ip, capacity, &clock)` configures the singleton
client; its clock hooks must remain valid and advance during polling.
`tftp_fetch_kernel(filename, load_address)` synchronously polls UDP, copies only
in-order payload within the configured capacity, and returns an explicit result.
`tftp_fetched_size()` is nonzero only for a successfully fetched nonempty file;
failures report zero even if partial bytes remain in the buffer. Server ERROR
codes are available through `tftp_remote_error()`. No dynamic storage, filename
allocation, option negotiation, block-size extension or block-number rollover is
used: the hard image limit is 8 MiB and filenames are 1–127 printable ASCII bytes.

Unanswered RRQs/ACKs are retransmitted after one second, up to five retries per
block. ARP resolution uses the same bounded retry window. Duplicate traffic does
not reset progress timeouts. After ACKing the final block the client waits two
seconds to re-ACK a retransmitted final DATA, then releases the UDP binding.
Network errors, buffer overflow, remote ERROR and timeout abort the transfer.
The fetcher must run in serialized kernel context, not a UDP callback/ISR.

From the **CLI**, after assigning IPv4, load a trusted warm-entry image:

```text
NETCONFIG 192.168.7.10 255.255.255.0 0.0.0.0
NETBOOT 192.168.7.20 kernel.warm
```

NETBOOT is rejected from the running desktop. It verifies the complete
80–88 MiB load window is usable RAM in the firmware memory map, outside the
running kernel, all Multiboot modules, the Multiboot information block and the
framebuffer, before writing any file bytes. No fallback address is guessed.
It fetches into physical/identity-mapped virtual `0x05000000`, validates the
following **little-endian** 32-byte executable header, then zeroes the image's
declared BSS tail. Ordinary ELF, Linux, PE and Multiboot images are rejected:
BOOK8's current `_start` expects a 32-bit boot entry, not a long-mode jump.

| Offset | Type | Field |
|---|---|---|
| 0 | uint64 | `0x314D524157384B42` (ASCII `BK8WARM1`) |
| 8 | uint32 | Version, exactly 1 |
| 12 | uint32 | Header size, exactly 32 |
| 16 | uint32 | Exact fetched file size, including header |
| 20 | uint32 | Total in-memory size, file size through 8 MiB |
| 24 | uint32 | Entry offset, at least 32 and strictly inside file bytes |
| 28 | uint32 | IEEE CRC32 of the entire file with this field zeroed |

Build a freestanding flat x86-64 payload for entry at `0x05000020` (or use
position-independent code), then wrap it on the development host, for example:

```python
import struct
import zlib
from pathlib import Path

payload = Path("kernel-payload.bin").read_bytes()
size = 32 + len(payload)
image = bytearray(struct.pack("<Q6I", 0x314D524157384B42, 1, 32,
                              size, size, 32, 0) + payload)
struct.pack_into("<I", image, 28, zlib.crc32(image))
Path("kernel.warm").write_bytes(image)
```

CRC32 detects corruption, **not authenticity**. TFTP has no authentication or
encryption. Only boot images you trust on an isolated/trusted network; fetched
code runs with full kernel privileges. There is no automatic boot or fallback.
The existing BOOK8 ELF cannot simply be wrapped: a payload must implement the
warm ABI and be linked/position-independent for the stated address.

The warm entry has CPL0 long mode, low-4-GiB identity mappings, IF=0, DF=0,
a fresh 16 KiB stack (`RSP % 16 == 8` as on a SysV function entry),
`RDI=BOOK8_WARM_MAGIC`, and `RSI` pointing to `book8_warm_info` in the old
kernel's reserved memory. That structure contains six uint64 values: magic,
version, load address, file size, memory size and CR3. Interrupts/PIC are masked,
RTL8168 TX/RX is stopped, and PCI bus mastering is cleared/read back for every
discovered function before the jump. Failure to clear DMA halts rather than
resuming a partly stopped kernel. The payload must install its own GDT/IDT,
page tables and drivers before enabling interrupts or reclaiming old kernel
memory; controller registers and firmware state are not reset to cold-boot
defaults. Returning from the entry halts.

The build runs [tftp_host_test.c](./tests/tftp_host_test.c) under ASan/UBSan for
wire serialization, TID locking, lost RRQ/DATA retries, duplicates, final ACK
replay, exact-size/empty files, bounds/errors and executable rejection/CRC32.
The default QEMU checks also run
`python3 tests/qemu_netboot_jump_test.py`, an isolated diskless ISO exercising
the real warm-jump assembly at 80 MiB with register/stack/flag checks.
Logs are retained under `build/rtl8168-validation/`.
This validates the transfer engine with simulated UDP and the handoff in QEMU,
not a real RTL8168 TFTP download: QEMU still lacks this NIC model.

Storage lists discovered volumes and incrementally counts FAT free/used clusters,
with at most one FAT-sector read per input-pump poll. Used-space bars update when
sampling completes, and filesystem writes invalidate the samples. NTFS shows
capacity only; USB mass-storage devices are not enumerated by the current HID
USB stack. Administrative Tools shows a bounded boot memory-map snapshot,
actual heap statistics, cached NVIDIA BAR0/probe state and recent diagnostic
text, without arbitrary register writes. About shows CPU, RAM, heap usage,
uptime and the GUI version. File Explorer, Command Shell and Settings retain
the native sunken viewport, scrollbar and status-bar widgets.

#### Display mode confirmation

Backends may provide `set_display_mode(width, height)`. A successful switch
opens a desktop-modal confirmation: "Keep this display setting?" and
"Reverting in [XX] seconds." Keep Changes or Enter before the 30-second deadline
saves and commits the proposed configuration. Timeout, Revert or Escape restores
the previous actual dimensions and window geometry without saving the choice.
A save failure also rolls back. Restore failures remain modal and offer retry;
they are never reported as successful restoration. A working monotonic clock
and storage hook are required before previewing a mode. Clock edits must be
saved separately before a display preview.

The firmware-framebuffer backend changes the software logical canvas and its
back-buffer pitch, then nearest-neighbor scales each present into the boot-time
scanout. GRUB and the Multiboot2 header prefer `1024x768x32`; the display
preference is restored when BOOK8GUI starts. This is not physical monitor
modesetting and does not provide Vsync.

Control-panel validation under WSL:

```sh
python3 tests/qemu_control_panel_test.py \
  build/book8-cli.iso build/book8-data16.img
```

This test copies the data image to a temporary disposable disk, navigates all
nine pages, saves preferences, reopens the desktop and boots again to verify
restore. Logs and page screenshots are kept in `build/control-panel-validation`.
`build.sh` also runs host regressions for configuration validation, native
forms, incremental FAT accounting, and display confirmation with a fake
mode-change backend (Keep click/Enter, timeout, cancellation, save failure
and restore retry). QEMU does not validate physical NVIDIA modesetting.

Task Manager (Start > Programs > Task Manager, or `Ctrl+Alt+Esc` from anywhere
on the desktop) is single-instance and refreshes once per second. The
`[ Processes ]` tab lists the desktop and every window with Name, PID (window
id), CPU %, Memory and Disk columns; select a row with the mouse or Up/Down and
use the `End Task` button or `Delete` to close it (the desktop row, PID 0,
cannot be ended). The `[ Performance ]` tab is a text summary of CPU (brand
string, speed, logical processors, utilization), memory, disk (utilization,
read/write rates, drive model) and uptime (`HH:MM:SS`). Left/Right switch tabs.
The status bar shows the task count and used / total memory. The figures are
approximations: CPU is measured busy time attributed to each window, memory is
the app's state size, disk is what the app reports via `book8gui_note_disk`
(the NoteEditor), and only the boot CPU runs, so only "Core 0" is listed. The
kernel supplies the system-wide figures through the optional platform hook
`system_info` (CPUID brand/threads, a PIT-calibrated TSC for speed and uptime,
the Multiboot2 memory map for total memory, and sector counters in the disk
layer).

File Explorer lists up to 128 entries at `A:\`; FAT16/FAT32 directories can be
traversed with a double-click (400 ms tick-based interval) or Enter, and the
`[UP]` control returns to the parent. When installed, Code Editor is the default
for `.c`, `.h`, `.cfg`, `.txt` and `.bat` (case-insensitive). Other files, or
those extensions after Code Editor is uninstalled, launch NoteEditor.
FAT and supported NTFS nested files can be read; FAT saves support existing
subdirectories and are bounded to
64 KiB; failures are reported in the editor. Files containing NUL bytes can
be opened but cannot be saved by NoteEditor, preventing binary corruption.
Explorer's item context menu offers Open, Properties, Rename, Delete and Open
With. Open With lists NoteEditor plus Code Editor and Hex Viewer while their
ELFs are registered. Properties shows filesystem metadata, with FAT creation
times shown as volume-local and NTFS creation times in UTC; NTFS directory
allocation is reported as unavailable. Rename accepts an 8.3 destination or
an ASCII VFAT destination when all changed records fit in the source directory
sector; it otherwise rejects the rename. Rename and Delete support validated
ASCII VFAT long-name sources. Malformed or non-ASCII long-name
records, NTFS and read-only or system items are rejected. Delete preflights a
tree of up to 128 entries before removing it.
I/O errors during deletion can leave changes incomplete. Cut and Copy share a
desktop clipboard; Paste is available on an Explorer item or empty list area,
and the desktop background offers Paste to `A:\`. Paste currently supports files
up to 1 MiB with 8.3 or validated ASCII long names into FAT directories; FAT16
fixed roots must have enough free directory slots. It refuses collisions rather
than overwriting. Cut requires a writable FAT source. FAT directory trees of
at most 128 entries, including validated ASCII long-named items, can also be
copied or moved; a folder cannot be pasted into itself or a
descendant. File copies are read back before a cut removes the source. A failed
copy can leave a partial destination, and a failed cut can leave a destination
copy and an incompletely removed source. Other long-name rename layouts,
NTFS destinations and configurable file associations are not
implemented.
The Command Shell sends entered lines through the same command dispatcher as the
kernel shell. Keyboard events go to the focused application; wheel events go to
the window under the pointer.
Signed wheel magnitudes are preserved: positive notches move toward earlier
rows, negative notches toward later rows. Explorer moves one row per notch;
NoteEditor moves three visual lines per notch and clamps at the first/last
page. Wheel input over the taskbar or uncovered desktop is not forwarded to a
background window.
`python3 tests/qemu_explorer_context_test.py build/book8-cli.iso
build/book8-data16.img` checks Explorer's context Open and explicit desktop
exit with the data disk attached in snapshot mode; use a disposable image.

The GUI needs a 24- or 32-bit RGB framebuffer of at least 640x300 pixels. Its
input pump is scheduled independently of drawing (target 1000 Hz); dirty
windows are rendered on the 60 Hz frame schedule when the timer source supports
it. PS/2 mouse IRQ handling enqueues input rather than drawing from the handler.
USB mice are serviced by polling: UHCI supports boot protocol and the supported
non-boot relative X/Y report subset, while the polled OHCI driver supports the
tested boot-protocol mouse configuration. EHCI is used for companion routing;
xHCI and USB hubs are unsupported.
UHCI enumeration, configuration parsing, HID protocol setup, descriptor
parsing, and report polling support keyboards and mice. UHCI transfer waits are
bounded using the controller's 1 ms frame counter. The OHCI startup telemetry
reports controller/port/HID/report/error counts; `INPUTSTAT` reports USB input
counts and queue drops. EHCI legacy ownership and companion routing are
attempted when the controller topology requires them. This QEMU coverage does
not replace bare-metal verification of chipset-specific port wiring.
User programs can query framebuffer dimensions, pitch, BPP, and RGB channel
layout with `sys_fb_info`, then draw bounds-checked RGB pixels with
`sys_draw_pixel`; the kernel does not expose the physical framebuffer address
or a writable raw mapping. The framebuffer is accepted only when its RGB pixel
layout, size (at most 1 GiB), and address range (within the boot identity map's
first 4 GiB) are valid; otherwise the kernel keeps the VGA text fallback.
The kernel validates the bootloader-provided pitch against width and BPP using
wide arithmetic. BOOK8GUI uses a system-memory backbuffer when the complete
pitched surface fits its reserved buffer, then copies each visible scanline to
the boot framebuffer when a frame is presented. This avoids drawing each GUI
primitive directly to scanout, but the memory copy is not a hardware page flip
and cannot guarantee tear-free output.
BOOK8GUI has no separate process or loader of its own: it is a separately built
source module linked into the kernel and normally starts automatically after GPU
initialization. A command-shell boot (`book8.autostart=0`) can launch it with the
`BOOK8GUI` command. Standalone Ring 3 utilities are loaded by `EXEC`, but they do
not host the desktop.

#### Desktop shell validation

From WSL Ubuntu, run the focused headless UI preflight:

```sh
mkdir -p build
gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -o build/book8gui_host_test tests/book8gui_host_test.c \
  src/book8gui.c src/apps/book8_explorer.c src/apps/book8_shell.c \
  src/apps/book8_editor.c src/apps/book8_settings.c src/apps/book8_taskmgr.c \
  src/apps/book8_utilities.c src/apps/book8_netshare.c src/system_config.c \
  net/net.c net/tcp.c net/smb.c net/ntlm.c net/sha256.c
./build/book8gui_host_test
```

The supported build also runs `utilities_host_test` for hex formatting,
scrolling, editor keys, Save/Save As, and failure retention. To exercise actual
ELFs, Explorer associations/Open With, FAT writes and utility removal:

```sh
python3 tests/qemu_utilities_test.py build/book8-cli.iso build/book8-data16.img build/utilities-fat16-validation
python3 tests/qemu_utilities_test.py build/book8-cli.iso build/book8-data32.img build/utilities-fat32-validation
```

Each run writes only a temporary image copy and retains serial logs/screenshots.

The build runs sanitized TCP, NTLM hash-vector, SMB2-parser/signing and
share-browser host tests.
For interoperability, use an isolated Python environment with `impacket`:

```sh
python3 -m venv build/netshare-test-env
build/netshare-test-env/bin/python -m pip install impacket
build/netshare-test-env/bin/python tests/smb_integration_test.py build/smb_integration_host build/netshare-validation
python3 tests/qemu_netshare_test.py build/book8-cli.iso build/book8-data16.img
```

The interoperability test creates disposable loopback-only guest/read-only and
authenticated/writable shares and
passes its traffic through BOOK8's real Ethernet/TCP/SMB layers with a host
socket adapter, verifying signed NTLMv2 logins, nested enumeration, byte-exact
multi-read downloads/multi-write uploads, empty uploads, collision protection and
rejected passwords. Stock QEMU does not emulate BOOK8's RTL8168/8169 NIC: its
netshare test checks ELF/menu integration, local create/rename/properties/delete,
password-free profile persistence and cold-boot restore, invalid/offline alerts and uninstall,
not live SMB traffic. Use the generated CLI ISO for tests; the consolidated
delivery image is `build/book8.iso`.

The preflight covers live maximize/restore geometry at 800x600, 1024x768 and
640x300, taskbar exclusion, PS/2 ID 3/4 wheel decoding, pointer-based wheel
routing without keyboard-focus changes, and rendered NoteEditor scroll offsets
for multi-notch and extreme deltas. Display-preview keep/revert geometry is
also checked by the build's `control_panel_host_test`.

GUI and USB mouse events carry a dedicated signed `int16_t wheel_delta`:
one upward detent is `+120`, one downward detent is `-120`. PS/2 and HID
rotation is converted at the driver boundary; there is no depth coordinate.
The message pump's portable `WM_MOUSEWHEEL` decoder extracts the signed high
word of `wParam`, while `WM_MOUSEHWHEEL` is ignored without vertical scrolling.
BOOK8 is not a Win32 runtime; these are compatible wheel units/message decoding,
not a general Windows message API. Screen coordinates remain separate 32-bit
values, including for absolute HID pointers.

Each scrollable control accumulates partial deltas until a full 120-unit detent
is available, then applies its existing rows-per-detent policy with clamped
content offsets. For example, two `+60` messages equal one `+120` message.
Wheel events go to the window under the pointer without stealing keyboard
focus; that window routes them to the hovered list/scrollbar control. Thumb
dragging still uses pointer coordinates, separately from wheel rotation.

For the full desktop interaction test, first build the ISO and the disposable
FAT data image. Create the required scratch target only if it does not already
exist, then remove it on exit:

```sh
test ! -e /tmp/d.img || { echo "/tmp/d.img already exists; refusing to overwrite"; exit 1; }
trap 'rm -f -- /tmp/d.img' EXIT
cp build/book8-data16.img /tmp/d.img
python3 tests/qemu_desktop_test.py build/book8.iso /tmp/d.img
```

The test attaches that regular file as a raw IDE disk with QEMU snapshot mode,
and configures an explicit `pci-ohci` controller plus `usb-mouse`; it does not
use a physical disk or partition. QEMU's UHCI mouse descriptor enumeration does
not complete reliably, so this test uses the supported OHCI boot-HID path. The
graphical desktop autostarts with the modal `BOOK8 Sign In` window; the test
signs in as `default` and then launches apps from the Start menu. The sign-in
modal consumes cascade slot 0, so user windows are offset by one 24 px step and
Settings is paged from Network to Date/Time before the hour is changed.
Expected serial markers include:
`BOOK8-DESKTOP: TASKBAR height=36 reserved`,
`BOOK8-DESKTOP: WINDOW id=1 title="BOOK8 Sign In" registered`,
`BOOK8-SESSION: login accepted uid=0`,
`BOOK8-DESKTOP: WINDOW id=2 title="File Explorer" registered`,
`BOOK8-DESKTOP: WINDOW id=3 title="NoteEditor" registered`,
`BOOK8-DESKTOP: WINDOW id=4 title="Command Shell" registered`,
`WINDOW minimized/maximized/restored id=2`, `BOOK8-SETTINGS: time set`,
`BOOK8-DESKTOP: WINDOW id=6 title="Task Manager" registered`,
`BOOK8-DESKTOP: hotkey Task Manager id=6` (QEMU `sendkey ctrl-alt-esc`),
`WINDOW closed id=2` (Task Manager End Task), focus/input-routing messages, and
`BOOK8-EXPLORER: path=A:\DATA\`. The test requires positive USB mouse reports,
`USB_QUEUE_DROPS=0`, and prints `PASS: BOOK8 desktop test` on success. It also
checks the maximized frame's screen edges and compares NoteEditor viewport
screenshots before wheel-up and after an equal wheel-down event using unsaved
scratch text. Screenshots and serial logs from this test are temporary.
The graphics/input smoke test additionally requires both `WHEEL_DELTA=120` and
`WHEEL_DELTA=-120` from wheel-only reports on the PS/2 and UHCI USB paths.
Legacy `WHEEL=1`/`WHEEL=-1` diagnostic detent counts remain for older log readers.

For the Explorer context-menu path, run:

```sh
python3 tests/qemu_explorer_context_test.py build/book8-cli.iso build/book8-data16.img
```

It signs in, resolves File Explorer from the live program rows, right-clicks
`A:\NVMAP.CFG`, chooses Open, checks that NoteEditor opens the `.CFG` fallback
instead of the command shell, then exits the desktop. It uses a snapshot copy of
the disposable data image and prints
`PASS: Explorer context Open, editor fallback and explicit desktop exit`.

For Add/Remove Programs persistence and menu launch coverage, run:

```sh
python3 tests/qemu_programs_test.py build/book8-cli.iso build/book8-data16.img
python3 tests/qemu_programs_test.py build/book8-cli.iso build/book8-data32.img
python3 tests/qemu_programs_overflow_test.py build/book8-cli.iso build/book8-data16.img
python3 tests/qemu_programs_overflow_test.py build/book8-cli.iso build/book8-data32.img
```

Each run copies the source into its own temporary FAT disk, adds scratch ELF
utilities, cancels then confirms removal, verifies the removed file is absent
and the retained/core files are byte-identical, and reboots to launch the
remaining utility from Programs. Only the scratch copy is writable. Logs and
screenshots are retained in `build/programs-validation` (pass a third argument
to retain separate FAT16/FAT32 results). The host GUI preflight additionally
tests protected entries and a failed deletion retaining its shortcut.

To validate OHCI mouse input separately, run
`python3 tests/qemu_ohci_mouse_test.py build/book8.iso`. It requires the
SB710-like OHCI enumeration counters, positive USB reports and queued events,
zero queue drops, and `PASS: OHCI mouse test`. Stop at QEMU validation; do not
flash or install to physical hardware as part of these tests.

The filesystem module implements:

- Automatic mount and `MOUNT <disk>` for FAT16, FAT32, and the limited NTFS
  subset below. The mounted volume is exposed as `A:\`.
- `DIR`, `VOL`, `TYPE <name>`, `COPY <source> <destination>`,
  `WRITEFILE <name> <text>`, and `DEL <name>` operate on the mounted FAT
  volume's root directory; shell `MD`/`RD` remain deferred. FAT supports VFAT
  long-name reads and validated ASCII long-name creation. Quote names containing
  spaces. Subdirectory traversal, file creation/replacement/removal and directory
  creation/removal are implemented through the filesystem API and the desktop
  applications described above, with the bounds and failure semantics documented
  there. NTFS support remains read-mostly as described below.
- `FDISK <disk>` lists recognized MBR/GPT partitions.
  `FDISK <disk> CREATE MBR|GPT` replaces the table with one data partition.
- `FORMAT <disk> [partition-number] FAT16|FAT32` formats the whole disk when
  no partition number is supplied, otherwise the selected partition. FAT32
  formatting requires a suitably large volume.

`FORMAT` and partition-table replacement require typing `YES`. `WRITE
<disk-number> <LBA> <text>` replaces one sector, zero-fills the rest, and also
requires confirmation. Use these commands only with disposable images.

### NVIDIA Maxwell foundation (experimental, bare metal only)

#### PCIe/chipset prerequisite

`build.sh` compiles and links the current PCIe source and interrupt entry
assembly into the kernel. Before packaging it checks that the PCIe routines,
safe MSR reader and #GP entry are defined code symbols. It then extracts
`/boot/book8.elf` from the generated installer image and both ISOs and compares
each byte-for-byte with the linked kernel, failing the build on a mismatch.
Build on Windows with `.\build.ps1`; the target boot ISO is `build/book8.iso`.

The chipset foundation in [drivers/pcie.c](drivers/pcie.c) starts after core
CPU setup and ACPI discovery, before storage/GPU probing. It validates MCFG
and accesses PCIe ECAM configuration space (including extended offsets and
multiple segments) through uncached mappings within the kernel's existing
0..4 GiB identity map. Missing/unsupported MCFG uses CF8/CFC for conventional
segment-zero configuration space; above-4-GiB ECAM is explicitly unsupported.
Kernel, network, storage, UHCI and OHCI configuration accesses share this path.

The target-specific [drivers/amd_host_bridge.c](drivers/amd_host_bridge.c)
reuses validated MCFG ECAM for bus zero. It checks RS780 host `00:00.0`
(`1022:9600`), GFX root port `00:02.0` (`1022:9603`), and bridge routing to
bus 1. No CPU family or MSR check gates this path.

Before asserting root-port or target MSE/BME, the driver reads the RS780
port-indirect `LC_STATE0` register: PCI offsets `0xE0` (index) / `0xE4` (data),
index `0xA5`, `LC_CURRENT_STATE[5:0] == 0x10` (L0). It polls at most 50 times
with 10 ms delays. Index selection/read/restore runs with interrupts saved
and disabled; the original selector is restored and verified before returning.
The data register is never written. Missing ECAM, identity/routing mismatch,
all-ones reads, restore failures and training timeout block command writes.
The target-command helper rechecks L0 so config-only GPU initialization cannot
bypass the prerequisite. Existing firmware command bits are not forcibly
cleared, nor is the link reset/retrained.

Register references: AMD *780G Family BIOS Developer's Guide*, section 2.2
([port-indirect register access](https://studylib.net/doc/8087703/amd-780g-family-bios-developer-s-guide));
the index and L0 encoding are independently corroborated by
[coreboot 4.11 RS780 definitions](https://github.com/coreboot/coreboot/blob/4.11/src/southbridge/amd/rs780/rs780.h)
and its [link-training implementation](https://github.com/coreboot/coreboot/blob/4.11/src/southbridge/amd/rs780/cmn.c).
BOOK8 implements read-only status verification, not that firmware's hardware
reset or training-programming sequences.

Enumeration records functions, bridge primary/secondary/subordinate buses,
BAR encodings/addresses, and the PCI interrupt line/pin without probing BAR
sizes or changing firmware configuration. A driver match table keys on vendor,
device, and masked class code; successful `probe()` callbacks may request a
driver-sized BAR0 window mapped UC through the platform mapper. The NVIDIA
Maxwell entry matches `10DE:13C0`/`10DE:13C2` on segment zero only when firmware
has already enabled memory and bus-master decoding. The GTX 970's active
device-specific initialization remains restricted to the verified target path.
For GTX 970 `0000:01:00.0`
(`10DE:13C2`), the kernel validates the upstream bridge chain, requests standard
PCI PM D0 upstream-first, and verifies upstream memory-decode/bus-master bits.
RS780 LTSSM L0 must be observed before those command bits are asserted.
The target endpoint's command bits 1/2 are also set and read back before BAR
configuration reads. Failed/all-ones readback blocks BAR access. The GPU
mapping helper separately verifies both command bits before mapping/dereference.
Changed PMCSR states are polled after 10 ms delays, at most 50 times; PME status
is not acknowledged. This is **not** evidence that chipset lane gates are open.

BOOK8 does not read or write the unverified `0xC0012121` MSR and does not
perform a platform PMC handshake. Once the checked link and PCI prerequisites
pass, it probes and sizes the target's BARs and maps its registers; optional
GPU channel binding remains separately gated by `BOOK8_NV_BIND_CHANNEL`.
Link L0 and D0 do not prove lane-domain power or guarantee that MMIO completes;
the target has not been validated on physical hardware. Unassigned BARs remain
blocked: BOOK8 has no
ACPI `_CRS` resource-window allocator and must not guess free address space
or overwrite firmware bridge windows.
**Recovering from #GP does not clear a hardware lane gate.** The guarded
configuration access does not recover a stalled PCIe transaction.

`PCIE1` events appear immediately on serial and are buffered in order for
existing v2 UDP/8080 telemetry once the NIC and DHCP/saved network config are
ready. Boot events drain at up to 100 packets/second through normal network
servicing, then metrics return to the existing once-per-second cadence.
Failed sends retain the current event and retry at 1 Hz; queue overflow is
explicitly reported.
Early events are replayed, **not transmitted before the NIC is initialized**.
Immediately after RTL8168 initialization, DHCP begins (unless `book8.net=off`)
and the pulse containing target PCI ID/command is queued.
No boot-time wait for DHCP or ARP is introduced; UDP submission begins once
a lease or explicit static configuration provides a collector route.
Saved configuration takes precedence after filesystem initialization.
No supported NIC produces an explicit skip. UDP success is not a collector ACK.
With dynamic addresses, omit the listener's `--source` filter, or update it to
the assigned address. An unreachable collector is reported without discarding
the DHCP lease.
The packet remains 292 bytes with the event in its existing 128-byte log field.
QEMU has neither a Maxwell GPU nor a supported RTL8168 wire device; host tests
cover register sequencing, while physical PMC transitions/UDP delivery remain
unverified.

`MSRGUARDTEST` performs a safe EFER read, probes an invalid MSR index, and
verifies #GP recovery, unchanged failed-read output and interrupt-enable state.
It writes no MSRs. Use it under QEMU for the exception-path regression.
Under WSL Ubuntu, run
`python3 tests/qemu_msr_guard_test.py build/book8-cli.iso`; it attaches no disks,
checks the boot skip on AMD Family 15h and Intel and verifies the shell still executes
commands after recovery. Logs remain in `build/msr-guard-amd15h.log` and
`build/msr-guard-intel.log`. The reserved x2APIC register-zero MSR (`0x800`)
provides the deterministic fault probe: QEMU TCG can return zero for arbitrary
unknown MSRs, including `0xC0012121`, which is not hardware support evidence.

#### Manual RS780 single-index probes

The packaged `drivers/rs780_manual_audit.h` / `.c` API additionally supports
strict **per-transaction** handshakes, unlike `PCIEPROBE`'s grouped supporting
selector/discovery reads. It is a standalone operator-driven C API, not an
additional shell command and not invoked automatically during boot.
Zero-initialize a context, call `rs780_audit_init`, then explicitly request
each selector/discovery operation with `rs780_audit_request_next`.
Each request publishes INTENT without accessing hardware; after operator
verification of the current target/session collector record, call
`rs780_audit_confirm_collector` and `rs780_audit_ack_intent` for that ID.
Exactly one guarded DWORD access occurs. Confirm the RESULT separately, then
call `rs780_audit_ack_result`. No handler issues the next request or an ACK.
LINKCAP and LINKSTATUS each require their dedicated request function and
full independent acknowledgement cycles.
After IDs 1 through 6 have completed, `rs780_audit_finish` optionally ends
only the selector/LTSSM audit and releases ownership. It rejects premature,
failed or unacknowledged restoration. Full capability/status preflight remains
incomplete. Alternatively continue the existing discovery and isolated link
requests without calling finish. All API symbols are checked during packaging.

The sequence saves/selects/checks A5, reads LTSSM, restores/checks the original
selector, and discovers the standard PCIe capability using individually
acknowledged status/pointer/header reads. Pointer alignment, bounds, cycles,
Root Port type and v1/v2 metadata are checked. `(data & 0x3F) == 0x10` is required
to acknowledge the LTSSM result; failed access or semantic checks stop advancement.
Only E0 selector writes exist. E4, GPU MMIO and PMC writes remain prohibited.
The API claims the shared `rs780_access_blocked` interlock across waits without
holding IF disabled; all other indexed accesses must honor it. Raw PCI writes,
SMM and SMP are not protected by this BSP-local ownership convention.

Retry resubmits records only. Explicit abort after confirmed failure allows
only manually requested selector restoration and readback, each with both ACKs.
Recovery failure retains the interlock and invalidates host/link verification.
A failed audit cannot become successful through recovery. Preflight describes
separate snapshots, never permanent L0 or GPU authorization. No timers or loops
appear in the driver function bodies. `tests/rs780_manual_audit_test.c` tests
all 12 access-fault positions, send failure, stale ACKs, malformed capability
metadata, recovery lockout and exact single-access behavior during the build.

`PCIEPROBE` is opt-in shell diagnostics, not a boot-time address sweep.
It permits one 32-bit indirect read at `0000:00:02.0`, index `0xA2` or `0xA5`
through `0xAF`, using the existing validated ECAM mapping of `0xE0/0xE4`.
These are PCI configuration offsets, **not** CPU I/O ports.
`A2` is the documented link-width register and `A5` supplies LTSSM status;
adjacency does **not** establish that `A6..AF` are power-gating controls.
Existing host/CPU verification must have passed. A prior L0 result is no
longer required for diagnostic reads, so a non-L0 link can be inspected.
The boot command-enforcement path still requires L0 before MSE/BME assertions.
No GPU MMIO authorization, MSR writes, or lane-control writes are added.

For example, in the target shell:

```text
PCIEPROBE A5
ACK 1 INTENT
ACK 1 RESULT
PCIEPROBE A6
ACK 2 INTENT
ACK 2 RESULT
```

Named requests use the same per-request handshake:

```text
PCIEPROBE LTSSM
ACK 3 INTENT
ACK 3 RESULT
PCIEPROBE LINKCAP
ACK 4 INTENT
ACK 4 RESULT
PCIEPROBE LINKSTATUS
ACK 5 INTENT
ACK 5 RESULT
PCIEPROBE STATUS
```

Use the actual reported IDs; the example assumes two earlier requests.
`LTSSM` aliases index A5; `LINKWIDTH` aliases A2 (current link-width encoding
in bits 6:4, not a direct lane count).
`LINKCAP` and `LINKSTATUS` are **standard PCIe capability** reads at the same
root port, not indirect-register aliases. The driver checks the capability-list
status, follows aligned conventional pointers with cycle detection, verifies a
v1/v2 Root Port PCIe capability, and reads exactly one requested DWORD:
Link Capabilities at capability base + `0x0C`, or Link Control/Status at
base + `0x10`. Supporting discovery reads are part of that one request,
not separately acknowledged operations. No standard capability registers are
written. Missing capabilities report errno 19; malformed lists report errno 5.
The RESULT includes `target`, the actual configuration `off`, raw `data`,
`valid`, `ex`, and `errno`; selector-restoration fields apply only to indexed reads.

After RESULT acknowledgement, STATUS reports decoded maximum/negotiated speed
codes and lane counts, training, and DLL active/reporting bits.
`link-preflight=1` requires an acknowledged A5 L0 sample and acknowledged,
successful LINKCAP/LINKSTATUS samples with nonzero negotiated speed/width
within the reported maxima, training inactive, and DLL active when reporting
is supported. Starting a replacement request clears that sample's acceptance.
Pending requests and restoration lockouts fail preflight. This is a summary
of separate snapshots, **not** a fresh atomic link measurement or permission
to run GPU probing. The audit is independent of normal boot initialization;
there is no automatic video-driver continuation after ACK.

**Before each ACK, verify the matching `PROBE1 id=... INTENT` or `RESULT`
record in the collector's decoded capture.** The local ACK is the operator's
attestation of receipt, not a remote UDP acknowledgement. Include the phase:
duplicate INTENT ACKs cannot consume a RESULT acknowledgement. IDs increase
within this boot; never replay commands from an earlier boot.

The INTENT record is submitted immediately to `192.168.50.17:8080` before any
probe access. The INTENT ACK performs exactly one bounded indexed-register
transaction: save selector, select the requested index, verify selection,
read data, restore the exact original selector, and verify restoration.
These supporting `E0` operations are part of that one indirect read, not
separately acknowledged probes. IF is restored before immediate RESULT
submission. Restoration precedes UDP transmission to avoid holding a changed
selector across networking or an acknowledgement wait. The data register `E4`
is **never** written, even with mask zero: echoing read status could clear W1C
bits. An optional nonzero hex mask is logged as a denied request (`errno=1`);
it cannot authorize a write. Future assertions require a documented contract
and a separate implementation, not just another ACK.

`PCIEPROBE STATUS` displays pending phase/submission state.
`PCIEPROBE RETRY` retransmits only the pending record, never the access.
No automatic timeout, heartbeat advance, address loop, or write retry exists.
Failed submissions retain the record and block ACK until an explicit retry
succeeds. Normal input/network polling continues while awaiting the operator.
Disabled networking or a different collector blocks submission and access;
restore the network configuration and retry the record.

RESULT carries `valid`, `data`, CPU exception vector (`ex=13` or `14`),
`errno` (`19` unavailable, `5` access/verification failure, `1` denied write),
and independent restoration error/exception fields. A restoration failure
poisons further probes until reboot, even after RESULT acknowledgement.
The restoration lockout is shared with boot LTSSM access and cannot be cleared
by re-running host identification. Both paths reuse `drivers/rs780.c` and its
exception-aware read/write callbacks; no unguarded indexed fallback is allowed.
Only exact CPL0 load/store instructions have #GP/#PF fixups; unrelated faults
remain fatal. A bus hang, machine check, posted-write completion failure, or
register side effect cannot be made safe by those fixups. An INTENT without
a RESULT identifies the selected indirect register, not which supporting bus
transaction stalled. Physical access and collector delivery are not QEMU-verified.

`MSRGUARDTEST` also checks guarded RAM reads/writes, noncanonical-address #GP
and unmapped-address #PF recovery for both directions, failed-read output and
IF preservation. It prints `MEMGUARDTEST: PASS` for the memory checks and does
not access a physical device during fault injection.

#### Existing GPU foundation

##### Separate NVIDIA/PCIe diagnostic boot image

Build with `.\build.ps1 -Diagnostic` (WSL: `BOOK8_DIAGNOSTIC=1 bash build.sh`).
All diagnostic objects, kernels and disk images go to `build/nv-diagnostic/`;
the output is `build/nv-diagnostic/book8-nv-diagnostic.iso`. This does **not**
replace `build/book8.iso` or the normal kernel/application images.
When QEMU is available, this profile runs the focused boot-log check rather
than desktop-autostart tests, since its GRUB entry intentionally boots to the
command shell.
The diagnostic GRUB entry boots to the command shell and enables kernel
initialization capture; it does not enable experimental GPU channel binding,
change the designated GTX 970 target, or flash GPU firmware.

To opt in an **existing writable FAT BOOK8 boot volume**, copy the supplied
`build/nv-diagnostic/NVLOG.OK` file unchanged into that volume's root directory.
Its contents must be exactly `BOOK8-NVLOG-1` followed by CRLF. The same volume
must contain `BOOT\BOOK8.ELF`. Only one physical FAT volume may have this marker.
Remove it to disable diagnostic writes. The diagnostic ISO includes the marker
for convenience but never writes it onto any attached disk automatically.
Boot the separate diagnostic ISO; the kernel writes `nvinfo.txt` in the opted-in
volume's root, overwriting the previous diagnostic log. It does not assume A:
is writable or select unrelated disks; the ISO's in-memory application volume
is excluded. NTFS and ISO9660 destinations are not supported.

BOOK8 has one kernel console/serial stream, rather than boot-time process
stdout/stderr. Capture starts at kernel entry, including early `PCIE1` events,
console errors and `BOOK8-NV:` messages when a supported GPU is found. Before
filesystem initialization, output is buffered in 64 KiB of RAM. Once the marked
volume is mounted, each completed line is checkpointed through the existing FAT
writer and read back for verification. Begin/end markers surround PCIe target
preparation and NVIDIA probing, so the last complete line shows where progress
stopped. A full boot ends with `END initialization; entering shell`; capture then
stops so shell input and later credentials are not logged.

**Limit:** an ISO/CD cannot store a persistent log, and GRUB cannot redirect
kernel output into a file. A hard hang before the filesystem is usable cannot
flush the RAM buffer; capture COM1 externally for that case. Missing/invalid
markers, multiple destinations, I/O/verification failures and truncation are
reported explicitly as `BOOK8-NVLOG: ERROR`. Do not interpret an old log as proof
of a new boot when such an error occurs. No physical drive preparation or
installation is performed by the build/test tools.

Focused validation (WSL, disposable image only):

```sh
python3 tests/qemu_boot_log_test.py \
  build/nv-diagnostic/book8-nv-diagnostic.iso \
  build/nv-diagnostic/book8-data16.img
```

The test checks byte-exact serial/file capture on a marked FAT copy and verifies
an unmarked copy is unchanged using a whole-image SHA-256. Logs are retained in
`build/nv-diagnostic/boot-log-validation/`. QEMU lacks a Maxwell GPU; it validates
capture/persistence and the probe boundary markers, not physical NVIDIA access.

After the target prerequisites above, the matched NVIDIA probe uses the
enumerated PCI function rather than a hardcoded scan and `drivers/nv_maxwell.c`
probes the designated GTX 970 at `01:00.0`; the same table can match a
GTX 980 (`10DE:13C0`) when firmware has enabled memory and bus mastering. The NVIDIA
driver sizes BARs only during its later device-specific probe and maps
BAR0 (registers, mapped uncached through the
boot page tables) and BAR1 (VRAM aperture), and verifies `PMC_BOOT_0` reports
chipset GM204 (0x124). It builds a GPFIFO ring plus push-buffer in system
memory, encodes Maxwell method packets (`inc`, `non-inc`, `imm`, `inc-once`
with subchannel/method fields) and emits NV902D 2D streams for rectangle fills
and screen copies (`nv_2d_fill_rect`, `nv_2d_copy_rect`). `gui_fill_rect` tries
the GPU first via `nv_accel_ready()`. The serial log shows `BOOK8-NV:` lines.

Channel binding (`nv_channel_bind`) builds, in VRAM through the PRAMIN window,
an instance block (RAMFC with USERD and GPFIFO pointers, page directory and
tables mapping the push-buffer and GPFIFO), a BAR1 identity-map VM, and a
runlist; it enables PFIFO, binds the channel, starts it, and rings the USERD
doorbell with an empty packet. `nv_accel_ready()` becomes true only when
PFIFO advances `GP_GET`. It is opt-in: build with `-DBOOK8_NV_BIND_CHANNEL=1`.
Hardening: `nv_accel_ready()` requires `ring_acked`, set only after a PBDMA
NOP (method 0x100) is submitted and `GP_GET` catches `GP_PUT`. Any failed
stage (PFIFO enable, BAR1/scan-out PTE verify, RAMFC readback, BAR1 identity
probe, runlist, ring ack) unwinds in reverse: channel stopped, runlist emptied,
BAR1 bind/PRAMIN window/PMC bit restored, VRAM structures zeroed,
`nv_gpu.bind_fail` names the stage. `BOOK8-NV:   LABEL = 0x...` serial lines
dump PMC_ENABLE, BAR1_BIND, channel regs, runlist pending, PFIFO/PBDMA0
interrupts, engine status and GP_GET/GP_PUT/delta. The scan-out surface must
sit below the 256 MiB BAR1 identity map and every one of its PTEs is verified
before BAR1 is rebound; the display engine scans out by VRAM address, so the
rebind cannot blank the screen.

Register layouts follow nouveau, but PRAMIN/BAR1-bind/PFIFO-enable details,
and the PBDMA/engine diagnostic registers, are unverified on hardware, and
replacing the BAR1 mapping can still disturb CPU framebuffer access if the
firmware used a non-identity map (the old bind value is restored on failure). GM204 graphics (the 2D class) also needs signed FECS/GPCCS
firmware, which is not available, so NV902D methods will not execute yet.
Display-controller modesetting is not implemented. Without the opt-in the
CPU draws the boot framebuffer. QEMU has no Maxwell device; coverage is
`tests/nv_maxwell_host_test.c` (run by `build.sh`) using a faked GPU model.

### Standalone NVIDIA mapping diagnostics

The independently linked Ring 3 utility [user/nvmap.c](user/nvmap.c) uses a
dedicated privilege bridge, not a module loader or dynamic linker. Build only
the utility under WSL Ubuntu with `bash user/build-nvmap.sh`; this produces
`build/user/nvmap.elf` without rebuilding the kernel. A one-time kernel rebuild
with `.\build.ps1` is necessary to install the new syscall bridge. Thereafter,
updating the utility does not change the kernel image.

The shared ABI is [include/hwdiag.h](include/hwdiag.h), using
`sys_hwdiag(operation, buffer, exact_size)` (syscall 63):

| Operation | Buffer | Behavior |
|---|---|---|
| `BOOK8_HWDIAG_INFO` | `book8_hwdiag_info` (48 bytes) | Cached GTX 970/980 PCI identity, BDF, BAR addresses/sizes and boot register; flags bit 0 present, bit 1 mapped. Absent hardware returns a zeroed snapshot. |
| `BOOK8_HWDIAG_READ32` | `book8_hwdiag_read32` (8 bytes) | Read only BAR0 `PMC_BOOT_0` (offset 0) or `PMC_ENABLE` (0x200), through the existing driver mapping. |
| `BOOK8_HWDIAG_REPORT` | `book8_hwdiag_report` (8 bytes) | Disabled; returns `ENOSYS` and never submits a snapshot. |

[kernel/hwdiag.c](kernel/hwdiag.c) owns the allowlist and PCI power guard.
There is no arbitrary physical mapping, user-accessible MMIO page, register
write, PCI configuration write, BAR resize, channel binding, or power-state
change. Register reads require the original PCI identity, enabled memory
decoding, a valid bounded PCI capability chain and D0 when a PCI PM capability
exists. Unmapped/absent hardware returns `ENODEV`; non-D0 or disabled memory
decoding returns `EAGAIN` without touching MMIO; malformed capabilities and
all-ones register responses return `EIO`. Other offsets return `EPERM`.
The syscall validates operation, exact buffer size and user page permissions,
and handles unaligned buffers without exposing kernel pointers.

This is **power-aware read-only diagnostics**, not an internal NVIDIA
power-gating controller or detector. Neither PCI D0 nor PMC_ENABLE proves
that a particular engine is powered. The listener explicitly reports internal
power gating as unknown. PCI checks cannot protect against surprise device
removal or guarantee that a physical MMIO transaction completes; this is not
a hotplug-safe general-purpose hardware API. Kernel GPU initialization is
outside this helper's no-write guarantee. For a target whose link or mapping
prerequisites fail, INFO can report present but not mapped; BAR sizes and
boot registers are then not measured.

The generated FAT data and installer images contain `NVMAP.ELF` for explicit
manual execution (`EXEC A:\NVMAP.ELF`). Boot does not run NVMAP and ignores
the legacy `NVMAP.CFG` even if it is enabled. The utility reports absent or
config-only devices locally, reads the allowed register only when mapped,
and prints read failures; it never submits a snapshot. The report syscall
returns `ENOSYS` even for older utilities. Existing ordinary UDP 8080 metrics
and operator-initiated probe telemetry are independent and remain available;
Telemetry is gated off throughout initialization. After initialization, the
CLI/desktop service pump makes one send attempt per telemetry interval; ARP
pending and full TX rings defer delivery without a completion/retry wait loop.
Queued PCIe events remain queued when an attempt is not accepted.

The default boot automatically launches BOOK8GUI after GPU initialization.
Choose the ISO's command-shell GRUB entry (`book8.autostart=0`) to suppress
desktop startup, or use Start > Shutdown > Exit to Prompt. NVMAP and the push
agent remain disabled at boot. QEMU tests check disabled NVMAP startup across legacy
configurations, explicit NVMAP execution, and graphics/input behavior. QEMU
does not model the target Maxwell GPU or RTL8168.

The network host regression also sends telemetry through the real RTL8168
transport with simulated stalled DMA, a full TX ring, and an absent ARP peer.
It asserts zero completion-poll callbacks and successful retry after a
descriptor becomes available.

### NTFS limitations

NTFS support is experimental and deliberately limited: BOOK8 can mount a
recognized NTFS volume and list/read files in the root directory's resident
`$I30` index and allocated index blocks. It reads resident and non-resident,
uncompressed, unencrypted data streams up to 1 MiB. It can overwrite an existing
non-resident root file only when the replacement has exactly the same size;
this requires confirmation. NTFS formatting, file creation/deletion/resizing,
subdirectories, compressed/encrypted/sparse streams, and journaling are not
implemented.

NTFS writes bypass its journal. Only test on disposable, unmounted images;
after an BOOK8 write, verify the image and run the host OS's filesystem checks
before using it elsewhere. BOOK8 does not provide production-safe NTFS write
support.

Utilities such as `ATTRIB`, `CHKDSK`, `DEFRAG`, `EDIT`, `MOVE`, `SCANDISK`,
`SYS`, and `XCOPY`, as well as running external programs, are not implemented.
They report that limitation instead of claiming success.

Full 3x3 emulator matrix (QEMU/Bochs/VirtualBox x SB710/G41/stock), under WSL: `python3 tests/emulation_matrix.py build/book8.iso`. Results and caveats: KERNEL_NOTES.md section 32.

## Freestanding libc

`include/` and `lib/` hold a small freestanding libc (string, malloc, stdlib, stdio). `build.sh` runs its self-test as a host program on every build, the `LIBTEST` shell command runs it inside the kernel, and `python3 tests/qemu_libtest.py build/book8.iso` automates that under QEMU. Details: `KERNEL_NOTES.md` section 33.

Ring 3 smoke test: the GDT contains a 64-bit TSS descriptor loaded with `ltr`;
RSP0 follows the active task's kernel stack, with a bootstrap stack retained
when no user task is active. The `RING3TEST` shell command maps the user pages
(U/S=1; kernel pages stay supervisor-only), enters CPL 3 via IRETQ, and runs
`src/user_smoke.S` (SYSCALL register preservation, invalid-pointer rejection,
`__syscall` getpid/write/exit). The scheduler regression also checks that
Ring 3 timer frames land on the task's kernel stack. `build.sh` runs
`tests/qemu_ring3_test.py` after the ISO is built (`BOOK8_SKIP_RING3_SMOKE=1`
skips, `BOOK8_RING3_VBOX=1` adds VirtualBox); `tests/emulation_matrix.py`
reports a Ring 3 column for QEMU and VirtualBox.

Storage, FAT and ELF exec: `drivers/block.c` wraps the IDE/AHCI sector routines as `block_device_t` with a 64-sector write-through cache; `fs/fs.c` is a read-only FAT16/32 reader (8.3 names, subdirectories, bare or MBR volumes) exposing `fs_mount/fs_open/fs_read/fs_seek/fs_close`; `kernel/exec.c` loads ELF64 executables (linked in the 0x8000000000 user window, see `user/user.ld`) into U/S=1 pages with a 4 KiB user stack and a TCB, and backs the `open/read/close/execve` syscalls. The shell command `EXEC <file> [args]` runs a program from disk in Ring 3; it accepts up to 16 whitespace-separated arguments and rejects overflow. Installed package entries under `A:\APPS\<name>\` require a matching root `manifest.ini`; listed `.b8s` libraries load in manifest order from the package's `lib` directory before falling back to `A:\LIB\`. The loader resolves versioned imports, rejects dependency cycles, and applies allocated `SHT_RELA` records linked to ELF symbol/string tables. Supported x86-64 relocations are `R_X86_64_RELATIVE`, `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`, `R_X86_64_GLOB_DAT`, and `R_X86_64_JUMP_SLOT`; undefined symbols must also appear in the versioned `.b8s_imports` table and resolve to one compatible `.b8s` export. Unsupported relocation encodings are rejected with `ENOTSUP`. `execve` stages the replacement image and its libraries in a spare address-space slot before committing, preserving the old image on validation or loading failures. `build.sh` builds `hello.elf`/`cat.elf` and the FAT images `build/book8-data16.img` and `build/book8-data32.img`, and runs `tests/qemu_exec_test.py <iso> <img>` against both; the VirtualBox matrix attaches the FAT16 image and checks `exec hello.elf`. Limitations: read-only, no LFN/FAT12, one process at a time, no Ring 3 fault handlers.

Kernel threads use a general IRQ0 round-robin scheduler with READY, RUNNING,
BLOCKED, and DEAD states, fixed reusable TCB/stack slots, and `sched_spawn`,
`sched_yield`, `sched_sleep_ms`, and `sched_exit`. The `THREADS` shell command
and Task Manager's Processes tab show thread names, IDs, states, and timer
ticks. Kernel threads share the kernel address space; they do not add a
per-thread user address space. The shell's single active ELF execution still
uses the existing process TCB and user-page-table model. `SCHEDTEST` and
`SCHEDUSER` continue exercising their isolated kernel/Ring 3 frame path and
retain the existing two-tick quantum. The PIT uses its 65536-count period;
GUI timing continues polling the PIT. IRQ0 saves all GPRs and restores the
interrupt frame through `iretq`, and writes CR3 only when the selected context
uses a different root. Heap, filesystem, and RTL8168 shared-state entry points
preserve the incoming interrupt state while mutating shared buffers.
Each ELF execution now has its own PML4 and a fixed 512-page pool, including
page tables and user frames. The existing low identity-mapped kernel is shared
as supervisor-only; existing upper-half entries are mirrored, but BOOK8 does
not currently create a higher-half kernel mapping. The legacy Ring 3 smoke
pages remain in the boot address space and are not accessible to ELF tasks.
IRQ0 switches CR3 only when its target differs. `VMTEST` checks two distinct
roots mapping the same virtual address to different frames. These are isolated
address-space primitives, not concurrent ELF process support.

Swap is not implemented or provisioned. The boot memory map counts usable
memory rather than reliably reporting installed RAM, and there is no configured,
reserved swap backing store or demand-paging/eviction path. No disk is modified
for swap; an exact installed-RAM-sized, fixed preallocated page file requires
those prerequisites before it can safely be enabled.
`INPUTSTAT` includes an IRQ0 counter. Run
`python3 tests/qemu_scheduler_test.py build/book8-cli.iso` to check repeated
address-space checks, kernel-thread and Ring 3 interrupt-frame switches, register and direction-flag
preservation, Ring 3 execution afterwards, and GUI timer/PS/2 mouse interrupts. The build's QEMU
regressions also run this test unless `BOOK8_SKIP_RING3_SMOKE=1`.
The build additionally runs `tests/scheduler_host_test.c` to check the legacy
two-tick path, generalized thread creation/state reporting, saved-frame
rotation, CR3 changes, re-entry guards, and test cleanup.
