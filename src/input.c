#include "kernel_internal.h"

/* PS/2 and USB keyboard/mouse input, CMOS clock and PIT. */

uint8_t cmos_read(uint8_t register_index)
{
    outb(0x70u, register_index);
    return inb(0x71u);
}

uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)((value & 0x0fu) + (value >> 4u) * 10u);
}

struct clock_time read_clock(void)
{
    struct clock_time clock;
    while ((cmos_read(0x0au) & 0x80u) != 0u) {
    }
    uint8_t mode = cmos_read(0x0bu);
    clock.second = cmos_read(0x00u);
    clock.minute = cmos_read(0x02u);
    uint8_t hour = cmos_read(0x04u);
    clock.day = cmos_read(0x07u);
    clock.month = cmos_read(0x08u);
    uint8_t year = cmos_read(0x09u);
    uint8_t century = cmos_read(0x32u);
    if ((mode & 0x04u) == 0u) {
        clock.second = bcd_to_binary(clock.second);
        clock.minute = bcd_to_binary(clock.minute);
        clock.day = bcd_to_binary(clock.day);
        clock.month = bcd_to_binary(clock.month);
        year = bcd_to_binary(year);
        century = bcd_to_binary(century);
        hour = (uint8_t)((hour & 0x80u) | bcd_to_binary(hour & 0x7fu));
    }
    if ((mode & 0x02u) == 0u) {
        bool pm = (hour & 0x80u) != 0u;
        hour &= 0x7fu;
        hour = (uint8_t)(hour % 12u + (pm ? 12u : 0u));
    }
    clock.hour = hour;
    clock.year = century >= 19u && century <= 99u
        ? (uint16_t)(century * 100u + year)
        : (uint16_t)(2000u + year);
    return clock;
}

void print_two_digits(uint8_t value)
{
    console_putc((char)('0' + value / 10u));
    console_putc((char)('0' + value % 10u));
}

char keyboard_char(uint8_t scan)
{
    static const char keys[58] = {
        0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted_keys[58] = {
        0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (scan == 0x2au || scan == 0x36u) {
        shifted = true;
        return 0;
    }
    if (scan == 0xaau || scan == 0xb6u) {
        shifted = false;
        return 0;
    }
    if ((scan & 0x80u) != 0u || scan >= sizeof(keys)) {
        return 0;
    }
    return shifted ? shifted_keys[scan] : keys[scan];
}

bool usb_report_has_key(const struct book8_usb_keyboard_report *report,
    uint8_t key)
{
    for (uint32_t i = 0u; i < sizeof(report->keys); ++i) {
        if (report->keys[i] == key) {
            return true;
        }
    }
    return false;
}

uint32_t gui_key_with_modifiers(uint32_t key, bool ctrl, bool shift)
{
    if (key >= 0x20u && key <= 0x7eu) {
        if (!ctrl) {
            return key;
        }
        if (key >= 'A' && key <= 'Z') {
            key += 'a' - 'A';
        }
        return key | BOOK8GUI_KEY_CTRL | (shift ? BOOK8GUI_KEY_SHIFT : 0u);
    }
    if ((key >= BOOK8GUI_KEY_UP && key <= BOOK8GUI_KEY_F12)
        || key == BOOK8GUI_KEY_TAB) {
        return key | (ctrl ? BOOK8GUI_KEY_CTRL : 0u)
            | (shift ? BOOK8GUI_KEY_SHIFT : 0u);
    }
    return key;
}

bool usb_queue_key(uint32_t key)
{
    uint8_t next = (uint8_t)((usb_key_queue_tail + 1u)
        % USB_KEY_QUEUE_SIZE);
    if (next == usb_key_queue_head) {
        return false;
    }
    usb_key_queue[usb_key_queue_tail] = key;
    usb_key_queue_tail = next;
    return true;
}

uint32_t usb_key_value(uint8_t usage, uint8_t modifiers)
{
    bool shift = (modifiers & 0x22u) != 0u;
    if (usage >= 0x04u && usage <= 0x1du) {
        bool uppercase = shift != ((usb_keyboard_leds & 0x02u) != 0u);
        return (uint32_t)(uppercase
            ? 'A' + usage - 0x04u : 'a' + usage - 0x04u);
    }
    if (usage >= 0x1eu && usage <= 0x27u) {
        static const char digits[] = "1234567890";
        static const char shifted_digits[] = "!@#$%^&*()";
        uint32_t index = usage - 0x1eu;
        return (uint8_t)(shift ? shifted_digits[index] : digits[index]);
    }
    switch (usage) {
    case 0x28u: return BOOK8GUI_KEY_ENTER;
    case 0x29u: return BOOK8GUI_KEY_ESCAPE;
    case 0x2au: return BOOK8GUI_KEY_BACKSPACE;
    case 0x2bu: return BOOK8GUI_KEY_TAB;
    case 0x2cu: return ' ';
    case 0x2du: return shift ? '_' : '-';
    case 0x2eu: return shift ? '+' : '=';
    case 0x2fu: return shift ? '{' : '[';
    case 0x30u: return shift ? '}' : ']';
    case 0x31u: return shift ? '|' : '\\';
    case 0x32u: return shift ? '~' : '#';
    case 0x33u: return shift ? ':' : ';';
    case 0x34u: return shift ? '"' : '\'';
    case 0x35u: return shift ? '~' : '`';
    case 0x36u: return shift ? '<' : ',';
    case 0x37u: return shift ? '>' : '.';
    case 0x38u: return shift ? '?' : '/';
    case 0x39u: return 0u;
    case 0x51u: return BOOK8GUI_KEY_DOWN;
    case 0x52u: return BOOK8GUI_KEY_UP;
    case 0x50u: return BOOK8GUI_KEY_LEFT;
    case 0x4fu: return BOOK8GUI_KEY_RIGHT;
    case 0x4cu: return BOOK8GUI_KEY_DELETE;
    case 0x4au: return BOOK8GUI_KEY_HOME;
    case 0x4du: return BOOK8GUI_KEY_END;
    case 0x4bu: return BOOK8GUI_KEY_PAGE_UP;
    case 0x4eu: return BOOK8GUI_KEY_PAGE_DOWN;
    default:
        if (usage >= 0x3au && usage <= 0x45u) {
            return BOOK8GUI_KEY_F1 + (usage - 0x3au);
        }
        return 0u;
    }
}

void usb_keyboard_update(void)
{
    struct book8_usb_keyboard_report report;
    if (!book8_usb_poll_keyboard(&report)) {
        return;
    }
    for (uint32_t i = 0u; i < sizeof(report.keys); ++i) {
        uint8_t usage = report.keys[i];
        if (usage == 0u || usage == 1u
            || usb_report_has_key(&usb_previous_keyboard_report, usage)) {
            continue;
        }
        uint8_t led_mask = usage == 0x39u ? 0x02u
            : usage == 0x53u ? 0x01u
            : usage == 0x47u ? 0x04u : 0u;
        if (led_mask != 0u) {
            usb_keyboard_leds ^= led_mask;
            continue;
        }
        uint32_t key = usb_key_value(usage, report.modifiers);
        if (key != 0u) {
            key = gui_key_with_modifiers(key,
                (report.modifiers & 0x11u) != 0u,
                (report.modifiers & 0x22u) != 0u);
            if (key == BOOK8GUI_KEY_ESCAPE
                && (report.modifiers & 0x11u) != 0u
                && (report.modifiers & 0x44u) != 0u) {
                key |= BOOK8GUI_KEY_CTRL | BOOK8GUI_KEY_ALT;
            }
            (void)usb_queue_key(key);
        }
    }
    usb_previous_keyboard_report = report;
}

bool usb_keyboard_get_key(uint32_t *key)
{
    usb_keyboard_update();
    if (usb_key_queue_head == usb_key_queue_tail) {
        return false;
    }
    *key = usb_key_queue[usb_key_queue_head];
    usb_key_queue_head = (uint8_t)((usb_key_queue_head + 1u)
        % USB_KEY_QUEUE_SIZE);
    return true;
}

bool ps2_wait_input_empty(void)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        if ((inb(0x64u) & 2u) == 0u) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool ps2_wait_output_full(uint8_t *status)
{
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t current = inb(0x64u);
        if ((current & 1u) != 0u) {
            *status = current;
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool ps2_read_command_byte(uint8_t *command_byte)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }
    /* A pending keyboard byte (e.g. a key release) would otherwise be read
     * back as the command byte and then written to the controller. */
    ps2_flush_output();
    outb(0x64u, 0x20u);
    for (uint32_t i = 0u; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(0x64u);
        if ((status & 1u) != 0u) {
            uint8_t value = inb(0x60u);
            if ((status & 0x20u) == 0u) {
                *command_byte = value;
                return true;
            }
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool ps2_keyboard_initialize(void)
{
    if (inb(0x64u) == 0xffu) {
        return false;
    }
    uint8_t command_byte;
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    command_byte = (uint8_t)((command_byte & ~0x10u) | 0x40u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, command_byte);

    uint8_t verified_command_byte;
    return ps2_read_command_byte(&verified_command_byte)
        && (verified_command_byte & 0x50u) == 0x40u;
}

void ps2_flush_output(void)
{
    for (uint32_t i = 0u; i < 32u && (inb(0x64u) & 1u) != 0u; ++i) {
        (void)inb(0x60u);
        for (uint32_t d = 0u; d < 2000u; ++d) {
            __asm__ volatile ("pause");
        }
    }
}

bool ps2_write_command_byte(uint8_t value)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, value);
    return ps2_wait_input_empty();
}

bool ps2_mouse_send(uint8_t value)
{
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
        uint8_t status;
        if (!ps2_wait_input_empty()) {
            return false;
        }
        outb(0x64u, 0xd4u);
        if (!ps2_wait_input_empty()) {
            return false;
        }
        outb(0x60u, value);
        if (!ps2_wait_output_full(&status)) {
            return false;
        }
        uint8_t reply = inb(0x60u);
        if (reply == 0xfau) {
            return true;
        }
        if (reply != 0xfeu) {
            return false;
        }
    }
    return false;
}

bool ps2_mouse_read_id(uint8_t *device_id)
{
    uint8_t status;
    if (!ps2_mouse_send(0xf2u) || !ps2_wait_output_full(&status)) {
        return false;
    }
    *device_id = inb(0x60u);
    return true;
}

bool ps2_mouse_set_sample_rate(uint8_t rate)
{
    return ps2_mouse_send(0xf3u) && ps2_mouse_send(rate);
}

bool ps2_mouse_initialize(void)
{
    uint8_t command_byte;
    if (inb(0x64u) == 0xffu) {
        return false;
    }
    ps2_flush_output();
    /* Park the aux port, flush, then enable it and clear the aux clock
     * disable bit (5) in the controller command byte. IRQ12 (bit 1) stays
     * as-is; ps2_mouse_irq_enable() manages it. */
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0xa7u);
    ps2_flush_output();
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0xa8u);
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    command_byte = (uint8_t)(command_byte & ~0x20u);
    if (!ps2_write_command_byte(command_byte)) {
        return false;
    }
    ps2_flush_output();

    bool ready = false;
    for (uint32_t attempt = 0u; attempt < 3u && !ready; ++attempt) {
        uint8_t status;
        /* Reset: FA, AA, 00. Absent or slow devices fall through to F6/F4. */
        if (ps2_mouse_send(0xffu)) {
            for (uint32_t i = 0u; i < 2u; ++i) {
                if (!ps2_wait_output_full(&status)) {
                    break;
                }
                (void)inb(0x60u);
            }
        }
        ps2_flush_output();
        ready = ps2_mouse_send(0xf6u) && ps2_mouse_send(0xf4u);
    }
    if (!ready) {
        return false;
    }
    ps2_mouse_packet_length = 3u;
    ps2_mouse_id = 0u;
    uint8_t mouse_id = 0u;
    if (ps2_mouse_send(0xf5u)
        && ps2_mouse_set_sample_rate(200u)
        && ps2_mouse_set_sample_rate(100u)
        && ps2_mouse_set_sample_rate(80u)
        && ps2_mouse_read_id(&mouse_id)
        && (mouse_id == 3u || mouse_id == 4u)) {
        ps2_mouse_packet_length = 4u;
        ps2_mouse_id = mouse_id;
    }
    if (!ps2_mouse_send(0xf4u)) {
        console_write("PS/2 mouse reporting could not be enabled.\n");
        return false;
    }
    ps2_mouse_packet_index = 0u;
    pointer_buttons = 0u;
    pointer_x = screen.width != 0u ? (int32_t)(screen.width / 2u) : 0;
    pointer_y = screen.height != 0u ? (int32_t)(screen.height / 2u) : 0;
    ps2_mouse_available = true;
    return true;
}

bool ps2_mouse_set_irq_mode(bool enabled)
{
    uint8_t command_byte;
    if (!ps2_read_command_byte(&command_byte)) {
        return false;
    }
    if (enabled) {
        command_byte = (uint8_t)((command_byte | 0x02u) & ~0x20u);
    } else {
        command_byte = (uint8_t)(command_byte & ~0x02u);
    }
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x64u, 0x60u);
    if (!ps2_wait_input_empty()) {
        return false;
    }
    outb(0x60u, command_byte);
    uint8_t verified;
    return ps2_read_command_byte(&verified)
        && ((verified & 0x02u) != 0u) == enabled;
}

bool ps2_mouse_irq_enable(void)
{
    uint64_t flags = irq_save();

    if (!ps2_mouse_available) {
        irq_restore(flags);
        return false;
    }
    interrupt_table_initialize();
    if (!ps2_mouse_set_irq_mode(true)) {
        irq_restore(flags);
        return false;
    }
    mouse_event_head = 0u;
    mouse_event_tail = 0u;
    mouse_event_last_buttons = pointer_buttons;
    mouse_event_last_coalescible = false;
    outb(0x21u, 0xfau);
    outb(0xa1u, 0xefu);
    ps2_mouse_irq_enabled = true;
    __asm__ volatile ("sti" : : : "memory");
    return true;
}

bool ps2_mouse_irq_disable(void)
{
    uint64_t flags = irq_save();
    bool disabled;

    outb(0x21u, 0xfeu);
    outb(0xa1u, 0xffu);
    ps2_mouse_irq_enabled = false;
    ps2_mouse_packet_index = 0u;
    disabled = ps2_mouse_set_irq_mode(false);
    irq_restore(flags);
    return disabled;
}

bool ps2_mouse_feed(uint8_t data, struct book8gui_event *event)
{
    if (!ps2_mouse_available) {
        return false;
    }
    if (ps2_mouse_packet_index == 0u && (data & 0x08u) == 0u) {
        return false;
    }
    ps2_mouse_packet[ps2_mouse_packet_index++] = data;
    if (ps2_mouse_packet_index < ps2_mouse_packet_length) {
        return false;
    }
    ps2_mouse_packet_index = 0u;
    if ((ps2_mouse_packet[0] & 0xc0u) != 0u) {
        return false;
    }
    ++ps2_mouse_packet_count;
    pointer_x += book8_pointer_delta((int8_t)ps2_mouse_packet[1], &pointer_remainder_x);
    pointer_y += book8_pointer_delta(-(int32_t)(int8_t)ps2_mouse_packet[2], &pointer_remainder_y);
    if (pointer_x < 0) {
        pointer_x = 0;
    } else if ((uint32_t)pointer_x >= screen.width) {
        pointer_x = (int32_t)(screen.width - 1u);
    }
    if (pointer_y < 0) {
        pointer_y = 0;
    } else if ((uint32_t)pointer_y >= screen.height) {
        pointer_y = (int32_t)(screen.height - 1u);
    }
    pointer_buttons = ps2_mouse_packet[0] & 0x07u;
    event->wheel_delta = 0;
    if (ps2_mouse_packet_length == 4u) {
        event->wheel_delta = book8_ps2_wheel(ps2_mouse_packet[3], ps2_mouse_id);
        if (ps2_mouse_id == 4u) {
            pointer_buttons |= (uint8_t)((ps2_mouse_packet[3] & 0x30u) >> 1u);
        }
    }
    event->type = BOOK8GUI_EVENT_MOUSE;
    event->key = 0u;
    event->mouse_x = pointer_x;
    event->mouse_y = pointer_y;
    event->mouse_buttons = book8_pointer_buttons(pointer_buttons);
    return true;
}

bool mouse_event_enqueue(const struct book8gui_event *event, bool usb)
{
    uint64_t flags = irq_save();
    uint8_t head = mouse_event_head;
    bool button_transition =
        event->mouse_buttons != mouse_event_last_buttons;

    if (!button_transition && event->wheel_delta == 0
        && mouse_event_last_coalescible && head != mouse_event_tail) {
        uint8_t last = (uint8_t)((head - 1u)
            & (MOUSE_EVENT_QUEUE_SIZE - 1u));
        if (mouse_events[last].wheel_delta == 0
            && mouse_events[last].mouse_buttons == event->mouse_buttons) {
            mouse_events[last] = *event;
            mouse_event_last_buttons = event->mouse_buttons;
            if (usb) {
                ++usb_mouse_queue_events;
            } else {
                ++ps2_mouse_queue_events;
            }
            irq_restore(flags);
            return true;
        }
    }

    uint8_t next = (uint8_t)((head + 1u) & (MOUSE_EVENT_QUEUE_SIZE - 1u));

    if (next == mouse_event_tail) {
        if (usb) {
            ++usb_mouse_queue_drops;
        } else {
            ++ps2_mouse_queue_drops;
        }
        irq_restore(flags);
        return false;
    }
    mouse_events[head] = *event;
    mouse_event_head = next;
    mouse_event_last_buttons = event->mouse_buttons;
    mouse_event_last_coalescible =
        !button_transition && event->wheel_delta == 0;
    if (usb) {
        ++usb_mouse_queue_events;
    } else {
        ++ps2_mouse_queue_events;
    }
    irq_restore(flags);
    return true;
}

bool mouse_event_dequeue(struct book8gui_event *event)
{
    uint64_t flags = irq_save();
    uint8_t tail = mouse_event_tail;

    if (tail == mouse_event_head) {
        irq_restore(flags);
        return false;
    }
    *event = mouse_events[tail];
    mouse_event_tail = (uint8_t)((tail + 1u) & (MOUSE_EVENT_QUEUE_SIZE - 1u));
    irq_restore(flags);
    return true;
}

void book8_ps2_mouse_irq_dispatch(void)
{
    uint8_t status = inb(0x64u);

    ++ps2_mouse_irq_count;
    if ((status & 1u) != 0u && (status & 0x20u) != 0u) {
        struct book8gui_event event;
        if (ps2_mouse_feed(inb(0x60u), &event)) {
            (void)mouse_event_enqueue(&event, false);
        }
    }
    outb(0xa0u, 0x20u);
    outb(0x20u, 0x20u);
    book8_apic_eoi();
}

void gui_pump_input(void)
{
    network_rx_poll();
    fs_space_poll();
    if (speaker_stop != 0u && read_tsc() >= speaker_stop) {
        outb(0x61u, (uint8_t)(inb(0x61u) & ~3u));
        speaker_stop = 0u;
    }
    if (!ps2_mouse_irq_enabled && ps2_mouse_available) {
        for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
            uint8_t status = inb(0x64u);
            if ((status & 1u) == 0u || (status & 0x20u) == 0u) {
                break;
            }
            struct book8gui_event event;
            if (ps2_mouse_feed(inb(0x60u), &event)) {
                (void)mouse_event_enqueue(&event, false);
            }
        }
    }

    struct book8_usb_mouse_report usb_report;
    for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
        if (!book8_usb_poll_mouse(&usb_report)) {
            break;
        }
        ++usb_mouse_report_count;
        struct book8gui_event usb_event;
        uint64_t flags = irq_save();
        if (usb_report.absolute) {
            int64_t x_range = (int64_t)usb_report.logical_max_x
                - usb_report.logical_min_x;
            int64_t y_range = (int64_t)usb_report.logical_max_y
                - usb_report.logical_min_y;
            int64_t x_value = usb_report.x;
            int64_t y_value = usb_report.y;
            if (x_value < usb_report.logical_min_x) {
                x_value = usb_report.logical_min_x;
            } else if (x_value > usb_report.logical_max_x) {
                x_value = usb_report.logical_max_x;
            }
            if (y_value < usb_report.logical_min_y) {
                y_value = usb_report.logical_min_y;
            } else if (y_value > usb_report.logical_max_y) {
                y_value = usb_report.logical_max_y;
            }
            pointer_x = x_range > 0
                ? (int32_t)(((uint64_t)(x_value - usb_report.logical_min_x)
                    * (screen.width - 1u)) / (uint64_t)x_range)
                : 0;
            pointer_y = y_range > 0
                ? (int32_t)(((uint64_t)(y_value - usb_report.logical_min_y)
                    * (screen.height - 1u)) / (uint64_t)y_range)
                : 0;
        } else {
            int64_t next_x = (int64_t)pointer_x
                + book8_pointer_delta(usb_report.x, &pointer_remainder_x);
            /* HID +Y is toward the user (down); screen Y also grows down. */
            int64_t next_y = (int64_t)pointer_y
                + book8_pointer_delta(usb_report.y, &pointer_remainder_y);
            pointer_x = next_x < 0 ? 0
                : next_x >= screen.width ? (int32_t)(screen.width - 1u)
                : (int32_t)next_x;
            pointer_y = next_y < 0 ? 0
                : next_y >= screen.height ? (int32_t)(screen.height - 1u)
                : (int32_t)next_y;
        }
        pointer_buttons = usb_report.buttons;
        usb_event.type = BOOK8GUI_EVENT_MOUSE;
        usb_event.key = 0u;
        usb_event.mouse_x = pointer_x;
        usb_event.mouse_y = pointer_y;
        usb_event.mouse_buttons = book8_pointer_buttons(pointer_buttons);
        usb_event.wheel_delta = usb_report.wheel_delta;
        irq_restore(flags);
        (void)mouse_event_enqueue(&usb_event, true);
    }
}

bool gui_poll_event(struct book8gui_event *event)
{
    if (serial_ready()) {
        uint8_t value = inb(COM1);
        event->type = BOOK8GUI_EVENT_KEY;
        event->key = value == '\r' || value == '\n'
            ? BOOK8GUI_KEY_ENTER : (value == '\b' ? BOOK8GUI_KEY_BACKSPACE
                : (value == 0x1bu ? BOOK8GUI_KEY_ESCAPE
                    : (value == '\t' ? BOOK8GUI_KEY_TAB : value)));
        return true;
    }

    uint32_t usb_key;
    if (usb_keyboard_get_key(&usb_key)) {
        event->type = BOOK8GUI_EVENT_KEY;
        event->key = usb_key;
        return true;
    }

    if (mouse_event_dequeue(event)) {
        if (event->wheel_delta != 0) {
            uint32_t buttons = event->mouse_buttons;
            uintptr_t wparam = (uintptr_t)(uint16_t)event->wheel_delta << 16u;
            wparam |= (buttons & 3u) | ((buttons & 4u) << 2u)
                | ((buttons & 24u) << 2u);
            (void)book8gui_decode_wheel_message(event, BOOK8_WM_MOUSEWHEEL, wparam,
                event->mouse_x, event->mouse_y);
            console_write("BOOK8-MOUSE: X=");
            print_signed(event->mouse_x);
            console_write(" Y=");
            print_signed(event->mouse_y);
            console_write(" WHEEL=");
            print_signed(event->wheel_delta / BOOK8_WHEEL_DELTA);
            console_write(" WHEEL_DELTA=");
            print_signed(event->wheel_delta);
            console_write(" BUTTONS=");
            print_unsigned(event->mouse_buttons);
            console_putc('\n');
        }
        return true;
    }

    static bool extended_scan = false;
    for (uint32_t attempt = 0u; attempt < 16u; ++attempt) {
        uint8_t status = inb(0x64u);
        if ((status & 1u) == 0u) {
            break;
        }
        if ((status & 0x20u) != 0u && ps2_mouse_irq_enabled) {
            continue;
        }
        uint8_t scan = inb(0x60u);
        if ((status & 0x20u) != 0u) {
            if (ps2_mouse_feed(scan, event)) {
                (void)mouse_event_enqueue(event, false);
            }
            continue;
        }
        if (scan == 0xe0u) {
            extended_scan = true;
            continue;
        }
        if (extended_scan) {
            extended_scan = false;
            if (scan == 0x1du || scan == 0x9du) {
                ps2_ctrl_down = scan == 0x1du;
                continue;
            }
            if (scan == 0x38u || scan == 0xb8u) {
                ps2_alt_down = scan == 0x38u;
                continue;
            }
            uint32_t extended_key = 0u;
            switch (scan) {
            case 0x48u: extended_key = BOOK8GUI_KEY_UP; break;
            case 0x50u: extended_key = BOOK8GUI_KEY_DOWN; break;
            case 0x4bu: extended_key = BOOK8GUI_KEY_LEFT; break;
            case 0x4du: extended_key = BOOK8GUI_KEY_RIGHT; break;
            case 0x53u: extended_key = BOOK8GUI_KEY_DELETE; break;
            case 0x47u: extended_key = BOOK8GUI_KEY_HOME; break;
            case 0x4fu: extended_key = BOOK8GUI_KEY_END; break;
            case 0x49u: extended_key = BOOK8GUI_KEY_PAGE_UP; break;
            case 0x51u: extended_key = BOOK8GUI_KEY_PAGE_DOWN; break;
            default: break;
            }
            if (extended_key != 0u) {
                event->type = BOOK8GUI_EVENT_KEY;
                event->key = gui_key_with_modifiers(extended_key,
                    ps2_ctrl_down, shifted);
                return true;
            }
            continue;
        }
        if (scan == 0x1du || scan == 0x9du) {
            ps2_ctrl_down = scan == 0x1du;
            continue;
        }
        if (scan == 0x38u || scan == 0xb8u) {
            ps2_alt_down = scan == 0x38u;
            continue;
        }
        uint32_t function_key = 0u;
        if (scan >= 0x3bu && scan <= 0x44u) {
            function_key = BOOK8GUI_KEY_F1 + (scan - 0x3bu);
        } else if (scan == 0x57u || scan == 0x58u) {
            function_key = BOOK8GUI_KEY_F11 + (scan - 0x57u);
        }
        if (function_key != 0u) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers(function_key,
                ps2_ctrl_down, shifted);
            return true;
        }
        if (scan == 0x01u) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = ps2_ctrl_down && ps2_alt_down
                ? (BOOK8GUI_KEY_ESCAPE | BOOK8GUI_KEY_CTRL
                    | BOOK8GUI_KEY_ALT)
                : BOOK8GUI_KEY_ESCAPE;
            return true;
        }
        if (scan == 0x1cu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = BOOK8GUI_KEY_ENTER;
            return true;
        }
        if (scan == 0x0eu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = BOOK8GUI_KEY_BACKSPACE;
            return true;
        }
        if (scan == 0x0fu) {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers(BOOK8GUI_KEY_TAB,
                ps2_ctrl_down, shifted);
            return true;
        }
        char value = keyboard_char(scan);
        if (value != '\0') {
            event->type = BOOK8GUI_EVENT_KEY;
            event->key = gui_key_with_modifiers((uint8_t)value,
                ps2_ctrl_down, shifted);
            return true;
        }
    }
    return mouse_event_dequeue(event);
}

void pit_clock_start(void)
{
    outb(0x43u, 0x34u);
    outb(0x40u, 0u);
    outb(0x40u, 0u);
}

uint64_t pit_clock_ticks(void)
{
    static uint64_t total;
    static uint16_t last;
    outb(0x43u, 0x00u);
    uint16_t count = inb(0x40u);
    count = (uint16_t)(count | ((uint16_t)inb(0x40u) << 8u));
    total += (uint16_t)(last - count);
    last = count;
    return total;
}
