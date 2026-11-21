#ifndef BOOK8_UTILITIES_H
#define BOOK8_UTILITIES_H

#include "../book8gui.h"

extern const struct book8gui_app_class book8_hexview_class;
extern const struct book8gui_app_class book8_codeedit_class;
void book8_hex_format(uint32_t offset, const uint8_t *data, uint32_t size,
    char line[79]);

#endif
