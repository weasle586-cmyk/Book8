#include "ulib.h"

/* Static data and bss on writable user pages. */
static char banner[] = "HELLO: running at CPL3 from an ELF on disk\n";
static int counter;
static char scratch[5000]; /* bss spanning more than one page */

int main(int argc, char **argv)
{
    int i;
    struct book8_fb_info framebuffer;
    unsigned long flags;

    uputs(banner);
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    if ((flags & 0x200u) == 0u) {
        uputs("HELLO: interrupts disabled\n");
        return 10;
    }
    uputs("HELLO: interrupts enabled\n");
    if (sys_fb_info(&framebuffer) == 0) {
        if (framebuffer.width < 2u || framebuffer.height < 2u
            || sys_draw_pixel(0u, 0u, 0xffffffu) != 0
            || sys_draw_pixel(framebuffer.width - 2u,
                framebuffer.height - 2u, 0x0d25fbu) != 0
            || sys_draw_pixel(framebuffer.width, 0u, 0u) != -1
            || errno != BOOK8_EINVAL) {
            uputs("HELLO: framebuffer syscall check FAILED\n");
            return 8;
        }
        uputs("HELLO: framebuffer syscalls ok\n");
    } else if (errno == BOOK8_ENODEV) {
        uputs("HELLO: framebuffer unavailable\n");
    } else {
        uputs("HELLO: framebuffer info failed\n");
        return 9;
    }
    uputs("HELLO: argc=");
    uputnum(argc);
    uputs(" pid=");
    uputnum(sys_getpid());
    uputs("\n");
    for (i = 0; i < argc; i++) {
        uputs("HELLO: argv[");
        uputnum(i);
        uputs("]=");
        uputs(argv[i]);
        uputs("\n");
        counter += i;
    }
    scratch[sizeof(scratch) - 1] = 'z'; /* touch the last bss page */
    if (counter != argc * (argc - 1) / 2 || scratch[0] != 0
        || scratch[sizeof(scratch) - 1] != 'z') {
        uputs("HELLO: data/bss check FAILED\n");
        return 1;
    }
    uputs("HELLO: data/bss ok\n");
    return 7;
}
