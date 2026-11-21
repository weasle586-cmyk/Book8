# BOOK8 (Flight of Icarus) Kernel Notes

This file is the working developer reference for the BOOK8 kernel itself. It is intentionally narrower and more implementation-focused than the user-facing README. It is meant to help developers, future AI agents, and contributors understand what the kernel already does, what is intentionally limited, and how to build apps against the current kernel model without assuming a full modern userspace exists.

`README.md` is canonical for current build, boot and usage behavior. Dated sections below (15 onward) are investigation logs; later sections supersede earlier ones (for example, section 26 resolves the Bochs UHCI warnings discussed in sections 18-21).

## Critical boot-safe rules

These rules are mandatory for every future BOOK8 boot change:

- The 32-bit startup path must enable both `CR4.PAE` and `CR4.PSE` together before entering long mode. A 2 MiB identity-mapped page layout without `PSE` is invalid on real hardware and can triple-fault immediately.
- The Multiboot2 framebuffer tag must never be forced to a specific unsupported VBE mode. If the firmware does not provide a valid framebuffer, BOOK8 must fall back to VGA text mode instead of touching invalid video memory.
- `kernel_main()` must not assume a valid framebuffer or an accepted firmware mode before validating the Multiboot2 tag.
- Serial output can be used for debugging, but it is not a substitute for a correct early boot transition.

## 1. Kernel purpose and execution model

BOOK8 is a freestanding x86-64 kernel that boots directly from Multiboot2 and runs as a single-address-space, kernel-centric operating environment. It has a preemptive kernel-thread scheduler and a limited single-active-task ELF loader, but no general user-process scheduler or protected multi-process virtual-memory model.

Experimental SMP support discovers processors from ACPI MADT, starts xAPIC-compatible application processors with INIT/SIPI, and schedules kernel threads with CPU affinity and work stealing. APs run kernel threads only: Ring 3 execution and current shell, GUI, and device-service paths remain BSP-owned. Do not call driver/module APIs from AP workers until those APIs have explicit cross-core synchronization. x2APIC and APIC IDs above 255 are not started; unsupported systems continue in BSP-only mode. Before an in-RAM kernel relaunch overwrites the loaded image, the BSP broadcasts a quiesce request, waits for every AP to acknowledge, and parks APs in the low trampoline page; the relaunched kernel restarts them with INIT/SIPI.

The current execution model is:

1. GRUB loads the BOOK8 ELF as a Multiboot2 kernel.
2. `src/boot.S` defines the Multiboot2 header and sets up paging/long mode.
3. The boot code transitions from 32-bit protected mode to 64-bit long mode.
4. `kernel_main()` runs in `src/kernel.c`.
5. The kernel initializes console, hardware, storage, USB, and filesystem services.
6. The shell starts and accepts commands.
7. Built-in kernel modules such as the filesystem and GUI are invoked directly from the kernel command dispatcher.

This is a kernel-native app model. Future applications should be planned as kernel-linked modules or built-in app entry points until a proper process abstraction is added.

## 2. Boot and CPU initialization

### Boot stage

`src/boot.S` provides:

- Multiboot2 header with framebuffer mode configuration
- temporary page tables for identity mapping
- PML4, PDPT, PD tables for long mode
- 64-bit GDT setup
- stack initialization
- handoff to `kernel_main`

### CPU state assumptions

The present kernel assumes:

- x86-64 CPU present
- paged memory enabled
- no standard C runtime support
- no libc or hosted runtime
- direct hardware I/O with inb/outb/outw/outl instructions
- freestanding C implementation only

### Boot validation

`kernel_main()` validates the Multiboot2 handoff and confirms that:

- the boot magic matches `MULTIBOOT2_BOOTLOADER_MAGIC`
- the info block exists and is structurally valid
- framebuffer metadata is parsed if present
- the installer payload module is detected when the installer menu entry is used
- ACPI shutdown metadata is discovered from Multiboot2 ACPI tags, with an EBDA/BIOS RSDP scan fallback

The `HALT`, `EXIT`, `QUIT`, and `BYE` commands request ACPI S5 shutdown using firmware-discovered RSDP/RSDT/XSDT, FADT, and DSDT data. The kernel reads PM1a/PM1b control GAS fields (or the legacy PM1 control blocks), extracts `_S5_` sleep types from the AML package, and sends the FADT's ACPI-enable command through `SMI_CMD` if `SCI_EN` is not set. PM1 control registers may be system I/O or system memory. Boot telemetry reports the RSDP revision/root address and, when found, the FADT revision/address, DSDT, PM1a, and S5 type.

This follows the ACPI tables supplied by firmware and is suitable for platforms such as the AMD SB710; do not hard-code chipset-specific PM ports. If the firmware tables are missing, invalid, or use unsupported AML, BOOK8 reports shutdown unavailable and falls back to `cli; hlt`. QEMU PC validation observed the PM1a I/O block at `0x604`, S5 type `0`, and QMP `shutdown` after `HALT`; bare-metal confirmation is still required for each firmware.

## 3. Console, output, and shell environment

### PCIe initialization before GPU MMIO

After GPU initialization, BOOK8GUI starts automatically unless
`book8.autostart=0` or the installer/text-mode path requires a prompt.
Default graphical startup clears the boot console and suppresses early text
output while preserving the Multiboot2 framebuffer handoff marker on serial.
Installer-module boots also require the visible console path: they skip desktop
autostart but must not inherit quiet graphical startup. VGA text memory is used
only after Multiboot2 framebuffer validation rejects or lacks a usable mode.
NV register diagnostics are presented by the removable `A:\BIN\NVDIAG.ELF`
launcher through a read-only, allowlisted kernel snapshot. The snapshot also
carries copy-engine discovery/self-test state and the driver's acceleration
counters; the Accel tab renders them without touching hardware. VBIOS parsing,
ACR firmware loading, and memory-controller telemetry are not implemented.
Telemetry submission is gated until initialization completes. The runtime
CLI/desktop pump attempts one packet at a time through non-blocking RTL8168
TX enqueue; full rings return busy without waiting for DMA completion.
NVMAP snapshot reporting and the UDP 8081 push agent remain disabled.

`drivers/amd_host_bridge.c` reuses MCFG-derived segment-zero ECAM (bus 0
coverage required) and checks exact RS780 host/root IDs `96001022/96031022`,
and root routing `00 -> 01 -> 01`. No CPU family or MSR check gates this
path. No new host-routing programming or ECAM/MSR base discovery is performed.

RS780 GFX-port verification selects index `0xA5` at `00:02.0:0xE0` and reads
`0xE4`; bits 5:0 must be `0x10` (LTSSM L0). The transaction saves/disables IF
and restores/verifies the original selector; it never writes the data register.
This is serialized in BOOK8's single-BSP execution model, not an SMP/SMM lock.
Polling is bounded to 50 attempts / 10 ms intervals. No root or endpoint command
assertion is permitted without successful L0 verification, including direct
config-only calls. Host mismatch, missing ECAM, access/restore errors, delay
failure and timeout fail closed; existing firmware command bits are preserved.
Sources: AMD 780G BIOS Developer's Guide section 2.2 and coreboot 4.11
`src/southbridge/amd/rs780/{rs780.h,cmn.c}` register/access/L0 definitions.
The MSR #GP fixup remains independent. L0 is a link observation, not a
lane-gate register handshake. The driver polls the firmware-established link;
it does not implement a forced training or reset sequence.

`drivers/pcie.c` validates complete MCFG allocation sets before enabling ECAM.
Each base is bus-zero-relative; mappings cover the advertised bus range, use
the existing uncached MMIO mapper, and must fit below 4 GiB. Invalid tables,
overlapping allocations, mapping failures and unsupported offsets are logged.
CF8/CFC remains the conventional segment-zero fallback. Shared kernel/USB
word writes use actual 16-bit stores, avoiding accidental PCI Status W1C
acknowledgements.

Early enumeration records up to 512 functions and preserves firmware resources.
After networking/filesystem setup, target `0000:01:00.0` (`13c210de`) preparation
validates its bridge chain, requests PCI PM D0 upstream-first, and verifies
upstream command bits 1/2. Changed power states use a calibrated TSC delay
(10 ms, maximum 50 polls); malformed/cyclic capability lists and timeout fail
closed. BAR assignment is blocked without a verified firmware-window allocator.
The target endpoint also receives command bits 1/2 with checked readback
before BAR reads; the NVIDIA mapping helper independently checks these bits
before MMIO. Word writes preserve unrelated command bits and PCI Status.
Boot does not read or write the unverified platform MSR `0xC0012121`.
After verified RS780 L0 and PCI prerequisites, the target can be probed and
mapped without a PMC handshake. D0 and L0 do not establish lane-domain power
or guarantee an MMIO transaction completes. The exact-instruction #GP fixup
remains available to the independent `MSRGUARDTEST`. No lane-control writes
are allowed without verified masks.

Serial `PCIE1` events are queued (1024 x 128 bytes) until UDP/8080 is ready.
Normal network servicing drains them at up to 100 Hz, preserving event order
and retaining failed sends (retries slow to 1 Hz). Overflow is reported on
serial and telemetry.
Existing v2 metrics/log layout is unchanged; ordinary telemetry resumes at 1 Hz.
DHCP starts immediately after RTL8168 initialization unless `book8.net=off`.
The early v2 pulse (target command/identity) is queued until
DHCP ACK or forced-static configuration supplies a route, without blocking.
Network polling handles DHCP after RX and before telemetry/push; each poll
makes at most one DHCP TX attempt. Renewal keeps the active address until
NAK/expiry. Settings precedence is BOOK8DHCP.CFG, legacy
BOOK8NET.CFG fallback values, then DHCP defaults. The old static address is
applied only after a 30-second acquisition timeout; DHCP keeps running and
a later ACK replaces active IP, mask and gateway. Forced-static mode requires
explicit selection. The GUI network-enable toggle suspends IPv4
and resumes the chosen mode. No NIC produces an explicit skip.
`MSRGUARDTEST` checks safe EFER read, invalid-index #GP recovery, output and IF.
`tests/qemu_msr_guard_test.py build/book8-cli.iso` attaches no disks and checks
AMD15h/Intel CPU routing, the real exception path and interpreter continuation.
Its reserved x2APIC register-zero read (`0x800`) faults deterministically under
QEMU; arbitrary unknown MSR reads may instead return an emulated zero.
`tests/pcie_host_test.c` runs with ASan/UBSan during the build and checks ECAM
addressing/widths, malformed MCFG rejection, topology, D0 timing, W1C preservation,
unchanged BARs and command verification without an MSR prerequisite.
The host tests additionally emulate changing RS780 state through ECAM and
check exact host gating, L0-before-command order, selector/IF restoration,
all-ones data, training timeouts and prevention of direct-call bypass. QEMU cannot verify this platform's
PMC hardware or physical RTL8168 UDP delivery.

### ATI SB7xx HD Audio baseline

`drivers/hda.c` is a boot-time baseline for ATI `1002:4383` at `00:14.2`.
It enables PCI memory decoding and bus mastering, sets only the SB7xx snoop
bit at config `0x42` (preserving the other bits), decodes the assigned memory
BAR without resizing/reassigning it, and maps the controller uncached. PCI
config `0x44` and `0x4c` are not written. Controller reset, supported-size
CORB/RIRB setup and codec verbs use bounded polling.

The VIA codec's audio function group and connection lists are queried at boot.
The driver finds the DAC path to NIDs `0x1c` and `0x1d`, handles selector and
mixer nodes, enables D0/pin output, and retains the amplifier capabilities on
the active routes. The initial 70% level is applied across those amps; the
runtime setter maps 0–100% to each amp's advertised gain-step range, with
zero using the amp mute bit. It configures 48 kHz, stereo, 16-bit PCM with a
two-block, 128-byte-aligned BDL. `drivers/hda_stream.c` parses bounded RIFF/WAVE
files and accepts only PCM 48 kHz stereo 16-bit samples. It copies at most 2 MiB
of PCM into a persistent stream buffer, fills/refills the BDL blocks based on
LPIB, and exposes play, pause, seek, stop and status operations. The desktop
frame loop services playback; long frame stalls can produce a reported
underrun. The native `BOOK8 Player` has a path/load control, transport buttons,
timeline and volume slider.

The kernel has no IOAPIC device-IRQ dispatch path, so INTCTL, RIRB interrupt
enable, and stream interrupt delivery remain disabled. The DMA engine runs
independently after boot initialization; the desktop tray popup issues bounded
synchronous CORB/RIRB amp-control verbs only when the user changes volume or
mute. It provides a classic beveled vertical slider and mute checkbox above
the network/clock tray area, and outside presses continue through normal
window-manager handling. The browser media extension lists HTML audio/video/
source tags and media-file links; direct HTTP WAV responses up to the browser's
512 KiB response limit use the same HDA stream API. Video, compressed audio,
HTTPS/TLS, JavaScript and adaptive streaming are explicitly unsupported.
`tests/hda_host_test.c` validates runtime gain/mute, PCI preservation, VIA
mixer/selector routing, codec setup, BDL layout and stream activation;
`tests/hda_stream_host_test.c` checks WAV format validation, BDL refills,
pause/resume and end-seek stop behavior. GUI/app host tests cover player
controls, media-link discovery and HTTP WAV dispatch. Generic QEMU audio
devices do not verify the physical ATI/VIA route or audible output on the ASUS
CM1830.

### Opt-in single-index probe

`drivers/rs780_manual_audit.c` is an additional packaged API, not wired to
automatic boot/shell execution. It implements individually acknowledged
selector/discovery operations and isolated LINKCAP/LINKSTATUS requests.
`rs780_audit_ack_intent` executes exactly one guarded callback transaction,
then publishes RESULT. Only a matching session/ID/phase/record operator
attestation enables either ACK. `request_next` never performs access and
does not accept LINKCAP/LINKSTATUS; dedicated requests are required.
No driver function contains an address/discovery loop or timer. Capability
chain progression occurs only after a header RESULT ACK and a new request.
Shared `rs780_access_blocked` excludes cooperative indexed callers across
manual waits; interrupts are disabled only during the one access or ownership
change. Explicit restore/readback recovery does not turn a failed audit into
a passing preflight. The API cannot exclude SMM/raw PCI writes or recover bus hangs.

`PCIEPROBE <A2/A5..AF> [hex-mask=0]` prepares a single RS780 `00:02.0`
`E0/E4` indirect read. It sends a retained `PROBE1 INTENT` via the unchanged
292-byte v2 UDP packet to `192.168.50.17:8080`. The operator verifies collector
receipt before typing `ACK <decimal-id> INTENT`. A zero mask means **no data
write**, not read/echo-write; A6..AF semantics remain undocumented.
Nonzero masks produce a denied RESULT with no hardware access.

The bounded operation uses existing host/CPU gates and validated ECAM.
Prior L0 is deliberately not required for read-only training diagnostics;
boot MSE/BME assertions continue to require independently verified L0.
It saves the selector, selects/checks one index, reads data, then restores/
checks the original selector before re-enabling the original IF state and
submitting RESULT. Supporting selector accesses are not separate probe requests.
There is no fresh LTSSM polling loop in this path. Selector ownership is
BSP/interrupt-local, not SMM exclusion. Restoration is attempted on access
faults; restoration faults/failed readback invalidate host/link verification
and poison further indexed/manual access until reboot. The sticky lockout
cannot be cleared by re-identifying the host.

`drivers/rs780.c` owns the indexed transaction used by both boot LTSSM and the
manual controller. Boot supplies guarded load/store platform callbacks;
missing callbacks fail closed, never fall back to unguarded indexed accesses.
E0/E4 refer to PCI configuration offsets, not CPU I/O ports.
`LTSSM` aliases A5 and `LINKWIDTH` aliases A2. LINKCAP/LINKSTATUS requests use
standard PCIe capability registers, with guarded capability discovery reads
under one INTENT/RESULT handshake. Alignment/bounds, cycles, PCIe v1/v2 and
Root Port type are checked before reading base+0C or base+10; no writes occur.
The RESULT records the requested target, actual DWORD offset, raw data and
error/vector. A missing capability yields errno 19; malformed metadata errno 5.

Only successful RESULT-acknowledged samples count toward the manual
`book8_probe_link_verified` diagnostic summary: A5 L0, LINKCAP maxima and
LINKSTATUS negotiated speed/width, training inactive, optional DLL active.
Starting a replacement sample invalidates its acceptance; any pending request
or restoration lockout rejects preflight. Samples are not atomic/current-link
proof and do not authorize GPU MMIO, alter target_ready or trigger driver
continuation. No undocumented power registers are touched.

`ACK <id> RESULT` releases the pending request only; the next index must be
entered explicitly. Wrong IDs/phases or failed packet submissions cannot
advance it. `PCIEPROBE RETRY` resends the retained record, never hardware
access. Polling/input remain live between commands, without a timer/heartbeat
that advances probes. ACKs are local operator attestations, not authenticated
remote traffic; no additional host socket is bound.

The exact guarded CPL0 DWORD load/store instructions recover #GP/#PF as
vectors 13/14 while preserving failed-read output and IF; unrelated exception
sites retain fatal behavior. These guards cannot recover PCIe bus hangs or
machine checks. INTENT without RESULT identifies the indirect target but
cannot distinguish a hang in selector save/select/data/restore transactions.
`MSRGUARDTEST` includes RAM success and invalid-address read/write fault tests;
the existing AMD/Intel diskless QEMU regression requires these checks to pass.

### Console output

The kernel supports:

- serial console output via COM1
- VGA text mode output when no framebuffer is active
- framebuffer drawing for the GUI shell when a supported 32-bit framebuffer is present

The console output layer handles:

- text printing
- prompt rendering
- command echoing
- shell status messages
- error and confirmation strings

### Shell

The shell loop is in `kernel_main()` and uses the text console as the primary user interface. It supports commands such as:

- HELP / ?
- VER
- CLS
- ECHO
- DATE
- TIME
- SET
- PATH
- PROMPT
- CD / CHDIR
- DISK
- READ
- WRITE
- INSTALL
- MOUNT
- VOL
- DIR
- TYPE
- COPY
- WRITEFILE
- DEL
- FDISK
- FORMAT
- BOOK8GUI
- HALT

The shell is designed to be expanded with additional kernel-linked commands and apps.

### Prompt capabilities

The prompt format supports variables such as:

- `$P` for current path
- `$G` for `>`
- `$V` for OS version label
- `$$` for literal dollar sign

This is currently a simple shell-only feature but is useful as a model for app launchers and command prompts.

## 4. Keyboard and mouse input

### PS/2 input

The kernel supports:

- PS/2 keyboard scan decoding
- key translation to ASCII when possible
- PS/2 mouse packet handling, including four-byte IntelliMouse packets and
  wheel deltas when the device identifies as wheel-capable
- mouse coordinate clamping to viewport bounds
- mouse button state tracking

The system keeps keyboard state for shell and GUI use, including shift/caps handling and scan code translation.

### USB HID input

USB support is implemented in `src/usb.c` and exposed via `usb.h`.

Supported features:

- PCI USB controller discovery
- UHCI controller detection
- PCI I/O or memory decode and bus-master enablement with readback verification for UHCI and EHCI
- UHCI root-port reset and polling, including separate schedules for each discovered controller
- UHCI low-speed detection uses the PORTSC LSDA flag (bit 8), not the D+ line-status bit
- EHCI legacy semaphore lookup through PCI configuration space, setting OS ownership and disabling legacy SMI enables
- EHCI CONFIGFLAG set explicitly to 1 after the ownership check; optional port power-up when PPC is supported
- HCSPARAMS is read at capability offset 0x04; its separate 64-bit HCSP-PORTROUTE registers start at offsets 0x0C and 0x10 and encode one 4-bit companion number per port
- HCSP-PORTROUTE decode maps each EHCI companion number to discovered UHCI controllers, preferring functions on the same PCI device and then using other UHCI controllers as a static fallback; zero or unavailable routes use a per-port fallback assignment
- Connected low/full-speed EHCI ports are handed to an available UHCI companion; route-to-UHCI and unmapped-port counts are reported
- UHCI interrupt schedules remain active for controllers with HID endpoints; the RS bit and frame-list base are checked without a polling wait, completed HID interrupt reports are counted, and queues are disabled if their controller frame number remains stalled
- HID report polling is non-blocking. Runtime HID control requests are not issued from the prompt polling path; keyboard lock-key state is maintained in software to avoid stopping/restoring an active UHCI schedule
- control transfer flow for descriptor retrieval
- the initial device-descriptor probe uses 8 bytes at low speed and 64 bytes at full speed, resets the port before `SET_ADDRESS`, and handles short IN packets before the status stage; the control queue stays terminated while its TDs are prepared
- boot-protocol keyboard enumeration and polling
- boot-protocol keyboard and mouse enumeration/polling
- HID report-descriptor parsing for non-boot mouse interfaces, including report IDs, relative or absolute X/Y axes up to 32 bits, logical coordinate bounds, button bits, and a relative wheel field
- explicit `SET_PROTOCOL` attempts on every detected HID interface; unsupported non-boot requests are tolerated when a mouse report descriptor can be decoded
- multiple HID interfaces per USB device, subject to the fixed device/interface limit
- periodic interrupt polling for input devices
- controller counts for UHCI/OHCI/EHCI/xHCI diagnosis
- EHCI USB legacy SMI handoff and descriptor/enumeration stage counters

Current status:

- UHCI boot-HID keyboards remain supported; mouse interfaces use boot protocol or a report descriptor parsed for relative/absolute X/Y and optional wheel data
- Initialization resets and enumerates every discovered UHCI controller and builds a separate polling schedule for each controller, preserving all discovered mouse/keyboard endpoints
- UHCI port registers do not define a standard VBUS power-enable bit; BOOK8 avoids writing reserved UHCI PORTSC bits. EHCI PP is only used when the controller advertises per-port power control.
- EHCI extended-capability offsets are PCI configuration-space offsets, not MMIO offsets; the driver now reads the legacy support semaphore from PCI config space
- EHCI ports advertising connected full-/low-speed devices are assigned to the controller selected by the HCSP-PORTROUTE vector after EHCI routing is enabled; the PCI function-to-companion-number ordering remains chipset-dependent and must be checked against bare-metal SB710 telemetry
- Startup also reports the number of discovered companion mappings and when the AMD zero-route fallback was used; a mapping count does not prove a port has a connected, routable device.
- EHCI is not a native HID host driver and is used for companion routing; a polled OHCI 1.0 driver handles low/full-speed boot-HID devices on the SB710-like topology. xHCI and devices behind USB hubs remain unsupported
- Startup telemetry reports connected/reset/addressed ports, boot-HID interfaces/devices, EHCI handoffs, and descriptor/control failures
- USB hubs are not implemented
- generic HID support remains limited to the mouse fields BOOK8 recognizes: X/Y axes, up to three primary buttons, and an optional relative wheel field. Arbitrary HID layouts and generic HID keyboards are not implemented.

UHCI legacy handoff (ICH7 `USB_LEGKEY`, PCI 0xC0): `initialize_controller()` now zeroes USBINTR, then writes `0x8F00` to PCI 0xC0 (clears the legacy SMI enables and write-1-to-clear status bits) before stopping/resetting the controller, and writes `0x2000` (PIRQ enable) after the reset, matching Linux's `uhci_check_and_reset_hc`. QEMU regression passes, but QEMU has no SMM trap, so this is **unverified on the Intel G41 board**; the freeze there is still a hypothesis. If it still freezes, capture the serial/VGA output of the last UHCI line printed.

A polled OHCI 1.0 boot-HID driver in `src/ohci.c` now covers the AMD SB710 board's low/full-speed devices, with EHCI companion handoff and QEMU SB710-profile validation (sections 30-31). EHCI itself still has no native HID transfer engine and xHCI is unsupported. Bare-metal SB710 keyboard input still comes from BIOS USB-to-PS/2 emulation; native OHCI input has not been verified on silicon. See DEVREADME's 2026-11-03 OHCI follow-up for the frame-deadline and ED-rebuild fixes.

Absolute USB coordinates are clamped to their logical ranges and scaled to the current framebuffer bounds. Relative USB reports continue to update the pointer by displacement. GUI wheel events scroll the active file/output pane and emit `BOOK8-MOUSE` telemetry. These paths are exercised by the QEMU smoke test; the PS/2 four-byte negotiation still needs physical-device testing.

### System control

- `HALT` and the existing `EXIT`/`QUIT` aliases request ACPI S5 power-off.
- `REBOOT` and `RESET` first request a reset through the 8042 keyboard controller, then use an empty-IDT software interrupt to trigger a triple-fault reset if the controller does not reset the system.

### Input integration in GUI and shell

The shell accepts input from:

- serial console
- PS/2 keyboard
- USB HID keyboard

The GUI accepts input from:

- PS/2 keyboard
- USB HID keyboard
- PS/2 mouse
- USB HID mouse

The GUI uses the kernel poll layer to unify input from multiple devices.

## 5. Storage architecture

The kernel discovers and manages storage via IDE and AHCI paths.

### IDE / PIO storage

Implemented behavior:

- IDE channel discovery
- ATA IDENTIFY command execution
- LBA28/LBA48 capacity detection
- read-sector and write-sector flow
- controller-ready and data-phase wait loops
- model name extraction from IDENTIFY data

Supported capabilities:

- ATA PIO read and write using the IDE controller command block
- detection of disk model and sector count
- LBA48 sector addressing for large disks

### AHCI storage

Implemented behavior:

- PCI AHCI controller discovery
- port detection and setup
- command list and command table setup
- FIS-based transaction handling
- read/write DMA support for AHCI devices
- sector count and drive metadata extraction

This allows the OS to treat SATA disks similarly to IDE disks at the syscall/driver layer and filesystem layer.

### Disk inventory and naming

The kernel tracks disk devices in a global array with:

- device name like `0`, `1`, etc. as shown in the shell
- model string
- sector count
- LBA48 flag
- controller type (AHCI or IDE)
- port/channel metadata

This is used by `DISK`, `READ`, `WRITE`, `INSTALL`, `MOUNT`, and filesystem operations.

## 6. Filesystem subsystem

The filesystem layer is implemented in `src/filesystem.c` and exported via `filesystem.h`.

### Filesystem goals

The current filesystem layer is built to support the kernelâ€™s own needs for:

- mounting a working boot disk
- navigating root directories
- reading and writing files from FAT volumes
- formatting simple FAT partitions
- installing an OS image to a disk
- reading a limited NTFS subset

### FAT support

BOOK8 supports:

- FAT16 mounting
- FAT32 mounting
- root directory enumeration
- directory-entry reads
- long filename (VFAT/LFN) decoding
- simple file reads
- file writes for existing root files
- root file creation
- root file deletion
- FAT formatting of a disk or partition
- partition table parsing and MBR/GPT detection
- disk-level raw writes with confirmation checks

Those bullets describe the shell-level commands. The kernel API (`fs_list_directory`,
`fs_read_file`, `fs_write_file`, `fs_make_directory`, `fs_rename_entry`,
`fs_delete_entry`) also supports absolute paths and FAT subdirectories; see README.

The FAT implementation is intentionally conservative and is tuned to the projectâ€™s lab environment and test setup.

### FAT commands

The following commands are available through the shell and filesystem layer:

- `MOUNT <disk-number>`
- `DIR`
- `VOL`
- `TYPE <name>`
- `COPY <source> <destination>`
- `WRITEFILE <name> <text>`
- `DEL <name>`
- `FDISK <disk> [CREATE MBR|GPT]`
- `FORMAT <disk> [partition] FAT16|FAT32`

### NTFS support

The kernel includes limited NTFS support intentionally designed for read-heavy and carefully restricted usage. It can:

- detect an NTFS volume
- mount a recognized NTFS volume
- list root entries from the resident `$I30` index
- read files from NTFS root metadata and index blocks
- read resident and non-resident uncompressed data streams up to 1 MiB
- overwrite an existing non-resident root file only when the replacement size is identical

Important limitations:

- NTFS is not a complete implementation
- directory creation and recursive filesystem traversal are not full features
- non-root NTFS writes are not implemented
- non-resident uncompressed data only
- the implementation is intentionally restricted for safety and correctness

### Volume semantics

The mounted filesystem is exposed as drive `A:\` for the shell and GUI. The filesystem layer tracks:

- mounted volume type
- disk index
- partition index
- start LBA
- total sector count
- FAT vs NTFS classification
- current path

## 7. Installer and boot image support

The project contains a dedicated installer workflow.

### Installer payload

The installer image is produced by `build-installer-image.sh` and contains:

- BIOS boot code
- UEFI FAT32 bootloader stub
- BOOK8 kernel image
- `BIN/HEXVIEW.ELF`, `BIN/CODEEDIT.ELF`, and `BIN/NETSHARE.ELF`
- `CAT.ELF`, with its `HELLO.ELF` and `DATA/MSG.TXT` runtime dependencies
- an installer partition layout

BOOK8GUI lists these installed applications directly in Start > Programs
beside File Explorer and NoteEditor, rather than hiding them in Utilities.

The kernel detects a module named `book8-installer` from the GRUB boot entry and stores it in memory as `installer_image`.

### Installer commands

The kernel supports:

- `DISK` to show discovered local ATA/AHCI devices
- `INSTALL <disk-number>` to copy the embedded installation image to the target disk

Installation is intentionally guarded:

- target disk must be explicitly selected by the user
- the disk model and capacity are printed
- confirmation phrase must be entered exactly
- installation only proceeds after the user confirms
- the code warns clearly that this is for disposable lab disks

### Installed disk layout

The installer creates:

- an active MBR
- a FAT32 EFI System Partition
- BIOS GRUB boot code
- UEFI fallback loader at `EFI/BOOT/BOOTX64.EFI`

This build is aimed at disposable QEMU and lab disks, not production host disks.

## 8. GUI subsystem

The GUI runtime is in `src/book8gui.c` and is linked directly into the kernel alongside the rest of the kernel image.

### GUI model

BOOK8GUI is not a user-mode separate executable. It is a kernel-linked app that starts automatically after GPU initialization on graphical boots, and can be launched on demand with `BOOK8GUI` from a command-shell boot (`book8.autostart=0`) or after Start > Shutdown > Exit to Prompt.

This means:

- no independent process environment
- no loader or ELF ABI for GUI apps
- no context switching between user app and kernel app
- direct function-calling into the kernel runtime

### GUI features

The GUI currently provides:

- a 36-pixel taskbar with Start launcher, window buttons, clock and NIC indicators
- File Explorer, NoteEditor, Command Shell, Task Manager and native utility windows
- Start > Programs/Settings menus, Add/Remove Programs and categorized System Settings
- wheel scrolling, thumb dragging, window minimize/maximize/restore and focus switching with Tab
- Start > Shutdown > Exit to Prompt to return to the shell; Escape cancels dialogs/popups only
- direct command dispatch into the same shell command parser

### GUI input support

The GUI can use:

- PS/2 keyboard and mouse (including IntelliMouse wheel packets)
- UHCI boot-protocol and supported non-boot HID keyboards/mice
- polled OHCI boot-protocol mice on the SB710-like profile
- absolute USB tablets and wheel events in addition to relative mice

The GUI is designed for a 32-bit framebuffer, minimum 640x300, with a simple fixed-color UI shell.

## 9. USB and hardware support summary

### Detected controller types

The kernel reports separate counts for:

- total controllers detected
- UHCI count
- OHCI count
- EHCI count
- xHCI count

This is important when diagnosing which controllers are actually usable versus merely present.

### Supported input flow

Current practical hardware paths:

- UHCI root-hub enumeration with boot-protocol and supported non-boot report-descriptor HID
- polled OHCI 1.0 enumeration of low/full-speed boot-HID devices
- EHCI legacy handoff/companion routing (no native EHCI HID transfer engine)
- report decoding and wheel handling into shell/GUI events

### Unsupported or deferred

Current known unsupported areas:

- EHCI and xHCI native HID input
- USB hubs
- arbitrary HID layouts and generic HID keyboards
- full EHCI high-speed transfers
- bare-metal OHCI validation (currently QEMU/VirtualBox validation only)

## 10. Safety and destructive operations

The BOOK8 kernel contains intentionally destructive commands and requires explicit caution.

Examples:

- `FORMAT` can erase a selected disk or partition
- `FDISK CREATE` can replace a partition table
- `WRITE` can overwrite a sector
- `INSTALL` writes a system image to a target disk

The kernel includes confirmation prompts for critical actions and warns that these operations should be used only with disposable or lab media.

This is a strong reminder that BOOK8 is a build-and-test OS, not a host-safe or production-safe environment.

## 11. Current technical boundaries

The kernel currently does not include:

- concurrent protected userspace processes (`EXEC` loads one Ring 3 ELF at a time)
- a standard hosted libc runtime (a freestanding subset lives in `include/`/`lib/`)
- general per-process virtual-memory isolation (each ELF task gets its own PML4;
  kernel threads share the kernel address space)
- a file permissions model
- system calls in the modern Unix sense
- memory-safe process management
- general-purpose userspace sockets or a DNS resolver (bounded kernel TCP/SMB,
  DHCP and DNS display exist; see README)
- general audio codec/mixer support (PCM 48 kHz stereo 16-bit WAV playback on the
  ATI SB7xx HDA baseline exists)
- a full graphics toolkit (the kernel-linked BOOK8GUI desktop exists)

Kernel threads, experimental SMP, a single-active-task ELF loader and HDA playback are implemented; general process isolation, swap and broader device support remain intentionally deferred.

## 12. Developer guidance for writing apps against BOOK8

The most important rule: do not assume a Linux-style userspace exists.

For future app development, the appropriate kernel-first model is:

1. Add a new kernel module in `src/`.
2. Expose a command or entry point from the shell.
3. Reuse the console, filesystem, and input APIs instead of directly touching hardware.
4. Keep driver access in dedicated modules and avoid scattering I/O into app code.
5. Maintain a clean separation between service logic and UI code.
6. Treat the kernel as the service provider and runtime; `EXEC` loads one Ring 3 ELF at a time and there is no concurrent process model.

### Good app patterns

Possible future kernel-linked app categories:

- file management tools
- text editor or shell helper app
- diagnostics utility
- calculator or small command app
- framebuffer-based utility app
- serial or device debugging utility

### Bad assumptions to avoid

- assuming a standard executable loader exists
- assuming a POSIX API or libc is available
- assuming every controller is usable for input
- assuming filesystem behavior is fully general-purpose
- assuming disk writes are safe without confirmation checks
- assuming the app can bypass kernel-owned hardware control

## 13. Long-term kernel evolution direction

The current kernel is already a stable base for small, kernel-native apps. The most logical next steps are:

1. app registry and command dispatch expansion
2. a small kernel-linked app architecture
3. a clearer runtime boundary for app entry points
4. modular UI shells and app panels
5. a real process/task abstraction only after the app model proves itself

The system is already strong enough to support more kernel applications, but it should remain explicitly kernel-resident and kernel-owned until a more mature process model is designed.

## 14. Working summary for future AI and developers

BOOK8 is a compact but functional prototype operating system. It includes:

- x86-64 boot and long mode
- system shell and command parser
- VGA and serial console output
- PS/2 and USB HID input paths
- IDE/AHCI storage support
- FAT16/FAT32 filesystem support
- limited NTFS file access
- disk installer workflow
- framebuffer GUI linked into the kernel
- a reasonably clear internal app model for future kernel-native software

The key architectural fact is this: BOOK8 is already a working kernel foundation, but not yet a conventional user-mode OS. Developers writing future software for it should build in the kernel, reuse the current driver and filesystem APIs, and keep all app code aligned with the existing single-kernel runtime model.

## 15. Cross-emulator validation status (2026-11-01)

The rebuilt `build/book8.iso` was tested in QEMU, VirtualBox 7.2.22, and Bochs 2.8:

- **QEMU: PASS.** The complete smoke suite passed PS/2 and USB keyboard input, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot.
- **VirtualBox: PARTIAL PASS.** The guest reached `A:\>` and accepted `USBSTAT`. The PIIX3 VM reported 2 controllers (0 UHCI, 1 OHCI, 1 EHCI, 0 xHCI), no enumerated USB HID device, and `USB HID poll (active/completions/stalls): 0/0/0`. This validates startup and shell responsiveness, not UHCI polling. The serial capture was empty; VGA screenshots are the telemetry evidence.
- **Bochs: INCONCLUSIVE; UHCI warnings remain.** Bochs 2.8 was continued past its BIOS reset-vector debugger and run with UHCI keyboard and mouse devices enabled. The captured framebuffer remained black and no shell/USBSTAT telemetry was obtained. Its log contained 10,688 â€œWhen Active bit is setâ€ warnings, 10,688 ISO-bit warnings, and 10,687 reserved-link-bit warnings during the bounded run. These are not a clean UHCI validation and do not establish that the guest reached `A:\>`.

`src/usb.c` now uses compiler memory clobbers on port-I/O helpers to order DMA-visible schedule writes with controller register accesses, and compile-time checks guard UHCI descriptor strides. The change builds and the QEMU suite passes, but it did not remove the Bochs warnings. An experimental direct-TD periodic schedule also failed to change the Bochs result and was reverted. Do not claim a clean three-emulator pass or bare-metal validation; Bochs descriptor/polling diagnosis remains open.

## 16. Bare-metal USB follow-up and diagnostic pass (2026-11-01)

The latest user-supplied hardware report shows zero connected, reset, addressed, boot-HID, and active USB devices on both test systems. The Gigabyte/SB710 report lists 8 controllers (0 UHCI, 5 OHCI, 2 EHCI, 1 xHCI) and companion mapped/unmapped `0/1`. The G41 report lists 5 UHCI, 4 OHCI, 0 EHCI, and 1 xHCI, but its stated total of 6 does not match those subtype counts, which sum to 10. Its aggregate enumeration metrics are also zero. PS/2 keyboard input remains available.

These counters place the failure before USB descriptor requests and HID schedule polling: no port was reported connected. The report does not include raw port status, so it does not establish whether the cause is port power/wiring, firmware routing, controller detection, or a status-read issue. No speculative port-power or reset workaround was added.

`USBSTAT` now prints each discovered UHCI controller's PCI BDF, BAR, PCI command value, controller status, frame number, and last sampled status for its two root ports. PORTSC bit 0 is connection status, bit 2 is port enable, and bit 8 is low-speed indication; `FFFF` means no sample was taken. This is diagnostic instrumentation, not a behavior change.

Validation on the rebuilt ISO:

- **QEMU: PASS.** The full smoke suite passed again, including PS/2/USB keyboard input, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot. The test now also asserts that `USBSTAT` prints UHCI controller and port diagnostics.
- **VirtualBox 7.2.22: PARTIAL PASS.** The guest reached `A:\>` and accepted `USBSTAT`; telemetry again showed 0 UHCI, 1 OHCI, 1 EHCI, no enumerated HID, and poll counters `0/0/0`. The configured PIIX3 model exposes no UHCI controller, so this does not validate UHCI polling.
- **Bochs 2.8: INCONCLUSIVE.** A bounded run with a connected RFB client produced 8,032 UHCI active-status warnings and 8,032 ISO-bit warnings. No BOOK8 shell or serial telemetry was verified. The run is not a clean boot or UHCI pass.

The physical-hardware cause remains unconfirmed. Re-test `USBSTAT` on both systems and retain the new per-controller BAR/command/status/frame/PORTSC values before changing reset or companion-routing behavior.

## 17. Preserve legacy keyboard input during USB initialization (2026-11-01)

The PCI command helper only enables the requested PCI I/O, memory, and bus-master bits; it does not change i8042 ports `0x60/0x64` or chipset SMI controls. However, the prior EHCI scan immediately set the EHCI OS-owned semaphore and cleared `USBLEGCTLSTS` SMI enables for every detected EHCI controller, even on an EHCI-only machine where BOOK8 has no native EHCI HID driver or UHCI companion. That can remove firmware USB-to-i8042 emulation without creating a supported USB input path.

EHCI discovery now records controller PCI locations first and performs the legacy handoff only after the complete PCI scan confirms at least one enabled UHCI controller. With no UHCI path, EHCI ownership and routing are left with firmware, and telemetry reports how many handoffs were skipped. The controller scan order can no longer make an EHCI function be handed off before a UHCI function later in PCI enumeration is found.

After USB initialization, the kernel now reads the i8042 command byte and, with bounded waits, enables the keyboard interface and set-1 translation while preserving the other command-byte bits. It verifies those settings and reports controller configuration separately from proof of physical key input. The shell's earlier â€œkeyboard remains activeâ€ banner was based on PS/2 mouse state; that false inference is removed. PS/2 AUX bytes are also excluded from the shell keyboard path.

The report's G41 configuration lists no EHCI controller, so the EHCI handoff was not a possible cause there. The reported UHCI/OHCI/EHCI/xHCI subtype counts also conflict with its stated total; the G41 input issue therefore remains unproven pending corrected counts and key/controller telemetry.

Validation:

- **Build: PASS** with the project's `-Wall -Wextra -Werror` kernel build.
- **QEMU: PASS.** Full integration suite passed, including a new assertion for successful i8042 polling configuration and existing PS/2/USB keyboard input tests.
- **VirtualBox 7.2.22: PASS for this handoff guard.** The PIIX3 guest reached `A:\>` with its PS/2 keyboard controller configured. `USBSTAT` showed 0 UHCI, 1 OHCI, 1 EHCI, `EHCI legacy handoffs: 0`, and `handoffs skipped without UHCI: 1`; the command was entered successfully. This confirms the EHCI-only path remains firmware-owned in that VM, not bare-metal keyboard behavior.
- **Bare metal: NOT YET VERIFIED.** The changes provide a conservative fallback and improved reporting, but physical keyboard input and raw UHCI PORTSC data still need retesting on the Gigabyte and G41 systems.

## 18. Mandatory three-emulator validation of keyboard handoff build (2026-11-01)

The same rebuilt `build/book8.iso` (SHA-256 `B3AE8CBE9129A56D4F067AE7FE4E3031D0B161F3BE47C6F6A2CAA7719B2CAB2D`) was tested in all three environments:

- **QEMU: PASS.** `tests/qemu_installer_smoke.py` passed its full regression suite: PS/2 and USB keyboard, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot. The PS/2 test also confirmed the new i8042 initialization banner.
- **VirtualBox 7.2.22: PASS for shell and EHCI-only fallback.** The guest reached `A:\>` and accepted `USBSTAT`. VGA output showed PS/2 controller polling configured; controller counts `2/0/1/1/0` (total/UHCI/OHCI/EHCI/xHCI); USB enumeration `0/0/0/0/0`; EHCI legacy handoffs `0`, handoff failures `0`, and handoffs skipped without UHCI `1`; HID poll `0/0/0`. Thus this run verifies the EHCI-only firmware-ownership guard and shell input in VirtualBox, but not UHCI HID or bare-metal legacy emulation. Captures: `%TEMP%\book8-vbox-matrix-final\boot.png` and `usbstat.png`.
- **Bochs 2.8: INCONCLUSIVE; UHCI descriptor warnings reproduce.** The run loaded the ISO, initialized UHCI at device 1/function 2 with BAR4 I/O base `0xc000`, and connected Bochs' USB/PS2 keyboard and mouse. The exact repeated warnings were:

  ```text
  UHCI Core: When Active bit is set, all others in the 'Status' byte must be zero. (0x7F)
  UHCI Core: ISO bit is set...
  UHCI Core: Reserved bit in the Link Pointer is not zero.
  ```

  The captured log contains 11,594 active-status warnings, 11,594 ISO-bit warnings, and 11,593 reserved-link-bit warnings. Bochs also printed ALSA host-audio setup errors because no default sound card exists in the WSL environment. The RFB framebuffer server was reached, but this run produced no COM1 serial file and did not verify an BOOK8 prompt or USB telemetry. Do not mark this as a Bochs pass; its exact config, process output, and log are retained under `%TEMP%\book8-bochs-matrix-final\`.

The QEMU and VirtualBox results support the handoff guard's expected behavior. Bochs' repeated descriptor warnings remain a separate UHCI schedule/descriptor issue; this run does not establish that the keyboard handoff patch caused or fixed them. Bare-metal input behavior remains unverified.

## 19. UHCI descriptor encoding hardening (2026-11-01)

`src/usb.c` now centralizes UHCI link-pointer encoding: descriptor addresses are aligned down to the 16-byte pointer boundary and only the defined terminate, queue-head, and depth attributes are retained. The initial active-TD status is explicitly sanitized to clear all UHCI error-status and isochronous bits while retaining the required active, retry-count, actual-length, and optional low-speed fields. Existing descriptor structures remain 16-byte aligned, and the frame list remains 4-KiB aligned.

Validation used the rebuilt ISO (SHA-256 `9D9D13F29DAC62FE8DB0F2E2817221B2F7856D4684FA1CD8E8E63B8841B70DB5`):

- **Build: PASS.** `build.ps1` completed with the project's `-Wall -Wextra -Werror` settings.
- **QEMU: PASS.** The full `tests/qemu_installer_smoke.py build/book8.iso` suite passed for PS/2/USB keyboard input, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot.
- **VirtualBox 7.2.22: PARTIAL PASS.** The updated ISO booted to `A:\>` with the expected USB/controller telemetry visible in the startup screen. A subsequent `USBSTAT` command was not captured; this run verifies boot only, not the UHCI schedule.
- **Bochs 2.8: INCONCLUSIVE; warnings unchanged** (later resolved as a Bochs SeaBIOS image issue in section 26). A bounded 60-second run with its RFB client connected and the UHCI keyboard/mouse attached produced 16,735 each of the active-status, ISO-bit, and reserved-link warnings. The captured framebuffer remained black and the log did not verify an BOOK8 prompt or USB telemetry, so this is not a clean Bochs pass. Captures/configuration are under `%TEMP%\book8-bochs-descriptor-pass\`.

The descriptor fields generated by `td_status()` contain no ISO or error-status bits, and the static DMA structures are below 4 GiB and correctly aligned in the linked image. Therefore, the remaining Bochs warnings are not demonstrated to originate from those source initializers; the exact descriptor address consumed by Bochs remains unverified. Do not claim a clean three-emulator or bare-metal HID pass based on this run.

## 20. UHCI descriptor pool initialization and FLBASEADD ordering (2026-11-01)

The UHCI QH/TD storage is statically allocated, not dynamically allocated. `book8_usb_initialize()` now explicitly zeroes every field in the complete control-TD and interrupt-TD pools, all QHs, and every frame-list entry before assigning safe terminate links or discovering/starting controllers. Reinitialization first stops any previously discovered UHCI schedules.

Controller initialization now fills the frame list and initializes the control QH before writing its address to FLBASEADD, then starts the controller. Control transfers and periodic-schedule restoration stop the schedule before changing descriptor links/frame entries, so the controller cannot walk partially rebuilt schedule memory.

Validation: `build.ps1` completed successfully, and `tests/qemu_installer_smoke.py build/book8.iso` passed the full QEMU suite (PS/2/USB keyboard, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot). Bochs was not rerun for this targeted build; the prior warning counts are not evidence that this change fixed them.

## 21. Bounded three-emulator UHCI warning investigation (2026-11-01)

Added a bounded `USBSTS.HCHalted` confirmation after clearing UHCI `RUN`, before rebuilding control TDs/QHs, replacing interrupt frame-list entries, or resetting a controller's descriptor pools. If the controller does not report halted within the finite poll limit, the code declines to rewrite the schedule. This addresses the DMA race left by relying on a `RUN=0` write alone.

Validation used the same rebuilt ISO (SHA-256 `31C7141D5B280D8AE186398B519124E0B0EF1E25D675FCEFE2D09A153A2E53CC`):

- **QEMU: PASS.** The full regression script passed PS/2 and USB keyboard input, USB mouse GUI input, reboot, installer cancellation/install, FAT32 mount, BIOS boot, and UEFI boot.
- **VirtualBox 7.2.22: PASS to shell.** The guest reached `A:\>` and accepted `USBSTAT`. The screenshot reports controller totals `2/0/1/1/0`, zero enumerated USB devices, and HID poll `0/0/0`; this VM does not expose a UHCI HID path.
- **Bochs 2.8: FAIL / INCONCLUSIVE.** A bounded 45-second run with the USB UHCI keyboard and mouse enabled and an RFB client connected produced 4,590 occurrences each of:
  - `UHCI Core: When Active bit is set, all others in the 'Status' byte must be zero. (0x7F)`
  - `UHCI Core: ISO bit is set...`
  - `UHCI Core: Reserved bit in the Link Pointer is not zero.`

  Its framebuffer remained black and no BOOK8 shell or serial telemetry was verified. The exact run artifacts are under `%TEMP%\book8-bochs-zero-warning-halt-pass\`. QEMU and VirtualBox success do not establish that the descriptor warnings are fixed; the strict zero-warning condition remains unmet.

No further bit-clearing was applied: the TD constructor uses the UHCI active/retry/length fields, while the link encoder masks pointer attributes. Clearing spec-significant fields to silence a Bochs diagnostic without identifying the physical descriptor address would be unsafe. The next useful diagnostic must capture the exact frame-list/QH/TD physical address and four descriptor dwords Bochs passes to its checker, or establish that the warnings originate before BOOK8 owns the controller.

## 22. Temporary UHCI descriptor submission telemetry (2026-11-01)

Added bounded serial records for UHCI controller initialization, control-transfer submissions, periodic QHs, interrupt TD submissions, and FLBASEADD handoffs. Records use `UHCI-DESC` / `UHCI-FLBASE` prefixes and include a monotonically increasing sequence number, event, controller/index, physical address, and each applicable raw dword. The dump is capped at 512 records per boot. Kernel serial output is exposed to the USB module through `book8_usb_debug_serial_putc()` and remains a no-op if COM1 initialization failed.

Validation:

- Build passed; the full QEMU regression suite passed.
- Focused QEMU UHCI boot emitted descriptor snapshots to `%TEMP%\book8-qemu-descriptor-dump.log`. Example:

  ```text
  UHCI-DESC seq=00000003 event=control-submit controller=00000000 type=TD index=00000000 pa=0023D200 d0=0023D210 d1=188007FF d2=00E0002D d3=0023C200
  UHCI-DESC seq=0000000D event=control-submit controller=00000000 type=QH index=00000000 pa=0023D680 d0=00000001 d1=0023D200
  UHCI-FLBASE seq=0000000E event=control-submit controller=00000000 pa=0023E000 entry0=0023D682
  ```

- VirtualBox 7.2.22 reached `A:\>` and accepted `USBSTAT`; this VM reported no UHCI controller or HID device.
- Bochs 2.8 still emitted 20,132 active-status warnings, 20,131 ISO-bit warnings, and 20,131 reserved-link warnings during the bounded run. Its framebuffer was black and no Bochs COM1 serial file was created, so the new BOOK8 descriptor records could not be correlated with those warnings. The Bochs log does not establish that the warning-producing TD is one of BOOK8's logged descriptors.

The QEMU snapshots show aligned TD addresses, zero low-nibble TD link attributes, TD status `0x188007FF` before execution, and a QH frame-list entry with only the QH attribute set (`...682`). Do not infer the Bochs warning's source from QEMU's valid descriptors; obtain guest execution/serial output from Bochs or a Bochs physical-memory/debugger capture to tie a warning to an address.

## 23. Bounded COM1 output and Bochs telemetry follow-up (2026-11-01)

Changed kernel COM1 initialization to configure the UART directly and detect an absent port from the line-status register (`0xFF`) instead of requiring a loopback self-test. Transmitter-ready polling is now bounded to 100,000 checks with `pause`; if the UART remains unavailable, that character is dropped rather than stalling kernel execution. This keeps serial telemetry useful with emulators that do not implement UART loopback while preserving bounded behavior on hardware without a responding COM1.

Audited UHCI schedule setup and mutation order. `book8_usb_initialize()` stops previously discovered schedules before clearing descriptor memory, initializes the complete pools and terminate links before controller discovery, and `initialize_controller()` sets every frame-list entry to terminate and initializes the control QH before writing FLBASEADD and RUN. Control and periodic submissions are logged before activation, and schedule rewrites remain behind the bounded HCHalted check. No further UHCI ordering change was justified by this audit.

Validation on the rebuilt `build/book8.iso`:

- **Build and QEMU: PASS.** `build.sh` completed and `tests/qemu_installer_smoke.py build/book8.iso` passed the full regression suite, including PS/2/USB keyboard input and USB mouse GUI input.
- **Bochs 2.8: INCONCLUSIVE.** The current config enabled `com1: enabled=1, mode=file`; Bochs logged `com1 at 0x03f8` and accepted an RFB client. The debugger accepted a continue command, but no COM1 guest file was created, so no BOOK8 startup or `UHCI-DESC` telemetry was captured. The bounded run produced 5,660 active-status warnings and 5,659 each of the ISO-bit and reserved-link warnings. They cannot be attributed to BOOK8 descriptors without guest logs or a physical-memory capture.
- **VirtualBox: NOT RUN.** `VBoxManage` is not available in the current Windows or WSL command paths.

The Bochs result does not establish that the guest reached `kernel_main()`; it is not a successful boot/serial validation. Retain the process log and configuration under `%TEMP%\\book8-bochs-serial-fix-current\\` for follow-up. The updated Bochs setup notes in `README.md` distinguish the COM1 guest log from Bochs' process log and explain that the serial file is created only after guest output is written.

## 24. Bochs COM1 marker and UHCI firmware isolation (2026-11-01)

Added `BOOK8-BOOT: kernel_main entered` as the first serial message immediately after COM1 initialization and before USB or storage initialization. A focused QEMU run captured the marker, confirming the new image emits the signature on its polled COM1 path.

The Bochs tests isolate the earlier missing serial file to the attached UHCI keyboard/firmware boot path, not a COM1 address or file-backend mismatch:

- Bochs 2.8 reports `com1 at 0x03f8 irq 4 (mode: file)`, matching the kernel's `COM1` base (`0x3f8`). This driver polls LSR and does not rely on IRQ 4.
- With the UHCI controller enabled and no attached devices, Bochs created the guest COM1 file, printed the marker and BOOK8 prompt, and logged zero UHCI descriptor warnings.
- With only a USB mouse attached, Bochs created COM1 output and printed the marker and BOOK8 prompt. A 12-second capture contained 44 `UHCI-DESC` records and 10 `UHCI-FLBASE` records, with zero active-status, ISO-bit, or reserved-link warnings. The captured TD/QH records therefore can be inspected when the warning-producing USB keyboard is absent.
- With only a USB keyboard attached, the bounded run produced 15,300 occurrences each of the active-status, ISO-bit, and reserved-link warnings and no guest COM1 file or boot marker. With keyboard and mouse attached, a 15-second run produced 24,990 occurrences of each warning and no guest COM1 file. These runs do not establish that BOOK8 entered `kernel_main()`; the warnings cannot be correlated to BOOK8's descriptors and are consistent with a pre-kernel firmware/input stall.

This rules out a Bochs COM1 base alias and shows that the file backend works when the guest writes to it. The exact firmware stage that stalls with Bochs' emulated USB keyboard remains unproven, but BIOS code does not intercept the kernel's polled I/O at `0x3f8`. Use the no-device or mouse-only Bochs configuration to collect BOOK8 boot and UHCI telemetry; treat the keyboard-attached warning run as a Bochs firmware/input issue, not an BOOK8 UHCI descriptor regression. `README.md` now documents this limitation.

## 25. UHCI firmware-started controller takeover hardening (2026-11-01)

`initialize_controller()` now first clears `RUN` and waits for `HCHalted` using the existing bounded stop check before asserting the UHCI global-reset bit. It then polls for the reset bit to self-clear with a finite limit and aborts initialization if reset does not complete, rather than proceeding after a fixed delay and an unchecked reset poll. This makes kernel takeover safer when firmware left a controller running, without attributing the keyboard-attached Bochs warnings to BOOK8.

Validation:

- Build and full QEMU smoke suite passed, including PS/2/USB keyboard input, USB mouse GUI input, installer, FAT32, BIOS and UEFI checks.
- Bochs 2.8 mouse-only with the rebuilt ISO reached BOOK8 startup, emitted 44 `UHCI-DESC` and 10 `UHCI-FLBASE` records, and produced zero active-status, ISO-bit, or reserved-link warnings.
- Bochs 2.8 keyboard-only remained inconclusive for BOOK8 execution: the 12-second run produced 3,287 warnings in each category and no guest COM1 file or boot marker. This does not demonstrate a kernel-side port-reset failure; the exact pre-kernel-versus-UART-readiness ambiguity remains unresolved.

## 26. Bochs USB keyboard resolved: SeaBIOS image, not BOOK8 (2026-11-01)

**Emulator quirk (resolved, no BOOK8 source change).** Debugger traces (`pb 0x101000` / `pb 0x107180`; `kernel_main` = 0x107180, `_start` = 0x101000) showed that with a UHCI keyboard the default 128 KB Bochs SeaBIOS image (`bios.bin`, 1.17.0) never reaches GRUB: it writes the reset port `0xCF9` about every 6.43M instructions and logs the UHCI active-status/ISO-bit/reserved-link warnings from its own TD schedule. COM1 (0x3F8-0x3FF) is never touched, so earlier hypotheses of a dropped marker (UART LSR) or stale-schedule reuse by BOOK8 are ruled out. Using `romimage: file=/usr/share/seabios/bios-256k.bin` with `usb_uhci: enabled=1, port1=keyboard` reaches `kernel_main` deterministically (t=100451764 in three runs) with 0 resets and 0 warnings. `speed:low/full`, keyboard on port 2, and keyboard+mouse did not help with the default image. `README.md` documents the recommended configuration. The root cause inside the 128 KB image was inferred, not confirmed; the images were not diffed.

**Validation under the 256 KB image (Bochs 2.8 via WSL, RFB key injection):**

- **Keyboard boot: PASS.** `BOOK8-BOOT` marker and `A:\>` appeared; 502 `UHCI-DESC` and 10 `UHCI-FLBASE` records; no UHCI warnings. Boot-time `USBSTAT` enumeration 2/2/1/1/1 (connected/reset/addressed/boot-HID/active), 0 completions.
- **USB keystrokes: PASS (partial reliability).** Typed text reached the shell (`help` printed the HELP text; `usbstat` ran) and `USB HID poll (active/completions/stalls)` went from 0 completions to 1/31/0, showing the USB path as the source. Key injection was lossy because Bochs handles RFB events in simulated time; some typed commands were lost. No PS/2-only comparison was run.
- **Mouse-only and no-device regressions: PASS** with the 256 KB image.
- **BOOK8GUI: NOT VALIDATED.** The Bochs guest reported `BOOK8: VGA fallback active; framebuffer rejected or missing.` and `book8gui` printed `BOOK8GUI requires a supported 32-bit framebuffer.` GUI keyboard input under Bochs needs a different video configuration.
- **Open item:** one Bochs log line `[USBHID] DATA IN EP1: Packet Toggle indicator doesn't match Device Toggle indicator. 0 != 1` appears near the first interrupt IN (tâ‰ˆ80M), even with no keys typed. It may indicate a data-toggle initialization quirk in the BOOK8 interrupt-IN TD; not investigated and not changed.
- Artifacts: `%LOCALAPPDATA%\Temp\book8-bochs-step1\step2\` (config matrix) and `...\step3\` (`run.sh`, `rfbkeys.py`, `K10*`, `K13*`, `R1*`, `R2*`).

**Separate tracking: bare-metal silicon (unaffected by the above, still unverified).** The Bochs fix says nothing about real hardware. These remain distinct open items:

- Intel G41/ICH7 (e.g. Dell OptiPlex 380 / Vostro 230): BOOK8 does not clear the UHCI legacy register `USB_LEGKEY` (PCI offset 0xC0 trap/SMI enables) before controller reset; bit definitions must be confirmed in the ICH7 datasheet, and a fresh `USBSTAT` capture with a consistent total/subtype count is still needed.
- AMD SB710 (GIGABYTE GA-78LMT-USB3): OHCI/EHCI/xHCI are only counted, not driven, so zero enumeration there is expected until such drivers exist.
- Details and sources are in `invest.txt`. Bochs has no ICH-style SMM trap, so these items cannot be tested in it.

## 27. Bare-metal results: Intel G41 and GIGABYTE GA-78LMT-USB3 (2026-11-01)

Manual observations from the user (video and photos, transcribed; not independently re-checked). These are real-silicon results and are separate from the Bochs findings in section 26.

**Intel G41 board (Seiki display): FAIL for input.**

- Boots to the BOOK8 shell on the VGA text console (blue background, `A:\>` prompt).
- `PS/2 keyboard controller configured for polling`, `PS/2 mouse unavailable`, `USB HID keyboard unavailable. PS/2 serial fallback enabled`.
- Host controllers total/UHCI/OHCI/EHCI/XHCI = 5/4/0/1/0 (consistent: 4 + 0 + 1 + 0 = 5, unlike the earlier inconsistent report).
- Enumeration connected/reset/addressed/boot-HID/active = 0/0/0/0/0; UHCI running/schedule failures 0/0; HID completions 0; PCI command-enable failures 0; descriptor failures 0/0/0/0.
- The prompt is frozen and does not respond to the desktop keyboard.
- Interpretation (hypotheses, not verified): with EHCI present, firmware may route the root ports to EHCI, so the UHCI companion ports show no connection (0 connected). The keyboard is probably still served through BIOS legacy USB emulation (ICH7 `USB_LEGKEY`, PCI 0xC0, trapping ports 0x60/0x64 into SMM). BOOK8 resets the UHCI controllers without clearing those enables, and the i8042 it polls may be an SMM emulation that stops responding after the reset. This is the unresolved ICH7 legacy-handoff item above and is the most likely next target. BOOK8 has no OHCI/EHCI HID driver, so if the ports are routed to EHCI there is no native path either.
- Needed next: the raw per-controller/PORTSC lines and the legacy-handoff telemetry lines (lines 9-22 of the status screen were not transcribed), and a test with a real PS/2 keyboard or with BIOS "USB Legacy Support" toggled, to separate "SMM emulation broken by the UHCI reset" from "no input path at all".

**GIGABYTE GA-78LMT-USB3 rev 6.0, BIOS F2 (Sceptre display): PASS.**

- Award BIOS POST, AMD FX-4130, GRUB 2.14 menu (`BOOK8 x86-64`, `BOOK8 Installer`), shell, `BOOK8GUI` launched (files panel listing NTFS system files plus command window), `HELP` ran, and `HALT` powered the machine off.
- Keyboard input worked, but this board is SB710 (OHCI/EHCI only), which BOOK8 does not drive for HID. Input therefore must have come through the firmware's PS/2/legacy USB emulation, not from `usb.c`. This is consistent with the expectation in section 26 and does not validate the UHCI HID path. The USBSTAT counters from this run were not transcribed.
- The 32-bit framebuffer worked on this board, so BOOK8GUI is validated on real hardware here (unlike Bochs).

## 28. Pointer-input validation across QEMU, VirtualBox and Bochs (2026-11-01)

This section records the current implementation and validation boundary. The GUI polls PS/2 and USB mouse reports only while BOOK8GUI is active; cursor and wheel telemetry should therefore be checked from BOOK8GUI, not from the text shell.

**QEMU (WSL QEMU 10.2.1; machine `pc`, 256 MB, `-vga std`).** The full `tests/qemu_installer_smoke.py build/book8.iso` suite passed after the framebuffer and mouse changes, covering PS/2/USB keyboard input, relative USB mouse input, absolute USB tablet motion/click, wheel telemetry, installer and storage checks, BIOS, and UEFI. The useful USB device flags are:

```text
-device piix3-usb-uhci,id=uhci -device usb-mouse,bus=uhci.0
-device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0
```

The relative mouse updates coordinates by displacement; the tablet maps its absolute logical ranges to framebuffer bounds. The smoke test moves/clicks the tablet over BOOK8GUI's EXIT control and requires the GUI-close serial marker. The mouse case requires a nonzero `WHEEL=` report and also exercises GUI clicks. Current GRUB configuration prefers `1024x768x32`, with an `800x600x32` fallback; the kernel accepts supported RGB handoffs and uses the descriptor's byte pitch. Earlier runs documented below used `800x600x24`.

**VirtualBox 7.2.22.** A disposable `Other_64` BIOS VM with `--graphicscontroller vboxvga --vram 16 --mouse ps2`, 256 MiB RAM, and COM1 file logging booted BOOK8GUI with a 640x480x32 framebuffer. The Windows `SendInput` approach did not reach the guest reliably. `VBoxManage controlvm` has no pointer-motion command, but the installed VirtualBox COM API does: [vbox_mouse_inject.ps1](./tests/vbox_mouse_inject.ps1) locks a running VM with a shared session and calls `IConsole.Mouse.PutMouseEvent`.

The COM injector was run against the disposable VM. The guest cursor visibly moved and serial logged `BOOK8-MOUSE: X=360 Y=265 WHEEL=1 BUTTONS=0`, then `X=380 Y=275 WHEEL=-1`; this verifies relative displacement and wheel input. The optional click flag sends button state, but a GUI action from that click was not independently confirmed. `--mouse usb`, `usbtablet`, and `usbmultitouch` use VirtualBox's OHCI path, which BOOK8 does not drive; use `--mouse ps2`.

**Bochs 2.8.** Retested with the latest ISO requesting `800x600x24`, 256 KB SeaBIOS, `vgaromimage: file=/usr/share/seabios/vgabios-bochs-display.bin`, `vga: extension=vbe`, RFB, `mouse: enabled=1`, and `usb_uhci: enabled=1, port1=mouse`. It reaches `kernel_main()` and reports an active UHCI mouse, but serial explicitly reports `framebuffer tag missing; VGA fallback`. BOOK8GUI and pointer event delivery therefore remain **inconclusive**; enabling VBE in the Bochs config and requesting a GRUB graphics mode did not result in a framebuffer handoff. A supported Bochs graphics/firmware combination is still needed.

The Bochs keyboard fix (`bios-256k.bin`) and bare-metal Intel ICH7 / AMD SB710 handoff investigations remain separate: neither the emulator framebuffer limitation nor these pointer tests establish physical chipset behavior.

## 29. G41-like QEMU profile (Foxconn G41T-DM / Intel G41 + ICH7)

[tests/qemu_g41_profile.py](./tests/qemu_g41_profile.py) approximates the G41T-DM USB topology on QEMU Q35/ICH9: `-machine q35,i8042=off` (no PS/2 controller, as on the USB-only rear panel), `-cpu Penryn`, DDR2-era 1024 MB, `ich9-usb-ehci1` at 1d.7 with three EHCI-companion UHCI controllers and one standalone UHCI (4 UHCI + 1 EHCI, reported by BOOK8 as `5/4/0/1/0`, matching the board). Usage under WSL: `python3 tests/qemu_g41_profile.py build/book8.iso [ehci|uhci4|none]`.

Findings:
- Keyboard on the standalone UHCI (`uhci4`): enumeration `2/2/1/1/1`, boot HID active.
- Keyboard behind the EHCI (`ehci`, routed to a companion UHCI): enumeration `0/0/0/0/0` and `EHCI powered/companion ports: 0/0`. This matches the bare-metal G41 screen, so the G41 symptom may be EHCI companion-port routing in `ehci_route_companion_ports()` rather than (or in addition to) the SMI handoff. Cause is **undiagnosed**: adding a post-CONFIGFLAG delay, and resetting non-K-state ports before handoff, did not change it, so both experiments were reverted. Next step: log raw EHCI PORTSC values per port.
- Limits: QEMU has no ICH7, no SMM trapping and no BIOS USB legacy emulation, so the USB_LEGKEY (0xC0) freeze cannot be reproduced; the 0xC0 handoff in `usb.c` remains unverified on real hardware. Bochs and VirtualBox cannot model ICH7 UHCI+EHCI at all. AMD SB710 OHCI/EHCI HID drivers remain unimplemented.

## 30. AMD 760G + SB710 QEMU profile and the dual baseline (GIGABYTE GA-78LMT-USB3)

[tests/qemu_amd_sb710_profile.py](./tests/qemu_amd_sb710_profile.py) models what BOOK8 can see on the AMD bench on QEMU `pc` (i440fx): **no UHCI**, five `pci-ohci` functions (two behind each of two EHCI controllers, one standalone, mirroring SB7xx OHCI1-5/EHCI1-2), `Opteron_G5` (QEMU's closest Piledriver model, 8 vCPUs, 4 GiB DDR3-sized RAM), and the i8042 as the keyboard path. Variants: `none|mouse|tablet` (USB pointer attached to the OHCI side). Expected and asserted: controllers `8/0/5/2/1`, `OHCI/EHCI HID is unsupported`, USB enumeration `0/0/0/0/0`, and a typed `ver` echoed through PS/2 - the same shape as the bare-metal AMD report (USB pointers unavailable, keyboard via BIOS legacy translation).

Limits: QEMU has no 760G/SB710, no SMM and no BIOS USB-legacy trap; the injected PS/2 keystrokes only stand in for the BIOS-translated USB keyboard. The EHCI controllers are Intel `ich9-usb-ehci` devices (the only QEMU EHCI with companion support), and QEMU OHCI has 3 ports, not SB710's. The xHCI is `qemu-xhci`, standing in for the add-on USB 3.0 controller. FX-8350 was requested; the bare-metal log shows an FX-4130 - the profile uses the 8-vCPU request, which does not affect BOOK8's USB path.

**Discrepancy to resolve on hardware:** the earlier AMD report quoted `5/4/0/1/0` (4 UHCI, 1 EHCI), identical to the Intel G41 screen, yet SB710 has no UHCI. Either that figure was copied from the Intel capture or BOOK8 mis-detects; recapture the AMD screen and **resolved: the bare-metal recapture reads `8/0/5/2/1`** (0 UHCI, 5 OHCI, 2 EHCI, 1 xHCI).

**Dual baseline.** `tests/run_baseline_profiles.py <book8.iso>` (under WSL) runs the G41/ICH7 profile (standalone-UHCI keyboard must be active) and the SB710 profile (all three variants) and passes only if all hold; it currently passes. Use both profiles for emulator regression and cross-architecture telemetry. For the other emulators, only partial equivalents exist and are not yet automated: VirtualBox (`--chipset ich9`, OHCI+EHCI, `--mouse ps2`) resembles SB710's USB shape; Bochs 2.8 (`usb_uhci`/`usb_ohci`, 256 KB SeaBIOS) cannot model either ICH7 UHCI+EHCI or SB710. The G41 EHCI-companion keyboard variant (`0/0/0/0/0`) remains an open, undiagnosed gap and is deliberately excluded from the baseline assertions.

## 31. Bare-metal recapture (G41 freeze, SB710 topology) and OHCI roadmap

**Intel G41 (Seiki).** 1280x1024x32 framebuffer and the command interpreter come up. Telemetry: controllers `5/4/0/1/0`, `EHCI legacy handoffs: 1`, `companion mappings/static fallbacks: 4/1`. The machine then freezes dead right after the `A:\>` prompt is drawn. Inference (unverified): USB init completes, so the hang is in the first interactive-loop iteration - USB/PS-2 polling, or an SMI/PIRQ storm after the UHCI controllers were un-owned from the BIOS. The QEMU `ehci` companion variant (`0/0/0/0/0`) is a separate, still-undiagnosed routing question; QEMU cannot reproduce the freeze.
Next diagnostics (not yet implemented; no `usb.c` change this pass): (1) serial/screen-log raw EHCI `PORTSC` per port before and after `ehci_route_companion_ports()`, plus each UHCI `USBSTS`/`PORTSC` right after reset; (2) log PCI 0xC0 before the clear and after the PIRQ restore; (3) print a marker just before and after the first shell poll to bisect the freeze; (4) A/B test with the 0xC0 write skipped, and with UHCI polling disabled until the first key.

**AMD SB710 (GA-78LMT-USB3).** Verified telemetry: controllers `8/0/5/2/1` (0 UHCI, 5 OHCI, 2 EHCI, 1 xHCI), `OHCI/EHCI HID is unsupported`, `PS/2 mouse unavailable`, GUI pointer stuck at `MOUSE X=-1 Y=-1 W=0`, keyboard working via BIOS PS/2 legacy translation. This supersedes the earlier `5/4/0/1/0` reading. `tests/qemu_amd_sb710_profile.py` now models exactly this (adds `qemu-xhci`) and asserts `8/0/5/2/1`.

**Native OHCI HID roadmap (design only, nothing implemented).**
1. *Discovery/ownership:* OHCI is PCI class 0C03 prog-if 0x10, MMIO BAR0 (HcRevision 0x10). If HcControl.IR (bit 8) is set, take over from SMM: set HcCommandStatus.OCR (bit 3) and wait for IR to clear (bounded), then reset (HcCommandStatus.HCR) and wait (bounded, <10 us spec).
2. *Memory:* 256-byte aligned HCCA (identity-mapped, <4 GiB, DMA-safe), endpoint descriptors (16 B aligned) and transfer descriptors (16 B aligned) from a static pool; write HcHCCA, HcControlHeadED, HcBulkHeadED, then HcFmInterval (FSLargestDataPacket, FI=0x2EDF) and HcPeriodicStart (90 %).
3. *Run:* HcControl = USBOPERATIONAL | CLE | PLE (| IE later), HcRhDescriptorA gives NDP/power mode; power ports (SetPortPower), then reset each connected port (SetPortReset, wait PRSC) and note low-speed (LSDA).
4. *Enumeration:* default-control-pipe ED + TD chain (SETUP/IN/OUT) reusing the descriptor/boot-protocol code now in the UHCI path: GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, SET_PROTOCOL(boot)/SET_IDLE, then the report-descriptor parsing already used for absolute/wheel mice.
5. *Interrupt IN:* one interrupt ED (low-speed, 8-byte max packet) linked into HCCA interrupt table slot for the poll interval; poll the TD's condition code/CurrentBufferPointer in the existing polled GUI loop (no IRQ needed), re-arm by rewriting the TD and toggling DATA0/1 via the ED toggle carry; feed reports into the same HID parser as UHCI.
6. *EHCI companions:* SB710 ports with low/full-speed devices route to OHCI via PORT_OWNER, so OHCI (plus the existing `ehci_route_companion_ports`) is enough for mice; no EHCI transfer engine is needed for low-speed mice. xHCI is out of scope.
7. *Validation:* the SB710 QEMU profile with `mouse`/`tablet` on OHCI (QEMU `pci-ohci` + `usb-mouse`) is the first test target: expect enumeration >0 and `BOOK8-MOUSE` serial telemetry; final proof requires the GA-78LMT-USB3 bench.

**Register-level diagnostics (implemented).** The "next diagnostics" list above is now in `src/usb.c` and `src/kernel.c`. All reads are non-destructive (PCI config, EHCI PORTSC/CONFIGFLAG MMIO, UHCI I/O status registers). Output goes to COM1 (`DIAG ...` lines) and an 8 KB buffer that the `USBREG` shell command prints.
- Logged: PCI 0xC0 of every UHCI before/after the EHCI handoff loop and after the clear / PIRQ restore; EHCI USBLEGSUP and per-port PORTSC before/after `ehci_route_companion_ports()`; UHCI USBCMD/USBSTS/USBINTR/PORTSC1-2 at `pre-reset`, `post-reset`, `post-enum`.
- Build flags (pass via `BOOK8_DIAG_CFLAGS`, e.g. `$env:BOOK8_DIAG_CFLAGS='-DBOOK8_DIAG_SKIP_LEGSUP=1'` then `cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 2>&1"`; clear the variable and rebuild for the default):

| Flag | Default | Effect |
|---|---|---|
| `BOOK8_DIAG_REGS` | 1 | Register logging on/off |
| `BOOK8_DIAG_SKIP_LEGSUP` | 0 | **Test A:** skip both PCI 0xC0 writes (0x8F00 clear, 0x2000 PIRQ) |
| `BOOK8_DIAG_NO_UHCI_POLL` | 0 | **Test B:** `book8_usb_poll_keyboard/mouse` return false (UHCI polling disabled) |
| `BOOK8_DIAG_MARKERS` | 1 | 0 off; 1 serial `BOOK8-DIAG:` markers; 2 also prints `[tag]` on screen |

- Markers: `M1` shell loop top, `M2` prompt printed, `M3` `read_char` entered, then `S` serial / `U` USB / `P` PS/2 polled (first iteration), `K` first key. On a serial-less G41 build with `BOOK8_DIAG_MARKERS=2`, the last tag shown on screen is where the freeze occurs. Markers=2 breaks the smoke test's echo check, so keep the default for CI.
- Bare-metal plan: (1) default build, note last marker; (2) markers=2 + Test A; (3) markers=2 + Test B; (4) if the shell survives, run `USBREG` and photograph the log.
- `tests/diag_capture.py <iso> g41|sb710 [variant]` boots a QEMU profile and prints the DIAG lines.
- QEMU findings (profile artifacts, not proven G41 bugs): the `ehci` variant attaches a high-speed keyboard straight to the EHCI (PORTSC1 `00001005`), which BOOK8 cannot drive (`0/0/0/0/0`). The `companion` variant (full-speed) shows QEMU assigning the port to the companion UHCI (PORTSC1 `0x3002`), then `CONFIGFLAG=1` clears all owner bits (PORTSC1 `0x1003`) and the handoff loop skips the port on line status 00. Candidate fix, not applied: write CONFIGFLAG only when 0 and reset ports before handoff. Under QEMU the shell reaches M1..P, so the freeze is not reproducible there.
- Verification: with default flags `tests/qemu_installer_smoke.py`, `tests/run_baseline_profiles.py` and `tests/emulation_matrix.py` (9/9 PASS, same counts as section 32) are green; the flag builds were checked on the QEMU G41 profile (A: C0 stays `8F00`; B: shell still reaches the polling loop).

## 32. 3x3 emulation matrix (QEMU / Bochs / VirtualBox x SB710 / G41 / stock)

Run under WSL: `python3 tests/emulation_matrix.py build/book8.iso` (`MATRIX_ENGINES=qemu,bochs,vbox` filters; raw serial logs and `results.json` go to `MATRIX_OUT`, default `/tmp/book8-matrix`). Only QEMU can approximate the board topologies; Bochs and VirtualBox get the closest controller mix, and for them the pass criterion is only "reaches the command interpreter". Keystroke injection is attempted on QEMU only. Results (2026-11-02, current ISO, no `usb.c` changes):

| Engine | Profile | Boot | Counts T/U/O/E/X | Ideal | EHCI handoffs | Companion map/fallback | Enum c/r/a/hid/act | Input state | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| QEMU | AMD SB710 | yes | 8/0/5/2/1 | 8/0/5/2/1 | 0 | 0/0 | 0/0/0/0/0 | PS/2 mouse avail, USB kbd unavailable, `ver` echoed via PS/2 | PASS |
| QEMU | Intel G41 | yes | 5/4/0/1/0 | 5/4/0/1/0 | 1 | 4/1 | 2/2/1/1/1 | USB kbd active (standalone UHCI), PS/2 mouse unavailable | PASS |
| QEMU | Stock (pc) | yes | 1/1/0/0/0 | 1/1/0/0/0 | 0 | 0/0 | 2/2/1/1/1 | USB kbd active, PS/2 mouse avail | PASS |
| Bochs | SB710 (ohci+ehci+xhci) | yes | 7/4/1/1/1 | 8/0/5/2/1 | 0 | 4/0 | 0/0/0/0/0 | PS/2 mouse off (bochsrc), USB kbd unavailable | PASS (topology differs) |
| Bochs | G41 (uhci+ehci) | yes | 5/4/0/1/0 | 5/4/0/1/0 | 1 | 4/0 | 0/0/0/0/0 | same | PASS (topology matches) |
| Bochs | Stock (uhci) | yes | 1/1/0/0/0 | 1/1/0/0/0 | 0 | 0/0 | 0/0/0/0/0 | same | PASS |
| VirtualBox | SB710 (ohci+ehci+xhci) | yes | 3/0/1/1/1 | 8/0/5/2/1 | 0 | 0/0 | 0/0/0/0/0 | PS/2 mouse avail, USB kbd unavailable | PASS (topology differs) |
| VirtualBox | G41 (ich9, ohci+ehci) | yes | 2/0/1/1/0 | 5/4/0/1/0 | 0 | 0/0 | 0/0/0/0/0 | same | PASS (topology differs) |
| VirtualBox | Stock (piix3, ohci) | yes | 1/0/1/0/0 | 1/1/0/0/0 | 0 | 0/0 | 0/0/0/0/0 | same | PASS (no UHCI available) |

Findings:
- **No regressions** against the baseline assertions: all three QEMU profiles match their expected topology and boot to the interpreter with no stall; no engine froze at the prompt.
- **Bochs's `usb_ehci` brings four UHCI companions**, so `usb_uhci` + `usb_ehci` reproduces the G41's `5/4/0/1/0` and `EHCI legacy handoffs: 1` exactly - a useful second engine for the G41 routing question. Its enumeration is `0/0/0/0/0` only because no USB device is attached in this matrix; attach `usb_uhci: port1=keyboard`/`usb_ehci` devices to probe companion routing. Bochs SB710 reads `7/4/1/1/1` (the EHCI drags in its UHCI companions), so Bochs cannot model a UHCI-free SB710.
- **VirtualBox cannot model UHCI**, so it only represents the SB710 shape (OHCI/EHCI/xHCI) in reduced form; the G41 profile there is not a G41.
- Bochs's first matrix attempt failed only because the Debian Bochs is a debugger build and needs `-rc` with `c` to start running (stdin EOF otherwise drops to the debugger and exits); the runner now does that.
- Not verified by the matrix: the G41 post-prompt freeze (SMM/legacy USB is not emulated anywhere), the `echo` column for QEMU G41/stock (monitor `sendkey` reaches BOOK8 unreliably when a USB keyboard is present; recorded `no`, not treated as failure), and any guest input on Bochs/VirtualBox (no injection attempted).

## 33. Freestanding libc subset (include/ + lib/)

A standalone libc subset now lives in `include/` (headers) and `lib/` (sources). It needs nothing from the host: no external headers, no startup files, no syscalls.

| Piece | Contents |
|---|---|
| `include/stdint.h stdbool.h stddef.h stdarg.h limits.h` | Fixed-width ints, bool, `size_t`/`ptrdiff_t`, varargs, limits, all built on GCC's predefined `__*_TYPE__` macros |
| `lib/string.c` | `memcpy memmove memset` (`rep movsb/stosb`; `memmove` copies backwards with `std` on overlap), `memcmp memchr strlen strnlen strcpy strncpy strcmp strncmp strchr strrchr strcat strncat` |
| `lib/malloc.c` | Boundary-tag allocator: 8-byte header/footer tags, explicit doubly linked free list, first-fit, split, immediate coalescing; 16-byte aligned results; `malloc free calloc realloc` (calloc overflow-checked, realloc grows in place into a free neighbour, `free` ignores invalid or already-free pointers); `heap_init heap_get_stats heap_check` |
| `lib/stdlib.c` | `atoi atol strtol strtoul` (bases 0/2-36, clamping on overflow), `itoa utoa hex_to_str abs labs` |
| `lib/stdio.c` | `vsnprintf snprintf vsprintf sprintf`: `%d %i %u %x %X %o %c %s %p %%`, flags `- + space # 0`, width/precision (also `*`), `hh h l ll z t j`; C99 return value; no floating point. `printf puts putchar vprintf` write through `stdio_set_output()` (nothing happens until a hook is set; `vprintf` formats at most 255 chars) |
| `lib/libc_selftest.c` | 123 checks covering strings, memmove overlap, conversions, formatting and the heap |

**Heap bounds.** The default arena is a 1 MiB page-aligned array in `.bss` (`HEAP_DEFAULT_SIZE`), initialised on first `malloc`. `heap_init(base, size)` replaces it with any region: base is rounded up and the end down to 4 KiB, so heap bounds are always page-aligned. Layout: 8 byte pad, 16 byte prologue block, free blocks, 8 byte epilogue tag. The kernel image is identity-mapped by `boot.S` (first 4 GiB with 2 MiB pages), so the arena needs no extra mapping.

**Build flags.** `build.sh` compiles `lib/*.c` with `-ffreestanding -nostdinc -I include -fno-builtin -fno-strict-aliasing -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror -mno-sse...`. The last flag stops GCC rewriting loops in `memcpy`/`memset` into calls to themselves. Kernel sources are unchanged apart from `kernel.c` (see below) and keep using the compiler's own headers; only the lib objects are linked in.

**Verification.**
- Build time: `build.sh` links the lib sources with `tests/libc_host_test.c` into a static, startfile-less program using raw syscalls only, and runs it. A failing check fails the build (`libc self-test: 123 checks, 0 failed`).
- In the kernel: the `LIBTEST` shell command runs the same suite and prints the result; `python3 tests/qemu_libtest.py build/book8.iso` automates it on the QEMU SB710 profile (PASS).
- Regression: `tests/qemu_installer_smoke.py` and `tests/run_baseline_profiles.py` still pass.

**Not done / notes.** The existing kernel code still uses its own static helpers (`string_length`, `zero_memory`, ...); migrating them is optional. The heap is not thread/interrupt safe (the kernel is single-threaded and polling). The heap self-test uses the live heap and leaves it unchanged. No `errno`, floats, `qsort` or `ctype`.
