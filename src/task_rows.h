/* ===========================================================================
 * task_rows.h — the task-row renderer, as a reusable widget.
 *
 * A "task row" in this app is not a line of text.  It is a tall markup
 * cell carrying the title, a glyph stack, an "in <list>" line, a notes
 * preview and up to four subtasks, drawn over alternating stripes, with
 * a ✓ column that writes back through the app's status rule.  Getting it
 * right involves several traps that are recorded in comments here and
 * nowhere else — the UTF-8 boundary the 120-char preview cap must land
 * on, the escaping every database string needs before it reaches Pango,
 * the blank-line gate that keeps a one-space note from silently making a
 * row taller.
 *
 * All of that used to live inside library_window.c.  The renderer lives
 * here so that every pane showing tasks — the task list, the Kanban
 * cards, and whatever views come later — gets exactly the same row
 * rather than a reimplementation that drifts.
 *
 * PERFORMANCE.  The expensive lookups — attachment counts, subtasks,
 * list names — are gathered ONCE per refresh into a TaskRowCtx and
 * shared across every row, because the alternative is a query per row.
 * Build the context, fill as many stores as you like from it, clear it.
 * Nothing here belongs in a cell data function: those run per DRAW.
 * =========================================================================== */

#ifndef TASK_ROWS_H
#define TASK_ROWS_H

#include "app.h"
#include "list_rows.h"

/* The alternating row tint (the app's pale blue), painted by CSS on the
 * task pane's even rows (task_list.c's stylesheet).                       */
#define ROW_TINT "#e8f2fb"

/* ---------------------------------------------------------------------------
 * TaskRowCtx — the shared lookups behind one refresh.
 *
 * Fields are private; the struct is exposed only so a caller can put one
 * on the stack.  Build with _init, use for as many stores as needed,
 * release with _clear.
 *
 * `virtual_view` decides whether rows carry their "in <list>" line: TRUE
 * for a view gathering tasks from several lists, where that line is the
 * only thing saying where a task actually lives.
 * ------------------------------------------------------------------------- */
typedef struct {
    GHashTable *att_counts;          /* task id → attachment count          */
    GPtrArray  *all_subs;            /* owns the subtask rows below         */
    GHashTable *subs_by_parent;      /* parent id → GPtrArray of borrowed   */
    GHashTable *list_names;          /* list id → name, NULL for list views */
    gboolean    bold;                /* the bold_task_titles setting        */
    gboolean    show_done;           /* the show_completed toggle           */
} TaskRowCtx;

void task_row_ctx_init(TaskApp *app, TaskRowCtx *ctx, gboolean virtual_view);
void task_row_ctx_clear(TaskRowCtx *ctx);

/* ---------------------------------------------------------------------------
 * task_rows_append() — append `tasks` to `store` (a GListStore of TaskRow)
 * through `ctx`, honoring the completed-visibility setting, as ONE splice.
 * Returns how many rows were actually appended, which is NOT tasks->len
 * when completed tasks are hidden.
 * ------------------------------------------------------------------------- */
guint task_rows_append(GListStore *store, GPtrArray *tasks,
                       const TaskRowCtx *ctx);

/* ---------------------------------------------------------------------------
 * task_rows_desc_markup() — just the markup cell, for a caller building
 * its own row (the Kanban cards do this).  New string (g_free).
 * ------------------------------------------------------------------------- */
gchar *task_rows_desc_markup(const Task *t, const gchar *list_name,
                             gint att_count, GPtrArray *subs,
                             const TaskRowCtx *ctx);

/* ---------------------------------------------------------------------------
 * task_rows_toggle_done() — what a click on the ✓ column means.
 *
 * Writes the row's new status through the app's rule (ticking means Done;
 * unticking means In Progress, because a task that was ticked has plainly
 * been worked on and New would lose that), then fires the task-pane
 * refresh.  There is no fade-out any more: a completed row that the
 * visibility setting hides goes with the refresh.
 *
 * Every pane with a checkbox uses THIS.  Separate copies of the same
 * twenty lines are how two of them eventually disagree about what a tick
 * means.
 * ------------------------------------------------------------------------- */
void task_rows_toggle_done(TaskApp *app, TaskRow *row);

#endif /* TASK_ROWS_H */
