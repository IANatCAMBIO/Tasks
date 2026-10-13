/* ===========================================================================
 * task_view.c — the virtual-view registry (see task_view.h)
 * =========================================================================== */

#include "task_view.h"

static GPtrArray *views = NULL;      /* const TaskView*, in display order   */

/* view_cmp() — `sort` ascending.  g_ptr_array_sort is not documented as
 * stable, so registration order is preserved explicitly by comparing the
 * seq stamped at registration when the sorts tie.                          */
typedef struct {
    const TaskView *view;
    guint           seq;
} Entry;

static gint
view_cmp(gconstpointer a, gconstpointer b)
{
    const Entry *ea = *(const Entry **)a;
    const Entry *eb = *(const Entry **)b;
    if (ea->view->sort != eb->view->sort)
        return ea->view->sort < eb->view->sort ? -1 : 1;
    return ea->seq < eb->seq ? -1 : (ea->seq > eb->seq ? 1 : 0);
}

void
task_view_register(const TaskView *v)
{
    if (v == NULL || v->id == NULL)
        return;
    /* A view without a query would show an empty pane with no hint why,
     * so refuse it loudly at startup.                                     */
    if (v->query == NULL) {
        g_warning("task_view: \"%s\" supplies no query — not registered",
                  v->id);
        return;
    }
    if (views == NULL)
        views = g_ptr_array_new();
    Entry *e = g_new0(Entry, 1);
    e->view = v;
    e->seq  = views->len;
    g_ptr_array_add(views, e);
    g_ptr_array_sort(views, view_cmp);
}

guint
task_view_count(void)
{
    return views != NULL ? views->len : 0;
}

const TaskView *
task_view_nth(guint index)
{
    if (views == NULL || index >= views->len)
        return NULL;
    return ((const Entry *)g_ptr_array_index(views, index))->view;
}

const TaskView *
task_view_find(const gchar *id)
{
    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *v = task_view_nth(i);
        if (g_strcmp0(v->id, id) == 0)
            return v;
    }
    return NULL;
}

gint
task_view_index_of(const TaskView *v)
{
    for (guint i = 0; i < task_view_count(); i++)
        if (task_view_nth(i) == v)
            return (gint)i;
    return -1;
}

/*
 * task_view_shown — whether a view's row exists right now.
 *
 * Inputs:
 *   v   — the view (not NULL)
 *   app — the context its `visible` reads
 *
 * Output:
 *   TRUE when the view has no `visible` or it answers TRUE.
 */
gboolean
task_view_shown(const TaskView *v, TaskApp *app)
{
    return v->visible == NULL || v->visible(app, v->user_data);
}

/*
 * task_view_shown_mask — the shown views as a bit per registry index.
 *
 * Inputs:
 *   app — the context
 *
 * Output:
 *   bit i set when task_view_nth(i) is shown (the first 64 views only).
 */
guint64
task_view_shown_mask(TaskApp *app)
{
    guint64 mask = 0;
    for (guint i = 0; i < task_view_count() && i < 64; i++)
        if (task_view_shown(task_view_nth(i), app))
            mask |= G_GUINT64_CONSTANT(1) << i;
    return mask;
}
