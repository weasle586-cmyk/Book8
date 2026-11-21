#include "ulib.h"

/* Exercises sys_open/read/close on a file in a subdirectory, then replaces
 * itself with HELLO.ELF through sys_execve. */
int main(int argc, char **argv)
{
    static char buf[256];
    char *new_argv[3];
    long fd, n;

    (void)argc;
    (void)argv;
    fd = sys_open("/DATA/MSG.TXT", O_RDONLY);
    if (fd < 3) {
        uputs("CAT: open failed\n");
        return 2;
    }
    uputs("CAT: ");
    while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0)
        sys_write(1, buf, (unsigned long)n);
    sys_close((int)fd);
    if (sys_read((int)fd, buf, 1) != -1) {
        uputs("CAT: read on closed fd succeeded\n");
        return 3;
    }
    if (sys_open("/NOPE.TXT", O_RDONLY) != -1) {
        uputs("CAT: missing file opened\n");
        return 4;
    }
    new_argv[0] = "hello";
    new_argv[1] = "from-cat";
    new_argv[2] = 0;
    if (sys_execve("A:\\APPS\\ExecRollback\\app.elf", new_argv, 0) != -1) {
        uputs("CAT: invalid package execve unexpectedly succeeded\n");
        return 6;
    }
    uputs("CAT: failed package execve preserved image\n");
    uputs("CAT: execve hello\n");
    sys_execve("/HELLO.ELF", new_argv, 0);
    uputs("CAT: execve failed\n");
    return 5;
}
