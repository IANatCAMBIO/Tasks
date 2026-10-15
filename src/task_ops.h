/* ===========================================================================
 * task_ops.h — core task operations.
 *
 * These are the operations that are LOCAL FIRST: the database write is
 * the operation.  They live here rather than in the window code because
 * they are the same whether or not any integration is watching, and the
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
 * task_ops_clear_completed() — tombstone every completed top-level task
 * carrying `tag_id` (subtasks go with their parents), wherever else it is
 * tagged.
 *
 * Tombstones rather than physically purging, because that is the answer
 * that is correct with or without a sync: the removal propagates like
 * any other delete.
 *
 * Returns how many tasks went.
 * ------------------------------------------------------------------------- */
guint task_ops_clear_completed(TaskApp *app, gint64 tag_id);

/* ---------------------------------------------------------------------------
 * task_ops_default_tag() — the default tag (Settings: TASK_CFG_DEFAULT_TAG),
 * which New Task gives a task when no tag is selected and Quick Task
 * always does.
 *
 * The setting holds a NAME, not an id: a tag exists only while a task
 * carries it, so the default may name a tag that has gone (or never
 * existed yet); task_db_task_create then makes it again.
 *
 * Returns the name (g_free), or NULL when no default is set.
 * ------------------------------------------------------------------------- */
gchar *task_ops_default_tag(TaskApp *app);

#endif /* TASK_OPS_H */
