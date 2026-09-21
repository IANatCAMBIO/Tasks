/* ===========================================================================
 * list_rows.h — the item objects behind the GTK4 list widgets
 *
 * GtkColumnView and GtkListView show a GListModel of GObjects and render
 * each through a GtkListItemFactory; the tree stores and cell renderers
 * they replaced carried typed columns instead.  These are the two item
 * classes the library window lists: a task row (the task pane) and a
 * sidebar row (with its own children model, for GtkTreeListModel).  They
 * are plain data holders — public fields, no properties — because every
 * consumer is C in this program and binds by hand in its factory.
 *
 * Updating a row IN PLACE is task_row_touch: it emits the row's "changed"
 * signal, which the factories' bind handlers listen for
 * (task_row_factory_new wires that up), then items-changed (position, 1,
 * 1) on the store that holds it.  Both halves are needed: a
 * GtkSortListModel re-sorts on items-changed and a GtkMultiSelection keeps
 * the SAME object selected across it — but the list item manager REUSES
 * the widget of an item it finds re-added and does NOT rebind it
 * (gtklistfactorywidget.c: bind only when `item != old_item`), so a field
 * change is invisible without the signal.  That is the whole protocol;
 * there is no per-field notification.  (Notes D33; this file is a port of
 * ~/salt_development/notes/src/list_rows.[ch].)
 * =========================================================================== */

#ifndef TASK_LIST_ROWS_H
#define TASK_LIST_ROWS_H

#include <gtk/gtk.h>
#include "db.h"

/* ---------------------------------------------------------------------------
 * TaskListRow — the base of the two: nothing but the "changed" signal.
 * ------------------------------------------------------------------------- */
#define TASK_TYPE_LIST_ROW (task_list_row_get_type())
G_DECLARE_DERIVABLE_TYPE(TaskListRow, task_list_row, TASK, LIST_ROW, GObject)

struct _TaskListRowClass {
    GObjectClass parent_class;
};

/* ---------------------------------------------------------------------------
 * TaskRow — one task in the task pane, built by task_rows_append from a
 * Task and the shared TaskRowCtx.  The columns sort on the RAW fields and
 * show the formatted ones; the tall markup cell is `markup`.
 * ------------------------------------------------------------------------- */
#define TASK_TYPE_ROW (task_row_get_type())
G_DECLARE_FINAL_TYPE(TaskRow, task_row, TASK, ROW, TaskListRow)

struct _TaskRow {
    TaskListRow parent;
    gint64      id;                  /* task id (never 0 in the pane)       */
    TaskStatus  status;              /* the tri-state; `done` is derived    */
    gchar      *title;               /* plain title (the Task column's sort)*/
    gchar      *markup;              /* the tall cell: title, glyphs,
                                      * "in <list>", preview, subtasks      */
    gint64      due;                 /* local midnight, 0 = none            */
    gint        due_time;            /* minutes past midnight               */
    gint64      due_instant;         /* task_due_instant(due, due_time):
                                      * the Due column's sort key           */
    gchar      *due_text;            /* task_due_format_at, "" for none     */
    gint64      completed_at;        /* the Completed column's sort key     */
    gchar      *completed_text;      /* formatted, "" for none              */
    gchar      *status_text;         /* "New" / "In Progress" / "Done"      */
};

/* task_row_new() — an empty row (id 0, every string NULL).  Fields are
 * filled by the caller; strings are owned by the row.                     */
TaskRow *task_row_new(void);

/* task_row_done() — the ✓ column's view of the status.                    */
static inline gboolean
task_row_done(const TaskRow *r)
{
    return r->status == TASK_STATUS_DONE;
}

/* ---------------------------------------------------------------------------
 * TaskSbRow — one sidebar row: a registered view, the Lists header, a
 * list, or a list group.  `children` is the model a GtkTreeListModel
 * expands into (NULL for a leaf): the header holds the ungrouped lists and
 * the groups, a group holds its lists.
 * ------------------------------------------------------------------------- */
#define TASK_TYPE_SB_ROW (task_sb_row_get_type())
G_DECLARE_FINAL_TYPE(TaskSbRow, task_sb_row, TASK, SB_ROW, TaskListRow)

struct _TaskSbRow {
    TaskListRow parent;
    gint        kind;                /* SB_KIND_* (library_priv.h)          */
    gint64      id;                  /* view index, list id or group id     */
    gchar      *label;               /* display text (emoji + two spaces +
                                      * name for a list)                    */
    gboolean    bold;                /* the meta views and the headers      */
    GListStore *children;            /* TaskSbRow children, or NULL (leaf)  */
};

/* task_sb_row_new() — a row; `expandable` gives it an (empty) children
 * store.  The label is copied.                                             */
TaskSbRow *task_sb_row_new(gint kind, gint64 id, const gchar *label,
                           gboolean bold, gboolean expandable);

/* ---------------------------------------------------------------------------
 * task_row_touch() — a row's fields changed: every bound widget rebinds
 * ("changed"), and the store re-announces the item so a sorted model
 * re-sorts it.  Finds the row by identity.
 *   store — the GListStore holding `row`.
 *   row   — the row (either class).
 * Returns FALSE when the row is not in the store (the signal is still
 * emitted; nothing else happens).
 * ------------------------------------------------------------------------- */
gboolean task_row_touch(GListStore *store, gpointer row);

/* ---------------------------------------------------------------------------
 * task_row_factory_new() — a GtkSignalListItemFactory whose `bind` runs
 * again whenever the bound row emits "changed" (and stops listening on
 * unbind).  THE factory constructor for every list of these rows; an item
 * that is not one (a GtkTreeListRow wrapper) just binds once.
 *   setup     — the "setup" handler.
 *   bind      — the "bind" handler, also the rebind.
 *   user_data — passed to both.
 * Returns the factory (the caller hands it to a view or column).
 * ------------------------------------------------------------------------- */
GtkListItemFactory *task_row_factory_new(GCallback setup, GCallback bind,
                                         gpointer user_data);

#endif /* TASK_LIST_ROWS_H */
