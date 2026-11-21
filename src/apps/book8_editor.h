#ifndef BOOK8_APPS_EDITOR_H
#define BOOK8_APPS_EDITOR_H

#include "../book8gui.h"

extern const struct book8gui_app_class book8_editor_class;
bool book8_editor_open_path(const struct book8gui_platform *platform,
    struct WindowApp *app, const char *path);

#endif
