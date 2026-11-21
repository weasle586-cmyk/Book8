#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libc_selftest.h"

static int failures;
static int checks;
static libc_selftest_report_fn report;

static void check(bool condition, const char *description, int line)
{
    char message[96];

    checks++;
    if (!condition) {
        failures++;
        snprintf(message, sizeof(message), "FAIL line %d: %s\n", line, description);
        if (report != NULL) {
            report(message);
        }
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

static bool format_is(const char *expected, const char *format, ...)
{
    char buf[160];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return strcmp(buf, expected) == 0 && (size_t)length == strlen(expected);
}

static void test_memory(void)
{
    unsigned char a[32];
    unsigned char b[32];

    for (int i = 0; i < 32; i++) {
        a[i] = (unsigned char)i;
    }
    CHECK(memset(b, 0xAB, sizeof(b)) == b && b[0] == 0xAB && b[31] == 0xAB);
    CHECK(memcpy(b, a, 32) == b && memcmp(a, b, 32) == 0);
    CHECK(memcmp("abc", "abd", 3) < 0 && memcmp("abd", "abc", 3) > 0);
    CHECK(memcmp("abc", "abd", 2) == 0 && memcmp("", "", 0) == 0);

    memcpy(b, a, 32);
    memmove(b + 4, b, 16); /* overlap, dest above src */
    CHECK(b[4] == 0 && b[19] == 15 && b[3] == 3 && b[20] == 20);
    memcpy(b, a, 32);
    memmove(b, b + 4, 16); /* overlap, dest below src */
    CHECK(b[0] == 4 && b[15] == 19 && b[16] == 16);
    memcpy(b, a, 32);
    memmove(b, b, 32);
    CHECK(memcmp(a, b, 32) == 0);
    CHECK(memchr(a, 7, 32) == a + 7 && memchr(a, 7, 7) == NULL);
}

static void test_strings(void)
{
    char buf[32];

    CHECK(strlen("") == 0 && strlen("hello") == 5 && strnlen("hello", 3) == 3);
    CHECK(strcmp("a", "a") == 0 && strcmp("a", "b") < 0 && strcmp("b", "a") > 0);
    CHECK(strcmp("ab", "a") > 0 && strcmp("\xff", "a") > 0);
    CHECK(strncmp("abcd", "abzz", 2) == 0 && strncmp("abcd", "abzz", 3) < 0);
    CHECK(strncmp("a", "a", 100) == 0);

    CHECK(strcpy(buf, "book8") == buf && strcmp(buf, "book8") == 0);
    memset(buf, 'x', sizeof(buf));
    strncpy(buf, "ab", 5);
    CHECK(buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\0' && buf[4] == '\0' &&
          buf[5] == 'x');
    strcpy(buf, "foo");
    CHECK(strcat(buf, "bar") == buf && strcmp(buf, "foobar") == 0);
    CHECK(strncat(buf, "bazqux", 3) == buf && strcmp(buf, "foobarbaz") == 0);
    CHECK(strchr(buf, 'b') == buf + 3 && strchr(buf, 'z') == buf + 8);
    CHECK(strchr(buf, '\0') == buf + 9 && strchr(buf, 'q') == NULL);
    CHECK(strrchr(buf, 'b') == buf + 6 && strrchr(buf, 'q') == NULL);
}

static void test_conversions(void)
{
    char buf[40];
    char *end;

    CHECK(atoi("42") == 42 && atoi("  -17x") == -17 && atoi("+5") == 5 && atoi("") == 0);
    CHECK(atol("123456789012") == 123456789012l);
    CHECK(strtol("0x1fZ", &end, 16) == 31 && *end == 'Z');
    CHECK(strtol("0755", &end, 0) == 493 && *end == '\0');
    CHECK(strtol("0x", &end, 0) == 0 && end[0] == 'x');
    CHECK(strtol("zz", &end, 36) == 1295);
    CHECK(strtol("abc", &end, 10) == 0 && end[0] == 'a');
    CHECK(strtol("-9223372036854775808", NULL, 10) == LONG_MIN);
    CHECK(strtol("9223372036854775808", NULL, 10) == LONG_MAX);
    CHECK(strtol("-9223372036854775809", NULL, 10) == LONG_MIN);
    CHECK(strtoul("18446744073709551615", NULL, 10) == ULONG_MAX);
    CHECK(strtoul("18446744073709551616", NULL, 10) == ULONG_MAX);
    CHECK(abs(-3) == 3 && abs(4) == 4 && labs(-5l) == 5l);

    CHECK(strcmp(itoa(0, buf, 10), "0") == 0);
    CHECK(strcmp(itoa(-2147483647 - 1, buf, 10), "-2147483648") == 0);
    CHECK(strcmp(itoa(255, buf, 16), "ff") == 0);
    CHECK(strcmp(itoa(5, buf, 2), "101") == 0);
    CHECK(strcmp(utoa(UINT64_MAX, buf, 10), "18446744073709551615") == 0);
    CHECK(hex_to_str(0xDEADBEEFull, buf, sizeof(buf)) == buf &&
          strcmp(buf, "deadbeef") == 0);
    CHECK(strcmp(hex_to_str(0, buf, sizeof(buf)), "0") == 0);
    CHECK(hex_to_str(UINT64_MAX, buf, 17) != NULL && strlen(buf) == 16);
    CHECK(hex_to_str(0x1234, buf, 4) == NULL && hex_to_str(0x123, buf, 4) != NULL);
}

static void test_format(void)
{
    char buf[8];
    int length;

    CHECK(format_is("hello", "hello"));
    CHECK(format_is("100%", "%d%%", 100));
    CHECK(format_is("-42 42 4294967295", "%d %i %u", -42, 42, 4294967295u));
    CHECK(format_is("ff FF 0xff 377", "%x %X %#x %o", 255, 255, 255, 255));
    CHECK(format_is("0", "%#x", 0));
    CHECK(format_is("A", "%c", 'A'));
    CHECK(format_is("str (null)", "%s %s", "str", (char *)NULL));
    CHECK(format_is("0xdeadbeef", "%p", (void *)(uintptr_t)0xDEADBEEFu));
    CHECK(format_is("(nil)", "%p", (void *)0));
    CHECK(format_is("[  12][12  ][0012][+12][ 12]", "[%4d][%-4d][%04d][%+d][% d]",
                    12, 12, 12, 12, 12));
    CHECK(format_is("[-012][ab][  ab]", "[%04d][%.2s][%4.2s]", -12, "abcd", "abcd"));
    CHECK(format_is("[00012][  abc]", "[%.5d][%*s]", 12, 5, "abc"));
    CHECK(format_is("[abc  ]", "[%-*s]", 5, "abc"));
    CHECK(format_is("-9223372036854775808", "%lld", (long long)INT64_MIN));
    CHECK(format_is("18446744073709551615", "%llu", (unsigned long long)UINT64_MAX));
    CHECK(format_is("ffffffffffffffff", "%lx", (unsigned long)UINT64_MAX));
    CHECK(format_is("255 -1 65535", "%hhu %hhd %hu", 255u, 255, 65535u));
    CHECK(format_is("123456789", "%zu", (size_t)123456789u));
    CHECK(format_is("", "%.0d", 0));
    CHECK(format_is("%q", "%q"));

    length = snprintf(buf, sizeof(buf), "%s", "0123456789");
    CHECK(length == 10 && strcmp(buf, "0123456") == 0);
    length = snprintf(buf, 1, "%s", "abc");
    CHECK(length == 3 && buf[0] == '\0');
    CHECK(snprintf(NULL, 0, "%d-%d", 12, 345) == 6);
    CHECK(sprintf(buf, "%d", 7) == 1 && strcmp(buf, "7") == 0);
}

static void test_heap(void)
{
    heap_stats_t before;
    heap_stats_t after;
    uint8_t *p[16];
    uint8_t *chain = NULL;
    size_t count = 0;

    CHECK(heap_check());
    heap_get_stats(&before);
    CHECK((before.start & (HEAP_PAGE_SIZE - 1u)) == 0);
    CHECK((before.end & (HEAP_PAGE_SIZE - 1u)) == 0 && before.total != 0);
    CHECK(before.used + before.free_bytes + 32u == before.total);

    CHECK(malloc(0) == NULL);
    for (size_t i = 0; i < 16; i++) {
        p[i] = malloc(1u + i * 37u);
        CHECK(p[i] != NULL && ((uintptr_t)p[i] & (HEAP_ALIGNMENT - 1u)) == 0);
        if (p[i] != NULL) {
            memset(p[i], (int)(i + 1), 1u + i * 37u);
        }
    }
    CHECK(heap_check());
    for (size_t i = 0; i < 16; i++) {
        bool intact = p[i] != NULL;
        for (size_t j = 0; intact && j < 1u + i * 37u; j++) {
            intact = p[i][j] == (uint8_t)(i + 1);
        }
        CHECK(intact);
    }
    for (size_t i = 0; i < 16; i += 2) {
        free(p[i]);
    }
    CHECK(heap_check());
    for (size_t i = 1; i < 16; i += 2) {
        free(p[i]);
    }
    free(NULL);
    CHECK(heap_check());
    heap_get_stats(&after);
    CHECK(after.free_blocks == before.free_blocks && after.free_bytes == before.free_bytes &&
          after.used == before.used);

    {
        uint8_t *z = calloc(64, 4);
        bool zeroed = z != NULL;
        for (size_t i = 0; zeroed && i < 256; i++) {
            zeroed = z[i] == 0;
        }
        CHECK(zeroed);
        free(z);
    }
    CHECK(calloc(SIZE_MAX / 2, 3) == NULL && calloc(SIZE_MAX, SIZE_MAX) == NULL);
    CHECK(malloc(SIZE_MAX) == NULL && malloc(SIZE_MAX - 16) == NULL);
    CHECK(malloc(HEAP_DEFAULT_SIZE) == NULL);

    {
        uint8_t *r = malloc(40);
        uint8_t *guard = malloc(40);
        bool kept = r != NULL && guard != NULL;
        for (size_t i = 0; kept && i < 40; i++) {
            r[i] = (uint8_t)(i * 3);
        }
        r = realloc(r, 2000); /* guard blocks in-place growth: must move */
        for (size_t i = 0; kept && r != NULL && i < 40; i++) {
            kept = r[i] == (uint8_t)(i * 3);
        }
        CHECK(kept && r != NULL);
        r = realloc(r, 20); /* shrink in place */
        CHECK(r != NULL && r[19] == (uint8_t)(19 * 3));
        r = realloc(r, 25);
        CHECK(r != NULL && r[19] == (uint8_t)(19 * 3));
        CHECK(realloc(r, 0) == NULL);
        r = realloc(NULL, 10);
        CHECK(r != NULL);
        free(r);
        free(guard);
        CHECK(heap_check());
    }

    /* Exhaust the heap with 1000-byte blocks linked through their payload. */
    for (;;) {
        uint8_t *block = malloc(1000);
        if (block == NULL) {
            break;
        }
        memcpy(block, &chain, sizeof(chain));
        chain = block;
        count++;
    }
    CHECK(count > 100 && malloc(1000) == NULL);
    CHECK(heap_check());
    while (chain != NULL) {
        uint8_t *next;
        memcpy(&next, chain, sizeof(next));
        free(chain);
        chain = next;
    }
    CHECK(heap_check());
    heap_get_stats(&after);
    CHECK(after.free_blocks <= before.free_blocks + 1u && after.largest_free >=
          before.largest_free);
    {
        void *whole = malloc(after.largest_free - 16u);
        CHECK(whole != NULL);
        free(whole);
    }
    CHECK(heap_check());
}

int libc_selftest(libc_selftest_report_fn reporter)
{
    report = reporter;
    failures = 0;
    checks = 0;
    test_memory();
    test_strings();
    test_conversions();
    test_format();
    test_heap();
    return failures;
}

int libc_selftest_checks(void)
{
    return checks;
}
