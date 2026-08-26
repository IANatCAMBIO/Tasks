/* ===========================================================================
 * db.c — SQLite storage for Tasks (see db.h)
 * =========================================================================== */

#include "db.h"
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
    g_free(l->gtasks_id);
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
    g_free(t->gtasks_id);
    g_free(t->etag);
    g_free(t->web_link);
    g_free(t->glinks);
    g_free(t->assigned);
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
     * every column is declared HERE — there are no ALTER-based migrations
     * to reach the current shape, so a fresh file and an existing one have
     * identical structure.  list_groups comes first: lists.group_id
     * references it.                                                       */
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
        "  gtasks_id  TEXT,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  deleted    INTEGER NOT NULL DEFAULT 0)");
    exec(db,
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
        "  gtasks_id    TEXT,"
        "  updated_at   INTEGER NOT NULL DEFAULT 0,"
        "  deleted      INTEGER NOT NULL DEFAULT 0,"
        "  completed_at INTEGER NOT NULL DEFAULT 0,"
        "  etag         TEXT,"
        "  web_link     TEXT,"
        "  glinks       TEXT,"
        "  assigned     TEXT,"
        "  bn_uid       INTEGER NOT NULL DEFAULT 0,"
        "  bn_done      INTEGER NOT NULL DEFAULT 0,"
        "  bn_due       INTEGER NOT NULL DEFAULT 0)");
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
    exec(db,
        "CREATE TABLE IF NOT EXISTS bn_deleted ("
        "  uid INTEGER PRIMARY KEY)");  /* mirror tasks deleted in Tasks   */
    /* Both indexes sit in the schema block.  The old rule about creating
     * idx_tasks_bn_uid only AFTER the migrations existed because bn_uid
     * arrived via ALTER; it is a declared column now, so there is no
     * ordering hazard left.                                                */
    exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_list "
             "ON tasks(list_id, parent_id, position)");
    exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_bn_uid "
             "ON tasks(bn_uid)");

    sqlite3_stmt *vst = NULL;
    gint uv = 0;                     /* the file's schema version           */
    if (sqlite3_prepare_v2(sq, "PRAGMA user_version", -1, &vst, NULL)
        == SQLITE_OK && sqlite3_step(vst) == SQLITE_ROW)
        uv = sqlite3_column_int(vst, 0);
    sqlite3_finalize(vst);

    /* ---------------------------------------------------------------------
     * IF A MIGRATION IS EVER ADDED, back the file up before it runs.
     *
     * There are no migrations today — every column is declared in the
     * schema block above — so this is INERT and deliberately kept that
     * way: it is the guard the next schema change inherits for free.
     * Whoever adds that change must bump TASK_DB_SCHEMA_VERSION and add the
     * ALTERs below this block, and they get a backup automatically.
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
    exec(db, "PRAGMA user_version = 7");
    gint uv_after = -1;              /* what the file now claims            */
    vst = NULL;
    if (sqlite3_prepare_v2(sq, "PRAGMA user_version", -1, &vst, NULL)
        == SQLITE_OK && sqlite3_step(vst) == SQLITE_ROW)
        uv_after = sqlite3_column_int(vst, 0);
    sqlite3_finalize(vst);
    if (uv_after != 7)
        g_warning("db: %s still reports schema version %d after the "
                  "migration to 7 — the version stamp did not stick "
                  "(migrations will re-run harmlessly on every launch)",
                  path, uv_after);
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
    l->gtasks_id  = column_text_dup(st, 3);
    l->updated_at = sqlite3_column_int64(st, 4);
    l->deleted    = sqlite3_column_int(st, 5) != 0;
    l->emoji      = column_text_dup(st, 6);
    l->group_id   = sqlite3_column_int64(st, 7);
    if (l->name == NULL)
        l->name = g_strdup("");
    if (l->emoji == NULL)
        l->emoji = g_strdup("");
    return l;
}

/* The shared column list for read_list().                                  */
#define LIST_COLS "id, name, position, gtasks_id, updated_at, deleted, " \
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

/* task_db_groups() — all groups, ordered by position then name.            */
GPtrArray *
task_db_groups(TaskDatabase *db)
{
    GPtrArray *out = g_ptr_array_new();
    const gchar *sql =
        "SELECT id, name, position FROM list_groups "
        "ORDER BY position, lower(name)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) {
            TaskGroup *g = g_new0(TaskGroup, 1);
            g->id       = sqlite3_column_int64(st, 0);
            g->name     = column_text_dup(st, 1);
            if (g->name == NULL) g->name = g_strdup("");
            g->position = sqlite3_column_int(st, 2);
            g_ptr_array_add(out, g);
        }
    sqlite3_finalize(st);
    return out;
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
task_db_list_emoji_if_empty(TaskDatabase *db, const gchar *gtasks_id,
                            const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET emoji = ?1 WHERE gtasks_id = ?2 AND "
            "emoji = '' AND deleted = 0", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, emoji, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, gtasks_id, -1, SQLITE_TRANSIENT);
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
                  "status, pinned, position, gtasks_id, updated_at, deleted, "\
                  "completed_at, etag, web_link, glinks, assigned, priority, "\
                  "bn_uid, bn_done, bn_due"

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
    t->gtasks_id    = column_text_dup(st, 9);
    t->updated_at   = sqlite3_column_int64(st, 10);
    t->deleted      = sqlite3_column_int(st, 11) != 0;
    t->completed_at = sqlite3_column_int64(st, 12);
    t->etag         = column_text_dup(st, 13);
    t->web_link     = column_text_dup(st, 14);
    t->glinks       = column_text_dup(st, 15);
    t->assigned     = column_text_dup(st, 16);
    t->priority     = sqlite3_column_int(st, 17) != 0;
    t->bn_uid       = sqlite3_column_int64(st, 18);
    t->bn_done      = sqlite3_column_int(st, 19) != 0;
    t->bn_due       = sqlite3_column_int64(st, 20);
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

/* task_db_tasks_due_between() — the Due Today / Weekly Forecast views.     */
GPtrArray *
task_db_tasks_due_between(TaskDatabase *db, gint64 lo, gint64 hi)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
        "deleted = 0 ORDER BY priority DESC, due, list_id, position",
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
    /* completed_at follows the DONE status: stamped when the row enters
     * it (the CASE reads the OLD row values, gotcha 8), cleared when it
     * leaves.  An already-done task keeps its original stamp, so a New →
     * Done → In Progress → Done round trip re-stamps only on the way
     * back in.  updated_at is stamped unconditionally here — this path
     * also writes title/notes/due, which Google does want.                 */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?1, notes = ?2, due = ?3, "
            "completed_at = CASE WHEN ?4 = 2 AND status <> 2 THEN ?6 "
            "                    WHEN ?4 <> 2 THEN 0 "
            "                    ELSE completed_at END, "
            "status = ?4, pinned = ?5, updated_at = ?6, priority = ?8 "
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
        step_done(db, st, "task update");
    } else {
        step_done(db, NULL, "task update");
    }
    sqlite3_finalize(st);
    if (t->status == TASK_STATUS_DONE)
        parent_started(db, t->id);
}


/* ---------------------------------------------------------------------------
 * task_db_task_set_status() — write the status, stamping/clearing
 * completed_at (see db.h).  Same completed_at CASE as task_db_task_update,
 * so both paths agree: an already-done task keeps its original stamp.
 * It reads the OLD row (gotcha 8).
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
            "                    WHEN ?1 <> 2 THEN 0 "
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
 * task_db_task_delete() — tombstone the task and its subtasks.
 *
 * A mirror task also records its bn_uid in bn_deleted, in the SAME
 * transaction: Notes has no CLI verb that deletes an action item, so
 * the item survives there, and without this the very next mirror pass
 * would see a uid with no task and helpfully re-create the row the user
 * just deleted.  task_bnsync clears the suppression once the item leaves
 * Notes for real.  Subtasks never carry a uid (Notes has no
 * subtasks), so only the task's own row is consulted.
 * ------------------------------------------------------------------------- */
void
task_db_task_delete(TaskDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "INSERT OR IGNORE INTO bn_deleted (uid) "
        "  SELECT bn_uid FROM tasks WHERE id = %lld AND bn_uid > 0;"
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE parent_id = %lld;"
        "UPDATE tasks SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)id,
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
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

/* set_gtasks_id() — shared body of the two id setters.                     */
static void
set_gtasks_id(TaskDatabase *db, const gchar *table, gint64 id,
              const gchar *gid)
{
    gchar *sql = g_strdup_printf(
        "UPDATE %s SET gtasks_id = ? WHERE id = %lld",
        table, (long long)id);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, gid, -1, SQLITE_TRANSIENT);
        step_done(db, st, "gtasks id set");
    } else {
        step_done(db, NULL, "gtasks id set");
    }
    sqlite3_finalize(st);
    g_free(sql);
}

/* task_db_list_set_gtasks_id() — bind a list to its Google id WITHOUT
 * stamping updated_at (see db.h).                                          */
void
task_db_list_set_gtasks_id(TaskDatabase *db, gint64 id, const gchar *gid)
{
    set_gtasks_id(db, "lists", id, gid);
}

/* task_db_task_set_gtasks_id() — task variant of the above (see db.h).     */
void
task_db_task_set_gtasks_id(TaskDatabase *db, gint64 id, const gchar *gid)
{
    set_gtasks_id(db, "tasks", id, gid);
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

/* task_db_task_apply_remote() — overwrite the synced fields (title, notes,
 * due, status) plus the Google-mirror metadata (completed_at, etag,
 * web_link, glinks, assigned) from remote data; pinned and priority are
 * local-only and untouched.  `t->status` is written VERBATIM: Google
 * only ever reports done-ness, so the caller has already folded that
 * through task_status_apply_done against the row it read.                  */
void
task_db_task_apply_remote(TaskDatabase *db, const Task *t)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?, notes = ?, due = ?, status = ?, "
            "updated_at = ?, completed_at = ?, etag = ?, web_link = ?, "
            "glinks = ?, assigned = ? WHERE id = ?", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, t->due);
        sqlite3_bind_int(st, 4, (gint)t->status);
        sqlite3_bind_int64(st, 5, t->updated_at);
        sqlite3_bind_int64(st, 6, t->completed_at);
        sqlite3_bind_text(st, 7, t->etag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, t->web_link, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, t->glinks, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, t->assigned, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 11, t->id);
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

/* task_db_tasks_bn_mirror() — every visible mirror task (see db.h).        */
GPtrArray *
task_db_tasks_bn_mirror(TaskDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE bn_uid > 0 AND deleted = 0 "
        "ORDER BY priority DESC, list_id, position, id", 0, 0, 0);
}

/* task_db_task_by_bn_uid() — the visible mirror task for `uid` (see db.h). */
Task *
task_db_task_by_bn_uid(TaskDatabase *db, gint64 uid)
{
    GPtrArray *a = task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE bn_uid = ? AND deleted = 0 "
        "LIMIT 1", 1, uid, 0);
    Task *t = a->len > 0 ? g_ptr_array_index(a, 0) : NULL;
    g_ptr_array_free(a, TRUE);
    return t;
}

/* ---------------------------------------------------------------------------
 * task_db_task_set_bn() — bind a task to a Notes item and record the
 * push baseline (see db.h).  NO updated_at bump: the binding is local
 * bookkeeping, and dirtying the row here would buy a no-op Google PATCH
 * on every mirror pass (the same reasoning as set_pinned).
 * ------------------------------------------------------------------------- */
void
task_db_task_set_bn(TaskDatabase *db, gint64 id, gint64 uid, gboolean done,
                    gint64 due)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET bn_uid = %lld, bn_done = %d, bn_due = %lld "
        "WHERE id = %lld", (long long)uid, done ? 1 : 0, (long long)due,
        (long long)id);
    exec(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * task_db_task_apply_notes() — overwrite the Notes-owned fields and
 * re-baseline in ONE statement (see db.h).  updated_at IS stamped: the
 * change came from outside Tasks and has to reach Google too.  The
 * completed_at CASE repeats set_status's transition rule, which relies
 * on SET expressions reading the OLD row (gotcha 8).
 *
 * Notes has no third state, so its binary `done` reaches tasks.status
 * through task_status_apply_done's rule, spelled here as a CASE for the
 * same reason: it needs the status the row already held, and reading it
 * back in C would be a second statement racing the first.
 *
 * The baselines are passed SEPARATELY from the applied values because
 * the two diverge on a failed push: the task keeps the user's local
 * done/due, while bn_done/bn_due must stay at what Notes still holds
 * so the delta is retried instead of being silently swallowed.
 * ------------------------------------------------------------------------- */
void
task_db_task_apply_notes(TaskDatabase *db, gint64 id, const gchar *title,
                         gboolean done, gint64 due, gboolean bn_done,
                         gint64 bn_due)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET completed_at = CASE "
            "                     WHEN ?1 = 1 AND status <> 2 THEN ?2 "
            "                     WHEN ?1 = 0 THEN 0 "
            "                     ELSE completed_at END, "
            "title = ?3, due = ?4, bn_done = ?5, bn_due = ?6, "
            "status = CASE WHEN ?1 = 1   THEN 2 "     /* → Done            */
            "              WHEN status = 2 THEN 1 "   /* Done → In Progress*/
            "              ELSE status END, "         /* New/In Prog. stay */
            "updated_at = ?2 WHERE id = ?7", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, done ? 1 : 0);
        sqlite3_bind_int64(st, 2, now());
        sqlite3_bind_text(st, 3, title != NULL ? title : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, due);
        sqlite3_bind_int(st, 5, bn_done ? 1 : 0);
        sqlite3_bind_int64(st, 6, bn_due);
        sqlite3_bind_int64(st, 7, id);
        step_done(db, st, "task apply notes");
    } else {
        step_done(db, NULL, "task apply notes");
    }
    sqlite3_finalize(st);
    if (done)
        parent_started(db, id);
}

/* task_db_bn_deleted() — the suppressed-uid set (see db.h).  Keys are the
 * packed uids; the table owns nothing.                                     */
GHashTable *
task_db_bn_deleted(TaskDatabase *db)
{
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, "SELECT uid FROM bn_deleted", -1,
                           &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            g_hash_table_add(set,
                GSIZE_TO_POINTER(sqlite3_column_int64(st, 0)));
    } else {
        step_done(db, NULL, "bn deleted query");
    }
    sqlite3_finalize(st);
    return set;
}

/* task_db_bn_deleted_forget() — drop one suppression (see db.h).           */
void
task_db_bn_deleted_forget(TaskDatabase *db, gint64 uid)
{
    gchar *sql = sqlite3_mprintf("DELETE FROM bn_deleted WHERE uid = %lld",
                                 (long long)uid);
    exec(db, sql);
    sqlite3_free(sql);
}

/* task_db_tasks_clear_gtasks_ids() — unbind one list's tasks (see db.h).   */
void
task_db_tasks_clear_gtasks_ids(TaskDatabase *db, gint64 list_id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET gtasks_id = NULL, etag = NULL "
        "WHERE list_id = %lld", (long long)list_id);
    exec(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * task_db_insert_remote_tombstone() — offline-move stub (see db.h).
 * ------------------------------------------------------------------------- */
void
task_db_insert_remote_tombstone(TaskDatabase *db, gint64 list_id,
                                const gchar *gtasks_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO tasks(list_id, title, deleted, gtasks_id, "
            "updated_at) VALUES(?, '', 1, ?, ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        sqlite3_bind_text(st, 2, gtasks_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        step_done(db, st, "remote tombstone insert");
    } else {
        step_done(db, NULL, "remote tombstone insert");
    }
    sqlite3_finalize(st);
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
