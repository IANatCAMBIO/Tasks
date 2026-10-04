#include "task_pane.h"

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
 * task_sel_list_id — the selected real list, or 0.
 * Inputs/output are documented in task_pane.h.
 */
int64_t
task_sel_list_id(task_sel_t sel)
{
    return sel.kind == task_sel_list ? sel.id : 0;
}

/*
 * task_sel_is_virtual — whether rows carry their "in <list>" line.
 * Inputs/output are documented in task_pane.h.
 */
bool
task_sel_is_virtual(task_sel_t sel)
{
    const TaskView *view = task_sel_view_of(sel);

    if (view != NULL) return view->virtual_rows;
    return sel.kind == task_sel_group;
}

/*
 * task_pane_collect — tasks of the selection, filtered and described.
 * Inputs/output are documented in task_pane.h.
 */
task_pane_content_t *
task_pane_collect(TaskApp *app, task_sel_t sel, const TaskSearch *search)
{
    /* A registered view answers for itself; a group shows the tasks of
     * every list under it, aggregated; anything else is a real list. */
    const TaskView *view  = task_sel_view_of(sel);
    TaskGroup      *group = NULL;   /* owns `where` for a group           */
    TaskList       *list  = NULL;   /* owns `where` for a list            */
    GPtrArray      *tasks;
    const gchar    *where = "?";
    const gchar    *unit  = "task";

    if (view != NULL) {
        tasks = view->query(app, view->user_data);
        where = view->name != NULL ? view->name : "";
        if (view->unit != NULL) unit = view->unit;
    } else if (sel.kind == task_sel_group) {
        tasks = task_db_tasks_in_group(app->db, sel.id);
        group = task_db_group_get(app->db, sel.id);
        where = group != NULL ? group->name : "";
    } else {
        tasks = task_db_tasks_toplevel(app->db, sel.id);
        list  = task_db_list_get(app->db, sel.id);
        if (list != NULL) where = list->name;
    }

    TaskRowCtx ctx;
    task_row_ctx_init(app, &ctx, task_sel_is_virtual(sel));

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

    /* The view names its own noun; only the plural "s" is ours. */
    if (asprintf(&content->status_text, "%s - %zu %s%s%s", where,
                 content->rows->count, search != NULL ? "matching " : "",
                 unit, content->rows->count == 1 ? "" : "s") < 0) {
        abort();
    }

    task_row_ctx_clear(&ctx);
    if (filtered != NULL) g_ptr_array_free(filtered, TRUE);
    task_ptr_array_free_tasks(tasks);
    task_group_free(group);
    task_list_free(list);
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
