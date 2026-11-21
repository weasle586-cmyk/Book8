#ifndef BOOK8_MOUSE_INPUT_H
#define BOOK8_MOUSE_INPUT_H

#include <stdint.h>

#define BOOK8_WHEEL_DELTA 120
#define BOOK8_WM_MOUSEWHEEL 0x020au
#define BOOK8_WM_MOUSEHWHEEL 0x020eu

static inline int16_t book8_wheel_detents(int32_t detents)
{
    if (detents > INT16_MAX / BOOK8_WHEEL_DELTA) { detents = INT16_MAX / BOOK8_WHEEL_DELTA; }
    if (detents < INT16_MIN / BOOK8_WHEEL_DELTA) { detents = INT16_MIN / BOOK8_WHEEL_DELTA; }
    return (int16_t)(detents * BOOK8_WHEEL_DELTA);
}

static inline int16_t book8_wheel_wparam(uintptr_t wparam)
{
    uint16_t high = (uint16_t)(wparam >> 16u);
    return high < 0x8000u ? (int16_t)high : (int16_t)((int32_t)high - 65536);
}

/* ID 3 uses a signed byte; ID 4 uses a signed nibble plus extra buttons.
 * PS/2 rotation is opposite to the GUI/HID positive-up convention. */
static inline int16_t book8_ps2_wheel(uint8_t data, uint8_t id)
{
    int32_t rotation = (int8_t)data;
    if (id == 4u) {
        rotation = data & 0x0fu;
        if ((rotation & 0x08) != 0) {
            rotation -= 16;
        }
    }
    return book8_wheel_detents(-rotation);
}

#endif
