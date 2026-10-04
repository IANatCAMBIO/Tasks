/*
 * health_test.c — the Database plate's logic (src/task_health.h) and the
 * file health pass under it (task_db_health_check_file, db.h): the stamp
 * and the verdict's wording, what the check says about a sound, a damaged,
 * an unreadable and a missing file, and the background probe — what it
 * records, what it leaves alone, and that an answer about a database the
 * app no longer has open is dropped.
 */

#include "fixture.h"
#include "task_health.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* What the probe's callback saw. */
typedef struct {
    int     calls;
    bool    current;
    int64_t size;
    char    sha256[80];             /* "" when the probe had none          */
} seen_t;

/*
 * local_time — a local wall-clock moment as unix time, so that the stamp
 * tests read the same in every timezone.
 *
 * Inputs:
 *   year, month, day, hour, minute — the local date and time (month 1-12)
 *
 * Output:
 *   the unix time.
 */
static int64_t
local_time(int year, int month, int day, int hour, int minute)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                     .tm_hour = hour, .tm_min = minute, .tm_isdst = -1 };

    return (int64_t)mktime(&tm);
}

/*
 * stamp_is — whether task_health_stamp gives the wanted text.
 *
 * Inputs:
 *   when — the moment stamped
 *   now  — the moment it is judged from
 *   want — the expected text
 *
 * Output:
 *   non-zero when it matches.
 */
static int
stamp_is(int64_t when, int64_t now, const char *want)
{
    char *got = task_health_stamp(when, now);
    int   ok  = got != NULL && strcmp(got, want) == 0;

    if (!ok) printf("     stamp: got \"%s\", wanted \"%s\"\n", got != NULL ? got : "(null)", want);
    free(got);
    return ok;
}

/*
 * test_stamp — "HH:MM today" on the same local day, the date otherwise.
 */
static void
test_stamp(void)
{
    int64_t noon = local_time(2026, 9, 8, 12, 0);

    check(stamp_is(local_time(2026, 9, 8, 9, 2), noon, "09:02 today"),
          "stamp: earlier the same day is a time and \"today\"");
    check(stamp_is(local_time(2026, 9, 8, 0, 0), local_time(2026, 9, 8, 23, 59), "00:00 today"),
          "stamp: midnight and 23:59 are the same day");
    check(stamp_is(local_time(2026, 9, 7, 23, 59), noon, "Sep 7 at 23:59"),
          "stamp: a minute before midnight is yesterday's date");
    check(stamp_is(local_time(2026, 1, 31, 13, 24), noon, "Jan 31 at 13:24"),
          "stamp: another month, the day unpadded");
    check(stamp_is(local_time(2025, 9, 8, 9, 2), local_time(2026, 9, 8, 12, 0), "Sep 8 at 09:02"),
          "stamp: the same day of another year is not today");
    check(stamp_is(INT64_MAX, noon, "an unknown time"),
          "stamp: a moment that cannot be shown says so");
}

/*
 * describe_is — whether task_health_describe gives the wanted light, line
 * and detail.
 *
 * Inputs:
 *   health  — the verdict, or NULL
 *   now     — the moment it is judged from
 *   light   — the expected light
 *   summary — the expected line
 *   detail  — text the detail must contain
 *
 * Output:
 *   non-zero when all three match.
 */
static int
describe_is(const TaskDbHealth *health, int64_t now, task_health_light_t light,
            const char *summary, const char *detail)
{
    char *got_summary = NULL;
    char *got_detail  = NULL;
    int   ok = task_health_describe(health, now, &got_summary, &got_detail) == light &&
               got_summary != NULL && strcmp(got_summary, summary) == 0 &&
               got_detail != NULL && strstr(got_detail, detail) != NULL;

    if (!ok) {
        printf("     describe: got \"%s\" / \"%s\"\n", got_summary != NULL ? got_summary : "(null)",
               got_detail != NULL ? got_detail : "(null)");
    }
    free(got_summary);
    free(got_detail);
    return ok;
}

/*
 * test_describe — the four states of the health line.
 */
static void
test_describe(void)
{
    int64_t      now  = local_time(2026, 9, 8, 14, 0);
    int64_t      when = local_time(2026, 9, 8, 13, 24);
    char         words[] = "row 7 missing from index";
    TaskDbHealth passed  = { .ok = TRUE,  .ran = TRUE,  .when = when, .detail = NULL };
    TaskDbHealth damaged = { .ok = FALSE, .ran = TRUE,  .when = when, .detail = words };
    TaskDbHealth blocked = { .ok = FALSE, .ran = FALSE, .when = when, .detail = words };
    TaskDbHealth silent  = { .ok = FALSE, .ran = TRUE,  .when = when, .detail = NULL };

    check(describe_is(NULL, now, task_health_unknown, "Not checked", "Press Update"),
          "describe: no verdict is not green, and says how to get one");
    check(describe_is(&passed, now, task_health_good,
                      "Healthy \xe2\x80\x94 checked 13:24 today", "both passed"),
          "describe: a passed check is green and says when");
    check(describe_is(&damaged, now, task_health_bad,
                      "Problems found \xe2\x80\x94 13:24 today", words),
          "describe: damage is \"problems found\", with sqlite's words as the detail");
    check(describe_is(&blocked, now, task_health_bad,
                      "Check did not complete \xe2\x80\x94 13:24 today", words),
          "describe: a check that could not run is a different sentence");
    check(describe_is(&silent, now, task_health_bad,
                      "Problems found \xe2\x80\x94 13:24 today", "no further detail"),
          "describe: a failure with no words does not claim the checks passed");
}

/*
 * exists — whether a path exists.
 *
 * Inputs:
 *   path — the path
 *
 * Output:
 *   non-zero when it does.
 */
static int
exists(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0;
}

/*
 * test_check_file — the file health pass, against files in every state.
 *
 * Inputs:
 *   app — the fixture; its database is the sound file
 */
static void
test_check_file(TaskApp *app)
{
    char         path[1200];
    TaskDbHealth verdict = { 0 };
    int64_t      before  = (int64_t)time(NULL);

    check(task_db_health_check_file(app->db->path, &verdict) && verdict.ok && verdict.ran &&
          verdict.detail == NULL && verdict.when >= before,
          "check file: a sound database passes, with the time of the check");
    check(task_db_health(app->db) == NULL,
          "check file: ... and nothing is stored on the open handle");

    /* A file that is not a database: the check learned nothing. */
    snprintf(path, sizeof path, "%s.garbage", app->db->path);
    FILE *f = fopen(path, "w");
    fputs("this is not a SQLite database, only some text of about the right shape", f);
    fclose(f);
    memset(&verdict, 0, sizeof verdict);
    check(!task_db_health_check_file(path, &verdict) && !verdict.ok && !verdict.ran &&
          verdict.detail != NULL,
          "check file: a file that is not a database did not complete, with a reason");
    g_free(verdict.detail);

    /* A file that is not there is not created by being checked. */
    snprintf(path, sizeof path, "%s.missing", app->db->path);
    memset(&verdict, 0, sizeof verdict);
    check(!task_db_health_check_file(path, &verdict) && !verdict.ran && verdict.detail != NULL,
          "check file: a missing file did not complete");
    check(!exists(path), "check file: ... and was not created");
    g_free(verdict.detail);

    /* A broken reference: the checks ran and found something. */
    sqlite3_exec(app->db->sq,
                 "PRAGMA foreign_keys = OFF;"
                 "INSERT INTO attachments (task_id, path) VALUES (424242, '/nowhere');"
                 "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    memset(&verdict, 0, sizeof verdict);
    check(!task_db_health_check_file(app->db->path, &verdict) && verdict.ran &&
          verdict.detail != NULL && strstr(verdict.detail, "attachments") != NULL,
          "check file: a dangling reference is \"problems found\", naming the table");
    g_free(verdict.detail);
    sqlite3_exec(app->db->sq, "DELETE FROM attachments WHERE task_id = 424242", NULL, NULL, NULL);

    /* The same pass through the open handle agrees, and is stored. */
    check(task_db_health_check(app->db) && task_db_health(app->db) != NULL &&
          task_db_health(app->db)->ok && task_db_health(app->db)->ran,
          "check file: the open-handle check still records its verdict");

    /* task_db_verify_file is the same body, reporting less. */
    gchar *detail = NULL;
    check(task_db_verify_file(app->db->path, &detail) && detail == NULL,
          "check file: task_db_verify_file still passes a sound file");
    snprintf(path, sizeof path, "%s.garbage", app->db->path);
    check(!task_db_verify_file(path, &detail) && detail != NULL,
          "check file: ... and still fails a bad one with a reason");
    g_free(detail);
    check(!task_db_verify_file(path, NULL), "check file: ... with or without a detail pointer");
}

/*
 * on_probe — the probe's callback: keep what it reported.
 *
 * Inputs:
 *   app   — the context (unused)
 *   probe — the result (borrowed)
 *   data  — the seen_t
 */
static void
on_probe(TaskApp *app, const task_health_probe_t *probe, void *data)
{
    seen_t *seen = data;

    (void)app;
    seen->calls++;
    seen->current = probe->current;
    seen->size    = probe->size;
    snprintf(seen->sha256, sizeof seen->sha256, "%s", probe->sha256 != NULL ? probe->sha256 : "");
}

/*
 * run_probe — start a probe and let it finish.
 *
 * Inputs:
 *   app   — the context
 *   check — whether the integrity check is asked for
 *
 * Output:
 *   what the callback saw.
 */
static seen_t
run_probe(TaskApp *app, bool check)
{
    seen_t seen = { 0 };

    task_health_probe_start(app, check, on_probe, &seen);
    for (int i = 0; i < 100 && seen.calls == 0; i++) check_spin(0.05);
    return seen;
}

/*
 * forget_verdict — put the handle back to "never checked".
 *
 * Inputs:
 *   db — the database handle
 */
static void
forget_verdict(TaskDatabase *db)
{
    TaskDbHealth none = { 0 };

    task_db_health_record(db, &none);
}

/*
 * test_probe — the background probe against the fixture's database.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_probe(TaskApp *app)
{
    struct stat sb;

    task_db_list_create(app->db, "Probe", "");
    forget_verdict(app->db);
    check(task_db_health(app->db) == NULL, "probe: (the handle starts unchecked)");

    /* Without the check: the file's facts, and the verdict left alone. */
    seen_t  plain  = run_probe(app, false);
    gchar  *digest = task_db_file_sha256(app->db->path);
    check(plain.calls == 1 && plain.current, "probe: reports once, about the open database");
    check(stat(app->db->path, &sb) == 0 && plain.size == (int64_t)sb.st_size && plain.size > 0,
          "probe: the size is the file's size on disk");
    check(strlen(plain.sha256) == 64 && digest != NULL && strcmp(plain.sha256, digest) == 0,
          "probe: the digest is the file's SHA-256");
    check(task_db_health(app->db) == NULL, "probe: without the check no verdict is recorded");

    /* With the check: recorded on the handle, and the file untouched. */
    int64_t before  = (int64_t)time(NULL);
    seen_t  checked = run_probe(app, true);
    const TaskDbHealth *health = task_db_health(app->db);
    check(checked.calls == 1 && checked.current, "probe: a checking probe reports once");
    check(health != NULL && health->ok && health->ran && health->when >= before,
          "probe: its verdict is recorded on the open handle");
    check(strcmp(checked.sha256, plain.sha256) == 0,
          "probe: the check changed nothing in the file (same digest)");
    g_free(digest);

    /* The staleness guard: the app opens another database while a probe
     * is in flight.  The answer is about the old file and is dropped. */
    char other_path[1200];
    snprintf(other_path, sizeof other_path, "%s.other", app->db->path);
    TaskDatabase *first = app->db;
    TaskDatabase *other = task_db_open(other_path, NULL);
    check(other != NULL, "probe: (opened a second database)");

    forget_verdict(first);
    seen_t stale = { 0 };
    task_health_probe_start(app, true, on_probe, &stale);
    app->db = other;
    for (int i = 0; i < 100 && stale.calls == 0; i++) check_spin(0.05);
    check(stale.calls == 1 && !stale.current,
          "probe: an answer about a database no longer open is marked stale");
    check(task_db_health(other) == NULL && task_db_health(first) == NULL,
          "probe: ... and its verdict is recorded on neither handle");
    app->db = first;
    task_db_close(other);

    /* The database file vanishes under the open handle (the folder went to
     * the trash).  The probe must say so and must NOT put a file back. */
    char moved[1200];
    snprintf(moved, sizeof moved, "%s.moved", app->db->path);
    check(rename(app->db->path, moved) == 0, "probe: (moved the database file away)");
    seen_t gone = run_probe(app, true);
    health = task_db_health(app->db);
    check(gone.calls == 1 && gone.current && gone.size == -1 && gone.sha256[0] == '\0',
          "probe: a vanished file has no size and no digest");
    check(health != NULL && !health->ok && !health->ran && health->detail != NULL,
          "probe: ... and the check did not complete, with a reason");
    check(!exists(app->db->path), "probe: ... and no file was created in its place");
    check(rename(moved, app->db->path) == 0, "probe: (moved it back)");
}

/*
 * main — run the health tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    test_stamp();
    test_describe();

    TaskApp *app = fixture_open();
    test_check_file(app);
    test_probe(app);
    fixture_close(app);
    return check_result();
}
