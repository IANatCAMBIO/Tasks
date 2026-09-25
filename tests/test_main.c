/*
 * test_main.c — logic test suite for Tasks.
 *
 * Exercises search-query parsing, recurrence arithmetic, and the database
 * schema without any GTK display.  Link against build/{app,db,search,recur}.o
 * via the Makefile's `make test` target.
 *
 * Run:  make test
 */
#include <glib.h>
#include <glib/gstdio.h>
#include "app.h"
#include "db.h"
#include "search.h"
#include "recur.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/*
 * make_task — allocate a stack-initialised Task with title + notes.
 * The caller owns it; free with task_free().
 */
static Task *
make_task(const gchar *title, const gchar *notes)
{
    Task *t   = g_new0(Task, 1);
    t->title  = g_strdup(title != NULL ? title : "");
    t->notes  = g_strdup(notes != NULL ? notes  : "");
    t->status = TASK_STATUS_NEW;
    return t;
}

/* ---------------------------------------------------------------------------
 * Search tests
 * ------------------------------------------------------------------------- */

static void
test_search_null_returns_null(void)
{
    g_assert_null(task_search_parse(NULL));
    g_assert_null(task_search_parse(""));
    g_assert_null(task_search_parse("   "));
    g_assert_null(task_search_parse("-"));          /* bare exclude: no terms */
}

static void
test_search_basic_match(void)
{
    TaskSearch *q = task_search_parse("groceries");
    g_assert_nonnull(q);

    Task *yes = make_task("Buy groceries", NULL);
    Task *no  = make_task("Call dentist",  NULL);

    g_assert_true(task_search_matches(q, yes, NULL));
    g_assert_false(task_search_matches(q, no,  NULL));

    task_free(yes);
    task_free(no);
    task_search_free(q);
}

static void
test_search_case_insensitive(void)
{
    TaskSearch *q = task_search_parse("GROCERIES");
    g_assert_nonnull(q);

    Task *t = make_task("Buy groceries", NULL);
    g_assert_true(task_search_matches(q, t, NULL));
    task_free(t);
    task_search_free(q);
}

static void
test_search_multi_term_and(void)
{
    TaskSearch *q = task_search_parse("buy milk");
    g_assert_nonnull(q);

    Task *both = make_task("Buy milk and bread", NULL);
    Task *one  = make_task("Buy bread",          NULL);

    g_assert_true(task_search_matches(q, both, NULL));
    g_assert_false(task_search_matches(q, one,  NULL));

    task_free(both);
    task_free(one);
    task_search_free(q);
}

static void
test_search_exclude(void)
{
    TaskSearch *q = task_search_parse("buy -milk");
    g_assert_nonnull(q);

    Task *included = make_task("Buy bread",       NULL);
    Task *excluded = make_task("Buy milk and bread", NULL);

    g_assert_true(task_search_matches(q, included, NULL));
    g_assert_false(task_search_matches(q, excluded, NULL));

    task_free(included);
    task_free(excluded);
    task_search_free(q);
}

static void
test_search_notes_field(void)
{
    TaskSearch *q = task_search_parse("urgent");
    g_assert_nonnull(q);

    Task *in_notes = make_task("Weekly review", "urgent: prepare agenda");
    Task *absent   = make_task("Weekly review", "nothing special");

    g_assert_true(task_search_matches(q, in_notes, NULL));
    g_assert_false(task_search_matches(q, absent,   NULL));

    task_free(in_notes);
    task_free(absent);
    task_search_free(q);
}

/* ---------------------------------------------------------------------------
 * Recurrence arithmetic tests
 * ------------------------------------------------------------------------- */

/* Convenience: unix time for a specific local date at midnight. */
static gint64
local_midnight(gint year, gint month, gint day)
{
    GTimeZone *tz = task_local_tz();
    GDateTime *dt = g_date_time_new(tz, year, month, day, 0, 0, 0);
    gint64 ts = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return ts;
}

static void
test_recur_advance_daily(void)
{
    gint64 jan1  = local_midnight(2026, 1, 1);
    gint64 jan2  = local_midnight(2026, 1, 2);
    gint64 result = task_recur_advance(jan1, TASK_RECUR_DAY, 1, 0);
    g_assert_cmpint(result, ==, jan2);
}

static void
test_recur_advance_weekly(void)
{
    gint64 jan1  = local_midnight(2026, 1,  1);
    gint64 jan8  = local_midnight(2026, 1,  8);
    gint64 result = task_recur_advance(jan1, TASK_RECUR_WEEK, 1, 0);
    g_assert_cmpint(result, ==, jan8);
}

static void
test_recur_advance_monthly(void)
{
    gint64 jan1  = local_midnight(2026, 1, 1);
    gint64 feb1  = local_midnight(2026, 2, 1);
    gint64 result = task_recur_advance(jan1, TASK_RECUR_MONTH, 1, 0);
    g_assert_cmpint(result, ==, feb1);
}

static void
test_recur_monthly_clamp(void)
{
    /* Jan 31 + 1 month → Feb 28 (not the 31st, which doesn't exist).
     * The clamp is STICKY: the next step is Mar 28, not Mar 31.          */
    gint64 jan31  = local_midnight(2026, 1, 31);
    gint64 feb28  = local_midnight(2026, 2, 28);
    gint64 mar28  = local_midnight(2026, 3, 28);

    gint64 step1 = task_recur_advance(jan31, TASK_RECUR_MONTH, 1, 0);
    gint64 step2 = task_recur_advance(step1, TASK_RECUR_MONTH, 1, 0);

    g_assert_cmpint(step1, ==, feb28);
    g_assert_cmpint(step2, ==, mar28);
}

static void
test_recur_advance_interval(void)
{
    /* every 2 weeks */
    gint64 jan1  = local_midnight(2026, 1,  1);
    gint64 jan15 = local_midnight(2026, 1, 15);
    gint64 result = task_recur_advance(jan1, TASK_RECUR_WEEK, 2, 0);
    g_assert_cmpint(result, ==, jan15);
}

static void
test_recur_lead_default_weekly(void)
{
    /* weekly: half of 7 days = 3.5, rounded down to 3 days = 4320 min */
    gint lead = task_recur_lead_default(TASK_RECUR_WEEK, 1);
    g_assert_cmpint(lead, ==, 3 * 24 * 60);
}

static void
test_recur_lead_default_daily(void)
{
    /* daily: half of 1 day = 12 hours = 720 min */
    gint lead = task_recur_lead_default(TASK_RECUR_DAY, 1);
    g_assert_cmpint(lead, ==, 12 * 60);
}

static void
test_recur_lead_default_hourly(void)
{
    /* hourly: half of 60 min = 30 min */
    gint lead = task_recur_lead_default(TASK_RECUR_HOUR, 1);
    g_assert_cmpint(lead, ==, 30);
}

static void
test_recur_lead_default_nonrecurring(void)
{
    /* interval=0 means does not recur — lead default is 0 */
    gint lead = task_recur_lead_default(TASK_RECUR_DAY, 0);
    g_assert_cmpint(lead, ==, 0);
}

/* ---------------------------------------------------------------------------
 * Database schema tests
 * ------------------------------------------------------------------------- */

static void
test_db_fresh_schema(void)
{
    GError *err   = NULL;
    gchar  *tmpdir = g_dir_make_tmp("tasks-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(tmpdir);

    gchar *path = g_build_filename(tmpdir, "tasks.db", NULL);

    TaskDatabase *db = task_db_open(path, &err);
    g_assert_no_error(err);
    g_assert_nonnull(db);

    task_db_close(db);

    g_remove(path);
    g_remove(tmpdir);
    g_free(path);
    g_free(tmpdir);
}

static void
test_db_create_read_task(void)
{
    GError *err    = NULL;
    gchar  *tmpdir = g_dir_make_tmp("tasks-test-XXXXXX", &err);
    g_assert_no_error(err);

    gchar *path = g_build_filename(tmpdir, "tasks.db", NULL);
    TaskDatabase *db = task_db_open(path, &err);
    g_assert_no_error(err);
    g_assert_nonnull(db);

    /* Create a list, then a task in it. */
    gint64 list_id = task_db_list_create(db, "Test List", NULL);
    g_assert_cmpint(list_id, >, 0);

    gint64 task_id = task_db_task_create(db, list_id, 0, "Walk the dog");
    g_assert_cmpint(task_id, >, 0);

    /* Read the task back and check its fields. */
    Task *t = task_db_task_get(db, task_id);
    g_assert_nonnull(t);
    g_assert_cmpstr(t->title, ==, "Walk the dog");
    g_assert_cmpint(t->list_id, ==, list_id);
    g_assert_cmpint(t->parent_id, ==, 0);
    g_assert_cmpint(t->status, ==, TASK_STATUS_NEW);
    g_assert_cmpint(t->updated_at, >, 0);   /* stamped on create */
    task_free(t);

    task_db_close(db);
    g_remove(path);
    g_remove(tmpdir);
    g_free(path);
    g_free(tmpdir);
}

static void
test_db_status_stamps_updated_at(void)
{
    GError *err    = NULL;
    gchar  *tmpdir = g_dir_make_tmp("tasks-test-XXXXXX", &err);
    g_assert_no_error(err);

    gchar *path = g_build_filename(tmpdir, "tasks.db", NULL);
    TaskDatabase *db = task_db_open(path, &err);
    g_assert_no_error(err);

    gint64 list_id = task_db_list_create(db, "L", NULL);
    gint64 task_id = task_db_task_create(db, list_id, 0, "Task");

    Task *before = task_db_task_get(db, task_id);
    gint64 at_create = before->updated_at;
    task_free(before);

    /* Sleep 1 second so the clock moves. */
    g_usleep(G_USEC_PER_SEC);

    task_db_task_set_status(db, task_id, TASK_STATUS_DONE);

    Task *after = task_db_task_get(db, task_id);
    g_assert_cmpint(after->updated_at, >, at_create);
    g_assert_cmpint(after->status, ==, TASK_STATUS_DONE);
    task_free(after);

    task_db_close(db);
    g_remove(path);
    g_remove(tmpdir);
    g_free(path);
    g_free(tmpdir);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Search */
    g_test_add_func("/search/null_returns_null",    test_search_null_returns_null);
    g_test_add_func("/search/basic_match",          test_search_basic_match);
    g_test_add_func("/search/case_insensitive",     test_search_case_insensitive);
    g_test_add_func("/search/multi_term_and",       test_search_multi_term_and);
    g_test_add_func("/search/exclude",              test_search_exclude);
    g_test_add_func("/search/notes_field",          test_search_notes_field);

    /* Recurrence */
    g_test_add_func("/recur/advance_daily",         test_recur_advance_daily);
    g_test_add_func("/recur/advance_weekly",        test_recur_advance_weekly);
    g_test_add_func("/recur/advance_monthly",       test_recur_advance_monthly);
    g_test_add_func("/recur/monthly_clamp",         test_recur_monthly_clamp);
    g_test_add_func("/recur/advance_interval",      test_recur_advance_interval);
    g_test_add_func("/recur/lead_default_weekly",   test_recur_lead_default_weekly);
    g_test_add_func("/recur/lead_default_daily",    test_recur_lead_default_daily);
    g_test_add_func("/recur/lead_default_hourly",   test_recur_lead_default_hourly);
    g_test_add_func("/recur/lead_default_nonrecurring", test_recur_lead_default_nonrecurring);

    /* Database */
    g_test_add_func("/db/fresh_schema",             test_db_fresh_schema);
    g_test_add_func("/db/create_read_task",         test_db_create_read_task);
    g_test_add_func("/db/status_stamps_updated_at", test_db_status_stamps_updated_at);

    return g_test_run();
}
