#include "ulib.h"

#ifndef BOOK8_UTILITY_KIND
#error BOOK8_UTILITY_KIND must select the GUI utility
#endif

int main(int argc, char **argv)
{
    struct book8_gui_utility_request request = {.kind = BOOK8_UTILITY_KIND};
    if (BOOK8_UTILITY_KIND == BOOK8_GUI_NETSHARE && argc != 1) {
        uputs("NETSHARE: launch without file arguments; enter host/share in the window\n");
        return 1;
    }
    if (argc > 2) {
        uputs("GUI utility: expected at most one file path\n");
        return 1;
    }
    if (argc == 2) {
        unsigned n = 0u;
        while (argv[1][n] != '\0' && n + 1u < sizeof(request.path)) {
            request.path[n] = argv[1][n];
            ++n;
        }
        if (argv[1][n] != '\0') {
            uputs("GUI utility: file path is too long\n");
            return 1;
        }
    }
    long result = _syscall2(SYS_gui_utility, (long)&request, sizeof(request));
    if (result != 0) {
        uputs("GUI utility: desktop request failed\n");
        return 1;
    }
    return 0;
}
