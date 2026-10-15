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

/*
 * task_ops_default_tag — the configured default tag's name (see
 * task_ops.h).
 *
 * Inputs:
 *   app — the context (unused: a tag lives by its name, see task_ops.h)
 *
 * Output:
 *   the name (g_free), or NULL when none is set.
 */
gchar *
task_ops_default_tag(TaskApp *app)
{
    (void)app;
    gchar *name = task_config_get(TASK_CFG_DEFAULT_TAG);
    if (name != NULL) g_strstrip(name);
    if (name != NULL && name[0] == '\0') {
        g_free(name);
        name = NULL;
    }
    return name;
}
