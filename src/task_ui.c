/* ===========================================================================
 * task_ui.c — the window extension registries (see task_ui.h)
 * =========================================================================== */

#include "task_ui.h"

/* One entry, whatever kind.  `seq` preserves registration order when two
 * items sort equal — g_ptr_array_sort is not documented as stable.       */
typedef struct {
    gconstpointer def;
    gint          sort;
    guint         seq;
} Entry;

static GPtrArray *tools      = NULL;
static GPtrArray *menus      = NULL;
static GPtrArray *task_menus = NULL;
static GPtrArray *editors    = NULL;

/* The widget the window built for each contributed toolbar item, by id.
 * Window-private: rebuilt whenever the library window is.                 */
static GHashTable *tool_widgets = NULL;

static gint
entry_cmp(gconstpointer a, gconstpointer b)
{
    const Entry *ea = *(const Entry **)a;
    const Entry *eb = *(const Entry **)b;
    if (ea->sort != eb->sort)
        return ea->sort < eb->sort ? -1 : 1;
    return ea->seq < eb->seq ? -1 : (ea->seq > eb->seq ? 1 : 0);
}

static void
entry_add(GPtrArray **list, gconstpointer def, gint sort)
{
    if (def == NULL)
        return;
    if (*list == NULL)
        *list = g_ptr_array_new();
    Entry *e = g_new0(Entry, 1);
    e->def  = def;
    e->sort = sort;
    e->seq  = (*list)->len;
    g_ptr_array_add(*list, e);
    g_ptr_array_sort(*list, entry_cmp);
}

static gconstpointer
entry_nth(GPtrArray *list, guint i)
{
    if (list == NULL || i >= list->len)
        return NULL;
    return ((const Entry *)g_ptr_array_index(list, i))->def;
}

static guint
entry_count(GPtrArray *list)
{
    return list != NULL ? list->len : 0;
}

/* --- registration --------------------------------------------------------- */

void
task_ui_add_tool(const TaskUiToolDef *def)
{
    entry_add(&tools, def, def != NULL ? def->sort : 0);
}

void
task_ui_add_menu_item(const TaskUiMenuDef *def)
{
    entry_add(&menus, def, def != NULL ? def->sort : 0);
}

void
task_ui_add_task_menu_item(const TaskUiTaskMenuDef *def)
{
    entry_add(&task_menus, def, def != NULL ? def->sort : 0);
}

void
task_ui_add_editor_section(const TaskUiEditorDef *def)
{
    entry_add(&editors, def, def != NULL ? def->sort : 0);
}

/* --- enumeration ---------------------------------------------------------- */

guint                    task_ui_tool_count(void)      { return entry_count(tools); }
const TaskUiToolDef     *task_ui_tool_nth(guint i)     { return entry_nth(tools, i); }
guint                    task_ui_menu_count(void)      { return entry_count(menus); }
const TaskUiMenuDef     *task_ui_menu_nth(guint i)     { return entry_nth(menus, i); }
guint                    task_ui_task_menu_count(void) { return entry_count(task_menus); }
const TaskUiTaskMenuDef *task_ui_task_menu_nth(guint i){ return entry_nth(task_menus, i); }
guint                    task_ui_editor_count(void)    { return entry_count(editors); }
const TaskUiEditorDef   *task_ui_editor_nth(guint i)   { return entry_nth(editors, i); }

/* --- the toolbar widget map ----------------------------------------------- */

void
task_ui_tool_bind(const gchar *id, GtkWidget *widget)
{
    if (id == NULL || widget == NULL)
        return;
    if (tool_widgets == NULL)
        tool_widgets = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    g_hash_table_insert(tool_widgets, g_strdup(id), widget);
}

/* task_ui_tool_forget_all() — the window that owned these widgets is
 * going.  The widgets are NOT unreffed: the toolbar owned them and has
 * already destroyed them, so this drops dangling pointers rather than
 * releasing anything.                                                     */
void
task_ui_tool_forget_all(void)
{
    if (tool_widgets != NULL)
        g_hash_table_remove_all(tool_widgets);
}

void
task_ui_tool_set_sensitive(const gchar *id, gboolean sensitive)
{
    if (tool_widgets == NULL || id == NULL)
        return;
    GtkWidget *w = g_hash_table_lookup(tool_widgets, id);
    if (w != NULL)
        gtk_widget_set_sensitive(w, sensitive);
}

/* ---------------------------------------------------------------------------
 * task_ui_tools_apply_visibility() — re-ask every contributed item
 * whether it should be on screen (see task_ui.h).
 *
 * Runs on a full refresh rather than being pushed by whoever changed the
 * setting: a button tied to a config key would otherwise need its owner
 * to notice every route by which that key can change.
 * ------------------------------------------------------------------------- */
gboolean
task_ui_any_tool_visible(TaskApp *app)
{
    for (guint i = 0; i < task_ui_tool_count(); i++) {
        const TaskUiToolDef *d = task_ui_tool_nth(i);
        if (d->visible == NULL || d->visible(app, d->user_data))
            return TRUE;
    }
    return FALSE;
}

void
task_ui_tools_apply_visibility(TaskApp *app)
{
    if (tool_widgets == NULL)
        return;
    for (guint i = 0; i < task_ui_tool_count(); i++) {
        const TaskUiToolDef *d = task_ui_tool_nth(i);
        GtkWidget *w = g_hash_table_lookup(tool_widgets, d->id);
        if (w == NULL)
            continue;
        gtk_widget_set_visible(w, d->visible == NULL ||
                                  d->visible(app, d->user_data));
    }
}
