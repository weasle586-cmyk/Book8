---
name: book8-qemu-validation
description: "Select and run focused BOOK8 build and QEMU regression checks, interpret serial output and test results, and keep storage or installer tests confined to disposable virtual disks. Use when validating changes to BOOK8 kernel, drivers, filesystem, GUI, input, syscalls, boot, or installer."
---

# BOOK8 Build and QEMU Validation

Use this workflow after changing BOOK8 code or when diagnosing a QEMU regression. Select the narrowest test that exercises the changed behavior; do not run every emulator matrix by default.

## 1. Inspect the change and choose validation

1. Identify changed source files and the behavior they affect.
2. Check the current test scripts and their usage before invoking them. Use `README.md`, `DEVREADME.md`, and `KERNEL_NOTES.md` for current commands, hardware profiles, expected output, and known limitations; the mapping below is a starting point, not a substitute for checking recent changes.
3. Use the smallest relevant check:

| Change area | Focused validation |
|---|---|
| Freestanding kernel build or core integration | `.\build.ps1` from PowerShell (builds through WSL Ubuntu; `build.sh` also runs host-side libc, ATA DMA, and framebuffer checks). |
| Storage drivers, DMA, block layer, or write behavior | `python3 tests/qemu_storage_test.py build/book8.iso build/book8-data16.img [generic\|target-pc\|target-q35]`. Use `python3 tests/storage_matrix.py build/book8.iso build/book8-data16.img` only when cross-profile storage coverage is needed. |
| Filesystem, ELF loader, process execution, or syscalls | `python3 tests/qemu_exec_test.py build/book8.iso build/book8-data16.img`, and `build/book8-data32.img` when FAT32 coverage is relevant. |
| Ring 3 transition or syscall smoke path | `python3 tests/qemu_ring3_test.py build/book8.iso`. |
| Freestanding libc behavior | `python3 tests/qemu_libtest.py build/book8.iso`. |
| OHCI HID or SB710-like USB mouse path | `python3 tests/qemu_ohci_mouse_test.py build/book8.iso`. The profile configures the EHCI/OHCI companion topology and attaches `-device usb-mouse,bus=ehci1.0,port=1`; validate its serial telemetry and `INPUTSTAT` counters. Do not replace the explicit profile with generic `-usb` flags. |
| Framebuffer, drawing syscalls, PS/2 or UHCI mouse | Fast preflight: `.\build.ps1` (runs the host framebuffer primitive test as part of `build.sh`). QEMU rendering/input check: `python3 tests/qemu_graphics_input_smoke.py build/book8.iso build/book8-data16.img`. It uses `-vga std`; its UHCI case configures `-device piix3-usb-uhci,id=uhci` and `-device usb-mouse,bus=uhci.0`. It checks actual firmware framebuffer handoff and a rendered Ring 3 pixel, and boots the ISO; it does not force a fixed resolution or provide a bootless QEMU rendering test. |
| GUI click behavior | Check `tests/qemu_gui_click_test.py` usage, then run it with the ISO and any required arguments. |
| Installer, boot input, cancellation, or cold boot | `python3 tests/qemu_installer_smoke.py build/book8.iso`. This test exercises a disposable virtual install disk. |
| Emulator chipset/profile compatibility | `python3 tests/run_baseline_profiles.py build/book8.iso`; use `tests/emulation_matrix.py` only when the broader emulator matrix is requested or the change warrants it. |

Run Linux test scripts in the repository's WSL Ubuntu environment, as documented in `README.md`. On Windows, use `.\build.ps1` for the supported WSL build entry point. Do not assume a QEMU test has run merely because the build succeeded.

## 2. Protect disks and host data

- Use only temporary/disposable disk-image files created for the test or disposable copies under `build/` (for example `build/book8-data16.img`). Never pass a physical disk, host device, raw partition, or valuable image as a QEMU drive target, including for `WRITE`, `FORMAT`, `INSTALL`, or destructive self-tests.
- Before launching QEMU, verify every disk argument is an intended regular image file and disposable. If a target is a device/partition, its nature is unclear, or the test could touch valuable data, stop and ask for a disposable image; do not proceed.
- The graphics and exec smoke tests attach the data image with QEMU `snapshot=on`; still use a disposable image and verify the test's actual arguments. The storage test creates temporary IDE/AHCI scratch images marked `BOOK8SCRATCH`, intentionally writes to them, and checks their contents. Never substitute a physical target or reuse valuable image contents.
- Before running a test, inspect its setup if it creates, formats, attaches, or modifies images. Prefer tests that create their own temporary scratch images and clean them up.
- Do not broaden to real hardware, VirtualBox, Bochs, or full matrices without a reason tied to the change or an explicit request.
- Stop at QEMU validation. Do not request or perform physical bare-metal flashing, installation, or destructive hardware testing unless the user explicitly asks for it.

## 3. Decide whether validation passed

- Require the test process to exit successfully and report its own `PASS` result or expected success markers. A VM booting or producing a serial log alone is not a pass. Match the test's own assertions; useful exact markers include:
  - OHCI mouse: `USB HID mouse active`, an `OHCI started/ports/connected/HID/hubs/reports/errors:` line with the expected enumeration counts, and `PASS: OHCI mouse test`. `INPUTSTAT` must show `USB_REPORTS > 0`, `USB_QUEUED > 0`, and `USB_QUEUE_DROPS=0`.
  - Graphics/input: `BOOK8 framebuffer handoff: <width>x<height>x<24|32>`, `HELLO: framebuffer syscalls ok`, and `PASS: QEMU graphics/input smoke tests`; the screenshot must match the handoff dimensions and the tested pixel `(13, 37, 251)`. The test also requires mouse input and zero queue drops.
  - Storage: `STORAGE: ... selftest PASS` / `write selftest PASS` lines and `PASS: storage test [<profile>]`; the process fails on missing/forbidden output or scratch-image guard/pattern mismatches.
  - Ring 3, libc, and exec: require the test process's `PASS: Ring 3 smoke test`, `PASS: in-kernel libc self-test`, or `PASS: exec test` output, respectively.
- Compare serial output with the test's assertions and documented expectations. Treat missing markers, forbidden `FAIL`/error output, timeouts, unexpected early QEMU exit, and image guard/pattern mismatches as failures. Some scripts intentionally terminate QEMU after collecting results; judge success by the script's exit status and assertions, not by whether the guest shuts itself down.
- For driver and hardware-profile tests, distinguish a BOOK8 failure from a known emulator limitation only when the test and project notes support that distinction. Do not infer success from a missing log marker.
- If a test cannot run because a dependency, emulator, or WSL environment is missing, report that explicitly; do not silently substitute an unrelated test.
- Report the exact command, pass/fail result, relevant serial markers, and where retained logs or screenshots were written. State any skipped coverage and why. Do not continue to physical bare-metal flash or install instructions unless explicitly requested.
