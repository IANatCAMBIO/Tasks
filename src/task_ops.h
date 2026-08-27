/* ===========================================================================
 * task_ops.h — core task operations, and the hooks integrations observe
 * them through.
 *
 * These are the operations that are LOCAL FIRST: the database write is
 * the operation, and any remote half is a consequence of it.  They live
 * here rather than in a sync engine because that is the wrong way round
 * — a move is a move whether or not anything syncs, and burying the
 * local write inside the Google engine is what made the Notes mirror
 * depend on Google Tasks in order to file its own items.
 *
 * The pattern every operation follows:
 *
 *     validate → write locally → notify every hook
 *
 * A hook runs on the MAIN THREAD, after the write has committed, and is
 * free to start a worker of its own.  It is a HINT, not a work queue:
 * nothing is retried, nothing is persisted, and a hook that never runs
 * (the app was killed between the commit and the notify) must not lose
 * work.  That is why both integrations derive their pending set from
 * database STATE — `updated_at` against sync_state.last_sync for Google,
 * the bn_done/bn_due baselines for Notes — and why they must keep doing
 * so.  A hook tells a feature "look now"; the database tells it what to
 * do.
 *
 * Vetoes are the one exception to "write, then notify": they run BEFORE
 * the operation and can refuse it.  They exist for the case where the
 * remote side is known to reject something the local side would happily
 * do (Google's default tasklist cannot be deleted by any client), where
 * letting the local write proceed means the row vanishes and then
 * reappears at the next sync.
 *
 * Registration is process-wide, once, at startup, before any worker
 * thread exists.  Hooks are never removed.
 * =========================================================================== */

#ifndef TASK_OPS_H
#define TASK_OPS_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * Hook signatures.
 *
 * moved   — a top-level task (with its subtasks) changed list.  Both the
 *           old and new list ids are passed because the row itself no
 *           longer remembers where it came from, and a remote move needs
 *           the SOURCE list to address the task it is moving.
 * cleared — a list's completed tasks were tombstoned.  `task_ids` holds
 *           the ids that actually went (gint64), borrowed for the call.
 *           It is one event rather than N delete events on purpose: a
 *           remote side with a batch "clear" call can use it.
 * veto    — may this list be deleted?  Return FALSE and set `*why` to a
 *           newly-allocated reason shown to the user; the caller frees.
 * ------------------------------------------------------------------------- */
typedef void (*TaskOpsMovedFn)(TaskApp *app, gint64 task_id,
                               gint64 from_list, gint64 to_list,
                               gpointer user_data);
typedef void (*TaskOpsClearedFn)(TaskApp *app, gint64 list_id,
                                 GArray *task_ids, gpointer user_data);
typedef gboolean (*TaskOpsListVetoFn)(TaskApp *app, const TaskList *list,
                                      gchar **why, gpointer user_data);

void task_ops_add_moved_hook(TaskOpsMovedFn fn, gpointer user_data);
void task_ops_add_cleared_hook(TaskOpsClearedFn fn, gpointer user_data);
void task_ops_add_list_veto(TaskOpsListVetoFn fn, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_ops_move_to_list() — move a TOP-LEVEL task and its subtasks to
 * another list, then notify.
 *
 * Refuses (returning FALSE, writing nothing, notifying nobody) when the
 * task is gone, is a subtask, or is already in that list.  Subtasks
 * travel with their parent, so a subtask has no move of its own.
 *
 * Returns TRUE when the task moved.  Main thread only.
 * ------------------------------------------------------------------------- */
gboolean task_ops_move_to_list(TaskApp *app, gint64 task_id,
                               gint64 dest_list_id);

/* ---------------------------------------------------------------------------
 * task_ops_clear_completed() — tombstone every completed top-level task
 * of `list_id` (subtasks go with their parents), then notify.
 *
 * Tombstones rather than physically purging, because that is the answer
 * that is correct with or without a sync: the removal propagates like
 * any other delete.  A hook whose remote side archived the same tasks
 * separately may purge those rows for real afterwards — it is handed
 * their ids for exactly that.
 *
 * Returns how many tasks went.  Main thread only.
 * ------------------------------------------------------------------------- */
guint task_ops_clear_completed(TaskApp *app, gint64 list_id);

/* ---------------------------------------------------------------------------
 * task_ops_list_can_delete() — run every registered veto against `list`.
 *
 * Returns TRUE when no veto objected.  On FALSE, `*why` holds a
 * newly-allocated reason to show the user (the caller frees it); pass
 * NULL for `why` if the reason is not wanted.  Vetoes run in
 * registration order and the first refusal wins.
 * ------------------------------------------------------------------------- */
gboolean task_ops_list_can_delete(TaskApp *app, const TaskList *list,
                                  gchar **why);

/* ---------------------------------------------------------------------------
 * task_ops_remove_owner() — remove everything plugin `owner`
 * registered here.  Called when a plugin is switched off while the app is
 * running; the app's OWN registrations are unowned and never match.
 * ------------------------------------------------------------------------- */
void task_ops_remove_owner(const gchar *owner);

#endif /* TASK_OPS_H */
