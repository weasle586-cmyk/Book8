#ifndef BOOK8_APPS_EXPLORER_H
#define BOOK8_APPS_EXPLORER_H

#include "../book8gui.h"

extern const struct book8gui_app_class book8_explorer_class;
bool book8_explorer_open_directory(const struct book8gui_platform *platform,
    struct WindowApp *app, const char *path);

#endif
