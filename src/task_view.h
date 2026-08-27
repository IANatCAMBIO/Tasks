/* ===========================================================================
 * task_view.h — the sidebar's virtual views, as a registry.
 *
 * A "virtual view" is a sidebar row that is NOT a list: Favorites, All
 * Tasks, Due Today, Action Items, Weekly Forecast.  Each one used to be
 * an SbKind enum member, and adding one meant editing five places in
 * library_window.c that had to agree — the metas[] literal, its
 * visibility gate, the refresh_tasks switch, kanban_order_key and
 * view_order_key — plus two "that is a view, not a list" refusals.  Five
 * parallel switches over one enum is a table wearing a disguise, and a
 * view that ships separately cannot edit any of them.
 *
 * So a view is a row in a registry instead.  The core registers its own
 * through exactly this API, which is the point: if the four built-in
 * views cannot be expressed by it, it is not good enough for a plugin
 * either.
 *
 * TWO SHAPES OF VIEW
 * ------------------
 * Most views are a QUERY: they answer with a GPtrArray of Task*, and the
 * core renders them — as a list or as a Kanban board, with the manual
 * sort, the stripes, the ✓ column and everything else, for free.  Set
 * `query` and nothing else.
 *
 * The Weekly Forecast is the other shape: it is a PANEL, seven dated day
 * sections stacked in one scroller, and there is no task list for the
 * core to lay out.  Such a view supplies its own widget and owns what
 * happens inside it.  Set `panel_new` / `panel_refresh` instead, and
 * `panel_selection` so the toolbar's Delete Task can still ask what is
 * selected without knowing which pane is up.
 *
 * A view sets ONE of `query` or `panel_new`.  A panel view outranks
 * Kanban — there is nothing for a board to lay out — and has no card
 * order or manual order, because both are orderings OF a task list.
 *
 * ORDER KEYS ARE DERIVED FROM `id`.  "manual_order_<id>" and
 * "kanban_order_<id>", which is exactly the existing spelling:
 * manual_order_all, kanban_order_bn_actions.  So `id` is part of the ini
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
     * order, so a plugin that does not care can leave it 0 and land
     * after the core's.                                                   */
    gint sort;

    /* Whether the row exists at all right now — Favorites appears only
     * while something is pinned, Action Items only while the Notes
     * integration is on.  NULL means always.                              */
    gboolean (*visible)(TaskApp *app, gpointer user_data);

    /* QUERY VIEW: the tasks to show, newly allocated, freed by the caller
     * with task_ptr_array_free_tasks.  Mutually exclusive with panel_new. */
    GPtrArray *(*query)(TaskApp *app, gpointer user_data);

    /* Whether rows should carry their "in <list>" line.  A query view
     * that gathers tasks from across lists wants TRUE (the line is the
     * only thing saying where the task actually sits).                    */
    gboolean virtual_rows;

    /* PANEL VIEW: build the widget once, refresh it per change, and
     * report its selection.  Mutually exclusive with query.               */
    GtkWidget *(*panel_new)(TaskApp *app, gpointer user_data);
    void       (*panel_refresh)(TaskApp *app, GtkWidget *panel,
                                gpointer user_data);
    /* Selected task ids (gint64) as a new GArray, or NULL for none.  The
     * caller frees it.  NULL means "this panel has no selection", which
     * is a perfectly good answer — the forecast's day views deliberately
     * have none.                                                          */
    GArray    *(*panel_selection)(TaskApp *app, GtkWidget *panel,
                                  gpointer user_data);

    /* Shown when the user tries to edit or delete this view as though it
     * were a list.  NULL falls back to a generic refusal.                 */
    const gchar *not_a_list;

    gpointer user_data;
};

/* ---------------------------------------------------------------------------
 * task_view_register() — add a view.  `v` is borrowed and must outlive
 * the app (a file-static struct).  Call once per view at startup, before
 * any window is built; the registry is unlocked, like the other startup
 * registries.  A view supplying neither `query` nor `panel_new`, or
 * both, is rejected.
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

/* task_view_is_panel() — TRUE for a panel view (see the header comment). */
gboolean         task_view_is_panel(const TaskView *v);

/* ---------------------------------------------------------------------------
 * task_view_order_key() — "manual_order_<id>" / "kanban_order_<id>" for
 * a view, or NULL when it cannot be ordered (a panel view).  New string.
 *
 * `family` is "manual_order" or "kanban_order".  The two families are
 * deliberately separate — a board drag must not silently rearrange a
 * list the user hand-sorted in the list view.
 * ------------------------------------------------------------------------- */
gchar *task_view_order_key(const TaskView *v, const gchar *family);

/* ---------------------------------------------------------------------------
 * task_view_remove_owner() — remove everything plugin `owner`
 * registered here.  Called when a plugin is switched off while the app is
 * running; the app's OWN registrations are unowned and never match.
 * ------------------------------------------------------------------------- */
void task_view_remove_owner(const gchar *owner);

#endif /* TASK_VIEW_H */
