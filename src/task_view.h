/* ===========================================================================
 * task_view.h — the sidebar's virtual views, as a registry.
 *
 * A "virtual view" is a sidebar row that is NOT a list: Favorites, All
 * Tasks, Due Today.  Each one used to be an SbKind enum member, and
 * adding one meant editing five places in library_window.c that had to
 * agree — the metas[] literal, its visibility gate, the refresh_tasks
 * switch, kanban_order_key and view_order_key — plus two "that is a
 * view, not a list" refusals.  Five parallel switches over one enum is a
 * table wearing a disguise.
 *
 * So a view is a row in a registry instead: a QUERY answering with a
 * GPtrArray of Task*, which the core renders — as a list or as a Kanban
 * board, with the manual sort, the stripes, the ✓ column and everything
 * else, for free.
 *
 * ORDER KEYS ARE DERIVED FROM `id`.  "manual_order_<id>" and
 * "kanban_order_<id>", which is exactly the existing spelling:
 * manual_order_all, kanban_order_today.  So `id` is part of the ini
 * format and must not be renamed once a view has shipped.
 *
 * Registration happens once at startup, before any window exists.
 * =========================================================================== */

#ifndef TASK_VIEW_H
#define TASK_VIEW_H

#include "app.h"

typedef struct TaskView TaskView;

struct TaskView {
    /* Stable identity.  Used to build the per-view config keys, so it is
     * part of the ini format: lowercase, no spaces, never renamed.        */
    const gchar *id;

    /* Sidebar row text: emoji + TWO spaces + name, matching how lists
     * render their own emoji.                                             */
    const gchar *label;

    /* The name the status bar shows for this view ("All Tasks").          */
    const gchar *name;

    /* Singular noun for the status bar's count — "task" gives
     * "12 tasks", "action item" gives "12 action items".  NULL means
     * "task".  Only the plural "s" is added, so a noun that pluralises
     * some other way does not belong here.                                */
    const gchar *unit;

    /* Display order among views, low first.  Ties keep registration
     * order.                                                              */
    gint sort;

    /* Whether the row exists at all right now — Favorites appears only
     * while something is pinned.  NULL means always.                      */
    gboolean (*visible)(TaskApp *app, gpointer user_data);

    /* The tasks to show, newly allocated, freed by the caller with
     * task_ptr_array_free_tasks.  Required.                               */
    GPtrArray *(*query)(TaskApp *app, gpointer user_data);

    /* Whether rows should carry their "in <list>" line.  A query view
     * that gathers tasks from across lists wants TRUE (the line is the
     * only thing saying where the task actually sits).                    */
    gboolean virtual_rows;

    /* Shown when the user tries to edit or delete this view as though it
     * were a list.  NULL falls back to a generic refusal.                 */
    const gchar *not_a_list;

    gpointer user_data;
};

/* ---------------------------------------------------------------------------
 * task_view_register() — add a view.  `v` is borrowed and must outlive
 * the app (a file-static struct).  Call once per view at startup, before
 * any window is built; the registry is unlocked, like the other startup
 * registries.  A view without a `query` is rejected.
 * ------------------------------------------------------------------------- */
void task_view_register(const TaskView *v);

/* ---------------------------------------------------------------------------
 * The registry, in display order (`sort`, then registration order).
 * Indices are stable for the life of the process, which is what lets the
 * sidebar store a view as an index in its id column.
 * ------------------------------------------------------------------------- */
guint            task_view_count(void);
const TaskView  *task_view_nth(guint index);

/* task_view_find() — by id, or NULL.                                      */
const TaskView  *task_view_find(const gchar *id);

/* task_view_index_of() — a view's index, or -1 when not registered.       */
gint             task_view_index_of(const TaskView *v);

/* ---------------------------------------------------------------------------
 * task_view_order_key() — "manual_order_<id>" / "kanban_order_<id>" for
 * a view.  New string.
 *
 * `family` is "manual_order" or "kanban_order".  The two families are
 * deliberately separate — a board drag must not silently rearrange a
 * list the user hand-sorted in the list view.
 * ------------------------------------------------------------------------- */
gchar *task_view_order_key(const TaskView *v, const gchar *family);

#endif /* TASK_VIEW_H */
