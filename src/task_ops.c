/* ===========================================================================
 * task_ops.c — core task operations (see task_ops.h)
 * =========================================================================== */

#include "task_ops.h"
#include "task_config.h"

/*
 * task_ops_clear_completed — tombstone a tag's done tasks (see
 * task_ops.h).
 *
 * Inputs:
 *   app    — the context
 *   tag_id — the tag
 *
 * Output:
 *   how many top-level tasks went.
 */
guint
task_ops_clear_completed(TaskApp *app, gint64 tag_id)
{
    return task_db_tasks_delete_done(app->db, tag_id);
}
