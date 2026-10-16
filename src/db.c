/* ===========================================================================
 * db.c — SQLite storage for Tasks (see db.h)
 * =========================================================================== */

#include "db.h"
#include "task_paths.h"
#include <glib/gstdio.h>             /* g_unlink, g_stat                    */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * revs_moved — which remembered change revisions differ from those just
 * polled, adopting the polled ones.
 *
 * Inputs:
 *   db        — the connection
 *   rev_tasks — the SHARED_DB_REV_TASKS revision just polled
 *   rev_notes — the SHARED_DB_REV_NOTES revision just polled
 *
 * Output:
 *   TASK_DB_CHANGED_* bits of the revisions that moved.
 */
static guint
revs_moved(TaskDatabase *db, int64_t rev_tasks, int64_t rev_notes)
{
    guint moved = (rev_tasks != db->tasks_rev ? TASK_DB_CHANGED_TASKS : 0) |
                  (rev_notes != db->notes_rev ? TASK_DB_CHANGED_NOTES : 0);
    db->tasks_rev = rev_tasks;
    db->notes_rev = rev_notes;
    return moved;
}

/*
 * own_commit — this connection has just committed: poll again
 * (shared_db_poll) so its own writes are never taken for another app's
 * (task_db_outside_change).  When no other connection committed since the
 * last poll, every revision movement is this connection's own: adopt the
 * revisions.  When one did, its change cannot be told apart from ours:
 * every revision that moved is kept pending, so the next
 * task_db_outside_change reports it.  A failed poll changes nothing: the
 * module then still reports the outside commit at the next poll.
 *
 * Inputs:
 *   db — the connection, outside any transaction
 */
static void
own_commit(TaskDatabase *db)
{
    int     changed   = 0;
    int64_t rev_tasks = 0;
    int64_t rev_notes = 0;
    if (shared_db_poll(db->shared, &changed, &rev_tasks, &rev_notes, NULL) != 0)
        return;
    guint moved = revs_moved(db, rev_tasks, rev_notes);
    if (changed)
        db->outside_pending |= moved;
}

/* ---------------------------------------------------------------------------
 * exec() — run one or more statements with no results; failures are
 * logged, not fatal.  Returns TRUE when everything ran (and, outside a
 * transaction, committed: own_commit then notes it).
 * ------------------------------------------------------------------------- */
static gboolean
exec(TaskDatabase *db, const gchar *sql)
{
    gchar *msg = NULL;               /* sqlite's error text                 */
    if (sqlite3_exec(shared_db_handle(db->shared), sql, NULL, NULL, &msg) != SQLITE_OK) {
        g_warning("db: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    if (sqlite3_get_autocommit(shared_db_handle(db->shared)))
        own_commit(db);              /* it committed on its own             */
    return TRUE;
}

/* task_db_transaction_begin() — see db.h.  The shared module's BEGIN
 * IMMEDIATE; a failure is logged.                                           */
gboolean
task_db_transaction_begin(TaskDatabase *db)
{
    char *why = NULL;
    if (shared_db_begin(db->shared, &why) != 0) {
        g_warning("db: BEGIN: %s", why != NULL ? why : "?");
        free(why);
        return FALSE;
    }
    return TRUE;
}

/* task_db_transaction_end() — see db.h.  shared_db_commit rolls back by
 * itself when the COMMIT fails; a failure is logged.                        */
gboolean
task_db_transaction_end(TaskDatabase *db, gboolean ok)
{
    char *why = NULL;
    if (!ok) {
        shared_db_rollback(db->shared);
        return FALSE;
    }
    if (shared_db_commit(db->shared, &why) != 0) {
        g_warning("db: COMMIT: %s", why != NULL ? why : "?");
        free(why);
        return FALSE;
    }
    own_commit(db);
    return TRUE;
}

/*
 * txn_join — make sure a write transaction is open: BEGIN IMMEDIATE when
 * none is, else join the caller's.  Pairs with txn_leave.
 *
 * Inputs:
 *   db  — the connection
 *   own — out: TRUE when this call began the transaction
 *
 * Output:
 *   TRUE when a transaction is open; FALSE when BEGIN failed (logged).
 */
static gboolean
txn_join(TaskDatabase *db, gboolean *own)
{
    *own = sqlite3_get_autocommit(shared_db_handle(db->shared)) != 0;
    return !*own || task_db_transaction_begin(db);
}

/*
 * txn_leave — end what txn_join began: commit, or roll back when `ok` is
 * FALSE.  A joined transaction is left to its owner, who learns of a
 * failure from the result.
 *
 * Inputs:
 *   db  — the connection
 *   own — txn_join's `own`
 *   ok  — whether every write inside succeeded
 *
 * Output:
 *   TRUE when the writes succeeded (and, when owned, committed).
 */
static gboolean
txn_leave(TaskDatabase *db, gboolean own, gboolean ok)
{
    return own ? task_db_transaction_end(db, ok) : ok;
}

/* ---------------------------------------------------------------------------
 * step_done() — run a prepared WRITE statement to completion, logging
 * sqlite's message when it fails (SQLITE_BUSY against the sync worker,
 * constraint violations, I/O errors) — silent write loss is the one
 * unacceptable outcome.
 *   db  — the connection (for the error text).
 *   st  — the prepared, bound statement, or NULL when the PREPARE
 *         itself failed (also logged); the caller finalizes either way.
 *   ctx — short operation name for the log line.
 *   sum — for a statement with a RETURNING clause: receives the sum of
 *         the integer first column of the rows it returned; else NULL.
 * Returns TRUE when the statement completed (and, outside a transaction,
 * committed: own_commit then notes it).
 * ------------------------------------------------------------------------- */
static gboolean
step_done(TaskDatabase *db, sqlite3_stmt *st, const gchar *ctx, gint *sum)
{
    if (st == NULL) {
        g_warning("db: %s: prepare failed: %s", ctx,
                  sqlite3_errmsg(shared_db_handle(db->shared)));
        return FALSE;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        if (sum != NULL) *sum += sqlite3_column_int(st, 0);
    if (rc != SQLITE_DONE) {
        g_warning("db: %s: %s", ctx, sqlite3_errmsg(shared_db_handle(db->shared)));
        return FALSE;
    }
    if (sqlite3_get_autocommit(shared_db_handle(db->shared)))
        own_commit(db);              /* it committed on its own             */
    return TRUE;
}

/* column_text_dup() — g_strdup a TEXT column, NULL when SQL NULL.
 *   st/col — the row being read and the 0-based column index.              */
static gchar *
column_text_dup(sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    return s != NULL ? g_strdup((const gchar *)s) : NULL;
}

/* The status codes as SQL literals (shared_db.h defines the values once).  */
#define DONE_SQL SHARED_DB_STR(SHARED_DB_TASK_STATUS_DONE)
#define NEW_SQL  SHARED_DB_STR(SHARED_DB_TASK_STATUS_NEW)

/* now() — current unix time, the updated_at stamp.                         */
static gint64
now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---------------------------------------------------------------------------
 * Struct free helpers.
 * ------------------------------------------------------------------------- */
/* task_free() — free one task and its owned strings.  NULL-safe.        */
void
task_free(Task *t)
{
    if (t == NULL)
        return;
    g_free(t->title);
    g_free(t->notes);
    g_free(t);
}

/* task_dup() — a deep copy of a task (see db.h).                         */
Task *
task_dup(const Task *t)
{
    Task *copy  = g_new(Task, 1);
    *copy       = *t;
    copy->title = g_strdup(t->title != NULL ? t->title : "");
    copy->notes = g_strdup(t->notes != NULL ? t->notes : "");
    return copy;
}

/* task_status_label() — the user-facing status name (see db.h).  The
 * default arm is not dead code: the value comes off disk, so a
 * hand-edited or future-version row can carry anything at all, and a
 * NULL here would blank the whole cell rather than one word of it.        */
const gchar *
task_status_label(TaskStatus status)
{
    switch (status) {
    case TASK_STATUS_IN_PROGRESS: return "In Progress";
    case TASK_STATUS_DONE:        return "Done";
    case TASK_STATUS_NEW:
    default:                    return "New";
    }
}

/* task_status_apply_done() — fold a binary done flag into the tri-state
 * (see db.h).  Every done-only source funnels through here so the
 * "untick means In Progress, and only from Done" rule has exactly one
 * definition; the SQL paths that need the OLD row value spell the same
 * rule as a CASE and say so.                                               */
TaskStatus
task_status_apply_done(TaskStatus cur, gboolean done)
{
    if (done)
        return TASK_STATUS_DONE;
    return cur == TASK_STATUS_DONE ? TASK_STATUS_IN_PROGRESS : cur;
}

/* task_attachment_free() — free one attachment row.  NULL-safe.            */
static void
task_attachment_free(TaskAttachment *a)
{
    if (a == NULL)
        return;
    g_free(a->path);
    g_free(a);
}

/* task_ptr_array_free_tasks() — free an array of Task*.  NULL-safe.        */
void
task_ptr_array_free_tasks(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        task_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* task_ptr_array_free_attachments() — free TaskAttachment*s.  NULL-safe.   */
void
task_ptr_array_free_attachments(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        task_attachment_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/*
 * check_verdict — turn one answer of the shared module's check
 * (shared_db_check_summary / shared_db_check_file: 0 clean, 1 findings,
 * -1 did not run) into Tasks' verdict.  "Ran" is kept apart from "passed":
 * a check that could not run reports nothing, which must never read as a
 * clean result.
 *
 * Inputs:
 *   rc     — the module's answer
 *   ran    — whether the check executed (the module's, for a file)
 *   found  — the findings text (malloc'd, NULL unless rc == 1); consumed
 *   why    — why it did not run (malloc'd, may be NULL); consumed
 *   ran_out — optional out: TRUE when the check actually executed
 *   detail — optional out: the findings or why the check could not run,
 *            one per line (g_free), NULL on success
 *
 * Output:
 *   TRUE only when the check ran and found nothing.
 */
static gboolean
check_verdict(int rc, gboolean ran, char *found, char *why, gboolean *ran_out,
              gchar **detail)
{
    if (ran_out != NULL)
        *ran_out = ran;
    if (detail != NULL)
        *detail = rc == 1 ? g_strdup(found)
                : rc < 0  ? g_strdup(why != NULL ? why : "the check did not run")
                          : NULL;
    free(found);
    free(why);
    return rc == 0;
}

/*
 * db_check — the shared module's full check (integrity_check,
 * foreign_key_check, the stamps, WAL and the cross-table invariants) on an
 * open connection: task_db_health_check's body, and through it the startup
 * check.
 *
 * Inputs:
 *   sdb    — an open connection (read-only or read-write), not NULL
 *   ran    — optional out: TRUE when the check actually executed
 *   detail — optional out: as for check_verdict
 *
 * Output:
 *   TRUE only when the check ran and found nothing.
 */
static gboolean
db_check(shared_db *sdb, gboolean *ran, gchar **detail)
{
    char *found = NULL;
    char *why   = NULL;
    int   rc    = shared_db_check_summary(sdb, 1, &found, &why);
    return check_verdict(rc, rc >= 0, found, why, ran, detail);
}

/* What every entry point that takes a path says when that path is inside
 * the GTK app's live data directory (task_paths.h).  This app refuses to
 * read or write there, whoever asks.                                       */
#define LIVE_DATA_REFUSAL \
    "refused: %s is inside the live Tasks data directory, which this " \
    "app never opens"

/* ---------------------------------------------------------------------------
 * verify_file() — the check on a file nothing has open
 * (shared_db_check_file: a separate read-only connection, which refuses a
 * file that is not a stamped shared database), after Tasks' own refusal of
 * the GTK app's live data: the body shared by task_db_verify_file and
 * task_db_health_check_file, which differ only in what they report.
 *
 * Inputs:
 *   path   — the database file
 *   ran    — optional out: TRUE when the check actually executed; FALSE
 *            when the file was refused or could not be opened
 *   detail — optional out: what went wrong (g_free), NULL when sound
 *
 * Output:
 *   TRUE only when the check ran and passed.
 * ------------------------------------------------------------------------- */
static gboolean
verify_file(const gchar *path, gboolean *ran, gchar **detail)
{
    if (task_path_is_live_data(path)) {
        if (ran != NULL)
            *ran = FALSE;
        if (detail != NULL)
            *detail = g_strdup_printf(LIVE_DATA_REFUSAL, path);
        return FALSE;
    }
    char *found  = NULL;
    char *why    = NULL;
    int   ran_rc = 0;
    int   rc     = shared_db_check_file(path, 1, &ran_rc, &found, &why);
    return check_verdict(rc, ran_rc != 0, found, why, ran, detail);
}

/* task_db_verify_file() — see db.h.                                        */
gboolean
task_db_verify_file(const gchar *path, gchar **detail)
{
    return verify_file(path, NULL, detail);
}

/* ---------------------------------------------------------------------------
 * task_db_file_sha256() — hex SHA-256 of the file at `path` (see db.h).
 *
 * Read in fixed-size chunks rather than slurped: this runs against the
 * live database, whose size is the user's business and not ours to hold
 * in memory twice.
 * ------------------------------------------------------------------------- */
#define SHA_CHUNK 65536              /* bytes per read                      */

gchar *
task_db_file_sha256(const gchar *path)
{
    if (task_path_is_live_data(path))
        return NULL;
    FILE *f = g_fopen(path, "rb");
    if (f == NULL)
        return NULL;

    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    guchar    *buf = g_malloc(SHA_CHUNK);
    gsize      n;                    /* bytes in the current chunk          */
    while ((n = fread(buf, 1, SHA_CHUNK, f)) > 0)
        g_checksum_update(sum, buf, (gssize)n);
    gboolean bad = ferror(f) != 0;
    g_free(buf);
    fclose(f);

    gchar *hex = bad ? NULL : g_strdup(g_checksum_get_string(sum));
    g_checksum_free(sum);
    return hex;
}

/* ---------------------------------------------------------------------------
 * task_db_health_check() / task_db_health() — the health of the open
 * database, held on the connection (see db.h).
 *
 * Nothing is written to the database.  Recording the verdict IN it made
 * the check change the bytes it had just measured, which moved the digest
 * shown beside it on every run.
 * ------------------------------------------------------------------------- */
gboolean
task_db_health_check(TaskDatabase *db)
{
    TaskDbHealth verdict = { 0 };

    verdict.ok   = db_check(db->shared, &verdict.ran, &verdict.detail);
    verdict.when = g_get_real_time() / G_USEC_PER_SEC;
    task_db_health_record(db, &verdict);
    return db->health.ok;
}

/* task_db_health_check_file() — see db.h.                                  */
gboolean
task_db_health_check_file(const gchar *path, TaskDbHealth *out)
{
    out->ok   = verify_file(path, &out->ran, &out->detail);
    out->when = g_get_real_time() / G_USEC_PER_SEC;
    return out->ok;
}

/* task_db_health_record() — see db.h.                                      */
void
task_db_health_record(TaskDatabase *db, TaskDbHealth *verdict)
{
    g_free(db->health.detail);
    db->health      = *verdict;      /* the detail changes hands            */
    verdict->detail = NULL;
}

const TaskDbHealth *
task_db_health(TaskDatabase *db)
{
    return db->health.when != 0 ? &db->health : NULL;
}

/* ---------------------------------------------------------------------------
 * open_task_db() — the body of task_db_open and task_db_open_existing.  The
 * schema, the stamps, the refusal of any other file, WAL and the pragmas
 * are the shared module's (shared_db.h); this adds only the fence round
 * the GTK app's live data.
 *
 * Inputs:
 *   path   — the database file.
 *   create — TRUE: a missing file is created (shared_db_open); FALSE: it is
 *            refused (shared_db_open_existing).
 *   err    — optional error out-param, set to the module's own words.
 *
 * Output:
 *   the handle, or NULL with `err` set.
 * ------------------------------------------------------------------------- */
static TaskDatabase *
open_task_db(const gchar *path, gboolean create, GError **err)
{
    if (task_path_is_live_data(path)) {
        g_set_error(err, g_quark_from_static_string("task-db"), 2,
                    LIVE_DATA_REFUSAL, path);
        return NULL;
    }
    char      *why    = NULL;
    shared_db *shared = create ? shared_db_open(path, &why)
                               : shared_db_open_existing(path, &why);
    if (shared == NULL) {
        g_set_error(err, g_quark_from_static_string("task-db"), 1, "%s",
                    why != NULL ? why : "the database cannot be opened");
        free(why);
        return NULL;
    }
    TaskDatabase *db = g_new0(TaskDatabase, 1);
    db->shared = shared;
    /* The data as opened is the data the app loads first.  A failed poll
     * leaves -1, which no stored revision equals: the first outside
     * change then refreshes.                                              */
    int changed = 0;
    if (shared_db_poll(shared, &changed, &db->tasks_rev, &db->notes_rev, NULL) != 0)
        db->tasks_rev = db->notes_rev = -1;
    return db;
}

/* task_db_open() — see db.h.                                               */
TaskDatabase *
task_db_open(const gchar *path, GError **err)
{
    return open_task_db(path, TRUE, err);
}

/* task_db_open_existing() — see db.h.                                      */
TaskDatabase *
task_db_open_existing(const gchar *path, GError **err)
{
    return open_task_db(path, FALSE, err);
}

/* task_db_outside_change() — see db.h.                                     */
guint
task_db_outside_change(TaskDatabase *db)
{
    int     changed   = 0;
    int64_t rev_tasks = 0;
    int64_t rev_notes = 0;
    if (shared_db_poll(db->shared, &changed, &rev_tasks, &rev_notes, NULL) != 0)
        return TASK_DB_CHANGED_TASKS | TASK_DB_CHANGED_NOTES;
    guint found = db->outside_pending;
    db->outside_pending = 0;
    if (changed)
        found |= revs_moved(db, rev_tasks, rev_notes);
    return found;
}

/* task_db_close() — close the connection (see db.h).                       */
void
task_db_close(TaskDatabase *db)
{
    if (db == NULL)
        return;
    shared_db_close(db->shared);
    g_free(db->health.detail);
    g_free(db);
}

/*
 * tombstone_each — tombstone each task `parents` names, with its live
 * subtasks, in ONE statement: SHARED_DB_SQL_TOMBSTONE_WHERE applied to
 * every id the subquery yields (already-deleted rows keep their stamp).
 * The subquery is wrapped in a derived table whose only column is `pid`,
 * so the fragment's unqualified columns name the row being updated.
 *
 * Inputs:
 *   db      — the connection
 *   parents — a SELECT of one column named pid; it may use ?2
 *   owner   — the value bound to ?2
 *   ctx     — short operation name for the log line
 *
 * Output:
 *   how many top-level tasks (parent_id NULL) were tombstoned, or -1 when
 *   the statement failed (logged).  Outside a transaction it commits on
 *   its own (own_commit then notes it).
 */
static gint
tombstone_each(TaskDatabase *db, const gchar *parents, gint64 owner, const gchar *ctx)
{
    gchar *sql = g_strconcat(
        "UPDATE tasks SET deleted = 1, updated_at = ?1 "
        "  WHERE EXISTS (SELECT 1 FROM (", parents, ") "
        "                 WHERE " SHARED_DB_SQL_TOMBSTONE_WHERE("pid") ") "
        "  RETURNING parent_id IS NULL", NULL);
    sqlite3_stmt *st  = NULL;
    gint          top = 0;
    gboolean      ok  = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now());
        sqlite3_bind_int64(st, 2, owner);
        ok = step_done(db, st, ctx, &top);
    } else {
        step_done(db, NULL, ctx, NULL);
    }
    sqlite3_finalize(st);
    g_free(sql);
    return ok ? top : -1;
}

/* ---------------------------------------------------------------------------
 * read_task() — build a Task from the standard tasks SELECT.
 * ------------------------------------------------------------------------- */
#define TASK_COLS_WITH(notes) \
                  "id, COALESCE(parent_id, 0), title, " notes ", due, " \
                  "status, position, updated_at, deleted, "\
                  "completed_at, priority, "\
                  "recur_interval, recur_unit, recur_time, recur_lead, "\
                  "recur_next, recur_start, due_time, "\
                  "scheduled_start, scheduled_duration"
#define TASK_COLS       TASK_COLS_WITH("notes")
/* The same shape with the notes left out (an empty string in their
 * place), for a query whose rows are read for their titles alone: the
 * notes are the bulk of a task's bytes.                                    */
#define TASK_COLS_BRIEF TASK_COLS_WITH("''")

static Task *
read_task(sqlite3_stmt *st)
{
    Task *t = g_new0(Task, 1);
    t->id           = sqlite3_column_int64(st, 0);
    t->parent_id    = sqlite3_column_int64(st, 1);
    t->title        = column_text_dup(st, 2);
    t->notes        = column_text_dup(st, 3);
    t->due          = sqlite3_column_int64(st, 4);
    t->status       = (TaskStatus)sqlite3_column_int(st, 5);
    t->position     = sqlite3_column_int(st, 6);
    t->updated_at   = sqlite3_column_int64(st, 7);
    t->deleted      = sqlite3_column_int(st, 8) != 0;
    t->completed_at = sqlite3_column_int64(st, 9);
    t->priority     = sqlite3_column_int(st, 10) != 0;
    t->recur_interval = sqlite3_column_int(st, 11);
    /* Clamped on the way IN rather than at every reader: recur_unit is an
     * enum index into the unit tables in recur.c and the editor's combo,
     * and a hand-edited or future-version value must not index off the end
     * of either.  Minutes is the safe floor for the same reason New is
     * TaskStatus's (see editor_status_get).                                */
    {
        gint u = sqlite3_column_int(st, 12);
        t->recur_unit = (u >= 0 && u < TASK_RECUR_N_UNITS)
                        ? (TaskRecurUnit)u : TASK_RECUR_MINUTE;
    }
    t->recur_time   = sqlite3_column_int(st, 13);
    t->recur_lead   = sqlite3_column_int(st, 14);
    t->recur_next   = sqlite3_column_int64(st, 15);
    t->recur_start  = sqlite3_column_int64(st, 16);
    /* Clamped on the way IN, like recur_unit above: a hand-edited or
     * out-of-range value would otherwise format as a nonsense clock time
     * on every row that shows one.                                       */
    {
        gint m = sqlite3_column_int(st, 17);
        t->due_time = (m >= 0 && m <= 23 * 60 + 59) ? m
                                                    : SHARED_DB_TASK_DUE_TIME_DEFAULT;
    }
    t->scheduled_start    = sqlite3_column_int64(st, 18);
    t->scheduled_duration = sqlite3_column_int(st, 19);
    if (t->scheduled_duration <= 0)
        t->scheduled_duration = SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT;
    if (t->title == NULL) t->title = g_strdup("");
    if (t->notes == NULL) t->notes = g_strdup("");
    return t;
}

/* ---------------------------------------------------------------------------
 * task_query() — run a tasks SELECT (already using TASK_COLS) and
 * collect the rows.
 *   db    — the connection.
 *   sql   — the full statement text.
 *   nbind — how many of a/b to bind as int64 parameters ?1/?2 (0-2).
 * Returns a GPtrArray of Task* (possibly empty, never NULL); free
 * with task_ptr_array_free_tasks.  Prepare failures are logged and yield
 * the empty array.
 * ------------------------------------------------------------------------- */
static GPtrArray *
task_query(TaskDatabase *db, const gchar *sql, gint nbind, gint64 a, gint64 b)
{
    GPtrArray *out = g_ptr_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK) {
        if (nbind >= 1) sqlite3_bind_int64(st, 1, a);
        if (nbind >= 2) sqlite3_bind_int64(st, 2, b);
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_task(st));
    } else {
        step_done(db, NULL, "task query", NULL);
    }
    sqlite3_finalize(st);
    return out;
}

/* task_db_task_get() — one task row; NULL if absent.                       */
Task *
task_db_task_get(TaskDatabase *db, gint64 id)
{
    GPtrArray *a = task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE id = ?", 1, id, 0);
    Task *t = a->len > 0 ? g_ptr_array_index(a, 0) : NULL;
    g_ptr_array_free(a, TRUE);
    return t;
}

/* View queries sort `priority DESC` first: a high-priority task rises
 * to the top of every view it appears in (within its parent group for
 * subtask queries).                                                        */

/* task_db_subtasks() — visible subtasks of a task.                         */
GPtrArray *
task_db_subtasks(TaskDatabase *db, gint64 parent_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id = ? AND "
        "deleted = 0 ORDER BY priority DESC, position, id",
        1, parent_id, 0);
}

/* task_db_subtasks_all_visible() — every visible subtask, one query, their
 * notes left out (db.h): the pane reads their titles and statuses.          */
GPtrArray *
task_db_subtasks_all_visible(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS_BRIEF " FROM tasks WHERE parent_id IS NOT NULL AND "
        "deleted = 0 ORDER BY parent_id, priority DESC, position, id",
        0, 0, 0);
}

/* The order of every view: high priority first, then by position — one
 * sequence across all top-level tasks (the shared schema's migration from
 * version 100 renumbered the per-list positions into it).  A subtask shown on a row of its own
 * (a tag or Favorites gathers any level) sorts right after its parent, by
 * its position among its siblings.                                         */
#define VIEW_ORDER \
    " ORDER BY priority DESC," \
    " COALESCE((SELECT p.position FROM tasks p WHERE p.id = tasks.parent_id), position)," \
    " parent_id IS NOT NULL, position, id"

/* task_db_tasks_all_visible() — the All Tasks view (top-level tasks; their
 * subtasks render inside the rows as usual).                               */
GPtrArray *
task_db_tasks_all_visible(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NULL AND "
        "deleted = 0" VIEW_ORDER,
        0, 0, 0);
}

/*
 * task_db_tasks_favorite — the Favorites view (see db.h).
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   GPtrArray of Task* (possibly empty, never NULL); free with
 *   task_ptr_array_free_tasks.
 */
GPtrArray *
task_db_tasks_favorite(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE deleted = 0 AND id IN "
        "(SELECT tt.task_id FROM task_tags tt JOIN tags n "
        " ON n.id = tt.tag_id WHERE n.favorite_tasks <> 0)" VIEW_ORDER,
        0, 0, 0);
}

/* task_db_tasks_due_between() — the 3-Day Forecast view and
 * the notification worker (see db.h).                                      */
GPtrArray *
task_db_tasks_due_between(TaskDatabase *db, gint64 lo, gint64 hi, gboolean open_only)
{
    return task_query(db,
        /* due_time joins the sort so a day's tasks read in the order they
         * actually come due — `due` alone is midnight for every row in
         * the bucket and so cannot separate them (the 3-Day Forecast,
         * which spans several days, is the visible case).                */
        open_only
        ? "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
          "deleted = 0 AND status <> " DONE_SQL " ORDER BY priority DESC, due, due_time, "
          "position"
        : "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
          "deleted = 0 ORDER BY priority DESC, due, due_time, position",
        2, lo, hi);
}

/*
 * task_db_task_create — a task and its tags, in one transaction (see
 * db.h).  A top-level task goes at the end of the one top-level order; a
 * subtask at the end of its siblings, with every tag its parent carries.
 *
 * Inputs:
 *   db        — the connection
 *   parent_id — 0, or the live top-level parent
 *   title     — the title
 *   tag       — the tag's name; required for a top-level task
 *
 * Output:
 *   the new id, or 0 when refused or a write failed.
 */
gint64
task_db_task_create(TaskDatabase *db, gint64 parent_id, const gchar *title,
                    const gchar *tag)
{
    gchar   *clean   = g_strstrip(g_strdup(tag != NULL ? tag : ""));
    gboolean has_tag = clean[0] != '\0';
    g_free(clean);
    if (parent_id == 0 && !has_tag)
        return 0;
    if (parent_id != 0) {
        Task *p = task_db_task_get(db, parent_id);
        gboolean ok = p != NULL && p->parent_id == 0 && !p->deleted;
        task_free(p);
        if (!ok)
            return 0;
    }

    gboolean own;
    if (!txn_join(db, &own))
        return 0;
    gboolean      ok = TRUE;
    sqlite3_stmt *st = NULL;
    gint64        id = 0;            /* the new rowid                       */
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "INSERT INTO tasks(parent_id, title, position, updated_at) "
            "VALUES(?1, ?2, (SELECT COALESCE(MAX(position), 0) + 1 FROM tasks "
            "                WHERE parent_id IS ?1), ?3)", -1,
            &st, NULL) == SQLITE_OK) {
        if (parent_id != 0)
            sqlite3_bind_int64(st, 1, parent_id);
        else
            sqlite3_bind_null(st, 1);
        sqlite3_bind_text(st, 2, title, -1, SQLITE_TRANSIENT);
        /* due_time is left to the column DEFAULT, as Notes leaves it: the
         * shared module alone creates the schema, from
         * SHARED_DB_TASK_DUE_TIME_DEFAULT, so the DEFAULT is that value.  */
        sqlite3_bind_int64(st, 3, now());
        ok = step_done(db, st, "task create", NULL);
        if (ok)
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        ok = step_done(db, NULL, "task create", NULL);
    }
    sqlite3_finalize(st);

    if (ok && parent_id != 0) {
        gchar *sql = sqlite3_mprintf(
            "INSERT INTO task_tags (task_id, tag_id) "
            "SELECT %lld, tag_id FROM task_tags WHERE task_id = %lld",
            (long long)id, (long long)parent_id);
        ok = exec(db, sql);
        sqlite3_free(sql);
    }
    if (ok && has_tag)
        ok = task_db_task_tag_add(db, id, tag) != 0;
    return txn_leave(db, own, ok) ? id : 0;
}

/*
 * parent_started — a completed subtask means its parent has been worked
 * on, so move that parent from New to In Progress.
 *
 * Inputs:
 *   db       — open database
 *   child_id — the task that has just become Done
 *
 * Output:
 *   TRUE when the statement ran (logged when not).  It changes nothing
 *   unless there is a parent AND that parent is New:
 *   a top-level task's parent_id is NULL, so `id = (SELECT parent_id …)`
 *   matches no row and no branch is needed; an In Progress parent is
 *   already right; and a DONE parent is deliberately LEFT DONE — ticking
 *   one more child is progress, not regress, and dragging a finished
 *   parent backwards is the user's call, not a side effect.
 *
 * Only New → In Progress, so this never stamps or clears completed_at and
 * can never cascade: the rule fires on Done, and In Progress is not Done,
 * so promoting a parent cannot promote a grandparent.
 *
 * Callers run it in the SAME transaction as the child's own write
 * (txn_join), so the two rows change together and a held lock is waited
 * out once, not once per row.
 */
static gboolean
parent_started(TaskDatabase *db, gint64 child_id)
{
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE tasks SET status = ?1, updated_at = ?2 "
            "WHERE id = (SELECT parent_id FROM tasks WHERE id = ?3) "
            "AND status = ?4 AND deleted = 0", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, (gint)TASK_STATUS_IN_PROGRESS);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_int64(st, 3, child_id);
        sqlite3_bind_int(st, 4, (gint)TASK_STATUS_NEW);
        ok = step_done(db, st, "parent started", NULL);
    } else {
        step_done(db, NULL, "parent started", NULL);
    }
    sqlite3_finalize(st);
    return ok;
}

/*
 * The editable columns task_db_task_update compares and writes: the
 * column, how its value is held in a Task, where, and whether a change to
 * it is a change to a SYNCED field (stamps updated_at).  One table, so the
 * comparison, the SET list and the binding cannot disagree.
 */
typedef enum {
    EDIT_TEXT,                      /* gchar *                             */
    EDIT_INT,                       /* gint, or an enum stored as one      */
    EDIT_BOOL,                      /* gboolean, written 0 / 1             */
    EDIT_INT64                      /* gint64                              */
} edit_kind_t;

static const struct {
    const char  *column;
    edit_kind_t  kind;
    size_t       offset;
    gboolean     synced;
} edit_columns[] = {
    { "title",          EDIT_TEXT,  offsetof(Task, title),          TRUE  },
    { "notes",          EDIT_TEXT,  offsetof(Task, notes),          TRUE  },
    { "due",            EDIT_INT64, offsetof(Task, due),            TRUE  },
    { "due_time",       EDIT_INT,   offsetof(Task, due_time),       TRUE  },
    { "status",         EDIT_INT,   offsetof(Task, status),         TRUE  },
    { "priority",       EDIT_BOOL,  offsetof(Task, priority),       FALSE },
    { "recur_interval", EDIT_INT,   offsetof(Task, recur_interval), FALSE },
    { "recur_unit",     EDIT_INT,   offsetof(Task, recur_unit),     FALSE },
    { "recur_time",     EDIT_INT,   offsetof(Task, recur_time),     FALSE },
    { "recur_lead",     EDIT_INT,   offsetof(Task, recur_lead),     FALSE },
    { "recur_next",     EDIT_INT64, offsetof(Task, recur_next),     FALSE },
    { "recur_start",    EDIT_INT64, offsetof(Task, recur_start),    FALSE },
};

/*
 * edit_differs — whether one editable column differs between two tasks.
 *
 * Inputs:
 *   i      — index into edit_columns
 *   before — the row as loaded
 *   after  — the row as edited
 *
 * Output:
 *   TRUE when the values differ (texts by content; NULL reads as "").
 */
static gboolean
edit_differs(size_t i, const Task *before, const Task *after)
{
    const char *a = (const char *)before + edit_columns[i].offset;
    const char *b = (const char *)after + edit_columns[i].offset;

    switch (edit_columns[i].kind) {
    case EDIT_TEXT: {
        const gchar *x = *(const gchar * const *)a, *y = *(const gchar * const *)b;
        return g_strcmp0(x != NULL ? x : "", y != NULL ? y : "") != 0;
    }
    case EDIT_INT:   return *(const gint *)a != *(const gint *)b;
    case EDIT_BOOL:  return (*(const gboolean *)a != 0) != (*(const gboolean *)b != 0);
    case EDIT_INT64: return *(const gint64 *)a != *(const gint64 *)b;
    }
    return FALSE;
}

/*
 * edit_bind — bind one editable column's value from a task.
 *
 * Inputs:
 *   st    — the statement
 *   param — the 1-based parameter index
 *   i     — index into edit_columns
 *   t     — the task to read the value from
 */
static void
edit_bind(sqlite3_stmt *st, int param, size_t i, const Task *t)
{
    const char *field = (const char *)t + edit_columns[i].offset;

    switch (edit_columns[i].kind) {
    case EDIT_TEXT: {
        const gchar *text = *(const gchar * const *)field;
        sqlite3_bind_text(st, param, text != NULL ? text : "", -1, SQLITE_TRANSIENT);
        break;
    }
    case EDIT_INT:   sqlite3_bind_int(st, param, *(const gint *)field);          break;
    case EDIT_BOOL:  sqlite3_bind_int(st, param, *(const gboolean *)field != 0); break;
    case EDIT_INT64: sqlite3_bind_int64(st, param, *(const gint64 *)field);      break;
    }
}

/* ---------------------------------------------------------------------------
 * task_db_task_update() — write only the changed editable columns (see
 * db.h).  Parameter ?1 is the id, ?2 the stamp; the changed columns follow
 * from ?3 in table order.  completed_at is stamped when the row ENTERS
 * Done (the CASE reads the OLD row) and is otherwise left as it is —
 * leaving Done does not clear it: it answers "when was this last
 * completed?", which stays a fact after someone reopens the task.
 * ------------------------------------------------------------------------- */
gint
task_db_task_update(TaskDatabase *db, const Task *before, const Task *after)
{
    GString  *sql     = g_string_new("UPDATE tasks SET ");
    gboolean  changed = FALSE;
    gboolean  synced  = FALSE;
    int       param   = 3;

    for (size_t i = 0; i < G_N_ELEMENTS(edit_columns); i++) {
        if (!edit_differs(i, before, after))
            continue;
        if (changed)
            g_string_append(sql, ", ");
        if (strcmp(edit_columns[i].column, "status") == 0)
            g_string_append_printf(sql,
                "completed_at = CASE WHEN ?%d = " DONE_SQL " AND status <> " DONE_SQL
                " THEN ?2 "
                "ELSE completed_at END, ",
                param);
        g_string_append_printf(sql, "%s = ?%d", edit_columns[i].column, param++);
        changed = TRUE;
        synced  = synced || edit_columns[i].synced;
    }
    if (!changed) {
        g_string_free(sql, TRUE);
        return 0;
    }
    if (synced)
        g_string_append(sql, ", updated_at = ?2");
    g_string_append(sql, " WHERE id = ?1");

    gboolean      own = FALSE;
    if (!txn_join(db, &own)) {
        g_string_free(sql, TRUE);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql->str, -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, after->id);
        sqlite3_bind_int64(st, 2, now());
        param = 3;
        for (size_t i = 0; i < G_N_ELEMENTS(edit_columns); i++)
            if (edit_differs(i, before, after))
                edit_bind(st, param++, i, after);
        ok = step_done(db, st, "task update", NULL);
    } else {
        step_done(db, NULL, "task update", NULL);
    }
    sqlite3_finalize(st);
    g_string_free(sql, TRUE);
    if (ok && after->status == TASK_STATUS_DONE && before->status != TASK_STATUS_DONE)
        ok = parent_started(db, after->id);
    return txn_leave(db, own, ok) ? 1 : -1;
}

/* ---------------------------------------------------------------------------
 * task_db_task_set_status() — write the status, stamping completed_at on
 * the way into Done (see db.h), and move a New parent to In Progress when
 * a subtask enters Done (parent_started) — both in ONE transaction, the
 * caller's when one is open (txn_join), so a held lock is waited out once
 * and a failed status write writes nothing at all.  The same rule as task_db_task_update, so
 * both paths agree: entering Done stamps, and every other move leaves the
 * stamp alone — including leaving Done.  It reads the OLD row (gotcha 8).
 *
 * updated_at is stamped for EVERY status change, including New ↔ In
 * Progress.  That is deliberate and costs something: neither Google
 * Tasks nor Notes has a third state, so such a move dirties a row whose
 * remote content is unchanged, and the incremental listing path (where
 * an unchanged remote task is simply absent) answers a dirty row with an
 * etag-guarded PATCH carrying a body identical to what is already there.
 * The alternative was worse — a status move that stamps nothing is
 * invisible to every consumer of updated_at, so the row reads as
 * untouched since the last sync and there is no record that anything
 * happened.  Status is the successor of a SYNCED field, not a local flag
 * like priority: a change to it is a change to the task.
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_set_status(TaskDatabase *db, gint64 id, TaskStatus status)
{
    gboolean own = FALSE;
    if (!txn_join(db, &own))
        return FALSE;
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE tasks SET completed_at = CASE "
            "                    WHEN ?1 = " DONE_SQL " AND status <> " DONE_SQL " THEN ?2 "
            "                    ELSE completed_at END, "
            "updated_at = ?2, status = ?1 WHERE id = ?3", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, (gint)status);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_int64(st, 3, id);
        ok = step_done(db, st, "task set status", NULL);
    } else {
        step_done(db, NULL, "task set status", NULL);
    }
    sqlite3_finalize(st);
    if (ok && status == TASK_STATUS_DONE)
        ok = parent_started(db, id);
    return txn_leave(db, own, ok);
}

/* task_db_task_set_priority() — toggle the local-only high-priority flag
 * (see db.h).  Deliberately NO updated_at bump: the flag is local-only and
 * must not dirty the row for sync (a bump makes newest-wins push a no-op
 * PATCH and can starve a concurrent remote edit behind a 412).             */
gboolean
task_db_task_set_priority(TaskDatabase *db, gint64 id, gboolean priority)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET priority = %d WHERE id = %lld",
        priority ? 1 : 0, (long long)id);
    gboolean ok = exec(db, sql);
    g_free(sql);
    return ok;
}

/*
 * task_db_task_set_scheduled() — write the day-planner schedule fields.
 * Local-only: no updated_at bump (see set_priority).
 *
 * Inputs:
 *   db           — the connection
 *   id           — the task id
 *   start_unix   — unix seconds of the session start; 0 = clear
 *   duration_min — session length in minutes (clamped to 1+ if <= 0)
 */
gboolean
task_db_task_set_scheduled(TaskDatabase *db, gint64 id,
                            gint64 start_unix, gint duration_min)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET scheduled_start = %lld, scheduled_duration = %d "
        "WHERE id = %lld",
        (long long)start_unix,
        duration_min > 0 ? duration_min : SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT,
        (long long)id);
    gboolean ok = exec(db, sql);
    g_free(sql);
    return ok;
}

/*
 * task_db_tasks_scheduled_for_day() — tasks scheduled within a local day.
 *
 * Inputs:
 *   db — the connection
 *   lo — unix seconds of the local midnight that starts the day
 *   hi — unix seconds of the next local midnight (task_day_bounds gives
 *        both: a day is 23 or 25 hours across a DST change, never
 *        assumed to be 24)
 *
 * Output:
 *   new GPtrArray of Task* ordered by scheduled_start; never NULL.
 *   Free with task_ptr_array_free_tasks + g_ptr_array_free.
 */
GPtrArray *
task_db_tasks_scheduled_for_day(TaskDatabase *db, gint64 lo, gint64 hi)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks "
        "WHERE deleted = 0 AND scheduled_start >= ? "
        "AND scheduled_start < ? "
        "ORDER BY scheduled_start",
        2, lo, hi);
}

/* ---------------------------------------------------------------------------
 * The recurrence pass's SQL (see db.h and recur.h).
 * ------------------------------------------------------------------------- */

/* task_db_tasks_recurring() — one pass's candidate set.  Tombstones are
 * excluded: a deleted task's schedule is over, and rolling one forward
 * would resurrect it in every view the moment the tombstone is purged.     */
GPtrArray *
task_db_tasks_recurring(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE recur_interval > 0 AND "
        "deleted = 0 ORDER BY id", 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * task_db_task_recur_apply() — an occurrence came due (see db.h).
 *
 * ONE statement, so the halves of a roll-forward cannot come apart: the
 * new due date AND its time of day, the reset of a COMPLETED task back to
 * New, and the stamp of the occurrence AFTER this one.  due_time is what
 * makes "every Monday at 9:00 AM" land a due date that actually says 9:00
 * — the schedule's time of day reaching the task rather than staying
 * locked in the schedule.  The status CASE reads the OLD
 * status (gotcha 8), which is what lets "was it Done?" be asked once here
 * rather than read back in a second statement.
 *
 * completed_at is deliberately NOT in this statement.  Reopening a task
 * for its next repeat does not un-complete the last one, and the stamp is
 * how the editor still says when that was — which is most of the point of
 * keeping it on a recurring task.
 *
 * A task that was NOT Done keeps whichever status it had: New stays New
 * and In Progress stays In Progress.  Resetting work already under way
 * would throw away the only thing that distinguishes them.
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_recur_apply(TaskDatabase *db, gint64 id, gint64 due,
                         gint due_time, gint64 next)
{
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE tasks SET due = ?1, due_time = ?5, "
            "status       = CASE WHEN status = " DONE_SQL " THEN " NEW_SQL " ELSE status END, "
            "recur_next = ?2, updated_at = ?3 WHERE id = ?4", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, due);
        sqlite3_bind_int64(st, 2, next);
        sqlite3_bind_int64(st, 3, now());
        sqlite3_bind_int64(st, 4, id);
        sqlite3_bind_int(st, 5, due_time);
        ok = step_done(db, st, "task recur apply", NULL);
    } else {
        step_done(db, NULL, "task recur apply", NULL);
    }
    sqlite3_finalize(st);
    return ok;
}

/* task_db_task_recur_set_next() — store the next-occurrence stamp alone
 * (see db.h).  Deliberately NO updated_at bump, the same rule
 * set_priority follows: this is local bookkeeping, and seeding
 * or advancing it must not dirty the row for sync.                          */
gboolean
task_db_task_recur_set_next(TaskDatabase *db, gint64 id, gint64 next)
{
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE tasks SET recur_next = ?1 WHERE id = ?2", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, next);
        sqlite3_bind_int64(st, 2, id);
        ok = step_done(db, st, "task recur set next", NULL);
    } else {
        step_done(db, NULL, "task recur set next", NULL);
    }
    sqlite3_finalize(st);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_db_subtask_move() — swap the display position of subtask `id` with
 * its neighbor (see db.h).
 * ------------------------------------------------------------------------- */
gboolean
task_db_subtask_move(TaskDatabase *db, gint64 id, gint direction)
{
    Task *t = task_db_task_get(db, id);
    if (t == NULL || t->parent_id == 0) {
        task_free(t);
        return TRUE;
    }
    gint64 parent_id = t->parent_id;
    task_free(t);

    GPtrArray *subs = task_db_subtasks(db, parent_id);
    gint idx = -1;
    for (guint i = 0; i < subs->len; i++) {
        if (((Task *)g_ptr_array_index(subs, i))->id == id) {
            idx = (gint)i;
            break;
        }
    }
    gint target = idx + direction;
    if (idx < 0 || target < 0 || target >= (gint)subs->len) {
        task_ptr_array_free_tasks(subs);
        return TRUE;
    }

    /* Swap the two entries, then write new positions 0…n in the new order.
     * updated_at is intentionally not stamped — position is local-only.    */
    gpointer tmp = g_ptr_array_index(subs, idx);
    g_ptr_array_index(subs, idx)    = g_ptr_array_index(subs, target);
    g_ptr_array_index(subs, target) = tmp;

    sqlite3_stmt *st    = NULL;
    gboolean      moved = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), "UPDATE tasks SET position = ? WHERE id = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        step_done(db, NULL, "subtask_move prepare", NULL);
    } else if (task_db_transaction_begin(db)) {
        /* One statement, one transaction: a row per sibling, not a
         * journalled write per sibling. */
        gboolean ok = TRUE;
        for (guint i = 0; ok && i < subs->len; i++) {
            sqlite3_reset(st);
            sqlite3_bind_int(st, 1, (int)i);
            sqlite3_bind_int64(st, 2, ((Task *)g_ptr_array_index(subs, i))->id);
            ok = step_done(db, st, "subtask_move", NULL);
        }
        moved = task_db_transaction_end(db, ok);
    }
    sqlite3_finalize(st);
    task_ptr_array_free_tasks(subs);
    return moved;
}

/* ---------------------------------------------------------------------------
 * task_db_task_delete() — tombstone the task and its subtasks, in ONE
 * statement.
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_delete(TaskDatabase *db, gint64 id)
{
    return tombstone_each(db, "SELECT ?2 AS pid", id, "task delete") >= 0;
}

/* ---------------------------------------------------------------------------
 * task_db_tasks_delete_done() — tombstone a tag's completed top-level
 * tasks and their subtasks, in ONE statement (see db.h).
 * ------------------------------------------------------------------------- */
guint
task_db_tasks_delete_done(TaskDatabase *db, gint64 tag_id)
{
    gint top = tombstone_each(db,
        "SELECT id AS pid FROM tasks WHERE parent_id IS NULL "
        "   AND status = " DONE_SQL " AND deleted = 0 "
        "   AND id IN (SELECT task_id FROM task_tags WHERE tag_id = ?2)",
        tag_id, "delete done");
    return top > 0 ? (guint)top : 0;
}

/*
 * task_db_tasks_tagged — a tag's view (see db.h).
 *
 * Inputs:
 *   db     — the connection
 *   tag_id — the tag
 *
 * Output:
 *   GPtrArray of Task* (possibly empty, never NULL); free with
 *   task_ptr_array_free_tasks.
 */
GPtrArray *
task_db_tasks_tagged(TaskDatabase *db, gint64 tag_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE deleted = 0 AND "
        "id IN (SELECT task_id FROM task_tags WHERE tag_id = ?)" VIEW_ORDER,
        1, tag_id, 0);
}

/* ---------------------------------------------------------------------------
 * Attachments.
 * ------------------------------------------------------------------------- */
GPtrArray *
task_db_attachments(TaskDatabase *db, gint64 task_id)
{
    GPtrArray *out = g_ptr_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT id, task_id, path FROM attachments WHERE task_id = ? "
            "ORDER BY added_at, id", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            TaskAttachment *a = g_new0(TaskAttachment, 1);
            a->id      = sqlite3_column_int64(st, 0);
            a->task_id = sqlite3_column_int64(st, 1);
            a->path    = column_text_dup(st, 2);
            g_ptr_array_add(out, a);
        }
    }
    sqlite3_finalize(st);
    return out;
}

/* task_db_attachment_add() — new attachment row; the new id, 0 on
 * failure (see db.h).                                                      */
gint64
task_db_attachment_add(TaskDatabase *db, gint64 task_id, const gchar *path)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "INSERT INTO attachments(task_id, path, added_at) "
            "VALUES(?, ?, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "attachment add", NULL))
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        step_done(db, NULL, "attachment add", NULL);
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_attachment_remove() — drop one attachment row (see db.h).        */
gboolean
task_db_attachment_remove(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM attachments WHERE id = %lld", (long long)id);
    gboolean ok = exec(db, sql);
    sqlite3_free(sql);
    return ok;
}

/* task_db_attachment_counts() — task_id → count map, one query (see db.h). */
GHashTable *
task_db_attachment_counts(TaskDatabase *db)
{
    GHashTable *map = g_hash_table_new(g_direct_hash, g_direct_equal);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT task_id, COUNT(*) FROM attachments GROUP BY task_id",
            -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            g_hash_table_insert(map,
                GINT_TO_POINTER(sqlite3_column_int64(st, 0)),
                GINT_TO_POINTER(sqlite3_column_int(st, 1)));
    sqlite3_finalize(st);
    return map;
}

/* ---------------------------------------------------------------------------
 * Tags.
 * ------------------------------------------------------------------------- */

/*
 * task_tag_free — free a tag and its strings.
 *
 * Inputs:
 *   tag — the tag, or NULL (ignored)
 */
void
task_tag_free(TaskTag *tag)
{
    if (tag == NULL) return;
    g_free(tag->name);
    g_free(tag->emoji);
    g_free(tag);
}

/*
 * task_ptr_array_free_tags — free an array of tags and the tags in it.
 *
 * Inputs:
 *   a — the array, or NULL (ignored)
 */
void
task_ptr_array_free_tags(GPtrArray *a)
{
    if (a == NULL) return;
    for (guint i = 0; i < a->len; i++)
        task_tag_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/*
 * task_tag_label — a tag's display label (see db.h).
 *
 * Inputs:
 *   tag — the tag
 *
 * Output:
 *   new string (g_free).
 */
gchar *
task_tag_label(const TaskTag *tag)
{
    return tag->emoji != NULL && tag->emoji[0] != '\0'
        ? g_strdup_printf("%s  %s", tag->emoji, tag->name)
        : g_strdup(tag->name);
}

/*
 * read_tag — a TaskTag from a row whose first four columns are TAG_COLS.
 *
 * Inputs:
 *   st — a stepped statement on a row
 *
 * Output:
 *   new tag (task_tag_free).
 */
static TaskTag *
read_tag(sqlite3_stmt *st)
{
    TaskTag *tag = g_new0(TaskTag, 1);
    tag->id       = sqlite3_column_int64(st, 0);
    tag->name     = column_text_dup(st, 1);
    tag->emoji    = column_text_dup(st, 2);
    tag->favorite = sqlite3_column_int(st, 3) != 0;
    if (tag->name == NULL) tag->name = g_strdup("");
    if (tag->emoji == NULL) tag->emoji = g_strdup("");
    return tag;
}

/*
 * tag_query — the tags a query yields, as TAG_COLS rows.
 *
 * Inputs:
 *   db    — the connection
 *   sql   — the query, with at most one ? (bound to `param`)
 *   param — the value bound to the ?, when there is one
 *
 * Output:
 *   GPtrArray of TaskTag* (task_ptr_array_free_tags); never NULL.
 */
static GPtrArray *
tag_query(TaskDatabase *db, const gchar *sql, gint64 param)
{
    GPtrArray    *out = g_ptr_array_new();
    sqlite3_stmt *st  = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_bind_parameter_count(st) > 0)
            sqlite3_bind_int64(st, 1, param);
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_tag(st));
    } else {
        step_done(db, NULL, "tag query", NULL);
    }
    sqlite3_finalize(st);
    return out;
}

/* The columns tag_query reads, over the shared tags table aliased n.     */
#define TAG_COLS "n.id, n.name, n.emoji, n.favorite_tasks"

/*
 * task_db_tags — the tags of a scope, alphabetical (see db.h).  A task's
 * task_tags rows go when it is tombstoned, so a row names a live task.
 *
 * Inputs:
 *   db    — the connection
 *   scope — TASK_TAGS_CARRIED or TASK_TAGS_ALL
 *
 * Output:
 *   GPtrArray of TaskTag* (task_ptr_array_free_tags); never NULL.
 */
GPtrArray *
task_db_tags(TaskDatabase *db, TaskTagScope scope)
{
    return tag_query(db, scope == TASK_TAGS_ALL
                         ? "SELECT " TAG_COLS " FROM tags n "
                           "ORDER BY n.name COLLATE NOCASE, n.id"
                         : "SELECT " TAG_COLS " FROM tags n WHERE EXISTS "
                           "(SELECT 1 FROM task_tags tt WHERE tt.tag_id = n.id) "
                           "ORDER BY n.name COLLATE NOCASE, n.id", 0);
}

/*
 * first_tag — the first tag a query yields, or NULL.
 *
 * Inputs:
 *   a — tag_query's result, freed here
 *
 * Output:
 *   the tag (task_tag_free), or NULL when the query yielded none.
 */
static TaskTag *
first_tag(GPtrArray *a)
{
    TaskTag *tag = a->len > 0 ? g_ptr_array_steal_index(a, 0) : NULL;
    task_ptr_array_free_tags(a);
    return tag;
}

/* task_db_tag_get() — one tag, or NULL (see db.h).                         */
TaskTag *
task_db_tag_get(TaskDatabase *db, gint64 id)
{
    return first_tag(tag_query(db, "SELECT " TAG_COLS " FROM tags n "
                                   "WHERE n.id = ?", id));
}

/*
 * task_db_tag_find — the tag of a name (see db.h).
 *
 * Inputs:
 *   db   — the connection
 *   name — the name; trimmed here
 *
 * Output:
 *   the tag (task_tag_free), or NULL when no tag has that name.
 */
TaskTag *
task_db_tag_find(TaskDatabase *db, const gchar *name)
{
    gchar        *clean = g_strstrip(g_strdup(name != NULL ? name : ""));
    sqlite3_stmt *st    = NULL;
    gint64        id    = 0;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT id FROM tags WHERE name = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, clean, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            id = sqlite3_column_int64(st, 0);
    } else {
        step_done(db, NULL, "tag find", NULL);
    }
    sqlite3_finalize(st);
    g_free(clean);
    return id != 0 ? task_db_tag_get(db, id) : NULL;
}

/*
 * task_db_tag_set_emoji — a tag's display prefix (see db.h).
 *
 * Inputs:
 *   db    — the connection
 *   id    — the tag
 *   emoji — the emoji, trimmed; NULL or "" for none
 *
 * Output:
 *   TRUE when written.
 */
gboolean
task_db_tag_set_emoji(TaskDatabase *db, gint64 id, const gchar *emoji)
{
    gchar *clean = g_strstrip(g_strdup(emoji != NULL ? emoji : ""));
    gchar *sql   = sqlite3_mprintf("UPDATE tags SET emoji = %Q WHERE id = %lld",
                                   clean, (long long)id);
    gboolean ok = exec(db, sql);
    sqlite3_free(sql);
    g_free(clean);
    return ok;
}

/*
 * task_db_tag_set_favorite — put a tag in the favorites or take it out
 * (see db.h).
 *
 * Inputs:
 *   db       — the connection
 *   id       — the tag
 *   favorite — the new flag
 *
 * Output:
 *   TRUE when written.
 */
gboolean
task_db_tag_set_favorite(TaskDatabase *db, gint64 id, gboolean favorite)
{
    gchar *sql = sqlite3_mprintf("UPDATE tags SET favorite_tasks = %d WHERE id = %lld",
                                 favorite ? 1 : 0, (long long)id);
    gboolean ok = exec(db, sql);
    sqlite3_free(sql);
    return ok;
}

/*
 * task_db_tag_open_counts — each tag's open tasks, one query (see db.h).
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   tag id → count; never NULL.
 */
GHashTable *
task_db_tag_open_counts(TaskDatabase *db)
{
    GHashTable   *map = g_hash_table_new(g_direct_hash, g_direct_equal);
    sqlite3_stmt *st  = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT tt.tag_id, COUNT(*) FROM task_tags tt JOIN tasks t ON t.id = tt.task_id "
            "WHERE t.deleted = 0 AND t.status <> " DONE_SQL " GROUP BY tt.tag_id",
            -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            g_hash_table_insert(map, GINT_TO_POINTER(sqlite3_column_int64(st, 0)),
                                GINT_TO_POINTER(sqlite3_column_int(st, 1)));
    } else {
        step_done(db, NULL, "tag counts", NULL);
    }
    sqlite3_finalize(st);
    return map;
}

/*
 * task_db_tags_by_task — every live task's tags, one query (see db.h).
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   task id → GPtrArray of TaskTag* (each array frees its tags); never
 *   NULL.
 */
GHashTable *
task_db_tags_by_task(TaskDatabase *db)
{
    GHashTable   *map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                              (GDestroyNotify)task_ptr_array_free_tags);
    sqlite3_stmt *st  = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT " TAG_COLS ", tt.task_id FROM task_tags tt "
            "JOIN tags n ON n.id = tt.tag_id "
            "JOIN tasks t ON t.id = tt.task_id WHERE t.deleted = 0 "
            "ORDER BY tt.task_id, n.name COLLATE NOCASE, n.id", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            gpointer   key  = GINT_TO_POINTER(sqlite3_column_int64(st, 4));
            GPtrArray *tags = g_hash_table_lookup(map, key);
            if (tags == NULL) {
                tags = g_ptr_array_new();
                g_hash_table_insert(map, key, tags);
            }
            g_ptr_array_add(tags, read_tag(st));
        }
    } else {
        step_done(db, NULL, "tags by task", NULL);
    }
    sqlite3_finalize(st);
    return map;
}

/* task_db_task_tags() — one task's tags, alphabetical (see db.h).          */
GPtrArray *
task_db_task_tags(TaskDatabase *db, gint64 task_id)
{
    return tag_query(db, "SELECT " TAG_COLS " FROM task_tags tt "
                         "JOIN tags n ON n.id = tt.tag_id "
                         "WHERE tt.task_id = ? ORDER BY n.name COLLATE NOCASE, n.id",
                     task_id);
}

/*
 * task_db_task_tag_add — give a live task a tag by name (see db.h).  Three
 * statements in one transaction: the name (unless a tag of that name
 * exists, made by either app), the link, and the tag's id.  Both writes
 * are conditional on the task being live, so a refused add writes nothing:
 * a name row without a link would be a tag nothing carries.
 *
 * Inputs:
 *   db      — the connection
 *   task_id — the task
 *   name    — the tag's name, trimmed here
 *
 * Output:
 *   the tag id, or 0 when refused or a write failed.
 */
gint64
task_db_task_tag_add(TaskDatabase *db, gint64 task_id, const gchar *name)
{
    static const gchar *const steps[] = {
        "INSERT INTO tags (name) SELECT ?1 "
        " WHERE EXISTS (SELECT 1 FROM tasks WHERE id = ?2 AND deleted = 0) "
        "ON CONFLICT (name) DO NOTHING",
        "INSERT OR IGNORE INTO task_tags (task_id, tag_id) "
        "SELECT ?2, id FROM tags WHERE name = ?1 "
        " AND EXISTS (SELECT 1 FROM tasks WHERE id = ?2 AND deleted = 0)",
        "SELECT tt.tag_id FROM task_tags tt "
        "JOIN tags n ON n.id = tt.tag_id "
        "WHERE tt.task_id = ?2 AND n.name = ?1",
    };
    gchar *clean = g_strstrip(g_strdup(name != NULL ? name : ""));
    gint64 tag_id = 0;
    gboolean own;

    if (clean[0] == '\0' || !txn_join(db, &own)) {
        g_free(clean);
        return 0;
    }
    gboolean ok = TRUE;
    for (gsize i = 0; ok && i < G_N_ELEMENTS(steps); i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(shared_db_handle(db->shared), steps[i], -1, &st, NULL)
            == SQLITE_OK) {
            sqlite3_bind_text(st, 1, clean, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, task_id);
        }
        if (i + 1 < G_N_ELEMENTS(steps)) {
            ok = step_done(db, st, "tag add", NULL);
        } else if (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
            tag_id = sqlite3_column_int64(st, 0);
        } else {
            ok = FALSE;              /* refused: the task is not live      */
        }
        sqlite3_finalize(st);
    }
    g_free(clean);
    return txn_leave(db, own, ok) ? tag_id : 0;
}

/*
 * task_db_task_tag_remove — take a tag off a task (see db.h).  The schema
 * refuses a live task's last tag (task_tags_keep_one): that refusal is an
 * answer, not a failure, so it is told apart by its error and not logged.
 *
 * Inputs:
 *   db      — the connection
 *   task_id — the task
 *   tag_id  — the tag
 *
 * Output:
 *   1 written, 0 refused (the last tag), -1 failed (logged).
 */
gint
task_db_task_tag_remove(TaskDatabase *db, gint64 task_id, gint64 tag_id)
{
    sqlite3      *sq = shared_db_handle(db->shared);
    sqlite3_stmt *st = NULL;
    gint          rc = -1;
    if (sqlite3_prepare_v2(sq, "DELETE FROM task_tags WHERE task_id = ? AND tag_id = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        sqlite3_bind_int64(st, 2, tag_id);
        int step = sqlite3_step(st);
        if (step == SQLITE_DONE) {
            rc = 1;
            if (sqlite3_get_autocommit(sq))
                own_commit(db);
        } else if (sqlite3_extended_errcode(sq) == SQLITE_CONSTRAINT_TRIGGER &&
                   g_strcmp0(sqlite3_errmsg(sq), SHARED_DB_TAG_KEEP_ONE_MSG) == 0) {
            rc = 0;
        } else {
            g_warning("db: tag remove: %s", sqlite3_errmsg(sq));
        }
    } else {
        step_done(db, NULL, "tag remove", NULL);
    }
    sqlite3_finalize(st);
    return rc;
}

/* task_db_totals() — non-tombstoned task and tag counts (see db.h).        */
void
task_db_totals(TaskDatabase *db, gint *n_tasks, gint *n_tags)
{
    const struct {
        const gchar *sql;
        gint        *out;            /* caller's slot (may be NULL)         */
    } q[] = {
        { "SELECT COUNT(*) FROM tasks WHERE deleted = 0", n_tasks },
        { "SELECT COUNT(*) FROM tags n WHERE EXISTS "
          "(SELECT 1 FROM task_tags tt WHERE tt.tag_id = n.id)", n_tags },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(q); i++) {
        if (q[i].out == NULL)
            continue;
        *q[i].out = 0;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(shared_db_handle(db->shared), q[i].sql, -1, &st, NULL)
                == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW)
            *q[i].out = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
}

/*
 * purge_unless_linked — physically delete rows, in one transaction, unless
 * a task among them is a Notes action item (a note_task row names it):
 * Tasks never purges one of those, since the note's line would lose its
 * task.  The check and the deletes share the transaction, so a link made
 * between them cannot be lost.
 *
 * Inputs:
 *   db      — the connection
 *   members — a SELECT of the task ids the purge would remove (one ?1)
 *   deletes — the DELETE statements (each with ?1 where the id goes)
 *   id      — the task id bound to ?1
 *
 * Output:
 *   TRUE when the rows were deleted; FALSE when a linked task refused the
 *   purge or a statement failed (nothing is deleted either way).
 */
static gboolean
purge_unless_linked(TaskDatabase *db, const gchar *members, const gchar *const *deletes,
                    gint64 id)
{
    if (!task_db_transaction_begin(db))
        return FALSE;

    sqlite3      *sq     = shared_db_handle(db->shared);
    gchar        *ask    = g_strdup_printf("SELECT EXISTS(SELECT 1 FROM note_task "
                                           "WHERE task_id IN (%s))", members);
    sqlite3_stmt *st     = NULL;
    gboolean      linked = TRUE;     /* a failed check refuses the purge   */
    if (sqlite3_prepare_v2(sq, ask, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW)
            linked = sqlite3_column_int(st, 0) != 0;
    } else {
        step_done(db, NULL, "purge check", NULL);
    }
    sqlite3_finalize(st);
    g_free(ask);

    gboolean ok = !linked;
    for (const gchar *const *sql = deletes; ok && *sql != NULL; sql++) {
        st = NULL;
        if (sqlite3_prepare_v2(sq, *sql, -1, &st, NULL) == SQLITE_OK)
            sqlite3_bind_int64(st, 1, id);
        ok = step_done(db, st, "purge", NULL);
        sqlite3_finalize(st);
    }
    return task_db_transaction_end(db, ok);
}

/* task_db_task_purge() — physically delete a task row + its subtasks,
 * unless one of them is a Notes action item (see db.h).                    */
gboolean
task_db_task_purge(TaskDatabase *db, gint64 id)
{
    static const gchar *const deletes[] = {
        "DELETE FROM tasks WHERE parent_id = ?1",
        "DELETE FROM tasks WHERE id = ?1",
        NULL
    };
    return purge_unless_linked(db, "SELECT id FROM tasks WHERE id = ?1 OR parent_id = ?1",
                               deletes, id);
}

/* task_db_task_note() — the title of the note a task was born in (see
 * db.h).                                                                    */
gchar *
task_db_task_note(TaskDatabase *db, gint64 task_id)
{
    sqlite3_stmt *st    = NULL;
    gchar        *title = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT COALESCE(n.title, '') FROM note_task nt "
            "JOIN notes n ON n.id = nt.note_id WHERE nt.task_id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        if (sqlite3_step(st) == SQLITE_ROW)
            title = column_text_dup(st, 0);
    } else {
        step_done(db, NULL, "task note", NULL);
    }
    sqlite3_finalize(st);
    return title;
}
