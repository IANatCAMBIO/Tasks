/* ===========================================================================
 * task_ops.h — core task operations.
 *
 * These are the operations that are LOCAL FIRST: the database write is
 * the operation.  They live here rather than in the window code because
 * a move is a move whether or not any integration is watching, and the
 * integrations that come back later (a sync, a mirror) will observe
 * these rather than reimplement them.
 *
 * Main thread only.
 * =========================================================================== */

#ifndef TASK_OPS_H
#define TASK_OPS_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * task_ops_move_to_list() — move a TOP-LEVEL task and its subtasks to
 * another list.
 *
 * Refuses (returning FALSE, writing nothing) when the task is gone, is a
 * subtask, or is already in that list.  Subtasks travel with their
 * parent, so a subtask has no move of its own.
 *
 * Returns TRUE when the task moved.
 * ------------------------------------------------------------------------- */
gboolean task_ops_move_to_list(TaskApp *app, gint64 task_id,
                               gint64 dest_list_id);

/* ---------------------------------------------------------------------------
 * task_ops_clear_completed() — tombstone every completed top-level task
 * of `list_id` (subtasks go with their parents).
 *
 * Tombstones rather than physically purging, because that is the answer
 * that is correct with or without a sync: the removal propagates like
 * any other delete.
 *
 * Returns how many tasks went.
 * ------------------------------------------------------------------------- */
guint task_ops_clear_completed(TaskApp *app, gint64 list_id);

#endif /* TASK_OPS_H */
