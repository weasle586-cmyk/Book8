#ifndef BOOK8_GUI_H
#define BOOK8_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "filesystem.h"
#include "system_config.h"
#include "mouse_input.h"

enum book8_control_page {
    BOOK8_PANEL_TIME, BOOK8_PANEL_KEYBOARD, BOOK8_PANEL_MOUSE,
    BOOK8_PANEL_DISPLAY, BOOK8_PANEL_NETWORK, BOOK8_PANEL_STORAGE,
    BOOK8_PANEL_SOUND, BOOK8_PANEL_ADMIN, BOOK8_PANEL_ABOUT,
    BOOK8_PANEL_PROGRAMS,
    BOOK8_PANEL_COUNT
};

enum book8gui_event_type {
    BOOK8GUI_EVENT_KEY,
    BOOK8GUI_EVENT_MOUSE
};

enum book8gui_key {
    BOOK8GUI_KEY_ENTER = 0x100,
    BOOK8GUI_KEY_BACKSPACE,
    BOOK8GUI_KEY_ESCAPE,
    BOOK8GUI_KEY_TAB,
    BOOK8GUI_KEY_UP,
    BOOK8GUI_KEY_DOWN,
    BOOK8GUI_KEY_LEFT,
    BOOK8GUI_KEY_RIGHT,
    BOOK8GUI_KEY_DELETE,
    BOOK8GUI_KEY_HOME,
    BOOK8GUI_KEY_END,
    BOOK8GUI_KEY_PAGE_UP,
    BOOK8GUI_KEY_PAGE_DOWN,
    BOOK8GUI_KEY_F1,
    BOOK8GUI_KEY_F2,
    BOOK8GUI_KEY_F3,
    BOOK8GUI_KEY_F4,
    BOOK8GUI_KEY_F5,
    BOOK8GUI_KEY_F6,
    BOOK8GUI_KEY_F7,
    BOOK8GUI_KEY_F8,
    BOOK8GUI_KEY_F9,
    BOOK8GUI_KEY_F10,
    BOOK8GUI_KEY_F11,
    BOOK8GUI_KEY_F12
};

/*
 * Modifier flags ride in the upper bits of event.key. They are only added to
 * Ctrl+character combinations (character is lower case) and to the navigation
 * and function keys above; plain characters, Enter, Backspace, Tab and Escape
 * never carry flags except Shift+Tab, which the window manager reserves for
 * cycling to the next application window.
 */
#define BOOK8GUI_KEY_CTRL 0x10000u
#define BOOK8GUI_KEY_SHIFT 0x20000u
/* Only ever set on Escape together with CTRL (the Ctrl+Alt+Esc hotkey). */
#define BOOK8GUI_KEY_ALT 0x40000u
#define BOOK8GUI_KEY_MASK 0xffffu


struct book8gui_event {
    enum book8gui_event_type type;
    uint32_t key;
    int32_t mouse_x;
    int32_t mouse_y;
    int16_t wheel_delta; /* Windows units: +120 per upward detent. */
    uint8_t mouse_buttons;
};

bool book8gui_decode_wheel_message(struct book8gui_event *event,
    uint32_t message, uintptr_t wparam, int32_t x, int32_t y);

struct book8gui_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct book8gui_platform;
struct WindowApp;
struct book8_nvdiag_snapshot;

typedef void (*book8gui_render_fn)(const struct book8gui_platform *platform,
    struct WindowApp *app);
typedef bool (*book8gui_event_fn)(const struct book8gui_platform *platform,
    struct WindowApp *app, const struct book8gui_event *event);

/*
 * Static description of a launchable program. The coordinator creates one
 * WindowApp per launch; create() returns that window's private state (NULL is
 * valid only for stateless classes, which set create to NULL).
 */
struct book8gui_app_class {
    const char *name;
    uint32_t default_width;
    uint32_t default_height;
    bool single_instance;
    /* Text-input apps see Tab and Escape first; return false to pass on. */
    bool wants_all_keys;
    /* Bytes of per-instance state, reported as the window's memory. */
    uint32_t state_size;
    /* Redraw once per second so live telemetry stays current. */
    bool refresh_1hz;
    void *(*create)(const struct book8gui_platform *platform);
    void (*destroy)(void *state);
    book8gui_render_fn render;
    book8gui_event_fn handle_event;
    bool (*open_path)(const struct book8gui_platform *platform,
        struct WindowApp *app, const char *path);
    bool (*can_close)(const struct book8gui_platform *platform, struct WindowApp *app);
    void (*tick)(const struct book8gui_platform *platform, struct WindowApp *app);
};

struct WindowApp {
    uint32_t id;
    const struct book8gui_app_class *cls;
    const char *title;
    struct book8gui_rect bounds;
    bool visible;
    bool focused;
    bool minimized;
    bool maximized;
    struct book8gui_rect restore_bounds;
    /* Telemetry: busy/disk accumulators reset by the 1 Hz sampler. */
    uint64_t cpu_ticks;
    uint32_t disk_read_acc;
    uint32_t disk_write_acc;
    uint32_t cpu_permille;
    uint32_t disk_read_bps;
    uint32_t disk_write_bps;
    void *state;
    book8gui_render_fn render;
    book8gui_event_fn handle_event;
};

struct book8gui_time {
    uint8_t hour;
    uint8_t minute;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

#define BOOK8GUI_STATUS_KEYBOARD 0x01u
#define BOOK8GUI_STATUS_MOUSE 0x02u
#define BOOK8GUI_STATUS_STORAGE 0x04u
#define BOOK8GUI_STATUS_NETWORK 0x08u
#define BOOK8GUI_STATUS_SOUND 0x10u

/* Optional hardware/system facts for the Task Manager. */
struct book8gui_sysinfo {
    char cpu_name[49];
    uint32_t cpu_threads;
    uint32_t cpu_mhz;
    uint64_t memory_total_kb;
    uint64_t memory_used_kb;
    uint64_t uptime_seconds;
    /* Cumulative disk counters; busy time is in platform clock ticks. */
    uint64_t disk_read_bytes;
    uint64_t disk_write_bytes;
    uint64_t disk_busy_ticks;
    uint32_t disk_count;
    uint64_t disk_total_kb;
    char disk_label[41];
};

/* Once-per-second desktop telemetry (permille = tenths of a percent). */
struct book8gui_telemetry {
    uint32_t cpu_permille;
    uint32_t disk_busy_permille;
    uint32_t disk_read_bps;
    uint32_t disk_write_bps;
    uint32_t task_count;
    uint64_t uptime_seconds;
    bool sys_valid;
    struct book8gui_sysinfo sys;
};

struct book8gui_window_info {
    uint32_t id;
    const char *title;
    uint32_t cpu_permille;
    uint32_t memory_kb;
    uint32_t disk_read_bps;
    uint32_t disk_write_bps;
    bool minimized;
};

struct book8gui_thread_info {
    uint32_t tid;
    char name[32];
    char state[8];
    uint64_t cpu_ticks;
};

struct book8gui_media_status {
    uint32_t position_ms;
    uint32_t duration_ms;
    bool loaded;
    bool playing;
    bool paused;
    bool failed;
};

struct book8gui_platform {
    uint32_t width;
    uint32_t height;
    void (*fill_rect)(uint32_t x, uint32_t y, uint32_t width,
        uint32_t height, uint32_t color);
    /* Blit RGBA pixels with byte stride and nearest-neighbor integer scale. */
    void (*draw_rgba)(uint32_t x, uint32_t y, uint32_t width,
        uint32_t height, const uint8_t *pixels, uint32_t stride,
        uint32_t scale);
    void (*draw_text)(uint32_t x, uint32_t y, const char *text,
        uint32_t foreground, uint32_t background);
    void (*pump_input)(void);
    bool (*poll_event)(struct book8gui_event *event);
    bool (*usb_mouse_active)(void);
    void (*run_command)(char *command);
    /* Optional power actions; the Start menu hides them when NULL. */
    void (*shutdown)(void);
    void (*reboot)(void);
    /* Optional hardware flags and wall-clock time. */
    uint32_t (*status_flags)(void);
    /* Monotonic NIC packet counters; false if the adapter is unavailable. */
    bool (*network_activity)(uint32_t *tx, uint32_t *rx);
    /* Audio state returns false when no controllable output is available. */
    bool (*audio_state)(uint8_t *volume, bool *muted);
    bool (*audio_set_volume)(uint8_t volume);
    bool (*audio_set_mute)(bool muted);
    bool (*media_play_wav)(const uint8_t *wav, uint32_t size);
    bool (*media_pause)(bool paused);
    void (*media_stop)(void);
    bool (*media_seek)(uint32_t position_ms);
    bool (*media_state)(struct book8gui_media_status *status);
    void (*media_service)(void);
    bool (*read_time)(struct book8gui_time *time);
    /* Optional: writes the hardware clock; false for invalid values. */
    bool (*set_time)(const struct book8gui_time *time);
    /* Optional: CPU, memory, disk and uptime facts for the Task Manager. */
    bool (*system_info)(struct book8gui_sysinfo *info);
    uint32_t (*session_uid)(void);
    uint64_t (*session_token)(void);
    /* Optional live kernel-thread snapshot for the Task Manager. */
    uint32_t (*thread_snapshot)(struct book8gui_thread_info *out,
        uint32_t capacity);
    /* Optional whole-file access; FAT parents must already exist. */
    bool (*read_file)(const char *path, void *destination,
        uint32_t capacity, uint32_t *size);
    bool (*write_file)(const char *path, const void *data, uint32_t size);
    /* Optional integer-scaled text (scale 1..4, optional faux bold). */
    void (*draw_text_scaled)(uint32_t x, uint32_t y, const char *text,
        uint32_t foreground, uint32_t background, uint32_t scale, bool bold);
    bool (*list_directory)(const char *path,
        book8_directory_entry_fn visitor, void *context);
    bool (*file_info)(const char *path, struct book8_file_info *info);
    bool (*rename_entry)(const char *path, const char *new_name);
    bool (*delete_entry)(const char *path);
    bool (*copy_to_directory)(const char *source, const char *directory, bool cut);
    /* Optional drive registry; NULL means a single A: drive. */
    uint32_t (*drive_count)(void);
    char (*drive_letter)(uint32_t index);
    bool (*drive_readable)(uint32_t index);
    /* Optional application-volume policy, separate from Explorer drive browsing. */
    bool (*program_drive_readable)(uint32_t index);
    const char *(*last_error_text)(void);
    void (*diagnostic)(const char *message);
    uint64_t (*clock_ticks)(void);
    uint32_t clock_hz;
    void (*present)(void);
    bool (*cursor_save_under)(uint32_t x, uint32_t y, uint32_t width,
        uint32_t height);
    bool (*cursor_restore_under)(void);
    void (*present_rect)(uint32_t x, uint32_t y, uint32_t width,
        uint32_t height);
    /* Persistence succeeds before shared runtime preferences are committed. */
    bool (*save_config)(const struct book8_system_config *config);
    /* Read-only, bounded text rows; bar=0..1000 or UINT32_MAX (no graph).
     * False marks the end of the page's telemetry, not a hardware action. */
    bool (*control_readout)(enum book8_control_page page, uint32_t row,
        char *text, uint32_t capacity, uint32_t *bar);
    bool (*test_tone)(uint32_t frequency);
    /* Switches the active desktop mode. On failure it must leave the current
     * mode intact; the backend owns any hardware-specific presentation path. */
    bool (*set_display_mode)(uint32_t width, uint32_t height);
    /* Optional live scanout dimensions, queried before maximizing. */
    void (*display_size)(uint32_t *width, uint32_t *height);
    /* Executes a registered ELF target; errors are reported by the backend. */
    bool (*launch_program)(const char *path);
    bool (*launch_utility)(uint32_t kind, const char *document);
    bool (*nvdiag_snapshot)(struct book8_nvdiag_snapshot *snapshot);
    bool (*read_cluster)(char drive, uint32_t cluster, void *data,
        uint32_t capacity, uint32_t *size);
    struct book8_net *(*network)(void);
    bool (*make_directory)(const char *path);
    bool (*create_file)(const char *path, const void *data, uint32_t size);
};

#define BOOK8GUI_MAX_PROGRAMS 512u
#define BOOK8GUI_PROGRAM_PATH_SIZE 512u
struct book8gui_program {
    char name[96];
    char path[BOOK8GUI_PROGRAM_PATH_SIZE];
    uint64_t allocated_bytes;
    bool system_component, allocation_valid, hidden;
    const struct book8gui_app_class *launch_class;
    char registry_key[BOOK8GUI_PROGRAM_PATH_SIZE];
};
bool book8gui_refresh_programs(void);
uint32_t book8gui_program_count(void);
bool book8gui_program_info(uint32_t index, struct book8gui_program *out);
bool book8gui_remove_program(const char *path);
const char *book8gui_program_status(void);
long book8gui_queue_utility(uint32_t kind, const char *path);
bool book8gui_utility_pending(void);
void book8gui_cancel_pending_utility(void);
bool book8gui_open_pending_utility(void);
bool book8gui_utility_installed(uint32_t kind);
bool book8gui_open_utility(uint32_t kind, const char *document);

#define BOOK8GUI_FRAME_HZ 60u
#define BOOK8GUI_INPUT_HZ 1000u
#define BOOK8GUI_MAX_EVENTS_PER_FRAME 64u
#define BOOK8GUI_TASKBAR_HEIGHT 36u
#define BOOK8GUI_MAX_WINDOWS 8u
#define BOOK8GUI_MAX_INSTANCES_PER_APP 4u

enum book8gui_menu_kind {
    BOOK8GUI_MENU_ITEM,
    BOOK8GUI_MENU_SUBMENU,
    BOOK8GUI_MENU_SHUTDOWN,
    BOOK8GUI_MENU_REBOOT,
    BOOK8GUI_MENU_EXIT
};

/* Const Start-menu table: an item launches a class, a submenu lists children. */
struct book8gui_menu_item {
    const char *label;
    enum book8gui_menu_kind kind;
    const struct book8gui_app_class *launch;
    const struct book8gui_menu_item *children;
    uint8_t child_count;
    const char *executable_path;
};

bool book8gui_run(const struct book8gui_platform *platform);
void book8gui_invalidate(void);
/* Route input exclusively to the owning window until its dialog closes. */
void book8gui_set_modal(const struct WindowApp *app, bool modal);
/* Launch another window of a class (NULL when refused or at the limit). */
struct WindowApp *book8gui_launch_class(const struct book8gui_app_class *cls);
const struct book8gui_app_class *book8gui_file_app(uint32_t index);
const struct book8gui_app_class *book8gui_app_class_by_name(const char *name);
bool book8gui_clipboard_set(const char *path, bool cut);
bool book8gui_clipboard_paste(const char *directory);
const char *book8gui_clipboard_status(void);
/* Closes the window once the current event has been handled. */
void book8gui_request_close(const struct WindowApp *app);
/* Latest 1 Hz telemetry sample. */
void book8gui_get_telemetry(struct book8gui_telemetry *out);
/* Fills out[0] with the desktop itself (id 0), then one entry per window. */
uint32_t book8gui_window_snapshot(struct book8gui_window_info *out,
    uint32_t max);
/* Closes window id once the current event has been handled. */
void book8gui_end_task(uint32_t id);
/* Lets an app attribute file transfers to its own window. */
void book8gui_note_disk(const struct WindowApp *app, uint32_t read_bytes,
    uint32_t write_bytes);
void book8gui_console_char(char value);
void book8gui_console_clear(void);

enum book8_display_request {
    BOOK8_DISPLAY_UNAVAILABLE, BOOK8_DISPLAY_STARTED, BOOK8_DISPLAY_FAILED
};
enum book8_display_request book8gui_preview_display(
    const struct book8_system_config *config);
const char *book8gui_display_status(void);

#endif
