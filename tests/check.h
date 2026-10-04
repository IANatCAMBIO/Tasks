/*
 * check.h — the test harness shared by every test program: a failure counter, a
 * scratch directory inside the sandbox, and a way to let the main queue
 * run.  Plain C; each test is a main() that returns check_result().
 *
 * Every test runs with TASKS_DATA_DIR pointing into dev/ (the Makefile
 * sets it), and check_scratch_dir refuses to run without it, so a test
 * binary started by hand cannot write into the per-user data directory.
 */

#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int check_failures;

/*
 * check — record one assertion.
 *
 * Inputs:
 *   ok   — the condition that should hold
 *   what — what was being checked, printed either way
 */
static inline void
check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) check_failures++;
}

/*
 * check_result — the test program's exit status.
 *
 * Output:
 *   0 when every check passed, 1 otherwise.
 */
static inline int
check_result(void)
{
    if (check_failures > 0) printf("%d check(s) failed\n", check_failures);
    return check_failures > 0 ? 1 : 0;
}

/*
 * check_scratch_dir — make a fresh, empty directory inside the sandbox
 * ($TASKS_DATA_DIR) for one test to work in.  Exits with status 2 when the
 * sandbox variable is not set or the directory cannot be made.
 *
 * Inputs:
 *   buf  — receives the directory's absolute path
 *   size — size of buf
 */
static inline void
check_scratch_dir(char *buf, size_t size)
{
    const char *base = getenv("TASKS_DATA_DIR");

    if (base == NULL || base[0] == '\0') {
        fprintf(stderr, "TASKS_DATA_DIR is not set; run the tests with `make test`\n");
        exit(2);
    }
    snprintf(buf, size, "%s/scratch-XXXXXX", base);
    if (mkdtemp(buf) == NULL) {
        perror(buf);
        exit(2);
    }
}

/*
 * check_spin — run the main run loop (and so the main dispatch queue) for a
 * while, letting timers and queued callbacks fire.
 *
 * Inputs:
 *   seconds — how long to run
 */
static inline void
check_spin(double seconds)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

#endif /* TESTS_CHECK_H */
