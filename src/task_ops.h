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

#include <stdint.h>

#include "task_app.h"

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

/* ---------------------------------------------------------------------------
 * task_ops_default_list() — the default list (Settings: TASK_CFG_DEFAULT_LIST),
 * where New Task files a task when no list is selected and Quick Task
 * always does.
 *
 * The setting holds a list id; a list that has since been deleted (or a
 * setting written against another database) counts as no default, so the
 * caller never creates a task in a list the user cannot see.
 *
 * Returns the list id, or 0 when there is no usable default.
 * ------------------------------------------------------------------------- */
int64_t task_ops_default_list(TaskApp *app);

#endif /* TASK_OPS_H */
