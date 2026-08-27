/* ===========================================================================
 * task_view.c — the virtual-view registry (see task_view.h)
 * =========================================================================== */

#include "task_view.h"
#include "plugin_owner.h"

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
    /* Exactly one of the two shapes.  Getting this wrong would show an
     * empty pane with no hint why, so refuse it loudly at startup.        */
    gboolean has_query = v->query != NULL;
    gboolean has_panel = v->panel_new != NULL;
    if (has_query == has_panel) {
        g_warning("task_view: \"%s\" must supply exactly one of query "
                  "or panel_new — not registered", v->id);
        return;
    }
    if (views == NULL)
        views = g_ptr_array_new();
    Entry *e = g_new0(Entry, 1);
    e->view = v;
    e->seq  = views->len;
    task_plugin_owner_stamp(e);
    g_ptr_array_add(views, e);
    g_ptr_array_sort(views, view_cmp);
}

/* ---------------------------------------------------------------------------
 * task_view_remove_owner() — drop every view registered by `owner`
 * (see task_view.h).  Walks backwards so removal cannot skip an entry.
 * ------------------------------------------------------------------------- */
void
task_view_remove_owner(const gchar *owner)
{
    if (views == NULL || owner == NULL)
        return;
    for (guint i = views->len; i > 0; i--) {
        Entry *e = g_ptr_array_index(views, i - 1);
        if (!task_plugin_owner_is(e, owner))
            continue;
        task_plugin_owner_forget(e);
        g_ptr_array_remove_index(views, i - 1);
        g_free(e);
    }
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

gboolean
task_view_is_panel(const TaskView *v)
{
    return v != NULL && v->panel_new != NULL;
}

gchar *
task_view_order_key(const TaskView *v, const gchar *family)
{
    if (v == NULL || task_view_is_panel(v))
        return NULL;                 /* nothing to order                    */
    return g_strdup_printf("%s_%s", family, v->id);
}
