/*
 * fixture.h — a throwaway application context for the tests that need a
 * database: a fresh sandbox directory, the config loaded from it, the
 * built-in views registered, and an empty shared database in it
 * (CAMBIO_SHARED_DB, which check_scratch_dir points there) opened through
 * the core.
 */

#ifndef TESTS_FIXTURE_H
#define TESTS_FIXTURE_H

#include "check.h"
#include "core_views.h"
#include "task_app.h"
#include "task_config.h"
#include "task_paths.h"

/*
 * fixture_open — build the context.  Exits with status 2 when the sandbox
 * cannot be set up.
 *
 * Output:
 *   a TaskApp with an empty database; release with fixture_close.
 */
static inline TaskApp *
fixture_open(void)
{
    char scratch[1024];

    check_scratch_dir(scratch, sizeof scratch);
    setenv(TASK_DATA_DIR_ENV, scratch, 1);
    if (!task_config_init()) {
        fprintf(stderr, "fixture: cannot initialise the config in %s\n", scratch);
        exit(2);
    }
    task_core_views_init();

    TaskApp *app  = task_app_new();
    char    *path = shared_db_path(NULL);
    app->db = path != NULL ? task_db_open(path, NULL) : NULL;
    free(path);
    if (app->db == NULL) {
        fprintf(stderr, "fixture: cannot open a database in %s\n", scratch);
        exit(2);
    }
    return app;
}

/*
 * fixture_close — close the database and the config.
 *
 * Inputs:
 *   app — the context from fixture_open
 */
static inline void
fixture_close(TaskApp *app)
{
    task_app_free(app);
    task_config_close();
}

/*
 * fixture_link_to_note — make a task a Notes action item, as Notes writes
 * one: a note, and a note_task row naming the task.  What tells the tests
 * of anything that treats Notes-born tasks differently (a purge, the
 * cleanup pass) that this task came from a note.
 *
 * Inputs:
 *   app     — the fixture
 *   task_id — the task
 *   title   — the note's title
 */
static inline void
fixture_link_to_note(TaskApp *app, gint64 task_id, const char *title)
{
    char *sql = sqlite3_mprintf(
        "INSERT INTO notes (title, created_at, updated_at) VALUES (%Q, 0, 0);"
        "INSERT INTO note_task (task_id, note_id, ord) VALUES (%lld, last_insert_rowid(), 0);"
        "UPDATE action_uid_seq SET next = (SELECT max(uid) + 1 FROM note_task);",
        title, (long long)task_id);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);
}

/*
 * fixture_row_int — the first column of the first row of a query (a count,
 * or one value of one row): how a test looks at a table the core's API
 * does not expose.
 *
 * Inputs:
 *   app — the fixture
 *   sql — the query
 *
 * Output:
 *   the value, -1 on error or when there is no row.
 */
static inline gint64
fixture_row_int(TaskApp *app, const char *sql)
{
    sqlite3_stmt *st = NULL;
    gint64        n  = -1;

    if (sqlite3_prepare_v2(shared_db_handle(app->db->shared), sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

#endif /* TESTS_FIXTURE_H */
