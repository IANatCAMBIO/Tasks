#include "task_pane.h"
#include "task_config.h"
#include "task_ops.h"

#include <stdio.h>
#include <stdlib.h>

/* The id of the view a window starts on (core_views.c registers it). */
#define DEFAULT_VIEW_ID "all"

/*
 * task_sel_default — the All Tasks view.
 * Output is documented in task_pane.h.
 */
task_sel_t
task_sel_default(void)
{
    gint       index = task_view_index_of(task_view_find(DEFAULT_VIEW_ID));
    task_sel_t sel   = { task_sel_view, index >= 0 ? index : 0 };

    return sel;
}

/*
 * task_sel_view_of — the view behind a selection.
 * Inputs/output are documented in task_pane.h.
 */
const TaskView *
task_sel_view_of(task_sel_t sel)
{
    if (sel.kind != task_sel_view || sel.id < 0) return NULL;
    return task_view_nth((guint)sel.id);
}

/*
 * task_sel_tag_id — the selected tag, or 0.
 * Inputs/output are documented in task_pane.h.
 */
int64_t
task_sel_tag_id(task_sel_t sel)
{
    return sel.kind == task_sel_tag ? sel.id : 0;
}

/*
 * task_sel_button_id — a selection's Favorites bar button id.
 * Inputs/output are documented in task_pane.h.
 */
int64_t
task_sel_button_id(task_sel_t sel)
{
    if (sel.kind == task_sel_tag) return sel.id;
    return sel.kind == task_sel_view ? -(sel.id + 1) : 0;
}

/*
 * task_sel_from_button_id — the selection of a button id.
 * Inputs/output are documented in task_pane.h.
 */
task_sel_t
task_sel_from_button_id(int64_t id)
{
    task_sel_t sel = id < 0 ? (task_sel_t){ task_sel_view, -id - 1 }
                            : (task_sel_t){ task_sel_tag, id };
    return sel;
}

/*
 * task_view_is_favorite — whether a view is listed in the setting.
 * Inputs/output are documented in task_pane.h.
 */
bool
task_view_is_favorite(const TaskView *view)
{
    if (view == NULL) return false;

    gchar  *saved = task_config_get(TASK_CFG_FAVORITE_VIEWS);
    gchar **ids   = g_strsplit(saved != NULL ? saved : "", ",", -1);
    bool    found = g_strv_contains((const gchar *const *)ids, view->id);

    g_strfreev(ids);
    g_free(saved);
    return found;
}

/*
 * task_view_set_favorite — add the view to the setting, or remove it.
 * Inputs are documented in task_pane.h.
 */
void
task_view_set_favorite(const TaskView *view, bool favorite)
{
    if (view == NULL || task_view_is_favorite(view) == favorite) return;

    gchar   *saved = task_config_get(TASK_CFG_FAVORITE_VIEWS);
    gchar  **ids   = g_strsplit(saved != NULL ? saved : "", ",", -1);
    GString *out   = g_string_new("");

    for (gchar **id = ids; *id != NULL; id++) {
        if (**id == '\0' || g_strcmp0(*id, view->id) == 0) continue;
        g_string_append_printf(out, "%s%s", out->len > 0 ? "," : "", *id);
    }
    if (favorite) g_string_append_printf(out, "%s%s", out->len > 0 ? "," : "", view->id);
    task_config_set(TASK_CFG_FAVORITE_VIEWS, out->len > 0 ? out->str : NULL);

    g_string_free(out, TRUE);
    g_strfreev(ids);
    g_free(saved);
}

/*
 * task_sel_new_task_tag — the selected tag's name, else none.
 * Inputs/output are documented in task_pane.h.
 */
gchar *
task_sel_new_task_tag(TaskApp *app, task_sel_t sel)
{
    if (sel.kind != task_sel_tag) return NULL;

    TaskTag *tag  = task_db_tag_get(app->db, sel.id);
    gchar   *name = tag != NULL ? g_strdup(tag->name) : NULL;
    task_tag_free(tag);
    return name;
}

/*
 * task_pane_collect — tasks of the selection, filtered and described.
 * Inputs/output are documented in task_pane.h.
 */
task_pane_content_t *
task_pane_collect(TaskApp *app, task_sel_t sel, const TaskSearch *search)
{
    /* A registered view answers for itself; a tag shows every task
     * carrying it. */
    const TaskView *view      = task_sel_view_of(sel);
    gchar          *tag_where = NULL;   /* "#<tag>", owned, for a tag     */
    GPtrArray      *tasks;
    const gchar    *where     = "";

    if (view != NULL) {
        tasks = view->query(app, view->user_data);
        where = view->name != NULL ? view->name : "";
    } else {
        /* A tag is written as the sidebar's filter, "#<tag>", as Notes'
         * status bar writes it. */
        tasks = task_db_tasks_tagged(app->db, sel.id);
        TaskTag *tag = task_db_tag_get(app->db, sel.id);
        if (tag != NULL) {
            tag_where = g_strdup_printf("#%s", tag->name);
            where     = tag_where;
        }
        task_tag_free(tag);
    }

    TaskRowCtx ctx;
    task_row_ctx_init(app, &ctx);

    /* The search narrows the view IN PLACE, between the collection and the
     * presentation.  The filtered array BORROWS its elements; `tasks`
     * still owns them.  A subtask match surfaces its PARENT: subtask
     * titles are matched through the grouping the row context already
     * made, not a query per task. */
    GPtrArray *filtered = NULL;
    if (search != NULL) {
        filtered = g_ptr_array_new();
        for (guint i = 0; i < tasks->len; i++) {
            Task *t = g_ptr_array_index(tasks, i);
            if (task_search_matches(search, t,
                                    task_row_ctx_subtasks(&ctx, t))) {
                g_ptr_array_add(filtered, t);
            }
        }
    }

    task_pane_content_t *content = calloc(1, sizeof *content);
    if (content == NULL) abort();
    content->rows = task_rows_build(filtered != NULL ? filtered : tasks, &ctx);

    /* The location alone, as Notes' status bar: the sidebar already counts
     * the view's tasks.  A search's matches are counted, since nothing
     * else shows how many there are. */
    if (search == NULL) {
        content->status_text = strdup(where);
        if (content->status_text == NULL) abort();
    } else if (asprintf(&content->status_text, "%s - %zu matching task%s", where,
                        content->rows->count, content->rows->count == 1 ? "" : "s") < 0) {
        abort();
    }

    task_row_ctx_clear(&ctx);
    if (filtered != NULL) g_ptr_array_free(filtered, TRUE);
    task_ptr_array_free_tasks(tasks);
    g_free(tag_where);
    return content;
}

/*
 * task_pane_content_free — free a content.
 * Inputs are documented in task_pane.h.
 */
void
task_pane_content_free(task_pane_content_t *content)
{
    if (content == NULL) return;

    task_row_list_free(content->rows);
    free(content->status_text);
    free(content);
}
