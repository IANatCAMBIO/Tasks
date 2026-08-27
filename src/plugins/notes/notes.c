/* ===========================================================================
 * notes.c — the Notes action-item mirror, as a plugin.
 *
 * Mirrors the companion Notes app's action items ('!' lines) as ORDINARY
 * tasks, so each one carries notes, subtasks, attachments, a pin and a
 * priority like anything else — and, living in a real list, syncs on to
 * Google Tasks too.  bnotes.c underneath is the CLI wrapper.
 *
 * FIELD OWNERSHIP.  Notes owns TITLE, DONE and DUE; a title edited in
 * Tasks is overwritten on the next pass, because the CLI has no verb to
 * rewrite an item's text.  Everything else is Tasks-only and never
 * leaves.  Notes' DONE is BINARY, so the mirror speaks only in the
 * done-ness of `status`: a New <-> In Progress move is not a pending
 * write and has nothing to push.
 *
 * WRITES ARE CACHED, NOT LIVE.  notes_task.done/due hold what Notes was
 * last known to have, so the rows whose done-ness or due date differs
 * from that baseline ARE the pending-write set — no queue table to
 * corrupt, and it survives a crash.
 *
 * IDENTITY IS THE UID, never the position: NOTEID:ORD renumbers whenever
 * a note gains or loses a '!' line, so a stored positional ref silently
 * comes to mean a different item and a "done" tick would strike the
 * wrong line.
 *
 * The plugin reaches the app ONLY through the host table (plugin.h).
 * =========================================================================== */

#include "plugin_ctx.h"
#include "bnotes.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The host table and this plugin's identity — defined here, declared in
 * plugin_ctx.h so bnotes.c shares exactly one of each.                    */
const TaskHostApi *host = NULL;
const TaskPlugin  *self = NULL;

/* The mirror's in-flight guard and timer.  These used to be fields on
 * TaskApp; a plugin owns its own, which is the point — the app no longer
 * carries a slot per integration.                                        */
static gboolean bn_running = FALSE;
static guint    bn_timer   = 0;

/* bn_status() — task_app_status's printf shape over the host's plain
 * status(), so the call sites below read as they always did.  The status
 * bar is a PLAIN-TEXT label, so nothing here is markup-escaped.          */
static void
bn_status(TaskApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
bn_status(TaskApp *app, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    host->notify->status(app, msg);
    g_free(msg);
}

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
    host->db->exec_query(db, id_sql, collect_i64, ids);
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < ids->len; i++) {
        Task *t = host->db->task_get(db, g_array_index(ids, gint64, i));
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
    gint64 uid = host->db->scalar(db, sql);
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
    gint64 id = host->db->scalar(db, sql);
    g_free(sql);
    return id > 0 ? host->db->task_get(db, id) : NULL;
}

/* bn_baseline() — what Notes was last known to hold for this task.       */
static void
bn_baseline(TaskDatabase *db, gint64 task_id, gboolean *done, gint64 *due)
{
    gchar *sql = g_strdup_printf(
        "SELECT done FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    *done = host->db->scalar(db, sql) > 0;
    g_free(sql);
    sql = g_strdup_printf(
        "SELECT due FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    gint64 d = host->db->scalar(db, sql);
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
    host->db->exec(db, sql);
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
    host->db->exec_query(db, "SELECT uid FROM notes_deleted",
                       collect_uid_direct, set);
    return set;
}

static void
bn_forget(TaskDatabase *db, gint64 uid)
{
    gchar *sql = g_strdup_printf(
        "DELETE FROM notes_deleted WHERE uid = %" G_GINT64_FORMAT, uid);
    host->db->exec(db, sql);
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

    gboolean        ok;
    gchar          *message;         /* summary or error (owned)            */
    gint            n_created;
    gint            n_updated;
    gint            n_removed;
    gint            n_pushed;
    gint            n_failed;        /* pushes Notes refused              */
    gint            n_unreaped;      /* reap refused — see reap_missing()   */
} BnJob;

/* ---------------------------------------------------------------------------
 * bn_target_list() — resolve the destination list mirrored items are
 * filed into: the configured list when it names a LIVE one, else the
 * managed "Action Items" list, created on first use.
 * ------------------------------------------------------------------------- */
gint64
bn_target_list(TaskDatabase *db, gint64 configured)
{
    if (configured > 0) {
        TaskList *l = host->db->list_get(db, configured);
        if (l != NULL) {
            gboolean alive = !l->deleted;
            host->db->list_free(l);
            if (alive)
                return configured;
        }
        /* A deleted or vanished target falls through to the managed
         * list rather than stranding every item (bn_embed_list did the
         * same before the mirror existed).                                 */
    }
    GPtrArray *lists = host->db->lists(db, FALSE);
    gint64 found = 0;                /* the managed list, if it exists      */
    for (guint i = 0; i < lists->len && found == 0; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        if (g_strcmp0(l->name, BN_LIST_NAME) == 0)
            found = l->id;
    }
    host->db->lists_free(lists);
    if (found != 0)
        return found;
    return host->db->list_create(db, BN_LIST_NAME, BN_LIST_EMOJI);
}

/* ---------------------------------------------------------------------------
 * bn_reconcile_target() — apply a changed target list to the
 * items already mirrored.
 * ------------------------------------------------------------------------- */
void
bn_reconcile_target(TaskApp *app)
{
    if (!host->config->get_bool(self, "sync", FALSE))
        return;
    gchar *v = host->config->get(self, "embed_list");
    gint64 configured = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
    g_free(v);

    gint64 target = bn_target_list(host->db->main_db(app), configured);
    if (target == 0)
        return;                      /* the list could not be created       */

    /* An ABSENT applied-value counts as "not applied yet", not as "same
     * as now": that is the upgrade case, where tasks were mirrored by a
     * build that only honored the setting at creation time and are
     * sitting in the wrong list.                                           */
    gchar *applied_s = host->db->state_get(host->db->main_db(app), "bn_target_list");
    gint64 applied = applied_s != NULL
                   ? g_ascii_strtoll(applied_s, NULL, 10) : 0;
    gboolean known = applied_s != NULL;
    g_free(applied_s);
    if (known && applied == target)
        return;                      /* nothing changed — leave hand moves  */

    GPtrArray *mirror = bn_mirror_tasks(host->db->main_db(app));
    guint moved = 0;                 /* rows that actually changed list     */
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        /* Subtasks travel with their parent; mirror rows are top-level
         * anyway, so this only guards against hand-edited data.            */
        if (host->ops->move_to_list(app, t->id, target))
            moved++;                 /* it declines subtasks and no-op moves */
    }
    host->db->tasks_free(mirror);

    gchar *stamp = g_strdup_printf("%" G_GINT64_FORMAT, target);
    host->db->state_set(host->db->main_db(app), "bn_target_list", stamp);
    g_free(stamp);

    if (moved > 0) {
        bn_status(app, "Moved %u action item%s", moved,
                        moved == 1 ? "" : "s");
        host->notify->notify_changed(app);
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
        gint64 id = host->db->task_create(db, target, 0, it->text);
        if (id == 0) {
            job->n_failed++;         /* create failures must not be silent  */
            return;
        }
        host->db->task_apply_done_source(db, id, it->text, it->done, it->due);
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
        host->db->task_apply_done_source(db, t->id, it->text, new_done,
                                       new_due);
        bn_set(db, t->id, it->uid, base_done, base_due);
        job->n_updated++;
    } else if (base_done != base_have_done || base_due != base_have_due) {
        /* Nothing the user can see changed — only the push baseline —
         * so this must NOT stamp updated_at, or every pass would dirty
         * the row and buy a no-op Google PATCH.                            */
        bn_set(db, t->id, it->uid, base_done, base_due);
    }
    host->db->task_free(t);
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
 *
 * EXCEPT when the listing is EMPTY.  That is not a hypothetical: a CLI
 * call is answered by whichever Notes instance owns the socket, not by
 * the binary on disk (gotcha 17), and a stale one answers `action list`
 * with NO ROWS AND EXIT 0 — indistinguishable, here, from "the user
 * deleted every action item".  Believing it tombstones every mirrored
 * task, and because a tombstone is what the Google sync pushes, those
 * deletes then propagate off this machine.  So an empty listing that
 * would reap ANYTHING is refused: the tasks are left exactly as they
 * are and the pass says so.
 *
 * This is the app's own "ABSENCE NEVER DELETES" rule, which the Google
 * sync already follows, and the same shape as the v8/v9 migrations —
 * a copy that does not verify drops nothing and reports.  The cost is
 * accepted deliberately: a Notes that HAS genuinely been emptied leaves
 * its mirrored tasks behind, and the user deletes them in Tasks.  That
 * direction is recoverable; the other is not.
 *
 * The suppression sweep is skipped too.  A listing not trusted to say
 * what still exists cannot be trusted to say what is gone for good, and
 * forgetting a suppression on a bad listing would let the next pass
 * re-create the very task the user deleted here.
 * ------------------------------------------------------------------------- */
static void
reap_missing(BnJob *job, TaskDatabase *db, GHashTable *present,
             GHashTable *suppressed)
{
    GPtrArray *mirror = bn_mirror_tasks(db);

    if (g_hash_table_size(present) == 0 && mirror->len > 0) {
        job->n_unreaped = (gint)mirror->len;
        /* Logged as well as reported: the status-bar line fades after a
         * few seconds, and "we declined to delete %u tasks" is the kind
         * of thing someone needs to find afterwards.                    */
        g_warning("notes: the listing was EMPTY \xe2\x80\x94 refusing to "
                  "reap %u mirrored task%s; nothing was deleted",
                  mirror->len, mirror->len == 1 ? "" : "s");
        host->db->tasks_free(mirror);
        return;
    }

    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        if (g_hash_table_contains(present, GSIZE_TO_POINTER(bn_uid_of(db, t->id))))
            continue;
        host->db->task_delete(db, t->id);
        /* task_delete parks the uid in bn_deleted so a live item is not
         * re-created after a local delete; here the item is gone from
         * Notes, so that suppression has nothing left to suppress.       */
        bn_forget(db, bn_uid_of(db, t->id));
        job->n_removed++;
    }
    host->db->tasks_free(mirror);

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
    bn_running = FALSE;
    if (job->message != NULL)
        bn_status(job->app, "%s", job->message);
    /* Structural: tasks appeared or vanished, and a first pass may have
     * created the list they live in.                                       */
    host->notify->notify_changed(job->app);
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
    TaskDatabase *db = host->db->open(job->db_path, &gerr);
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
        host->db->close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gint64 target = bn_target_list(db, job->configured_list);
    if (target == 0) {
        job->message = g_strdup("Notes sync failed: cannot create the "
                                "Action Items list");
        task_bnotes_actions_free(items);
        host->db->close(db);
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
    host->db->state_set(db, "bn_last_sync", stamp);
    g_free(stamp);

    job->ok = job->n_failed == 0 && job->n_unreaped == 0;
    if (job->n_unreaped > 0) {
        /* Its OWN message, not a count folded in with the others: this
         * is the pass declining to do something, and it names the likely
         * cause because the fix is a restart of the other Notes rather
         * than anything in Tasks.                                        */
        job->message = g_strdup_printf(
            "Notes listed no action items \xe2\x80\x94 left %d mirrored "
            "task%s alone.  Is an old Notes still running?",
            job->n_unreaped, job->n_unreaped == 1 ? "" : "s");
    } else if (job->n_created == 0 && job->n_updated == 0 &&
               job->n_removed == 0 && job->n_pushed == 0 &&
               job->n_failed == 0) {
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
    host->db->close(db);
    g_idle_add(bn_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bn_start() — kick off one mirror pass on a worker thread.
 *
 * Two early-outs: a pass already in flight (silent — the guard is there
 * to stop a tick piling onto a slow CLI round trip), and the integration
 * switched off in Settings, which says so.  Main thread only.
 * ------------------------------------------------------------------------- */
static void
bn_start(TaskApp *app, const gchar *db_path)
{
    if (bn_running)
        return;                      /* silent: a pass is already running   */
    if (!host->config->get_bool(self, "sync", FALSE)) {
        bn_status(app, "Notes integration is off");
        return;
    }

    BnJob *job = g_new0(BnJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    /* Config is read HERE, on the main thread — the worker must not
     * touch the shared GKeyFile.                                           */
    gchar *v = host->config->get(self, "embed_list");
    job->configured_list = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
    g_free(v);

    bn_running = TRUE;
    bn_status(app, "Syncing action items\xe2\x80\xa6");
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
    bn_start(app, db_path);
}

/* bn_on_arm() — before the timer goes in: a target list changed while
 * the mirror was switched off (or by an earlier build that only honored
 * the setting at creation time) still has to reach the items already
 * mirrored.                                                               */
static void
bn_on_arm(TaskApp *app)
{
    bn_reconcile_target(app);
}

static const TaskWorkerDef bn_worker = {
    .id               = "notes",
    /* Before the Google sync (which takes the default 0): a new action
     * item is mirrored and then pushed on to Google by ONE press of
     * Sync.  Stated here rather than left to plugin load order, which is
     * whatever the loader's directory read happened to hand back.      */
    .sort             = -10,
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
 * bn_auto_start() — (re)arm the mirror timer from the interval setting
 * (default 5 minutes; 0 = only when Sync is pressed).
 * ------------------------------------------------------------------------- */
void
bn_auto_start(TaskApp *app, const gchar *db_path)
{
    host->worker->arm(app, &bn_worker_live, db_path);
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
    return host->config->get_bool(self, "sync", FALSE) &&
           host->config->get_bool(self, "meta_row", TRUE);
}

static GPtrArray *
bn_view_query(TaskApp *app, gpointer d)
{
    (void)d;
    return bn_mirror_tasks(host->db->main_db(app));
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
    if (!host->config->get_bool(self, "sync", FALSE))
        return NULL;                 /* integration off: nothing to mark   */

    GHashTable *set = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    GPtrArray *mirror = bn_mirror_tasks(host->db->main_db(app));
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        gint64 *k = g_new(gint64, 1);
        *k = t->id;
        g_hash_table_add(set, k);
    }
    host->db->tasks_free(mirror);
    return set;
}

static const TaskRowDecorDef bn_decor = {
    .id      = "notes-action-item",
    .sort    = 50,                   /* inside favourite (100)             */
    .collect = bn_decor_collect,
    .prefix  = "\xe2\x9d\x97  ",       /* ❗                               */
};

/* ===========================================================================
 * The Notes section of the Settings window.
 *
 * Contributed through host->settings->add_section() rather than written
 * into settings_window.c, for the reason every other integration setting
 * is: the window should not know what a Notes action item is.  The
 * mirror owns its own controls, so switching it off is a matter of not
 * registering this.
 *
 * The builder runs afresh every time Settings is opened, against a
 * window that is destroyed each time — so nothing here is remembered
 * between calls.  Widget values are set BEFORE the handlers are
 * connected, which is what removes the need for the window's own
 * `loading` guard: a set that happens before a connect cannot fire one.
 * =========================================================================== */

/* The combo index -> list id table, hung off the combo itself: a handler
 * needs it, and the window it lives in is destroyed and rebuilt on every
 * opening, so a file-scope copy would have to be kept in step by hand.    */
#define BN_SET_IDS    "bn-set-ids"

/* bn_ids_free() — GDestroyNotify for that table.                          */
static void
bn_ids_free(gpointer data)
{
    g_array_free(data, TRUE);
}

/* bn_settings_db_path() — the path the mirror's worker should be armed
 * on.  Read from the LIVE connection rather than remembered: a database
 * switch replaces it, and a handler holding the old one would arm a
 * worker on a file that has just been removed.                            */
static const gchar *
bn_settings_db_path(TaskApp *app)
{
    return app->db != NULL ? app->db->path : NULL;
}

/* on_bn_toggled() — the mirror's master switch: persist, then re-arm the
 * timer.  Switching ON runs a pass immediately (that is what populates
 * the Action Items view); switching OFF stops the timer.                  */
static void
on_bn_toggled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "sync",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    bn_auto_start(app, bn_settings_db_path(app));
    host->notify->notify_changed(app);
}

/* on_bn_interval_changed() — write-through + re-arm the mirror timer.     */
static void
on_bn_interval_changed(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    gchar *v = g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)));
    host->config->set(self, "sync_interval_min", v);
    g_free(v);
    bn_auto_start(app, bn_settings_db_path(app));
}

/* on_bn_meta_toggled() — show/hide the sidebar's Action Items view.       */
static void
on_bn_meta_toggled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "meta_row",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    host->notify->notify_changed(app);
}

/* on_bn_embed_changed() — which list mirrored action items live in.
 *
 * Persists the choice, MOVES the existing items there (the setting names
 * where they live, not merely where the next one lands), and runs a pass
 * so the change is visible immediately.  A per-task move made by hand
 * still sticks until this setting is touched again.                       */
static void
on_bn_embed_changed(GtkComboBox *combo, gpointer data)
{
    TaskApp *app = data;
    GArray  *ids = g_object_get_data(G_OBJECT(combo), BN_SET_IDS);
    gint active = gtk_combo_box_get_active(combo);
    if (ids == NULL || active < 0 || active >= (gint)ids->len)
        return;
    gint64 id = g_array_index(ids, gint64, active);
    if (id == 0) {
        host->config->set(self, "embed_list", NULL);
    } else {
        gchar *v = g_strdup_printf("%" G_GINT64_FORMAT, id);
        host->config->set(self, "embed_list", v);
        g_free(v);
    }
    bn_reconcile_target(app);
    if (host->config->get_bool(self, "sync", FALSE))
        bn_start(app, bn_settings_db_path(app));
    host->notify->notify_changed(app);
}

/* on_bn_cli_changed() — the CLI path entry: persist ONLY.  The mirror
 * pass happens on commit (focus-out/Enter) — running it per keystroke
 * would spawn the half-typed command over and over.                       */
static void
on_bn_cli_changed(GtkWidget *w, gpointer data)
{
    (void)data;
    const gchar *cli = gtk_entry_get_text(GTK_ENTRY(w));
    host->config->set(self, "cli", *cli != '\0' ? cli : NULL);
}

/* on_bn_cli_commit() — Enter in the CLI path entry: run a pass against
 * the newly named binary so a wrong path reports itself now rather than
 * at the next tick.                                                       */
static void
on_bn_cli_commit(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskApp *app = data;
    if (host->config->get_bool(self, "sync", FALSE))
        bn_start(app, bn_settings_db_path(app));
    host->notify->notify_changed(app);
}

/* on_bn_cli_focus_out() — leaving the CLI path entry: commit now.         */
static gboolean
on_bn_cli_focus_out(GtkWidget *w, GdkEventFocus *event, gpointer data)
{
    (void)event;
    on_bn_cli_commit(w, data);
    return FALSE;                    /* propagate                           */
}

/* bn_settings_section() — build the Notes section into the window's one
 * scrolling column (see settings_window.h).                               */
static void
bn_settings_section(TaskApp *app, GtkWidget *column, GtkWindow *window,
                    gpointer user_data)
{
    (void)window;
    (void)user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(column), box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       host->settings->heading("Notes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), host->settings->note(
        "Mirror the action items from Notes as ordinary tasks, with "
        "their own notes, subtasks and attachments. Ticking one off or "
        "changing its due date is sent back to Notes on the interval "
        "below; the item's text belongs to the note it lives in, so "
        "edit that in Notes."), FALSE, FALSE, 0);

    GtkWidget *check = gtk_check_button_new_with_label(
        "Mirror Notes action items");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
        host->config->get_bool(self, "sync", FALSE));
    g_signal_connect(check, "toggled", G_CALLBACK(on_bn_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);

    GtkWidget *cli_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cli_row), gtk_label_new("Notes command:"),
                       FALSE, FALSE, 0);
    GtkWidget *cli = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cli), "notes (searched on PATH)");
    gtk_widget_set_hexpand(cli, TRUE);
    gchar *cli_v = host->config->get(self, "cli");
    if (cli_v != NULL)
        gtk_entry_set_text(GTK_ENTRY(cli), cli_v);
    g_free(cli_v);
    g_signal_connect(cli, "changed", G_CALLBACK(on_bn_cli_changed), app);
    g_signal_connect(cli, "activate", G_CALLBACK(on_bn_cli_commit), app);
    g_signal_connect(cli, "focus-out-event",
                     G_CALLBACK(on_bn_cli_focus_out), app);
    gtk_box_pack_start(GTK_BOX(cli_row), cli, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), cli_row, FALSE, FALSE, 0);

    /* Which real list the mirrored tasks are filed into.  0 = let the
     * mirror manage its own "Action Items" list, created on first use.  */
    GtkWidget *embed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(embed_row),
                       gtk_label_new("Mirror action items into:"),
                       FALSE, FALSE, 0);
    GtkWidget *combo = gtk_combo_box_text_new();
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    gint64 own = 0;
    g_array_append_val(ids, own);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Action Items (managed list)");
    gchar *embed_v = host->config->get(self, "embed_list");
    gint64 embed_id = embed_v != NULL
                      ? g_ascii_strtoll(embed_v, NULL, 10) : 0;
    g_free(embed_v);
    gint embed_active = 0;           /* combo index to preselect            */
    GPtrArray *lists = host->db->lists(host->db->main_db(app), FALSE);
    for (guint i = 0; i < lists->len; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        gchar *label = *l->emoji != '\0'
            ? g_strdup_printf("%s  %s", l->emoji, l->name)
            : g_strdup(l->name);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), label);
        g_free(label);
        g_array_append_val(ids, l->id);
        if (l->id == embed_id)
            embed_active = (gint)ids->len - 1;
    }
    host->db->lists_free(lists);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), embed_active);
    gtk_widget_set_tooltip_text(combo,
        "Action items live here \xe2\x80\x94 changing this moves the "
        "existing ones too");
    /* The id table dies WITH the combo: the window is rebuilt per
     * opening, so anything outliving it would leak one array a time.    */
    g_object_set_data_full(G_OBJECT(combo), BN_SET_IDS, ids,
                           (GDestroyNotify)bn_ids_free);
    g_signal_connect(combo, "changed", G_CALLBACK(on_bn_embed_changed), app);
    gtk_box_pack_start(GTK_BOX(embed_row), combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), embed_row, FALSE, FALSE, 0);

    /* How often the mirror runs — the same shape as the Google sync's
     * own (0 = only when Sync is pressed).                              */
    GtkWidget *iv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(iv_row),
                       gtk_label_new("Sync action items every"),
                       FALSE, FALSE, 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(spin, "0 = only when you press Sync");
    gchar *iv_v = host->config->get(self, "sync_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                              iv_v != NULL ? g_ascii_strtod(iv_v, NULL) : 5);
    g_free(iv_v);
    g_signal_connect(spin, "value-changed",
                     G_CALLBACK(on_bn_interval_changed), app);
    gtk_box_pack_start(GTK_BOX(iv_row), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(iv_row), gtk_label_new("minutes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), iv_row, FALSE, FALSE, 0);

    GtkWidget *meta = gtk_check_button_new_with_label(
        "Show the Action Items view in the sidebar");
    gtk_widget_set_tooltip_text(meta,
        "Lists every mirrored action item in one place, whichever list "
        "each one lives in");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta),
        host->config->get_bool(self, "meta_row", TRUE));
    g_signal_connect(meta, "toggled", G_CALLBACK(on_bn_meta_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), meta, FALSE, FALSE, 0);
}

/* ---------------------------------------------------------------------------
 * notes_init() — the plugin's init hook: register the worker, the sidebar
 * view, the row decoration, the delete hook and the settings section.
 * Cheap by design; it runs before the window is shown (see plugin.h).
 * ------------------------------------------------------------------------- */
static gboolean
notes_init(TaskApp *app, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    bn_worker_live         = bn_worker;
    bn_worker_live.running = &bn_running;
    bn_worker_live.timer   = &bn_timer;
    host->worker->register_worker(&bn_worker_live);

    host->ops->add_delete_hook(bn_delete_hook, NULL);
    host->views->register_view(&bn_view);
    host->rows->add_decoration(&bn_decor);
    host->settings->add_section(bn_settings_section, NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * notes_db_open() — create this plugin's own tables.
 *
 * Called for the main connection at startup and again whenever the
 * database changes identity.  IF NOT EXISTS because a database migrated
 * from schema v9 already has them, and the worker opens the same file on
 * its own connection.
 *
 * notes_task is the mirror's whole bookkeeping: the uid that IS the
 * item's identity, plus `done`/`due` as the BASELINE — what Notes was
 * last known to hold.  Diffing a row against that baseline is what makes
 * the pending-write set DERIVABLE rather than queued, so there is no
 * queue table to corrupt and the set survives a crash.
 *
 * notes_deleted parks the uid of a mirror task deleted in Tasks.  Notes
 * has no CLI verb to delete an action item, so without this the very
 * next pass would helpfully re-create what the user just deleted.
 *
 * These statements are duplicated by the v9 migration in db.c, and that
 * is correct rather than redundant: the migration has to MOVE existing
 * data whether or not this plugin is installed, and this hook has to
 * work on a database that never had the old columns at all.
 * ------------------------------------------------------------------------- */
static void
notes_db_open(TaskApp *app, TaskDatabase *db, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS notes_task ("
        "  task_id INTEGER PRIMARY KEY REFERENCES tasks(id)"
        "          ON DELETE CASCADE,"
        "  uid     INTEGER NOT NULL,"
        "  done    INTEGER NOT NULL DEFAULT 0,"
        "  due     INTEGER NOT NULL DEFAULT 0)");
    host->db->exec(db,
        "CREATE INDEX IF NOT EXISTS idx_notes_task_uid ON notes_task(uid)");
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS notes_deleted (uid INTEGER PRIMARY KEY)");
}

static const TaskPlugin notes_plugin = {
    .abi_version     = TASK_PLUGIN_ABI_VERSION,
    .abi_revision    = TASK_PLUGIN_ABI_REVISION,
    .id              = "notes",
    .name            = "Notes",
    .description     = "Mirror the companion Notes app's action items as "
                       "ordinary tasks.",
    .version         = "1.0.0",
    .enabled_default = TRUE,
    .init            = notes_init,
    .db_open         = notes_db_open,
};

TASK_PLUGIN_EXPORT const TaskPlugin *
task_plugin_entry(const TaskHostApi *api)
{
    host = api;
    self = &notes_plugin;
    return &notes_plugin;
}
