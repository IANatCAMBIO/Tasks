/*
 * check.h — the test harness shared by every test program: a failure counter, a
 * scratch directory inside the sandbox, and a way to let the main queue
 * run.  Plain C; each test is a main() that returns check_result().
 *
 * Every test runs with TASKS_DATA_DIR pointing into dev/ and
 * CAMBIO_SHARED_DB naming a file inside it (the Makefile sets both).
 * check_scratch_dir refuses to run without them, refuses a data directory
 * inside the GTK app's live data directory, and refuses a shared database
 * outside the sandbox, so a test binary started by hand cannot write into
 * the per-user data directory, the per-user shared database or the live
 * GTK data.  It then points CAMBIO_SHARED_DB at the new scratch directory,
 * so every database a test opens is inside it.
 */

#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared_db.h"
#include "task_paths.h"

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
 * check_inside — whether `path` lies strictly inside directory `dir`, by
 * the text of the two (both as the Makefile spells them: absolute, no "..").
 *
 * Inputs:
 *   path — the path asked about; NULL is "no"
 *   dir  — the directory
 *
 * Output:
 *   1 when path is dir + "/" + something without "..", 0 otherwise.
 */
static inline int
check_inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    return path != NULL && path[0] == '/' && strncmp(path, dir, n) == 0 &&
           path[n] == '/' && path[n + 1] != '\0' && strstr(path, "/..") == NULL;
}

/*
 * check_scratch_dir — make a fresh, empty directory inside the sandbox
 * ($TASKS_DATA_DIR) for one test to work in, and point CAMBIO_SHARED_DB at
 * a database file inside it.  Exits with status 2 when the sandbox variable
 * is not set or names the live data directory, when CAMBIO_SHARED_DB does
 * not already name a file inside the sandbox, or when the directory cannot
 * be made.
 *
 * Inputs:
 *   buf  — receives the directory's absolute path
 *   size — size of buf
 */
static inline void
check_scratch_dir(char *buf, size_t size)
{
    const char *base   = getenv(TASK_DATA_DIR_ENV);
    const char *shared = getenv(SHARED_DB_ENV_VAR);
    char        db[1100];

    if (base == NULL || base[0] != '/' || task_path_is_live_data(base)) {
        fprintf(stderr, "%s is not set to a sandbox; run the tests with `make test`\n",
                TASK_DATA_DIR_ENV);
        exit(2);
    }
    if (!check_inside(shared, base)) {
        fprintf(stderr, "%s does not name a file inside %s; run the tests with `make test`\n",
                SHARED_DB_ENV_VAR, base);
        exit(2);
    }
    snprintf(buf, size, "%s/scratch-XXXXXX", base);
    if (mkdtemp(buf) == NULL) {
        perror(buf);
        exit(2);
    }
    snprintf(db, sizeof db, "%s/%s", buf, SHARED_DB_FILE_NAME);
    setenv(SHARED_DB_ENV_VAR, db, 1);
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
