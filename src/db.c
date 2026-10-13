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
 * own_commit — this connection has just committed: bring the Tasks change
 * revision it remembers up to date, so its own writes are never taken for
 * another app's (task_db_outside_change).  When another connection has
 * ALSO committed since the last look (shared_db_changed), the revision is
 * left alone and the change is kept pending instead: re-reading it now
 * would hide that writer's change to Tasks' data.
 *
 * Inputs:
 *   db — the connection, outside any transaction
 */
static void
own_commit(TaskDatabase *db)
{
    int64_t rev = 0;
    if (shared_db_changed(db->shared) != 0 ||
        shared_db_rev(db->shared, SHARED_DB_REV_TASKS, &rev, NULL) != 0) {
        db->outside_pending = TRUE;
        return;
    }
    db->tasks_rev = rev;
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

/* ---------------------------------------------------------------------------
 * exec_txn() — run `sql` (one or more statements) inside a transaction,
 * ROLLING BACK on failure.  A bare "BEGIN;…;COMMIT;" through exec()
 * would leave the connection stuck inside an open transaction when a
 * middle statement fails (e.g. SQLITE_BUSY against the sync worker) —
 * every later BEGIN would then fail and writes would silently vanish
 * until the connection closes.  Returns TRUE when it committed.
 * ------------------------------------------------------------------------- */
static gboolean
exec_txn(TaskDatabase *db, const gchar *sql)
{
    if (!task_db_transaction_begin(db))
        return FALSE;
    return task_db_transaction_end(db, exec(db, sql));
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
 * Returns TRUE when the statement completed (and, outside a transaction,
 * committed: own_commit then notes it).
 * ------------------------------------------------------------------------- */
static gboolean
step_done(TaskDatabase *db, sqlite3_stmt *st, const gchar *ctx)
{
    if (st == NULL) {
        g_warning("db: %s: prepare failed: %s", ctx,
                  sqlite3_errmsg(shared_db_handle(db->shared)));
        return FALSE;
    }
    if (sqlite3_step(st) != SQLITE_DONE) {
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
void
task_list_free(TaskList *l)
{
    if (l == NULL)
        return;
    g_free(l->name);
    g_free(l->emoji);
    g_free(l);
}

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

/* task_ptr_array_free_lists() — free an array of TaskList*.  NULL-safe.    */
void
task_ptr_array_free_lists(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        task_list_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* task_group_free() / task_ptr_array_free_groups() — free a TaskGroup.     */
void
task_group_free(TaskGroup *g)
{
    if (g == NULL) return;
    g_free(g->name);
    g_free(g);
}

void
task_ptr_array_free_groups(GPtrArray *a)
{
    if (a == NULL) return;
    for (guint i = 0; i < a->len; i++)
        task_group_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
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
    /* The data as opened is the data the app loads first.  A failed read
     * leaves -1, which no stored revision equals: the first outside
     * change then refreshes.                                              */
    if (shared_db_rev(shared, SHARED_DB_REV_TASKS, &db->tasks_rev, NULL) != 0)
        db->tasks_rev = -1;
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
gboolean
task_db_outside_change(TaskDatabase *db)
{
    int changed = shared_db_changed(db->shared);
    if (changed == 0 && !db->outside_pending)
        return FALSE;
    db->outside_pending = FALSE;

    int64_t rev = 0;
    if (shared_db_rev(db->shared, SHARED_DB_REV_TASKS, &rev, NULL) != 0)
        return TRUE;                 /* cannot tell: refresh to be safe     */
    if (rev == db->tasks_rev)
        return FALSE;                /* only Notes' data (or settings) moved */
    db->tasks_rev = rev;
    return TRUE;
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

/* ---------------------------------------------------------------------------
 * read_list() — build a TaskList from the standard lists SELECT
 * (LIST_COLS: id, name, position, updated_at, deleted, emoji, group_id).
 * ------------------------------------------------------------------------- */
static TaskList *
read_list(sqlite3_stmt *st)
{
    TaskList *l = g_new0(TaskList, 1);
    l->id         = sqlite3_column_int64(st, 0);
    l->name       = column_text_dup(st, 1);
    l->position   = sqlite3_column_int(st, 2);
    l->updated_at = sqlite3_column_int64(st, 3);
    l->deleted    = sqlite3_column_int(st, 4) != 0;
    l->emoji      = column_text_dup(st, 5);
    l->group_id   = sqlite3_column_int64(st, 6);
    if (l->name == NULL)
        l->name = g_strdup("");
    if (l->emoji == NULL)
        l->emoji = g_strdup("");
    return l;
}

/* The shared column list for read_list().                                  */
#define LIST_COLS "id, name, position, updated_at, deleted, " \
                  "emoji, COALESCE(group_id, 0)"

/* ---------------------------------------------------------------------------
 * task_db_lists() — all (visible) lists (see db.h).  Alphabetical until
 * the user drag-reorders the sidebar (which sets the sync_state flag
 * "lists_custom_order"); the dragged positions rule after that.
 * ------------------------------------------------------------------------- */
GPtrArray *
task_db_lists(TaskDatabase *db, gboolean include_deleted)
{
    GPtrArray *out = g_ptr_array_new();
    /* Check once whether the user has ever drag-reordered the sidebar;
     * if so, use stored positions (name-tiebroken), otherwise alphabetical. */
    gboolean custom = FALSE;
    {
        sqlite3_stmt *cs = NULL;
        if (sqlite3_prepare_v2(shared_db_handle(db->shared),
                "SELECT 1 FROM sync_state WHERE key='lists_custom_order'",
                -1, &cs, NULL) == SQLITE_OK)
            custom = (sqlite3_step(cs) == SQLITE_ROW);
        sqlite3_finalize(cs);
    }
    gchar *sql = g_strdup_printf(
        "SELECT " LIST_COLS " FROM lists%s ORDER BY %s",
        include_deleted ? "" : " WHERE deleted = 0",
        custom ? "position, lower(name)" : "lower(name)");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_list(st));
    else
        step_done(db, NULL, "lists query");
    sqlite3_finalize(st);
    g_free(sql);
    return out;
}

/* ---------------------------------------------------------------------------
 * task_db_lists_reorder() — persist a drag-reorder (see db.h).
 * ------------------------------------------------------------------------- */
gboolean
task_db_lists_reorder(TaskDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction: position = array index for every id, plus the
     * flag flipping task_db_lists into custom-order mode.  Positions are
     * LOCAL-ONLY (Google tasklists carry no order), so updated_at is
     * NOT stamped — a reorder must not dirty the rows for sync.            */
    GString *sql = g_string_new(NULL);
    for (gsize i = 0; i < n; i++)
        g_string_append_printf(sql,
            "UPDATE lists SET position = %d WHERE id = %lld;",
            (gint)i, (long long)ids[i]);
    g_string_append(sql,
        "INSERT OR REPLACE INTO sync_state(key, value) "
        "VALUES('lists_custom_order', '1');");
    gboolean ok = exec_txn(db, sql->str);
    g_string_free(sql, TRUE);
    return ok;
}

/* task_db_list_get() — one list row, tombstoned or not; NULL if absent.    */
TaskList *
task_db_list_get(TaskDatabase *db, gint64 id)
{
    sqlite3_stmt *st = NULL;
    TaskList *l = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT " LIST_COLS " FROM lists WHERE id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW)
            l = read_list(st);
    }
    sqlite3_finalize(st);
    return l;
}

/* ---------------------------------------------------------------------------
 * task_db_list_create() — append a new list (see db.h).
 * ------------------------------------------------------------------------- */
gint64
task_db_list_create(TaskDatabase *db, const gchar *name, const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "INSERT INTO lists(name, emoji, position, updated_at) "
            "VALUES(?, ?, "
            "(SELECT COALESCE(MAX(position), 0) + 1 FROM lists), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "list create"))
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        step_done(db, NULL, "list create");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_list_update() — rename/re-emoji + stamp.                         */
gboolean
task_db_list_update(TaskDatabase *db, gint64 id, const gchar *name,
                    const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE lists SET name = ?, emoji = ?, updated_at = ? "
            "WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        sqlite3_bind_int64(st, 4, id);
        ok = step_done(db, st, "list update");
    } else {
        step_done(db, NULL, "list update");
    }
    sqlite3_finalize(st);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_db_list_delete() — tombstone the list and every task in it.
 * ------------------------------------------------------------------------- */
gboolean
task_db_list_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE list_id = %lld;"
        "UPDATE lists SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    gboolean ok = exec_txn(db, sql);
    sqlite3_free(sql);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Group CRUD.
 * ------------------------------------------------------------------------- */

/* The group column list, so the two queries below cannot disagree about
 * what read_group is reading.                                              */
#define GROUP_COLS "id, name, position"

/*
 * read_group — build a TaskGroup from the current row of a statement
 * selecting GROUP_COLS.
 *
 * Inputs:
 *   st — a stepped statement positioned on a row
 *
 * Output:
 *   a new TaskGroup (never NULL); free with task_group_free.
 */
static TaskGroup *
read_group(sqlite3_stmt *st)
{
    TaskGroup *g = g_new0(TaskGroup, 1);
    g->id       = sqlite3_column_int64(st, 0);
    g->name     = column_text_dup(st, 1);
    if (g->name == NULL) g->name = g_strdup("");
    g->position = sqlite3_column_int(st, 2);
    return g;
}

/* task_db_groups() — all groups, ordered by position then name.            */
GPtrArray *
task_db_groups(TaskDatabase *db)
{
    GPtrArray *out = g_ptr_array_new();
    const gchar *sql =
        "SELECT " GROUP_COLS " FROM list_groups "
        "ORDER BY position, lower(name)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_group(st));
    sqlite3_finalize(st);
    return out;
}

/* task_db_group_get() — one group row; NULL if absent (see db.h).          */
TaskGroup *
task_db_group_get(TaskDatabase *db, gint64 id)
{
    sqlite3_stmt *st = NULL;
    TaskGroup    *g  = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "SELECT " GROUP_COLS " FROM list_groups WHERE id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW)
            g = read_group(st);
    }
    sqlite3_finalize(st);
    return g;
}

/* task_db_group_create() — insert a new group; returns rowid or 0.         */
gint64
task_db_group_create(TaskDatabase *db, const gchar *name)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "INSERT INTO list_groups(name) VALUES(?)", -1, &st, NULL)
            == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        if (step_done(db, st, "group create"))
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        step_done(db, NULL, "group create");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_group_delete() — remove the group; un-groups all its lists.      */
gboolean
task_db_group_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE lists SET group_id = NULL WHERE group_id = %lld;"
        "DELETE FROM list_groups WHERE id = %lld;",
        (long long)id, (long long)id);
    gboolean ok = exec_txn(db, sql);
    sqlite3_free(sql);
    return ok;
}

/* task_db_group_rename() — update the group's name.                        */
gboolean
task_db_group_rename(TaskDatabase *db, gint64 id, const gchar *name)
{
    sqlite3_stmt *st = NULL;
    gboolean      ok = FALSE;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE list_groups SET name = ? WHERE id = ?", -1, &st, NULL)
            == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        ok = step_done(db, st, "group rename");
    } else {
        step_done(db, NULL, "group rename");
    }
    sqlite3_finalize(st);
    return ok;
}

/* task_db_list_set_group() — assign a list to a group (0 = ungrouped).     */
gboolean
task_db_list_set_group(TaskDatabase *db, gint64 list_id, gint64 group_id)
{
    gchar *sql = group_id == 0
        ? sqlite3_mprintf(
              "UPDATE lists SET group_id = NULL WHERE id = %lld;",
              (long long)list_id)
        : sqlite3_mprintf(
              "UPDATE lists SET group_id = %lld WHERE id = %lld;",
              (long long)group_id, (long long)list_id);
    gboolean ok = exec_txn(db, sql);
    sqlite3_free(sql);
    return ok;
}

/* ---------------------------------------------------------------------------
 * read_task() — build a Task from the standard tasks SELECT.
 * ------------------------------------------------------------------------- */
#define TASK_COLS_WITH(notes) \
                  "id, list_id, COALESCE(parent_id, 0), title, " notes ", due, " \
                  "status, pinned, position, updated_at, deleted, "\
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
    t->list_id      = sqlite3_column_int64(st, 1);
    t->parent_id    = sqlite3_column_int64(st, 2);
    t->title        = column_text_dup(st, 3);
    t->notes        = column_text_dup(st, 4);
    t->due          = sqlite3_column_int64(st, 5);
    t->status       = (TaskStatus)sqlite3_column_int(st, 6);
    t->pinned       = sqlite3_column_int(st, 7) != 0;
    t->position     = sqlite3_column_int(st, 8);
    t->updated_at   = sqlite3_column_int64(st, 9);
    t->deleted      = sqlite3_column_int(st, 10) != 0;
    t->completed_at = sqlite3_column_int64(st, 11);
    t->priority     = sqlite3_column_int(st, 12) != 0;
    t->recur_interval = sqlite3_column_int(st, 13);
    /* Clamped on the way IN rather than at every reader: recur_unit is an
     * enum index into the unit tables in recur.c and the editor's combo,
     * and a hand-edited or future-version value must not index off the end
     * of either.  Minutes is the safe floor for the same reason New is
     * TaskStatus's (see editor_status_get).                                */
    {
        gint u = sqlite3_column_int(st, 14);
        t->recur_unit = (u >= 0 && u < TASK_RECUR_N_UNITS)
                        ? (TaskRecurUnit)u : TASK_RECUR_MINUTE;
    }
    t->recur_time   = sqlite3_column_int(st, 15);
    t->recur_lead   = sqlite3_column_int(st, 16);
    t->recur_next   = sqlite3_column_int64(st, 17);
    t->recur_start  = sqlite3_column_int64(st, 18);
    /* Clamped on the way IN, like recur_unit above: a hand-edited or
     * out-of-range value would otherwise format as a nonsense clock time
     * on every row that shows one.                                       */
    {
        gint m = sqlite3_column_int(st, 19);
        t->due_time = (m >= 0 && m <= 23 * 60 + 59) ? m
                                                    : SHARED_DB_TASK_DUE_TIME_DEFAULT;
    }
    t->scheduled_start    = sqlite3_column_int64(st, 20);
    t->scheduled_duration = sqlite3_column_int(st, 21);
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
        step_done(db, NULL, "task query");
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
 * to the top of every list it appears in (within its parent group for
 * subtask queries).                                                        */

/* task_db_tasks_toplevel() — visible top-level tasks of a list.            */
GPtrArray *
task_db_tasks_toplevel(TaskDatabase *db, gint64 list_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE list_id = ? AND "
        "parent_id IS NULL AND deleted = 0 "
        "ORDER BY priority DESC, position, id",
        1, list_id, 0);
}

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

/* task_db_tasks_pinned() — the Pinned Tasks meta list.                     */
GPtrArray *
task_db_tasks_pinned(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE pinned = 1 AND deleted = 0 "
        "ORDER BY priority DESC, list_id, position, id", 0, 0, 0);
}

/* task_db_has_pinned() — any pinned task exists (see db.h).                */
gboolean
task_db_has_pinned(TaskDatabase *db)
{
    const gchar *sql = "SELECT EXISTS(SELECT 1 FROM tasks "
                       "              WHERE pinned = 1 AND deleted = 0)";
    gboolean has = FALSE;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared), sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        has = sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return has;
}

/* task_db_tasks_all_visible() — the All Tasks meta list (top-level tasks
 * of every list; their subtasks render inside the rows as usual).          */
GPtrArray *
task_db_tasks_all_visible(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NULL AND "
        "deleted = 0 ORDER BY priority DESC, list_id, position, id",
        0, 0, 0);
}

/* task_db_tasks_in_group() — All Tasks narrowed to one group's lists.
 * The subquery names the LIVE lists only: a tombstoned list keeps its
 * group_id, and its tasks are tombstoned with it, so leaving it in would
 * be a second spelling of the same exclusion.                              */
GPtrArray *
task_db_tasks_in_group(TaskDatabase *db, gint64 group_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NULL AND "
        "deleted = 0 AND list_id IN (SELECT id FROM lists "
        "                            WHERE group_id = ? AND deleted = 0) "
        "ORDER BY priority DESC, list_id, position, id",
        1, group_id, 0);
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
          "list_id, position"
        : "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
          "deleted = 0 ORDER BY priority DESC, due, due_time, list_id, "
          "position",
        2, lo, hi);
}

/* task_db_tasks_in_list_all() — one list's rows incl. tombstones (sync),
 * parents before subtasks.                                                 */
GPtrArray *
task_db_tasks_in_list_all(TaskDatabase *db, gint64 list_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE list_id = ? ORDER BY "
        "parent_id IS NOT NULL, position, id", 1, list_id, 0);
}

/* ---------------------------------------------------------------------------
 * task_db_task_create() — append a task (see db.h).  One nesting level: a
 * non-zero parent must itself be a top-level, undeleted task in the same
 * list, or the insert is refused.
 * ------------------------------------------------------------------------- */
gint64
task_db_task_create(TaskDatabase *db, gint64 list_id, gint64 parent_id,
                    const gchar *title)
{
    if (parent_id != 0) {
        Task *p = task_db_task_get(db, parent_id);
        gboolean ok = p != NULL && p->parent_id == 0 && !p->deleted &&
                      p->list_id == list_id;
        task_free(p);
        if (!ok)
            return 0;
    }
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "INSERT INTO tasks(list_id, parent_id, title, position, "
            "updated_at) VALUES(?, ?, ?, (SELECT "
            "COALESCE(MAX(position), 0) + 1 FROM tasks WHERE list_id = ?1 "
            "AND COALESCE(parent_id, 0) = COALESCE(?2, 0)), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        if (parent_id != 0)
            sqlite3_bind_int64(st, 2, parent_id);
        else
            sqlite3_bind_null(st, 2);
        sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
        /* due_time is left to the column DEFAULT, as Notes leaves it: the
         * shared module alone creates the schema, from
         * SHARED_DB_TASK_DUE_TIME_DEFAULT, so the DEFAULT is that value.  */
        sqlite3_bind_int64(st, 4, now());
        if (step_done(db, st, "task create"))
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        step_done(db, NULL, "task create");
    }
    sqlite3_finalize(st);
    return id;
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
 *   none.  Silent no-op unless there is a parent AND that parent is New:
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
 * ONE statement, deliberately not wrapped in a transaction with the
 * child's own write: the two rows are written separately, and a crash in
 * between leaves the parent New — exactly what the old behavior was, and
 * the next tick puts it right.  A transaction here would buy nothing that
 * matters and would have to be threaded through four callers.
 */
static void
parent_started(TaskDatabase *db, gint64 child_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(shared_db_handle(db->shared),
            "UPDATE tasks SET status = ?1, updated_at = ?2 "
            "WHERE id = (SELECT parent_id FROM tasks WHERE id = ?3) "
            "AND status = ?4 AND deleted = 0", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, (gint)TASK_STATUS_IN_PROGRESS);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_int64(st, 3, child_id);
        sqlite3_bind_int(st, 4, (gint)TASK_STATUS_NEW);
        step_done(db, st, "parent started");
    } else {
        step_done(db, NULL, "parent started");
    }
    sqlite3_finalize(st);
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
    { "pinned",         EDIT_BOOL,  offsetof(Task, pinned),         FALSE },
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
        ok = step_done(db, st, "task update");
    } else {
        step_done(db, NULL, "task update");
    }
    sqlite3_finalize(st);
    g_string_free(sql, TRUE);
    if (ok && after->status == TASK_STATUS_DONE && before->status != TASK_STATUS_DONE)
        parent_started(db, after->id);
    return ok ? 1 : -1;
}

/* ---------------------------------------------------------------------------
 * task_db_task_set_status() — write the status, stamping completed_at on
 * the way into Done (see db.h).  The same rule as task_db_task_update, so
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
 * like pinned/priority: a change to it is a change to the task.
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_set_status(TaskDatabase *db, gint64 id, TaskStatus status)
{
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
        ok = step_done(db, st, "task set status");
    } else {
        step_done(db, NULL, "task set status");
    }
    sqlite3_finalize(st);
    if (status == TASK_STATUS_DONE)
        parent_started(db, id);
    return ok;
}

/* task_db_task_set_pinned() — toggle the local-only pin (see db.h).
 * Deliberately NO updated_at bump: the pin is local-only and must not
 * dirty the row for sync (a bump makes newest-wins push a no-op PATCH
 * and can starve a concurrent remote edit behind a 412).                   */
gboolean
task_db_task_set_pinned(TaskDatabase *db, gint64 id, gboolean pinned)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET pinned = %d WHERE id = %lld",
        pinned ? 1 : 0, (long long)id);
    gboolean ok = exec(db, sql);
    g_free(sql);
    return ok;
}

/* task_db_task_set_priority() — toggle the local-only high-priority flag
 * (see db.h).  Deliberately NO updated_at bump (see set_pinned above).     */
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
 * Local-only: no updated_at bump (see set_pinned).
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
        ok = step_done(db, st, "task recur apply");
    } else {
        step_done(db, NULL, "task recur apply");
    }
    sqlite3_finalize(st);
    return ok;
}

/* task_db_task_recur_set_next() — store the next-occurrence stamp alone
 * (see db.h).  Deliberately NO updated_at bump, the same rule
 * set_pinned/set_priority follow: this is local bookkeeping, and seeding
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
        ok = step_done(db, st, "task recur set next");
    } else {
        step_done(db, NULL, "task recur set next");
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
        step_done(db, NULL, "subtask_move prepare");
    } else if (task_db_transaction_begin(db)) {
        /* One statement, one transaction: a row per sibling, not a
         * journalled write per sibling. */
        gboolean ok = TRUE;
        for (guint i = 0; ok && i < subs->len; i++) {
            sqlite3_reset(st);
            sqlite3_bind_int(st, 1, (int)i);
            sqlite3_bind_int64(st, 2, ((Task *)g_ptr_array_index(subs, i))->id);
            ok = step_done(db, st, "subtask_move");
        }
        moved = task_db_transaction_end(db, ok);
    }
    sqlite3_finalize(st);
    task_ptr_array_free_tasks(subs);
    return moved;
}

/* ---------------------------------------------------------------------------
 * task_db_task_delete() — tombstone the task and its subtasks, in ONE
 * transaction.
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE parent_id = %lld;"
        "UPDATE tasks SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    gboolean ok = exec_txn(db, sql);
    sqlite3_free(sql);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_db_tasks_delete_done() — tombstone a list's completed top-level
 * tasks and their subtasks, in ONE transaction (see db.h).  The subtasks
 * go first: the second statement is what the first selects them by.
 * ------------------------------------------------------------------------- */
guint
task_db_tasks_delete_done(TaskDatabase *db, gint64 list_id)
{
    long long stamp = (long long)now();
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE deleted = 0 AND parent_id IN "
        "    (SELECT id FROM tasks WHERE list_id = %lld AND parent_id IS NULL "
        "       AND status = " DONE_SQL " AND deleted = 0);"
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE list_id = %lld AND parent_id IS NULL AND status = " DONE_SQL " AND deleted = 0;",
        stamp, (long long)list_id, stamp, (long long)list_id);
    exec_txn(db, sql);
    sqlite3_free(sql);
    /* The changes of the last UPDATE: the parents.  Zero after a rollback
     * too, since the rollback is not a write of its own. */
    return (guint)MAX(sqlite3_changes(shared_db_handle(db->shared)), 0);
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
        if (step_done(db, st, "attachment add"))
            id = sqlite3_last_insert_rowid(shared_db_handle(db->shared));
    } else {
        step_done(db, NULL, "attachment add");
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

/* task_db_totals() — non-tombstoned task/list counts (see db.h).           */
void
task_db_totals(TaskDatabase *db, gint *n_tasks, gint *n_lists)
{
    const struct {
        const gchar *sql;
        gint        *out;            /* caller's slot (may be NULL)         */
    } q[] = {
        { "SELECT COUNT(*) FROM tasks WHERE deleted = 0", n_tasks },
        { "SELECT COUNT(*) FROM lists WHERE deleted = 0", n_lists },
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

/* ---------------------------------------------------------------------------
 * Sync-side mutators (no updated_at stamp — see db.h).
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * task_db_task_move_list() — cross-list move (see db.h).
 * ------------------------------------------------------------------------- */
gboolean
task_db_task_move_list(TaskDatabase *db, gint64 id, gint64 dest_list)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET list_id = %lld, updated_at = %lld, "
        "  position = (SELECT COALESCE(MAX(position), 0) + 1 FROM tasks "
        "              WHERE list_id = %lld AND parent_id IS NULL) "
        "  WHERE id = %lld;"
        "UPDATE tasks SET list_id = %lld, updated_at = %lld "
        "  WHERE parent_id = %lld;",
        (long long)dest_list, (long long)now(), (long long)dest_list,
        (long long)id,
        (long long)dest_list, (long long)now(), (long long)id);
    gboolean ok = exec_txn(db, sql);
    sqlite3_free(sql);
    return ok;
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
 *   id      — the list or task id bound to ?1
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
        step_done(db, NULL, "purge check");
    }
    sqlite3_finalize(st);
    g_free(ask);

    gboolean ok = !linked;
    for (const gchar *const *sql = deletes; ok && *sql != NULL; sql++) {
        st = NULL;
        if (sqlite3_prepare_v2(sq, *sql, -1, &st, NULL) == SQLITE_OK)
            sqlite3_bind_int64(st, 1, id);
        ok = step_done(db, st, "purge");
        sqlite3_finalize(st);
    }
    return task_db_transaction_end(db, ok);
}

/* task_db_list_purge() — physically delete a list row + all its tasks'
 * rows (attachments cascade), unless one of them is a Notes action item
 * (see db.h).                                                               */
gboolean
task_db_list_purge(TaskDatabase *db, gint64 id)
{
    static const gchar *const deletes[] = {
        "DELETE FROM tasks WHERE list_id = ?1 AND parent_id IS NOT NULL",
        "DELETE FROM tasks WHERE list_id = ?1",
        "DELETE FROM lists WHERE id = ?1",
        NULL
    };
    return purge_unless_linked(db, "SELECT id FROM tasks WHERE list_id = ?1",
                               deletes, id);
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
        step_done(db, NULL, "task note");
    }
    sqlite3_finalize(st);
    return title;
}
