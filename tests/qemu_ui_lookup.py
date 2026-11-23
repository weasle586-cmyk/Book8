#!/usr/bin/env python3
"""Dynamic Programs-menu lookup for BOOK8 QEMU regression tests.

`book8gui_rescan_programs()` emits one diagnostic per visible Programs row:

    PROGRAM-MENU-ROW: <row>|<display name>|<executable path>

Tests use this module to resolve a row by label or path and compute its click
coordinate from the live framebuffer height. Never hardcode a menu row index:
registering a new ELF legitimately shifts every row below it.
"""
import errno
import re
import time
from pathlib import Path

import ui_geometry as geo

_ROW = re.compile(r"PROGRAM-MENU-ROW:\s*(\d+)\|([^|\r\n]+)\|([^\r\n]*)")
_COUNT = re.compile(r"Programs indexed:\s*(\d+)")
_FIRST_ROW = "PROGRAM-MENU-ROW: 0|"


def read_serial(path):
    """Read a QEMU serial log without failing on transient ENODATA."""
    try:
        path = Path(path)
        return path.read_text(errors="replace") if path.exists() else ""
    except OSError as error:
        if error.errno != errno.ENODATA:
            raise
        return ""


def wait_for(path, process, expected, timeout=60):
    """Poll a serial log until `expected` appears or QEMU exits."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited while waiting for {expected!r}")
        if expected in read_serial(path):
            return read_serial(path)
        time.sleep(0.1)
    raise TimeoutError(f"Timed out waiting for {expected!r}")


def program_rows(serial):
    """Return {row: (label, path)} for the most recent Programs scan.

    A rescan can happen more than once (for example after an app is removed),
    so only the last contiguous block is used; stale rows from an earlier scan
    must not be resolved.
    """
    log = read_serial(serial)
    if _FIRST_ROW not in log:
        return {}
    block = _FIRST_ROW + log.rsplit(_FIRST_ROW, 1)[1]
    rows = {}
    expected = 0
    for match in _ROW.finditer(block):
        row = int(match.group(1))
        if row != expected:
            break
        rows[row] = (match.group(2).strip(), match.group(3).strip())
        expected += 1
    return rows


def await_program_rows(serial, process, timeout=60):
    """Wait for the GUI to publish menu rows, then return the mapping."""
    wait_for(serial, process, _FIRST_ROW, timeout=timeout)
    rows = program_rows(serial)
    if not rows:
        raise AssertionError("Programs menu rows were not reported by book8gui")
    return rows


def menu_row_count(serial, process=None):
    """Number of rows in the Programs submenu (dynamic, sorted registry)."""
    rows = await_program_rows(serial, process) if process else program_rows(serial)
    if rows:
        return len(rows)
    log = read_serial(serial)
    match = _COUNT.search(log)
    if not match:
        raise AssertionError("Programs menu size was not reported")
    return max(geo.MENU_MIN_ROWS, int(match.group(1)))


def resolve_row(serial, label=None, path=None, process=None):
    """Resolve one Programs row by display label or installed ELF path."""
    rows = await_program_rows(serial, process) if process else program_rows(serial)
    if not rows:
        raise AssertionError(
            f"Cannot resolve Programs row {label!r} {path!r}: no row diagnostics")
    if label:
        target = label.casefold()
        for row, (name, _) in rows.items():
            if name.casefold() == target:
                return row
        for row, (name, _) in rows.items():
            if target in name.casefold():
                return row
    if path:
        target = path.casefold()
        for row, (_, row_path) in rows.items():
            if row_path.casefold() == target:
                return row
        for row, (_, row_path) in rows.items():
            if target and target in row_path.casefold():
                return row
    available = ", ".join(f"{row}={name}" for row, (name, _) in sorted(rows.items()))
    raise AssertionError(f"Programs entry {label!r} {path!r} not found; menu: {available}")


def panel_top(height, rows):
    """Y of the Programs submenu panel; see tests/ui_geometry.py."""
    return geo.panel_top(height, rows)


def row_y(height, row, rows):
    """Centre Y of a Programs submenu row, mirroring menu_row_rect()."""
    return geo.row_y(height, row, rows)


def root_row_y(height, row):
    """Centre Y of a root Start-menu row, mirroring menu_row_rect(level=0)."""
    return geo.root_row_y(height, row)


def click_program(monitor, serial, height, label=None, path=None, process=None):
    """Open Start -> Programs and click the requested entry by identity."""
    rows = await_program_rows(serial, process) if process else program_rows(serial)
    if not rows:
        raise AssertionError("Programs menu rows unavailable for dynamic click")
    row = resolve_row(serial, label=label, path=path, process=process)
    monitor.click(geo.TASKBAR_START_X, height - geo.TASKBAR_START_Y_OFFSET)
    monitor.click(geo.PROGRAMS_MENU_X, height - geo.PROGRAMS_MENU_Y_OFFSET)
    monitor.click(geo.PROGRAM_CLICK_X, row_y(height, row, len(rows)))
    return row
