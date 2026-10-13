/* ===========================================================================
 * gcal_store.c — the Google Calendar sync's own tables (gcal_store.h)
 * =========================================================================== */

#include "gcal_store.h"

#include <string.h>

/* The sync_state key naming the account the bindings belong to. */
#define ACCOUNT_KEY "gcal_account"

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
 * run — execute statements with no result; failures are logged.
 *
 * Inputs:
 *   db  — the connection
 *   sql — the statements
 *
 * Output:
 *   TRUE when everything ran.
 */
static gboolean
run(TaskDatabase *db, const gchar *sql)
{
    gchar *msg = NULL;
    if (sqlite3_exec(shared_db_handle(db->shared), sql, NULL, NULL, &msg) != SQLITE_OK) {
        g_warning("gcal_store: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    return TRUE;
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
 * gcal_calendar_free / gcal_event_free / gcal_binding_free — free one
 * row and its strings.  NULL-safe.  Documented in gcal_store.h.
 */
void
gcal_calendar_free(gcal_calendar_t *c)
{
    if (c == NULL) return;
    g_free(c->id);
    g_free(c->summary);
    g_free(c->color);
    g_free(c);
}

void
gcal_event_free(gcal_event_t *e)
{
    if (e == NULL) return;
    g_free(e->calendar_id);
    g_free(e->event_id);
    g_free(e->title);
    g_free(e->color);
    g_free(e);
}

void
gcal_binding_free(gcal_binding_t *b)
{
    if (b == NULL) return;
    g_free(b->calendar_id);
    g_free(b->event_id);
    g_free(b->pushed_title);
    g_free(b);
}

/*
 * gcal_store_calendars — the stored calendar list.
 * Documented in gcal_store.h.
 */
GPtrArray *
gcal_store_calendars(TaskDatabase *db)
{
    GPtrArray    *out = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_calendar_free);
    sqlite3_stmt *st  = prepare(db,
        "SELECT id, summary, color, writable, is_primary FROM gcal_calendar "
        "ORDER BY is_primary DESC, summary COLLATE NOCASE");
    if (st == NULL) return out;

    while (sqlite3_step(st) == SQLITE_ROW) {
        gcal_calendar_t *c = g_new0(gcal_calendar_t, 1);
        c->id         = text(st, 0);
        c->summary    = text(st, 1);
        c->color      = text(st, 2);
        c->writable   = sqlite3_column_int(st, 3) != 0;
        c->is_primary = sqlite3_column_int(st, 4) != 0;
        g_ptr_array_add(out, c);
    }
    sqlite3_finalize(st);
    return out;
}

/*
 * gcal_store_set_calendars — replace the calendar list, in one
 * transaction (nothing is written when it cannot begin).  Documented in
 * gcal_store.h.
 */
gboolean
gcal_store_set_calendars(TaskDatabase *db, GPtrArray *calendars)
{
    if (!task_db_transaction_begin(db)) return FALSE;
    gboolean      ok  = run(db, "DELETE FROM gcal_calendar");
    sqlite3_stmt *st  = prepare(db,
        "INSERT OR REPLACE INTO gcal_calendar (id, summary, color, writable, is_primary) "
        "VALUES (?, ?, ?, ?, ?)");
    for (guint i = 0; ok && st != NULL && i < calendars->len; i++) {
        const gcal_calendar_t *c = g_ptr_array_index(calendars, i);
        bind_text(st, 1, c->id);
        bind_text(st, 2, c->summary);
        bind_text(st, 3, c->color);
        sqlite3_bind_int(st, 4, c->writable ? 1 : 0);
        sqlite3_bind_int(st, 5, c->is_primary ? 1 : 0);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return task_db_transaction_end(db, ok && st != NULL);
}

/*
 * gcal_store_set_events — replace one calendar's cached events, in one
 * transaction (nothing is written when it cannot begin).  Documented in
 * gcal_store.h.
 */
gboolean
gcal_store_set_events(TaskDatabase *db, const gchar *calendar_id, GPtrArray *events)
{
    if (!task_db_transaction_begin(db)) return FALSE;
    sqlite3_stmt *del = prepare(db, "DELETE FROM gcal_event WHERE calendar_id = ?");
    sqlite3_stmt *ins = prepare(db,
        "INSERT OR REPLACE INTO gcal_event (calendar_id, event_id, title, start, end, all_day) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    gboolean ok = del != NULL && ins != NULL;
    if (ok) {
        bind_text(del, 1, calendar_id);
        ok = sqlite3_step(del) == SQLITE_DONE;
    }
    for (guint i = 0; ok && i < events->len; i++) {
        const gcal_event_t *e = g_ptr_array_index(events, i);
        bind_text(ins, 1, calendar_id);
        bind_text(ins, 2, e->event_id);
        bind_text(ins, 3, e->title);
        sqlite3_bind_int64(ins, 4, e->start);
        sqlite3_bind_int64(ins, 5, e->end);
        sqlite3_bind_int(ins, 6, e->all_day ? 1 : 0);
        ok = sqlite3_step(ins) == SQLITE_DONE;
        sqlite3_reset(ins);
    }
    sqlite3_finalize(del);
    sqlite3_finalize(ins);
    return task_db_transaction_end(db, ok);
}

/*
 * gcal_store_keep_events — drop the events of calendars not named.
 * Documented in gcal_store.h.
 */
void
gcal_store_keep_events(TaskDatabase *db, GPtrArray *keep)
{
    GString *sql = g_string_new("DELETE FROM gcal_event WHERE calendar_id NOT IN (");
    for (guint i = 0; i < keep->len; i++) g_string_append(sql, i == 0 ? "?" : ", ?");
    g_string_append(sql, ")");

    sqlite3_stmt *st = prepare(db, sql->str);
    g_string_free(sql, TRUE);
    if (st == NULL) return;
    for (guint i = 0; i < keep->len; i++) bind_text(st, (int)i + 1, g_ptr_array_index(keep, i));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/*
 * gcal_store_events_for_day — the cached events overlapping a day.
 * Documented in gcal_store.h.
 */
GPtrArray *
gcal_store_events_for_day(TaskDatabase *db, gint64 lo, gint64 hi)
{
    GPtrArray    *out = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_event_free);
    sqlite3_stmt *st  = prepare(db,
        "SELECT e.calendar_id, e.event_id, e.title, COALESCE(c.color, ''), "
        "       e.start, e.end, e.all_day "
        "  FROM gcal_event e LEFT JOIN gcal_calendar c ON c.id = e.calendar_id "
        " WHERE e.start < ? AND e.end > ? "
        " ORDER BY e.all_day DESC, e.start, e.end DESC");
    if (st == NULL) return out;

    sqlite3_bind_int64(st, 1, hi);
    sqlite3_bind_int64(st, 2, lo);
    while (sqlite3_step(st) == SQLITE_ROW) {
        gcal_event_t *e = g_new0(gcal_event_t, 1);
        e->calendar_id = text(st, 0);
        e->event_id    = text(st, 1);
        e->title       = text(st, 2);
        e->color       = text(st, 3);
        e->start       = sqlite3_column_int64(st, 4);
        e->end         = sqlite3_column_int64(st, 5);
        e->all_day     = sqlite3_column_int(st, 6) != 0;
        g_ptr_array_add(out, e);
    }
    sqlite3_finalize(st);
    return out;
}

/*
 * gcal_store_bindings — every binding.
 * Documented in gcal_store.h.
 */
GPtrArray *
gcal_store_bindings(TaskDatabase *db)
{
    GPtrArray    *out = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_binding_free);
    sqlite3_stmt *st  = prepare(db,
        "SELECT task_id, calendar_id, event_id, pushed_start, pushed_duration, pushed_title "
        "  FROM gcal_task ORDER BY task_id");
    if (st == NULL) return out;

    while (sqlite3_step(st) == SQLITE_ROW) {
        gcal_binding_t *b = g_new0(gcal_binding_t, 1);
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
 * gcal_store_bind — write a binding.
 * Documented in gcal_store.h.
 */
void
gcal_store_bind(TaskDatabase *db, const gcal_binding_t *b)
{
    sqlite3_stmt *st = prepare(db,
        "INSERT OR REPLACE INTO gcal_task "
        "  (task_id, calendar_id, event_id, pushed_start, pushed_duration, pushed_title) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    if (st == NULL) {
        g_warning("gcal_store: cannot bind task %lld", (long long)b->task_id);
        return;
    }
    sqlite3_bind_int64(st, 1, b->task_id);
    bind_text(st, 2, b->calendar_id);
    bind_text(st, 3, b->event_id);
    sqlite3_bind_int64(st, 4, b->pushed_start);
    sqlite3_bind_int(st, 5, b->pushed_duration);
    bind_text(st, 6, b->pushed_title);
    if (sqlite3_step(st) != SQLITE_DONE)
        g_warning("gcal_store: binding task %lld: %s", (long long)b->task_id,
                  sqlite3_errmsg(shared_db_handle(db->shared)));
    sqlite3_finalize(st);
}

/*
 * gcal_store_unbind — drop one binding, or all.
 * Documented in gcal_store.h.
 */
void
gcal_store_unbind(TaskDatabase *db, gint64 task_id)
{
    sqlite3_stmt *st = prepare(db, task_id != 0 ? "DELETE FROM gcal_task WHERE task_id = ?"
                                                : "DELETE FROM gcal_task");
    if (st == NULL) return;
    if (task_id != 0) sqlite3_bind_int64(st, 1, task_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/*
 * gcal_store_dirty — whether a push is due.  The two halves are the two
 * directions a binding goes stale: a scheduled task that is unbound or
 * differs from its baseline, and a binding whose task is no longer
 * scheduled (or no longer exists).  Documented in gcal_store.h.
 */
gboolean
gcal_store_dirty(TaskDatabase *db)
{
    sqlite3_stmt *st = prepare(db,
        "SELECT 1 FROM tasks t LEFT JOIN gcal_task g ON g.task_id = t.id "
        " WHERE t.deleted = 0 AND t.scheduled_start > 0 "
        "   AND (g.task_id IS NULL OR g.pushed_start != t.scheduled_start "
        "        OR g.pushed_duration != t.scheduled_duration OR g.pushed_title != t.title) "
        "UNION ALL "
        "SELECT 1 FROM gcal_task g LEFT JOIN tasks t ON t.id = g.task_id "
        " WHERE t.id IS NULL OR t.deleted != 0 OR t.scheduled_start = 0 "
        "LIMIT 1");
    if (st == NULL) return FALSE;
    gboolean dirty = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return dirty;
}

/*
 * gcal_store_reschedule — compare-and-set a task's session.  Like the
 * planner's own writes (task_db_task_set_scheduled) it does not stamp
 * updated_at: the session is local-only as far as the task's other
 * fields are concerned.  Documented in gcal_store.h.
 */
gboolean
gcal_store_reschedule(TaskDatabase *db, gint64 task_id, gint64 expect_start,
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

/*
 * gcal_store_account — the account the bindings belong to.
 * Documented in gcal_store.h.
 */
gchar *
gcal_store_account(TaskDatabase *db)
{
    sqlite3_stmt *st = prepare(db, "SELECT value FROM sync_state WHERE key = '" ACCOUNT_KEY "'");
    if (st == NULL) return NULL;
    gchar *account = sqlite3_step(st) == SQLITE_ROW ? text(st, 0) : NULL;
    sqlite3_finalize(st);
    return account;
}

/*
 * gcal_store_set_account — record the account the bindings belong to.
 * Documented in gcal_store.h.
 */
void
gcal_store_set_account(TaskDatabase *db, const gchar *account)
{
    sqlite3_stmt *st = prepare(db,
        "INSERT OR REPLACE INTO sync_state (key, value) VALUES ('" ACCOUNT_KEY "', ?)");
    if (st == NULL) return;
    bind_text(st, 1, account);
    sqlite3_step(st);
    sqlite3_finalize(st);
}
