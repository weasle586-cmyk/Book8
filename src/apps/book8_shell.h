#ifndef BOOK8_APPS_SHELL_H
#define BOOK8_APPS_SHELL_H

#include "../book8gui.h"

extern const struct book8gui_app_class book8_shell_class;

void book8_shell_append_char(char value);
void book8_shell_clear(void);
bool book8_shell_is_open(void);

#endif
