/* ===========================================================================
 * cal_store.c — the calendar sync's own table (cal_store.h)
 * =========================================================================== */

#include "cal_store.h"

/*
 * prepare — compile one statement.  Failures are not logged: every reader
 * then returns nothing (the tables are the shared schema's, so a failure
 * here means a damaged file, which the health check reports).
 *
 * Inputs:
 *   db  — the connection
 *   sql — the statement
 *
 * Output:
 *   the statement (sqlite3_finalize it), or NULL.
 */
static sqlite3_stmt *
prepare(TaskDatabase *db, const gchar *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    return st;
}

/*
 * text — a text column as a new string, "" for NULL.
 *
 * Inputs:
 *   st  — the statement, on a row
 *   col — the column
 *
 * Output:
 *   the string (g_free).
 */
static gchar *
text(sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    return g_strdup(s != NULL ? (const gchar *)s : "");
}

/*
 * bind_text — bind a string parameter, copied.
 *
 * Inputs:
 *   st  — the statement
 *   idx — the parameter, 1-based
 *   s   — the string; NULL binds ""
 */
static void
bind_text(sqlite3_stmt *st, int idx, const gchar *s)
{
    sqlite3_bind_text(st, idx, s != NULL ? s : "", -1, SQLITE_TRANSIENT);
}

/*
 * cal_binding_free — free one binding and its strings.  NULL-safe.
 * Documented in cal_store.h.
 */
void
cal_binding_free(cal_binding_t *b)
{
    if (b == NULL) return;
    g_free(b->calendar_id);
    g_free(b->event_id);
    g_free(b->pushed_title);
    g_free(b);
}

/*
 * cal_store_bindings — every binding.
 * Documented in cal_store.h.
 */
GPtrArray *
cal_store_bindings(TaskDatabase *db)
{
    GPtrArray    *out = g_ptr_array_new_with_free_func((GDestroyNotify)cal_binding_free);
    sqlite3_stmt *st  = prepare(db,
        "SELECT task_id, calendar_id, event_id, pushed_start, pushed_duration, pushed_title "
        "  FROM cal_task ORDER BY task_id");
    if (st == NULL) return out;

    while (sqlite3_step(st) == SQLITE_ROW) {
        cal_binding_t *b = g_new0(cal_binding_t, 1);
        b->task_id         = sqlite3_column_int64(st, 0);
        b->calendar_id     = text(st, 1);
        b->event_id        = text(st, 2);
        b->pushed_start    = sqlite3_column_int64(st, 3);
        b->pushed_duration = sqlite3_column_int(st, 4);
        b->pushed_title    = text(st, 5);
        g_ptr_array_add(out, b);
    }
    sqlite3_finalize(st);
    return out;
}

/*
 * cal_store_bind — write a binding.
 * Documented in cal_store.h.
 */
void
cal_store_bind(TaskDatabase *db, const cal_binding_t *b)
{
    sqlite3_stmt *st = prepare(db,
        "INSERT OR REPLACE INTO cal_task "
        "  (task_id, calendar_id, event_id, pushed_start, pushed_duration, pushed_title) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    if (st == NULL) {
        g_warning("cal_store: cannot bind task %lld", (long long)b->task_id);
        return;
    }
    sqlite3_bind_int64(st, 1, b->task_id);
    bind_text(st, 2, b->calendar_id);
    bind_text(st, 3, b->event_id);
    sqlite3_bind_int64(st, 4, b->pushed_start);
    sqlite3_bind_int(st, 5, b->pushed_duration);
    bind_text(st, 6, b->pushed_title);
    if (sqlite3_step(st) != SQLITE_DONE)
        g_warning("cal_store: binding task %lld: %s", (long long)b->task_id,
                  sqlite3_errmsg(shared_db_handle(db->shared)));
    sqlite3_finalize(st);
}

/*
 * cal_store_unbind — drop one binding.
 * Documented in cal_store.h.
 */
void
cal_store_unbind(TaskDatabase *db, gint64 task_id)
{
    sqlite3_stmt *st = prepare(db, "DELETE FROM cal_task WHERE task_id = ?");
    if (st == NULL) return;
    sqlite3_bind_int64(st, 1, task_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/*
 * cal_store_dirty — whether a push is due.  The two halves are the two
 * directions a binding goes stale: a scheduled task that is unbound or
 * differs from its baseline, and a binding whose task is no longer
 * scheduled (or no longer exists).  Documented in cal_store.h.
 */
gboolean
cal_store_dirty(TaskDatabase *db)
{
    sqlite3_stmt *st = prepare(db,
        "SELECT 1 FROM tasks t LEFT JOIN cal_task g ON g.task_id = t.id "
        " WHERE t.deleted = 0 AND t.scheduled_start > 0 "
        "   AND (g.task_id IS NULL OR g.pushed_start != t.scheduled_start "
        "        OR g.pushed_duration != t.scheduled_duration OR g.pushed_title != t.title) "
        "UNION ALL "
        "SELECT 1 FROM cal_task g LEFT JOIN tasks t ON t.id = g.task_id "
        " WHERE t.id IS NULL OR t.deleted != 0 OR t.scheduled_start = 0 "
        "LIMIT 1");
    if (st == NULL) return FALSE;
    gboolean dirty = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return dirty;
}

/*
 * cal_store_reschedule — compare-and-set a task's session.  Like the
 * planner's own writes (task_db_task_set_scheduled) it does not stamp
 * updated_at: the session is local-only as far as the task's other
 * fields are concerned.  Documented in cal_store.h.
 */
gboolean
cal_store_reschedule(TaskDatabase *db, gint64 task_id, gint64 expect_start,
                      gint expect_dur, gint64 start, gint duration)
{
    sqlite3_stmt *st = prepare(db,
        "UPDATE tasks SET scheduled_start = ?, scheduled_duration = ? "
        " WHERE id = ? AND scheduled_start = ? AND scheduled_duration = ?");
    if (st == NULL) return FALSE;
    sqlite3_bind_int64(st, 1, start);
    sqlite3_bind_int(st, 2, duration > 0 ? duration
                                       : SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT);
    sqlite3_bind_int64(st, 3, task_id);
    sqlite3_bind_int64(st, 4, expect_start);
    sqlite3_bind_int(st, 5, expect_dur);
    gboolean ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(shared_db_handle(db->shared)) > 0;
    sqlite3_finalize(st);
    return ok;
}
