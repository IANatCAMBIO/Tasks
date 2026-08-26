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
 * All of that used to live inside library_window.c, where the Weekly
 * Forecast reached into it directly.  A plugin cannot: the forecast is
 * moving out to a shared object, and it needs SEVEN of these.  So the
 * renderer lives here, the app's own task pane uses it, and a plugin
 * gets exactly what the app has rather than a reimplementation that
 * drifts.
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

/* The alternating row tint (the app's pale blue).                          */
#define ROW_TINT "#e8f2fb"

/* ---------------------------------------------------------------------------
 * Store columns.  Any store a task row is written into has exactly this
 * shape; task_rows_store_new() builds one.
 * ------------------------------------------------------------------------- */
enum {
    TL_ID = 0,                       /* gint64: task id (0 = a placeholder
                                      * row, e.g. "No tasks due")          */
    TL_DONE,                         /* gboolean: status == TASK_STATUS_DONE,
                                      * the checkbox column's view of the
                                      * status — never stored separately   */
    TL_DESC,                         /* gchar*: the tall markup cell        */
    TL_DUE,                          /* gchar*: formatted due date ("")     */
    TL_DUE_RAW,                      /* gint64: due timestamp (sort/tint)   */
    TL_TITLE,                        /* gchar*: plain title (sorting)       */
    TL_COMPLETED,                    /* gchar*: formatted completion date   */
    TL_COMPLETED_RAW,                /* gint64: completion timestamp        */
    TL_STATUS,                       /* gint: TaskStatus (sorts by workflow)*/
    TL_STATUS_TEXT,                  /* gchar*: its label                   */
    TL_N_COLS
};

/* task_rows_store_new() — a GtkListStore of the shape above.              */
GtkListStore *task_rows_store_new(void);

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
 * task_rows_append() — append `tasks` to `store` through `ctx`, honoring
 * the completed-visibility setting.  Returns how many rows were actually
 * appended, which is NOT tasks->len when completed tasks are hidden.
 * ------------------------------------------------------------------------- */
guint task_rows_append(GtkListStore *store, GPtrArray *tasks,
                       const TaskRowCtx *ctx);

/* ---------------------------------------------------------------------------
 * task_rows_desc_markup() — just the markup cell, for a caller building
 * its own row (the Kanban cards do this).  New string (g_free).
 * ------------------------------------------------------------------------- */
gchar *task_rows_desc_markup(const Task *t, const gchar *list_name,
                             gint att_count, GPtrArray *subs, gboolean bold);

/* ---------------------------------------------------------------------------
 * task_rows_stripe_color() — the alternating tint for a row, or NULL for
 * the un-tinted one.  Exposed so a caller whose cell data function does
 * MORE than the stripe (urgency tint, a drag highlight) can apply both
 * from its single function — a renderer gets one data func, so they have
 * to share it.
 * ------------------------------------------------------------------------- */
const gchar *task_rows_stripe_color(GtkTreeModel *model, GtkTreeIter *iter);

/* task_rows_bg_func() — a ready-made data func that applies only the
 * stripe.  Pass NULL as user data.                                        */
void task_rows_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                       GtkTreeModel *model, GtkTreeIter *iter,
                       gpointer data);

/* ---------------------------------------------------------------------------
 * task_rows_toggle_done() — what a click on the ✓ column means.
 *
 * Reads the row's id, current state and title out of `store`, writes the
 * new status through the app's rule (ticking means Done; unticking means
 * In Progress, because a task that was ticked has plainly been worked on
 * and New would lose that), and then either fades the row out — when
 * completed tasks are hidden, so it does not vanish under the pointer —
 * or fires a refresh.
 *
 * A row whose id is 0 is a placeholder, not a task, and is ignored.
 *
 * Every pane with a checkbox uses THIS: the task pane, the seven Weekly
 * Forecast day views, and any plugin's.  They were separate copies of the
 * same twenty lines, which is how two of them eventually disagree about
 * what a tick means.
 * ------------------------------------------------------------------------- */
void task_rows_toggle_done(TaskApp *app, GtkListStore *store,
                           GtkTreeIter *iter);

#endif /* TASK_ROWS_H */
