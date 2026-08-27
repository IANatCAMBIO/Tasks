/* ===========================================================================
 * task_ops.c — core task operations and their hooks (see task_ops.h)
 * =========================================================================== */

#include "task_ops.h"
#include "plugin_owner.h"

/* ---------------------------------------------------------------------------
 * The three registries.  Entries are never removed and registration
 * happens once at startup, before any worker thread exists, so no lock
 * is needed — the same contract task_db_add_delete_hook() documents.
 * ------------------------------------------------------------------------- */
typedef struct {
    gpointer fn;                     /* one of the three signatures         */
    gpointer user_data;
} Hook;

static GSList *moved_hooks   = NULL; /* Hook*, registration order           */
static GSList *cleared_hooks = NULL;
static GSList *list_vetoes   = NULL;

/* hook_add() — append one entry to `list` (shared by all three).           */
static void
hook_add(GSList **list, gpointer fn, gpointer user_data)
{
    if (fn == NULL)
        return;
    Hook *h = g_new0(Hook, 1);
    h->fn        = fn;
    h->user_data = user_data;
    task_plugin_owner_stamp(h);
    *list = g_slist_append(*list, h);
}

/* hook_remove_owner() — drop `owner`'s hooks from one list.               */
static void
hook_remove_owner(GSList **list, const gchar *owner)
{
    GSList *n = *list;
    while (n != NULL) {
        GSList *next = n->next;
        Hook   *h    = n->data;
        if (task_plugin_owner_is(h, owner)) {
            task_plugin_owner_forget(h);
            *list = g_slist_delete_link(*list, n);
            g_free(h);
        }
        n = next;
    }
}

/* ---------------------------------------------------------------------------
 * task_ops_remove_owner() — every op hook `owner` added (see task_ops.h).
 * ------------------------------------------------------------------------- */
void
task_ops_remove_owner(const gchar *owner)
{
    if (owner == NULL)
        return;
    hook_remove_owner(&moved_hooks,   owner);
    hook_remove_owner(&cleared_hooks, owner);
    hook_remove_owner(&list_vetoes,   owner);
}

void
task_ops_add_moved_hook(TaskOpsMovedFn fn, gpointer user_data)
{
    hook_add(&moved_hooks, (gpointer)fn, user_data);
}

void
task_ops_add_cleared_hook(TaskOpsClearedFn fn, gpointer user_data)
{
    hook_add(&cleared_hooks, (gpointer)fn, user_data);
}

void
task_ops_add_list_veto(TaskOpsListVetoFn fn, gpointer user_data)
{
    hook_add(&list_vetoes, (gpointer)fn, user_data);
}

/* ---------------------------------------------------------------------------
 * task_ops_move_to_list() — local move, then notify (see task_ops.h).
 * ------------------------------------------------------------------------- */
gboolean
task_ops_move_to_list(TaskApp *app, gint64 task_id, gint64 dest_list_id)
{
    Task *t = task_db_task_get(app->db, task_id);
    if (t == NULL || t->parent_id != 0 || t->list_id == dest_list_id) {
        task_free(t);
        return FALSE;
    }
    gint64 from_list = t->list_id;   /* read BEFORE the write moves it      */
    task_free(t);

    task_db_task_move_list(app->db, task_id, dest_list_id);

    for (GSList *l = moved_hooks; l != NULL; l = l->next) {
        Hook *h = l->data;
        ((TaskOpsMovedFn)h->fn)(app, task_id, from_list, dest_list_id,
                                h->user_data);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_ops_clear_completed() — tombstone the done tasks, then notify
 * (see task_ops.h).
 * ------------------------------------------------------------------------- */
guint
task_ops_clear_completed(TaskApp *app, gint64 list_id)
{
    GPtrArray *tasks = task_db_tasks_toplevel(app->db, list_id);
    GArray    *ids   = g_array_new(FALSE, FALSE, sizeof(gint64));

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        if (t->status == TASK_STATUS_DONE) {
            task_db_task_delete(app->db, t->id);
            g_array_append_val(ids, t->id);
        }
    }
    task_ptr_array_free_tasks(tasks);

    /* Hooks fire even for an empty clear: a remote side may still have
     * completed tasks of its own to archive, and it is the hook's job to
     * decide that, not ours.                                              */
    for (GSList *l = cleared_hooks; l != NULL; l = l->next) {
        Hook *h = l->data;
        ((TaskOpsClearedFn)h->fn)(app, list_id, ids, h->user_data);
    }

    guint n = ids->len;
    g_array_free(ids, TRUE);
    return n;
}

/* ---------------------------------------------------------------------------
 * task_ops_list_can_delete() — first refusal wins (see task_ops.h).
 * ------------------------------------------------------------------------- */
gboolean
task_ops_list_can_delete(TaskApp *app, const TaskList *list, gchar **why)
{
    if (why != NULL)
        *why = NULL;
    for (GSList *l = list_vetoes; l != NULL; l = l->next) {
        Hook *h = l->data;
        gchar *reason = NULL;
        if (!((TaskOpsListVetoFn)h->fn)(app, list, &reason, h->user_data)) {
            if (why != NULL)
                *why = reason;       /* hand the caller the ownership       */
            else
                g_free(reason);
            return FALSE;
        }
        g_free(reason);              /* a veto that passed but still spoke  */
    }
    return TRUE;
}
