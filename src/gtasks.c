/* ===========================================================================
 * gtasks.c — two-way Google Tasks sync (see gtasks.h)
 * =========================================================================== */

#include "gtasks.h"
#include "oauth.h"
#include "http.h"
#include "json.h"
#include "task_ops.h"                /* the hooks the remote half rides on  */
#include "task_worker.h"             /* the shared periodic-pass scheduler  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTASKS_API "https://tasks.googleapis.com/tasks/v1"

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
    return task_due_from_ymd(y, m, d);
}

/* due_to_rfc3339() — local midnight unix → "YYYY-MM-DDT00:00:00.000Z"
 * (the calendar date in LOCAL time — matching due_from_rfc3339).
 * Returns NULL for due == 0 (caller emits JSON null to clear the date).    */
static gchar *
due_to_rfc3339(gint64 due)
{
    if (due == 0)
        return NULL;
    gchar *date = task_due_format_iso(due);
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
        task_db_state_set(db, "default_list_gid",
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
    gchar *default_gid = task_db_state_get(db, "default_list_gid");

    GPtrArray *remote = fetch_remote_lists(token, err);
    if (remote == NULL) {
        g_free(default_gid);
        return FALSE;
    }
    GPtrArray *local = task_db_lists(db, TRUE);
    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        TaskList *l = g_ptr_array_index(local, i);

        /* Find the remote row this local one is bound to.                  */
        RemoteList *match = NULL;
        if (l->gtasks_id != NULL) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteList *r = g_ptr_array_index(remote, j);
                if (strcmp(r->gid, l->gtasks_id) == 0) {
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
                    task_db_list_set_gtasks_id(db, l->id, r->gid);
                    g_free(l->gtasks_id);
                    l->gtasks_id = g_strdup(r->gid);
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
            if (l->gtasks_id != NULL && default_gid != NULL &&
                strcmp(l->gtasks_id, default_gid) == 0) {
                task_db_list_restore(db, l->id);
                post_status(app, "Google's default list cannot be "
                            "deleted \xe2\x80\x94 restored");
                ListPair p = { l->id, g_strdup(l->gtasks_id) };
                g_array_append_val(pairs, p);
                continue;
            }
            /* Local tombstone: propagate, then purge.                      */
            if (l->gtasks_id != NULL) {
                gchar *url = tasklist_url(l->gtasks_id);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                task_db_list_purge(db, l->id);
                stats->deleted++;
            }
            continue;
        }

        if (l->gtasks_id == NULL || match == NULL) {
            /* Local new — or its bound remote list vanished without a
             * local tombstone.  NON-DESTRUCTIVE: absence never deletes;
             * the list exists here, so (re-)create it remotely and
             * adopt the new id.  On a re-create the list's tasks drop
             * their stale Google identities too, so the task pass
             * pushes every one of them as a new remote task.               */
            gboolean rebind = l->gtasks_id != NULL;
            gchar *body = list_body(l->name);
            TaskJson *reply = NULL;
            gchar *url = tasklist_url(NULL);
            ok = api_call("POST", url, token, NULL, body, &reply, err);
            g_free(url);
            if (ok && task_json_str(reply, "id") != NULL) {
                if (rebind)
                    task_db_tasks_clear_gtasks_ids(db, l->id);
                task_db_list_set_gtasks_id(db, l->id,
                                           task_json_str(reply, "id"));
                task_db_list_apply_remote(db, l->id, l->name,
                    remote_updated_of(reply, l->updated_at));
                g_free(l->gtasks_id);
                l->gtasks_id = g_strdup(task_json_str(reply, "id"));
                stats->pushed++;
            }
            task_json_free(reply);
            g_free(body);
        } else if (strcmp(match->title, l->name) != 0) {
            /* Both exist, names differ: newer side wins.                   */
            gboolean local_dirty = l->updated_at > last_sync;
            if (local_dirty && l->updated_at >= match->updated) {
                gchar *body = list_body(l->name);
                gchar *url = tasklist_url(l->gtasks_id);
                ok = api_call("PATCH", url, token, NULL, body, NULL, err);
                if (ok)
                    stats->pushed++;
                g_free(url);
                g_free(body);
            } else {
                task_db_list_apply_remote(db, l->id, match->title,
                                          match->updated);
                stats->pulled++;
            }
        }

        if (ok && l->gtasks_id != NULL) {
            ListPair p = { l->id, g_strdup(l->gtasks_id) };
            g_array_append_val(pairs, p);
        }
    }

    /* Remote lists nobody local claimed: new on the Google side.           */
    for (guint j = 0; j < remote->len && ok; j++) {
        RemoteList *r = g_ptr_array_index(remote, j);
        if (r->matched)
            continue;
        gint64 id = task_db_list_create(db, r->title, "");
        if (id != 0) {
            task_db_list_set_gtasks_id(db, id, r->gid);
            task_db_list_apply_remote(db, id, r->title, r->updated);
            ListPair p = { id, g_strdup(r->gid) };
            g_array_append_val(pairs, p);
            stats->pulled++;
        }
    }

    /* Google's undeletable DEFAULT list always wears a 🔴 indicator:
     * seeded only while the emoji is empty, so a user's later Edit List
     * choice sticks (clearing it brings the dot back next sync).           */
    if (ok && default_gid != NULL)
        task_db_list_emoji_if_empty(db, default_gid, "\xf0\x9f\x94\xb4");

    for (guint i = 0; i < remote->len; i++)
        remote_list_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    task_ptr_array_free_lists(local);
    g_free(default_gid);
    return ok;
}

/* stamp_clean() — after a successful create/patch, write the local row
 * back clean: the reply's updated time, etag and webViewLink (keeping
 * the stored link when the reply omits it).  Shallow overlay — nothing
 * in *t is modified or freed.                                              */
static void
stamp_clean(TaskDatabase *db, const Task *t, TaskJson *reply,
            SyncStats *stats)
{
    Task clean = *t;
    clean.updated_at = remote_updated_of(reply, t->updated_at);
    clean.etag       = (gchar *)task_json_str(reply, "etag");
    clean.web_link   = task_json_str(reply, "webViewLink") != NULL
                       ? (gchar *)task_json_str(reply, "webViewLink")
                       : t->web_link;
    task_db_task_apply_remote(db, &clean);
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
        task_db_task_set_gtasks_id(db, t->id, task_json_str(reply, "id"));
        g_free(t->gtasks_id);
        t->gtasks_id = g_strdup(task_json_str(reply, "id"));
        stamp_clean(db, t, reply, stats);
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
                const Task *t, SyncStats *stats, gchar **err)
{
    gchar *body = task_body(t);
    gchar *url = task_url(list_gid, t->gtasks_id);
    TaskJson *reply = NULL;
    gboolean ok = api_call("PATCH", url, token, t->etag, body,
                           &reply, err);
    if (ok) {
        stamp_clean(db, t, reply, stats);
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
            Task *t, GHashTable *local_by_gid, SyncStats *stats,
            gchar **err)
{
    Task *p = t->parent_id != 0 ? task_db_task_get(db, t->parent_id)
                                  : NULL;
    gboolean ok = push_task_create(db, token, list_gid, t,
                                   p != NULL ? p->gtasks_id : NULL,
                                   stats, err);
    task_free(p);                 /* owns parent_gid until after push    */
    if (t->gtasks_id != NULL)
        g_hash_table_insert(local_by_gid, t->gtasks_id, t);
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
    GPtrArray *local = task_db_tasks_in_list_all(db, list_id);

    /* gid → RemoteTask and gid → local Task maps for the match passes.    */
    GHashTable *by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < remote->len; i++) {
        RemoteTask *r = g_ptr_array_index(remote, i);
        g_hash_table_insert(by_gid, r->gid, r);
    }
    GHashTable *local_by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < local->len; i++) {
        Task *t = g_ptr_array_index(local, i);
        if (t->gtasks_id != NULL)
            g_hash_table_insert(local_by_gid, t->gtasks_id, t);
    }

    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        Task *t = g_ptr_array_index(local, i);

        RemoteTask *match = t->gtasks_id != NULL
            ? g_hash_table_lookup(by_gid, t->gtasks_id) : NULL;

        /* First-sync dedup: adopt an unmatched live remote task with the
         * same title (top-level against top-level only — subtask titles
         * repeat too easily across parents to guess).  Only meaningful
         * against a FULL listing.                                          */
        if (full_listing &&
            match == NULL && t->gtasks_id == NULL && !t->deleted &&
            t->parent_id == 0) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteTask *r = g_ptr_array_index(remote, j);
                if (!r->matched && !r->deleted && r->parent_gid == NULL &&
                    strcmp(r->title, t->title) == 0) {
                    match = r;
                    task_db_task_set_gtasks_id(db, t->id, r->gid);
                    g_free(t->gtasks_id);
                    t->gtasks_id = g_strdup(r->gid);
                    g_hash_table_insert(local_by_gid, t->gtasks_id, t);
                    break;
                }
            }
        }
        if (match != NULL)
            match->matched = TRUE;

        if (t->deleted) {
            /* Local tombstone: propagate, then purge.                      */
            if (t->gtasks_id != NULL &&
                (match == NULL || !match->deleted)) {
                gchar *url = task_url(list_gid, t->gtasks_id);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                task_db_task_purge(db, t->id);
                stats->deleted++;
            }
            continue;
        }

        if (t->gtasks_id == NULL) {
            /* Local new.                                                   */
            ok = push_as_new(db, token, list_gid, t, local_by_gid,
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
                g_hash_table_remove(local_by_gid, t->gtasks_id);
                task_db_task_set_gtasks_id(db, t->id, NULL);
                g_clear_pointer(&t->gtasks_id, g_free);
                ok = push_as_new(db, token, list_gid, t, local_by_gid,
                                 stats, err);
            } else if (t->updated_at > last_sync) {
                /* Incremental listing: absent just means unchanged — but
                 * the LOCAL side is dirty, so push (etag-guarded).         */
                ok = push_task_patch(db, token, list_gid, t, stats, err);
            }
            continue;
        }
        if (match->deleted) {
            task_db_task_purge(db, t->id);
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
            gboolean meta_stale =
                (t->etag == NULL && match->etag != NULL) ||
                (t->web_link == NULL && match->web_link != NULL);
            if (match->updated > t->updated_at || meta_stale) {
                Task apply = *t;
                apply.updated_at   = match->updated;
                apply.completed_at = match->completed;
                apply.etag         = match->etag;
                apply.web_link     = match->web_link;
                apply.glinks       = match->glinks;
                apply.assigned     = match->assigned;
                task_db_task_apply_remote(db, &apply);
            }
            continue;
        }
        gboolean local_dirty = t->updated_at > last_sync;
        if (local_dirty && t->updated_at >= match->updated) {
            ok = push_task_patch(db, token, list_gid, t, stats, err);
        } else {
            Task apply = *t;       /* shallow copy is fine here           */
            apply.title        = match->title;
            apply.notes        = match->notes;
            apply.due          = match->due;
            /* Google reports done-ness only, so fold it onto the status
             * the row already holds: a remote un-tick lands on In
             * Progress, and a still-unfinished New task stays New.         */
            apply.status       = task_status_apply_done(t->status,
                                                        match->done);
            apply.updated_at   = match->updated;
            apply.completed_at = match->completed;
            apply.etag         = match->etag;
            apply.web_link     = match->web_link;
            apply.glinks       = match->glinks;
            apply.assigned     = match->assigned;
            task_db_task_apply_remote(db, &apply);
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
            gint64 id = task_db_task_create(db, list_id, parent_id,
                                            r->title);
            if (id == 0)
                continue;
            task_db_task_set_gtasks_id(db, id, r->gid);
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
            nt.etag         = r->etag;
            nt.web_link     = r->web_link;
            nt.glinks       = r->glinks;
            nt.assigned     = r->assigned;
            task_db_task_apply_remote(db, &nt);
            r->matched = TRUE;
            stats->pulled++;
            if (!is_child) {
                /* Make the new row findable for pass 2's children.         */
                Task *row = task_db_task_get(db, id);
                if (row != NULL) {
                    g_hash_table_insert(local_by_gid, row->gtasks_id, row);
                    g_ptr_array_add(local, row);   /* owned by `local`      */
                }
            }
        }
    }

    g_hash_table_destroy(by_gid);
    g_hash_table_destroy(local_by_gid);
    task_ptr_array_free_tasks(local);
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
    task_app_status(sp->app, "%s", sp->msg);
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
    job->app->sync_running = FALSE;
    task_app_notify_changed(job->app);
    task_app_status(job->app, "%s", job->message);
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
    TaskDatabase *db = task_db_open(job->db_path, &gerr);
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
    gchar *ls = task_db_state_get(db, "last_sync");
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
        task_db_state_set(db, "last_sync", stamp);
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
    task_db_close(db);
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
    if (!task_app_config_get_bool("google_sync_enabled", TRUE)) {
        task_app_status(app, "Google Tasks sync is disabled \xe2\x80\x94 "
                        "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "sync disabled", user_data);
        return;
    }
    if (app->sync_running) {
        task_app_status(app, "Sync already running");
        return;
    }
    if (!task_oauth_authenticated()) {
        task_app_status(app, "Not signed in to Google \xe2\x80\x94 use the "
                        "Sync button or File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "not signed in", user_data);
        return;
    }
    app->sync_running = TRUE;
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
        task_app_notice(parent, GTK_MESSAGE_ERROR,
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
    task_db_insert_remote_tombstone(app->db, job->src_list_id,
                                    job->task_gid);
    task_db_task_set_gtasks_id(app->db, job->task_id, NULL);
    GPtrArray *subs = task_db_subtasks(app->db, job->task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);
        if (s->gtasks_id != NULL) {
            task_db_insert_remote_tombstone(app->db, job->src_list_id,
                                            s->gtasks_id);
            task_db_task_set_gtasks_id(app->db, s->id, NULL);
        }
    }
    task_ptr_array_free_tasks(subs);
}

/* move_apply() — main-thread completion of the remote move.                */
static gboolean
move_apply(gpointer data)
{
    MoveJob *job = data;
    if (!job->ok) {
        move_fallback(job->app, job);
        task_app_status(job->app, "Move will finish on the next sync (%s)",
                        job->error != NULL ? job->error : "remote move failed");
    } else {
        task_app_status(job->app, "Moved in Google Tasks");
    }
    task_app_notify_changed(job->app);
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
    Task *t = task_db_task_get(app->db, task_id);
    if (t == NULL)
        return;                      /* deleted between the write and here  */

    TaskList *src  = task_db_list_get(app->db, from_list);
    TaskList *dest = task_db_list_get(app->db, to_list);

    MoveJob *job = g_new0(MoveJob, 1);
    job->app         = app;
    job->task_id     = task_id;
    job->src_list_id = from_list;
    job->src_gid     = src != NULL ? g_strdup(src->gtasks_id) : NULL;
    job->dest_gid    = dest != NULL ? g_strdup(dest->gtasks_id) : NULL;
    job->task_gid    = g_strdup(t->gtasks_id);
    job->child_gids  = g_ptr_array_new();
    GPtrArray *subs = task_db_subtasks(app->db, task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);
        if (s->gtasks_id != NULL)
            g_ptr_array_add(job->child_gids, g_strdup(s->gtasks_id));
    }
    task_ptr_array_free_tasks(subs);
    task_list_free(src);
    task_list_free(dest);
    task_free(t);

    if (job->task_gid != NULL && job->src_gid != NULL &&
        job->dest_gid != NULL && task_oauth_authenticated()) {
        GThread *th = g_thread_new("task-move", move_thread, job);
        g_thread_unref(th);
    } else {
        move_fallback(app, job);     /* offline / unsynced endpoints        */
        task_app_notify_changed(app);
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
            task_db_task_purge(job->app->db,
                               g_array_index(job->ids, gint64, i));
        task_app_status(job->app, "Completed tasks cleared in Google Tasks");
    } else {
        task_app_status(job->app, "Cleared locally; Google will catch up "
                        "on the next sync (%s)",
                        job->error != NULL ? job->error : "clear failed");
    }
    task_app_notify_changed(job->app);
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

    TaskList *l = task_db_list_get(app->db, list_id);
    if (l == NULL)
        return;
    if (l->gtasks_id != NULL && task_oauth_authenticated()) {
        ClearJob *job = g_new0(ClearJob, 1);
        job->app      = app;
        job->list_id  = list_id;
        job->list_gid = g_strdup(l->gtasks_id);
        /* Own copy: the event's array is borrowed for the call only.      */
        job->ids      = g_array_sized_new(FALSE, FALSE, sizeof(gint64),
                                          task_ids->len);
        g_array_append_vals(job->ids, task_ids->data, task_ids->len);
        GThread *th = g_thread_new("task-clear", clear_thread, job);
        g_thread_unref(th);
    }
    task_list_free(l);
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

static const TaskWorkerDef sync_worker = {
    .id               = "gtasks",
    .enabled_key      = "google_sync_enabled",
    .enabled_default  = TRUE,
    .interval_key     = "sync_interval_min",
    .interval_default = 5,
    .initial          = TASK_WORKER_INITIAL_ARMED,
    .running          = NULL,        /* filled in by task_gtasks_init       */
    .timer            = NULL,
    .run              = sync_run,
    .ready            = sync_ready,
    .on_arm           = NULL,
};

/* The def carries POINTERS to the app's own flag and GSource id, which
 * are not known until an app exists — so the static above is completed
 * once, at registration.                                                  */
static TaskWorkerDef sync_worker_live;

/* task_sync_auto_start() — (re)arm the auto-sync timer (see gtasks.h).     */
void
task_sync_auto_start(TaskApp *app, const gchar *db_path)
{
    task_worker_arm(app, &sync_worker_live, db_path);
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
    if (list->gtasks_id == NULL)
        return TRUE;
    gchar *default_gid = task_db_state_get(app->db, "default_list_gid");
    gboolean ok = default_gid == NULL ||
                  strcmp(list->gtasks_id, default_gid) != 0;
    if (!ok && why != NULL)
        *why = g_strdup_printf("\xe2\x80\x9c%s\xe2\x80\x9d is Google's "
                               "default list and cannot be deleted",
                               list->name);
    g_free(default_gid);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_gtasks_init() — register the sync engine's hooks (see gtasks.h).
 * ------------------------------------------------------------------------- */
void
task_gtasks_init(TaskApp *app)
{
    sync_worker_live         = sync_worker;
    sync_worker_live.running = &app->sync_running;
    sync_worker_live.timer   = &app->sync_timer;
    task_worker_register(&sync_worker_live);

    task_ops_add_moved_hook(gtasks_task_moved, NULL);
    task_ops_add_cleared_hook(gtasks_completed_cleared, NULL);
    task_ops_add_list_veto(gtasks_list_veto, NULL);
}
