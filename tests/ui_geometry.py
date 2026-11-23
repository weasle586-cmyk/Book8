#!/usr/bin/env python3
"""Frozen BOOK8 UI geometry shared by QEMU regression tests.

These values mirror the kernel-side constants in ``src/book8gui.c`` and
``src/book8gui.h``. They are deliberately frozen: each one has a matching
``_Static_assert`` in the kernel, so a GUI layout change fails the build until
this file is updated in lockstep.

Only *geometry* is frozen here. Row *identity* (which program lives at which
row) is dynamic and comes from ``PROGRAM-MENU-ROW`` diagnostics via
``qemu_ui_lookup.py``. Tests must never combine a hardcoded row index with
these constants.
"""
# Kernel: BOOK8GUI_TASKBAR_HEIGHT / DESKTOP_TASKBAR_HEIGHT
TASKBAR_HEIGHT = 36
# Kernel: DESKTOP_TOP_HEIGHT (window work-area offset)
TOP_HEIGHT = 32
# Kernel: DESKTOP_START_WIDTH
START_WIDTH = 92
# Kernel: DESKTOP_MENU_X / DESKTOP_MENU_WIDTH
MENU_X = 4
MENU_WIDTH = 150
# Kernel: DESKTOP_MENU_ROW
MENU_ROW_HEIGHT = 20

# Menu panel chrome (menu_panel_rect / menu_row_rect in src/book8gui.c):
#   root_height = count * MENU_ROW_HEIGHT + MENU_PANEL_BORDER
#   y           = menu_top() - MENU_PANEL_TOP_GAP - panel_height
#   row centre  = y + MENU_PANEL_PADDING + row * MENU_ROW_HEIGHT
#                   + MENU_ROW_CENTER
MENU_PANEL_BORDER = 4
MENU_PANEL_TOP_GAP = 2
MENU_PANEL_PADDING = 2
MENU_ROW_CENTER = MENU_ROW_HEIGHT // 2

# Start menu: Programs, Settings, Shutdown (start_menu[] in src/book8gui.c).
MENU_ROOT_ROWS = 3
ROOT_PANEL_HEIGHT = MENU_ROOT_ROWS * MENU_ROW_HEIGHT + MENU_PANEL_BORDER  # 64
ROOT_PANEL_TOP_OFFSET = TASKBAR_HEIGHT + MENU_PANEL_TOP_GAP + ROOT_PANEL_HEIGHT  # 102

# Legacy minimum row count used when a menu-size diagnostic is unavailable.
MENU_MIN_ROWS = 9

# menu_visible_rows() cap: (menu_top() - 6) / DESKTOP_MENU_ROW.
MENU_CAPACITY_SKIP = 6
# Level-1 panel width when the Programs submenu is open (book8gui.c).
SUBMENU_WIDTH = 224

# Click targets (derived from the frozen geometry above where possible).
TASKBAR_START_X = 20
TASKBAR_START_Y_OFFSET = TASKBAR_HEIGHT // 2  # 18
PROGRAMS_MENU_X = 40
PROGRAMS_MENU_Y_OFFSET = (
    ROOT_PANEL_TOP_OFFSET - MENU_PANEL_PADDING - MENU_ROW_CENTER
)  # 90
PROGRAM_CLICK_X = MENU_X + MENU_WIDTH + 46  # 200, inside the 224px submenu


def menu_capacity(height):
    """Maximum rows menu_visible_rows() will draw at this framebuffer height."""
    return (height - TASKBAR_HEIGHT - MENU_CAPACITY_SKIP) // MENU_ROW_HEIGHT


def panel_top(height, rows):
    """Y of the Programs submenu panel for ``rows`` entries."""
    visible = min(max(MENU_MIN_ROWS, rows), menu_capacity(height))
    panel_height = visible * MENU_ROW_HEIGHT + MENU_PANEL_BORDER
    return height - TASKBAR_HEIGHT - MENU_PANEL_TOP_GAP - panel_height


def row_y(height, row, rows):
    """Centre Y of Programs submenu row ``row``."""
    return (panel_top(height, rows) + MENU_PANEL_PADDING
            + row * MENU_ROW_HEIGHT + MENU_ROW_CENTER)


def root_row_y(height, row):
    """Centre Y of a root Start-menu row (0=Programs, 1=Settings, 2=Shutdown)."""
    return (height - ROOT_PANEL_TOP_OFFSET + MENU_PANEL_PADDING
            + row * MENU_ROW_HEIGHT + MENU_ROW_CENTER)
