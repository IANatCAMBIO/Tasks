/* ===========================================================================
 * task_ops.c — core task operations (see task_ops.h)
 * =========================================================================== */

#include "task_ops.h"
#include "task_config.h"

#include <stdbool.h>

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
    return task_db_tasks_delete_done(app->db, list_id);
}

/* ---------------------------------------------------------------------------
 * task_ops_default_list() — the configured default list, if it is still a
 * live list (see task_ops.h).
 * ------------------------------------------------------------------------- */
int64_t
task_ops_default_list(TaskApp *app)
{
    int64_t id = task_config_get_int(TASK_CFG_DEFAULT_LIST);
    if (id <= 0) return 0;

    TaskList *list = task_db_list_get(app->db, id);
    bool live = list != NULL && !list->deleted;
    task_list_free(list);
    return live ? id : 0;
}
