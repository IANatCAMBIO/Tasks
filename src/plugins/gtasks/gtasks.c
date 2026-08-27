/* ===========================================================================
 * gtasks.c — two-way Google Tasks sync (see gtasks.h)
 * =========================================================================== */

#include "gtasks.h"
#include "plugin_ctx.h"
#include <curl/curl.h>
#include "oauth.h"
#include "http.h"
#include "json.h"
#include "task_ops.h"                /* the hooks the remote half rides on  */
#include "task_worker.h"
#include "task_ui.h"
#include "settings_window.h"             /* the shared periodic-pass scheduler  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTASKS_API "https://tasks.googleapis.com/tasks/v1"

/* The host table and this plugin's identity.  Defined here, declared in
 * plugin_ctx.h for the module's other translation units.                 */
const TaskHostApi *host;
const TaskPlugin  *self;

/* This sync's own in-flight guard and periodic timer.  They were fields
 * on TaskApp while this was compiled into the app; a plugin owns its own
 * — the scheduler is handed pointers to them (see task_worker.h), so the
 * host needs no field per integration.                                   */
static gboolean gt_running;
static guint    gt_timer;

/* ---------------------------------------------------------------------------
 * gt_status() / gt_notice() — printf-style wrappers over the host's
 * plain-string message calls.
 *
 * The host table deliberately takes a FINISHED string, so a plugin can
 * never hand user-supplied text to a format parser across the ABI
 * boundary.  These keep that property while letting the call sites read
 * the way they always did.
 * ------------------------------------------------------------------------- */
static void gt_status(TaskApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
static void
gt_status(TaskApp *app, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    host->notify->status(app, msg);
    g_free(msg);
}

static void gt_notice(GtkWindow *parent, GtkMessageType type,
                      const gchar *title, const gchar *fmt, ...)
                      G_GNUC_PRINTF(4, 5);
static void
gt_notice(GtkWindow *parent, GtkMessageType type, const gchar *title,
          const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    host->ui->notice(parent, type, title, msg);
    g_free(msg);
}


/* ===========================================================================
 * The side tables (schema v8).
 *
 * A task's Google identity — its remote id, its etag, the deep link and
 * the raw links[]/assignmentInfo blobs — lives in gtasks_task keyed by
 * task id, and a list's in gtasks_list.  None of it is on the core rows
 * any more: a task carries only what a task is, and this integration
 * carries what it knows about that task.
 *
 * These are per-row queries rather than a batched map, deliberately.
 * Every caller here is on the sync WORKER and is already making an HTTP
 * request per row, so one indexed primary-key lookup beside it is
 * nothing.  The batch rule in plugin.h is about the UI's draw path,
 * which none of this is on.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * Reading and writing the side tables.
 *
 * Every statement goes through the HOST (host->db->exec / exec_query).  A
 * plugin must never link SQLite itself: it is handed a live sqlite3*
 * belonging to the host, and a second copy of the library operating on
 * that handle is undefined behaviour — the one failure mode plugin.h
 * calls out as worth being paranoid about, given this database's
 * history.  So there is no sqlite3_* call anywhere below, and none in
 * this plugin at all.
 *
 * String values are quoted with host->db->quote (sqlite3_mprintf's %Q,
 * re-homed onto g_free), so nothing here hand-rolls escaping.  Integers
 * are formatted directly — they cannot carry an injection.
 * ------------------------------------------------------------------------- */

/* collect_text() — exec_query callback taking the first column of the
 * first row as a newly-allocated string.  Returns non-zero to stop after
 * that row.                                                              */
static gint
collect_text(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    gchar **out = data;
    if (n_cols > 0 && values[0] != NULL)
        *out = g_strdup(values[0]);
    return 1;                        /* one row is all we want            */
}

/* gt_text() — one TEXT column of a side row, or NULL.  New string.       */
static gchar *
gt_text(TaskDatabase *db, const gchar *table, const gchar *key_col,
        const gchar *col, gint64 id)
{
    gchar *sql = g_strdup_printf("SELECT %s FROM %s WHERE %s = %" G_GINT64_FORMAT,
                                 col, table, key_col, id);
    gchar *out = NULL;
    host->db->exec_query(db, sql, collect_text, &out);
    g_free(sql);
    return out;
}

/* gt_list_by_gid() — the local list bound to a Google tasklist id, or 0.
 * The reverse of gt_list_gid, for the places Google names a list.        */
static gint64
gt_list_by_gid(TaskDatabase *db, const gchar *gid)
{
    if (gid == NULL)
        return 0;
    gchar *q   = host->db->quote(gid);
    gchar *sql = g_strdup_printf(
        "SELECT list_id FROM gtasks_list WHERE gtasks_id = %s", q);
    gchar *got = NULL;
    host->db->exec_query(db, sql, collect_text, &got);
    gint64 id = got != NULL ? g_ascii_strtoll(got, NULL, 10) : 0;
    g_free(got);
    g_free(sql);
    g_free(q);
    return id;
}

static gchar *
gt_list_gid(TaskDatabase *db, gint64 list_id)
{
    return gt_text(db, "gtasks_list", "list_id", "gtasks_id", list_id);
}

static gchar *
gt_task_gid(TaskDatabase *db, gint64 task_id)
{
    return gt_text(db, "gtasks_task", "task_id", "gtasks_id", task_id);
}

static gchar *
gt_task_etag(TaskDatabase *db, gint64 task_id)
{
    return gt_text(db, "gtasks_task", "task_id", "etag", task_id);
}

/* gt_list_set_gid() — bind (or unbind, with NULL) a list.                */
static void
gt_list_set_gid(TaskDatabase *db, gint64 list_id, const gchar *gid)
{
    gchar *sql;
    if (gid != NULL) {
        gchar *q = host->db->quote(gid);
        sql = g_strdup_printf(
            "INSERT INTO gtasks_list (list_id, gtasks_id)"
            " VALUES (%" G_GINT64_FORMAT ", %s)"
            " ON CONFLICT(list_id) DO UPDATE SET gtasks_id = %s",
            list_id, q, q);
        g_free(q);
    } else {
        sql = g_strdup_printf(
            "DELETE FROM gtasks_list WHERE list_id = %" G_GINT64_FORMAT,
            list_id);
    }
    host->db->exec(db, sql);
    g_free(sql);
}

/* gt_task_set() — write a task's whole Google identity.  A NULL `gid`
 * DELETES the row: a task with no remote id has nothing to remember, and
 * leaving a stale etag behind would guard a push that no longer applies. */
static void
gt_task_set(TaskDatabase *db, gint64 task_id, const gchar *gid,
            const gchar *etag, const gchar *web_link,
            const gchar *glinks, const gchar *assigned)
{
    gchar *sql;
    if (gid != NULL) {
        gchar *qg = host->db->quote(gid);
        gchar *qe = host->db->quote(etag);
        gchar *qw = host->db->quote(web_link);
        gchar *ql = host->db->quote(glinks);
        gchar *qa = host->db->quote(assigned);
        sql = g_strdup_printf(
            "INSERT INTO gtasks_task"
            "  (task_id, gtasks_id, etag, web_link, glinks, assigned)"
            "  VALUES (%" G_GINT64_FORMAT ", %s, %s, %s, %s, %s)"
            "  ON CONFLICT(task_id) DO UPDATE SET"
            "    gtasks_id = excluded.gtasks_id,"
            "    etag      = excluded.etag,"
            "    web_link  = excluded.web_link,"
            "    glinks    = excluded.glinks,"
            "    assigned  = excluded.assigned",
            task_id, qg, qe, qw, ql, qa);
        g_free(qg); g_free(qe); g_free(qw); g_free(ql); g_free(qa);
    } else {
        sql = g_strdup_printf(
            "DELETE FROM gtasks_task WHERE task_id = %" G_GINT64_FORMAT,
            task_id);
    }
    host->db->exec(db, sql);
    g_free(sql);
}

/* gt_task_set_gid() — bind the id alone, leaving any etag/link intact.   */
static void
gt_task_set_gid(TaskDatabase *db, gint64 task_id, const gchar *gid)
{
    if (gid == NULL) {
        gt_task_set(db, task_id, NULL, NULL, NULL, NULL, NULL);
        return;
    }
    gchar *q = host->db->quote(gid);
    gchar *sql = g_strdup_printf(
        "INSERT INTO gtasks_task (task_id, gtasks_id)"
        " VALUES (%" G_GINT64_FORMAT ", %s)"
        " ON CONFLICT(task_id) DO UPDATE SET gtasks_id = %s",
        task_id, q, q);
    host->db->exec(db, sql);
    g_free(sql);
    g_free(q);
}

/* ---------------------------------------------------------------------------
 * gt_gids_for_list() — every bound task of one list, as task id → gid.
 *
 * ONE query for the whole list rather than a lookup per task, because
 * this one IS on a hot path: the match passes below consult it for every
 * task on both sides.  The map owns its strings, which is what lets them
 * double as keys in the gid → Task index the caller builds.
 * ------------------------------------------------------------------------- */
static gint
collect_gid_row(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    GHashTable *m = data;
    if (n_cols >= 2 && values[0] != NULL && values[1] != NULL) {
        gint64 *k = g_new(gint64, 1);
        *k = g_ascii_strtoll(values[0], NULL, 10);
        g_hash_table_insert(m, k, g_strdup(values[1]));
    }
    return 0;                        /* keep going                        */
}

static GHashTable *
gt_gids_for_list(TaskDatabase *db, gint64 list_id)
{
    GHashTable *m = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                          g_free, g_free);
    gchar *sql = g_strdup_printf(
        "SELECT g.task_id, g.gtasks_id FROM gtasks_task g"
        "  JOIN tasks t ON t.id = g.task_id"
        " WHERE t.list_id = %" G_GINT64_FORMAT
        "   AND g.gtasks_id IS NOT NULL", list_id);
    host->db->exec_query(db, sql, collect_gid_row, m);
    g_free(sql);
    return m;
}

/* gid_of() — a task's Google id from that map, or NULL.                   */
static const gchar *
gid_of(GHashTable *m, gint64 task_id)
{
    return g_hash_table_lookup(m, &task_id);
}

/* gid_put() — record a binding just made, so later passes see it.  NULL
 * removes.  The map owns the copy.                                        */
static void
gid_put(GHashTable *m, gint64 task_id, const gchar *gid)
{
    if (gid == NULL) {
        g_hash_table_remove(m, &task_id);
        return;
    }
    gint64 *k = g_new(gint64, 1);
    *k = task_id;
    g_hash_table_insert(m, k, g_strdup(gid));
}

/* gt_list_forget_tasks() — drop every task binding of one list, so the
 * task pass re-pushes them all as new (the bound remote list vanished). */
static void
gt_list_forget_tasks(TaskDatabase *db, gint64 list_id)
{
    gchar *sql = g_strdup_printf(
        "DELETE FROM gtasks_task WHERE task_id IN"
        "  (SELECT id FROM tasks WHERE list_id = %" G_GINT64_FORMAT ")",
        list_id);
    host->db->exec(db, sql);
    g_free(sql);
}

/* ---------------------------------------------------------------------------
 * gt_remote_tombstone() — a bare tombstone carrying a Google id.
 *
 * The offline half of a cross-list move: the moved row starts a NEW
 * remote task in the destination, while this stub deletes the old remote
 * copy on the next pass.  The tombstone is a core row; the id it carries
 * is ours, so it goes in the side table.
 * ------------------------------------------------------------------------- */
static void
gt_remote_tombstone(TaskDatabase *db, gint64 list_id, const gchar *gid)
{
    if (gid == NULL)
        return;
    gint64 id = host->db->insert_remote_tombstone(db, list_id);
    if (id != 0)
        gt_task_set_gid(db, id, gid);
}

static void post_status(TaskApp *app, const gchar *msg);

/* ---------------------------------------------------------------------------
 * Remote snapshots — the fields of a Google tasklist / task this sync
 * uses.  Strings are owned.
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar    *gid;                   /* Google tasklist id                  */
    gchar    *title;
    gint64    updated;               /* unix                                */
    gboolean  matched;               /* consumed by the match pass          */
} RemoteList;

typedef struct {
    gchar    *gid;                   /* Google task id                      */
    gchar    *title;
    gchar    *notes;
    gchar    *parent_gid;            /* Google id of the parent, or NULL    */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gint64    updated;               /* unix                                */
    gint64    completed;             /* unix completion time; 0 = none      */
    gchar    *etag;                  /* concurrency tag                     */
    gchar    *web_link;              /* Google Tasks UI deep link           */
    gchar    *glinks;                /* links[] as raw JSON, or NULL        */
    gchar    *assigned;              /* assignmentInfo as raw JSON, / NULL  */
    gboolean  done;
    gboolean  deleted;
    gboolean  hidden;                /* completed + cleared on Google       */
    gboolean  matched;
} RemoteTask;

/* remote_list_free() / remote_task_free() — free one snapshot row
 * (gpointer-typed so fetch_paginated can free either kind).                */
static void
remote_list_free(gpointer data)
{
    RemoteList *r = data;
    g_free(r->gid);
    g_free(r->title);
    g_free(r);
}

static void
remote_task_free(gpointer data)
{
    RemoteTask *r = data;
    g_free(r->gid);
    g_free(r->title);
    g_free(r->notes);
    g_free(r->parent_gid);
    g_free(r->etag);
    g_free(r->web_link);
    g_free(r->glinks);
    g_free(r->assigned);
    g_free(r);
}

/* Counters for the end-of-sync status line.                                */
typedef struct {
    gint pushed;                     /* local → Google writes               */
    gint pulled;                     /* Google → local writes               */
    gint deleted;                    /* rows removed on either side         */
} SyncStats;

/* escaped() — shorthand for the URI-escaping every URL builder needs.      */
static gchar *
escaped(const gchar *s)
{
    return g_uri_escape_string(s, NULL, FALSE);
}

/* tasklist_url() — "…/users/@me/lists[/<gid>]".                            */
static gchar *
tasklist_url(const gchar *gid)
{
    if (gid == NULL)
        return g_strdup(GTASKS_API "/users/@me/lists");
    gchar *gid_esc = escaped(gid);
    gchar *url = g_strdup_printf(GTASKS_API "/users/@me/lists/%s",
                                 gid_esc);
    g_free(gid_esc);
    return url;
}

/* task_url() — "…/lists/<list>/tasks[/<task>]".                            */
static gchar *
task_url(const gchar *list_gid, const gchar *task_gid)
{
    gchar *list_esc = escaped(list_gid);
    gchar *url;
    if (task_gid == NULL) {
        url = g_strdup_printf(GTASKS_API "/lists/%s/tasks", list_esc);
    } else {
        gchar *task_esc = escaped(task_gid);
        url = g_strdup_printf(GTASKS_API "/lists/%s/tasks/%s",
                              list_esc, task_esc);
        g_free(task_esc);
    }
    g_free(list_esc);
    return url;
}

/* ---------------------------------------------------------------------------
 * Time conversions.
 * ------------------------------------------------------------------------- */

/* rfc3339_to_unix() — parse a Google `updated` timestamp.  0 on failure.   */
static gint64
rfc3339_to_unix(const gchar *s)
{
    if (s == NULL)
        return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/* due_from_rfc3339() — a Google `due` carries only the DATE (the time
 * portion is documented as ignored, always midnight UTC); map that
 * calendar date to LOCAL midnight, the local representation.               */
static gint64
due_from_rfc3339(const gchar *s)
{
    gint y = 0, m = 0, d = 0;        /* the date portion                    */
    if (s == NULL || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
        return 0;
    return host->util->due_from_ymd(y, m, d);
}

/* due_to_rfc3339() — local midnight unix → "YYYY-MM-DDT00:00:00.000Z"
 * (the calendar date in LOCAL time — matching due_from_rfc3339).
 * Returns NULL for due == 0 (caller emits JSON null to clear the date).    */
static gchar *
due_to_rfc3339(gint64 due)
{
    if (due == 0)
        return NULL;
    gchar *date = host->util->due_format_iso(due);
    gchar *s = g_strdup_printf("%sT00:00:00.000Z", date);
    g_free(date);
    return s;
}

/* unix_to_rfc3339() — full UTC timestamp (unlike `due`, the `completed`
 * and `updatedMin` fields carry real times).  NULL for 0.                  */
static gchar *
unix_to_rfc3339(gint64 unix_time)
{
    if (unix_time == 0)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_utc(unix_time);
    gchar *s = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S.000Z");
    g_date_time_unref(dt);
    return s;
}

/* json_subtree_dup() — one member's raw JSON text (task_json_write), or
 * NULL when absent.                                                        */
static gchar *
json_subtree_dup(TaskJson *obj, const gchar *key)
{
    TaskJson *v = task_json_get(obj, key);
    if (v == NULL)
        return NULL;
    GString *s = g_string_new(NULL);
    task_json_write(s, v);
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------
 * api_call() — one authorized API request; parses the JSON reply.
 *   if_match — an etag for optimistic concurrency ("If-Match"), or NULL.
 *   body_out — optionally receives the parsed reply (caller frees); pass
 *              NULL when only success/failure matters (DELETE).
 * Returns TRUE on 2xx.  On failure *err is set (g_free).
 * ------------------------------------------------------------------------- */
static gboolean
api_call(const gchar *method, const gchar *url, const gchar *token,
         const gchar *if_match, const gchar *body, TaskJson **body_out,
         gchar **err)
{
    if (body_out != NULL)
        *body_out = NULL;
    gchar *match_hdr = if_match != NULL
        ? g_strdup_printf("If-Match: %s", if_match) : NULL;
    glong status = 0;
    gchar *terr = NULL;              /* transport error                     */
    gchar *resp = task_http_request(method, url, token, "application/json",
                                    match_hdr, body, &status, &terr);
    g_free(match_hdr);
    if (resp == NULL) {
        *err = g_strdup_printf("%s %s: %s", method, url,
                               terr != NULL ? terr : "network failure");
        g_free(terr);
        return FALSE;
    }
    g_free(terr);
    if (status < 200 || status > 299) {
        /* Try to surface Google's error message.                           */
        TaskJson *root = task_json_parse(resp, -1);
        const gchar *msg = task_json_str(task_json_get(root, "error"),
                                         "message");
        *err = g_strdup_printf("%s failed (HTTP %ld)%s%s", method, status,
                               msg != NULL ? ": " : "",
                               msg != NULL ? msg : "");
        task_json_free(root);
        g_free(resp);
        return FALSE;
    }
    if (body_out != NULL && *resp != '\0')
        *body_out = task_json_parse(resp, -1);
    g_free(resp);
    return TRUE;
}

/* api_call_delete() — DELETE variant where a 404 or 410 counts as
 * success (the row is gone either way — e.g. deleted from the Google
 * side too).                                                               */
static gboolean
api_call_delete(const gchar *url, const gchar *token, gchar **err)
{
    gchar *derr = NULL;
    if (api_call("DELETE", url, token, NULL, NULL, NULL, &derr))
        return TRUE;
    gboolean gone = strstr(derr, "HTTP 404") != NULL ||
                    strstr(derr, "HTTP 410") != NULL;
    if (gone) {
        g_free(derr);
        return TRUE;
    }
    *err = derr;
    return FALSE;
}

/* str_dup_or_empty() — dup a string member, "" when absent.                */
static gchar *
str_dup_or_empty(TaskJson *obj, const gchar *key)
{
    const gchar *v = task_json_str(obj, key);
    return g_strdup(v != NULL ? v : "");
}

/* ---------------------------------------------------------------------------
 * fetch_paginated() — run one paginated GET: `base_url` must already
 * carry a query string (a pageToken is appended with '&'); every items[]
 * entry that has an "id" goes through `add_item`, which appends a
 * snapshot row to `out`.  On failure the collected rows are freed with
 * `free_row` and NULL is returned (+ *err).
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_paginated(const gchar *base_url, const gchar *token,
                void (*add_item)(TaskJson *it, GPtrArray *out),
                void (*free_row)(gpointer row), gchar **err)
{
    GPtrArray *out = g_ptr_array_new();
    gchar *page = NULL;              /* nextPageToken                       */
    do {
        gchar *page_esc = page != NULL ? escaped(page) : NULL;
        gchar *url = g_strdup_printf("%s%s%s", base_url,
            page_esc != NULL ? "&pageToken=" : "",
            page_esc != NULL ? page_esc : "");
        g_free(page_esc);
        g_free(page);
        page = NULL;

        TaskJson *root = NULL;
        gboolean ok = api_call("GET", url, token, NULL, NULL, &root, err);
        g_free(url);
        if (!ok) {
            for (guint i = 0; i < out->len; i++)
                free_row(g_ptr_array_index(out, i));
            g_ptr_array_free(out, TRUE);
            return NULL;
        }
        TaskJson *items = task_json_get(root, "items");
        for (guint i = 0; i < task_json_len(items); i++) {
            TaskJson *it = task_json_at(items, i);
            if (task_json_str(it, "id") != NULL)
                add_item(it, out);
        }
        page = g_strdup(task_json_str(root, "nextPageToken"));
        task_json_free(root);
    } while (page != NULL);
    return out;
}

/* add_remote_list() — one tasklist item → RemoteList snapshot row.         */
static void
add_remote_list(TaskJson *it, GPtrArray *out)
{
    RemoteList *r = g_new0(RemoteList, 1);
    r->gid     = g_strdup(task_json_str(it, "id"));
    r->title   = str_dup_or_empty(it, "title");
    r->updated = rfc3339_to_unix(task_json_str(it, "updated"));
    g_ptr_array_add(out, r);
}

/* add_remote_task() — one task item → RemoteTask snapshot row.             */
static void
add_remote_task(TaskJson *it, GPtrArray *out)
{
    RemoteTask *r = g_new0(RemoteTask, 1);
    r->gid        = g_strdup(task_json_str(it, "id"));
    r->title      = str_dup_or_empty(it, "title");
    r->notes      = str_dup_or_empty(it, "notes");
    r->parent_gid = g_strdup(task_json_str(it, "parent"));
    r->due        = due_from_rfc3339(task_json_str(it, "due"));
    r->updated    = rfc3339_to_unix(task_json_str(it, "updated"));
    r->completed  = rfc3339_to_unix(task_json_str(it, "completed"));
    r->etag       = g_strdup(task_json_str(it, "etag"));
    r->web_link   = g_strdup(task_json_str(it, "webViewLink"));
    r->glinks     = json_subtree_dup(it, "links");
    r->assigned   = json_subtree_dup(it, "assignmentInfo");
    r->done       = g_strcmp0(task_json_str(it, "status"),
                              "completed") == 0;
    r->deleted    = task_json_bool(it, "deleted", FALSE);
    r->hidden     = task_json_bool(it, "hidden", FALSE);
    g_ptr_array_add(out, r);
}

/* ---------------------------------------------------------------------------
 * fetch_remote_lists() — GET all tasklists (paginated).  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_lists(const gchar *token, gchar **err)
{
    return fetch_paginated(
        GTASKS_API "/users/@me/lists?maxResults=100",
        token, add_remote_list, remote_list_free, err);
}

/* ---------------------------------------------------------------------------
 * fetch_remote_tasks() — GET the tasks of one tasklist, including
 * completed, hidden and (tombstoned) deleted ones.  When `updated_min`
 * is non-zero only items CHANGED since then come back (incremental
 * sync) — an item missing from such a listing means "unchanged", never
 * "deleted" (deletions arrive as deleted:true items).  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_tasks(const gchar *token, const gchar *list_gid,
                   gint64 updated_min, gchar **err)
{
    gchar *min_param = g_strdup("");
    if (updated_min > 0) {
        gchar *stamp = unix_to_rfc3339(updated_min);
        gchar *stamp_esc = escaped(stamp);
        g_free(min_param);
        min_param = g_strdup_printf("&updatedMin=%s", stamp_esc);
        g_free(stamp_esc);
        g_free(stamp);
    }
    gchar *gid_esc = escaped(list_gid);
    gchar *base_url = g_strdup_printf(
        GTASKS_API "/lists/%s/tasks?maxResults=100"
        "&showCompleted=true&showHidden=true&showDeleted=true%s",
        gid_esc, min_param);
    g_free(gid_esc);
    g_free(min_param);
    GPtrArray *out = fetch_paginated(base_url, token, add_remote_task,
                                     remote_task_free, err);
    g_free(base_url);
    return out;
}

/* ---------------------------------------------------------------------------
 * task_body() — the JSON body pushing a local task's synced fields.
 * `due: null` explicitly clears a remote date on PATCH.
 * ------------------------------------------------------------------------- */
static gchar *
task_body(const Task *t)
{
    GString *s = g_string_new("{\"title\": ");
    task_json_escape(s, t->title);
    g_string_append(s, ", \"notes\": ");
    task_json_escape(s, t->notes);
    /* Google's own "status" is BINARY (completed / needsAction), so our
     * tri-state flattens onto it: New and In Progress both push
     * needsAction, and the distinction between them stays local — the
     * API has no third value and no field to smuggle one in.               */
    g_string_append(s, ", \"status\": ");
    task_json_escape(s, t->status == TASK_STATUS_DONE ? "completed"
                                                  : "needsAction");
    gchar *due = due_to_rfc3339(t->due);
    g_string_append(s, ", \"due\": ");
    task_json_escape(s, due);         /* NULL → the literal `null`          */
    g_free(due);
    /* Preserve the real completion time across the sync (Google would
     * otherwise stamp "now" when status flips to completed).               */
    if (t->status == TASK_STATUS_DONE && t->completed_at != 0) {
        gchar *completed = unix_to_rfc3339(t->completed_at);
        g_string_append(s, ", \"completed\": ");
        task_json_escape(s, completed);
        g_free(completed);
    }
    g_string_append(s, "}");
    return g_string_free(s, FALSE);
}

/* list_body() — the JSON body for a tasklist create/rename.                */
static gchar *
list_body(const gchar *name)
{
    GString *s = g_string_new("{\"title\": ");
    task_json_escape(s, name);
    g_string_append(s, "}");
    return g_string_free(s, FALSE);
}

/* remote_updated_of() — pull the `updated` stamp out of a create/patch
 * reply so the local row can be stamped clean.  Falls back to `fallback`.  */
static gint64
remote_updated_of(TaskJson *reply, gint64 fallback)
{
    gint64 u = rfc3339_to_unix(task_json_str(reply, "updated"));
    return u != 0 ? u : fallback;
}

/* ---------------------------------------------------------------------------
 * fetch_default_list_gid() — GET the account's DEFAULT tasklist
 * (endpoint id "@default") and persist its id as
 * sync_state."default_list_gid".  The default list cannot be deleted
 * (tasklists.delete → 400 "Invalid Value" from any client), so the GUI
 * uses the stored id to refuse the deletion up front.  FALSE + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static gboolean
fetch_default_list_gid(TaskDatabase *db, const gchar *token, gchar **err)
{
    gchar *url = tasklist_url("@default");
    TaskJson *reply = NULL;
    gboolean ok = api_call("GET", url, token, NULL, NULL, &reply, err);
    if (ok && task_json_str(reply, "id") != NULL)
        host->db->state_set(db, "default_list_gid",
                          task_json_str(reply, "id"));
    task_json_free(reply);
    g_free(url);
    return ok;
}

/* ---------------------------------------------------------------------------
 * sync_lists() — reconcile tasklists.  Fills `pairs` with
 * (local id, gtasks id gchar*) tuples for the task pass — ownership of
 * the gid strings moves to the caller.  `app` is only for post_status
 * (idle-marshalled; safe from this worker).  FALSE + *err on failure.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  local_id;
    gchar  *gid;
} ListPair;

static gboolean
sync_lists(TaskApp *app, TaskDatabase *db, const gchar *token,
           gint64 last_sync, GArray *pairs, SyncStats *stats, gchar **err)
{
    if (!fetch_default_list_gid(db, token, err))
        return FALSE;
    gchar *default_gid = host->db->state_get(db, "default_list_gid");

    GPtrArray *remote = fetch_remote_lists(token, err);
    if (remote == NULL) {
        g_free(default_gid);
        return FALSE;
    }
    GPtrArray *local = host->db->lists(db, TRUE);
    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        TaskList *l = g_ptr_array_index(local, i);
        /* This list's Google identity, from the side table (schema v8).
         * Held as a local for the iteration because the binding can
         * CHANGE below — adopted by name, or replaced when the remote
         * list had to be re-created.                                      */
        gchar *lgid = gt_list_gid(db, l->id);

        /* Find the remote row this local one is bound to.                  */
        RemoteList *match = NULL;
        if (lgid != NULL) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteList *r = g_ptr_array_index(remote, j);
                if (strcmp(r->gid, lgid) == 0) {
                    match = r;
                    break;
                }
            }
        } else if (!l->deleted) {
            /* First-sync dedup: adopt an unmatched remote list with the
             * same name instead of creating a duplicate.                   */
            for (guint j = 0; j < remote->len; j++) {
                RemoteList *r = g_ptr_array_index(remote, j);
                if (!r->matched && strcmp(r->title, l->name) == 0) {
                    match = r;
                    gt_list_set_gid(db, l->id, r->gid);
                    g_free(lgid);
                    lgid = g_strdup(r->gid);
                    break;
                }
            }
        }
        if (match != NULL)
            match->matched = TRUE;

        if (l->deleted) {
            /* Google's DEFAULT tasklist is undeletable (the GUI refuses
             * up front; this catches a tombstone from an older build):
             * RESTORE the list and its same-moment task tombstones —
             * remote is the source of truth and still has everything.      */
            if (lgid != NULL && default_gid != NULL &&
                strcmp(lgid, default_gid) == 0) {
                host->db->list_restore(db, l->id);
                post_status(app, "Google's default list cannot be "
                            "deleted \xe2\x80\x94 restored");
                ListPair p = { l->id, g_strdup(lgid) };
                g_array_append_val(pairs, p);
                g_free(lgid);
                continue;
            }
            /* Local tombstone: propagate, then purge.                      */
            if (lgid != NULL) {
                gchar *url = tasklist_url(lgid);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                host->db->list_purge(db, l->id);
                stats->deleted++;
            }
            g_free(lgid);
            continue;
        }

        if (lgid == NULL || match == NULL) {
            /* Local new — or its bound remote list vanished without a
             * local tombstone.  NON-DESTRUCTIVE: absence never deletes;
             * the list exists here, so (re-)create it remotely and
             * adopt the new id.  On a re-create the list's tasks drop
             * their stale Google identities too, so the task pass
             * pushes every one of them as a new remote task.               */
            gboolean rebind = lgid != NULL;
            gchar *body = list_body(l->name);
            TaskJson *reply = NULL;
            gchar *url = tasklist_url(NULL);
            ok = api_call("POST", url, token, NULL, body, &reply, err);
            g_free(url);
            if (ok && task_json_str(reply, "id") != NULL) {
                if (rebind)
                    gt_list_forget_tasks(db, l->id);
                gt_list_set_gid(db, l->id, task_json_str(reply, "id"));
                host->db->list_apply_remote(db, l->id, l->name,
                    remote_updated_of(reply, l->updated_at));
                g_free(lgid);
                lgid = g_strdup(task_json_str(reply, "id"));
                stats->pushed++;
            }
            task_json_free(reply);
            g_free(body);
        } else if (strcmp(match->title, l->name) != 0) {
            /* Both exist, names differ: newer side wins.                   */
            gboolean local_dirty = l->updated_at > last_sync;
            if (local_dirty && l->updated_at >= match->updated) {
                gchar *body = list_body(l->name);
                gchar *url = tasklist_url(lgid);
                ok = api_call("PATCH", url, token, NULL, body, NULL, err);
                if (ok)
                    stats->pushed++;
                g_free(url);
                g_free(body);
            } else {
                host->db->list_apply_remote(db, l->id, match->title,
                                          match->updated);
                stats->pulled++;
            }
        }

        if (ok && lgid != NULL) {
            ListPair p = { l->id, g_strdup(lgid) };
            g_array_append_val(pairs, p);
        }
        g_free(lgid);
    }

    /* Remote lists nobody local claimed: new on the Google side.           */
    for (guint j = 0; j < remote->len && ok; j++) {
        RemoteList *r = g_ptr_array_index(remote, j);
        if (r->matched)
            continue;
        gint64 id = host->db->list_create(db, r->title, "");
        if (id != 0) {
            gt_list_set_gid(db, id, r->gid);
            host->db->list_apply_remote(db, id, r->title, r->updated);
            ListPair p = { id, g_strdup(r->gid) };
            g_array_append_val(pairs, p);
            stats->pulled++;
        }
    }

    /* Google's undeletable DEFAULT list always wears a 🔴 indicator:
     * seeded only while the emoji is empty, so a user's later Edit List
     * choice sticks (clearing it brings the dot back next sync).           */
    if (ok && default_gid != NULL) {
        /* Resolve Google's id to a local list first: the emoji setter is
         * a core function and is keyed on the list, not on whatever any
         * one integration calls it.                                       */
        gint64 dl = gt_list_by_gid(db, default_gid);
        if (dl != 0)
            host->db->list_emoji_if_empty(db, dl, "\xf0\x9f\x94\xb4");
    }

    for (guint i = 0; i < remote->len; i++)
        remote_list_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    host->db->lists_free(local);
    g_free(default_gid);
    return ok;
}

/* stamp_clean() — after a successful create/patch, write the local row
 * back CLEAN: the reply's updated time on the core row (so it is not
 * immediately dirty again), and the fresh etag / webViewLink in the side
 * table.
 *
 * A reply that omits webViewLink keeps whatever is stored: Google sends
 * it on create and not always on patch, and dropping it would lose the
 * "Open in Google Tasks" link on the next push.  Nothing in *t is
 * modified or freed.                                                       */
static void
stamp_clean(TaskDatabase *db, const Task *t, const gchar *gid,
            TaskJson *reply, SyncStats *stats)
{
    Task clean = *t;
    clean.updated_at = remote_updated_of(reply, t->updated_at);
    host->db->task_apply_remote(db, &clean);

    gchar *web = task_json_str(reply, "webViewLink") != NULL
               ? g_strdup(task_json_str(reply, "webViewLink"))
               : gt_text(db, "gtasks_task", "task_id", "web_link", t->id);
    gchar *glinks   = gt_text(db, "gtasks_task", "task_id", "glinks", t->id);
    gchar *assigned = gt_text(db, "gtasks_task", "task_id", "assigned", t->id);
    gt_task_set(db, t->id, gid, task_json_str(reply, "etag"), web,
                glinks, assigned);
    g_free(web);
    g_free(glinks);
    g_free(assigned);
    stats->pushed++;
}

/* ---------------------------------------------------------------------------
 * push_task_create() — POST one local task to Google (parent_gid NULL for
 * top-level) and adopt the returned id/stamp.  FALSE + *err on failure.
 * ------------------------------------------------------------------------- */
static gboolean
push_task_create(TaskDatabase *db, const gchar *token, const gchar *list_gid,
                 Task *t, const gchar *parent_gid, SyncStats *stats,
                 gchar **err)
{
    gchar *body = task_body(t);
    gchar *url = task_url(list_gid, NULL);
    if (parent_gid != NULL) {
        gchar *parent_esc = escaped(parent_gid);
        gchar *with_parent = g_strdup_printf("%s?parent=%s", url,
                                             parent_esc);
        g_free(parent_esc);
        g_free(url);
        url = with_parent;
    }
    TaskJson *reply = NULL;
    gboolean ok = api_call("POST", url, token, NULL, body, &reply, err);
    if (ok && task_json_str(reply, "id") != NULL) {
        stamp_clean(db, t, task_json_str(reply, "id"), reply, stats);
    }
    task_json_free(reply);
    g_free(url);
    g_free(body);
    return ok;
}

/* ---------------------------------------------------------------------------
 * push_task_patch() — PATCH one local task's synced fields, guarded by
 * the stored etag ("If-Match"): a 412 means the remote changed since we
 * last pulled it — the push is SKIPPED (not a sync failure) and the
 * next pull reconciles.  On success the reply's updated/etag stamp the
 * row clean.
 * ------------------------------------------------------------------------- */
static gboolean
push_task_patch(TaskDatabase *db, const gchar *token, const gchar *list_gid,
                const Task *t, const gchar *gid, SyncStats *stats,
                gchar **err)
{
    gchar *body = task_body(t);
    gchar *url = task_url(list_gid, gid);
    gchar *etag = gt_task_etag(db, t->id);
    TaskJson *reply = NULL;
    gboolean ok = api_call("PATCH", url, token, etag, body, &reply, err);
    g_free(etag);
    if (ok) {
        stamp_clean(db, t, gid, reply, stats);
    } else if (*err != NULL && strstr(*err, "HTTP 412") != NULL) {
        g_clear_pointer(err, g_free);  /* remote moved on: theirs wins      */
        ok = TRUE;
    }
    task_json_free(reply);
    g_free(url);
    g_free(body);
    return ok;
}

/* push_as_new() — POST one local task as a NEW remote one: resolve the
 * parent's gid (parents are pushed first, so a subtask's parent already
 * carries one), create, and index the adopted gid so later children can
 * find it.                                                                 */
static gboolean
push_as_new(TaskDatabase *db, const gchar *token, const gchar *list_gid,
            Task *t, GHashTable *gids, GHashTable *local_by_gid,
            SyncStats *stats, gchar **err)
{
    const gchar *parent_gid = t->parent_id != 0
                            ? gid_of(gids, t->parent_id) : NULL;
    gboolean ok = push_task_create(db, token, list_gid, t, parent_gid,
                                   stats, err);
    /* The create adopted an id; read it back into the map so a later
     * child of this task can address its parent.                        */
    if (ok) {
        gchar *fresh = gt_task_gid(db, t->id);
        if (fresh != NULL) {
            gid_put(gids, t->id, fresh);
            g_hash_table_insert(local_by_gid,
                                (gpointer)gid_of(gids, t->id), t);
            g_free(fresh);
        }
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * sync_tasks_for_list() — reconcile the tasks of one bound list pair.
 * ------------------------------------------------------------------------- */
static gboolean
sync_tasks_for_list(TaskDatabase *db, const gchar *token, gint64 list_id,
                    const gchar *list_gid, gint64 last_sync,
                    SyncStats *stats, gchar **err)
{
    /* After the first sync, fetch INCREMENTALLY: only items changed
     * since the last pass (with a 5-minute overlap for clock skew —
     * re-applying an unchanged item is a no-op).  A partial listing
     * changes the deletion inference below: "not in the response" means
     * unchanged, not deleted.                                              */
    gboolean full_listing = last_sync == 0;
    GPtrArray *remote = fetch_remote_tasks(token, list_gid,
        full_listing ? 0 : MAX(last_sync - 300, 1), err);
    if (remote == NULL)
        return FALSE;

    /* This list's rows, tombstones included; parents before subtasks, so
     * a new parent is pushed — and owns a gtasks_id — before its
     * children.  (One per-list query, not an all-rows scan per list.)      */
    GPtrArray *local = host->db->tasks_in_list_all(db, list_id);

    /* gid → RemoteTask and gid → local Task maps for the match passes.    */
    GHashTable *by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < remote->len; i++) {
        RemoteTask *r = g_ptr_array_index(remote, i);
        g_hash_table_insert(by_gid, r->gid, r);
    }
    /* Every bound task of this list, in one query (see gt_gids_for_list).*/
    GHashTable *gids = gt_gids_for_list(db, list_id);
    GHashTable *local_by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < local->len; i++) {
        Task *t = g_ptr_array_index(local, i);
        const gchar *g = gid_of(gids, t->id);
        if (g != NULL)
            g_hash_table_insert(local_by_gid, (gpointer)g, t);
    }

    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        Task *t = g_ptr_array_index(local, i);

        const gchar *tgid = gid_of(gids, t->id);
        RemoteTask *match = tgid != NULL
            ? g_hash_table_lookup(by_gid, tgid) : NULL;

        /* First-sync dedup: adopt an unmatched live remote task with the
         * same title (top-level against top-level only — subtask titles
         * repeat too easily across parents to guess).  Only meaningful
         * against a FULL listing.                                          */
        if (full_listing &&
            match == NULL && tgid == NULL && !t->deleted &&
            t->parent_id == 0) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteTask *r = g_ptr_array_index(remote, j);
                if (!r->matched && !r->deleted && r->parent_gid == NULL &&
                    strcmp(r->title, t->title) == 0) {
                    match = r;
                    gt_task_set_gid(db, t->id, r->gid);
                    gid_put(gids, t->id, r->gid);
                    tgid = gid_of(gids, t->id);
                    g_hash_table_insert(local_by_gid, (gpointer)tgid, t);
                    break;
                }
            }
        }
        if (match != NULL)
            match->matched = TRUE;

        if (t->deleted) {
            /* Local tombstone: propagate, then purge.                      */
            if (tgid != NULL &&
                (match == NULL || !match->deleted)) {
                gchar *url = task_url(list_gid, tgid);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                host->db->task_purge(db, t->id);
                stats->deleted++;
            }
            continue;
        }

        if (tgid == NULL) {
            /* Local new.                                                   */
            ok = push_as_new(db, token, list_gid, t, gids, local_by_gid,
                             stats, err);
            continue;
        }

        if (match == NULL) {
            if (full_listing) {
                /* Gone from a FULL listing (deleted on Google without a
                 * local tombstone).  NON-DESTRUCTIVE: absence never
                 * deletes — the task exists here, so drop the stale
                 * Google identity and push it back as a NEW remote
                 * task.  Explicit deletes still propagate: a local
                 * tombstone DELETEs remotely (above) and a remote
                 * `deleted:true` purges locally (below).                   */
                g_hash_table_remove(local_by_gid, tgid);
                gt_task_set(db, t->id, NULL, NULL, NULL, NULL, NULL);
                gid_put(gids, t->id, NULL);
                ok = push_as_new(db, token, list_gid, t, gids,
                                 local_by_gid, stats, err);
            } else if (t->updated_at > last_sync) {
                /* Incremental listing: absent just means unchanged — but
                 * the LOCAL side is dirty, so push (etag-guarded).         */
                ok = push_task_patch(db, token, list_gid, t, tgid, stats,
                                     err);
            }
            continue;
        }
        if (match->deleted) {
            host->db->task_purge(db, t->id);
            stats->deleted++;
            continue;
        }

        /* Both sides live: compare the synced fields.  Only the DONE-ness
         * of the status counts — a New ↔ In Progress move is invisible to
         * Google, so treating it as a difference would push a body
         * identical to what is already there.                              */
        gboolean differs = strcmp(t->title, match->title) != 0 ||
                           strcmp(t->notes, match->notes) != 0 ||
                           t->due != match->due ||
                           (t->status == TASK_STATUS_DONE) != match->done;
        if (!differs) {
            /* Content equal — still refresh the mirror metadata when the
             * remote bumped OR the row predates the metadata columns
             * (etag/web_link empty while the remote has them).             */
            gchar *have_etag = gt_task_etag(db, t->id);
            gchar *have_link  = gt_text(db, "gtasks_task", "task_id",
                                        "web_link", t->id);
            gboolean meta_stale =
                (have_etag == NULL && match->etag != NULL) ||
                (have_link == NULL && match->web_link != NULL);
            g_free(have_etag);
            g_free(have_link);
            if (match->updated > t->updated_at || meta_stale) {
                Task apply = *t;
                apply.updated_at   = match->updated;
                apply.completed_at = match->completed;
                host->db->task_apply_remote(db, &apply);
                gt_task_set(db, t->id, tgid, match->etag, match->web_link,
                            match->glinks, match->assigned);
            }
            continue;
        }
        gboolean local_dirty = t->updated_at > last_sync;
        if (local_dirty && t->updated_at >= match->updated) {
            ok = push_task_patch(db, token, list_gid, t, tgid, stats, err);
        } else {
            Task apply = *t;       /* shallow copy is fine here           */
            apply.title        = match->title;
            apply.notes        = match->notes;
            apply.due          = match->due;
            /* Google reports done-ness only, so fold it onto the status
             * the row already holds: a remote un-tick lands on In
             * Progress, and a still-unfinished New task stays New.         */
            apply.status       = host->util->status_apply_done(t->status,
                                                        match->done);
            apply.updated_at   = match->updated;
            apply.completed_at = match->completed;
            host->db->task_apply_remote(db, &apply);
            gt_task_set(db, t->id, tgid, match->etag, match->web_link,
                        match->glinks, match->assigned);
            stats->pulled++;
        }
    }

    /* Remote tasks nobody local claimed: new on the Google side.  Two
     * passes — parents first so subtasks can resolve their local parent.   */
    for (gint pass = 0; pass < 2 && ok; pass++) {
        for (guint j = 0; j < remote->len; j++) {
            RemoteTask *r = g_ptr_array_index(remote, j);
            gboolean is_child = r->parent_gid != NULL;
            /* hidden = completed + cleared on Google (tasks.clear): a
             * locally purged Clear-Completed victim must not resurrect.    */
            if (r->matched || r->deleted || r->hidden ||
                is_child != (pass == 1))
                continue;
            gint64 parent_id = 0;    /* local id of the parent task         */
            if (is_child) {
                Task *p = g_hash_table_lookup(local_by_gid,
                                              r->parent_gid);
                if (p == NULL || p->parent_id != 0)
                    continue;        /* orphan / over-deep: skip            */
                parent_id = p->id;
            }
            gint64 id = host->db->task_create(db, list_id, parent_id,
                                            r->title);
            if (id == 0)
                continue;
            Task nt = { 0 };       /* the fields apply_remote writes      */
            nt.id           = id;
            nt.title        = r->title;
            nt.notes        = r->notes;
            nt.due          = r->due;
            /* A row that did not exist here a moment ago: nothing to
             * preserve, so not-done means NEW.                             */
            nt.status       = r->done ? TASK_STATUS_DONE : TASK_STATUS_NEW;
            nt.updated_at   = r->updated;
            nt.completed_at = r->completed;
            host->db->task_apply_remote(db, &nt);
            gt_task_set(db, id, r->gid, r->etag, r->web_link, r->glinks,
                        r->assigned);
            gid_put(gids, id, r->gid);
            r->matched = TRUE;
            stats->pulled++;
            if (!is_child) {
                /* Make the new row findable for pass 2's children.         */
                Task *row = host->db->task_get(db, id);
                if (row != NULL) {
                    g_hash_table_insert(local_by_gid,
                                        (gpointer)gid_of(gids, id), row);
                    g_ptr_array_add(local, row);   /* owned by `local`      */
                }
            }
        }
    }

    g_hash_table_destroy(by_gid);
    g_hash_table_destroy(local_by_gid);
    host->db->tasks_free(local);
    for (guint i = 0; i < remote->len; i++)
        remote_task_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    return ok;
}

/* ===========================================================================
 * The worker thread and its main-thread marshalling.
 * =========================================================================== */

typedef struct {
    TaskApp        *app;               /* lives for the program's lifetime  */
    gchar        *db_path;
    TaskSyncDoneFn  done;
    gpointer      user_data;
    gboolean      ok;                /* out: result                         */
    gchar        *message;           /* out: summary or error               */
} SyncJob;

/* status_idle() — post a status-bar message from the worker.               */
typedef struct {
    TaskApp *app;
    gchar *msg;
} StatusPost;

static gboolean
status_idle(gpointer data)
{
    StatusPost *sp = data;
    gt_status(sp->app, "%s", sp->msg);
    g_free(sp->msg);
    g_free(sp);
    return G_SOURCE_REMOVE;
}

/* post_status() — queue a status-bar message from the worker thread
 * (marshalled to the main thread; `msg` is copied).                        */
static void
post_status(TaskApp *app, const gchar *msg)
{
    StatusPost *sp = g_new0(StatusPost, 1);
    sp->app = app;
    sp->msg = g_strdup(msg);
    g_idle_add(status_idle, sp);
}

/* sync_apply() — main-thread completion: clear the running flag, refresh
 * the library, report.                                                     */
static gboolean
sync_apply(gpointer data)
{
    SyncJob *job = data;
    gt_running = FALSE;
    host->notify->notify_changed(job->app);
    gt_status(job->app, "%s", job->message);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* sync_thread() — the whole sync pass (worker thread, own connection).     */
static gpointer
sync_thread(gpointer data)
{
    SyncJob *job = data;
    SyncStats stats = { 0, 0, 0 };
    gchar *err = NULL;

    gchar *token = task_oauth_access_token(&err);
    if (token == NULL) {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       err != NULL ? err : "no token");
        g_free(err);
        g_idle_add(sync_apply, job);
        return NULL;
    }

    GError *gerr = NULL;
    TaskDatabase *db = host->db->open(job->db_path, &gerr);
    if (db == NULL) {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(token);
        g_idle_add(sync_apply, job);
        return NULL;
    }

    /* last_sync gates the "locally dirty" test; the new value is the
     * time this pass STARTED, so mid-sync edits stay dirty for the next.   */
    gchar *ls = host->db->state_get(db, "last_sync");
    gint64 last_sync = ls != NULL ? g_ascii_strtoll(ls, NULL, 10) : 0;
    g_free(ls);
    gint64 started = g_get_real_time() / G_USEC_PER_SEC;

    post_status(job->app, "Syncing with Google Tasks\xe2\x80\xa6");

    GArray *pairs = g_array_new(FALSE, FALSE, sizeof(ListPair));
    gboolean ok = sync_lists(job->app, db, token, last_sync, pairs,
                             &stats, &err);
    for (guint i = 0; i < pairs->len && ok; i++) {
        ListPair *p = &g_array_index(pairs, ListPair, i);
        ok = sync_tasks_for_list(db, token, p->local_id, p->gid,
                                 last_sync, &stats, &err);
    }
    for (guint i = 0; i < pairs->len; i++)
        g_free(g_array_index(pairs, ListPair, i).gid);
    g_array_free(pairs, TRUE);

    if (ok) {
        gchar *stamp = g_strdup_printf("%lld", (long long)started);
        host->db->state_set(db, "last_sync", stamp);
        g_free(stamp);
        job->ok = TRUE;
        job->message = g_strdup_printf(
            "Sync done: %d pushed, %d pulled, %d deleted",
            stats.pushed, stats.pulled, stats.deleted);
    } else {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       err != NULL ? err : "unknown error");
    }
    g_free(err);
    host->db->close(db);
    g_free(token);
    g_idle_add(sync_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * task_sync_start() — kick off one pass (see gtasks.h).
 * ------------------------------------------------------------------------- */
void
task_sync_start(TaskApp *app, const gchar *db_path,
                TaskSyncDoneFn done, gpointer user_data)
{
    if (!host->config->get_bool(self, "sync_enabled", TRUE)) {
        gt_status(app, "Google Tasks sync is disabled \xe2\x80\x94 "
                        "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "sync disabled", user_data);
        return;
    }
    if (gt_running) {
        gt_status(app, "Sync already running");
        return;
    }
    if (!task_oauth_authenticated()) {
        gt_status(app, "Not signed in to Google \xe2\x80\x94 use the "
                        "Sync button or File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "not signed in", user_data);
        return;
    }
    gt_running = TRUE;
    SyncJob *job = g_new0(SyncJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->done      = done;
    job->user_data = user_data;
    GThread *th = g_thread_new("task-sync", sync_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * task_sync_signin_done() — shared tail of a browser sign-in that was
 * started to sync (see gtasks.h).
 * ------------------------------------------------------------------------- */
void
task_sync_signin_done(TaskApp *app, GtkWindow *parent, const gchar *db_path,
                      gboolean ok, const gchar *error, TaskSyncDoneFn done)
{
    if (ok) {
        task_sync_start(app, db_path, done, NULL);
    } else {
        gt_notice(parent, GTK_MESSAGE_ERROR,
                        "Tasks - Google Sign-In",
                        "Could not sign in: %s",
                        error != NULL ? error : "unknown error");
    }
}

/* ===========================================================================
 * Cross-list move (tasks.move + destinationTasklist) and Clear Completed
 * (tasks.clear) — one-shot worker jobs following the sync pattern:
 * network on a thread, results marshalled back via g_idle_add.
 * =========================================================================== */

typedef struct {
    TaskApp     *app;
    gchar     *src_gid;              /* source list gid                     */
    gchar     *dest_gid;             /* destination list gid                */
    gchar     *task_gid;             /* the moved task's gid                */
    GPtrArray *child_gids;           /* its subtasks' gids (owned)          */
    gint64     task_id;              /* local ids for the fallback          */
    gint64     src_list_id;
    gboolean   ok;
    gchar     *error;
} MoveJob;

/* move_job_free() — free a MoveJob and everything it owns.                 */
static void
move_job_free(MoveJob *job)
{
    g_free(job->src_gid);
    g_free(job->dest_gid);
    g_free(job->task_gid);
    for (guint i = 0; i < job->child_gids->len; i++)
        g_free(g_ptr_array_index(job->child_gids, i));
    g_ptr_array_free(job->child_gids, TRUE);
    g_free(job->error);
    g_free(job);
}

/* move_fallback() — the offline/failed-move recovery (main thread): the
 * moved rows lose their Google identity (they will push as NEW tasks in
 * the destination list) and stub tombstones in the SOURCE list delete
 * the old remote copies on the next sync.                                  */
static void
move_fallback(TaskApp *app, MoveJob *job)
{
    if (job->task_gid == NULL)
        return;                      /* never synced: nothing to unlink     */
    gt_remote_tombstone(app->db, job->src_list_id, job->task_gid);
    gt_task_set(app->db, job->task_id, NULL, NULL, NULL, NULL, NULL);
    GPtrArray *subs = host->db->subtasks(app->db, job->task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);
        gchar *sgid = gt_task_gid(app->db, s->id);
        if (sgid != NULL) {
            gt_remote_tombstone(app->db, job->src_list_id, sgid);
            gt_task_set(app->db, s->id, NULL, NULL, NULL, NULL, NULL);
            g_free(sgid);
        }
    }
    host->db->tasks_free(subs);
}

/* move_apply() — main-thread completion of the remote move.                */
static gboolean
move_apply(gpointer data)
{
    MoveJob *job = data;
    if (!job->ok) {
        move_fallback(job->app, job);
        gt_status(job->app, "Move will finish on the next sync (%s)",
                        job->error != NULL ? job->error : "remote move failed");
    } else {
        gt_status(job->app, "Moved in Google Tasks");
    }
    host->notify->notify_changed(job->app);
    move_job_free(job);
    return G_SOURCE_REMOVE;
}

/* move_call() — one tasks.move POST.                                       */
static gboolean
move_call(const gchar *token, const gchar *src_gid, const gchar *task_gid,
          const gchar *dest_gid, const gchar *parent_gid, gchar **err)
{
    gchar *base = task_url(src_gid, task_gid);
    gchar *dest_esc = escaped(dest_gid);
    GString *url = g_string_new(base);
    g_string_append_printf(url, "/move?destinationTasklist=%s", dest_esc);
    if (parent_gid != NULL) {
        gchar *parent_esc = escaped(parent_gid);
        g_string_append_printf(url, "&parent=%s", parent_esc);
        g_free(parent_esc);
    }
    gboolean ok = api_call("POST", url->str, token, NULL, NULL,
                           NULL, err);
    g_string_free(url, TRUE);
    g_free(dest_esc);
    g_free(base);
    return ok;
}

/* move_thread() — worker: move the parent, then each subtask under it.     */
static gpointer
move_thread(gpointer data)
{
    MoveJob *job = data;
    gchar *token = task_oauth_access_token(&job->error);
    if (token == NULL) {
        job->ok = FALSE;
        g_idle_add(move_apply, job);
        return NULL;
    }
    job->ok = move_call(token, job->src_gid, job->task_gid,
                        job->dest_gid, NULL, &job->error);
    for (guint i = 0; job->ok && i < job->child_gids->len; i++)
        job->ok = move_call(token, job->src_gid,
                            g_ptr_array_index(job->child_gids, i),
                            job->dest_gid, job->task_gid, &job->error);
    g_free(token);
    g_idle_add(move_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * gtasks_task_moved() — the Google half of a cross-list move, run as a
 * task_ops "moved" hook (see task_ops.h).
 *
 * The LOCAL move has already happened and committed by the time this
 * runs, which is why the hook is handed `from_list`: the row now reads
 * as belonging to the destination, and tasks.move has to address the
 * task through the list it is moving OUT of.  Everything else this needs
 * survives the local move untouched — the task's own gtasks_id, its
 * subtasks' gtasks_ids, and both lists' gtasks_ids.
 *
 * Validation ("is it top-level, is it actually changing list") is
 * task_ops' job and has already passed.
 * ------------------------------------------------------------------------- */
static void
gtasks_task_moved(TaskApp *app, gint64 task_id, gint64 from_list,
                  gint64 to_list, gpointer user_data)
{
    (void)user_data;
    Task *t = host->db->task_get(app->db, task_id);
    if (t == NULL)
        return;                      /* deleted between the write and here  */

    TaskList *src  = host->db->list_get(app->db, from_list);
    TaskList *dest = host->db->list_get(app->db, to_list);

    MoveJob *job = g_new0(MoveJob, 1);
    job->app         = app;
    job->task_id     = task_id;
    job->src_list_id = from_list;
    job->src_gid     = src != NULL ? gt_list_gid(app->db, src->id) : NULL;
    job->dest_gid    = dest != NULL ? gt_list_gid(app->db, dest->id) : NULL;
    job->task_gid    = gt_task_gid(app->db, task_id);
    job->child_gids  = g_ptr_array_new();
    GPtrArray *subs = host->db->subtasks(app->db, task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);
        gchar *sgid = gt_task_gid(app->db, s->id);
        if (sgid != NULL)
            g_ptr_array_add(job->child_gids, sgid);  /* takes ownership   */
    }
    host->db->tasks_free(subs);
    host->db->list_free(src);
    host->db->list_free(dest);
    host->db->task_free(t);

    if (job->task_gid != NULL && job->src_gid != NULL &&
        job->dest_gid != NULL && task_oauth_authenticated()) {
        GThread *th = g_thread_new("task-move", move_thread, job);
        g_thread_unref(th);
    } else {
        move_fallback(app, job);     /* offline / unsynced endpoints        */
        host->notify->notify_changed(app);
        move_job_free(job);
    }
}

/* --------------------------------------------------------------------------
 * Clear Completed.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp   *app;
    gchar   *list_gid;
    gint64   list_id;
    GArray  *ids;                    /* gint64; the rows task_ops cleared   */
    gboolean ok;
    gchar   *error;
} ClearJob;

/* clear_apply() — main-thread completion.
 *
 * On success the rows are purged for real: task_ops already tombstoned
 * them, and Google has now hidden its own copies, so there is no delete
 * left to propagate and the hidden guard keeps them from resurrecting.
 * Exactly the ids task_ops reported are purged, rather than re-running a
 * "delete every done task of this list" query — a task completed while
 * the request was in flight was never part of this clear and must keep
 * its tombstone so the next pass still tells Google about it.
 *
 * On FAILURE nothing is purged, and that is the whole recovery: the
 * tombstones task_ops wrote are still there and sync as ordinary
 * deletes.                                                                */
static gboolean
clear_apply(gpointer data)
{
    ClearJob *job = data;
    if (job->ok) {
        for (guint i = 0; i < job->ids->len; i++)
            host->db->task_purge(job->app->db,
                               g_array_index(job->ids, gint64, i));
        gt_status(job->app, "Completed tasks cleared in Google Tasks");
    } else {
        gt_status(job->app, "Cleared locally; Google will catch up "
                        "on the next sync (%s)",
                        job->error != NULL ? job->error : "clear failed");
    }
    host->notify->notify_changed(job->app);
    g_array_free(job->ids, TRUE);
    g_free(job->list_gid);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* clear_thread() — worker: POST tasks.clear.                               */
static gpointer
clear_thread(gpointer data)
{
    ClearJob *job = data;
    gchar *token = task_oauth_access_token(&job->error);
    if (token == NULL) {
        job->ok = FALSE;
        g_idle_add(clear_apply, job);
        return NULL;
    }
    gchar *gid_esc = escaped(job->list_gid);
    gchar *url = g_strdup_printf(GTASKS_API "/lists/%s/clear", gid_esc);
    job->ok = api_call("POST", url, token, NULL, NULL, NULL, &job->error);
    g_free(url);
    g_free(gid_esc);
    g_free(token);
    g_idle_add(clear_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * gtasks_completed_cleared() — the Google half of Clear Completed, run
 * as a task_ops "cleared" hook (see task_ops.h).
 *
 * task_ops has already tombstoned the rows, which is the answer that is
 * correct on its own: those tombstones sync as ordinary deletes and the
 * completed tasks disappear from Google on the next pass.  All this adds
 * is the SHORTCUT — one tasks.clear call archives the lot server-side,
 * after which the local rows can be purged outright instead of waiting
 * to be pushed one delete at a time.  That is also why the hook wants a
 * single "cleared" event rather than N delete events: the batch call has
 * nothing to batch otherwise.
 *
 * Doing nothing here is therefore always safe, and is what happens when
 * the list has never synced or nobody is signed in.
 * ------------------------------------------------------------------------- */
static void
gtasks_completed_cleared(TaskApp *app, gint64 list_id, GArray *task_ids,
                         gpointer user_data)
{
    (void)user_data;
    if (task_ids->len == 0)
        return;                      /* nothing went; nothing to archive    */

    TaskList *l = host->db->list_get(app->db, list_id);
    if (l == NULL)
        return;
    gchar *lgid = gt_list_gid(app->db, list_id);
    if (lgid != NULL && task_oauth_authenticated()) {
        ClearJob *job = g_new0(ClearJob, 1);
        job->app      = app;
        job->list_id  = list_id;
        job->list_gid = g_strdup(lgid);
        /* Own copy: the event's array is borrowed for the call only.      */
        job->ids      = g_array_sized_new(FALSE, FALSE, sizeof(gint64),
                                          task_ids->len);
        g_array_append_vals(job->ids, task_ids->data, task_ids->len);
        GThread *th = g_thread_new("task-clear", clear_thread, job);
        g_thread_unref(th);
    }
    host->db->list_free(l);
}

/* ---------------------------------------------------------------------------
 * Periodic auto-sync — the scheduler drives it (see task_worker.h).
 * ------------------------------------------------------------------------- */

/* sync_run() / sync_ready() — the two callbacks the scheduler needs.
 * `ready` gates both the periodic tick and the pass that arming runs:
 * there is nothing to sync while signed out, and asking would only
 * produce an error message nobody asked for.                              */
static void
sync_run(TaskApp *app, const gchar *db_path)
{
    task_sync_start(app, db_path, NULL, NULL);
}

static gboolean
sync_ready(TaskApp *app)
{
    (void)app;
    return task_oauth_authenticated();
}

/* signin_then_sync() — completion of the browser flow started below.
 * Re-resolves nothing: the app outlives the flow, and the window may
 * not, so the dialog on failure is parented on whatever is there.       */
typedef struct { TaskApp *app; gchar *db_path; } SigninJob;

static void
signin_then_sync(gboolean ok, const gchar *error, gpointer data)
{
    SigninJob *j = data;
    task_sync_signin_done(j->app,
                          j->app->library_window != NULL
                            ? GTK_WINDOW(j->app->library_window) : NULL,
                          j->db_path, ok, error, NULL);
    g_free(j->db_path);
    g_free(j);
}

/* task_sync_begin_signin() — start the browser flow and sync on success. */
static void
task_sync_begin_signin(TaskApp *app, const gchar *db_path)
{
    SigninJob *j = g_new0(SigninJob, 1);
    j->app     = app;
    j->db_path = g_strdup(db_path);
    task_oauth_begin(app->library_window != NULL
                       ? GTK_WINDOW(app->library_window) : NULL,
                     signin_then_sync, j);
}

/* sync_blocked() — the user pressed Sync Now while signed out.
 *
 * Sign-in is per session: the in-memory access token is gone on every
 * launch, so this is the ordinary path, not an error.  The browser flow
 * is usually a silent redirect that comes straight back.                 */
static void
sync_blocked(TaskApp *app, const gchar *db_path)
{
    if (!task_oauth_have_client()) {
        gt_status(app, "Google sync is not configured \xe2\x80\x94 "
                        "see File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return;
    }
    gt_status(app, "Opening browser for Google sign-in\xe2\x80\xa6");
    task_sync_begin_signin(app, db_path);
}

static const TaskWorkerDef sync_worker = {
    .id               = "gtasks",
    .enabled_key      = "gtasks_sync_enabled",
    .enabled_default  = TRUE,
    .interval_key     = "gtasks_interval_min",
    .interval_default = 5,
    .initial          = TASK_WORKER_INITIAL_ARMED,
    .running          = NULL,        /* filled in by gtasks_init            */
    .timer            = NULL,
    .run              = sync_run,
    .ready            = sync_ready,
    .on_arm           = NULL,
    .on_blocked       = sync_blocked,
};

/* The def carries POINTERS to the app's own flag and GSource id, which
 * are not known until an app exists — so the static above is completed
 * once, at registration.                                                  */
static TaskWorkerDef sync_worker_live;

/* task_sync_auto_start() — (re)arm the auto-sync timer (see gtasks.h).     */
void
task_sync_auto_start(TaskApp *app, const gchar *db_path)
{
    host->worker->arm(app, &sync_worker_live, db_path);
}

/* ---------------------------------------------------------------------------
 * gtasks_list_veto() — refuse to delete Google's default tasklist, run
 * as a task_ops list veto (see task_ops.h).
 *
 * tasklists.delete answers 400 "Invalid Value" for the default list from
 * any client, so there is no way to honour the delete remotely.  Vetoing
 * up front is what keeps the local side honest: without it the list is
 * tombstoned here, the push fails, and sync_lists has to RESTORE it —
 * the list visibly vanishes and comes back a few seconds later.
 *
 * A list that is not bound to Google, or a database that has not yet
 * learned which list is the default, is nobody's business here.
 * ------------------------------------------------------------------------- */
static gboolean
gtasks_list_veto(TaskApp *app, const TaskList *list, gchar **why,
                 gpointer user_data)
{
    (void)user_data;
    gchar *lgid = gt_list_gid(app->db, list->id);
    if (lgid == NULL)
        return TRUE;
    gchar *default_gid = host->db->state_get(app->db, "default_list_gid");
    gboolean ok = default_gid == NULL || strcmp(lgid, default_gid) != 0;
    g_free(lgid);
    if (!ok && why != NULL)
        *why = g_strdup_printf("\xe2\x80\x9c%s\xe2\x80\x9d is Google's "
                               "default list and cannot be deleted",
                               list->name);
    g_free(default_gid);
    return ok;
}

/* ===========================================================================
 * The toolbar button.
 *
 * Google's own, contributed through the UI registry (see task_ui.h)
 * rather than built into the window: it is this integration's control,
 * so it belongs with this integration's code and travels with it.
 *
 * File -> Sync Now stays the CATCH-ALL — every registered worker, in one
 * press.  This button is the specific one, which is why it can afford to
 * grey itself out while its own pass runs: there is exactly one sync it
 * can mean.
 * =========================================================================== */

/* toolbar_done() — the pass finished; give the button back.              */
static void
toolbar_done(TaskApp *app, gboolean ok, const gchar *message, gpointer d)
{
    (void)app; (void)ok; (void)message; (void)d;
    host->ui->tool_set_sensitive("gtasks-sync", TRUE);
}

/* toolbar_clicked() — sync now, signing in first if the session has no
 * token yet (which is every launch — the access token lives in memory
 * only, so the browser round trip is the ordinary path, not an error). */
static void
toolbar_clicked(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    if (!host->config->get_bool(self, "sync_enabled", TRUE)) {
        gt_status(app, "Google Tasks sync is disabled \xe2\x80\x94 "
                        "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return;
    }
    if (!task_oauth_have_client()) {
        gt_status(app, "Google sync is not configured \xe2\x80\x94 "
                        "see File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return;
    }
    host->ui->tool_set_sensitive("gtasks-sync", FALSE);
    if (task_oauth_authenticated()) {
        task_sync_start(app, app->db->path, toolbar_done, NULL);
    } else {
        /* The button comes back when the flow finishes either way: a
         * cancelled sign-in must not leave it dead.                     */
        gt_status(app,
                        "Opening browser for Google sign-in\xe2\x80\xa6");
        task_sync_begin_signin(app, app->db->path);
        host->ui->tool_set_sensitive("gtasks-sync", TRUE);
    }
}

/* Shown only when this integration is on AND the user wants the button.
 * Re-asked on every full refresh, so flipping either setting is enough. */
static gboolean
toolbar_visible(TaskApp *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    return host->config->get_bool(self, "sync_enabled", TRUE) &&
           host->config->get_bool(self, "toolbar_button", TRUE);
}

static const TaskUiToolDef sync_tool = {
    .id              = "gtasks-sync",
    .icon            = "google-symbol",
    .fallback_markup = "\xe2\x9f\xb3",
    .label           = "Sync",
    .tooltip         = "Sync with Google Tasks now",
    .sort            = 10,
    .clicked         = toolbar_clicked,
    .visible         = toolbar_visible,
};

/* ===========================================================================
 * The "From Google" editor section.
 *
 * Read-only metadata the sync pulled: completion time, a Docs/Chat
 * assignment origin, Google-attached links, and the deep link into
 * Google's own UI.  Contributed through the editor-section registry
 * (task_ui.h) rather than built into the editor, which is what lets the
 * editor stop parsing Google's JSON.
 *
 * Returns NULL for a task with none of it — the common case, and the
 * reason the editor's contributed area costs an ordinary task nothing.
 * =========================================================================== */

/* link_button() — a left-aligned GtkLinkButton row.                       */
static void
link_button(GtkWidget *box, const gchar *uri, const gchar *label)
{
    GtkWidget *btn = gtk_link_button_new_with_label(uri,
        label != NULL && *label != '\0' ? label : uri);
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
}

static GtkWidget *
editor_section_build(TaskApp *app, const Task *t, gpointer user_data)
{
    (void)user_data;
    /* What Google knows about this task, from the side table.  One read
     * per editor open, which is not a path worth batching.               */
    gchar *web      = gt_text(app->db, "gtasks_task", "task_id",
                              "web_link", t->id);
    gchar *glinks   = gt_text(app->db, "gtasks_task", "task_id",
                              "glinks", t->id);
    gchar *assigned = gt_text(app->db, "gtasks_task", "task_id",
                              "assigned", t->id);

    GString *info = g_string_new(NULL);
    if (t->status == TASK_STATUS_DONE && t->completed_at != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(t->completed_at);
        gchar *when = g_date_time_format(dt, "%b %-e, %Y at %H:%M");
        g_string_append_printf(info, "Completed %s", when);
        g_free(when);
        g_date_time_unref(dt);
    }
    if (assigned != NULL) {
        TaskJson *ai = task_json_parse(assigned, -1);
        const gchar *surface = task_json_str(ai, "surfaceType");
        if (info->len > 0)
            g_string_append_c(info, '\n');
        g_string_append_printf(info, "Assigned task (from %s)",
            g_strcmp0(surface, "DOCUMENT") == 0 ? "Google Docs"
            : g_strcmp0(surface, "SPACE") == 0  ? "Google Chat"
                                                : "Google Workspace");
        task_json_free(ai);
    }

    if (info->len == 0 && web == NULL && glinks == NULL) {
        g_string_free(info, TRUE);
        g_free(web);
        g_free(glinks);
        g_free(assigned);
        return NULL;                 /* nothing Google knows about this   */
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<b>From Google</b>");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

    if (info->len > 0) {
        GtkWidget *lbl = gtk_label_new(info->str);
        gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        host->ui->widget_add_css(lbl, "label { font-size: 85%; }");
        gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    }
    g_string_free(info, TRUE);

    GtkWidget *links = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (glinks != NULL) {
        TaskJson *arr = task_json_parse(glinks, -1);
        for (guint i = 0; i < task_json_len(arr); i++) {
            TaskJson *lk = task_json_at(arr, i);
            const gchar *uri = task_json_str(lk, "link");
            if (uri != NULL)
                link_button(links, uri, task_json_str(lk, "description"));
        }
        task_json_free(arr);
    }
    if (web != NULL)
        link_button(links, web, "Open in Google Tasks");
    gtk_box_pack_start(GTK_BOX(box), links, FALSE, FALSE, 0);
    g_free(web);
    g_free(glinks);
    g_free(assigned);
    return box;
}

static const TaskUiEditorDef editor_section = {
    .id    = "gtasks-from-google",
    .sort  = 10,
    .build = editor_section_build,
};

/* ---------------------------------------------------------------------------
 * "Open in Google Tasks" — the task context-menu item.
 *
 * Single selection only, and only for a task that HAS a remote copy:
 * there is no meaningful page to open for a task Google has never seen,
 * and "open" over a multi-selection would mean N browser tabs.  Both
 * cases grey the item rather than hiding it, so the menu keeps its shape.
 * ------------------------------------------------------------------------- */
static gboolean
ctx_open_enabled(TaskApp *app, GArray *ids, gpointer user_data)
{
    (void)user_data;
    if (ids->len != 1)
        return FALSE;
    gchar *web = gt_text(app->db, "gtasks_task", "task_id", "web_link",
                         g_array_index(ids, gint64, 0));
    gboolean ok = web != NULL;
    g_free(web);
    return ok;
}

static void
ctx_open_activate(TaskApp *app, GArray *ids, gpointer user_data)
{
    (void)user_data;
    gchar *web = gt_text(app->db, "gtasks_task", "task_id", "web_link",
                         g_array_index(ids, gint64, 0));
    if (web == NULL)
        return;
    GError *err = NULL;
    if (!gtk_show_uri_on_window(app->library_window != NULL
                                  ? GTK_WINDOW(app->library_window) : NULL,
                                web, GDK_CURRENT_TIME, &err)) {
        gt_status(app, "Cannot open browser: %s",
                        err != NULL ? err->message : "?");
        g_clear_error(&err);
    }
    g_free(web);
}

static const TaskUiTaskMenuDef ctx_open_item = {
    .id       = "gtasks-open",
    .label    = "Open in Google Tasks",
    .sort     = 10,
    .enabled  = ctx_open_enabled,
    .activate = ctx_open_activate,
};

/* ===========================================================================
 * The Settings section.
 *
 * Contributed through the settings registry (settings_window.h) like any
 * other, which is what lets an integration keep its settings UI whether
 * it is built in or shipped separately.
 *
 * The widgets are NOT cached: the Settings window is destroyed and
 * rebuilt on every open, so a pointer kept between openings is a
 * dangling one.  The three that need re-greying are carried on the
 * section's own box as object data and found again from there.
 * =========================================================================== */

#define GT_SET_STATE   "gtasks-state-label"
#define GT_SET_SIGNIN  "gtasks-signin"
#define GT_SET_SIGNOUT "gtasks-signout"
#define GT_SET_SPIN    "gtasks-interval"
#define GT_SET_TOOLBAR "gtasks-toolbar-check"

/* settings_state_refresh() — reflect the master switch and the sign-in
 * state: with the switch off, Sign In / Sign Out / the interval and the
 * toolbar preference all grey out, because none of them does anything.  */
static void
settings_state_refresh(GtkWidget *box)
{
    gboolean enabled = host->config->get_bool(self, "sync_enabled", TRUE);
    gboolean in      = task_oauth_authenticated();
    GtkWidget *state    = g_object_get_data(G_OBJECT(box), GT_SET_STATE);
    GtkWidget *signin   = g_object_get_data(G_OBJECT(box), GT_SET_SIGNIN);
    GtkWidget *signout  = g_object_get_data(G_OBJECT(box), GT_SET_SIGNOUT);
    GtkWidget *spin     = g_object_get_data(G_OBJECT(box), GT_SET_SPIN);
    GtkWidget *tb       = g_object_get_data(G_OBJECT(box), GT_SET_TOOLBAR);
    if (state != NULL)
        gtk_label_set_markup(GTK_LABEL(state),
            !enabled ? "<span foreground=\"#888888\">Sync disabled</span>"
            : in     ? "<span foreground=\"#26a269\">Signed in</span>"
                     : "<span foreground=\"#888888\">Not signed in</span>");
    if (signout != NULL)
        gtk_widget_set_sensitive(signout, enabled && in);
    if (signin != NULL)
        gtk_widget_set_sensitive(signin,
                                 enabled && task_oauth_have_client() && !in);
    if (spin != NULL)
        gtk_widget_set_sensitive(spin, enabled);
    if (tb != NULL)
        gtk_widget_set_sensitive(tb, enabled);
}

/* The section's box, so a handler can find its siblings.                  */
#define GT_SET_BOX "gtasks-section-box"

static void
on_set_enabled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    GtkWidget *box = g_object_get_data(G_OBJECT(w), GT_SET_BOX);
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    host->config->set(self, "sync_enabled", on ? "1" : "0");
    settings_state_refresh(box);
    task_sync_auto_start(app, app->db->path);
    /* Full notify: the toolbar button's visibility follows this.         */
    host->notify->notify_changed(app);
    gt_status(app, on ? "Google Tasks sync enabled"
                            : "Google Tasks sync disabled");
}

static void
on_set_interval(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    gchar *v = g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)));
    host->config->set(self, "interval_min", v);
    g_free(v);
    task_sync_auto_start(app, app->db->path);
}

static void
on_set_toolbar(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "toolbar_button",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    host->notify->notify_changed(app);
}

/* settings_signin_done() — completion of the browser flow.
 *
 * The Settings window may have CLOSED mid-flow, taking the section's
 * widgets with it; the box is held as a weak pointer so it reads NULL
 * rather than dangling, and the refresh is simply skipped.              */
typedef struct { TaskApp *app; GtkWidget *box; } SetSignin;

static void
settings_signin_done(gboolean ok, const gchar *error, gpointer data)
{
    SetSignin *j = data;
    if (j->box != NULL)
        settings_state_refresh(j->box);
    if (ok)
        gt_status(j->app, "Signed in to Google");
    task_sync_signin_done(j->app,
                          j->app->library_window != NULL
                            ? GTK_WINDOW(j->app->library_window) : NULL,
                          j->app->db->path, ok, error, NULL);
    if (j->box != NULL)
        g_object_remove_weak_pointer(G_OBJECT(j->box), (gpointer *)&j->box);
    g_free(j);
}

static void
on_set_signin(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    SetSignin *j = g_new0(SetSignin, 1);
    j->app = app;
    j->box = g_object_get_data(G_OBJECT(w), GT_SET_BOX);
    if (j->box != NULL)
        g_object_add_weak_pointer(G_OBJECT(j->box), (gpointer *)&j->box);
    gt_status(app, "Opening browser for Google sign-in\xe2\x80\xa6");
    task_oauth_begin(GTK_WINDOW(gtk_widget_get_toplevel(w)),
                     settings_signin_done, j);
}

static void
on_set_signout(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    task_oauth_signout();
    settings_state_refresh(g_object_get_data(G_OBJECT(w), GT_SET_BOX));
    gt_status(app, "Signed out \xe2\x80\x94 the stored sign-in "
                    "was removed and syncing stopped");
}

static void
gtasks_settings(TaskApp *app, GtkWidget *column, GtkWindow *window,
                gpointer user_data)
{
    (void)window;
    (void)user_data;

    /* Everything goes in one box so the handlers can find each other
     * through it; the column itself is shared with every other section. */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(column), box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       host->settings->heading("Google Tasks"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), host->settings->note(
        "Two-way non-destructive sync with Google Tasks.  Sign in will "
        "open a browser window for authentication; Sign out will remove "
        "the local stored token."), FALSE, FALSE, 0);

    GtkWidget *check = gtk_check_button_new_with_label(
        "Enable Google Tasks sync");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
        host->config->get_bool(self, "sync_enabled", TRUE));
    g_object_set_data(G_OBJECT(check), GT_SET_BOX, box);
    g_signal_connect(check, "toggled", G_CALLBACK(on_set_enabled), app);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *signin = gtk_button_new_with_label(
        "Sign In to Google\xe2\x80\xa6");
    g_object_set_data(G_OBJECT(signin), GT_SET_BOX, box);
    g_signal_connect(signin, "clicked", G_CALLBACK(on_set_signin), app);
    gtk_box_pack_start(GTK_BOX(row), signin, FALSE, FALSE, 0);
    GtkWidget *signout = gtk_button_new_with_label("Sign Out");
    g_object_set_data(G_OBJECT(signout), GT_SET_BOX, box);
    g_signal_connect(signout, "clicked", G_CALLBACK(on_set_signout), app);
    gtk_box_pack_start(GTK_BOX(row), signout, FALSE, FALSE, 0);
    GtkWidget *state = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(row), state, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *iv = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(iv), gtk_label_new("Auto-sync every"),
                       FALSE, FALSE, 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(spin,
        "Minutes between automatic syncs while signed in; 0 disables "
        "the timer (the Sync button always works)");
    gchar *cur = host->config->get(self, "interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                              cur != NULL ? g_ascii_strtod(cur, NULL) : 5);
    g_free(cur);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_set_interval),
                     app);
    gtk_box_pack_start(GTK_BOX(iv), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(iv), gtk_label_new("minutes (0 = off)"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), iv, FALSE, FALSE, 0);

    GtkWidget *tb = gtk_check_button_new_with_label(
        "Show Sync button in toolbar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb),
        host->config->get_bool(self, "toolbar_button", TRUE));
    g_object_set_data(G_OBJECT(tb), GT_SET_BOX, box);
    g_signal_connect(tb, "toggled", G_CALLBACK(on_set_toolbar), app);
    gtk_box_pack_start(GTK_BOX(box), tb, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(box), GT_SET_STATE,   state);
    g_object_set_data(G_OBJECT(box), GT_SET_SIGNIN,  signin);
    g_object_set_data(G_OBJECT(box), GT_SET_SIGNOUT, signout);
    g_object_set_data(G_OBJECT(box), GT_SET_SPIN,    spin);
    g_object_set_data(G_OBJECT(box), GT_SET_TOOLBAR, tb);
    settings_state_refresh(box);
}

/* ---------------------------------------------------------------------------
 * gtasks_init() — the plugin's init hook: register the worker, the UI
 * contributions and the core-operation hooks.  Cheap by design; it runs
 * before the window is shown (see plugin.h).
 * ------------------------------------------------------------------------- */
static gboolean
gtasks_init(TaskApp *app, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    /* Credentials are snapshotted before any worker thread can exist —
     * the same ordering main() used to guarantee.                       */
    task_oauth_init();
    sync_worker_live         = sync_worker;
    sync_worker_live.running = &gt_running;
    sync_worker_live.timer   = &gt_timer;
    host->worker->register_worker(&sync_worker_live);

    host->ui->add_tool(&sync_tool);
    host->ui->add_task_menu_item(&ctx_open_item);
    host->ui->add_editor_section(&editor_section);
    host->settings->add_section(gtasks_settings, NULL);
    host->ops->add_moved_hook(gtasks_task_moved, NULL);
    host->ops->add_cleared_hook(gtasks_completed_cleared, NULL);
    host->ops->add_list_veto(gtasks_list_veto, NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * gtasks_db_open() — create this plugin's own tables.
 *
 * Called for the main connection at startup and again whenever the
 * database changes identity.  IF NOT EXISTS because a database migrated
 * from schema v7 already has them, and the worker's own connection opens
 * the same file.
 * ------------------------------------------------------------------------- */
static void
gtasks_db_open(TaskApp *app, TaskDatabase *db, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS gtasks_list ("
        "  list_id   INTEGER PRIMARY KEY REFERENCES lists(id)"
        "            ON DELETE CASCADE,"
        "  gtasks_id TEXT)");
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS gtasks_task ("
        "  task_id   INTEGER PRIMARY KEY REFERENCES tasks(id)"
        "            ON DELETE CASCADE,"
        "  gtasks_id TEXT,"
        "  etag      TEXT,"
        "  web_link  TEXT,"
        "  glinks    TEXT,"
        "  assigned  TEXT)");
}

static const TaskPlugin gtasks_plugin = {
    .abi_version     = TASK_PLUGIN_ABI_VERSION,
    .abi_revision    = TASK_PLUGIN_ABI_REVISION,
    .id              = "gtasks",
    .name            = "Google Tasks Sync",
    .description     = "Two-way non-destructive sync with Google Tasks.",
    .version         = "1.0.0",
    .enabled_default = TRUE,
    .init            = gtasks_init,
    .db_open         = gtasks_db_open,
};

TASK_PLUGIN_EXPORT const TaskPlugin *
task_plugin_entry(const TaskHostApi *api)
{
    host = api;
    self = &gtasks_plugin;
    /* libcurl's implicit global init is not thread-safe, and this plugin
     * is the only thing in the process that uses it.  Doing it here, from
     * the entry point, is before any worker of ours can exist.          */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return &gtasks_plugin;
}
