/* ===========================================================================
 * task_ops.c — core task operations (see task_ops.h)
 * =========================================================================== */

#include "task_ops.h"

/* ---------------------------------------------------------------------------
 * task_ops_move_to_list() — the local move (see task_ops.h).
 * ------------------------------------------------------------------------- */
gboolean
task_ops_move_to_list(TaskApp *app, gint64 task_id, gint64 dest_list_id)
{
    Task *t = task_db_task_get(app->db, task_id);
    if (t == NULL || t->parent_id != 0 || t->list_id == dest_list_id) {
        task_free(t);
        return FALSE;
    }
    task_free(t);

    task_db_task_move_list(app->db, task_id, dest_list_id);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_ops_clear_completed() — tombstone the done tasks (see task_ops.h).
 * ------------------------------------------------------------------------- */
guint
task_ops_clear_completed(TaskApp *app, gint64 list_id)
{
    GPtrArray *tasks = task_db_tasks_toplevel(app->db, list_id);
    guint n = 0;

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        if (t->status == TASK_STATUS_DONE) {
            task_db_task_delete(app->db, t->id);
            n++;
        }
    }
    task_ptr_array_free_tasks(tasks);
    return n;
}
