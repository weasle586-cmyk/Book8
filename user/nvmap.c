#include "ulib.h"
#include <hwdiag.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct book8_hwdiag_info info;
    struct book8_hwdiag_read32 reg = {0x200u, 0u};
    if (sys_hwdiag(BOOK8_HWDIAG_INFO, &info, sizeof(info)) != 0) {
        uputs("NVMAP: info failed errno=");
        uputnum(errno);
        uputs("\n");
        return 1;
    }
    if ((info.flags & BOOK8_HWDIAG_MAPPED) == 0u) {
        if ((info.flags & BOOK8_HWDIAG_PRESENT) != 0u)
            uputs("NVMAP: target config-only; PMC register not read\n");
        else
            uputs("NVMAP: target absent; PMC register not read\n");
    } else if (sys_hwdiag(BOOK8_HWDIAG_READ32, &reg, sizeof(reg)) != 0) {
        uputs("NVMAP: register unavailable errno=");
        uputnum(errno);
        uputs(" (no power-state writes)\n");
        return 1;
    } else {
        uputs("NVMAP: PMC_ENABLE=");
        uputnum(reg.value);
        uputs("\n");
    }
    uputs("NVMAP: local diagnostics only; telemetry submission disabled\n");
    return 0;
}
