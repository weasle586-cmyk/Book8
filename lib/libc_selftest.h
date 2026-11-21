#ifndef BOOK8_LIBC_SELFTEST_H
#define BOOK8_LIBC_SELFTEST_H

typedef void (*libc_selftest_report_fn)(const char *message);

/* Runs all libc checks; failures are described through reporter (may be NULL).
 * Returns the number of failed checks. Leaves the heap as it found it. */
int libc_selftest(libc_selftest_report_fn reporter);
int libc_selftest_checks(void);

#endif
