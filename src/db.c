/* ===========================================================================
 * db.c — SQLite storage for Tasks (see db.h)
 * =========================================================================== */

#include "db.h"
#include "plugin_owner.h"
#include <glib/gstdio.h>             /* g_unlink, g_stat                    */
#include <string.h>

/* ---------------------------------------------------------------------------
 * exec() — run one or more statements with no results; failures are
 * logged, not fatal.  Returns TRUE when everything ran.
 * ------------------------------------------------------------------------- */
static gboolean
exec(TaskDatabase *db, const gchar *sql)
{
    gchar *msg = NULL;               /* sqlite's error text                 */
    if (sqlite3_exec(db->sq, sql, NULL, NULL, &msg) != SQLITE_OK) {
        g_warning("db: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * scalar() — run a one-value SELECT and return it, or -1 when the
 * statement could not run at all.  The -1 matters: a verification query
 * that never executed must not be mistaken for one that returned zero
 * problems, which is the "checked, all good, when nothing was checked"
 * failure the error discipline exists to prevent.
 * ------------------------------------------------------------------------- */
gint64
task_db_scalar(TaskDatabase *db, const gchar *sql)
{
    sqlite3_stmt *st = NULL;
    gint64 v = -1;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* task_db_exec_sql() / task_db_exec_query() — see db.h.  Both are thin
 * public faces on the internal exec paths, so there is exactly one
 * implementation whether the caller is in-tree or a plugin.             */
gboolean
task_db_exec_sql(TaskDatabase *db, const gchar *sql)
{
    return exec(db, sql);
}

typedef struct {
    gint (*cb)(gpointer, gint, gchar **, gchar **);
    gpointer user_data;
} QueryTramp;

static int
query_tramp(void *data, int n_cols, char **values, char **names)
{
    QueryTramp *q = data;
    return q->cb(q->user_data, n_cols, values, names);
}

gboolean
task_db_exec_query(TaskDatabase *db, const gchar *sql,
                   gint (*cb)(gpointer, gint, gchar **, gchar **),
                   gpointer user_data)
{
    QueryTramp q = { cb, user_data };
    gchar *msg = NULL;
    gint rc = sqlite3_exec(db->sq, sql, query_tramp, &q, &msg);
    /* SQLITE_ABORT is a callback asking to stop, which is the documented
     * way to read only the rows you need — not a failure.               */
    if (rc != SQLITE_OK && rc != SQLITE_ABORT) {
        g_warning("db: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    sqlite3_free(msg);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * exec_txn() — run `sql` (one or more statements) inside a transaction,
 * ROLLING BACK on failure.  A bare "BEGIN;…;COMMIT;" through exec()
 * would leave the connection stuck inside an open transaction when a
 * middle statement fails (e.g. SQLITE_BUSY against the sync worker) —
 * every later BEGIN would then fail and writes would silently vanish
 * until the connection closes.
 * ------------------------------------------------------------------------- */
static void
exec_txn(TaskDatabase *db, const gchar *sql)
{
    if (!exec(db, "BEGIN IMMEDIATE"))
        return;
    if (exec(db, sql)) {
        if (!exec(db, "COMMIT"))
            exec(db, "ROLLBACK");
    } else {
        exec(db, "ROLLBACK");
    }
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
 * Returns TRUE when the statement completed.
 * ------------------------------------------------------------------------- */
static gboolean
step_done(TaskDatabase *db, sqlite3_stmt *st, const gchar *ctx)
{
    if (st == NULL) {
        g_warning("db: %s: prepare failed: %s", ctx,
                  sqlite3_errmsg(db->sq));
        return FALSE;
    }
    if (sqlite3_step(st) != SQLITE_DONE) {
        g_warning("db: %s: %s", ctx, sqlite3_errmsg(db->sq));
        return FALSE;
    }
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

/* ---------------------------------------------------------------------------
 * task_db_default_path() — the standard db location (see db.h).
 * ------------------------------------------------------------------------- */
gchar *
task_db_default_path(void)
{
    gchar *dir = g_build_filename(g_get_user_data_dir(), TASK_APP_DIR, NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, TASK_DB_FILENAME, NULL);
    g_free(dir);
    return path;
}

/* task_db_resolve_path() — the file to open (see db.h).                    */
gchar *
task_db_resolve_path(const gchar *dir)
{
    if (dir != NULL && *dir != '\0')
        return g_build_filename(dir, TASK_DB_FILENAME, NULL);
    return task_db_default_path();     /* also creates the directory        */
}

/*
 * table_has_column — does `table` already carry a column called `column`?
 *
 * Inputs:
 *   db     — open connection
 *   table  — table name (a literal here; NOT interpolated from user data)
 *   column — column name to look for
 *
 * Output:
 *   TRUE when PRAGMA table_info names it.  FALSE when it does not — AND
 *   also when the PRAGMA could not run at all, which is the conservative
 *   answer for the only caller: a migration that then tries the ALTER and
 *   reports sqlite's own message through exec(), rather than one that
 *   silently decides the column is already there and moves on.
 */
static gboolean
table_has_column(TaskDatabase *db, const gchar *table, const gchar *column)
{
    gchar *sql = sqlite3_mprintf("PRAGMA table_info(%Q)", table);
    sqlite3_stmt *st = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK)
        while (!found && sqlite3_step(st) == SQLITE_ROW) {
            const guchar *n = sqlite3_column_text(st, 1);   /* 1 = name     */
            found = n != NULL && g_strcmp0((const gchar *)n, column) == 0;
        }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    return found;
}

/* ---------------------------------------------------------------------------
 * task_db_verify_file() — integrity_check + foreign_key_check on a separate
 * read-only connection (see db.h).
 *
 * Both exec return codes are load-bearing, the same rule
 * startup_integrity_check follows: a PRAGMA that never RAN collects no
 * rows, which is indistinguishable from a clean result if you only look
 * at the collector.  Reporting "verified" when nothing was checked is the
 * one answer this function must never give — it is what a caller is about
 * to delete the original on.
 * ------------------------------------------------------------------------- */
static int
verify_collect(void *data, int argc, char **argv, char **cols)
{
    (void)cols;
    GString *out = data;
    for (int i = 0; i < argc; i++)
        if (argv[i] != NULL && g_strcmp0(argv[i], "ok") != 0) {
            if (out->len > 0)
                g_string_append_c(out, '\n');
            g_string_append(out, argv[i]);
        }
    return 0;
}

gboolean
task_db_verify_file(const gchar *path, gchar **detail)
{
    if (detail != NULL)
        *detail = NULL;
    gchar   *uri = g_strdup_printf("file:%s?mode=ro", path);
    sqlite3 *sq  = NULL;
    if (sqlite3_open_v2(uri, &sq, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                        NULL) != SQLITE_OK) {
        if (detail != NULL)
            *detail = g_strdup_printf("cannot open %s: %s", path,
                sq != NULL ? sqlite3_errmsg(sq) : "?");
        sqlite3_close(sq);
        g_free(uri);
        return FALSE;
    }
    g_free(uri);

    GString *bad = g_string_new(NULL);
    gboolean ran = TRUE;
    gchar   *msg = NULL;
    if (sqlite3_exec(sq, "PRAGMA integrity_check", verify_collect, bad,
                     &msg) != SQLITE_OK) {
        ran = FALSE;
        g_string_append_printf(bad, "integrity_check did not run: %s",
                               msg != NULL ? msg : "?");
    }
    sqlite3_free(msg);
    msg = NULL;
    if (ran && sqlite3_exec(sq, "PRAGMA foreign_key_check", verify_collect,
                            bad, &msg) != SQLITE_OK) {
        ran = FALSE;
        g_string_append_printf(bad, "foreign_key_check did not run: %s",
                               msg != NULL ? msg : "?");
    }
    sqlite3_free(msg);
    sqlite3_close(sq);

    gboolean ok = ran && bad->len == 0;
    if (!ok && detail != NULL)
        *detail = g_strdup(bad->str);
    g_string_free(bad, TRUE);
    return ok;
}

/* task_db_copy_file() — a transactionally consistent copy (see db.h).      */
gboolean
task_db_copy_file(TaskDatabase *db, const gchar *dest, gchar **err)
{
    if (err != NULL)
        *err = NULL;
    gchar *q   = sqlite3_mprintf("VACUUM INTO %Q", dest);
    gchar *msg = NULL;
    gboolean ok = (sqlite3_exec(db->sq, q, NULL, NULL, &msg) == SQLITE_OK);
    if (!ok && err != NULL)
        *err = g_strdup(msg != NULL ? msg : "?");
    sqlite3_free(msg);
    sqlite3_free(q);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_db_open() — open + create/migrate the schema (see db.h).
 * ------------------------------------------------------------------------- */
TaskDatabase *
task_db_open(const gchar *path, GError **err)
{
    sqlite3 *sq = NULL;
    if (sqlite3_open(path, &sq) != SQLITE_OK) {
        g_set_error(err, g_quark_from_static_string("task-db"), 1,
                    "cannot open %s: %s", path,
                    sq != NULL ? sqlite3_errmsg(sq) : "?");
        if (sq != NULL)
            sqlite3_close(sq);
        return NULL;
    }
    TaskDatabase *db = g_new0(TaskDatabase, 1);
    db->sq   = sq;
    db->path = g_strdup(path);

    sqlite3_busy_timeout(sq, 5000);  /* GUI + sync worker share the file    */
    exec(db, "PRAGMA foreign_keys = ON");

    /* The schema, in full.  CREATE IF NOT EXISTS keeps reopen cheap, and
     * every column is declared HERE, so a FRESH file is complete the
     * moment this block has run and needs no migration at all.  An
     * EXISTING file reaches the same shape through the guarded ALTERs
     * below — v10 added the recurrence columns, and CREATE IF NOT EXISTS
     * is a no-op on a file that already has the table (gotcha 24).
     * list_groups comes first: lists.group_id references it.              */
    exec(db,
        "CREATE TABLE IF NOT EXISTS list_groups ("
        "  id       INTEGER PRIMARY KEY,"
        "  name     TEXT    NOT NULL DEFAULT '',"
        "  position INTEGER NOT NULL DEFAULT 0)");
    exec(db,
        "CREATE TABLE IF NOT EXISTS lists ("
        "  id         INTEGER PRIMARY KEY,"
        "  name       TEXT    NOT NULL DEFAULT '',"
        "  emoji      TEXT    NOT NULL DEFAULT '',"
        "  position   INTEGER NOT NULL DEFAULT 0,"
        "  group_id   INTEGER REFERENCES list_groups(id),"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  deleted    INTEGER NOT NULL DEFAULT 0)");
    /* The recurrence columns' DEFAULTs come from the macros rather than
     * from literals, and so does the v10 ALTER block below: two spellings
     * of "8am, five days ahead" is how a fresh file and a migrated one
     * come to disagree about what an untouched row means.                  */
    {
        gchar *tasks_sql = g_strdup_printf(
            "CREATE TABLE IF NOT EXISTS tasks ("
            "  id           INTEGER PRIMARY KEY,"
            "  list_id      INTEGER NOT NULL REFERENCES lists(id),"
            "  parent_id    INTEGER REFERENCES tasks(id),"
            "  title        TEXT    NOT NULL DEFAULT '',"
            "  notes        TEXT    NOT NULL DEFAULT '',"
            "  due          INTEGER NOT NULL DEFAULT 0,"
            "  status       INTEGER NOT NULL DEFAULT 0,"
            "  pinned       INTEGER NOT NULL DEFAULT 0,"
            "  priority     INTEGER NOT NULL DEFAULT 0,"
            "  position     INTEGER NOT NULL DEFAULT 0,"
            "  updated_at   INTEGER NOT NULL DEFAULT 0,"
            "  deleted      INTEGER NOT NULL DEFAULT 0,"
            "  completed_at INTEGER NOT NULL DEFAULT 0,"
            /* The recurrence schedule (v10, plus recur_start at v11).
             * Declared HERE so a fresh file needs no migration at all;
             * the v10 and v11 blocks below ADD the same columns to a
             * file that predates them.                                 */
            "  recur_interval INTEGER NOT NULL DEFAULT 0,"
            "  recur_unit     INTEGER NOT NULL DEFAULT 0,"
            "  recur_time     INTEGER NOT NULL DEFAULT %d,"
            "  recur_lead     INTEGER NOT NULL DEFAULT %d,"
            "  recur_next     INTEGER NOT NULL DEFAULT 0,"
            "  recur_start    INTEGER NOT NULL DEFAULT 0,"
            /* The due date's time of day (v12).  Its DEFAULT is the whole
             * migration for existing rows: every due date that nobody has
             * timed by hand means 08:00, here and in the ALTER below.   */
            "  due_time       INTEGER NOT NULL DEFAULT %d)",
            TASK_RECUR_TIME_DEFAULT, TASK_RECUR_LEAD_DEFAULT,
            TASK_DUE_TIME_DEFAULT);
        exec(db, tasks_sql);
        g_free(tasks_sql);
    }
    exec(db,
        "CREATE TABLE IF NOT EXISTS attachments ("
        "  id         INTEGER PRIMARY KEY,"
        "  task_id    INTEGER NOT NULL REFERENCES tasks(id)"
        "                     ON DELETE CASCADE,"
        "  path       TEXT    NOT NULL,"
        "  added_at   INTEGER NOT NULL DEFAULT 0)");
    exec(db,
        "CREATE TABLE IF NOT EXISTS sync_state ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT)");
    /* NO integration-owned tables here.  A side table belongs to whatever
     * owns the integration, and every one of them is now a plugin that
     * creates its own from its db_open hook (see plugin.h).  The two
     * MIGRATIONS below still name those tables — they have to, because a
     * migration moves data that already exists whether or not the plugin
     * that will read it is installed — so each one creates what it needs
     * itself rather than relying on a block up here.                     */
    exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_list "
             "ON tasks(list_id, parent_id, position)");

    sqlite3_stmt *vst = NULL;
    gint uv = 0;                     /* the file's schema version           */
    if (sqlite3_prepare_v2(sq, "PRAGMA user_version", -1, &vst, NULL)
        == SQLITE_OK && sqlite3_step(vst) == SQLITE_ROW)
        uv = sqlite3_column_int(vst, 0);
    sqlite3_finalize(vst);

    /* ---------------------------------------------------------------------
     * BACK THE FILE UP BEFORE ANY MIGRATION RUNS.
     *
     * This is live, not a placeholder: it fires for every file older than
     * TASK_DB_SCHEMA_VERSION, and v8, v9 and v10 have all used it.  A
     * later schema change inherits it for free — bump the constant and
     * add the migration below this block, and the backup happens.
     *
     * Not paranoia.  A table-rewriting migration (`ALTER TABLE … DROP
     * COLUMN`) running with no backup, against a database in a sync
     * folder, is exactly how a 1965-task database became unrecoverable on
     * 2026-08-26.  One file per FROM-version, never overwritten, so
     * repeated launches cannot erode it.
     * ------------------------------------------------------------------- */
    if (uv > 0 && uv < TASK_DB_SCHEMA_VERSION) {
        gchar *bak = g_strdup_printf("%s.pre-v%d.bak", path, uv);
        if (!g_file_test(bak, G_FILE_TEST_EXISTS)) {
            /* VACUUM INTO, not a byte copy: it is transactionally
             * consistent, so it cannot capture a torn page even if
             * something else is mid-write.                              */
            gchar *q = sqlite3_mprintf("VACUUM INTO %Q", bak);
            gchar *msg = NULL;
            if (sqlite3_exec(sq, q, NULL, NULL, &msg) == SQLITE_OK)
                g_message("Backed up the pre-v%d database to %s before "
                          "migrating", uv, bak);
            else
                g_warning("could not back up %s before the v%d migration "
                          "(%s) — MIGRATING ANYWAY", path, uv,
                          msg != NULL ? msg : "?");
            sqlite3_free(msg);
            sqlite3_free(q);
        }
        g_free(bak);
    }

    /* Stamp the version, then READ IT BACK.  A PRAGMA that reports success
     * without taking effect leaves the file claiming an older schema than
     * it has, and every later launch re-runs migrations it does not need —
     * silently, since re-running them is harmless.  That is exactly the
     * "checked, all good, when nothing was checked" failure the error
     * discipline forbids, so it gets a warning naming the file.  Seen once
     * on a database living in an iCloud Drive folder, where two copies of
     * the file can diverge; never reproduced locally.                      */
    /* -----------------------------------------------------------------
     * v8 — the Google Tasks columns move to side tables the sync owns.
     *
     * COPY, VERIFY, and only then DROP.  The drop rewrites the whole
     * tasks table, and doing that to someone's only copy after a copy
     * that silently did nothing is precisely how a 1965-task database
     * became unrecoverable.  A verify that fails leaves every column in
     * place and says so: an un-migrated database still works, because
     * the readers below no longer look at those columns either way.
     *
     * The copy is guarded on gtasks_id/etag/... EXISTING, which they do
     * not on a fresh file — hence the version test rather than an
     * unconditional run.
     * ----------------------------------------------------------------- */
    if (uv > 0 && uv < 8) {
        gboolean copied =
            /* The destinations are created HERE rather than in the schema
             * block above: the Google sync is a plugin and owns them, and
             * this migration must still run on a database whose owner is
             * not installed.  IF NOT EXISTS because the plugin's own
             * db_open may have created them already.                     */
            exec(db, "CREATE TABLE IF NOT EXISTS gtasks_list ("
                     "  list_id   INTEGER PRIMARY KEY REFERENCES lists(id)"
                     "            ON DELETE CASCADE,"
                     "  gtasks_id TEXT)") &&
            exec(db, "CREATE TABLE IF NOT EXISTS gtasks_task ("
                     "  task_id   INTEGER PRIMARY KEY REFERENCES tasks(id)"
                     "            ON DELETE CASCADE,"
                     "  gtasks_id TEXT,"
                     "  etag      TEXT,"
                     "  web_link  TEXT,"
                     "  glinks    TEXT,"
                     "  assigned  TEXT)") &&
            exec(db, "INSERT OR REPLACE INTO gtasks_list (list_id, gtasks_id)"
                     "  SELECT id, gtasks_id FROM lists"
                     "   WHERE gtasks_id IS NOT NULL") &&
            exec(db, "INSERT OR REPLACE INTO gtasks_task"
                     "       (task_id, gtasks_id, etag, web_link, glinks,"
                     "        assigned)"
                     "  SELECT id, gtasks_id, etag, web_link, glinks,"
                     "         assigned FROM tasks"
                     "   WHERE gtasks_id IS NOT NULL OR etag IS NOT NULL"
                     "      OR web_link IS NOT NULL OR glinks IS NOT NULL"
                     "      OR assigned IS NOT NULL");

        /* VERIFY: every row that had a remote identity must have one now,
         * and it must be the SAME one.  Counting is not enough — a copy
         * that wrote the right number of wrong rows would pass that.     */
        gint64 wrong = copied
            ? task_db_scalar(db, "SELECT COUNT(*) FROM tasks t"
                         " LEFT JOIN gtasks_task g ON g.task_id = t.id"
                         " WHERE (t.gtasks_id IS NOT NULL"
                         "        AND g.gtasks_id IS NOT t.gtasks_id)"
                         "    OR (t.etag IS NOT NULL"
                         "        AND g.etag IS NOT t.etag)")
            : -1;
        gint64 wrong_l = copied && wrong == 0
            ? task_db_scalar(db, "SELECT COUNT(*) FROM lists l"
                         " LEFT JOIN gtasks_list g ON g.list_id = l.id"
                         " WHERE l.gtasks_id IS NOT NULL"
                         "   AND g.gtasks_id IS NOT l.gtasks_id")
            : -1;

        if (copied && wrong == 0 && wrong_l == 0) {
            /* An sqlite too old for DROP COLUMN (< 3.35) simply leaves the
             * columns behind, unread.  Harmless: nothing selects them any
             * more, and the data now lives in the side tables.           */
            exec(db, "ALTER TABLE lists DROP COLUMN gtasks_id");
            exec(db, "ALTER TABLE tasks DROP COLUMN gtasks_id");
            exec(db, "ALTER TABLE tasks DROP COLUMN etag");
            exec(db, "ALTER TABLE tasks DROP COLUMN web_link");
            exec(db, "ALTER TABLE tasks DROP COLUMN glinks");
            exec(db, "ALTER TABLE tasks DROP COLUMN assigned");
            g_message("Migrated %s to schema v8: the Google Tasks columns "
                      "now live in gtasks_task / gtasks_list", path);
        } else {
            g_warning("db: %s — the v8 copy did not verify (%lld task "
                      "mismatches, %lld list mismatches); the old columns "
                      "were LEFT IN PLACE and nothing was dropped",
                      path, (long long)wrong, (long long)wrong_l);
        }
    }

    /* -----------------------------------------------------------------
     * v9 — the Notes mirror's columns move to its own side table, the
     * same shape v8 gave the Google sync.  COPY, VERIFY, then DROP.
     *
     * The INDEX goes first: SQLite refuses ALTER TABLE ... DROP COLUMN on
     * an indexed column, so leaving idx_tasks_bn_uid in place would fail
     * the drop and strand the migration half-applied.
     * ----------------------------------------------------------------- */
    if (uv > 0 && uv < 9) {
        gboolean copied =
            exec(db, "CREATE TABLE IF NOT EXISTS notes_task ("
                     "  task_id INTEGER PRIMARY KEY REFERENCES tasks(id)"
                     "          ON DELETE CASCADE,"
                     "  uid     INTEGER NOT NULL,"
                     "  done    INTEGER NOT NULL DEFAULT 0,"
                     "  due     INTEGER NOT NULL DEFAULT 0)") &&
            exec(db, "CREATE INDEX IF NOT EXISTS idx_notes_task_uid "
                     "ON notes_task(uid)") &&
            exec(db, "CREATE TABLE IF NOT EXISTS notes_deleted ("
                     "  uid INTEGER PRIMARY KEY)") &&
            exec(db, "INSERT OR REPLACE INTO notes_task"
                     "       (task_id, uid, done, due)"
                     "  SELECT id, bn_uid, bn_done, bn_due FROM tasks"
                     "   WHERE bn_uid > 0") &&
            exec(db, "INSERT OR IGNORE INTO notes_deleted (uid)"
                     "  SELECT uid FROM bn_deleted");

        gint64 wrong = copied
            ? task_db_scalar(db,
                  "SELECT COUNT(*) FROM tasks t"
                  " LEFT JOIN notes_task n ON n.task_id = t.id"
                  " WHERE t.bn_uid > 0"
                  "   AND (n.uid IS NOT t.bn_uid"
                  "     OR n.done IS NOT t.bn_done"
                  "     OR n.due IS NOT t.bn_due)")
            : -1;
        gint64 lost = copied && wrong == 0
            ? task_db_scalar(db, "SELECT (SELECT COUNT(*) FROM bn_deleted)"
                                 "     - (SELECT COUNT(*) FROM notes_deleted)")
            : -1;

        if (copied && wrong == 0 && lost == 0) {
            exec(db, "DROP INDEX IF EXISTS idx_tasks_bn_uid");
            exec(db, "ALTER TABLE tasks DROP COLUMN bn_uid");
            exec(db, "ALTER TABLE tasks DROP COLUMN bn_done");
            exec(db, "ALTER TABLE tasks DROP COLUMN bn_due");
            exec(db, "DROP TABLE IF EXISTS bn_deleted");

            /* bn_pins / bn_priority stored a Notes item's pin and
             * priority back when action items were a special row type;
             * the 2026-08-05 mirror rewrite made them ordinary tasks
             * carrying the ordinary flags, and nothing has referenced
             * either table since.  Dropped ONLY when provably empty — a
             * row in one is evidence the assumption is wrong, and that is
             * worth more than the tidiness.                             */
            if (task_db_scalar(db, "SELECT COUNT(*) FROM bn_pins") == 0)
                exec(db, "DROP TABLE IF EXISTS bn_pins");
            else
                g_warning("db: bn_pins is not empty \xe2\x80\x94 left in place");
            if (task_db_scalar(db, "SELECT COUNT(*) FROM bn_priority") == 0)
                exec(db, "DROP TABLE IF EXISTS bn_priority");
            else
                g_warning("db: bn_priority is not empty \xe2\x80\x94 left in place");

            g_message("Migrated %s to schema v9: the Notes mirror's "
                      "columns now live in notes_task / notes_deleted",
                      path);
        } else {
            g_warning("db: %s \xe2\x80\x94 the v9 copy did not verify (%lld task "
                      "mismatches, %lld suppressions lost); the old columns "
                      "were LEFT IN PLACE", path,
                      (long long)wrong, (long long)lost);
        }
    }

    /* -----------------------------------------------------------------
     * v10 — the recurrence schedule joins the task row.
     *
     * ADD COLUMN only: nothing is copied, nothing is dropped, and the
     * whole table is not rewritten, so this is the cheap and safe end of
     * the migration spectrum — the opposite of v7's DROP COLUMN.  Five
     * columns whose defaults mean "does not recur", which is what every
     * existing task is.
     *
     * Guarded on the COLUMN not existing rather than on the version
     * alone, so it is idempotent: the version stamp has been seen not to
     * stick on a database living in a sync folder (see below), and an
     * ALTER re-run on a healthy file would log "duplicate column name"
     * on every launch.  A warning that fires on the ordinary path is a
     * warning nobody reads.
     *
     * NO INDEX is created on these.  That is deliberate — an index on an
     * ALTER-added column must be created after the migrations rather
     * than in the schema block (gotcha 16), and the recurrence pass
     * scans a few thousand rows every few minutes, which wants no index
     * at all.
     * ----------------------------------------------------------------- */
    {
        static const struct { const gchar *name; gint def; } recur_cols[] = {
            { "recur_interval", 0 },
            { "recur_unit",     0 },
            { "recur_time",     TASK_RECUR_TIME_DEFAULT },
            { "recur_lead",     TASK_RECUR_LEAD_DEFAULT },
            { "recur_next",     0 },
        };
        gint added = 0;
        for (gsize i = 0; i < G_N_ELEMENTS(recur_cols); i++) {
            if (table_has_column(db, "tasks", recur_cols[i].name))
                continue;
            gchar *sql = g_strdup_printf(
                "ALTER TABLE tasks ADD COLUMN %s INTEGER NOT NULL "
                "DEFAULT %d", recur_cols[i].name, recur_cols[i].def);
            if (exec(db, sql))
                added++;
            g_free(sql);
        }
        if (added > 0)
            g_message("Migrated %s to schema v10: added %d recurrence "
                      "column(s) to tasks", path, added);
    }

    /* -----------------------------------------------------------------
     * v11 — recur_start, the schedule's own anchor date.
     *
     * A block of its own rather than a sixth row in the v10 table above,
     * so the log message keeps saying which version added what.  Same
     * shape and same guard: ADD COLUMN, keyed on the column not existing
     * so it is idempotent (gotcha 24).
     *
     * DEFAULT 0 means "unset", and unset is exactly what every existing
     * recurring task wants: the anchor then falls back to the due date,
     * which is the rule those tasks were created under.
     * ----------------------------------------------------------------- */
    if (!table_has_column(db, "tasks", "recur_start")) {
        if (exec(db, "ALTER TABLE tasks ADD COLUMN recur_start INTEGER "
                     "NOT NULL DEFAULT 0"))
            g_message("Migrated %s to schema v11: added tasks.recur_start",
                      path);
    }

    /* -----------------------------------------------------------------
     * v12 — due_time, the time of day a due DATE means.
     *
     * ADD COLUMN, guarded on the column (gotcha 24), and the DEFAULT does
     * the whole data migration: every existing due date comes back
     * meaning 08:00, which is what "all due dates default to 8am" asks
     * for.  Nothing is rewritten and no row is touched.
     *
     * The value is built from TASK_DUE_TIME_DEFAULT rather than written
     * as a literal, because the CREATE above spells the same default and
     * two spellings is how a fresh file and a migrated one come to
     * disagree about what an untouched row means (gotcha 24 again).
     * ----------------------------------------------------------------- */
    if (!table_has_column(db, "tasks", "due_time")) {
        gchar *sql = g_strdup_printf(
            "ALTER TABLE tasks ADD COLUMN due_time INTEGER NOT NULL "
            "DEFAULT %d", TASK_DUE_TIME_DEFAULT);
        if (exec(db, sql))
            g_message("Migrated %s to schema v12: added tasks.due_time",
                      path);
        g_free(sql);
    }

    {   /* Stamped from the CONSTANT for the same reason.               */
        gchar *stamp = g_strdup_printf("PRAGMA user_version = %d",
                                       TASK_DB_SCHEMA_VERSION);
        exec(db, stamp);
        g_free(stamp);
    }
    gint uv_after = -1;              /* what the file now claims            */
    vst = NULL;
    if (sqlite3_prepare_v2(sq, "PRAGMA user_version", -1, &vst, NULL)
        == SQLITE_OK && sqlite3_step(vst) == SQLITE_ROW)
        uv_after = sqlite3_column_int(vst, 0);
    sqlite3_finalize(vst);
    /* Checked against the CONSTANT, not a literal: the two drifted apart
     * once already, and a check comparing to the wrong number warns on
     * every SUCCESSFUL migration.                                      */
    if (uv_after != TASK_DB_SCHEMA_VERSION)
        g_warning("db: %s still reports schema version %d after the "
                  "migration to %d — the version stamp did not stick "
                  "(migrations will re-run harmlessly on every launch)",
                  path, uv_after, TASK_DB_SCHEMA_VERSION);
    return db;
}

/* task_db_close() — close the connection (see db.h).                       */
void
task_db_close(TaskDatabase *db)
{
    if (db == NULL)
        return;
    sqlite3_close(db->sq);
    g_free(db->path);
    g_free(db);
}

/* ---------------------------------------------------------------------------
 * read_list() — build a TaskList from the standard lists SELECT
 * (LIST_COLS: id, name, position, gtasks_id, updated_at, deleted, emoji).
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
        if (sqlite3_prepare_v2(db->sq,
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
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK)
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
void
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
    exec_txn(db, sql->str);
    g_string_free(sql, TRUE);
}

/* task_db_list_get() — one list row, tombstoned or not; NULL if absent.    */
TaskList *
task_db_list_get(TaskDatabase *db, gint64 id)
{
    sqlite3_stmt *st = NULL;
    TaskList *l = NULL;
    if (sqlite3_prepare_v2(db->sq,
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
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO lists(name, emoji, position, updated_at) "
            "VALUES(?, ?, "
            "(SELECT COALESCE(MAX(position), 0) + 1 FROM lists), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "list create"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "list create");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_list_update() — rename/re-emoji + stamp.                         */
void
task_db_list_update(TaskDatabase *db, gint64 id, const gchar *name,
                    const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET name = ?, emoji = ?, updated_at = ? "
            "WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        sqlite3_bind_int64(st, 4, id);
        step_done(db, st, "list update");
    } else {
        step_done(db, NULL, "list update");
    }
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------------------------
 * task_db_list_delete() — tombstone the list and every task in it.
 * ------------------------------------------------------------------------- */
void
task_db_list_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE list_id = %lld;"
        "UPDATE lists SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * task_db_list_restore() — undo a list tombstone (see db.h).  Tombstones
 * only persist until a sync pushes them, so any task tombstone still in
 * the list belongs to the same refused deletion and is restored too.
 * ------------------------------------------------------------------------- */
void
task_db_list_restore(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 0, updated_at = %lld "
        "  WHERE list_id = %lld AND deleted = 1;"
        "UPDATE lists SET deleted = 0, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
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
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK)
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
    if (sqlite3_prepare_v2(db->sq,
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
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO list_groups(name) VALUES(?)", -1, &st, NULL)
            == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        if (step_done(db, st, "group create"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "group create");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_group_delete() — remove the group; un-groups all its lists.      */
void
task_db_group_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE lists SET group_id = NULL WHERE group_id = %lld;"
        "DELETE FROM list_groups WHERE id = %lld;",
        (long long)id, (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* task_db_group_rename() — update the group's name.                        */
void
task_db_group_rename(TaskDatabase *db, gint64 id, const gchar *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE list_groups SET name = ? WHERE id = ?", -1, &st, NULL)
            == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        step_done(db, st, "group rename");
    } else {
        step_done(db, NULL, "group rename");
    }
    sqlite3_finalize(st);
}

/* task_db_list_set_group() — assign a list to a group (0 = ungrouped).     */
void
task_db_list_set_group(TaskDatabase *db, gint64 list_id, gint64 group_id)
{
    gchar *sql = group_id == 0
        ? sqlite3_mprintf(
              "UPDATE lists SET group_id = NULL WHERE id = %lld;",
              (long long)list_id)
        : sqlite3_mprintf(
              "UPDATE lists SET group_id = %lld WHERE id = %lld;",
              (long long)group_id, (long long)list_id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* task_db_list_emoji_if_empty() — seed the emoji of the list bound to
 * `gtasks_id` (see db.h).  Deliberately NO updated_at bump: the emoji
 * is local-only and must not dirty the row for sync.                       */
void
task_db_list_emoji_if_empty(TaskDatabase *db, gint64 list_id,
                            const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET emoji = ?1 WHERE id = ?2 AND "
            "emoji = '' AND deleted = 0", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, emoji, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, list_id);
        step_done(db, st, "list emoji seed");
    } else {
        step_done(db, NULL, "list emoji seed");
    }
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------------------------
 * read_task() — build a Task from the standard tasks SELECT.
 * ------------------------------------------------------------------------- */
#define TASK_COLS "id, list_id, COALESCE(parent_id, 0), title, notes, due, " \
                  "status, pinned, position, updated_at, deleted, "\
                  "completed_at, priority, "\
                  "recur_interval, recur_unit, recur_time, recur_lead, "\
                  "recur_next, recur_start, due_time"

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
                                                    : TASK_DUE_TIME_DEFAULT;
    }
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
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK) {
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

/* task_db_subtasks_all_visible() — every visible subtask, one query.       */
GPtrArray *
task_db_subtasks_all_visible(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NOT NULL AND "
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
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK &&
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

/* task_db_tasks_due_between() — the Due Today / Weekly Forecast views.     */
GPtrArray *
task_db_tasks_due_between(TaskDatabase *db, gint64 lo, gint64 hi)
{
    return task_query(db,
        /* due_time joins the sort so a day's tasks read in the order they
         * actually come due — `due` alone is midnight for every row in
         * the bucket and so cannot separate them (the Weekly Forecast's
         * day sections are the visible case).                            */
        "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
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
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO tasks(list_id, parent_id, title, position, "
            "updated_at) VALUES(?, ?, ?, (SELECT COALESCE(MAX(position), 0)"
            " + 1 FROM tasks WHERE list_id = ?1 AND "
            "COALESCE(parent_id, 0) = COALESCE(?2, 0)), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        if (parent_id != 0)
            sqlite3_bind_int64(st, 2, parent_id);
        else
            sqlite3_bind_null(st, 2);
        sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, now());
        if (step_done(db, st, "task create"))
            id = sqlite3_last_insert_rowid(db->sq);
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
    if (sqlite3_prepare_v2(db->sq,
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

/* ---------------------------------------------------------------------------
 * task_db_task_update() — write the editable fields back (see db.h).
 * ------------------------------------------------------------------------- */
void
task_db_task_update(TaskDatabase *db, const Task *t)
{
    /* completed_at is stamped when the row ENTERS Done (the CASE reads
     * the OLD row values, gotcha 8) and is otherwise left exactly as it
     * is — leaving Done does NOT clear it.  It answers "when was this
     * last completed?", which stays a fact about the task after someone
     * reopens it, and a New → Done → In Progress → Done round trip
     * re-stamps only on the way back in.  updated_at is stamped
     * unconditionally here — this path
     * also writes title/notes/due, which Google does want.
     *
     * The recurrence schedule rides along because it is EDITED in the same
     * place (the editor's Advanced block) and saved by the same debounce.
     * It is local-only, so it does not by itself justify the bump — but
     * this statement is never the only thing being written, so there is no
     * "recurrence alone" path here to dirty a row needlessly.  Advancing
     * recur_next between edits goes through task_db_task_recur_set_next
     * instead, which stamps nothing.                                       */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?1, notes = ?2, due = ?3, "
            "completed_at = CASE WHEN ?4 = 2 AND status <> 2 THEN ?6 "
            "                    ELSE completed_at END, "
            "status = ?4, pinned = ?5, updated_at = ?6, priority = ?8, "
            "recur_interval = ?9, recur_unit = ?10, recur_time = ?11, "
            "recur_lead = ?12, recur_next = ?13, recur_start = ?14, "
            "due_time = ?15 "
            "WHERE id = ?7", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, t->due);
        sqlite3_bind_int(st, 4, (gint)t->status);
        sqlite3_bind_int(st, 5, t->pinned ? 1 : 0);
        sqlite3_bind_int64(st, 6, now());
        sqlite3_bind_int64(st, 7, t->id);
        sqlite3_bind_int(st, 8, t->priority ? 1 : 0);
        sqlite3_bind_int(st,  9, t->recur_interval);
        sqlite3_bind_int(st, 10, (gint)t->recur_unit);
        sqlite3_bind_int(st, 11, t->recur_time);
        sqlite3_bind_int(st, 12, t->recur_lead);
        sqlite3_bind_int64(st, 13, t->recur_next);
        sqlite3_bind_int64(st, 14, t->recur_start);
        sqlite3_bind_int(st, 15, t->due_time);
        step_done(db, st, "task update");
    } else {
        step_done(db, NULL, "task update");
    }
    sqlite3_finalize(st);
    if (t->status == TASK_STATUS_DONE)
        parent_started(db, t->id);
}


/* ---------------------------------------------------------------------------
 * task_db_task_set_status() — write the status, stamping completed_at on
 * the way into Done (see db.h).  Same CASE as task_db_task_update, so
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
void
task_db_task_set_status(TaskDatabase *db, gint64 id, TaskStatus status)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET completed_at = CASE "
            "                    WHEN ?1 = 2 AND status <> 2 THEN ?2 "
            "                    ELSE completed_at END, "
            "updated_at = ?2, status = ?1 WHERE id = ?3", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, (gint)status);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_int64(st, 3, id);
        step_done(db, st, "task set status");
    } else {
        step_done(db, NULL, "task set status");
    }
    sqlite3_finalize(st);
    if (status == TASK_STATUS_DONE)
        parent_started(db, id);
}

/* task_db_task_set_pinned() — toggle the local-only pin (see db.h).
 * Deliberately NO updated_at bump: the pin is local-only and must not
 * dirty the row for sync (a bump makes newest-wins push a no-op PATCH
 * and can starve a concurrent remote edit behind a 412).                   */
void
task_db_task_set_pinned(TaskDatabase *db, gint64 id, gboolean pinned)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET pinned = %d WHERE id = %lld",
        pinned ? 1 : 0, (long long)id);
    exec(db, sql);
    g_free(sql);
}

/* task_db_task_set_priority() — toggle the local-only high-priority flag
 * (see db.h).  Deliberately NO updated_at bump (see set_pinned above).     */
void
task_db_task_set_priority(TaskDatabase *db, gint64 id, gboolean priority)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET priority = %d WHERE id = %lld",
        priority ? 1 : 0, (long long)id);
    exec(db, sql);
    g_free(sql);
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
void
task_db_task_recur_apply(TaskDatabase *db, gint64 id, gint64 due,
                         gint due_time, gint64 next)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET due = ?1, due_time = ?5, "
            "status       = CASE WHEN status = 2 THEN 0 ELSE status END, "
            "recur_next = ?2, updated_at = ?3 WHERE id = ?4", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, due);
        sqlite3_bind_int64(st, 2, next);
        sqlite3_bind_int64(st, 3, now());
        sqlite3_bind_int64(st, 4, id);
        sqlite3_bind_int(st, 5, due_time);
        step_done(db, st, "task recur apply");
    } else {
        step_done(db, NULL, "task recur apply");
    }
    sqlite3_finalize(st);
}

/* task_db_task_recur_set_next() — store the next-occurrence stamp alone
 * (see db.h).  Deliberately NO updated_at bump, the same rule
 * set_pinned/set_priority follow: this is local bookkeeping, and seeding
 * or advancing it must not dirty the row for sync.                          */
void
task_db_task_recur_set_next(TaskDatabase *db, gint64 id, gint64 next)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET recur_next = ?1 WHERE id = ?2", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, next);
        sqlite3_bind_int64(st, 2, id);
        step_done(db, st, "task recur set next");
    } else {
        step_done(db, NULL, "task recur set next");
    }
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------------------------
 * task_db_subtask_move() — swap the display position of subtask `id` with
 * its neighbor (see db.h).
 * ------------------------------------------------------------------------- */
void
task_db_subtask_move(TaskDatabase *db, gint64 id, gint direction)
{
    Task *t = task_db_task_get(db, id);
    if (t == NULL || t->parent_id == 0) {
        task_free(t);
        return;
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
        return;
    }

    /* Swap the two entries, then write new positions 0…n in the new order.
     * updated_at is intentionally not stamped — position is local-only.    */
    gpointer tmp = g_ptr_array_index(subs, idx);
    g_ptr_array_index(subs, idx)    = g_ptr_array_index(subs, target);
    g_ptr_array_index(subs, target) = tmp;

    for (guint i = 0; i < subs->len; i++) {
        gint64 sid = ((Task *)g_ptr_array_index(subs, i))->id;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db->sq,
                "UPDATE tasks SET position = ? WHERE id = ?",
                -1, &st, NULL) != SQLITE_OK) {
            step_done(db, NULL, "subtask_move prepare");
            continue;
        }
        sqlite3_bind_int(st, 1, (int)i);
        sqlite3_bind_int64(st, 2, sid);
        step_done(db, st, "subtask_move");
        sqlite3_finalize(st);
    }
    task_ptr_array_free_tasks(subs);
}

/* ---------------------------------------------------------------------------
 * Delete hooks (see db.h).  One process-wide list; entries are never
 * removed, so no lock is needed as long as registration happens at
 * startup before any worker thread exists — which is the documented
 * contract.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskDbDeleteSqlFn fn;
    gpointer          user_data;
} DeleteHook;

static GSList *delete_hooks = NULL;  /* DeleteHook*, registration order     */

/* ---------------------------------------------------------------------------
 * task_db_add_delete_hook() — register a delete hook (see db.h).
 * ------------------------------------------------------------------------- */
void
task_db_add_delete_hook(TaskDbDeleteSqlFn fn, gpointer user_data)
{
    if (fn == NULL)
        return;
    DeleteHook *h = g_new0(DeleteHook, 1);
    h->fn        = fn;
    h->user_data = user_data;
    task_plugin_owner_stamp(h);
    delete_hooks = g_slist_append(delete_hooks, h);
}

/* ---------------------------------------------------------------------------
 * task_db_remove_delete_hooks_owner() — drop `owner`'s delete hooks
 * (see db.h).  A disabled plugin must stop contributing SQL to a delete:
 * its statements name ITS tables, and it is no longer keeping them.
 * ------------------------------------------------------------------------- */
void
task_db_remove_delete_hooks_owner(const gchar *owner)
{
    if (owner == NULL)
        return;
    GSList *n = delete_hooks;
    while (n != NULL) {
        GSList *next = n->next;
        DeleteHook *h = n->data;
        if (task_plugin_owner_is(h, owner)) {
            task_plugin_owner_forget(h);
            delete_hooks = g_slist_delete_link(delete_hooks, n);
            g_free(h);
        }
        n = next;
    }
}

/* ---------------------------------------------------------------------------
 * task_db_task_delete() — tombstone the task and its subtasks.
 *
 * Registered delete hooks contribute their statements FIRST, while the
 * row is still untouched: a hook that copies an identity out of the row
 * (the Notes mirror parks its bn_uid so the next pass cannot helpfully
 * re-create what the user just deleted) must see the row as it was.  The
 * tombstone is a soft delete, so the ordering is belt and braces — but
 * it is the ordering those hooks were written against.
 *
 * Everything runs in ONE transaction, which is the point: a suppression
 * that commits without its delete, or a delete that commits without its
 * suppression, are both worse than neither.
 * ------------------------------------------------------------------------- */
void
task_db_task_delete(TaskDatabase *db, gint64 id)
{
    GString *sql = g_string_new(NULL);

    for (GSList *l = delete_hooks; l != NULL; l = l->next) {
        DeleteHook *h = l->data;
        h->fn(db, id, sql, h->user_data);
    }

    gchar *own = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE parent_id = %lld;"
        "UPDATE tasks SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    g_string_append(sql, own);
    sqlite3_free(own);

    exec_txn(db, sql->str);
    g_string_free(sql, TRUE);
}

/* ---------------------------------------------------------------------------
 * Attachments.
 * ------------------------------------------------------------------------- */
GPtrArray *
task_db_attachments(TaskDatabase *db, gint64 task_id)
{
    GPtrArray *out = g_ptr_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
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
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO attachments(task_id, path, added_at) "
            "VALUES(?, ?, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "attachment add"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "attachment add");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_attachment_remove() — drop one attachment row (see db.h).        */
void
task_db_attachment_remove(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM attachments WHERE id = %lld", (long long)id);
    exec(db, sql);
    sqlite3_free(sql);
}

/* task_db_attachment_counts() — task_id → count map, one query (see db.h). */
GHashTable *
task_db_attachment_counts(TaskDatabase *db)
{
    GHashTable *map = g_hash_table_new(g_direct_hash, g_direct_equal);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
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
        if (sqlite3_prepare_v2(db->sq, q[i].sql, -1, &st, NULL)
                == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW)
            *q[i].out = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
}

/* ---------------------------------------------------------------------------
 * Sync state + sync-side mutators (no updated_at stamp — see db.h).
 * ------------------------------------------------------------------------- */
gchar *
task_db_state_get(TaskDatabase *db, const gchar *key)
{
    sqlite3_stmt *st = NULL;
    gchar *val = NULL;               /* the fetched value                   */
    if (sqlite3_prepare_v2(db->sq,
            "SELECT value FROM sync_state WHERE key = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            val = column_text_dup(st, 0);
    }
    sqlite3_finalize(st);
    return val;
}

/* task_db_state_set() — upsert one sync_state row (see db.h).              */
void
task_db_state_set(TaskDatabase *db, const gchar *key, const gchar *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO sync_state(key, value) VALUES(?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
        step_done(db, st, "sync state set");
    } else {
        step_done(db, NULL, "sync state set");
    }
    sqlite3_finalize(st);
}


/* task_db_list_apply_remote() — overwrite name with the remote's, stamping
 * the REMOTE updated time so the row is clean after the sync.              */
void
task_db_list_apply_remote(TaskDatabase *db, gint64 id, const gchar *name,
                          gint64 updated_at)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET name = ?, updated_at = ? WHERE id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, updated_at);
        sqlite3_bind_int64(st, 3, id);
        step_done(db, st, "list apply remote");
    } else {
        step_done(db, NULL, "list apply remote");
    }
    sqlite3_finalize(st);
}

/* task_db_task_apply_remote() — overwrite the fields a remote source
 * owns (title, notes, due, status, completed_at) WITHOUT the usual now()
 * stamp: the caller passes the remote `updated_at`, so the row lands
 * clean rather than immediately dirty again.  pinned and priority are
 * local-only and untouched.
 *
 * completed_at is the ONE field here that is MERGED rather than
 * overwritten: `MAX(completed_at, ?)`, reading the OLD row (gotcha 8).
 * The stamp only ever moves FORWARD — it answers "when was this last
 * completed?" — and a remote source reports 0 for anything it does not
 * currently consider done, so a plain assignment would let an un-tick on
 * Google erase local history Google never knew about.  A remote
 * completion NEWER than ours still wins, which is the only case where
 * the remote genuinely knows better.
 *
 * `t->status` is written VERBATIM.  A done-only source has already
 * folded its flag through task_status_apply_done against the row it
 * read, which is what stops a round trip promoting a New task.
 *
 * An integration's OWN per-task state (a remote id, an etag, a deep
 * link) is not here: it lives in that integration's side table, and it
 * writes that itself.                                                     */
void
task_db_task_apply_remote(TaskDatabase *db, const Task *t)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?, notes = ?, due = ?, status = ?, "
            "updated_at = ?, completed_at = MAX(completed_at, ?) "
            "WHERE id = ?", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, t->due);
        sqlite3_bind_int(st, 4, (gint)t->status);
        sqlite3_bind_int64(st, 5, t->updated_at);
        sqlite3_bind_int64(st, 6, t->completed_at);
        sqlite3_bind_int64(st, 7, t->id);
        step_done(db, st, "task apply remote");
    } else {
        step_done(db, NULL, "task apply remote");
    }
    sqlite3_finalize(st);
    if (t->status == TASK_STATUS_DONE)
        parent_started(db, t->id);
}

/* ---------------------------------------------------------------------------
 * task_db_task_move_list() — cross-list move (see db.h).
 * ------------------------------------------------------------------------- */
void
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
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * task_db_purge_done() — remove a list's completed tasks (see db.h).
 * ------------------------------------------------------------------------- */
void
task_db_purge_done(TaskDatabase *db, gint64 list_id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE list_id = %lld AND parent_id IN "
        "  (SELECT id FROM tasks WHERE list_id = %lld AND status = 2);"
        "DELETE FROM tasks WHERE list_id = %lld AND status = 2;",
        (long long)list_id, (long long)list_id, (long long)list_id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * task_db_task_apply_done_source() — see db.h.
 * ------------------------------------------------------------------------- */
void
task_db_task_apply_done_source(TaskDatabase *db, gint64 id,
                               const gchar *title, gboolean done, gint64 due)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET completed_at = CASE "
            "                     WHEN ?1 = 1 AND status <> 2 THEN ?2 "
            "                     ELSE completed_at END, "
            "title = ?3, due = ?4, "
            "status = CASE WHEN ?1 = 1   THEN 2 "     /* → Done            */
            "              WHEN status = 2 THEN 1 "   /* Done → In Progress*/
            "              ELSE status END, "         /* New/In Prog. stay */
            "updated_at = ?2 WHERE id = ?5", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, done ? 1 : 0);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_text(st, 3, title != NULL ? title : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, due);
        sqlite3_bind_int64(st, 5, id);
        step_done(db, st, "task apply done-source");
    } else {
        step_done(db, NULL, "task apply done-source");
    }
    sqlite3_finalize(st);
    if (done)
        parent_started(db, id);
}

/* ---------------------------------------------------------------------------
 * task_db_insert_remote_tombstone() — offline-move stub (see db.h).
 * ------------------------------------------------------------------------- */
gint64
task_db_insert_remote_tombstone(TaskDatabase *db, gint64 list_id)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO tasks(list_id, title, deleted, updated_at) "
            "VALUES(?, '', 1, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        sqlite3_bind_int64(st, 2, now());
        if (step_done(db, st, "remote tombstone insert"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "remote tombstone insert");
    }
    sqlite3_finalize(st);
    return id;
}

/* task_db_list_purge() — physically delete a list row + all its tasks'
 * rows (attachments cascade).                                              */
void
task_db_list_purge(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE list_id = %lld AND parent_id IS NOT NULL;"
        "DELETE FROM tasks WHERE list_id = %lld;"
        "DELETE FROM lists WHERE id = %lld;",
        (long long)id, (long long)id, (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* task_db_task_purge() — physically delete a task row + its subtasks.      */
void
task_db_task_purge(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE parent_id = %lld;"
        "DELETE FROM tasks WHERE id = %lld;",
        (long long)id, (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}
