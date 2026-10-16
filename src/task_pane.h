/*
 * task_pane — what the task pane shows for a sidebar selection.
 *
 * The sidebar selects ONE row: a registered view (All Tasks, All
 * Favorites, 3-Day Forecast — task_view.h) or a tag (every task carrying it).  This module names that
 * selection (task_sel_t) and collects its content: the tasks, filtered by
 * the search box, with completed ones left out when the setting hides
 * them, described as rows (task_rows.h), plus the status bar's line.
 *
 * The collection is shared by BOTH presentations.  The task list and the
 * Kanban board are handed the same rows and differ only in how they draw
 * them, which is what gives the board the search filter for nothing.
 *
 * Lifted from the GTK build's lib_refresh_tasks (library_window.c).  Plain
 * C interface; MAIN THREAD ONLY (it reads the app's database connection).
 */

#ifndef TASK_PANE_H
#define TASK_PANE_H

#include <stdbool.h>
#include <stdint.h>

#include "search.h"
#include "task_app.h"
#include "task_rows.h"
#include "task_view.h"

/* The kinds of sidebar row. */
typedef enum {
    task_sel_view = 0,              /* a registered view; id is its INDEX
                                     * in the registry (task_view_nth)    */
    task_sel_header,                /* the "Tags" header: never selected,
                                     * here so the sidebar has one enum   */
    task_sel_tag                    /* a tag; id is the tag id
                                     * (task_db_tasks_tagged)             */
} task_sel_kind_t;

/* The sidebar selection. */
typedef struct {
    task_sel_kind_t kind;
    int64_t         id;
} task_sel_t;

/*
 * task_sel_default — the selection a window starts on before the sidebar
 * has restored anything: the All Tasks view.
 *
 * Output:
 *   the selection (the first registered view if "all" is not registered).
 */
task_sel_t task_sel_default(void);

/*
 * task_sel_view_of — the registered view a selection sits on.
 *
 * Inputs:
 *   sel — the selection
 *
 * Output:
 *   the view (borrowed), or NULL when the selection is a tag.
 */
const TaskView *task_sel_view_of(task_sel_t sel);

/*
 * task_sel_tag_id — the selected tag.
 *
 * Inputs:
 *   sel — the selection
 *
 * Output:
 *   the tag id, or 0 when the selection is not a tag.
 */
int64_t task_sel_tag_id(task_sel_t sel);

/*
 * task_sel_new_task_tag — the tag New Task gives its task: the selected
 * tag, or — with a view selected — the default tag (task_ops_default_tag).
 * Every task carries a tag, so with neither there is no New Task.
 *
 * Inputs:
 *   app — the context (the selected tag's name is read from its database)
 *   sel — the selection
 *
 * Output:
 *   the tag's name (g_free), or NULL when there is none.
 */
gchar *task_sel_new_task_tag(TaskApp *app, task_sel_t sel);

/*
 * task_pane_content_t — the content of the task pane for one refresh.
 *
 *   rows        — the rows to show, in query order (owned)
 *   status_text — the status bar's left text: "<where> - N task(s)", or
 *                 "... N matching task(s)" while a search is active
 *                 (owned).  It is deliberately not "3 of 30": the total
 *                 would need a second copy of the completed-visibility
 *                 rule.
 */
typedef struct {
    task_row_list_t *rows;
    char            *status_text;
} task_pane_content_t;

/*
 * task_pane_collect — gather the pane's content for a selection.
 *
 * Inputs:
 *   app    — the application context (database open)
 *   sel    — the sidebar selection
 *   search — the parsed search query, or NULL for no filter (which is the
 *            ONE test for "is a search active")
 *
 * Output:
 *   new content (task_pane_content_free); never NULL.
 */
task_pane_content_t *task_pane_collect(TaskApp *app, task_sel_t sel,
                                       const TaskSearch *search);

/*
 * task_pane_content_free — free a content and everything it still owns.  A
 * caller that hands `rows` on to a pane sets the field to NULL first.
 *
 * Inputs:
 *   content — the content, or NULL (ignored)
 */
void task_pane_content_free(task_pane_content_t *content);

#endif /* TASK_PANE_H */
