#include "kernel_internal.h"

/* GUI callbacks, desktop launch, windows and media services. */

bool gui_cursor_save_under(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    gui_cursor_under_valid = false;
    if (fbdev.draw == NULL || width == 0u || height == 0u
        || width > GUI_CURSOR_WIDTH || height > GUI_CURSOR_HEIGHT
        || x >= fbdev.width || y >= fbdev.height
        || width > fbdev.width - x || height > fbdev.height - y) {
        return false;
    }

    uint32_t row_bytes = width * fbdev.bytes;
    for (uint32_t row = 0u; row < height; ++row) {
        const uint8_t *source = fbdev.draw + (size_t)(y + row) * fbdev.pitch
            + (size_t)x * fbdev.bytes;
        uint8_t *destination = gui_cursor_under + (size_t)row * row_bytes;
        for (uint32_t byte = 0u; byte < row_bytes; ++byte) {
            destination[byte] = source[byte];
        }
    }
    gui_cursor_under_x = x;
    gui_cursor_under_y = y;
    gui_cursor_under_width = width;
    gui_cursor_under_height = height;
    gui_cursor_under_valid = true;
    return true;
}

bool gui_cursor_restore_under(void)
{
    if (!gui_cursor_under_valid || fbdev.draw == NULL
        || gui_cursor_under_x >= fbdev.width
        || gui_cursor_under_y >= fbdev.height
        || gui_cursor_under_width > fbdev.width - gui_cursor_under_x
        || gui_cursor_under_height > fbdev.height - gui_cursor_under_y) {
        gui_cursor_under_valid = false;
        return false;
    }

    uint32_t row_bytes = gui_cursor_under_width * fbdev.bytes;
    for (uint32_t row = 0u; row < gui_cursor_under_height; ++row) {
        uint8_t *destination = fbdev.draw
            + (size_t)(gui_cursor_under_y + row) * fbdev.pitch
            + (size_t)gui_cursor_under_x * fbdev.bytes;
        const uint8_t *source = gui_cursor_under + (size_t)row * row_bytes;
        for (uint32_t byte = 0u; byte < row_bytes; ++byte) {
            destination[byte] = source[byte];
        }
    }
    gui_cursor_under_valid = false;
    return true;
}

void gui_present_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
    if (x <= INT32_MAX && y <= INT32_MAX) {
        fb_present_rect(&fbdev, (int32_t)x, (int32_t)y, width, height);
    }
}

void gui_run_command(char *command)
{
    run_command(command);
}

void gui_shutdown(void)
{
    console_write("BOOK8 shutdown requested.\n");
    acpi_shutdown();
}

void gui_reboot(void)
{
    console_write("BOOK8 reboot requested.\n");
    reboot_system();
}

uint32_t gui_status_flags(void)
{
    uint32_t flags = 0u;
    if (ps2_keyboard_controller_ready || book8_usb_keyboard_active()) {
        flags |= BOOK8GUI_STATUS_KEYBOARD;
    }
    if (ps2_mouse_available || book8_usb_mouse_active()) {
        flags |= BOOK8GUI_STATUS_MOUSE;
    }
    if (disk_count != 0u) {
        flags |= BOOK8GUI_STATUS_STORAGE;
    }
    if (pci_network_present) {
        flags |= BOOK8GUI_STATUS_NETWORK;
    }
    if (pci_sound_present) {
        flags |= BOOK8GUI_STATUS_SOUND;
    }
    return flags;
}

bool gui_network_activity(uint32_t *tx, uint32_t *rx)
{
    if (!network_ready)
        return false;
    *tx = rtl_nic.tx_packets;
    *rx = rtl_nic.rx_packets;
    return true;
}

uint32_t gui_month_days(uint32_t year, uint32_t month)
{
    static const uint32_t days[] = {31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u};
    return days[month - 1u] + (month == 2u && year % 4u == 0u
        && (year % 100u != 0u || year % 400u == 0u) ? 1u : 0u);
}

bool gui_shift_time(struct book8gui_time *time, int64_t minutes)
{
    if (time->month < 1u || time->month > 12u
        || time->day < 1u || time->day > gui_month_days(time->year, time->month)) {
        return false;
    }
    minutes += (int64_t)time->hour * 60 + time->minute;
    while (minutes < 0) {
        minutes += 1440;
        if (--time->day == 0u) {
            if (--time->month == 0u) { time->month = 12u; --time->year; }
            time->day = (uint8_t)gui_month_days(time->year, time->month);
        }
    }
    while (minutes >= 1440) {
        minutes -= 1440;
        if (++time->day > gui_month_days(time->year, time->month)) {
            time->day = 1u;
            if (++time->month > 12u) { time->month = 1u; ++time->year; }
        }
    }
    time->hour = (uint8_t)(minutes / 60);
    time->minute = (uint8_t)(minutes % 60);
    return time->year >= 1980u && time->year <= 2099u;
}

bool gui_read_time(struct book8gui_time *time)
{
    struct clock_time now = read_clock();
    time->hour = now.hour;
    time->minute = now.minute;
    time->day = now.day;
    time->month = now.month;
    time->year = now.year;
    int64_t minutes = (int32_t)book8_config_value(BOOK8_CFG_TIMEZONE) * 15 - 720;
    if (software_clock_active) {
        *time = software_clock;
        if (tsc_hz != 0u) {
            minutes += (int64_t)((read_tsc() - software_clock_start) / tsc_hz / 60u);
        }
    }
    return gui_shift_time(time, minutes);
}

void cmos_write(uint8_t register_index, uint8_t value)
{
    outb(0x70u, register_index);
    outb(0x71u, value);
}

bool gui_set_time(const struct book8gui_time *time)
{
    static const uint8_t month_days[12] =
        {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (time->hour > 23u || time->minute > 59u || time->month < 1u
        || time->month > 12u || time->year < 1980u || time->year > 2099u) {
        return false;
    }
    bool leap = (time->year % 4u == 0u && time->year % 100u != 0u)
        || time->year % 400u == 0u;
    uint8_t days = month_days[time->month - 1u];
    if (time->month == 2u && leap) {
        days = 29u;
    }
    if (time->day < 1u || time->day > days) {
        return false;
    }
    struct book8gui_time utc = *time;
    if (!gui_shift_time(&utc,
        720 - (int32_t)book8_config_value(BOOK8_CFG_TIMEZONE) * 15)) {
        return false;
    }
    if (book8_config_value(BOOK8_CFG_RTC_SYNC) == 0u) {
        if (tsc_hz == 0u) { return false; }
        software_clock = utc;
        software_clock_start = read_tsc();
        software_clock_active = true;
        return true;
    }
    time = &utc;
    software_clock_active = false;

    uint8_t mode = cmos_read(0x0bu);
    uint8_t second = 0u;
    uint8_t minute = time->minute;
    uint8_t hour = time->hour;
    uint8_t day = time->day;
    uint8_t month = time->month;
    uint8_t year = (uint8_t)(time->year % 100u);
    uint8_t century = (uint8_t)(time->year / 100u);
    if ((mode & 0x02u) == 0u) {
        bool pm = hour >= 12u;
        hour = (uint8_t)(hour % 12u);
        if (hour == 0u) {
            hour = 12u;
        }
        if (pm) {
            hour |= 0x80u;
        }
    }
    if ((mode & 0x04u) == 0u) {
        uint8_t high = (uint8_t)(hour & 0x80u);
        hour &= 0x7fu;
        hour = (uint8_t)(((hour / 10u) << 4u) | (hour % 10u) | high);
        minute = (uint8_t)(((minute / 10u) << 4u) | (minute % 10u));
        day = (uint8_t)(((day / 10u) << 4u) | (day % 10u));
        month = (uint8_t)(((month / 10u) << 4u) | (month % 10u));
        year = (uint8_t)(((year / 10u) << 4u) | (year % 10u));
        century = (uint8_t)(((century / 10u) << 4u) | (century % 10u));
    }
    /* SET bit freezes the update cycle while the fields are rewritten. */
    cmos_write(0x0bu, (uint8_t)(mode | 0x80u));
    cmos_write(0x00u, second);
    cmos_write(0x02u, minute);
    cmos_write(0x04u, hour);
    cmos_write(0x07u, day);
    cmos_write(0x08u, month);
    cmos_write(0x09u, year);
    cmos_write(0x32u, century);
    cmos_write(0x0bu, (uint8_t)(mode & 0x7fu));
    return true;
}

bool gui_usb_mouse_active(void)
{
    return book8_usb_mouse_active();
}

bool gui_list_files(const char *path,
    book8_directory_entry_fn visitor, void *context)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_list_directory(path, visitor, context);
}

bool gui_file_info(const char *path, struct book8_file_info *info)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_file_info(path, info);
}

bool gui_rename_entry(const char *path, const char *new_name)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_rename_entry(path, new_name);
}

bool gui_delete_entry(const char *path)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_delete_entry(path);
}

bool gui_copy_to_directory(const char *source, const char *directory,
    bool cut)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ
        | BOOK8_SESSION_WRITE)
        && fs_copy_to_directory(source, directory, cut);
}

bool gui_make_directory(const char *path)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_make_directory(path);
}

bool gui_create_file(const char *path, const void *data, uint32_t size)
{
    return book8_session_authorize_current(BOOK8_SESSION_WRITE)
        && fs_create_file(path, data, size);
}

bool gui_read_cluster(char drive, uint32_t cluster, void *data,
    uint32_t capacity, uint32_t *size)
{
    return book8_session_authorize_current(BOOK8_SESSION_READ)
        && fs_read_cluster(drive, cluster, data, capacity, size);
}

uint32_t gui_session_uid(void)
{
    struct book8_session_info info;
    return book8_session_current(&info) ? info.uid : UINT32_MAX;
}

uint64_t gui_session_token(void)
{
    struct book8_session_info info;
    return book8_session_current(&info) ? info.token : 0u;
}

uint32_t gui_drive_count(void)
{
    return fs_drive_count();
}

char gui_drive_letter(uint32_t index)
{
    return fs_drive_letter(index);
}

const char *gui_last_error_text(void)
{
    return fs_error_text(fs_last_error());
}

void gui_diagnostic(const char *message)
{
    while (*message != '\0') {
        control_log_char(*message);
        serial_putc(*message++);
    }
    control_log_char('\n');
}

void gui_apply_registry_config(struct book8_system_config *config)
{
    static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    uint32_t value;
    char theme[BOOK8_REGISTRY_VALUE_MAX];
    if (reg_get_u32(path, "KeyRate", &value)
        && value >= book8_config_ranges[BOOK8_CFG_REPEAT_RATE].min
        && value <= book8_config_ranges[BOOK8_CFG_REPEAT_RATE].max) {
        config->values[BOOK8_CFG_REPEAT_RATE] = value;
    }
    if (reg_get_u32(path, "KeyDelay", &value) && value >= 250u
        && value <= 1000u && value % 250u == 0u) {
        config->values[BOOK8_CFG_KEY_DELAY] = value / 250u - 1u;
    }
    if (reg_get_u32(path, "MouseSpeed", &value)
        && value >= book8_config_ranges[BOOK8_CFG_POINTER_SPEED].min
        && value <= book8_config_ranges[BOOK8_CFG_POINTER_SPEED].max) {
        config->values[BOOK8_CFG_POINTER_SPEED] = value;
    }
    if (reg_get_u32(path, "DblClickMs", &value)
        && value >= book8_config_ranges[BOOK8_CFG_DOUBLE_CLICK].min
        && value <= book8_config_ranges[BOOK8_CFG_DOUBLE_CLICK].max) {
        config->values[BOOK8_CFG_DOUBLE_CLICK] = value;
    }
    if (reg_get_str(path, "Theme", theme, sizeof(theme))) {
        if (strcmp(theme, "Dark") == 0) { config->values[BOOK8_CFG_DARK] = 1u; }
        else if (strcmp(theme, "Light") == 0) { config->values[BOOK8_CFG_DARK] = 0u; }
    }
    if (reg_get_u32(path, "Accent", &value) && value < 8u) {
        config->values[BOOK8_CFG_ACCENT] = value;
    }
    if (reg_get_u32(path, "Volume", &value) && value <= 100u
        && value % 5u == 0u) {
        config->values[BOOK8_CFG_VOLUME] = value;
    }
    if (reg_get_u32(path, "Tone", &value)
        && value >= book8_config_ranges[BOOK8_CFG_TONE].min
        && value <= book8_config_ranges[BOOK8_CFG_TONE].max) {
        config->values[BOOK8_CFG_TONE] = value;
    }
    if (!book8_config_valid(config)) {
        book8_config_defaults(config);
    }
}

void gui_load_config(void)
{
    if (config_loaded) { return; }
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    uint32_t size = 0u;
    struct book8_system_config loaded;
    bool seed_defaults = false;
    if (!fs_read_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes), &size)) {
        int error = fs_last_error();
        snprintf(config_status, sizeof(config_status), "%s",
            error == BOOK8_FS_ERR_NOT_FOUND ? "No config file; defaults active."
            : "Config read failed; defaults active.");
        seed_defaults = error == BOOK8_FS_ERR_NOT_FOUND;
        book8_config_defaults(&loaded);
        (void)book8_config_commit(&loaded);
    } else if (!book8_config_decode(&loaded, bytes, size)) {
        snprintf(config_status, sizeof(config_status), "Invalid config file; defaults active.");
        book8_config_defaults(&loaded);
        (void)book8_config_commit(&loaded);
        seed_defaults = true;
    } else {
        (void)book8_config_commit(&loaded);
        snprintf(config_status, sizeof(config_status), "Loaded A:\\BOOK8.CFG.");
    }
    gui_apply_registry_config(&loaded);
    (void)book8_config_commit(&loaded);
    console_write("BOOK8-CONFIG: ");
    console_write(config_status);
    console_write("\n");
    char config_diagnostic[160];
    snprintf(config_diagnostic, sizeof(config_diagnostic),
        "BOOK8-CONFIG: %s\n", config_status);
    gui_diagnostic(config_diagnostic);
    console_write("BOOK8-CONFIG: active resolution=");
    print_unsigned(book8_config_value(BOOK8_CFG_RESOLUTION));
    console_write(" pointer-speed=");
    print_unsigned(book8_config_value(BOOK8_CFG_POINTER_SPEED));
    console_write(" double-click=");
    print_unsigned(book8_config_value(BOOK8_CFG_DOUBLE_CLICK));
    console_write(" swapped=");
    print_unsigned(book8_config_value(BOOK8_CFG_SWAP_BUTTONS));
    console_write("\n");
    config_loaded = true;
    if (seed_defaults) {
        console_write("BOOK8-CONFIG: generating a default profile.\n");
        (void)gui_save_config(&loaded);
    }
}

bool gui_save_config(const struct book8_system_config *config)
{
    uint8_t bytes[BOOK8_CONFIG_BYTES];
    static const char *const path = "HKLM\\SOFTWARE\\BOOK8\\Settings";
    char theme[8];
    if (!book8_config_encode(config, bytes)) {
        console_write("BOOK8-CONFIG: invalid preferences; not saved.\n");
        return false;
    }
    (void)snprintf(theme, sizeof(theme), "%s",
        config->values[BOOK8_CFG_DARK] != 0u ? "Dark" : "Light");
    if (!reg_set_u32(path, "KeyRate", config->values[BOOK8_CFG_REPEAT_RATE])
        || !reg_set_u32(path, "KeyDelay",
            (config->values[BOOK8_CFG_KEY_DELAY] + 1u) * 250u)
        || !reg_set_u32(path, "MouseSpeed",
            config->values[BOOK8_CFG_POINTER_SPEED])
        || !reg_set_u32(path, "DblClickMs",
            config->values[BOOK8_CFG_DOUBLE_CLICK])
        || !reg_set_str(path, "Theme", theme)
        || !reg_set_u32(path, "Accent", config->values[BOOK8_CFG_ACCENT])
        || !reg_set_u32(path, "Volume", config->values[BOOK8_CFG_VOLUME])
        || !reg_set_u32(path, "Tone", config->values[BOOK8_CFG_TONE])
        || !reg_save()) {
        console_write("BOOK8-CONFIG: registry save failed; preferences unchanged.\n");
        return false;
    }
    if (!fs_write_file(BOOK8_CONFIG_PATH, bytes, sizeof(bytes))) {
        console_write("BOOK8-CONFIG: save failed: ");
        console_write(fs_error_text(fs_last_error()));
        console_write("\n");
        return false;
    }
    snprintf(config_status, sizeof(config_status), "Saved A:\\BOOK8.CFG.");
    gui_diagnostic("BOOK8-CONFIG: saved A:\\BOOK8.CFG.\n");
    return true;
}

bool gui_test_tone(uint32_t frequency)
{
    if (frequency < 100u || frequency > 2000u || tsc_hz == 0u
        || book8_config_value(BOOK8_CFG_MUTE) != 0u
        || book8_config_value(BOOK8_CFG_VOLUME) == 0u) {
        return false;
    }
    uint32_t divisor = PIT_HZ / frequency;
    outb(0x43u, 0xb6u);
    outb(0x42u, (uint8_t)divisor);
    outb(0x42u, (uint8_t)(divisor >> 8u));
    outb(0x61u, (uint8_t)(inb(0x61u) | 3u));
    speaker_stop = read_tsc() + tsc_hz / 5u;
    return true;
}

bool gui_audio_state(uint8_t *volume, bool *muted)
{
    if (volume == NULL || muted == NULL || hda_api == NULL
        || !hda_api->is_available()) {
        return false;
    }
    *volume = hda_api->get_volume();
    *muted = hda_api->get_mute();
    return true;
}

bool gui_audio_set_volume(uint8_t volume)
{
    return hda_api != NULL && hda_api->set_volume(volume) == HDA_OK;
}

bool gui_audio_set_mute(bool muted)
{
    return hda_api != NULL && hda_api->set_mute(muted) == HDA_OK;
}

bool gui_media_play_wav(const uint8_t *wav, uint32_t size)
{
    if (hda_api == NULL)
        return false;
    int32_t result = hda_api->play_wav(wav, size);
    if (result != HDA_STREAM_OK) {
        serial_write("BOOK8-HDA: WAV playback failed: ");
        serial_write(hda_api->stream_result_name(result));
        serial_write("\r\n");
        return false;
    }
    return true;
}

bool gui_media_pause(bool paused)
{
    return hda_api != NULL
        && hda_api->pause(paused) == HDA_STREAM_OK;
}

void gui_media_stop(void)
{
    if (hda_api != NULL)
        hda_api->stop();
}

bool gui_media_seek(uint32_t position_ms)
{
    return hda_api != NULL
        && hda_api->seek(position_ms) == HDA_STREAM_OK;
}

bool gui_media_state(struct book8gui_media_status *status)
{
    if (status == NULL || hda_api == NULL || !hda_api->is_available()) {
        return false;
    }
    struct hda_stream_info info;
    hda_api->get_stream_info(&info);
    status->position_ms = info.position_ms;
    status->duration_ms = info.duration_ms;
    status->loaded = info.loaded;
    status->playing = info.playing;
    status->paused = info.paused;
    status->failed = info.failed;
    return true;
}

bool gui_media_pcm_window(const uint8_t **pcm, uint32_t *size,
    uint64_t *position_bytes)
{
    struct b8_hda_pcm_window window;
    if (pcm == NULL || size == NULL || position_bytes == NULL
        || hda_api == NULL || hda_driver.descriptor == NULL
        || hda_driver.descriptor->ioctl == NULL) {
        return false;
    }
    memset(&window, 0, sizeof(window));
    if (hda_driver.descriptor->ioctl(B8_HDA_IOCTL_GET_PCM_WINDOW,
            &window) != 0 || window.pcm == NULL || window.size == 0u) {
        return false;
    }
    *pcm = window.pcm;
    *size = window.size;
    *position_bytes = window.position_bytes;
    return true;
}

void gui_media_service(void)
{
    if (hda_api == NULL)
        return;
    int32_t result = hda_api->service();
    if (result != HDA_STREAM_OK && result != HDA_STREAM_UNAVAILABLE
        && result != HDA_STREAM_BAD_ARGUMENT) {
        serial_write("BOOK8-HDA: WAV stream stopped: ");
        serial_write(hda_api->stream_result_name(result));
        serial_write("\r\n");
    }
}

bool gui_control_readout(enum book8_control_page page, uint32_t row,
    char *text, uint32_t capacity, uint32_t *bar)
{
    *bar = UINT32_MAX;
    struct book8gui_sysinfo info;
    heap_stats_t heap;
    if (page == BOOK8_PANEL_ABOUT || page == BOOK8_PANEL_ADMIN) {
        heap_get_stats(&heap);
    }
    if (page == BOOK8_PANEL_TIME) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? "Clock: RTC assumed UTC; fields are local."
            : "RTC sync off: session-only software clock.");
    } else if (page == BOOK8_PANEL_KEYBOARD) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? "Input: PS/2 / USB polling" : "Repeat/delay: saved hardware requests.");
    } else if (page == BOOK8_PANEL_MOUSE) {
        if (row > 0u) { return false; }
        snprintf(text, capacity, "Speed/buttons active; absolute input unscaled.");
    } else if (page == BOOK8_PANEL_DISPLAY) {
        if (row == 0u) {
            snprintf(text, capacity, "Canvas: %ux%ux%u pitch %u", screen.width,
                screen.height, screen.bytes_per_pixel * 8u, screen.pitch_bytes);
        } else if (row == 1u) {
            snprintf(text, capacity, "Presentation: %s", fbdev.draw != fbdev.front
                ? "buffered copy" : "direct drawing");
        } else if (row == 2u) {
            snprintf(text, capacity, "Firmware scanout: %ux%u; no GPU modesetting.",
                screen.scanout_width, screen.scanout_height);
        } else { return false; }
    } else if (page == BOOK8_PANEL_NETWORK) {
        if (row > 4u) { return false; }
        if (row == 0u && rtl_nic.initialized) {
            snprintf(text, capacity, "RTL8168 MAC %02X:%02X:%02X:%02X:%02X:%02X",
                (unsigned)rtl_nic.mac[0], (unsigned)rtl_nic.mac[1],
                (unsigned)rtl_nic.mac[2], (unsigned)rtl_nic.mac[3],
                (unsigned)rtl_nic.mac[4], (unsigned)rtl_nic.mac[5]);
            return true;
        }
        if (row >= 2u && kernel_net.ip != 0u) {
            uint32_t address = row == 2u ? kernel_net.ip
                : row == 3u ? kernel_net.mask : kernel_net.gateway;
            snprintf(text, capacity, "%s: %u.%u.%u.%u",
                row == 2u ? "IP" : row == 3u ? "Subnet" : "Gateway",
                (unsigned)(address >> 24u), (unsigned)((address >> 16u) & 255u),
                (unsigned)((address >> 8u) & 255u), (unsigned)(address & 255u));
            return true;
        }
        const char *lines[] = {
            pci_network_present ? "PCI network adapter detected" : "PCI network adapter: none detected",
            "Link: not sampled", "IP default: 0.0.0.0",
            "Subnet default: 0.0.0.0", "Gateway default: 0.0.0.0"
        };
        snprintf(text, capacity, "%s", lines[row]);
    } else if (page == BOOK8_PANEL_STORAGE) {
        if (row == 0u) {
            snprintf(text, capacity, "Root A:; %u disks / %u volumes", disk_count, fs_drive_count());
        } else if (row <= fs_drive_count()) {
            const char *type = "";
            bool readable = false;
            struct book8_drive_space space;
            if (!fs_drive_info(row - 1u, &type, &readable)
                || !fs_drive_space(row - 1u, &space)) {
                snprintf(text, capacity, "Drive sample failed.");
            } else {
                if (space.state == BOOK8_SPACE_READY) {
                    uint64_t used = space.total_bytes - space.free_bytes;
                    *bar = space.total_bytes != 0u
                        ? (uint32_t)(used * 1000u / space.total_bytes) : 0u;
                    snprintf(text, capacity, "%c: %s %llu used / %llu free KiB",
                        fs_drive_letter(row - 1u), space.mounted ? "mounted" : type,
                        (unsigned long long)(used / 1024u),
                        (unsigned long long)(space.free_bytes / 1024u));
                } else {
                    snprintf(text, capacity, "%c: %s %llu KiB %s",
                        fs_drive_letter(row - 1u), readable ? type : "NOFS",
                        (unsigned long long)(space.total_bytes / 1024u),
                        space.state == BOOK8_SPACE_PENDING ? "sampling" :
                        space.state == BOOK8_SPACE_ERROR ? "read error" : "capacity only");
                }
            }
        } else if (row == fs_drive_count() + 1u) {
            snprintf(text, capacity, "Bars: used clusters; sampling is read-only.");
        } else if (row == fs_drive_count() + 2u) {
            snprintf(text, capacity, "USB mass-storage inventory: no driver");
        } else { return false; }
    } else if (page == BOOK8_PANEL_SOUND) {
        if (row > 1u) { return false; }
        snprintf(text, capacity, "%s", row == 0u
            ? (pci_sound_present ? "PCI audio controller detected" : "Audio: PC speaker")
            : "Mixer request saved; speaker uses mute/zero.");
    } else if (page == BOOK8_PANEL_ABOUT) {
        (void)gui_system_info(&info);
        if (row == 0u) { snprintf(text, capacity, "%s", BOOK8_GUI_VERSION); }
        else if (row == 1u) { snprintf(text, capacity, "x86-64: %.40s", info.cpu_name); }
        else if (row == 2u) { snprintf(text, capacity, "%u logical CPUs / BSP active", info.cpu_threads); }
        else if (row == 3u) { snprintf(text, capacity, "Physical RAM: %llu KiB", (unsigned long long)info.memory_total_kb); }
        else if (row == 4u) {
            snprintf(text, capacity, "Heap: %llu / %llu bytes", (unsigned long long)heap.used,
                (unsigned long long)heap.total);
            *bar = heap.total != 0u ? (uint32_t)(heap.used * 1000u / heap.total) : 0u;
        } else if (row == 5u) { snprintf(text, capacity, "Uptime: %llu seconds", (unsigned long long)info.uptime_seconds); }
        else if (row == 6u) { snprintf(text, capacity, "%s", config_status); }
        else { return false; }
    } else if (page == BOOK8_PANEL_ADMIN) {
        if (row == 0u) {
            snprintf(text, capacity, "Heap range: 0x%llx - 0x%llx",
                (unsigned long long)heap.start, (unsigned long long)heap.end);
        } else if (row == 1u) {
            snprintf(text, capacity, "Heap free: %llu bytes", (unsigned long long)heap.free_bytes);
            *bar = heap.total != 0u ? (uint32_t)(heap.used * 1000u / heap.total) : 0u;
        } else if (row == 2u) {
            snprintf(text, capacity, "NV BAR0: 0x%llx / BOOT0: %08x",
                (unsigned long long)nv_gpu.bar0_phys, nv_gpu.boot0);
        } else if (row == 3u) {
            snprintf(text, capacity, "NV present=%u mapped=%u (cached probe)", nv_gpu.present, nv_gpu.mapped);
        } else if (row < 4u + control_memory_count) {
            uint32_t i = row - 4u;
            snprintf(text, capacity, "Map %u: %llx +%llx type %u", i,
                (unsigned long long)control_memory_map[i].base,
                (unsigned long long)control_memory_map[i].size, control_memory_map[i].type);
        } else if (row < 12u + control_memory_count) {
            uint32_t i = (control_log_line + 1u + row - 4u - control_memory_count) % 8u;
            snprintf(text, capacity, "Log: %.80s", control_log[i]);
        } else { return false; }
    } else { return false; }
    return true;
}

bool gui_system_info(struct book8gui_sysinfo *info)
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    info->cpu_name[0] = '\0';
    info->cpu_threads = 1u;
    cpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        uint32_t words[12];
        for (uint32_t leaf = 0u; leaf < 3u; ++leaf) {
            cpu_id(0x80000002u + leaf, &words[leaf * 4u],
                &words[leaf * 4u + 1u], &words[leaf * 4u + 2u],
                &words[leaf * 4u + 3u]);
        }
        const char *raw = (const char *)words;
        uint32_t start = 0u;
        while (start < 48u && raw[start] == ' ') {
            ++start;
        }
        uint32_t n = 0u;
        for (; start < 48u && raw[start] != '\0'; ++start) {
            info->cpu_name[n++] = raw[start];
        }
        while (n != 0u && info->cpu_name[n - 1u] == ' ') {
            --n;
        }
        info->cpu_name[n] = '\0';
    }
    cpu_id(1u, &a, &b, &c, &d);
    if ((d & (1u << 28u)) != 0u && ((b >> 16u) & 0xffu) != 0u) {
        info->cpu_threads = (b >> 16u) & 0xffu;
    }
    cpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000008u) {
        cpu_id(0x80000008u, &a, &b, &c, &d);
        if ((c & 0xffu) + 1u > info->cpu_threads) {
            info->cpu_threads = (c & 0xffu) + 1u;
        }
    }
    info->cpu_mhz = (uint32_t)(tsc_hz / 1000000u);
    info->memory_total_kb = memory_total_kb;
    info->memory_used_kb = (uint64_t)(uintptr_t)__kernel_end / 1024u;
    if (info->memory_total_kb != 0u
        && info->memory_used_kb > info->memory_total_kb) {
        info->memory_used_kb = info->memory_total_kb;
    }
    info->uptime_seconds = tsc_hz != 0u
        ? (read_tsc() - boot_tsc) / tsc_hz : 0u;
    info->disk_read_bytes = disk_read_bytes_total;
    info->disk_write_bytes = disk_write_bytes_total;
    info->disk_busy_ticks = disk_busy_ticks_total;
    info->disk_count = disk_count;
    info->disk_total_kb = 0u;
    info->disk_label[0] = '\0';
    for (uint32_t i = 0u; i < disk_count; ++i) {
        info->disk_total_kb += disks[i].sectors / 2u;
    }
    if (disk_count != 0u) {
        uint32_t n = 0u;
        const char *model = disks[0].model;
        for (; model[n] != '\0' && n + 1u < sizeof(info->disk_label); ++n) {
            info->disk_label[n] = model[n];
        }
        info->disk_label[n] = '\0';
    }
    return true;
}

bool gui_platform_info(struct book8_platform_info *info)
{
    if (info == NULL) {
        return false;
    }
    *info = platform_inventory;

    uint32_t eax, ebx, ecx, edx;
    cpu_id(1u, &eax, &ebx, &ecx, &edx);
    uint32_t family = (eax >> 8u) & 0xfu;
    uint32_t model = (eax >> 4u) & 0xfu;
    if (family == 0xfu) {
        family += (eax >> 20u) & 0xffu;
        model += ((eax >> 16u) & 0xfu) << 4u;
    }
    info->cpu_family = (uint8_t)family;
    info->cpu_model_id = (uint8_t)model;
    info->cpu_stepping = (uint8_t)(eax & 0xfu);

    struct book8gui_sysinfo sys = {0};
    if (gui_system_info(&sys) && sys.cpu_name[0u] != '\0') {
        copy_string(info->cpu_model, sizeof(info->cpu_model), sys.cpu_name);
    }

    if (boot_handoff_uefi) {
        copy_string(info->boot_mode, sizeof(info->boot_mode),
            "UEFI (Multiboot2)");
        info->flags |= BOOK8_PLATFORM_UEFI_BOOT;
        info->csm_video = BOOK8_PLATFORM_OFF;
        info->gop_video = BOOK8_PLATFORM_ON;
        info->secure_boot = BOOK8_PLATFORM_UNKNOWN; /* UEFI vars not read */
    } else {
        copy_string(info->boot_mode, sizeof(info->boot_mode),
            "Legacy BIOS/CSM (Multiboot2)");
        info->csm_video = BOOK8_PLATFORM_ON;
        info->gop_video = BOOK8_PLATFORM_OFF;
        info->secure_boot = BOOK8_PLATFORM_OFF; /* Secure Boot needs UEFI */
    }
    info->fast_boot = BOOK8_PLATFORM_UNKNOWN; /* not exposed to the OS */

    struct book8_nvdiag_snapshot nv = {0};
    if (hwdiag_snapshot(&nv) == 0u
        && (nv.flags & BOOK8_NVDIAG_PRESENT) != 0u) {
        uint64_t four_gib = 1ull << 32u;
        info->above_4g = nv.bar0_phys >= four_gib || nv.bar1_phys >= four_gib
            ? BOOK8_PLATFORM_ON : BOOK8_PLATFORM_OFF;
        info->resizable_bar = nv.bar1_size > NV_GM204_BAR1_SIZE
            ? BOOK8_PLATFORM_ON : BOOK8_PLATFORM_OFF;
    }
    if (acpi_root_seen) {
        info->flags |= BOOK8_PLATFORM_ACPI_VALID;
        info->iommu = acpi_iommu_present
            ? BOOK8_PLATFORM_ON : BOOK8_PLATFORM_OFF;
    }
    return true;
}

void gui_display_size(uint32_t *width, uint32_t *height)
{
    *width = screen.width;
    *height = screen.height;
}

bool gui_set_display_mode(uint32_t width, uint32_t height)
{
    uint32_t old_width = screen.width;
    uint32_t old_height = screen.height;

    if (screen.pixels == NULL
        || !fb_set_logical_mode(&fbdev, width, height, gui_backbuffer,
            sizeof(gui_backbuffer))) {
        return false;
    }
    screen.width = width;
    screen.height = height;
    screen.pitch_bytes = fbdev.pitch;
    if (old_width != 0u && old_height != 0u
        && pointer_x >= 0 && pointer_y >= 0) {
        pointer_x = (int32_t)((uint64_t)(uint32_t)pointer_x * width
            / old_width);
        pointer_y = (int32_t)((uint64_t)(uint32_t)pointer_y * height
            / old_height);
    } else {
        pointer_x = (int32_t)(width / 2u);
        pointer_y = (int32_t)(height / 2u);
    }
    if ((uint32_t)pointer_x >= width) {
        pointer_x = (int32_t)(width - 1u);
    }
    if ((uint32_t)pointer_y >= height) {
        pointer_y = (int32_t)(height - 1u);
    }
    return true;
}

bool gui_nvdiag_snapshot(struct book8_nvdiag_snapshot *snapshot)
{
    return hwdiag_snapshot(snapshot) == 0;
}

struct book8_net *gui_share_network(void)
{
    return !cmdline_network_off && book8_config_value(BOOK8_CFG_NETWORK)
        ? book8_kernel_network() : NULL;
}

bool gui_launch_program(const char *path)
{
    char target[BOOK8GUI_PROGRAM_PATH_SIZE];
    int length = snprintf(target, sizeof(target), "%s", path);
    if (length < 0 || (uint32_t)length >= sizeof(target)) {
        console_write("EXEC: program path exceeds launcher capacity.\n");
        return false;
    }
    char *argv[] = {target};
    long status = exec_run(target, 1, argv);
    stdio_set_output(console_putc);
    if (status < 0 && status > -4096) {
        book8gui_cancel_pending_utility();
        console_write("EXEC: cannot execute ");
        console_write(path);
        printf(" (error %d)\n", (int)-status);
        return false;
    }
    console_write("EXEC: ");
    console_write(path);
    printf(" exited with status %d\n", (int)status);
    if (status != 0) { book8gui_cancel_pending_utility(); return true; }
    return book8gui_open_pending_utility();
}

long book8_gui_utility_request(const struct book8_gui_utility_request *request)
{
    if (screen.pixels == NULL) { return -BOOK8_ENODEV; }
    return book8gui_queue_utility(request->kind, request->path);
}

bool gui_launch_utility(uint32_t kind, const char *document)
{
    const char *path = kind == BOOK8_GUI_HEXVIEW ? "A:\\BIN\\HEXVIEW.ELF"
        : kind == BOOK8_GUI_CODEEDIT ? "A:\\BIN\\CODEEDIT.ELF"
        : kind == BOOK8_GUI_NETSHARE ? "A:\\BIN\\NETSHARE.ELF"
        : kind == BOOK8_GUI_PAINTBOOK ? "A:\\BIN\\PAINTBK.ELF"
        : kind == BOOK8_GUI_BROWSER ? "A:\\BIN\\BROWSER.ELF"
        : "A:\\BIN\\NVDIAG.ELF";
    char target[256], file[256];
    snprintf(target, sizeof(target), "%s", path);
    snprintf(file, sizeof(file), "%s", document != NULL ? document : "");
    char *argv[] = {target, file};
    long status = exec_run(target, file[0] != '\0' ? 2 : 1, argv);
    stdio_set_output(console_putc);
    if (status != 0) {
        book8gui_cancel_pending_utility();
        printf("GUI utility: %s launch failed (status %d)\n", path, (int)status);
        return false;
    }
    if (!book8gui_utility_pending()) {
        printf("GUI utility: %s did not request a window\n", path);
        return false;
    }
    return book8gui_open_pending_utility();
}

bool gui_drive_readable(uint32_t index)
{
    bool readable = false;
    return fs_drive_info(index, NULL, &readable) && readable;
}

bool gui_program_drive_readable(uint32_t index)
{
    if (index != 0u || !gui_drive_readable(index)) { return false; }
    if (application_image != NULL) { return true; }
    struct book8_file_info info;
    return fs_file_info("A:\\BOOT\\BOOK8.ELF", &info) && !info.directory;
}

uint32_t gui_thread_snapshot(struct book8gui_thread_info *out,
    uint32_t capacity)
{
    struct sched_thread_info threads[SCHED_MAX_THREADS];
    uint32_t count = sched_snapshot(threads, SCHED_MAX_THREADS);
    uint32_t copied = count < capacity ? count : capacity;

    for (uint32_t i = 0u; out != NULL && i < copied; ++i) {
        out[i].tid = threads[i].tid;
        out[i].cpu_ticks = threads[i].cpu_ticks;
        copy_string(out[i].name, sizeof(out[i].name), threads[i].name);
        copy_string(out[i].state, sizeof(out[i].state),
            sched_state_name(threads[i].state));
    }
    return count;
}

void gui_launch(bool quiet)
{
    if (gui_active) {
        if (!quiet) {
            console_write("BOOK8GUI is already running.\n");
        }
        return;
    }
    if (screen.pixels == NULL) {
        if (!quiet) {
            console_write("BOOK8GUI requires a supported RGB framebuffer.\n");
        }
        return;
    }
    if (quiet) {
        serial_write("BOOK8-BOOT: automatic desktop initialization started\r\n");
    }
    gui_load_config();
    if (quiet) {
        serial_write("BOOK8-BOOT: desktop profile initialization complete\r\n");
    }
    static const uint32_t mode_widths[] = {800u, 1024u, 1440u};
    static const uint32_t mode_heights[] = {600u, 768u, 900u};
    uint32_t mode = book8_config_value(BOOK8_CFG_RESOLUTION);
    if (mode >= sizeof(mode_widths) / sizeof(mode_widths[0])
        || !gui_set_display_mode(mode_widths[mode], mode_heights[mode])) {
        console_write("BOOK8GUI: saved logical display mode unavailable; using current mode.\n");
    }
    struct book8gui_platform platform = {
        .width = screen.width,
        .height = screen.height,
        .fill_rect = gui_fill_rect,
        .draw_rgba = gui_draw_rgba,
        .draw_text = gui_draw_text,
        .pump_input = gui_pump_input,
        .poll_event = gui_poll_event,
        .usb_mouse_active = gui_usb_mouse_active,
        .run_command = gui_run_command,
        .shutdown = gui_shutdown,
        .reboot = gui_reboot,
        .status_flags = gui_status_flags,
        .network_activity = gui_network_activity,
        .audio_state = gui_audio_state,
        .audio_set_volume = gui_audio_set_volume,
        .audio_set_mute = gui_audio_set_mute,
        .media_play_wav = gui_media_play_wav,
        .media_pause = gui_media_pause,
        .media_stop = gui_media_stop,
        .media_seek = gui_media_seek,
        .media_state = gui_media_state,
        .media_pcm_window = gui_media_pcm_window,
        .media_service = gui_media_service,
        .read_time = gui_read_time,
        .set_time = gui_set_time,
        .system_info = gui_system_info,
        .session_uid = gui_session_uid,
        .session_token = gui_session_token,
        .thread_snapshot = gui_thread_snapshot,
        .read_file = gui_read_file,
        .write_file = gui_write_file,
        .draw_text_scaled = gui_draw_text_scaled,
        .list_directory = gui_list_files,
        .file_info = gui_file_info,
        .rename_entry = gui_rename_entry,
        .delete_entry = gui_delete_entry,
        .copy_to_directory = gui_copy_to_directory,
        .drive_count = gui_drive_count,
        .drive_letter = gui_drive_letter,
        .drive_readable = gui_drive_readable,
        .program_drive_readable = gui_program_drive_readable,
        .last_error_text = gui_last_error_text,
        .diagnostic = gui_diagnostic,
        .clock_ticks = pit_clock_ticks,
        .clock_hz = PIT_HZ,
        .present = NULL,
        .cursor_save_under = gui_cursor_save_under,
        .cursor_restore_under = gui_cursor_restore_under,
        .present_rect = gui_present_rect,
        .save_config = gui_save_config,
        .control_readout = gui_control_readout,
        .test_tone = gui_test_tone,
        .set_display_mode = gui_set_display_mode,
        .display_size = gui_display_size,
        .launch_program = gui_launch_program,
        .launch_utility = gui_launch_utility,
        .nvdiag_snapshot = gui_nvdiag_snapshot,
        .platform_info = gui_platform_info,
        .read_cluster = gui_read_cluster,
        .network = gui_share_network,
        .make_directory = gui_make_directory,
        .create_file = gui_create_file
    };
    if (!quiet) {
        console_write("Launching BOOK8GUI. Use Start > Shutdown > Exit to return to the shell.\n");
    }
    gui_active = true;
    gui_cursor_under_valid = false;
    startup_quiet = false;
    pit_clock_start();
    (void)pit_clock_ticks();
    if (tsc_hz == 0u) {
        tsc_calibrate();
    }
    bool double_buffered = fbdev.draw != fbdev.front;
    if (double_buffered) {
        platform.present = gui_present;
    } else {
        console_write("BOOK8GUI: surface exceeds backbuffer; direct drawing active.\n");
    }
    bool mouse_irq = ps2_mouse_irq_enable();
    if (ps2_mouse_available && !mouse_irq) {
        console_write("PS/2 IRQ12 unavailable; mouse polling fallback active.\n");
    }
    bool started = book8gui_run(&platform);
    if (speaker_stop != 0u) {
        outb(0x61u, (uint8_t)(inb(0x61u) & ~3u));
        speaker_stop = 0u;
    }
    bool mouse_irq_disable_failed = mouse_irq
        && !ps2_mouse_irq_disable();
    gui_active = false;
    startup_quiet = false;
    console_clear();
    console_write(started ? "BOOK8GUI closed.\n"
        : "BOOK8GUI could not start on this display.\n");
    if (mouse_irq_disable_failed) {
        console_write("PS/2 IRQ12 disable failed; controller IRQ masked.\n");
    }
}
