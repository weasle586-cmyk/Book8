/* AFL-light syscall-boundary fuzzer.
 *
 * Mutates a seed corpus of syscall_frame images and feeds each one to the real
 * syscall_dispatch() used by the kernel. External dependencies are stubbed, so
 * ASan/UBSan in the build catch bad buffer arithmetic, overflows and invalid
 * accesses at the user/kernel boundary. Deterministic: a given seed and
 * iteration count always replays the same case sequence.
 *
 * usage: syscall_fuzz_host_test [iterations] [rng-seed] [seed-corpus-dir]
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/syscall.h"
#include "../kernel/exec.h"
#include "../kernel/hwdiag.h"

#define FUZZ_USER_BYTES 65536u
#define FUZZ_MAX_SEEDS 512u

static uint8_t user_buffer[FUZZ_USER_BYTES] __attribute__((aligned(4096)));
static bool has_user_space = true;
static unsigned utility_requests;
static unsigned console_bytes;

/* user_range_ok() consults these symbols in the non-host build too. */
char smoke_pages[64];
__asm__(".globl __user_text_start\n.set __user_text_start, smoke_pages\n"
        ".globl __user_text_end\n.set __user_text_end, smoke_pages + 16\n"
        ".globl __user_data_start\n.set __user_data_start, smoke_pages + 16\n"
        ".globl __user_stack_end\n.set __user_stack_end, smoke_pages + 64\n");

bool exec_user_range_ok(uint64_t ptr, uint64_t len, bool write)
{
    (void)write;
    uint64_t base = (uint64_t)(uintptr_t)user_buffer;
    return ptr >= base && ptr - base <= FUZZ_USER_BYTES
        && len <= FUZZ_USER_BYTES - (ptr - base);
}
bool exec_has_user_space(void) { return has_user_space; }
uint32_t exec_current_pid(void) { return 1u; }
long exec_sys_open(uint64_t path, uint64_t flags)
{ (void)path; (void)flags; return -BOOK8_ENOENT; }
long exec_sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{ (void)fd; (void)buf; (void)len; return -BOOK8_EBADF; }
long exec_sys_close(uint64_t fd) { (void)fd; return -BOOK8_EBADF; }
long exec_sys_execve(struct syscall_frame *f, uint64_t a, uint64_t b)
{ (void)f; (void)a; (void)b; return -BOOK8_ENOEXEC; }
long book8_console_write(const char *buf, unsigned long len)
{ (void)buf; console_bytes += (unsigned)len; return (long)len; }
long book8_console_read(char *buf, unsigned long len)
{ (void)buf; return (long)len; }
long book8_framebuffer_info(struct book8_fb_info *info)
{ (void)info; return -BOOK8_ENODEV; }
long book8_framebuffer_draw_pixel(uint64_t x, uint64_t y, uint32_t rgb)
{ (void)x; (void)y; (void)rgb; return -BOOK8_ENODEV; }
long book8_gui_utility_request(const struct book8_gui_utility_request *request)
{
    if (request->kind == 0u || request->kind > BOOK8_GUI_PLAYER)
        return -BOOK8_EINVAL;
    ++utility_requests;
    return 0;
}
void syscall_return_to_kernel(long status) { (void)status; abort(); }
void book8_task_exit(long status) { (void)status; }

long hwdiag_info(struct book8_hwdiag_info *info)
{ (void)info; return -BOOK8_ENOTSUP; }
long hwdiag_read32(struct book8_hwdiag_read32 *request)
{ (void)request; return -BOOK8_ENOTSUP; }
long hwdiag_report(const struct book8_hwdiag_report *report)
{ (void)report; return 0; }

static uint64_t rng_state;
static uint64_t next_random(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

static struct syscall_frame seeds[FUZZ_MAX_SEEDS];
static size_t seed_count;

static void add_seed(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3)
{
    if (seed_count == FUZZ_MAX_SEEDS) return;
    struct syscall_frame *frame = &seeds[seed_count++];
    memset(frame, 0, sizeof(*frame));
    frame->rax = number;
    frame->rdi = a1;
    frame->rsi = a2;
    frame->rdx = a3;
}

static void init_seeds(void)
{
    uint64_t base = (uint64_t)(uintptr_t)user_buffer;
    memset(user_buffer, 0, sizeof(user_buffer));
    memcpy(user_buffer, "A:\\BIN\\HEXVIEW.ELF", 18u);

    add_seed(SYS_read, 0u, base, 32u);
    add_seed(SYS_read, 999u, 0u, UINT64_MAX);
    add_seed(SYS_write, 1u, base, 64u);
    add_seed(SYS_write, 2u, base, 65536u);
    add_seed(SYS_open, base, 0u, 0u);
    add_seed(SYS_close, 3u, 0u, 0u);
    add_seed(SYS_getpid, 0u, 0u, 0u);
    add_seed(SYS_fb_info, base, 0u, 0u);
    add_seed(SYS_draw_pixel, 10u, 20u, 0x00ff0000u);
    add_seed(SYS_hwdiag, BOOK8_HWDIAG_INFO, base, 0u);
    add_seed(SYS_hwdiag, BOOK8_HWDIAG_READ32, base, 0u);
    add_seed(SYS_gui_utility, base, 260u, 0u);
    add_seed(SYS_execve, base, base + 32u, 0u);
    add_seed(SYS_gui_utility, 0u, 0u, 0u);
    add_seed(SYS_MAX, 0u, 0u, 0u);
    add_seed(UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX);
}

/* Optional hex seed files: 16 whitespace-separated 64-bit words per frame. */
static void load_corpus(const char *directory)
{
    if (directory == NULL || seed_count == FUZZ_MAX_SEEDS) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/frames.seed", directory);
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    while (seed_count < FUZZ_MAX_SEEDS) {
        struct syscall_frame frame;
        size_t words = 0u;
        uint64_t *raw = (uint64_t *)&frame;
        while (words < sizeof(frame) / sizeof(raw[0])
               && fscanf(file, "%" SCNx64, &raw[words]) == 1) {
            ++words;
        }
        if (words == 0u) break;
        while (words < sizeof(frame) / sizeof(raw[0])) raw[words++] = 0u;
        seeds[seed_count++] = frame;
    }
    fclose(file);
}

static const uint64_t interesting[] = {
    0u, 1u, 2u, 3u, 4u, 7u, 8u, 16u, 20u, 32u, 64u, 255u, 256u, 260u,
    0x1000u, 65535u, 65536u, 0x7fffffffu, 0x80000000u,
    BOOK8_GUI_PLAYER, SYS_MAX - 1u, SYS_MAX, SYS_exit,
    0x00007fffffffffffULL, 0x0000800000000000ULL,
    UINT64_MAX - 1u, UINT64_MAX
};
#define INTERESTING_COUNT (sizeof(interesting) / sizeof(interesting[0]))

static void mutate(struct syscall_frame *frame)
{
    uint64_t *words = (uint64_t *)frame;
    size_t word_count = sizeof(*frame) / sizeof(words[0]);
    unsigned rounds = 1u + (unsigned)(next_random() % 6u);
    for (unsigned i = 0u; i < rounds; ++i) {
        size_t index = (size_t)(next_random() % word_count);
        switch (next_random() % 7u) {
        case 0u:
            words[index] ^= 1ULL << (next_random() % 64u);
            break;
        case 1u:
            words[index] = interesting[next_random() % INTERESTING_COUNT];
            break;
        case 2u:
            words[index] = next_random();
            break;
        case 3u: {
            size_t other = (size_t)(next_random() % word_count);
            uint64_t swap = words[index];
            words[index] = words[other];
            words[other] = swap;
            break;
        }
        case 4u:
            words[index] += (next_random() % 5u) - 2u;
            break;
        case 5u:
            words[index] = ~words[index];
            break;
        default:
            words[index] = (uint64_t)(uintptr_t)user_buffer
                + (next_random() % (FUZZ_USER_BYTES + 16u));
            break;
        }
    }
}

static uint64_t run_fuzz(uint64_t initial_state, unsigned iterations)
{
    rng_state = initial_state;
    uint64_t checksum = 1469598103934665603ULL;
    for (unsigned i = 0u; i < iterations; ++i) {
        struct syscall_frame frame = seeds[next_random() % seed_count];
        mutate(&frame);
        /* SYS_exit would halt the host process; it is exercised by QEMU tests. */
        if (frame.rax == SYS_exit) frame.rax = SYS_getpid;
        syscall_dispatch(&frame);
        checksum ^= frame.rax + (uint64_t)i;
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

int main(int argc, char **argv)
{
    unsigned iterations = 5000u;
    uint64_t seed = 0x424f4f4b383841ULL;
    const char *corpus = NULL;
    if (argc > 1) iterations = (unsigned)strtoul(argv[1], NULL, 0);
    if (argc > 2) seed = strtoull(argv[2], NULL, 0);
    if (argc > 3) corpus = argv[3];
    if (iterations == 0u || iterations > 2000000u) {
        fprintf(stderr, "iterations must be in 1..2000000\n");
        return 2;
    }
    init_seeds();
    load_corpus(corpus);
    uint64_t first = run_fuzz(seed, iterations);
    uint64_t replay = run_fuzz(seed, iterations);
    if (first != replay) {
        fprintf(stderr, "FAIL: fuzz replay diverged (%" PRIx64 " != %" PRIx64 ")\n",
                first, replay);
        return 1;
    }
    printf("PASS: %u syscall boundary cases from %zu seeds, checksum %016" PRIx64
           ", %u utility requests, %u console bytes\n",
           iterations, seed_count, first, utility_requests, console_bytes);
    return 0;
}
