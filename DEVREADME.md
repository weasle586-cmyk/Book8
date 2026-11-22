# BOOK8 Developer README

This document is a development companion to the project README. It captures the implementation state, the progression of the kernel work so far, and the engineering notes needed to keep building more applications on top of the BOOK8 kernel without losing track of the current architecture.

`README.md` is canonical for current build, boot and usage behavior; dated sections below are historical implementation notes. Where this document disagrees with the README, follow the README.

## Scope and project status

BOOK8 is an experimental x86-64 freestanding kernel and command environment. It is not a conventional userspace OS yet. The system currently boots from a Multiboot2-compatible ELF, enters 64-bit long mode in `boot.S`, sets up a minimal page-table layout, and hands off to `kernel_main()` in `src/kernel.c`.

The current kernel already includes:

- bare-metal x86-64 startup and long-mode transition
- VGA and serial console output
- PS/2 keyboard and mouse support
- ATA IDE PIO storage discovery and read/write support
- AHCI SATA discovery and sector I/O
- FAT16/FAT32 filesystem mounting, read/write, and limited NTFS support
- installer image creation and disk installation flow
- USB UHCI keyboard/mouse support plus a polled OHCI boot-HID path
- framebuffer multi-window desktop shell (`BOOK8GUI`) with Explorer and Command Shell apps
- shell command dispatch and built-in command set
- RTL8168/RTL8111 polled TX/RX with a zero-allocation kernel ARP/IPv4/ICMP/UDP
  stack and non-blocking DHCP (DORA, renew/rebind/expiry), default dynamic IPv4,
  `A:\BOOK8DHCP.CFG` DHCP/static settings via `NETSAVE`, legacy static
  `A:\BOOK8NET.CFG` fallback values, runtime `NETCONFIG DHCP/RENEW`, and Network settings UI
  (old static IP only after 30s DHCP timeout; later ACK replaces IP/mask/gateway)
- `TELEMETRY` UDP/8080 metrics (v2: 292-byte packed big-endian packet with build,
  health, PHY, ring and poll diagnostics), streaming to the collector after DHCP by default
  with bounded once-per-second CLI/desktop servicing (see README wire format)
- bounded zero-allocation TFTP client and CLI `NETBOOT` for explicitly marked
  64-bit warm-entry images (not ordinary ELF/Multiboot; see README ABI)
- UDP 8081 push agent disabled; local `RELAUNCH` remains available
- NV register diagnostics are read-only and available in the NV Diagnostic GUI; boot automatically launches BOOK8GUI quietly
- QEMU smoke tests covering BIOS/UEFI boot and installer behavior
- a single-active-task Ring 3 ELF loader (`EXEC`) with versioned `.b8s` imports, plus kernel threads and experimental SMP (APs run kernel threads only)
- a freestanding libc subset with host and in-kernel self-tests (`LIBTEST`)

This is a kernel-first, single-address-space system. It is intentionally a lab OS and test environment rather than a polished production kernel.

## Project file review

The project is small but complete enough to understand as a single kernel build.

### Root project files

- `README.md`: public build and usage guide, plus operational notes for install and testing. This is the user-facing documentation.
- `KERNEL_NOTES.md`: maintained notes on kernel architecture, current feature boundaries, USB/ACPI behavior, and hardware validation status.
- `build.sh`: Linux/WSL build script. It checks for required tools, compiles the kernel objects, links them, and creates the ISO image.
- `build.ps1`: Windows entry point that validates WSL Ubuntu and invokes `build.sh` inside WSL.
- `build-installer-image.sh`: root helper that creates the FAT32 installer payload for BIOS + UEFI boot.
- `linker.ld`: kernel linker script linking the boot object and the kernel objects into a single ELF.
- `grub/grub.cfg`: GRUB menu entries for normal BOOK8 boot and the installer flow.
- `.gitignore`: ignores build artifacts and temporary working files.
- `tests/qemu_installer_smoke.py`: end-to-end QEMU validation script for shell, USB input, GUI, cancellation, install path, and cold boot checks.
- `tests/qemu_desktop_test.py`: disposable-disk QEMU regression for desktop registration, taskbar focus, input routing, Explorer navigation, and USB mouse counters.
- `tests/qemu_ohci_mouse_test.py`: SB710-like QEMU profile regression for OHCI HID mouse enumeration and input; pass a disposable FAT data image so the kernel can load its trusted USB module.
- `tests/book8gui_host_test.c`: headless desktop behavior test run during the build.

### Source tree

- `src/boot.S`: Multiboot2 header, page-table setup, 32-bit to 64-bit transition, and handoff to `kernel_main()`.
- `src/kernel.c`: the main kernel implementation: console, storage discovery, IDE/AHCI drivers, USB input, shell, GUI bridge, installer logic, and command dispatcher.
- `drivers/ata_dma.h` and `drivers/ata_dma.c`: PCI IDE bus-master DMA (READ/WRITE DMA and DMA EXT, LBA28/LBA48) with a 64 KiB-aligned bounce buffer shared with AHCI multi-sector transfers. `kernel.c` chunks block-layer requests (up to 128 sectors), retries once, then falls back to single-sector PIO. At boot each DMA/AHCI disk prints `STORAGE: <disk> ... selftest PASS|FAIL`, checked by `tests/qemu_storage_test.py`; `tests/ata_dma_host_test.c` unit-tests the register encoding. The same QEMU test also attaches IDE and AHCI scratch disks (sector 0 = `BOOK8SCRATCH`; the kernel's destructive write self-test runs only on such disks), checks DMA write/flush/read-back, injected bus-master/timeout/device faults (`ata_inject_fault`), and a 200-sector chunked write, then verifies the images from the host including guard sectors.
- `drivers/hda.h`, `drivers/hda.c` and `drivers/hda_stream.c`: ATI SB7xx HDA at PCI `00:14.2`, using firmware BARs, bus-master enable and the SB7xx snoop bit at PCI config `0x42` while preserving `0x44`/`0x4c`. It initializes CORB/RIRB, discovers the VIA codec's widget connections, routes 48 kHz stereo S16 audio to rear green and front headphone pins, and retains the route amplifier controls. The desktop taskbar adds a classic gray vertical volume popup and mute control between network indicators and the clock. `BOOK8 Player` now has a VLC-style dark interface with a menu bar (Media, Playback, Audio, Video, Subtitle, Tools, View, Help), transport toolbar, seek bar, volume slider, playlist, bookmarks, Jump to Time, and a Tools/Preferences dialog. It reads local WAV files and supports PCM 48 kHz stereo 16-bit playback, pause/resume, seek, stop, repeat, and HDA volume/mute control. Browser audio media can use the same path for bounded HTTP WAV responses. Stream refill is polled on desktop frames; IRQ delivery remains masked because BOOK8 does not yet route this controller's IRQ. WAVs are capped at 2 MiB locally and browser responses at 512 KiB. MP3/other codecs, HTTPS/TLS, JavaScript, adaptive streaming, YouTube and video decoding are unsupported. Host tests cover codec routing/gain/mute, stream parsing/refills and GUI/browser controls; physical ATI/VIA audio has not been verified.
- `drivers/pcie.h` and `drivers/pcie.c`: validated below-4-GiB MCFG/ECAM access,
  conventional segment-zero fallback, firmware topology enumeration and
  upstream-first PCI D0 preparation for GTX 970 `01:00.0`. PCI word writes
  preserve W1C status bits. Boot skips the unverified `0xC0012121` MSR;
  verified RS780 L0, PCI configuration and assigned BARs permit GPU probing
  without a PMC handshake. Undefined PMC writes and BAR allocation without
  verified firmware windows remain blocked. Ordered `PCIE1` boot events replay on existing
  UDP/8080 telemetry once networking is ready; an immediate post-NIC pulse uses
  a queued pulse without waiting for DHCP/ARP before saved config loads.
  `MSRGUARDTEST` checks exception recovery under QEMU. `tests/pcie_host_test.c` runs
  during the build with ASan/UBSan.
- `drivers/nv_maxwell.c`, `drivers/rtl8168.c`, and the USB host sources build as
  trusted ring-0 modules at `A:\DRIVERS\NVMAXWEL.DRV`,
  `A:\DRIVERS\RTL8168.DRV`, and `A:\DRIVERS\USBHOST.DRV`. The build embeds each
  SHA-256 digest in the kernel trust table, resolves versioned HAL imports
  through `kernel/driver_loader.c`, and packages the modules onto FAT16/FAT32
  test disks and the ISO's internal app volume. NVIDIA GUI fill/copy hooks
  remain in the module ABI; an unavailable GPU preserves the software display
  path. NIC/USB absence or module-load failure degrades without stopping boot.
  Driver images use contiguous low-memory backing so the HAL can translate
  module DMA buffers to valid below-4-GiB device addresses. The driver-loader
  host test covers digest rejection, imports, init, ABI, and unload for all
  three modules; `tests/qemu_driver_module_test.py` checks their trusted startup
  and absent-hardware behavior on both FAT formats.
- The audio sources in `drivers/hda.c` and `drivers/hda_stream.c` are also built
  into the trusted `A:\DRIVERS\AC97.DRV` module. This legacy filename contains
  the existing ATI SB7xx HDA/VIA implementation (`1002:4383`), not an AC'97
  controller driver. It uses versioned HAL imports for PCI configuration,
  MMIO, DMA translation, relax, and logging. Its API owns audio controller,
  DMA, and playback state; the kernel dispatches mixer/player operations through
  that API. The build hashes and packages the module on FAT16/FAT32 and the ISO
  internal app volume. Driver-loader host coverage checks the module ABI and
  absent-device probe; `tests/qemu_driver_module_test.py` checks trusted loading
  and clean startup when the matching HDA controller is absent.
- `drivers/amd_host_bridge.c`: target RS780 host identification through
  existing MCFG ECAM, read-only port-indirect `0xA5[5:0]` L0 verification via
  `00:02.0` index/data `0xE0/0xE4`, saved/restored selector and IF, and bounded
  polling before root/endpoint command assertions. No lane-control writes or
  forced retraining; L0 does not guarantee safe GPU MMIO.
- `drivers/rs780.h` and `drivers/rs780.c`: shared exception-aware GFX root-port
  transport for boot LTSSM and manual diagnostics. Validated ECAM only, indexed
  A2/A5..AF reads, exact selector restoration, sticky restoration lockout, and
  guarded standard PCIe Root Port capability discovery with pointer/cycle/
  version/bounds checks. E0/E4 are configuration offsets, not CPU I/O ports.
- `drivers/rs780_manual_audit.h` / `.c`: standalone, packaged per-transaction
  operator API with no timers, loops or automatic ACKs. Every selector/discovery
  access gets its own INTENT/RESULT; LINKCAP/LINKSTATUS have isolated request
  functions. Shared indexed interlock spans manual waits, with explicit
  confirmed-result abort/restoration. It does not alter existing shell/boot
  sequencing or GPU permissions. Build host tests cover all access-fault sites.
  `rs780_audit_finish` ends only a successful six-step LTSSM/restore audit;
  it cannot grant full link preflight or skip restoration acknowledgement.
- `drivers/pcie_probe.c`: manually acknowledged, single RS780 indirect read
  (`A2/A5..AF`) or standard LINKCAP/LINKSTATUS read with retained INTENT/RESULT
  records on UDP/8080. LTSSM/LINKWIDTH are named index aliases. Shell `PCIEPROBE`
  and `ACK <id> INTENT|RESULT` never auto-advance or retry an access.
  Nonzero masks are denied; E4 is never written. Selector restoration failure
  locks out further probes. `src/interrupt_entry.S` provides exact-instruction
  CPL0 DWORD load/store #GP/#PF recovery, exercised by `MSRGUARDTEST` using RAM
  and invalid virtual addresses. Host tests cover all six transaction fault
  locations, phase/ID gating, send failure and no duplicate accesses, plus
  malformed/faulted capability chains and acknowledgement-only preflight.
  Preflight is diagnostic only; it never sets target_ready or enables GPU MMIO.
- `src/filesystem.h` and `src/filesystem.c`: FAT16/FAT32 + limited NTFS volume layer, directory handling, MBR/GPT partition parsing, and command integration.
- `src/book8gui.h` and `src/book8gui.c`: shared desktop platform, static window registry, taskbar, event routing, and input/frame scheduling.
- `src/book8gui_explorer.c` and `src/book8gui_shell.c`: separately rendered and input-handled Explorer and Command Shell apps.
- `src/usb.h` and `src/usb.c`: PCI USB discovery, UHCI HID, and shared input
  APIs, built into `USBHOST.DRV` with `src/ohci.c`/`.h` for polled OHCI. Kernel
  USB entry points dispatch through the versioned module API; the QEMU OHCI
  mouse regression validates enumeration and reports after authenticated
  desktop entry.

## What the kernel already does

### Boot and execution model

`boot.S` performs the very early kernel startup:

- declares a Multiboot2 header
- creates page tables for identity-mapped lower memory and a 64-bit long-mode switch
- initializes a kernel stack
- loads the GDT for 64-bit mode
- jumps into `kernel_main`

`kernel_main()` verifies the Multiboot2 structure, discovers the framebuffer, initializes serial I/O, discovers ATA/AHCI disks, initializes USB, mounts the filesystem service, and finally enters the shell loop.

This means the kernel is acting as both:

- the boot runtime
- the command interpreter runtime
- the GUI runtime
- the driver runtime
- the filesystem service runtime

There is no general protected multi-process userspace: kernel threads and a single-active-task Ring 3 ELF loader (`EXEC`) exist, but concurrent ELF processes and per-process address spaces do not.

### Console and driver layer

The kernel writes to both serial and VGA output and keeps a simple text console. It also has a PS/2 input path and a USB HID input path.

Important design notes:

- serial is used as a debug/logging channel; COM1 availability is detected from
  the line-status register, and transmitter polling is bounded so a missing or
  stalled UART cannot hang kernel execution
- VGA is used for local display when no framebuffer is present
- the input path queues PS/2 mouse packets and pumps polled UHCI/OHCI reports into BOOK8GUI events
- the shell accepts keyboard input from PS/2 or UHCI boot-protocol USB HID, plus serial console input in QEMU; USB mouse reports can come from boot protocol or the supported relative-report descriptor subset

### Desktop shell architecture

`BOOK8GUI` is a kernel-linked desktop coordinator, not a user-mode process. Its
static `WindowApp` registry records each app's ID, title, bounds, visibility,
focus, state, render callback, and event callback. File Explorer and Command
Shell are separate app modules initialized and rendered through shared
platform callbacks.

The taskbar is global and occupies `Y = SCREEN_HEIGHT - 36` through
`SCREEN_HEIGHT - 1` (`BOOK8GUI_TASKBAR_HEIGHT`); application content and
hit-testing must not claim that area. It contains the Start launcher at left,
active-window buttons in the middle, and a time/date clock plus NIC activity
indicators at right. Mouse handling processes launcher/taskbar actions and
focus changes before routing events to the focused app.

Input servicing is separate from rendering: PS/2 IRQ handling enqueues decoded
events, and polled USB service runs from the input pump rather than draw
callbacks. The desktop targets a 1000 Hz pump and schedules dirty rendering at
60 Hz when the clock source supports that cadence. OHCI is polled; this path
has no OHCI ISR. Explorer uses timer ticks for a 400 ms double-click interval
and can traverse supported FAT subdirectories.

The host test covers registry initialization, the reserved taskbar area,
taskbar focus switching, focused-window input delivery, input pumping, and
Explorer double-click navigation. The QEMU desktop test exercises the real
framebuffer, USB mouse, taskbar, window routing, and Explorer path update.

### Storage layer

Storage is intentionally conservative and hardware-aware.

- IDE PIO discovery reads drive identity and detects LBA28/LBA48 support
- AHCI controllers are discovered via PCI enumeration
- the kernel can read and write sectors using the appropriate controller path
- the file system layer reads partitions and supports MBR/GPT detection
- FAT formatting and installation is implemented at the kernel layer, not through a separate userspace tool

#### Physical-hardware input fixes: OHCI HID + PS/2 aux init (2026-11-03)

Bare-metal telemetry (AMD SB7xx board: USB 8/0/5/2/1 total/UHCI/OHCI/EHCI/xHCI, PS/2 mouse unavailable) showed the UHCI-only HID stack could not see any device and EHCI legacy handoff was skipped when no UHCI existed.

- `src/ohci.c/.h`: polled OHCI 1.0 driver (no interrupts). Takes ownership from SMM (OCR/IR), resets the HC, builds HCCA, control and 1 ms periodic EDs, powers and resets root ports, enumerates low/full-speed devices (no hubs), selects boot-protocol HID mouse/keyboard, and polls interrupt-IN reports. Reports feed the existing `book8_usb_poll_mouse/keyboard` APIs, so BOOK8GUI polls PS/2 and OHCI mice with no GUI change.
- `src/usb.c`: OHCI BARs are recorded; when no UHCI exists but OHCI does, EHCI legacy handoff runs and every EHCI port is released to its OHCI companion (CONFIGFLAG=1, owner bit on all ports, CONFIGFLAG=0). The UHCI path is unchanged.
- `src/kernel.c`: `ps2_mouse_initialize` now flushes the output buffer, issues 0xA7/0xA8, does the 0x20 read / 0x60 write of the command byte clearing bit 5 (aux clock), resets the mouse (0xFF, FA/AA/00), 0xF6, 0xF4 with retries, and no longer requires the AUX status bit on ACK bytes. IRQ12 is still enabled only when BOOK8GUI starts.
- The boot banner prints `OHCI started/ports/connected/HID/hubs/reports/errors` and `EHCI released to OHCI`. `INPUTSTAT` USB_REPORTS now counts OHCI reports.
- Test: `tests/qemu_ohci_mouse_test.py` (SB710-like profile, usb-mouse behind OHCI companions) PASS: 40/40 reports, no drops; the UHCI/PS/2 graphics smoke still passes; the storage matrix is unaffected.
- Bare-metal follow-up (SB710): ports reset and the first GET_DESCRIPTOR worked, but all SET_ADDRESS transfers failed (control failures 0/4/0). Cause: control-transfer completion used a fixed `pause` spin count, far too short on fast AMD CPUs. Fix in `src/ohci.c`: frame-based (HcFmNumber) 100 ms transfer deadline; port reset now waits for PRS to self-clear plus PRSC, a 10 ms TRSTRCY recovery, polls PES (setting it if needed) and requires CCS+PES before any control transfer; SET_ADDRESS is retried up to 3 times, probing the new address in case only the status stage was lost. The banner adds `last CC/retries` (OHCI condition code of the last failed transfer) for the next bare-metal run.
- Validation (QEMU 10.2.1, VirtualBox 7.2.22): storage matrix all 5 runs PASS (QEMU generic, target-pc, target-q35, VBox generic, VBox target); SB710 OHCI profile 8/0/5/2/1 PASS with 1/1/1/1/1 enumeration, 40/40 mouse reports, 0 drops, 0 descriptor failures; PS/2 and UHCI graphics smoke PASS. `tests/storage_matrix.py` now closes exported raw media in VirtualBox so reruns do not hit duplicate-UUID clone errors.
- Bare-metal follow-up 2: SET_ADDRESS now worked but config/HID-request transfers failed with CC=15 (NOT ACCESSED: the HC never retired the TD) and 2 retries. Likely cause (QEMU does not model it): the ED was rewritten while the HC could still hold it in HcControlCurrentED and write a stale HeadP back. `control_once` now clears CLE, waits 2 ms (past an SOF), zeroes HcControlCurrentED, rebuilds ED/TDs, then re-enables CLE and sets CLF. Non-SET_ADDRESS control requests retry up to 3 times with 20 ms gaps (covers NAK/slow devices). Banner now shows `last CC/stage/retries` (stage = index of the failing TD: 0 setup, 1 data, last status). CC 15 at stage 0 means the HC never serviced the ED; at the status stage it means the device did not complete the handshake. Re-validated: all 5 storage matrix runs, SB710 OHCI profile (1/1/1/1/1, 0 descriptor failures, 40/40 reports, 0 drops), graphics smoke.
- Not verified on physical silicon. Limits: no USB hubs, no xHCI (the board's single xHCI port set is not driven), no EHCI high-speed transfers. A mouse behind a hub or on the xHCI ports will still not work; check the new banner counters from the next bare-metal boot.

#### Storage write-path validation matrix (2026-11-03)

`tests/storage_matrix.py build/book8.iso build/book8-data16.img` (run under WSL; also invoked by `build.sh` when `BOOK8_RING3_VBOX=1`) boots each profile with IDE and AHCI scratch disks and checks the DMA read self-test, the DMA write + flush + read-back compare, injected bus-master/timeout/device fault recovery (retry, then PIO fallback), the 200-sector chunked write, no `DMA read/write failed` messages, and a host-side inspection of the scratch images (guard sectors intact, final pattern exact). Run with QEMU 10.2.1 and VirtualBox 7.2.22.

| # | Profile | Controllers | Result |
|---|---------|-------------|--------|
| 1 | QEMU generic | default machine, PIIX IDE bus master, `ich9-ahci` | PASS |
| 2a | QEMU target `pc` | i440fx, PIIX IDE bus master, explicit `ich9-ahci` at 00:11.0 (SB7x0 SATA slot) | PASS |
| 2b | QEMU target `q35` | ICH9 AHCI only (all disks via AHCI; no legacy IDE controller on q35) | PASS |
| 3 | VirtualBox generic | PIIX3 chipset, PIIX4 IDE, Intel AHCI, headless | PASS |
| 4 | VirtualBox target | ICH9 chipset, ICH6 IDE, Intel AHCI, headless | PASS |

Every profile printed `selftest PASS` and `write selftest PASS` for each scratch disk (IDE0-S and SATA000 on all but 2b, where SATA001 and SATA003 are the scratch disks), with no failures. Raw per-profile logs and `results.json` are in `build/runtime-validation/storage-matrix/`.

Limits of this evidence: neither emulator models the AMD SB710 itself (the "target" profiles are the closest controller mixes), the injected faults abort a transfer before the command is issued rather than failing the device mid-write, and none of this has been run on the physical GA-78LMT bench.

The installer flow is especially important:

- the kernel reads a module containing the installer payload from the GRUB boot entry
- it exposes disk inventory to the shell
- the user chooses a target disk
- installation requires confirmation of the selected disk and destructive partition replacement before writing
- it copies the prepared FAT32 installer image in multi-sector batches, expands the root volume, then writes and verifies the root and raw-swap partition table
- progress is printed during image copy and FAT32 relocation/initialization
- registry writes are deferred until the installed FAT32 root is mounted as `A:`, where the installer seeds `BOOK8.CFG` and the initial registry state
- GUI startup falls back to safe defaults and attempts to regenerate a missing or invalid profile without aborting desktop startup

### Filesystem layer

The filesystem module is one of the most complete parts of the project.

Supported/relevant capabilities:

- MOUNT for FAT16/FAT32 and limited NTFS subset
- DIR, VOL, TYPE, COPY, WRITEFILE, DEL, FDISK, FORMAT (shell MD/MKDIR and RD/RMDIR are deferred; the filesystem API handles directories)
- FAT long-name handling and VFAT-compatible entries
- FAT32 boot sector creation and root-volume structure generation
- partition table creation for MBR/GPT data partitions
- limited NTFS root-directory reading and file overwrites when the replacement size matches exactly

Current limitations are explicit and deliberate:

- no general POSIX userspace or concurrent multi-process isolation (`EXEC` loads one Ring 3 ELF at a time)
- shell `MD`/`RD` are deferred; directory creation/removal is available through the filesystem API and GUI
- no general-purpose NTFS write pipeline beyond the constrained root-file overwrite path
- no production-grade safe behavior for blindly overwriting disk metadata

### GUI and input

`book8gui.c` coordinates the framebuffer desktop and is linked directly into
the kernel; it starts automatically after GPU initialization on graphical boots
and can also be launched with `BOOK8GUI` from a command-shell boot. Explorer and
Command Shell have separate render/event modules.

This keeps GUI code close to the kernel rather than depending on a separate
user-mode window manager; standalone Ring 3 utilities are loaded by `EXEC` but
do not host the desktop.

The desktop currently supports:

- a registered File Explorer and Command Shell window
- a 36-pixel global taskbar with Start launcher, task buttons, clock, and NIC indicators
- mouse hit-testing and event routing to the focused window
- keyboard focus switching with Tab; leave the desktop with Start > Shutdown > Exit to Prompt (Escape only cancels dialogs or popups)
- FAT directory traversal and tick-based double-click activation

It requires a 24- or 32-bit RGB framebuffer of at least 640x300. Rendering is
scheduled separately from the input pump, and mouse packet handlers do not draw.

`src/system_config.c/.h` owns the shared validated control-panel preferences
and versioned/checksummed serialization. `book8_settings.c` implements the ten
categorized native forms; platform hooks in `book8gui.h` separate disk persistence,
read-only telemetry, test tones and optional runtime display modesetting from
the UI. Saving must succeed before committing preferences. Unsupported hardware
actions remain saved requests, not fabricated live state.

The desktop coordinator owns the 30-second display confirmation modal and
intercepts input before window routing. Only a successful backend mode switch
starts confirmation; timeout/cancel/save failure restores the old actual mode
and saved window geometry. Restoration failure stays modal for retry. The
firmware framebuffer backend intentionally leaves `set_display_mode` NULL.
The host `control_panel_host_test` exercises this contract, while
`tests/qemu_control_panel_test.py` checks all pages and reboot persistence using
a temporary copy of a regular data image.

Add/Remove Programs uses the coordinator's bounded ELF discovery index and
the `launch_program` platform hook. Built-in shortcuts and reserved/system
targets are protected in the removal API, not just the UI. Directory enumeration
and file metadata reads run in separate passes because the filesystem shares
its sector buffer. The index is derived from disk, so successful binary deletion
unregisters the discovered shortcut immediately and remains absent after reboot; failed
deletion retains the index entry. No separate persistent package manifest exists.
The `programs_menu` array explicitly defines HexViewer, CodeEditor, NetShare
and CAT with ELF paths. These four rows render regardless of scan results and
remain after uninstall; launch reports missing binaries. Discovered applications
follow the static rows, omitting duplicate canonical targets only in the menu.
The static menu policy does not itself alter directory enumeration.
`tests/qemu_static_programs_test.py build/book8.iso build/book8-data16.img`
compares all eight rendered fixed rows on autostart boot with and without an
external disk, launches the four fixed ELFs plus PaintBook and Web Browser from
the ISO's internal FAT module, and refreshes Add/Remove Programs. Its external
fixture has 140 directories and HOSTAPP.ELF; the index must remain at 13 internal
entries. The host Programs test checks all four fixed labels and launch paths
with an empty discovery index, plus PaintBook/browser/NV Diagnostic discovery and
file-app routing.
The same QEMU check follows every Programs row (including NV Diagnostic and HELLO) and the
Settings link, asserts each expected native window title, and runs NVMAP via
the GUI Command Shell. This covers all nine ELFs built by `build.sh` without
depending on an attached application disk.
Normal `build.sh` QEMU validation runs this all-links test plus the real
utility and NetShare behavior tests. `BOOK8_SKIP_RING3_SMOKE=1` skips this
QEMU block as well as the existing emulator regressions. Utility/NetShare
fixtures include a boot kernel so the scratch disk qualifies as an installed
system volume under the internal-only Programs scan policy.
`build.sh` verifies every bundled ELF byte-for-byte in `book8-apps.img`, which
normal GRUB entries load with the `book8-apps` module tag. The kernel registers
it before physical block devices and exposes it through both filesystem
adapters so GUI metadata and the ELF loader agree on `A:`. It is RAM-backed
and writable only for this boot. The separate Programs drive-policy callback
excludes external volumes without changing Explorer's drive registry. Without
the module, only `A:` with an existing boot kernel qualifies as a system volume.
`tests/book8gui_host_test.c` covers metadata, modal cancellation, failure and
core protection; `tests/qemu_programs_test.py` validates scratch FAT16/FAT32
removal, retained-file integrity, reboot discovery and Start-menu execution.
The index holds 512 entries (including built-ins), with 512-byte paths shared
by discovery, removal and launch. The breadth-first directory scan allows
512 directories and 32 levels below each drive root; name validation and target deduplication happen
before insertion. Optional `drive_readable` filters unformatted/unsupported
volumes; unavailable per-file metadata retains a protected entry with an
unavailable size rather than failing the entire walk. Directory failures and
capacity/path-depth limits still report an incomplete scan.
Infrastructure filenames (NVMAP, BOOK8, KERNEL, BOOK8GUI and SHELL ELFs)
are filtered before index insertion. Root BOOT/EFI/DRIVERS/SYSTEM/MODULES/KERNEL
trees are not traversed. The five built-in application shortcuts remain visible
and immutable; filtering does not remove binaries from storage or change direct
EXEC behavior. Standalone Explorer/Text Editor/Image Viewer ELFs get friendly
names. Directory failures retain the first path and backend error before later
metadata calls can overwrite the filesystem's last-error state.

`fs_list_directory` normalizes `A:` to `A:\`, and implicit empty/root-relative
paths to the current command drive's root. It activates that drive and, only
for implicit paths, can mount the first readable volume with a diagnostic.
Explicit invalid drive/path requests still return errors. FAT16/FAT32 loader
host tests compare all supported root aliases and root-relative BIN traversal.
They also remove a simulated active command drive and verify the diagnostic
fallback succeeds with a cleared error while explicit missing-drive paths fail.

Programs menu rows are computed from the live desktop work area, not the static
four built-in menu items. Drawing, row dispatch, wheel bounds and thumb drag all
share this visible-row count. The shared UI helpers use the same thumb rectangle
for rendering and hit-testing and preserve the grab offset; Settings keeps
processing an active list drag even over its sidebar. `gui_programs_host_test`
checks 73 executable entries at 640x300 and 800x600 with malformed names,
unavailable metadata, drive filtering and last-row launch.
The host suite also checks 300 ELF fixtures, 140 sibling directories and
16-level paths longer than 256 characters, plus explicit capacity/path failures.
`qemu_programs_overflow_test.py` adds 300 ELF fixtures, 140 directories and
a 32-level long-path executable to a temporary FAT copy,
checks the actual registry count, launches Hex/Code/Netshare and CAT directly and the last
overflow entry using its full path, and compares rendered Settings rows during thumb drag/wheel.
It accepts both the autostart ISO and the command-shell ISO, asserting that no
boot or refresh scan reports incomplete.
It also plants excluded infrastructure ELFs and supports the partitioned FAT
installer image (mtools offset 1048576) to test its real BOOT/EFI trees without
modifying the source image.

Mouse events use `int16_t wheel_delta` in 120-unit detents, not a spatial
coordinate. `mouse_input.h` converts PS/2/HID detents with bounded signed
scaling; both UHCI descriptor-based reports and OHCI boot reports use it.
The native queue carries this value unchanged. `gui_poll_event` packs the
standard wheel high word and button flags and uses `book8gui_decode_wheel_message`
before dispatch. That decoder ignores horizontal/other messages and preserves
separate 32-bit screen coordinates. `ui_wheel_steps` retains signed fractional
remainders independently per scrollable control, then existing scroll helpers
apply whole detents. Host tests cover +/-240, signed high-word extremes, +60/+60
accumulation, ignored WM_MOUSEHWHEEL, hover routing and unchanged keyboard focus.
The graphics/input QEMU smoke requires actual +/-120 driver output for PS/2
and UHCI; it does not imply broader Win32 compatibility.

Hex Viewer and Code Editor are separate `user/utility.c` ELF builds installed
as `BIN/HEXVIEW.ELF` and `BIN/CODEEDIT.ELF` in FAT data/installer images.
`SYS_gui_utility` validates/copies a bounded kind/path request into a one-slot
queue. After successful ELF exit the coordinator launches the kernel-native
`src/apps/book8_utilities.c` window service. Failed executions cancel requests;
queue backpressure, launch failure and file I/O errors are reported explicitly.
The registry recognizes their canonical paths, lists them directly in Programs,
and refuses uninstall while native utility windows remain open.

The pure editor has a 64 KiB text limit, line numbers, four-column tabs,
keyboard navigation and Save/Save As path modals, with no suggestion engines.
Hex viewing is read-only, bounded to 1 MiB, with 16-byte formatting, horizontal
arrow-key panning, a vertical scrollbar and signed wheel scrolling.
Four combined utility state slots plus shared read staging use static memory.
Failed reads preserve the previous document; unsaved editing refuses normal
close/open and permits explicit Task Manager termination.
`fs_write_file` supports existing FAT subdirectories and shares the boot
updater's verified copy-on-write replacement helper; uncertain commits remain
errors. `fs_read_cluster` reads only validated FAT data clusters.

`tests/utilities_host_test.c` covers formatting, wheel, raw cluster, editing,
Save/Save As and error handling. `tests/fs_loader_host_test.c` checks nested
FAT16/FAT32 saves, empty replacement and failure retention in memory; existing
boot-update failure injection still validates the shared writer.
Run `python3 tests/qemu_utilities_test.py build/book8-cli.iso build/book8-data16.img`
(also FAT32) for real ELF launch, rendering/input, disk Save/Save As and removal
using a disposable image copy. Logs/screenshots default to `build/utilities-validation`.

### NetShare LAN file browser

`user/utility.c` also builds `NETSHARE.ELF` (GUI kind 3), registered under
Start > Programs as Network Share Browser. Its singleton native window lives in
`src/apps/book8_netshare.c`. Class `tick` callbacks run on every desktop frame,
including minimized windows, so network operations progress without blocking
input. Errors restore a minimized browser and display its modal status alert.
The platform supplies the active network and existing local filesystem hooks
for listing, metadata, reading, create-only downloads, rename/delete and
directory creation; network-disabled settings return no active network.
Local/Remote switches the split directory/file panes. Keyboard Up/Down can
select folders for properties, rename or deletion without navigating.

`net/tcp.c` provides a single active outgoing stream per network, bounded receive
storage, TCP/IP checksums, SYN/ACK, ACK/window/MSS tracking, stop-and-wait segments,
retransmission, duplicate/overlap handling, receive backpressure, reset handling
and a 10-second transport deadline. It hooks only protocol 6 into `net/net.c`;
existing ARP/ICMP/UDP behavior and NIC DMA remain unchanged.

`net/smb.c` builds/parses bounded direct-TCP SMB2.0.2/2.1 messages: negotiation,
SPNEGO anonymous NTLM or NTLMv2 credential session setup, disk tree connect,
create, query-directory, read, write and close. `net/ntlm.c` supplies bounded
MD4/HMAC-MD5 NTLMv2 response construction; `net/sha256.c` supplies SMB2 signing.
Signed responses are verified and unsigned/tampered responses are rejected
after an authenticated signed session. Responses validate framing, command/message
identity, session/tree IDs, buffer offsets and directory record lengths.
SMB operation deadlines are 15 seconds, transfers at most 1 MiB, directories at
most 128 ASCII entries. Upload uses FILE_CREATE (never overwrite), bounded
1024-byte writes, checked partial-write counts and close before success.
Failed/cancelled uploads may leave a partial remote file. Directory records
retain allocation, attributes and creation/modification FILETIMEs for properties.
Encrypted, compound, asynchronous, NTLM MIC/channel-binding and unsupported
dialect responses fail explicitly. There is no SMB1/SMB3 fallback, DNS resolver,
share-name RPC discovery, DFS or remote rename/delete/mkdir.
The share field is required and defaults to `public`.

The masked login modal offers username/domain/password, explicit Guest and an
optional transient login. Authentication errors reopen it for retry, while
share/file permission and connection errors use explanatory alerts, not raw
NTSTATUS codes. A credential login mapped to Guest is rejected.
`A:\NETSHARE.CFG` holds a versioned bounded binary record of eight profiles;
all fields are validated on load. Persistence commits before in-memory profile
changes. Passwords are excluded from the disk format and diagnostics; successful
logins can be remembered in a separate session-only profile cache. A cold boot,
transient selection or auth failure requires another prompt. SMB password/token
buffers are cleared after authentication and on close/failure.

`fs_create_file` reuses the checked FAT creation path but refuses collisions;
`fs_make_directory` reuses the existing checked tree-copy directory allocator
and creates one directory with an existing parent. The GUI creates DOWNLOADS
as needed, displays a short downloaded preview, launches Code Editor through its
ELF, or opens a new Explorer at the local directory. Uninstall leaves downloads
intact and is refused while the browser owns an active window.

The supported build runs `tcp_host_test`, `ntlm_host_test`, `smb_host_test` and
`netshare_host_test` (including local operations, password masking, transient
logins, profile serialization/session reuse and auth retry).
`smb_integration_test.py` requires `impacket` in an isolated interpreter and
launches guest/read-only and NTLMv2/writable servers on an ephemeral loopback
port, with only synthetic
fixtures. `smb_integration_host` sends actual BOOK8 wire packets through a POSIX
socket adapter to that server; it checks session/tree interoperability, nested
listing, byte-exact multi-read/multi-write data, signing, empty uploads,
collisions, rejected credentials and bounds/missing-share errors.
`qemu_netshare_test.py` checks GUI/ELF/menu/uninstall, local filesystem operations,
properties, persistent password-free profiles/cold-boot restore and offline alerts using a
temporary FAT copy. Stock QEMU has no RTL8168/8169 device, so it cannot establish
live SMB using the current NIC driver. Do not equate its GUI pass with live
hardware network validation.

### BOOK8 Player

BOOK8 Player is the desktop's VLC-style sound player. Its Media menu opens a
local WAV file, an HTTP WAV/network stream (handed to Web Browser), or a folder
as a generated playlist; Playback offers previous/next, Jump to Specific Time,
bookmarks and playback-speed markers; Audio exposes stereo/mono status, mute,
visualization and maximum volume; Tools opens Preferences, Codec Information
and Reset Preferences. The dark canvas shows an orange cone glyph, track-status
text and an optional spectrum-style visualization while playing.

Preferences are persisted in `A:\MEDIA.CFG` and shared with Web Browser. The
dialog has Interface (status bar/startup playlist/dark canvas), Audio (maximum
volume), Input/Codecs and Media Plugins (PCM WAV, HTTP WAV stream, playlist and
visualization plugins), Browser Integration (HTTP WAV autoplay, Page Links media
discovery, opening WAV links in Player), and Hotkeys sections. Browser playback
honors these settings: disabled HTTP WAV plugins or autoplay show a status and
skip playback; disabled media discovery omits audio/video sources from Page
Links; enabled WAV-link handoff queues the URL for Player. Playback speed,
mono mixing, video tracks, subtitles and fullscreen are informational only
because the audio path plays at the WAV's source rate and no video decoder is
present. Only PCM 48 kHz stereo 16-bit WAV is accepted; MP3/other codecs,
HTTPS/TLS, adaptive streaming, YouTube and video decoding remain unsupported.

### PaintBook and Web Browser

`user/utility.c` builds PaintBook (packaged as the FAT-compatible
`PAINTBK.ELF`) and `BROWSER.ELF` as standalone Programs
launchers. Both are scanned from `A:\BIN` and can be removed in Settings after
their windows are closed. PaintBook opens BMP, GIF, JPEG and PNG files up to
1024x768, provides pencil, eraser, line, rectangle, fill, palette, zoom and one
undo step, and saves drawings as BMP. Its open/save dialogs accept absolute FAT
paths.

Web Browser now uses an Internet Explorer-style chrome: raised beveled menu and
toolbar buttons with small Back/Forward/Stop/Refresh/Home/Go icons, hover and
pressed feedback, a separate Address field and Go button, and a scrollbar on
long pages. It keeps an editable address bar, a short history and clickable
links. Settings can persist the current supported address as the homepage and
clear session cookies. It supports bounded plain HTTP requests to IPv4 addresses
or hostnames resolved through the configured DHCP DNS server, text/plain and
modern HTML documents rendered as readable text. The parser understands semantic
HTML5 block tags, `<title>`, `<base href>`, character/numeric entities, headings,
lists and table-cell separators, resolves relative links, makes inline anchor
text clickable, follows up to four HTTP 3xx `Location` redirects, and honors
`Refresh` headers and `<meta http-equiv="refresh">` navigation. Standalone
BMP/GIF/JPEG/PNG images are decoded and shown; image, audio/video/source and
direct media links are listed in Page Links. Chunked transfer decoding and a
512 KiB response limit remain. HTTP WAV audio is passed to the shared HDA
playback path; only PCM 48 kHz stereo 16-bit WAV is accepted. Video and other
audio codecs display an unsupported-format status. Inline media layout, HTTPS/
TLS, JavaScript, CSS layout, adaptive streaming, YouTube and video codecs are
not supported. Google responds over HTTP with a chunked page, but may require
JavaScript to display search results; the browser can show its static response
text, not the interactive results. Network requests use the existing single
outgoing TCP context.
The host build also runs `book8_image_host_test` for BMP decoding and
dimension/input bounds and decoder-pool isolation, and `book8_apps_host_test`
for PaintBook drawing/BMP export, player menus/dialogs, WAV/volume/seek controls,
network-stream handoff, preferences persistence, browser homepage/cookie
controls, entity/title parsing, relative inline-link clicks, HTTP redirects,
meta-refresh navigation, media link detection, preference-gated HTTP WAV
playback and media discovery, startup and unsupported-address handling.
`build.sh` packages the browser as
`A:\\BIN\\BROWSER.ELF` in the boot-time application image.

### NV Diagnostic

`user/utility.c` builds `NVDIAG.ELF`, packaged under `A:\BIN` and discovered
as the removable Programs entry “NV Diagnostic”. Its native window refreshes
read-only GPU/PCI identity, BAR mappings, the GM204 architecture ID, allowlisted
BAR0 status registers, PFIFO/GPFIFO and pushbuffer bookkeeping, inherited
scanout placement, driver/firmware capability status, and copy-engine state
and counters (fills, copies, CPU small-rectangle skips, unsupported fallbacks
and fence timeouts). Register reads go through the kernel's existing PCI
power-state guard and a fixed offset allowlist; the GUI never accepts
arbitrary register addresses.

VBIOS image parsing, ACR firmware loading, and memory-controller telemetry
remain unimplemented and are labelled unavailable rather than inferred.
Normal graphical boot suppresses startup console/PCIe/NV text and clears the
framebuffer before entering BOOK8GUI; `book8.autostart=0` retains the verbose
CLI/diagnostic output. `book8_nvdiag_host_test` covers the tabs and unsupported
telemetry state, while `qemu_static_programs_test.py` checks the packaged
launcher and Programs-menu window.

### USB

USB support is a proof-of-concept but useful. The kernel enumerates PCI
controllers and detects UHCI/OHCI/EHCI/xHCI separately. UHCI drives boot-HID
keyboards and boot-protocol mice, plus the supported non-boot mouse report
subset. The polled OHCI path supports low/full-speed boot-HID devices without
hubs; the SB710-like QEMU profile validates its USB mouse path.

UHCI and EHCI PCI command registers are configured for their respective I/O
or memory decode and bus mastering, then read back before controller use.
EHCI legacy ownership is read from PCI configuration space, not from its MMIO
BAR. Handoff is deferred until PCI discovery completes. When no enabled UHCI
controller exists but a usable OHCI companion path does, the kernel releases
EHCI ports to OHCI; otherwise, the firmware retains EHCI ownership and legacy
SMI routing. Handoff clears
EHCI SMI enables, enables routing, and assigns connected full-/low-speed ports
to companions where line-status bits indicate a valid non-high-speed device.
EHCI port power is requested only when per-port power control is advertised.
UHCI PORTSC has no standard VBUS power-enable bit, so the driver does not set
reserved bits.

After USB initialization, the kernel preserves other i8042 command-byte flags
while enabling the keyboard interface and set-1 translation for polled input.
This confirms controller configuration, not that physical key bytes arrive.
UHCI host-controller reset and chipset-specific legacy-emulation behavior
still require bare-metal verification.

Notable constraints:

- UHCI is the native HID host path; the polled OHCI driver supports boot-HID devices on the tested SB710-like profile, with EHCI companion routing
- EHCI itself does not provide a native HID path; xHCI remains unsupported
- generic mouse support is intentionally limited; arbitrary HID layouts, wheel reports, and generic HID keyboards are not implemented
- no hub support yet
- no xHCI root-hub enumeration yet
- bare-metal SB710 validation remains necessary for chipset-specific physical port behavior

This is the current tested boundary for hardware input.

### Latest USB hardware/emulator follow-up

The supplied SB710 and G41 reports both show zero connected/reset/addressed USB devices and zero HID devices. That points to a failure before descriptor transfer or interrupt polling; the available counters do not justify a reset or power workaround. The G41 controller total is internally inconsistent: its reported subtypes add to 10, not the stated total of 6.

`USBSTAT` now includes each detected UHCI controller's PCI location, BAR, command register, controller status, frame number, and raw status for each root port. The QEMU integration suite passes with checks for this output. VirtualBox 7.2.22 still boots to the prompt but exposes no UHCI controller in its PIIX3 configuration and reports no HID. Bochs 2.8 remains inconclusive: the bounded run recorded 8,032 active-status and 8,032 ISO-bit warnings without verified BOOK8 shell/serial output. Do not treat either result as bare-metal UHCI validation.

The latest input-handoff pass leaves EHCI firmware-owned when no UHCI controller is available, configures i8042 keyboard polling, and distinguishes that configuration from physical keyboard activity in the boot banner. The QEMU full suite passes. VirtualBox's EHCI-only test showed `EHCI legacy handoffs: 0` and `handoffs skipped without UHCI: 1`, reached the shell, and accepted `USBSTAT`. The latest Bochs 2.8 run loaded the UHCI keyboard/mouse and started controller activity, but did not provide verified BOOK8 prompt or serial telemetry. It logged 11,594 each of `When Active bit is set, all others in the 'Status' byte must be zero. (0x7F)` and `ISO bit is set...`, plus 11,593 `Reserved bit in the Link Pointer is not zero.` warnings. The Bochs result is inconclusive, not a pass. This does not resolve the G41 report: it lists no EHCI, and resetting UHCI can still affect chipset-specific legacy USB emulation. Retest on hardware with corrected controller totals and raw port status before claiming USB or PS/2 input is repaired.

## Progression steps to date

The current project state reflects a clear progression, not a random collection of features:

1. Bootstrapping the kernel into long mode
   - bootloader handoff
   - page-table setup
   - stack and GDT initialization

2. Console and basic I/O
   - serial debug channel
   - VGA text output
   - keyboard scanning and event conversion

3. Storage discovery and driver development
   - IDE PIO drive identification
   - AHCI PCI enumeration and read/write flows
   - disk inventory and safe install commands

4. Filesystem layer
   - FAT volume detection and mounting
   - root-directory browsing and file commands
   - formatting and partition-table creation
   - NTFS root-file subset support

5. USB input integration
   - UHCI controller discovery
   - HID report handling
   - keyboard and mouse event flow into the shell and GUI

6. GUI and kernel-integrated app model
   - framebuffer explorer
   - command shell embedded in same kernel context
   - event polling and UI redraw loop

7. Installer packaging and validation
   - BIOS/UEFI installer image
   - GRUB menu entries
   - QEMU-driven smoke tests covering cold boot and install flow

This progression is the correct foundation for building more "applications" on top of the kernel. The main boundary is that those applications are still kernel-resident modules, not separate user-mode processes.

## Development notes and architectural lessons

### 1. Keep the kernel as the runtime core

The project is already structured around a single kernel runtime. That is the safest way to extend the OS without prematurely designing a full userspace ABI.

For future app work, do not assume a full POSIX process model exists. Apps can be kernel-linked modules or standalone freestanding ELF64 executables loaded by `EXEC` into the single active Ring 3 slot; there is no concurrent multi-process isolation or protected per-process address space.

### 2. Use the shell as the app launcher

The shell is the right top-level dispatcher. It already routes commands through a centralized command parser and can call GUI or filesystem functions directly.

The natural next step is to add a small app registry:

- app name
- entry function pointer
- required resources
- launch mode (shell, fullscreen, GUI panel, background task)

Then the shell command can call the app by name instead of hardcoding every feature into a giant command switch.

### 3. Separate kernel services from UI code

The current architecture keeps driver code, filesystem logic, and GUI logic in separate modules, which is good practice and should continue.

A future app should follow the same pattern:

- kernel/service layer for hardware and filesystem access
- UI layer for framebuffer or console rendering
- command/API bridge for app invocation and status

This keeps the BOOK8 kernel from becoming a one-file monolith while still avoiding the complexity of a full userspace separation.

### 4. Treat pages, memory layout, and app memory as a design boundary

Even though this project is still single-address-space, the kernel is already using a 64-bit paging setup. That means app memory should be planned with clear ownership boundaries:

- kernel memory reserved for core runtime and drivers
- app memory reserved for modules or dynamic allocations
- stack and heap regions should be explicit and tracked
- no app should bypass kernel-level ownership of physical/device memory

This is especially important as more apps are added to the same kernel image.

### 5. Do not overbuild the user environment too early

The OS is in a primitive but useful state. The next major milestone should not be a complete POSIX userspace. It should be one of these:

- a richer kernel command set
- a minimal app framework for kernel-linked apps
- a real process/task abstraction
- a file API for app-level reads/writes

The current code is already mature enough to support small, kernel-native app modules. That is the most productive path before trying to emulate Linux or Windows userspace complexity.

## Kernel execution notes for future app work

This section is intentionally operational and should be used as the living design reference while building more apps on the BOOK8 kernel.

### Execution model to preserve

The BOOK8 kernel is currently designed around this flow:

1. bootloader hands control to `boot.S`
2. long-mode setup and GDT activation
3. `kernel_main()` initializes hardware and services
4. shell starts and accepts commands
5. shell can invoke filesystem logic or launch the GUI
6. app-like modules are linked directly into the kernel image and executed by command dispatch

For future apps, maintain this model:

- the kernel remains the execution environment and service provider
- app entry points are kernel-linked functions/modules or freestanding ELF64 executables loaded by `EXEC` into the single Ring 3 slot
- command dispatch launches both forms from the same shell
- the shell remains the top-level orchestration layer

### Recommended app pattern

Each new app should follow this pattern:

- create a small module in `src/` or a focused subdirectory
- expose a startup entry function and a minimal command alias
- use the same console/print APIs as the shell
- call existing filesystem and input interfaces rather than reimplementing drivers
- keep UI logic separate from core logic as much as possible
- return to the shell cleanly when done

Implemented examples and patterns to follow:

- File Explorer (native `src/apps/book8_explorer.c`) using the filesystem API
- NoteEditor (`src/apps/book8_editor.c`): menu bar, dialogs, file I/O via `fs_read_file`/`fs_write_file`
- Task Manager (`src/apps/book8_taskmgr.c`): reads `book8gui_window_snapshot`
  / `book8gui_get_telemetry`; new apps should set `state_size` (reported as
  memory) and may call `book8gui_note_disk` for file transfers
- standalone utility ELFs (`user/utility.c`, `user/nvmap.c`) for the loader path
- further candidates: calculator/diagnostics commands, network/serial utilities and framebuffer tools built around `book8gui` concepts

### Interface conventions to keep

The codebase already has a good pattern for isolated services:

- `filesystem.h` exposes a clean kernel-facing API
- `book8gui.h` exposes a platform interface for UI launch
- `usb.h` exposes driver-facing abstractions for HID input
- kernel.c coordinates them and acts as the system orchestrator

Future apps should use that same layering. Do not let an app call disk, USB, and framebuffer registers directly from random places. Put a thin adapter layer in the kernel and then have the app call that layer.

### Memory and safety rules

As more apps are developed, enforce these rules:

- no app writes beyond its allocated buffer
- no app assumes the shell line buffer is safe to overwrite without bounds checking
- no app touches hardware directly unless it is part of a driver module
- no app reads or writes disk sectors without the kernel API
- any unsafe action should require explicit confirmation before modifying disk metadata

This is especially important because the project currently contains disk-erasing utilities and physically destructive operations.

### Validation approach for future work

Keep the current test discipline:

- rebuild with the project scripts
- run `tests/book8gui_host_test.c` for the desktop's headless preflight
- run `python3 tests/qemu_desktop_test.py build/book8.iso /tmp/d.img` for desktop interaction changes
- run `python3 tests/qemu_ohci_mouse_test.py build/book8.iso` for OHCI input changes
- run the QEMU smoke tests for installer and input flows
- smoke-test shell commands for each new app entry
- validate keyboard and mouse behaviors in the GUI or shell
- create `/tmp/d.img` only as a disposable copy of `build/book8-data16.img`; the desktop test attaches it with snapshot mode
- keep all runtime validation separate from installation to physical disks; stop at QEMU and do not flash hardware

The current QEMU test coverage is a strong foundation. Use it as the acceptance gate for each new app that changes kernel execution flow.

## Recommended next milestones

A sensible next step order is:

1. refine window lifecycle and layout behavior while preserving the taskbar reservation
2. expand filesystem browsing and file actions through the kernel filesystem API
3. strengthen input/render timing and regression coverage across UHCI, OHCI, and PS/2
4. consider a process abstraction only after the kernel-linked app model is stable

The BOOK8 code currently demonstrates a working kernel shell + GUI + storage + USB path. The next logical step is not a full UNIX clone; it is a small, stable application runtime designed around the existing single-kernel execution model.

## Final development summary

BOOK8 already has the broad features of a real prototype operating system:

- boot to 64-bit long mode
- kernel shell and commands
- ATA/AHCI storage stack
- filesystem implementation (FAT + limited NTFS)
- USB HID input
- framebuffer multi-window desktop shell with separate Explorer and Command Shell apps
- installer image and GRUB boot flow
- QEMU integration tests

The project has a static window registry and focused-app event routing. Future
app modules should preserve the same execution model and keep the kernel in
charge of hardware and system service access.

This is the right ground for building more apps on BOOK8 without jumping prematurely into a full userspace model.

## 16. UHCI descriptor encoding and current emulator status (2026-11-01, superseded by KERNEL_NOTES section 26)

UHCI schedule links in `src/usb.c` now pass through a single encoder that keeps the 16-byte pointer alignment and permits only defined link attributes. Active TD status construction explicitly clears error-status and isochronous flags. This hardening builds cleanly and the full QEMU regression suite passes.

The Bochs warnings recorded in this section were later traced to Bochs' default 128 KB SeaBIOS image, which resets the CPU before GRUB when a UHCI keyboard is attached; they were not BOOK8 descriptors. With `bios-256k.bin`, Bochs reaches `kernel_main()` with a USB keyboard and processes HID reports (KERNEL_NOTES section 26). BOOK8GUI pointer input remains unvalidated in Bochs because no Multiboot framebuffer tag is delivered there. The Intel G41/ICH7 freeze remains a separate, unverified bare-metal issue.

## BOOK8 Desktop shell and validation

- `src/book8gui.c` owns the static `WindowApp` registry, desktop event routing,
  launcher, taskbar, and frame/input scheduling. Explorer and Command Shell
  rendering and event handling live in `src/book8gui_explorer.c` and
  `src/book8gui_shell.c`.
- The 36-pixel taskbar is reserved across the full display width. Mouse input
  performs taskbar hit-testing and app focus changes before focused-window
  dispatch. PS/2 IRQ decoding and polled USB service remain separate from
  drawing.
- Run the headless behavior check through `build.sh` (or compile
  `tests/book8gui_host_test.c` directly). Run the QEMU desktop check with only a
  disposable `/tmp/d.img` copy; the script enforces that exact regular-file
  target and uses QEMU snapshot mode.
- Desktop acceptance markers include two registered windows, reserved taskbar
  height, focus and mouse-route messages, Explorer navigation to
  `A:\DATA\`, positive `USB_REPORTS`, zero `USB_QUEUE_DROPS`, and
  `PASS: BOOK8 desktop test`. The OHCI regression separately requires the
  expected SB710-like enumeration counters and zero drops.

## Multi-drive support

- src/filesystem.c scans every disk at startup (scan_drives): each mountable FAT16/FAT32/NTFS partition gets a letter in discovery order (A: = first/boot volume). Disks with no readable volume are listed last as unreadable and report BOOK8_FS_ERR_NO_PARTITION / BOOK8_FS_ERR_NO_VOLUME.
- fs_list_directory("B:\\...") selects the volume by letter (remounting on demand); fs_last_error()/fs_error_text() give the status. fs_drive_count/fs_drive_letter/fs_drive_info expose the registry.
- Shell: DRIVES lists letters, X: switches the shell drive, MOUNT <disk> rescans. File commands run on the shell drive.
- File Explorer has a drive row; clicking a letter lists that drive's root. tests/qemu_drives_test.py (in build.sh) boots two disks and checks DRIVES, B:, A: and an invalid letter.
