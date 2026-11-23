# BOOK8 consolidated checkpoint

## Scope

Phase 8 (QA & Testing), Phase 1 (Immediate Stabilization), and the carried-forward
Phase 5/7 defects called out in the previous checkpoint. All changes are in the
working tree and are **not committed**.

## Completed in this pass

### Phase 1 — Immediate Stabilization

1. **Frozen UI coordinates + dynamic resolution**
   - `tests/ui_geometry.py` centralizes taskbar/menu geometry (row pitch, panel
     chrome, click targets, visible-row capacity).
   - `qemu_ui_lookup.py` derives menu Y coordinates from that module;
     `qemu_desktop_test.py`, `qemu_static_programs_test.py`,
     `qemu_programs_overflow_test.py`, `qemu_utilities_test.py`,
     `qemu_netshare_test.py` and `qemu_explorer_context_test.py` use the shared
     row lookup.
   - Matching `_Static_assert` checks in `src/book8gui.c` pin taskbar height
     (36), menu row height (20), menu x/width (4/150), desktop top (32), start
     width (92) and the three root menu rows.
2. **Frozen media preference flag bits**
   - `src/apps/book8_media_extension.h` defines `BOOK8_MEDIA_IF_*`,
     `BOOK8_MEDIA_PLUGIN_*` and `BOOK8_MEDIA_WEB_*` as `1u << N` with a
     documented bit map and reserved-bit notes.
   - `_Static_assert` checks in `book8_media_extension.c` reject renumbering.
   - `tests/book8_apps_host_test.c` verifies serialized `A:\MEDIA.CFG` bytes at
     offsets 8/12/16 remain `0x11`, `0x21`, `0x48` for the known probe, so the
     frozen 28-byte layout is unchanged.
3. **Frozen settings categories**
   - `SETTINGS_CATEGORY_COUNT == 8u` is asserted in
     `src/apps/book8_settings.c`; adding a category requires an explicit
     migration.
4. **Unified program capacity**
   - `BOOK8_MAX_PROGRAMS` in `include/syscall.h` is `512u`, and
     `BOOK8GUI_MAX_PROGRAMS` in `src/book8gui.h` aliases it.
   - `_Static_assert(BOOK8GUI_MAX_PROGRAMS == BOOK8_MAX_PROGRAMS, ...)` in
     `src/book8gui.c` prevents the kernel/GUI capacities from drifting again.

### Phase 5 — Open With regression coverage

- `tests/book8gui_host_test.c` now exercises the full Open With matrix:
  - `.TXT`, `.INI`, `.CFG`, `.LOG` resolve to NoteEditor;
  - `.C`, `.H`, `.ASM`, `.PS1` resolve to Code Editor;
  - an unknown extension produces the no-association fallback status.
- Right-click > Open With is checked for every registered handler: editor,
  player, code editor, hex viewer and PaintBook. Each case starts from a fresh
  desktop so a launched app cannot leak focus into the next assertion.

### Phase 7 — Packaging & distribution

- **Boot ELF integrity manifest**
  - `tools/gen_boot_hashes.py` generates a fixed-layout `B8H1` v1 manifest after
    the final `book8.elf` link: a 16-byte header plus 64-byte
    key/SHA-256 entries, maximum 128 entries.
  - The current build has 13 entries / 848 bytes, covering the splash asset,
    three aliases for the final kernel, six `apps.manifest` payloads and the
    three root ELFs. The generator is rerun with `--check` so a stale manifest
    fails the build.
  - GRUB loads `boot/book8-hashes.bin` as Multiboot2 module `book8-hashes`.
    `src/boot_hash.c` parses it and verifies trusted payloads with the existing
    `book8_sha256()` function before execution. This is build integrity, not
    secure boot: the kernel cannot hash a cold-loaded copy of itself, so the
    manifest is bound to the final linked kernel by the build.
  - `tests/boot_hash_host_test.c` checks valid parsing, key normalization,
    memory/file verification and truncated/corrupt manifests; the truncated
    case now passes the correct entry offset.
  - `tests/qemu_boot_hash_test.py` corrupts `HELLO.ELF` and expects
    `BOOK8-HASH: verification failed for HELLO.ELF` plus execution rejection.
- **Installer ELF selection**
  - `INSTALL` now lists packaged applications and prompts
    `Install applications ([A]ll default, [N]one, [S]elect, Q to cancel)`.
    `S` asks per application with `Y`/`N`/`Q`; all default to install.
  - Exclusion state lives in `book8_install_app_excluded[]`. Deselected ELFs
    are removed from the installed `A:\BIN` volume and omitted from registry
    seeding. `Q` cancels before any disk write.
  - `tests/qemu_installer_smoke.py --install-only` validates cancellation,
    deselection, exact packaged files, and BIOS/UEFI cold boot.
- `b8pkg` schema upgrades for manifest entries remain deferred (see gaps).

### Other defect closure

- `tests/qemu_desktop_test.py` now matches the current graphical ISO: it waits
  for the autostart `BOOK8 Sign In` modal, signs in as `default`, resolves
  program rows dynamically, pages Settings to Date/Time, uses OHCI for the
  virtual mouse, and applies the sign-in cascade offset to in-window clicks.
  It passes instead of timing out on the legacy shell-banner flow.
- `kernel/relaunch.c` preserves Multiboot2 module tags across a warm restart;
  `tests/relaunch_host_test.c` asserts module payload bytes at
  `module[8u + i]`. The splash hash key in `src/boot.c` is now `SPLASH`.
- Media bookmarks persist in a bounded `A:\MEDIA.BMK`, keyed by media path
  (8 media x 8 bookmarks). `A:\MEDIA.CFG` remains frozen at 28 bytes.
  Player's **Open Network Stream...** action now shows an explicit Open/Cancel
  confirmation before handing an HTTP stream to Web Browser.
- Player visualization is a 32-point radix-2 Q15 FFT of live HDA PCM samples,
  not tick-driven bars. The HDA module exposes two 4096-byte DMA blocks; GUI
  obtains a snapshot through `B8_HDA_IOCTL_GET_PCM_WINDOW` /
  `struct b8_hda_pcm_window`. There is no `double`, `libm` or SSE dependency,
  and the versioned `b8_hda_module_api` ABI is unchanged.
- Temporary diagnostics were removed from `src/usb.c`, `src/input.c`,
  `src/book8gui.c`, `src/apps/book8_explorer.c` and the QEMU tests.

## Verification performed

| Command | Result |
|---|---|
| `BOOK8_SKIP_RING3_SMOKE=1 bash ./build.sh` | PASS — host suite, packaging and ISO verification; `PASS: boot hash manifest is current (848 bytes)` and `boot hash host test: PASS (13 generated entries)` |
| `bash ./tests/build_matrix.sh` | PASS — 6/6 network flag combinations |
| `bash ./build.sh` | PASS — full QEMU suite (boot hash, splash, About, installer boot, utilities, netshare, Ring 3, scheduler, MSR guard, netboot, exec, relaunch, drives, storage, trusted modules, OHCI mouse) |
| `python3 tests/qemu_desktop_test.py build/book8.iso /tmp/d.img` | PASS — autostart sign-in, Start menu, focus/min/max, Settings time, USB wheel rendering, Task Manager, Explorer and zero queue drops |
| `python3 tests/qemu_static_programs_test.py build/book8.iso build/book8-data16.img build/qa-static-recheck` | PASS — cascade-dependent window steps still match after sign-in |
| `python3 tests/qemu_installer_smoke.py build/book8-cli.iso --install-only` | PASS — cancellation, app deselection, exact packaged files, BIOS/UEFI cold boot |
| `python3 tests/qemu_explorer_context_test.py build/book8-cli.iso build/book8-data16.img` | PASS — Explorer context Open, editor fallback and explicit desktop exit |
| `python3 -m py_compile tests/qemu_desktop_test.py tests/qemu_installer_smoke.py tests/qemu_explorer_context_test.py` | PASS |

## Remaining gaps (carried forward)

- `utility_path()` compile-time validation is still a comment/guard, not an
  exhaustive compile-time check.
- Strict `book8gui_program_info()` path validation remains blocked by host-test
  expectations for `A:\BIN\IMGVIEW.ELF` and index 5 system-component behavior.
- Full “all 10 utility kinds” manifest verification is not implemented.
- `b8pkg` schema upgrades for manifest entries are not implemented.
- The default (no-mode) `qemu_installer_smoke.py` still runs legacy USB
  keyboard/mouse checks that require separate driver media; use
  `--install-only` for the installer-selection flow and the build's
  `--installer-boot-only` check for GRUB handoff.
- Manual-only QEMU scripts not invoked by `build.sh` may still contain fixed
  in-window coordinates; Programs-menu geometry now derives from
  `tests/ui_geometry.py`.
- Physical hardware validation is still out of scope for this pass.

## Reproduce

```bash
# Full host suite + packaging (skip the long QEMU block)
BOOK8_SKIP_RING3_SMOKE=1 bash ./build.sh

# Feature-flag matrix
bash ./tests/build_matrix.sh

# Full QEMU suite
bash ./build.sh

# Desktop regression (requires the exact disposable target path)
cp build/book8-data16.img /tmp/d.img
python3 tests/qemu_desktop_test.py build/book8.iso /tmp/d.img

# Cascade-dependent regression
python3 tests/qemu_static_programs_test.py build/book8.iso build/book8-data16.img build/qa-static-recheck

# Installer ELF selection flow
python3 tests/qemu_installer_smoke.py build/book8-cli.iso --install-only

# Explorer context Open / editor fallback
python3 tests/qemu_explorer_context_test.py build/book8-cli.iso build/book8-data16.img

# Fuzz harness directly
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOOK8_HOST_TEST \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined \
    -Wl,--gc-sections -o /tmp/syscall_fuzz_host_test \
    tests/syscall_fuzz_host_test.c src/syscall.c
/tmp/syscall_fuzz_host_test 5000 0x424f4f4b383841 tests/fuzz
```
