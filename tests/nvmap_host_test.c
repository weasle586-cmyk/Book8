#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define errno nvmap_errno
#define main nvmap_main
#include "../user/nvmap.c"
#undef main

int nvmap_errno;
static uint32_t flags, read_value, read_error, info_error;
static unsigned reads, reports;
static char output[1024];
static size_t output_length;

static bool contains(const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0u; i + length <= output_length; ++i)
        if (memcmp(output + i, text, length) == 0)
            return true;
    return false;
}

long sys_write(int fd, const void *buf, unsigned long count)
{
    assert(fd == 1 && count < sizeof(output) - output_length);
    memcpy(output + output_length, buf, count);
    output_length += count;
    output[output_length] = '\0';
    return (long)count;
}

long sys_hwdiag(unsigned operation, void *data, unsigned long size)
{
    if (operation == BOOK8_HWDIAG_INFO) {
        assert(size == sizeof(struct book8_hwdiag_info));
        if (info_error != 0u) {
            nvmap_errno = (int)info_error;
            return -1;
        }
        *(struct book8_hwdiag_info *)data = (struct book8_hwdiag_info){.flags = flags};
        return 0;
    }
    if (operation == BOOK8_HWDIAG_READ32) {
        assert(size == sizeof(struct book8_hwdiag_read32));
        struct book8_hwdiag_read32 *reg = data;
        assert(reg->offset == 0x200u);
        ++reads;
        if (read_error != 0u) {
            nvmap_errno = (int)read_error;
            return -1;
        }
        reg->value = read_value;
        return 0;
    }
    assert(operation == BOOK8_HWDIAG_REPORT
        && size == sizeof(struct book8_hwdiag_report));
    ++reports;
    nvmap_errno = BOOK8_ENOSYS;
    return -1;
}

static void reset(void)
{
    flags = read_value = read_error = info_error = 0u;
    reads = reports = 0u;
    output_length = 0u;
    output[0] = '\0';
}

int main(void)
{
    reset();
    assert(nvmap_main(0, NULL) == 0);
    assert(reads == 0u && reports == 0u);
    assert(contains("target absent"));
    assert(contains("local diagnostics only; telemetry submission disabled"));

    reset();
    flags = BOOK8_HWDIAG_PRESENT;
    assert(nvmap_main(0, NULL) == 0);
    assert(reads == 0u && reports == 0u);
    assert(contains("target config-only"));

    reset();
    flags = BOOK8_HWDIAG_PRESENT | BOOK8_HWDIAG_MAPPED;
    read_value = 0x12345678u;
    assert(nvmap_main(0, NULL) == 0);
    assert(reads == 1u && reports == 0u);
    assert(contains("PMC_ENABLE=305419896"));

    reset();
    flags = BOOK8_HWDIAG_PRESENT | BOOK8_HWDIAG_MAPPED;
    read_error = BOOK8_EIO;
    assert(nvmap_main(0, NULL) == 1);
    assert(reads == 1u && reports == 0u);
    assert(contains("register unavailable errno=5"));

    reset();
    info_error = BOOK8_EFAULT;
    assert(nvmap_main(0, NULL) == 1 && reads == 0u && reports == 0u);
    assert(contains("info failed errno=14"));

    puts("PASS: NVMAP local diagnostics, mapped read, errors and no snapshot submission");
    return 0;
}
