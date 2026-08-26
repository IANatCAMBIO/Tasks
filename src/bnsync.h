/* ===========================================================================
 * bnsync.h — Notes action-item mirror for Tasks
 *
 * Sync model
 * ----------
 * Every '!' action item in Notes is mirrored as an ORDINARY Tasks
 * task, so it carries notes, subtasks, attachments, a pin and a
 * priority like any other task — and, living in a real list, it syncs
 * on to Google Tasks as well.  Identity is the item's STABLE uid from
 * `notes action list --uid`, stored in tasks.bn_uid; it survives
 * rewording and the renumbering of a note's lines, which the older
 * NOTEID:ORD address did not.
 *
 *   remote item, no local task  → create the mirror task
 *   both present                → push cached local done/due changes,
 *                                 then take Notes' title/done/due
 *   local task, item gone       → tombstone the task (Notes is
 *                                 authoritative for existence)
 *   task deleted in Tasks       → uid parked in bn_deleted so the next
 *                                 pass does not re-create it (Notes
 *                                 has no CLI verb to delete an item)
 *
 * Field ownership.  Notes owns TITLE, DONE and DUE.  Everything else
 * — notes, subtasks, attachments, pin, priority, which list the task
 * lives in — is Tasks-only and never leaves.  The title is one-way by
 * necessity: the CLI has no verb that rewrites an item's text, so a
 * title edited in Tasks is overwritten on the next pass.
 *
 * Writes are CACHED, not live.  bn_done/bn_due record the state Notes
 * was last known to hold; a row where done/due differ from that
 * baseline IS the pending-write set, so the queue survives a crash and
 * cannot drift out of step with the tasks. Each pass pushes those
 * deltas in bulk, on the interval in "notes_sync_interval_min".  A
 * local change therefore WINS over a concurrent Notes-side change to
 * the same field: it is pushed first, and the listing that follows
 * reads back what was just written.
 *
 * Threading: the pass runs on a worker thread with its OWN SQLite
 * connection (a connection must not cross threads); the CLI is spawned
 * there too, so a slow Notes never blocks the UI.  Status and
 * completion are marshalled back with g_idle_add.
 *
 * Requires a Notes new enough to understand `action list --uid`.
 * Against an older build the pass refuses to run and says so rather
 * than falling back to positional addressing, which would silently
 * bind tasks to the wrong items.
 * =========================================================================== */

#ifndef TASK_BNSYNC_H
#define TASK_BNSYNC_H

#include "app.h"

/* Completion callback; runs on the main thread.  `message` is a short
 * human-readable summary or error (not owned by the callee).               */
typedef void (*TaskBnSyncDoneFn)(TaskApp *app, gboolean ok, const gchar *message,
                                 gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_bnsync_start() — kick off one mirror pass on a worker thread.
 * Early-outs, each with a status message: the Notes integration
 * switched off in Settings, and "already running" (which does not fire
 * `done`).  `done` may be NULL.  Main thread only.
 * ------------------------------------------------------------------------- */
void task_bnsync_start(TaskApp *app, const gchar *db_path,
                       TaskBnSyncDoneFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_bnsync_auto_start() — install the periodic mirror timer from the
 * "notes_sync_interval_min" config key (default 5; 0 disables) and
 * run one initial pass.  Safe to call again after the setting changes.
 * ------------------------------------------------------------------------- */
void task_bnsync_auto_start(TaskApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * task_bnsync_reconcile_target() — move every mirrored task into the
 * configured list when that setting has CHANGED since it was last
 * applied (the applied value lives in sync_state "bn_target_list").
 *
 * The mirror consults the target only when it CREATES a task, so
 * without this, pointing the setting at a different list would leave
 * every existing item where it was — the setting would look broken.
 * Comparing against the last-applied value is what keeps it from
 * fighting the user: a task moved to another list by hand stays there
 * until the setting itself changes again.
 *
 * Goes through task_ops_move_to_list(), so any integration watching for
 * moves gets its say — the Google plugin moves the remote copy rather
 * than stranding it in the old list.  Those hooks are main-thread-only,
 * which is why this is too and is not part of the worker pass.
 * ------------------------------------------------------------------------- */
void task_bnsync_reconcile_target(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_bnsync_target_list() — the list mirrored items are filed into: the
 * one named by "notes_embed_list" when it still exists, else the
 * app-managed "Action Items" list, created on first use.  Returns 0
 * only when the list could not be created.  The config key keeps its
 * pre-rename name — it sits in users' ini files.
 * ------------------------------------------------------------------------- */
gint64 task_bnsync_target_list(TaskDatabase *db, gint64 configured);

/* ---------------------------------------------------------------------------
 * task_bnsync_init() — register the mirror's periodic worker and its
 * database hooks.  Call ONCE from main(), after task_app_config_init()
 * and BEFORE any worker thread exists (the registries are process-wide
 * and unlocked — see task_db_add_delete_hook).
 *
 * Registration is unconditional, NOT gated on the "notes_sync" setting:
 * the hook parks the bn_uid of a deleted mirror task in bn_deleted so a
 * later pass cannot re-create it, and a user who turns the mirror off,
 * deletes some items and turns it back on must not have them all
 * resurrected.  On a row with no uid the hook's statement matches
 * nothing, so it costs an unsynced database nothing.
 * ------------------------------------------------------------------------- */
void task_bnsync_init(TaskApp *app);

#endif /* TASK_BNSYNC_H */
