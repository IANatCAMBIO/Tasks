/* ===========================================================================
 * bnsync.c — Notes action-item mirror (see bnsync.h)
 * =========================================================================== */

#include "bnsync.h"
#include "bnotes.h"
#include "task_ops.h"                /* cross-list moves are a core op       */
#include "task_worker.h"             /* the shared periodic-pass scheduler   */
#include "task_view.h"
#include "task_rows.h"               /* the "Action Items" sidebar view      */
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The list mirrored items land in when Settings names none.  Created on
 * first use; the emoji marks it the way the sync marks Google's default
 * list.                                                                    */
#define BN_LIST_NAME  "Action Items"
#define BN_LIST_EMOJI "\xe2\x9d\x97"          /* ❗                          */

/* ===========================================================================
 * The side tables (schema v9).
 *
 * A mirrored task's Notes identity — its stable uid — and the BASELINE of
 * what Notes was last known to hold live in notes_task, keyed by task id.
 * notes_deleted holds uids whose task was deleted here, suppressing the
 * re-create.  None of it is on the core rows any more.
 *
 * The baseline is what makes the write-back a bulk diff rather than a
 * queue: a row whose done-ness or due differs from it IS pending, which
 * survives a crash and cannot drift out of step with the task.
 * =========================================================================== */

/* collect_i64() — exec_query callback appending the first column to a
 * GArray of gint64.                                                      */
static gint
collect_i64(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    GArray *out = data;
    if (n_cols > 0 && values[0] != NULL) {
        gint64 v = g_ascii_strtoll(values[0], NULL, 10);
        g_array_append_val(out, v);
    }
    return 0;
}

/* bn_tasks_for() — run an id-yielding query and load those tasks.
 *
 * Two steps rather than one join returning task columns: the row shape
 * belongs to db.c and is not the plugin's to reproduce.  The id list is
 * one query; the loads are by primary key.                               */
static GPtrArray *
bn_tasks_for(TaskDatabase *db, const gchar *id_sql)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    task_db_exec_query(db, id_sql, collect_i64, ids);
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < ids->len; i++) {
        Task *t = task_db_task_get(db, g_array_index(ids, gint64, i));
        if (t != NULL)
            g_ptr_array_add(out, t);
    }
    g_array_free(ids, TRUE);
    return out;
}

/* bn_uid_of() — a task's Notes uid, or 0.                                 */
static gint64
bn_uid_of(TaskDatabase *db, gint64 task_id)
{
    gchar *sql = g_strdup_printf(
        "SELECT uid FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    gint64 uid = task_db_scalar(db, sql);
    g_free(sql);
    return uid > 0 ? uid : 0;
}

/* bn_task_for_uid() — the visible mirror task carrying `uid`, or NULL.   */
static Task *
bn_task_for_uid(TaskDatabase *db, gint64 uid)
{
    gchar *sql = g_strdup_printf(
        "SELECT n.task_id FROM notes_task n JOIN tasks t ON t.id = n.task_id"
        " WHERE n.uid = %" G_GINT64_FORMAT " AND t.deleted = 0 LIMIT 1",
        uid);
    gint64 id = task_db_scalar(db, sql);
    g_free(sql);
    return id > 0 ? task_db_task_get(db, id) : NULL;
}

/* bn_baseline() — what Notes was last known to hold for this task.       */
static void
bn_baseline(TaskDatabase *db, gint64 task_id, gboolean *done, gint64 *due)
{
    gchar *sql = g_strdup_printf(
        "SELECT done FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    *done = task_db_scalar(db, sql) > 0;
    g_free(sql);
    sql = g_strdup_printf(
        "SELECT due FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    gint64 d = task_db_scalar(db, sql);
    *due = d > 0 ? d : 0;
    g_free(sql);
}

/* bn_set() — bind a task to a uid and record the baseline.  Deliberately
 * does NOT stamp updated_at: the binding is local bookkeeping, not a
 * change to the task.                                                    */
static void
bn_set(TaskDatabase *db, gint64 task_id, gint64 uid, gboolean done,
       gint64 due)
{
    gchar *sql = g_strdup_printf(
        "INSERT INTO notes_task (task_id, uid, done, due)"
        " VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %d,"
        "         %" G_GINT64_FORMAT ")"
        " ON CONFLICT(task_id) DO UPDATE SET uid = excluded.uid,"
        "   done = excluded.done, due = excluded.due",
        task_id, uid, done ? 1 : 0, due);
    task_db_exec_sql(db, sql);
    g_free(sql);
}

/* bn_mirror_tasks() — every visible mirrored task.                       */
static GPtrArray *
bn_mirror_tasks(TaskDatabase *db)
{
    return bn_tasks_for(db,
        "SELECT n.task_id FROM notes_task n JOIN tasks t ON t.id = n.task_id"
        " WHERE t.deleted = 0"
        " ORDER BY t.priority DESC, t.list_id, t.position, t.id");
}

/* bn_suppressed() — the uid set whose tasks were deleted here.           */
/* bn_suppressed() — the uid set whose tasks were deleted here.
 *
 * Keyed by DIRECT POINTER (GSIZE_TO_POINTER), matching the `present` set
 * it is compared against in reap_missing.  Two sets of the same thing
 * keyed differently is a wild dereference waiting to happen: a
 * g_int64_hash table probed with GSIZE_TO_POINTER reads the uid AS an
 * address.  uids are small positive integers, so the direct-pointer form
 * is exact on any platform this builds for.                             */
static gint
collect_uid_direct(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    GHashTable *set = data;
    if (n_cols > 0 && values[0] != NULL)
        g_hash_table_add(set,
            GSIZE_TO_POINTER((gsize)g_ascii_strtoll(values[0], NULL, 10)));
    return 0;
}

static GHashTable *
bn_suppressed(TaskDatabase *db)
{
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    task_db_exec_query(db, "SELECT uid FROM notes_deleted",
                       collect_uid_direct, set);
    return set;
}

static void
bn_forget(TaskDatabase *db, gint64 uid)
{
    gchar *sql = g_strdup_printf(
        "DELETE FROM notes_deleted WHERE uid = %" G_GINT64_FORMAT, uid);
    task_db_exec_sql(db, sql);
    g_free(sql);
}

/* ---------------------------------------------------------------------------
 * bn_delete_hook() — contribute the mirror's half of a task delete (see
 * task_db_add_delete_hook in db.h).
 *
 * Notes has no CLI verb that deletes an action item, so the item
 * survives there after its mirror task is deleted here.  Parking the uid
 * in bn_deleted is what stops the very next pass from seeing a uid with
 * no task and helpfully re-creating the row the user just deleted;
 * reap_missing() drops the suppression once the item leaves Notes for
 * real.  Subtasks never carry a uid (Notes has no subtasks), so only the
 * task's own row is consulted.
 * ------------------------------------------------------------------------- */
static void
bn_delete_hook(TaskDatabase *db, gint64 task_id, GString *sql,
               gpointer user_data)
{
    (void)db;                        /* hooks contribute SQL, never run it  */
    (void)user_data;
    /* Reads the uid from the side table now (schema v9).  Still spliced
     * into the delete's own transaction, which is the point: a
     * suppression that commits without its delete, or a delete without
     * its suppression, are both worse than neither.                      */
    g_string_append_printf(sql,
        "INSERT OR IGNORE INTO notes_deleted (uid)"
        "  SELECT uid FROM notes_task WHERE task_id = %" G_GINT64_FORMAT ";",
        task_id);
}

/* ---------------------------------------------------------------------------
 * One mirror pass in flight.  Built on the main thread, handed to the
 * worker, freed by the completion callback.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp          *app;
    gchar          *db_path;         /* worker opens its OWN connection     */
    gint64          configured_list; /* Settings choice; 0 = auto           */
    TaskBnSyncDoneFn  done;
    gpointer        user_data;

    gboolean        ok;
    gchar          *message;         /* summary or error (owned)            */
    gint            n_created;
    gint            n_updated;
    gint            n_removed;
    gint            n_pushed;
    gint            n_failed;        /* pushes Notes refused              */
} BnJob;

/* ---------------------------------------------------------------------------
 * task_bnsync_target_list() — resolve the destination list (see bnsync.h).
 * ------------------------------------------------------------------------- */
gint64
task_bnsync_target_list(TaskDatabase *db, gint64 configured)
{
    if (configured > 0) {
        TaskList *l = task_db_list_get(db, configured);
        if (l != NULL) {
            gboolean alive = !l->deleted;
            task_list_free(l);
            if (alive)
                return configured;
        }
        /* A deleted or vanished target falls through to the managed
         * list rather than stranding every item (bn_embed_list did the
         * same before the mirror existed).                                 */
    }
    GPtrArray *lists = task_db_lists(db, FALSE);
    gint64 found = 0;                /* the managed list, if it exists      */
    for (guint i = 0; i < lists->len && found == 0; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        if (g_strcmp0(l->name, BN_LIST_NAME) == 0)
            found = l->id;
    }
    task_ptr_array_free_lists(lists);
    if (found != 0)
        return found;
    return task_db_list_create(db, BN_LIST_NAME, BN_LIST_EMOJI);
}

/* ---------------------------------------------------------------------------
 * task_bnsync_reconcile_target() — apply a changed target list to the
 * items already mirrored (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
task_bnsync_reconcile_target(TaskApp *app)
{
    if (!task_app_config_get_bool("notes_sync", FALSE))
        return;
    gchar *v = task_app_config_get("notes_embed_list");
    gint64 configured = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
    g_free(v);

    gint64 target = task_bnsync_target_list(app->db, configured);
    if (target == 0)
        return;                      /* the list could not be created       */

    /* An ABSENT applied-value counts as "not applied yet", not as "same
     * as now": that is the upgrade case, where tasks were mirrored by a
     * build that only honored the setting at creation time and are
     * sitting in the wrong list.                                           */
    gchar *applied_s = task_db_state_get(app->db, "bn_target_list");
    gint64 applied = applied_s != NULL
                   ? g_ascii_strtoll(applied_s, NULL, 10) : 0;
    gboolean known = applied_s != NULL;
    g_free(applied_s);
    if (known && applied == target)
        return;                      /* nothing changed — leave hand moves  */

    GPtrArray *mirror = bn_mirror_tasks(app->db);
    guint moved = 0;                 /* rows that actually changed list     */
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        /* Subtasks travel with their parent; mirror rows are top-level
         * anyway, so this only guards against hand-edited data.            */
        if (task_ops_move_to_list(app, t->id, target))
            moved++;                 /* it declines subtasks and no-op moves */
    }
    task_ptr_array_free_tasks(mirror);

    gchar *stamp = g_strdup_printf("%" G_GINT64_FORMAT, target);
    task_db_state_set(app->db, "bn_target_list", stamp);
    g_free(stamp);

    if (moved > 0) {
        task_app_status(app, "Moved %u action item%s", moved,
                        moved == 1 ? "" : "s");
        task_app_notify_changed(app);
    }
}

/* ---------------------------------------------------------------------------
 * sync_item() — reconcile ONE listed action item with its mirror task.
 * Creates the task when it is new, otherwise pushes any cached local
 * done/due change and then applies whatever Notes holds.  Counts land
 * in `job`.
 * ------------------------------------------------------------------------- */
static void
sync_item(BnJob *job, TaskDatabase *db, const TaskNoteAction *it, gint64 target)
{
    Task *t = bn_task_for_uid(db, it->uid);

    if (t == NULL) {                 /* new item → new mirror task          */
        gint64 id = task_db_task_create(db, target, 0, it->text);
        if (id == 0) {
            job->n_failed++;         /* create failures must not be silent  */
            return;
        }
        task_db_task_apply_done_source(db, id, it->text, it->done, it->due);
        bn_set(db, id, it->uid, it->done, it->due);
        job->n_created++;
        return;
    }

    /* Notes has no third state, so the whole exchange speaks in the
     * DONE-ness of the status: a New ↔ In Progress move is not a
     * pending write and has nothing to push.                              */
    gboolean local_done = t->status == TASK_STATUS_DONE;

    /* The baseline: what Notes was last known to hold for this task.     */
    gboolean base_have_done;
    gint64   base_have_due;
    bn_baseline(db, t->id, &base_have_done, &base_have_due);

    /* The pending-write set: fields that drifted from the baseline since
     * the last successful push.                                            */
    gboolean done_dirty = local_done != base_have_done;
    gboolean due_dirty  = t->due  != base_have_due;
    gboolean done_sent  = FALSE;     /* did Notes accept the push?        */
    gboolean due_sent   = FALSE;

    if (done_dirty) {
        gchar *err = NULL;
        done_sent = task_bnotes_action_set_done(it->uid, local_done, &err);
        if (done_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }
    if (due_dirty) {
        gchar *err = NULL;
        due_sent = task_bnotes_action_set_due(it->uid, t->due, &err);
        if (due_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }

    /* What the task should hold: a local change stands whether or not
     * the push landed (a refused push must never discard the user's
     * edit), otherwise Notes wins.                                        */
    gboolean new_done = done_dirty ? local_done : it->done;
    gint64   new_due  = due_dirty  ? t->due     : it->due;

    /* What Notes now holds: the pushed value only if it was accepted;
     * an unsent change keeps the old baseline so it is retried.            */
    gboolean base_done = done_dirty
                       ? (done_sent ? local_done : base_have_done)
                       : it->done;
    gint64   base_due  = due_dirty
                       ? (due_sent  ? t->due : base_have_due)
                       : it->due;

    gboolean content = g_strcmp0(t->title, it->text) != 0 ||
                       new_done != local_done || new_due != t->due;

    if (content) {
        /* Stamps updated_at, so the change reaches Google too.             */
        task_db_task_apply_done_source(db, t->id, it->text, new_done,
                                       new_due);
        bn_set(db, t->id, it->uid, base_done, base_due);
        job->n_updated++;
    } else if (base_done != base_have_done || base_due != base_have_due) {
        /* Nothing the user can see changed — only the push baseline —
         * so this must NOT stamp updated_at, or every pass would dirty
         * the row and buy a no-op Google PATCH.                            */
        bn_set(db, t->id, it->uid, base_done, base_due);
    }
    task_free(t);
}

/* ---------------------------------------------------------------------------
 * reap_missing() — tombstone mirror tasks whose item has left Notes
 * (Notes is authoritative for existence), and forget suppressions for
 * uids that are gone for good.
 *
 * `present` holds every uid in the listing.  The listing is always FULL
 * — Notes has no incremental form — so absence really does mean gone,
 * unlike the Google pass where a partial listing makes absence
 * meaningless.
 * ------------------------------------------------------------------------- */
static void
reap_missing(BnJob *job, TaskDatabase *db, GHashTable *present,
             GHashTable *suppressed)
{
    GPtrArray *mirror = bn_mirror_tasks(db);
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        if (g_hash_table_contains(present, GSIZE_TO_POINTER(bn_uid_of(db, t->id))))
            continue;
        task_db_task_delete(db, t->id);
        /* task_delete parks the uid in bn_deleted so a live item is not
         * re-created after a local delete; here the item is gone from
         * Notes, so that suppression has nothing left to suppress.       */
        bn_forget(db, bn_uid_of(db, t->id));
        job->n_removed++;
    }
    task_ptr_array_free_tasks(mirror);

    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, suppressed);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(present, key))
            bn_forget(db, (gint64)GPOINTER_TO_SIZE(key));
    }
}

/* ---------------------------------------------------------------------------
 * bn_apply() — main-thread completion: clear the guard, report, refresh.
 * ------------------------------------------------------------------------- */
static gboolean
bn_apply(gpointer data)
{
    BnJob *job = data;
    job->app->bn_sync_running = FALSE;
    if (job->message != NULL)
        task_app_status(job->app, "%s", job->message);
    /* Structural: tasks appeared or vanished, and a first pass may have
     * created the list they live in.                                       */
    task_app_notify_changed(job->app);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * bn_thread() — the worker: list, reconcile, reap.  Owns its SQLite
 * connection for the whole pass.
 * ------------------------------------------------------------------------- */
static gpointer
bn_thread(gpointer data)
{
    BnJob *job = data;
    GError *gerr = NULL;
    TaskDatabase *db = task_db_open(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Notes sync failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gchar *err = NULL;
    GPtrArray *items = task_bnotes_actions(&err);
    if (items == NULL) {
        /* Tell "Notes is too old" apart from every other failure —
         * the fix is entirely different, and positional addressing is
         * NOT an acceptable fallback (it would bind tasks to whichever
         * item happens to sit at that position).                           */
        if (!task_bnotes_supports_uid())
            job->message = g_strdup("Notes is too old for action-item "
                                    "sync \xe2\x80\x94 update Notes");
        else
            job->message = g_strdup_printf("Notes sync failed: %s",
                                           err != NULL ? err : "unknown");
        g_free(err);
        task_db_close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gint64 target = task_bnsync_target_list(db, job->configured_list);
    if (target == 0) {
        job->message = g_strdup("Notes sync failed: cannot create the "
                                "Action Items list");
        task_bnotes_actions_free(items);
        task_db_close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    GHashTable *suppressed = bn_suppressed(db);
    GHashTable *present = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < items->len; i++) {
        TaskNoteAction *it = g_ptr_array_index(items, i);
        g_hash_table_add(present, GSIZE_TO_POINTER(it->uid));
    }
    for (guint i = 0; i < items->len; i++) {
        TaskNoteAction *it = g_ptr_array_index(items, i);
        if (g_hash_table_contains(suppressed, GSIZE_TO_POINTER(it->uid)))
            continue;            /* deleted in Tasks; do not re-create      */
        sync_item(job, db, it, target);
    }
    reap_missing(job, db, present, suppressed);

    gchar *stamp = g_strdup_printf("%lld", (long long)time(NULL));
    task_db_state_set(db, "bn_last_sync", stamp);
    g_free(stamp);

    job->ok = job->n_failed == 0;
    if (job->n_created == 0 && job->n_updated == 0 && job->n_removed == 0 &&
        job->n_pushed == 0 && job->n_failed == 0) {
        job->message = g_strdup("Action items up to date");
    } else {
        GString *s = g_string_new("Action items:");
        if (job->n_created > 0)
            g_string_append_printf(s, " %d added,", job->n_created);
        if (job->n_updated > 0)
            g_string_append_printf(s, " %d updated,", job->n_updated);
        if (job->n_removed > 0)
            g_string_append_printf(s, " %d removed,", job->n_removed);
        if (job->n_pushed > 0)
            g_string_append_printf(s, " %d sent to Notes,", job->n_pushed);
        if (job->n_failed > 0)
            g_string_append_printf(s, " %d failed,", job->n_failed);
        if (s->len > 0 && s->str[s->len - 1] == ',')
            g_string_truncate(s, s->len - 1);
        job->message = g_string_free(s, FALSE);
    }

    g_hash_table_destroy(present);
    g_hash_table_destroy(suppressed);
    task_bnotes_actions_free(items);
    task_db_close(db);
    g_idle_add(bn_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * task_bnsync_start() — kick off one pass (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
task_bnsync_start(TaskApp *app, const gchar *db_path, TaskBnSyncDoneFn done,
                  gpointer user_data)
{
    if (app->bn_sync_running)
        return;                      /* silent: a pass is already running   */
    if (!task_app_config_get_bool("notes_sync", FALSE)) {
        task_app_status(app, "Notes integration is off");
        if (done != NULL)
            done(app, FALSE, "Notes integration is off", user_data);
        return;
    }

    BnJob *job = g_new0(BnJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->done      = done;
    job->user_data = user_data;
    /* Config is read HERE, on the main thread — the worker must not
     * touch the shared GKeyFile.                                           */
    gchar *v = task_app_config_get("notes_embed_list");
    job->configured_list = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
    g_free(v);

    app->bn_sync_running = TRUE;
    task_app_status(app, "Syncing action items\xe2\x80\xa6");
    GThread *th = g_thread_new("task-bnsync", bn_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * Periodic mirror pass — the scheduler drives it (see task_worker.h).
 * ------------------------------------------------------------------------- */

/* bn_run() — start one pass.                                              */
static void
bn_run(TaskApp *app, const gchar *db_path)
{
    task_bnsync_start(app, db_path, NULL, NULL);
}

/* bn_on_arm() — before the timer goes in: a target list changed while
 * the mirror was switched off (or by an earlier build that only honored
 * the setting at creation time) still has to reach the items already
 * mirrored.                                                               */
static void
bn_on_arm(TaskApp *app)
{
    task_bnsync_reconcile_target(app);
}

static const TaskWorkerDef bn_worker = {
    .id               = "notes",
    .enabled_key      = "notes_sync",
    .enabled_default  = FALSE,
    .interval_key     = "notes_sync_interval_min",
    .interval_default = 5,
    /* ALWAYS, not ARMED: at interval 0 the user asked for manual passes,
     * but the Action Items view is EMPTY until one has run — so "manual
     * only" still has to mean "populate it now".                          */
    .initial          = TASK_WORKER_INITIAL_ALWAYS,
    .running          = NULL,        /* completed by task_bnsync_init       */
    .timer            = NULL,
    .run              = bn_run,
    .ready            = NULL,        /* the CLI is always worth asking      */
    .on_arm           = bn_on_arm,
};

static TaskWorkerDef bn_worker_live;

/* ---------------------------------------------------------------------------
 * task_bnsync_auto_start() — (re)arm the mirror timer (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
task_bnsync_auto_start(TaskApp *app, const gchar *db_path)
{
    task_worker_arm(app, &bn_worker_live, db_path);
}

/* ---------------------------------------------------------------------------
 * The "Action Items" sidebar view.
 *
 * A FILTERED view over every mirrored task, wherever each one actually
 * lives — not a list of its own.  That is why virtual_rows is TRUE: each
 * row keeps its "in <list>" line, which is the only thing that says
 * where the task really sits.
 * ------------------------------------------------------------------------- */
static gboolean
bn_view_visible(TaskApp *app, gpointer d)
{
    (void)app;
    (void)d;
    return task_app_config_get_bool("notes_sync", FALSE) &&
           task_app_config_get_bool("notes_meta_row", TRUE);
}

static GPtrArray *
bn_view_query(TaskApp *app, gpointer d)
{
    (void)d;
    return bn_mirror_tasks(app->db);
}

/* The id is "bn_actions" because that is what manual_order_bn_actions and
 * kanban_order_bn_actions already say in users' ini files — the order
 * keys are derived from it (see task_view.h).                             */
static const TaskView bn_view = {
    .id           = "bn_actions",
    .label        = "\xe2\x9d\x97\xef\xb8\x8f  Action Items",
    .name         = "Action Items",
    .unit         = "action item",
    .sort         = 30,
    .visible      = bn_view_visible,
    .query        = bn_view_query,
    .virtual_rows = TRUE,
    .not_a_list   = "Action Items is a view, not a list \xe2\x80\x94 "
                    "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6",
};

/* ---------------------------------------------------------------------------
 * The ❗ glyph on a mirrored task's row.
 *
 * It used to be a hard-coded `t->bn_uid != 0` test inside the row
 * renderer — the renderer knowing what a Notes item was.  It is now a
 * registered decoration (task_rows.h), collected in ONE query per
 * refresh rather than asked per row.
 *
 * It sorts below the app's own glyphs (favourite 100, priority 200) so it
 * lands INNERMOST, nearest the title: it describes what the row IS, not
 * how the user has flagged it.  The full stack reads ↳ 🚨 ⭐️ ❗ Title.
 * ------------------------------------------------------------------------- */
static GHashTable *
bn_decor_collect(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    if (!task_app_config_get_bool("notes_sync", FALSE))
        return NULL;                 /* integration off: nothing to mark   */

    GHashTable *set = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    GPtrArray *mirror = bn_mirror_tasks(app->db);
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        gint64 *k = g_new(gint64, 1);
        *k = t->id;
        g_hash_table_add(set, k);
    }
    task_ptr_array_free_tasks(mirror);
    return set;
}

static const TaskRowDecorDef bn_decor = {
    .id      = "notes-action-item",
    .sort    = 50,                   /* inside favourite (100)             */
    .collect = bn_decor_collect,
    .prefix  = "\xe2\x9d\x97  ",       /* ❗                               */
};

/* ---------------------------------------------------------------------------
 * task_bnsync_init() — register the mirror's worker and db hooks (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
task_bnsync_init(TaskApp *app)
{
    bn_worker_live         = bn_worker;
    bn_worker_live.running = &app->bn_sync_running;
    bn_worker_live.timer   = &app->bn_sync_timer;
    task_worker_register(&bn_worker_live);

    task_db_add_delete_hook(bn_delete_hook, NULL);
    task_view_register(&bn_view);
    task_rows_add_decoration(&bn_decor);
}
