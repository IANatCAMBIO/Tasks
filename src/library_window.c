/* ===========================================================================
 * library_window.c — the main Tasks window (see library_window.h): the
 * chrome, the menus, the refresh orchestration, and the task actions the
 * sidebar, the list and the board all share.  Its state, TaskLibrary, is
 * published to those three files through library_priv.h.
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include "task_ops.h"
#include "backup.h"
#include "task_worker.h"
#include "settings_window.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * lib_manual_sort_live() — may rows be hand-reordered RIGHT NOW?
 *
 * The setting alone is not the answer: a SEARCH is on, and both order
 * writers — task_view_save_manual_order and the board's card_drop_apply —
 * serialize the rows CURRENTLY IN THE PANE and nothing else.  Drag one row
 * while a filter hides the rest and the saved order comes back holding only
 * the matches, with every hidden task's hand-made position gone for good.
 * It is the same trap the sync's "ABSENCE NEVER DELETES" rule exists for:
 * a partial listing is not permission to throw away what is missing from
 * it.
 *
 * So dragging is refused while filtered rather than made lossy, and the
 * refusal is VISIBLE — the ⠿ handle column goes, the sort toggle greys
 * with its reason in the tooltip.  READING a saved order is unaffected:
 * lib_refresh_tasks still applies it to whatever survived the filter, which
 * keeps the matches in the order the user put them in.
 *
 * The board has no handle column to hide, so card_drop_apply skips its
 * ORDER half instead and lets the status change through — that drag is not
 * lossy, and the two halves are already independent there.
 * ------------------------------------------------------------------------- */
gboolean
lib_manual_sort_live(TaskLibrary *lw)
{
    return lw->manual_sort && lw->search == NULL;
}

/* lib_list_label() — a list's display label: the optional emoji prefixes
 * the name, set off by two spaces.  New string (g_free).                   */
gchar *
lib_list_label(const TaskList *l)
{
    return *l->emoji != '\0'
        ? g_strdup_printf("%s  %s", l->emoji, l->name)
        : g_strdup(l->name);
}

/* lib_of() — the TaskLibrary behind app->library_window.                   */
TaskLibrary *
lib_of(TaskApp *app)
{
    if (app->library_window == NULL)
        return NULL;
    return g_object_get_data(G_OBJECT(app->library_window), "task-library");
}

/* ===========================================================================
 * Per-module CSS (installed once at display priority via task_app_css_install).
 * =========================================================================== */

/* ===========================================================================
 * Refreshes.
 * =========================================================================== */

/* lib_scroll_keep_queue() — restore a scrolled window's vertical position
 * after a model rebuild (idle-deferred so the rebuilt view re-validates
 * its height first — Notes gotcha #11).                                   */
typedef struct {
    GtkAdjustment *vadj;
    gdouble        value;
} ScrollKeep;

static gboolean
scroll_keep_apply(gpointer data)
{
    ScrollKeep *sk = data;
    gtk_adjustment_set_value(sk->vadj,
        MIN(sk->value, gtk_adjustment_get_upper(sk->vadj) -
                       gtk_adjustment_get_page_size(sk->vadj)));
    g_object_unref(sk->vadj);
    g_free(sk);
    return G_SOURCE_REMOVE;
}

/* lib_scroll_keep_queue_win() — the same, given the scrolled window itself.   */
void
lib_scroll_keep_queue_win(GtkWidget *scroll)
{
    if (!GTK_IS_SCROLLED_WINDOW(scroll))
        return;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scroll));
    ScrollKeep *sk = g_new0(ScrollKeep, 1);
    sk->vadj  = g_object_ref(vadj);
    sk->value = gtk_adjustment_get_value(vadj);
    g_idle_add(scroll_keep_apply, sk);
}

void
lib_scroll_keep_queue(GtkWidget *view)
{
    lib_scroll_keep_queue_win(gtk_widget_get_parent(view));
}

/* ---------------------------------------------------------------------------
 * lib_refresh_sidebar() — rebuild the sidebar and restore the selection.
 * ------------------------------------------------------------------------- */

static void     on_toggle_kanban(TaskLibrary *lw);
static void     on_new_task(TaskLibrary *lw);
static void     on_delete_task(TaskLibrary *lw);
static void     on_task_info(TaskLibrary *lw);
static const gchar *manual_sort_tooltip(TaskLibrary *lw);

/* lib_sel_view() — the registered view the sidebar is sitting on, or NULL
 * when the selection is a list or a group.                                 */
const TaskView *
lib_sel_view(TaskLibrary *lw)
{
    if (lw->sel_kind != SB_KIND_VIEW)
        return NULL;
    return task_view_nth((guint)lw->sel_id);
}

/* lib_view_refuse() — a virtual view is not a list, so Edit List / Delete
 * List have nothing to act on.  Posts the view's own explanation (or a
 * generic one) and returns TRUE when the caller should stop.
 *
 * `alternative` completes the sentence for a view that did not supply
 * its own `not_a_list` text.                                              */
gboolean
lib_view_refuse(TaskLibrary *lw, const gchar *alternative)
{
    const TaskView *v = lib_sel_view(lw);
    if (v == NULL)
        return FALSE;
    if (v->not_a_list != NULL)
        task_app_status(lw->app, "%s", v->not_a_list);
    else
        task_app_status(lw->app,
                        "%s is a view, not a list \xe2\x80\x94 %s",
                        v->name != NULL ? v->name : v->id, alternative);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * The board's selection: a SET of task ids, mirroring the task view's
 * GTK_SELECTION_MULTIPLE.
 *
 * Order is never stored.  Every consumer that needs a sequence takes it
 * from the board's DISPLAY order (lib_card_sel_ids), so a multi-selection acts
 * top-to-bottom, lane by lane, the way it looks on screen.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * The hand-rolled card drag.
 *
 * lib_card_drag_stop() is the ONE way out — release, Escape, a broken grab
 * and window teardown all funnel through it, so the grab can never be
 * left held and the ghost can never be orphaned.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Card order within a lane.
 *
 * ONE config key per view (`kanban_order_<view>`, mirroring the manual
 * sort's `manual_order_<view>`) holding EVERY card of that view as a
 * comma-separated id list, lane by lane in display order.  One list
 * rather than three because the lanes already filter by status, so the
 * concatenation projects onto each lane correctly — and one key per view
 * is one key to delete when a list goes (see on_delete_list).
 *
 * Deliberately its OWN key family, not the manual sort's: reordering a
 * board must not silently rearrange a list the user had hand-sorted in
 * the list view, and board ordering is always live (dragging is the
 * gesture) where manual sort is behind a toggle.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * task_pane_mode_apply() — show exactly ONE of the three task-pane
 * variants.  The single place that answers "which pane is on screen":
 * lib_refresh_tasks calls it, and so does the construction path after
 * show_all has made both visible at once.
 * ------------------------------------------------------------------------- */
static void
task_pane_mode_apply(TaskLibrary *lw)
{
    gboolean kanban = lw->board.kanban;
    gtk_widget_set_visible(lw->task_scroll, !kanban);
    gtk_widget_set_visible(lw->board.kanban_box, kanban);

    /* The sort toggle is INERT while Kanban View is on: the board is
     * always drag-sorted (its own per-lane order, kanban_order_*), and the
     * list view it governs is not reachable at all in that mode.  So grey
     * it out rather than leaving a control that silently does nothing.
     *
     * The TOOLBAR twin greys with it — one control in two places, and
     * leaving the button live would let a click change a setting the menu
     * has just declared unavailable.                                       */
    /* The pane controls name the pane a click switches TO, and this is the
     * single place that answers "which pane is on screen" — so both the
     * menu label and the toolbar button's icon are set here rather than in
     * the handler, and a kanban flag changed by any other route still
     * reaches them.
     *
     * The ICON names the action too, the same rule the completed-visibility
     * button follows — and BOTH faces come from menu.png, the bulleted
     * list: upright it offers the list, turned a quarter turn clockwise
     * its bullets sit on top of three vertical bars, which is a board.
     * One image, so the two faces cannot drift apart, and the turn is a
     * whole quarter on the pixbuf so nothing is resampled.  (menu.png is
     * also why the sort button wears neither of its old pictures — two
     * buttons wearing the same image read as one control.)              */
    lib_menu_pair_sync(lw, "pane-list", "pane-kanban", lw->board.kanban);
    if (lw->pane_item != NULL) {
        /* board.png is derived ONCE from menu.png by a quarter-turn clockwise,
         * so the two faces cannot drift apart.  No runtime rotation needed.  */
        task_app_tool_item_set_icon(lw->app, lw->pane_item,
            lw->board.kanban ? "menu" : "board",
            "\xe2\x96\xa6");
        task_app_set_tooltip(lw->pane_item,
            lw->board.kanban ? "Show the tasks as a list"
                             : "Show the tasks as a Kanban board");
    }

    /* Two reasons the sort control can be unavailable, and they get
     * DIFFERENT tooltips: one control greyed for two unrelated causes is
     * only honest if it says which one is in force.                       */
    gboolean sortable = !lw->board.kanban && lw->search == NULL;
    /* The sort PAIR is this function's to set, not manual_sort_icon_refresh's:
     * a greyed pair is "neither face", and this runs last on every path
     * that can change either the mode or the reason.                     */
    lib_app_action_set_enabled(lw, "sort-manual", sortable && !lw->manual_sort);
    lib_app_action_set_enabled(lw, "sort-auto",   sortable &&  lw->manual_sort);
    if (lw->manual_sort_item != NULL) {
        gtk_widget_set_sensitive(lw->manual_sort_item, sortable);
        /* The reason rides on the toolbar button, since a menu item from a
         * model carries no tooltip.                                        */
        task_app_set_tooltip(lw->manual_sort_item,
            sortable      ? manual_sort_tooltip(lw)
            : lw->board.kanban  ? "The Kanban board is always drag-sorted \xe2\x80\x94 "
                            "turn Kanban View off to change list sorting"
                          : "A search hides rows, and saving an order from "
                            "a filtered list would lose the hidden tasks' "
                            "places \xe2\x80\x94 clear the search box to "
                            "drag-sort again");
    }
}

/* ---------------------------------------------------------------------------
 * lib_refresh_tasks() — rebuild the task pane for the current selection.
 * With Kanban View on, every view renders its tasks as a board instead
 * of a list — the collection below is shared, only the presentation
 * differs.
 * ------------------------------------------------------------------------- */
void
lib_refresh_tasks(TaskLibrary *lw)
{
    gboolean kanban = lw->board.kanban;
    task_pane_mode_apply(lw);

    if (!kanban)
        lib_scroll_keep_queue(lw->task_view);
    /* Kanban mode keeps task_store EMPTY so Delete Task does not act on a
     * list-view selection while the board is showing.  For the list view,
     * task_rows_append() does a single atomic g_list_store_splice() that
     * replaces old items with new ones — no blank frame between remove and
     * refill, which is what caused the checkbox-toggle flicker on white
     * (non-selected) rows.                                                 */
    if (kanban)
        g_list_store_remove_all(lw->task_store);

    /* Collect the tasks of the current view.  A registered view answers
     * for itself (see task_view.h); anything else is a real list.          */
    const TaskView *view = lib_sel_view(lw);
    GPtrArray *tasks;                /* Task* rows to show                */
    gboolean virtual_view;           /* show the "in <list>" line           */
    const gchar *view_name = "";
    const gchar *unit      = "task";
    TaskGroup   *sel_group = NULL;   /* owns view_name for a group row      */
    if (view != NULL) {
        tasks        = view->query(lw->app, view->user_data);
        virtual_view = view->virtual_rows;
        view_name    = view->name != NULL ? view->name : "";
        if (view->unit != NULL)
            unit = view->unit;
    } else if (lw->sel_kind == SB_KIND_GROUP) {
        /* A group shows the tasks of every list under it, aggregated.  It
         * is VIRTUAL in exactly the sense the registered views are: the
         * rows come from several lists at once, so each keeps its
         * "in <list>" line — which is the only thing telling two
         * identically-titled tasks in different lists apart, and the
         * reason a group is worth selecting rather than each list in
         * turn.  Everything below this point is the shared path, so the
         * board, the search box and the manual order come for free.       */
        tasks        = task_db_tasks_in_group(lw->app->db, lw->sel_id);
        virtual_view = TRUE;
        sel_group    = task_db_group_get(lw->app->db, lw->sel_id);
        if (sel_group != NULL)
            view_name = sel_group->name;
    } else {
        tasks        = task_db_tasks_toplevel(lw->app->db, lw->sel_id);
        virtual_view = FALSE;
    }

    TaskRowCtx ctx;                  /* shared lookups (see above)          */
    task_row_ctx_init(lw->app, &ctx, virtual_view);

    /* The toolbar search box narrows the view IN PLACE, so its scope is
     * whatever the sidebar has selected — this list, or All Tasks for a
     * search across every one of them.  Filtering here, between the
     * collection and the presentation, is what gives the board the same
     * filter as the list for free: below this line the two branches differ
     * only in how they draw the tasks they were handed.
     *
     * The filtered array BORROWS its elements; `tasks` still owns them and
     * still frees them at the end.  Subtasks come from the row context that
     * was just built rather than a query of our own — it has already
     * grouped every visible subtask by parent for the row markup, and
     * asking the database again per task is exactly what that grouping
     * exists to avoid.                                                     */
    GPtrArray *filtered = NULL;      /* the matches, or NULL when unfiltered*/
    if (lw->search != NULL) {
        filtered = g_ptr_array_new();
        for (guint i = 0; i < tasks->len; i++) {
            Task *t = g_ptr_array_index(tasks, i);
            GPtrArray *subs = t->parent_id == 0
                ? g_hash_table_lookup(ctx.subs_by_parent,
                                      GINT_TO_POINTER(t->id))
                : NULL;
            if (task_search_matches(lw->search, t, subs))
                g_ptr_array_add(filtered, t);
        }
    }
    GPtrArray *rows = filtered != NULL ? filtered : tasks;

    guint shown = kanban
        ? lib_refresh_kanban(lw, rows, &ctx)
        : task_rows_append(lw->task_store, rows, &ctx);
    task_row_ctx_clear(&ctx);
    if (filtered != NULL)
        g_ptr_array_free(filtered, TRUE);   /* elements belong to `tasks`   */

    /* Reorder to match the saved manual order.  No-op when the mode is
     * off, and skipped entirely on the board — a manual order is a
     * position within ONE list, which three status lanes have no place
     * for (and the reorder walks task_store, which is empty here).        */
    if (lw->manual_sort && !kanban)
        task_view_apply_manual_order(lw);

    /* Status bar left: where we are + how many rows.                       */
    TaskList *sel_list = virtual_view ? NULL
                       : task_db_list_get(lw->app->db, lw->sel_id);
    const gchar *where = virtual_view    ? view_name
                       : sel_list != NULL ? sel_list->name : "?";
    /* The view names its own noun ("action item" reads better than
     * "task" for a mirrored list); only the plural "s" is ours.
     *
     * While a search is running the count says "matching", because a bare
     * count would claim the view holds three tasks when it holds thirty
     * and is showing three.  It is deliberately NOT "3 of 30": the total
     * would have to re-apply the completed-visibility rule that
     * task_rows_append already owns, and a third copy of that test is how
     * the three drift apart.                                              */
    gchar *loc = g_strdup_printf("%s - %u %s%s%s", where, shown,
                                 lw->search != NULL ? "matching " : "",
                                 unit, shown == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
    task_list_free(sel_list);
    task_group_free(sel_group);      /* view_name pointed into it           */

    task_ptr_array_free_tasks(tasks);
}

/* lib_full_refresh() — sidebar + task pane + open editors.                    */
void
lib_full_refresh(TaskLibrary *lw)
{
    lib_refresh_sidebar(lw);
    lib_refresh_tasks(lw);
    task_editor_refresh_all(lw->app);
}

/* ---------------------------------------------------------------------------
 * on_search_changed() — the toolbar search box's text changed: re-compile
 * the query and redraw the task pane through it.
 *
 * Only the TASK PANE is rebuilt, never the sidebar: a search narrows what
 * is shown of the selected view, and rebuilding the sidebar from a
 * keystroke would snapshot and restore the Lists expansion on every
 * character typed.
 *
 * GtkSearchEntry holds "search-changed" back until typing pauses, so this
 * does not run per keystroke; "activate" (Enter) is wired here too so the
 * filter lands at once for someone who types and immediately presses it.
 * The clear icon emits "search-changed" like any other edit, which is what
 * puts the whole view back.
 * ------------------------------------------------------------------------- */
static void
on_search_changed(GtkWidget *entry, gpointer data)
{
    TaskLibrary *lw = data;
    task_search_free(lw->search);
    lw->search = task_search_parse(gtk_editable_get_text(GTK_EDITABLE(entry)));
    /* Hand-sorting is suspended while a filter is up and comes back when
     * it clears (lib_manual_sort_live says why), so the ⠿ handle column has to
     * be re-applied on the way through — BEFORE the rows are rebuilt, so
     * the pane is drawn once, in the shape it is about to keep.           */
    task_manual_sort_apply(lw);
    lib_refresh_tasks(lw);
}

/* on_search_stopped() — Escape in the search box: empty it, which fires
 * "search-changed" and so drops the filter through the one path above.
 * GtkSearchEntry raises the signal but does not clear itself — that is
 * GtkSearchBar's job, and there is no search bar here.                     */
static void
on_search_stopped(GtkWidget *entry, gpointer data)
{
    (void)data;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

/* hide_done_icon_refresh() — point the completed-visibility toggle's
 * icon + tooltip at the ACTION it offers: hidden.png while completed
 * tasks are visible (click to hide them), visible.png while they are
 * hidden (click to bring them back).  The View menu's twin gets the
 * matching LABEL, "Hide Completed" / "Show Completed".                     */
static void
hide_done_icon_refresh(TaskLibrary *lw)
{
    gboolean show = task_app_config_get_bool("show_completed", TRUE);
    task_app_tool_item_set_icon(lw->app, lw->hide_done_item,
        show ? "hidden" : "visible", "\xf0\x9f\x91\x81");
    task_app_set_tooltip(lw->hide_done_item,
        show ? "Hide completed tasks" : "Show completed tasks");
    /* The menu twin says the same thing in words: the hidden-when pair.   */
    lib_menu_pair_sync(lw, "done-hide", "done-show", show);
}

/* on_toggle_done_visible() — the toolbar toggle behind it: flip the
 * persisted show_completed flag and rebuild the task pane.                 */
static void
on_toggle_done_visible(TaskLibrary *lw)
{
    gboolean show = !task_app_config_get_bool("show_completed", TRUE);
    task_app_config_set("show_completed", show ? "1" : "0");
    hide_done_icon_refresh(lw);
    lib_refresh_tasks(lw);
}

/* manual_sort_tooltip() — what a click on the Sort Mode button will do,
 * from the cached mode.  One spelling for the button's normal state, read
 * by manual_sort_icon_refresh and by task_pane_mode_apply when the
 * greying lifts.                                                           */
static const gchar *
manual_sort_tooltip(TaskLibrary *lw)
{
    return lw->manual_sort ? "Switch to automatic sorting"
                           : "Switch to manual drag sorting";
}

/* manual_sort_icon_refresh() — swap the sort-mode button's icon and tooltip
 * to the ACTION a click performs, the rule every toggle on this toolbar
 * follows: automatic.png (a gear selector) while MANUAL sorting is in
 * force, because the click on offer is "sort automatically"; manual.png
 * (a gearstick) while AUTOMATIC sorting is in force (the column headers
 * doing it), because the click on offer is "let me drag them".  So the picture is always the mode you are
 * switching TO, matching the tooltip beside it and the View menu's label.
 * NOT menu.png either way — that is the pane button's "show me the list"
 * picture, and one image on two buttons reads as one control.             */
static void
manual_sort_icon_refresh(TaskLibrary *lw)
{
    gboolean manual = lw->manual_sort;   /* every caller applies first      */
    task_app_tool_item_set_icon(lw->app, lw->manual_sort_item,
        manual ? "automatic" : "manual", "\xe2\x89\x8b");
    task_app_set_tooltip(lw->manual_sort_item, manual_sort_tooltip(lw));
    /* The menu pair is NOT set here: task_pane_mode_apply owns it, since
     * the greying it applies is part of the same answer.                  */
}

/* on_toggle_manual_sort() — toolbar button that flips task_list_manual_sort
 * and refreshes the pane.                                                  */
static void
on_toggle_manual_sort(TaskLibrary *lw)
{
    gboolean manual = !lw->manual_sort;
    task_app_config_set("task_list_manual_sort", manual ? "1" : "0");
    task_manual_sort_apply(lw);
    manual_sort_icon_refresh(lw);
    lib_refresh_tasks(lw);
}

/* notify_changed_hook() / notify_tasks_hook() / notify_status_hook() —
 * the window's subscriptions to the three TaskApp events (see app.h).
 * Each re-resolves the window through lib_of() rather than trusting the
 * user_data pointer: an event can arrive from a worker's idle callback
 * after the window has gone.                                              */
static void
notify_changed_hook(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    TaskLibrary *lw = lib_of(app);
    if (lw != NULL)
        lib_full_refresh(lw);
}

/* The light variant: task pane only (editor saves — see editor_notify).
 * One exception: an editor's pin flip can be the first/last pin, which
 * adds/removes the sidebar's Pinned Tasks row — rebuild the sidebar
 * only on that 0 <-> nonzero transition (it never runs the BN CLI).        */
static void
notify_tasks_hook(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    TaskLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    if (lib_sidebar_show_pinned(lw) != lw->pinned_row_shown)
        lib_refresh_sidebar(lw);
    lib_refresh_tasks(lw);
}

/* ---------------------------------------------------------------------------
 * Status-bar visibility: show the message on a GtkRevealer, crossfade it
 * away after a 3 s hold.  GTK4 has no alpha markup for labels; the
 * GtkRevealer with CROSSFADE does the equivalent without 20 timer steps.
 * ------------------------------------------------------------------------- */
#define STATUS_HOLD_SECONDS 3

static gboolean
on_status_hide(gpointer data)
{
    TaskLibrary *lw = lib_of((TaskApp *)data);
    if (lw == NULL)
        return G_SOURCE_REMOVE;
    lw->status_hide_source = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_reveal), FALSE);
    return G_SOURCE_REMOVE;
}

static void
notify_status_hook(TaskApp *app, const gchar *message, gpointer user_data)
{
    (void)user_data;
    TaskLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    if (lw->status_hide_source != 0) {
        g_source_remove(lw->status_hide_source);
        lw->status_hide_source = 0;
    }
    gtk_label_set_text(GTK_LABEL(lw->status_right), message);
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_reveal), TRUE);
    lw->status_hide_source =
        g_timeout_add_seconds(STATUS_HOLD_SECONDS, on_status_hide, app);
}

/* ===========================================================================
 * Sidebar behavior.
 * =========================================================================== */

/* ===========================================================================
 * Task pane behavior.
 * =========================================================================== */

/* selected_task_ids() — ids of every selected task (both panes are
 * multi-select: Ctrl/Cmd-click and Shift-click extend).  Free with
 * g_array_unref.  Rows with id 0 are excluded.
 *
 * On the Kanban board the tree view is hidden and its store deliberately
 * empty, so the BOARD's selection answers instead, in display order —
 * which is what keeps Delete Task (toolbar, floating pair, File menu) and
 * the context menu working there without any call site knowing which pane
 * is up, and what makes a bulk action apply to a multi-card selection.   */
static GArray *
selected_task_ids(TaskLibrary *lw)
{
    if (lw->board.kanban)
        return lib_card_sel_ids(lw);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkBitset *sel = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(lw->task_sel));
    GtkBitsetIter iter;
    guint pos;
    gboolean ok = gtk_bitset_iter_init_first(&iter, sel, &pos);
    while (ok) {
        TaskRow *row = g_list_model_get_item(
            G_LIST_MODEL(lw->task_sorted), pos);
        if (row != NULL) {
            if (row->id != 0)
                g_array_append_val(ids, row->id);
            g_object_unref(row);
        }
        ok = gtk_bitset_iter_next(&iter, &pos);
    }
    return ids;
}

/* ===========================================================================
 * Toolbar actions.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * compact_layout_apply() — put the window in (or take it out of) Compact
 * Layout, per the persisted `compact_layout` flag.
 *
 * Compact hides the whole top toolbar (and its rule) and shows the
 * floating New/Delete Task pair pinned to the bottom-right of the task
 * area instead.  It does NOT touch the lists pane: the sidebar follows
 * the user's own `sidebar_visible` preference in both modes, so entering
 * compact no longer makes an open sidebar vanish (it used to force-hide
 * it, which read as compact silently overriding the toggle — and the
 * Show Sidebar override it left behind was the only way back).
 *
 * gtk_widget_show() — never show_all() — on the toolbar: its children
 * carry their own visibility (a hidden Sync button must stay hidden).
 *
 * This is also the single place that labels the View item, for the same
 * reason task_pane_mode_apply labels the pane item: the label names what
 * a click DOES, and putting it here means a flag changed by any other
 * route still reaches the menu.
 * ------------------------------------------------------------------------- */
static void
compact_layout_apply(TaskLibrary *lw)
{
    gboolean compact = task_app_config_get_bool("compact_layout", FALSE);

    gtk_widget_set_visible(lw->toolbar,      !compact);
    gtk_widget_set_visible(lw->toolbar_rule, !compact);
    gtk_widget_set_visible(lw->float_bar,     compact);
    gtk_widget_set_visible(lw->sidebar_box,
        task_app_config_get_bool("sidebar_visible", FALSE));
    lib_sidebar_ui_sync(lw);

    lib_menu_pair_sync(lw, "controls-full", "controls-compact", compact);

    /* Compact Controls takes the whole toolbar away, search box included,
     * so a filter left running would go on hiding tasks with nothing left
     * on screen to say why or to switch it off — a worse trap than a
     * control that does nothing, because the pane looks like the data.
     * Emptying the box drops the filter through the ordinary
     * "search-changed" path, so there is no second place that knows how to
     * clear a search.  Only when there is one: an unconditional set_text
     * would refresh the pane on every layout toggle.                       */
    if (compact && lw->search != NULL && lw->search_entry != NULL)
        gtk_editable_set_text(GTK_EDITABLE(lw->search_entry), "");
}

/* on_new_task() — create an empty task in the selected list and open its
 * editor.  The virtual views cannot hold new tasks.                        */
static void
on_new_task(TaskLibrary *lw)
{
    gint64 list_id = lib_selected_list_id(lw);
    if (list_id == 0) {
        /* A group holds several lists, so "the selected list" has no
         * answer there — say which of the two refusals this is rather
         * than calling a group a virtual view.                            */
        task_app_status(lw->app, "Select a list first \xe2\x80\x94 %s",
                        lw->sel_kind == SB_KIND_GROUP
                        ? "a group has no one list to create the task in"
                        : "tasks cannot be created in the virtual views");
        return;
    }
    gint64 id = task_db_task_create(lw->app->db, list_id, 0, "New Task");
    if (id == 0) {                   /* write failed (logged by the db)     */
        task_app_status(lw->app, "Could not create the task \xe2\x80\x94 "
                        "database write failed");
        return;
    }
    lib_full_refresh(lw);
    task_editor_open_new(lw->app, id);  /* the Save / Cancel variant        */
}

/* on_delete_confirm() — the async confirmation callback for on_delete_task. */
typedef struct {
    TaskApp *app;
    GArray  *ids;
} DeleteConfirmCtx;

static void
on_delete_confirm(gboolean yes, gpointer data)
{
    DeleteConfirmCtx *ctx = data;
    if (yes) {
        for (guint i = 0; i < ctx->ids->len; i++) {
            gint64 id = g_array_index(ctx->ids, gint64, i);
            GtkWindow *editor =
                g_hash_table_lookup(ctx->app->editors, &id);
            if (editor != NULL)
                gtk_window_destroy(editor);
            task_db_task_delete(ctx->app->db, id);
        }
        TaskLibrary *lw = lib_of(ctx->app);
        if (lw != NULL) {
            lib_full_refresh(lw);
            task_app_status(ctx->app, "Deleted %u task%s", ctx->ids->len,
                            ctx->ids->len == 1 ? "" : "s");
        }
    }
    g_array_unref(ctx->ids);
    g_free(ctx);
}

/* on_delete_task() — confirm + tombstone the selected task.                */
static void
on_delete_task(TaskLibrary *lw)
{
    GArray *ids = selected_task_ids(lw);
    if (ids->len == 0) {
        task_app_status(lw->app, "Select a task to delete");
        g_array_unref(ids);
        return;
    }

    gchar *title, *message;
    if (ids->len == 1) {
        Task *t = task_db_task_get(lw->app->db,
                                   g_array_index(ids, gint64, 0));
        if (t == NULL) {
            g_array_unref(ids);
            return;
        }
        title   = g_strdup("Delete Task");
        message = g_strdup_printf(
            "Delete \xe2\x80\x9c%s\xe2\x80\x9d%s?",
            *t->title != '\0' ? t->title : "Untitled Task",
            t->parent_id == 0 ? " and its subtasks" : "");
        task_free(t);
    } else {
        title   = g_strdup("Delete Tasks");
        message = g_strdup_printf(
            "Delete the %u selected tasks (and their subtasks)?",
            ids->len);
    }

    DeleteConfirmCtx *ctx = g_new0(DeleteConfirmCtx, 1);
    ctx->app = lw->app;
    ctx->ids = ids;              /* ownership transferred to ctx            */
    task_app_confirm(GTK_WINDOW(lw->window), title, message,
                     on_delete_confirm, ctx);
    g_free(title);
    g_free(message);
}

/* ===========================================================================
 * Task context menu — Open in Google Tasks, Move to List, Delete.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * The task context menu's actions, all "win." and all acting on the
 * CURRENT selection (selected_task_ids): the menu is modal, so the
 * selection it was opened on is the selection its item acts on, whichever
 * pane the click came from.
 * ------------------------------------------------------------------------- */

/* on_task_info() — win.task-info: open the editor for the one selected
 * task (offered for a single selection only, like a double-click).        */
static void
on_task_info(TaskLibrary *lw)
{
    GArray *ids = selected_task_ids(lw);
    if (ids->len > 0)
        task_editor_open(lw->app, g_array_index(ids, gint64, 0));
    g_array_unref(ids);
}

/* on_mark_done() — win.mark-done(b): Mark (All) Complete / Incomplete.
 * These are the checkbox's two verbs in menu form and take the same
 * route: Complete → Done, Incomplete → In Progress.  Per row, so a
 * multi-row "Mark All Incomplete" over a mixed selection settles every
 * one of them on In Progress rather than half-reverting.                   */
static void
on_mark_done(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw = data;
    gboolean done = g_variant_get_boolean(param);
    TaskStatus status = done ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS;
    GArray *ids = selected_task_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_status(lw->app->db,
                                g_array_index(ids, gint64, i), status);
    lib_full_refresh(lw);
    task_app_status(lw->app, "Marked %u task%s %s", ids->len,
                    ids->len == 1 ? "" : "s",
                    done ? "complete" : "incomplete");
    g_array_unref(ids);
}

/* on_set_pinned() — win.set-pinned(b): Add to / Remove from Favorites on
 * the selection (local-only; the sidebar's Favorites row follows via
 * lib_full_refresh).                                                       */
static void
on_set_pinned(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw = data;
    gboolean pinned = g_variant_get_boolean(param);
    GArray *ids = selected_task_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_pinned(lw->app->db,
                                g_array_index(ids, gint64, i), pinned);
    lib_full_refresh(lw);
    task_app_status(lw->app, "%s %u task%s",
                    pinned ? "Added to Favorites" : "Removed from Favorites",
                    ids->len, ids->len == 1 ? "" : "s");
    g_array_unref(ids);
}

/* on_set_priority() — win.set-priority(b): Set / Clear High Priority on
 * the selection (local-only; the views re-sort via lib_full_refresh).      */
static void
on_set_priority(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw = data;
    gboolean priority = g_variant_get_boolean(param);
    GArray *ids = selected_task_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_priority(lw->app->db,
                                  g_array_index(ids, gint64, i), priority);
    lib_full_refresh(lw);
    task_app_status(lw->app, "%s high priority on %u task%s",
                    priority ? "Set" : "Cleared",
                    ids->len, ids->len == 1 ? "" : "s");
    g_array_unref(ids);
}

/* on_move_to_list() — win.move-to-list(x): a destination picked in the
 * Move to List menu: move every selected TOP-LEVEL task not already there
 * (subtasks travel with their parents; a selected subtask on its own
 * cannot move).                                                            */
static void
on_move_to_list(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw = data;
    gint64 dest_id = g_variant_get_int64(param);
    GArray *ids = selected_task_ids(lw);
    guint moved = 0;                 /* how many actually went              */
    for (guint i = 0; i < ids->len; i++) {
        gint64 id = g_array_index(ids, gint64, i);
        if (task_ops_move_to_list(lw->app, id, dest_id))
            moved++;                 /* it declines subtasks and no-op moves */
    }
    g_array_unref(ids);
    if (moved > 0) {
        lib_full_refresh(lw);
        task_app_status(lw->app, "Moved %u task%s", moved,
                        moved == 1 ? "" : "s");
    } else {
        task_app_status(lw->app, "Nothing to move (subtasks move with "
                        "their parent task)");
    }
}

/* menu_item_bool() — append to `section` one item naming `action` with a
 * boolean target: the two directions of a flag as two items.               */
static void
menu_item_bool(GMenu *section, const gchar *label, const gchar *action,
               gboolean value)
{
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target(item, action, "b", value);
    g_menu_append_item(section, item);
    g_object_unref(item);
}

/* ---------------------------------------------------------------------------
 * task_context_menu_popup() — build and pop the task context menu for the
 * CURRENT selection, whatever produced it.
 *
 * Shared by the list view's rows and the Kanban board's cards, so the two
 * can never drift apart.  It reads the selection through
 * selected_task_ids, which already answers with the board's card
 * selection while the board is up — so nothing here needs to know which
 * pane the click came from, and neither do the actions the items name.
 *
 * Returns TRUE when a menu was shown (the click is consumed).
 * ------------------------------------------------------------------------- */
gboolean
task_context_menu_popup(TaskLibrary *lw, GtkWidget *at, gdouble x, gdouble y)
{
    GArray *ids = selected_task_ids(lw);
    if (ids->len == 0) {
        g_array_unref(ids);
        return FALSE;
    }
    gboolean single = ids->len == 1;
    Task *t = single
        ? task_db_task_get(lw->app->db, g_array_index(ids, gint64, 0))
        : NULL;

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();

    /* Info… — single row only; opens the editor (same as double-click).    */
    if (single) {
        g_menu_append(section, "Info\xe2\x80\xa6", "win.task-info");
        task_app_menu_section_end(menu, &section);
    }

    /* Mark Complete / Mark Incomplete — single row: only the applicable
     * direction; multi: both (selection may be mixed).                     */
    if (single && t != NULL) {
        gboolean to_done = t->status != TASK_STATUS_DONE;
        menu_item_bool(section, to_done ? "Mark Complete" : "Mark Incomplete",
                       "win.mark-done", to_done);
    } else {
        menu_item_bool(section, "Mark All Complete",   "win.mark-done", TRUE);
        menu_item_bool(section, "Mark All Incomplete", "win.mark-done", FALSE);
    }
    task_app_menu_section_end(menu, &section);

    /* Pin / Unpin and High Priority: a single row gets just the action
     * that applies to it; a multi-selection (possibly mixed states)
     * gets both directions.                                                */
    if (single && t != NULL) {
        menu_item_bool(section,
                       t->pinned ? "Remove from Favorites" : "Add to Favorites",
                       "win.set-pinned", !t->pinned);
        menu_item_bool(section,
                       t->priority ? "Clear High Priority"
                                   : "Set High Priority",
                       "win.set-priority", !t->priority);
    } else {
        menu_item_bool(section, "Add All to Favorites",      "win.set-pinned",   TRUE);
        menu_item_bool(section, "Remove All from Favorites", "win.set-pinned",   FALSE);
        menu_item_bool(section, "Set All High Priority",     "win.set-priority", TRUE);
        menu_item_bool(section, "Clear All High Priority",   "win.set-priority", FALSE);
    }
    task_app_menu_section_end(menu, &section);

    /* Move to List — applies to the selection's top-level tasks, so it is
     * left out for a single selected subtask (they travel with their
     * parent) and when there is nowhere to move to.  For a single
     * selection its own list is pointless; keep every destination for
     * multi (rows may span lists in virtual views).                        */
    if (!(single && t != NULL && t->parent_id != 0)) {
        GMenu *sub = g_menu_new();
        GPtrArray *lists = task_db_lists(lw->app->db, FALSE);
        for (guint i = 0; i < lists->len; i++) {
            TaskList *l = g_ptr_array_index(lists, i);
            if (single && t != NULL && l->id == t->list_id)
                continue;
            gchar *label = lib_list_label(l);
            GMenuItem *dest = g_menu_item_new(label, NULL);
            g_menu_item_set_action_and_target(dest, "win.move-to-list",
                                              "x", l->id);
            g_menu_append_item(sub, dest);
            g_object_unref(dest);
            g_free(label);
        }
        task_ptr_array_free_lists(lists);
        if (g_menu_model_get_n_items(G_MENU_MODEL(sub)) > 0) {
            g_menu_append_submenu(section, "Move to List", G_MENU_MODEL(sub));
            task_app_menu_section_end(menu, &section);
        }
        g_object_unref(sub);
    }

    gchar *del_label = single
        ? g_strdup("Delete Task")
        : g_strdup_printf("Delete %u Tasks", ids->len);
    g_menu_append(section, del_label, "win.delete-task");
    g_free(del_label);
    task_app_menu_section_end(menu, &section);
    g_object_unref(section);

    task_free(t);
    g_array_unref(ids);
    task_app_menu_popup(at != NULL ? at : lw->window,
                        G_MENU_MODEL(menu), x, y);
    return TRUE;
}

/* ===========================================================================
 * Menu actions.
 * =========================================================================== */

/* on_menu_settings() — File → Settings…                                    */
static void
on_menu_settings(TaskLibrary *lw)
{
    task_settings_window_open(lw->app, GTK_WINDOW(lw->window), lw->app->db->path);
}

/* on_clear_confirm() — async callback for on_menu_clear_completed.         */
typedef struct {
    TaskApp *app;
    gint64   list_id;
} ClearConfirmCtx;

static void
on_clear_confirm(gboolean yes, gpointer data)
{
    ClearConfirmCtx *ctx = data;
    if (yes) {
        guint n = task_ops_clear_completed(ctx->app, ctx->list_id);
        task_app_status(ctx->app, "Cleared %u completed task%s", n,
                        n == 1 ? "" : "s");
        TaskLibrary *lw = lib_of(ctx->app);
        if (lw != NULL)
            lib_full_refresh(lw);
    }
    g_free(ctx);
}

/* on_menu_clear_completed() — File → Clear Completed Tasks: archive the
 * selected list's done tasks (Google's tasks.clear when synced).           */
static void
on_menu_clear_completed(TaskLibrary *lw)
{
    gint64 id = lib_selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app,
                        "Select a list to clear its completed tasks");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    gchar *message = g_strdup_printf(
        "Remove all completed tasks from \xe2\x80\x9c%s\xe2\x80\x9d?",
        l->name);
    task_list_free(l);
    ClearConfirmCtx *ctx = g_new0(ClearConfirmCtx, 1);
    ctx->app     = lw->app;
    ctx->list_id = id;
    task_app_confirm(GTK_WINDOW(lw->window), "Clear Completed", message,
                     on_clear_confirm, ctx);
    g_free(message);
}

/* ---------------------------------------------------------------------------
 * on_open_db() — File → Open Database File…: pick a .db file and open it as
 * the new default (Yes) or for this session only (No).  ASYNC: GTK4 has no
 * blocking dialogs.  Two async hops: file pick → confirm (Yes/No default).
 * ------------------------------------------------------------------------- */

/* open_db_do() — commit the switch after both dialogs have resolved.       */
static void
open_db_do(TaskApp *app, const gchar *file_path, gboolean set_default)
{
    TaskLibrary *lw = lib_of(app);
    task_editor_close_all(app);
    gchar *old_path = g_strdup(app->db->path);
    task_db_close(app->db);
    GError *gerr = NULL;
    app->db = task_db_open(file_path, &gerr);

    if (app->db == NULL) {
        task_app_notice(lw != NULL ? GTK_WINDOW(lw->window) : NULL,
                        "Tasks - Database Error",
                        "Could not open:\n%s\n\n%s",
                        file_path,
                        gerr != NULL ? gerr->message : "Unknown error");
        g_clear_error(&gerr);
        app->db = task_db_open(old_path, &gerr); /* revert                  */
        if (app->db == NULL)
            g_critical("open_db_do: cannot revert to %s: %s", old_path,
                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(old_path);
        return;
    }

    /* The ini key is the whole of "where the database is kept": it is read
     * once at startup to resolve the path, and this is its only writer.  */
    if (set_default) {
        gchar *dir = g_path_get_dirname(file_path);
        task_app_config_set("db_dir", dir);
        g_free(dir);
    }
    g_free(old_path);

    /* Every timer carries the db path it was armed with, so opening
     * another database must re-arm all of them or a worker keeps writing
     * to the file we just left.  This is now the ONLY site that opens a
     * different database (gotcha 14).                                    */
    task_worker_arm_all(app, app->db->path);
    task_app_notify_changed(app);
    task_app_status(app, "Opened %s", app->db->path);
}

typedef struct {
    TaskApp *app;
    gchar   *file_path;
} OpenDbCtx;

static void
on_open_db_confirm(gboolean set_default, gpointer data)
{
    OpenDbCtx *ctx = data;
    open_db_do(ctx->app, ctx->file_path, set_default);
    g_free(ctx->file_path);
    g_free(ctx);
}

static void
on_open_db_picked(gchar *file_path, gpointer data)
{
    TaskApp *app = data;
    if (file_path == NULL)
        return;
    if (g_strcmp0(file_path, app->db->path) == 0) { /* already open        */
        g_free(file_path);
        return;
    }

    gchar *display = g_path_get_basename(file_path);
    gchar *message = g_strdup_printf(
        "Set \xe2\x80\x9c%s\xe2\x80\x9d as your new default database?\n\n"
        "Yes = use as default from now on.\n"
        "No = open for this session only.", display);
    g_free(display);

    OpenDbCtx *ctx = g_new0(OpenDbCtx, 1);
    ctx->app       = app;
    ctx->file_path = file_path;
    TaskLibrary *lw = lib_of(app);
    task_app_confirm(lw != NULL ? GTK_WINDOW(lw->window) : NULL,
                     "Tasks - Open Database", message,
                     on_open_db_confirm, ctx);
    g_free(message);
}

static void
on_open_db(TaskLibrary *lw)
{
    task_app_pick_path(GTK_WINDOW(lw->window), "Open Database",
                       TASK_PICK_OPEN, "_Open",
                       "SQLite Database (*.db)", "*.db",
                       NULL,
                       on_open_db_picked, lw->app);
}

/* The View menu's Completed, Sorting and Sidebar items are wired straight
 * to their TOOLBAR twins (on_toggle_done_visible, on_toggle_manual_sort,
 * lib_on_toggle_sidebar).  Each of those already flips the persisted state and
 * calls the one refresh that re-labels both controls, so a separate menu
 * handler would only be the same three lines under another name — and two
 * copies of "what does this toggle do" is how the two controls drift.
 * There is no state to read off the widget either way: the label says
 * where a click GOES, so every handler flips the config or the cache.     */

/* ---------------------------------------------------------------------------
 * on_toggle_kanban() — the pane toggle, shared by View → Kanban View /
 * List View and its TOOLBAR twin: persist the flag, refresh the cached
 * copy, and rebuild the pane in the other presentation.  lib_refresh_tasks
 * runs task_pane_mode_apply, which is what re-labels and re-icons both
 * controls.
 *
 * FLIPS the cached flag rather than reading the widget: the label names
 * the pane a click switches TO, so the item no longer carries the current
 * state (same shape as the sort item).
 *
 * The board's selection is dropped on the way out AND on the way in: the
 * two panes track selection separately (a tree selection vs. a card id),
 * and carrying one across would leave Delete Task pointed at a task the
 * user can no longer see highlighted.
 * ------------------------------------------------------------------------- */
static void
on_toggle_kanban(TaskLibrary *lw)
{
    lw->board.kanban = !lw->board.kanban;
    task_app_config_set("kanban_view", lw->board.kanban ? "1" : "0");
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(lw->task_sel));
    g_hash_table_remove_all(lw->board.kanban_sel);
    lw->board.kanban_anchor = 0;
    lib_refresh_tasks(lw);
}

/* on_menu_toggle_compact() — View → Compact Controls / Full Controls:
 * FLIP the persisted flag and re-apply the layout (toolbar out, floating
 * New/Delete pair in).  Flips rather than reading the widget: the label
 * names the controls a click switches TO, so the item carries no state.
 * compact_layout_apply re-labels it.                                       */
static void
on_menu_toggle_compact(TaskLibrary *lw)
{
    task_app_config_set("compact_layout",
        task_app_config_get_bool("compact_layout", FALSE) ? "0" : "1");
    compact_layout_apply(lw);
}

/* on_menu_about() — File → About: the standard about dialog.  GTK4 has no
 * blocking dialogs; task_app_icon_paintable gives a sharp HiDPI logo.     */
static void
on_menu_about(TaskLibrary *lw)
{
    GdkPaintable *logo = task_app_icon_paintable(lw->app, "document", 128);
    const gchar *authors[] = { "Ian Campbell", "Claude", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "Tasks");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), TASK_VERSION);
    if (logo != NULL) {
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* No database vitals here — they live in File → Settings… → Database,
     * with the health check and the controls that act on the file.
     * __DATE__/__TIME__ expand when this file is compiled.                 */
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
        "Gettin' shit done since 2026!\n\n"
        "Compiled " __DATE__ " " __TIME__);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD_3);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");
    gtk_window_present(GTK_WINDOW(dialog));
}

/* on_menu_quit() — File → Quit.                                            */
static void
on_menu_quit(TaskLibrary *lw)
{
    gtk_window_destroy(GTK_WINDOW(lw->window));
}

/* ===========================================================================
 * Construction.
 * =========================================================================== */

/* tool_button() — a toolbar button (local icon + label) appended to `bar`.
 * Returns the button so the caller can keep a reference for later updates.  */
static GtkWidget *
tool_button(TaskLibrary *lw, GtkWidget *bar, const gchar *icon,
            const gchar *fallback_markup, const gchar *label,
            const gchar *tooltip, const gchar *action)
{
    GtkWidget *item = task_app_tool_item_new(lw->app, FALSE, icon,
                                             fallback_markup, label, tooltip);
    gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(item), action);
    gtk_box_append(GTK_BOX(bar), item);
    return item;
}

/* compact_bar_button() — one floating-bar button: the 24 px local icon
 * (Pango-markup glyph when the PNG is missing, matching the toolbar's
 * fallback rule) wired to `cb`, appended to `box`.                         */
static void
compact_bar_button(TaskLibrary *lw, GtkWidget *box, const gchar *icon,
                   const gchar *fallback_markup, const gchar *tooltip,
                   const gchar *action)
{
    GtkWidget *btn   = gtk_button_new();
    GtkWidget *image = task_app_icon_image_sized(lw->app, icon, 24);
    if (image == NULL) {
        image = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(image), fallback_markup);
    }
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_button_set_child(GTK_BUTTON(btn), image);
    task_app_set_tooltip(btn, tooltip);
    gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(btn), action);
    gtk_box_append(GTK_BOX(box), btn);
}

/* ---------------------------------------------------------------------------
 * compact_bar_new() — Compact Layout's floating toolbar: New Task and
 * Delete Task as a two-button pill pinned 20 px in from the bottom and
 * right edges of the task area.
 *
 * The plate's background and border come from the theme via the global CSS
 * class .task-float-bar installed once here: @theme_bg_color and a shade()
 * of it both resolve at render time, so a light/dark switch is handled by
 * GTK's CSS engine without any C callback.
 * ------------------------------------------------------------------------- */
static GtkWidget *
compact_bar_new(TaskLibrary *lw)
{
    static gboolean css_done = FALSE;
    if (!css_done) {
        css_done = TRUE;
        task_app_css_install(
            ".task-float-bar {"
            "  background-color: @theme_bg_color;"
            "  border: 1px solid shade(@theme_bg_color, 0.85);"
            "  border-radius: 8px;"
            "  padding: 4px 2px;"
            "}");
    }

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(bar, "task-float-bar");
    compact_bar_button(lw, bar, "add", "+", "Create a task in the "
                       "selected list", "win.new-task");
    compact_bar_button(lw, bar, "remove", "\xe2\x88\x92",
                       "Delete the selected task", "win.delete-task");

    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    gtk_widget_set_margin_end(bar, 20);
    gtk_widget_set_margin_bottom(bar, 20);
    lw->float_bar = bar;
    return bar;
}

/* on_paned_position() — track the divider for persistence.  Fires on
 * every step of a drag, so it only caches; on_library_destroy does the
 * single config write.                                                     */
static void
on_paned_position(GObject *paned, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    TaskLibrary *lw = data;
    gint pos = gtk_paned_get_position(GTK_PANED(paned));
    /* Ignore the collapse the Sidebar toggle causes: hiding the pane
     * drives the position to 0, and storing that would reopen the next
     * session with an invisible sidebar and no obvious way back.           */
    if (pos > 0)
        lw->sb_width = pos;
}

/* on_library_close() — "close-request": capture the live client size just
 * before the window closes, for persistence.  Returns FALSE so GTK proceeds
 * with the close.                                                           */
static gboolean
on_library_close(GtkWindow *w, gpointer data)
{
    TaskLibrary *lw = data;
    lw->win_w = gtk_widget_get_width(GTK_WIDGET(w));
    lw->win_h = gtk_widget_get_height(GTK_WIDGET(w));
    return FALSE;
}

/* on_library_destroy() — tear down: editors first (flushing saves).        */
static void
on_library_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    /* The closing size becomes the next launch's window size.              */
    if (lw->win_w > 0 && lw->win_h > 0) {
        gchar *v = g_strdup_printf("%d", lw->win_w);
        task_app_config_set("win_w", v);
        g_free(v);
        v = g_strdup_printf("%d", lw->win_h);
        task_app_config_set("win_h", v);
        g_free(v);
    }
    /* Likewise the divider: a sidebar narrowed by hand has to come back
     * that width, or every launch undoes the adjustment.  Written here,
     * not per drag step — "notify::position" fires continuously while
     * the handle moves and each write rewrites the ini.                    */
    if (lw->sb_width > 0) {
        gchar *v = g_strdup_printf("%d", lw->sb_width);
        task_app_config_set("sidebar_width", v);
        g_free(v);
    }
    /* Hooks come down BEFORE the editors: a closing editor's final save
     * would otherwise fire notify_changed → task_editor_refresh_all, which
     * can destroy sibling editors mid-teardown (a failing Notes CLI
     * closes its editors on reload) and leave close_all's snapshot list
     * holding freed windows.                                               */
    /* The entry is a child of the toolbar and goes with the window; the
     * PARSED query is ours and does not.                                   */
    g_clear_pointer(&lw->search, (GDestroyNotify)task_search_free);
    task_app_unlisten(lw->app, lw->listen_changed);
    task_app_unlisten(lw->app, lw->listen_tasks);
    task_app_unlisten(lw->app, lw->listen_status);
    lw->listen_changed = lw->listen_tasks = lw->listen_status = 0;
    lw->app->library_window = NULL;
    /* Cancel the status hide timer before the revealer is destroyed.        */
    if (lw->status_hide_source != 0) {
        g_source_remove(lw->status_hide_source);
        lw->status_hide_source = 0;
    }
    /* A card drag in flight holds a POINTER GRAB and owns a ghost window.
     * Both must come down before the library does, or the grab outlives
     * the widget it was taken on and the pointer is dead app-wide.        */
    lib_card_drag_stop(lw);
    task_editor_close_all(lw->app);
    g_clear_object(&lw->drag_cursor);
    g_clear_object(&lw->board.card_grab);
    g_clear_object(&lw->board.card_grabbing);
    if (lw->sb_fit_idle != 0) {
        g_signal_handler_disconnect(lw->sb_fit_clock, lw->sb_fit_idle);
        g_object_unref(lw->sb_fit_clock);
    }
    if (lw->group_expanded != NULL)
        g_hash_table_destroy(lw->group_expanded);
    if (lw->board.kanban_sel != NULL)
        g_hash_table_destroy(lw->board.kanban_sel);
    g_free(lw);
}

/* ===========================================================================
 * Actions (see library_priv.h).
 *
 * "app." — what the menubar names.  It must work whichever window is
 * focused (on macOS the native menubar is the only menubar), so it lives
 * on the GtkApplication and acts on THE library window through lib_of.
 * app.about, app.preferences and app.quit are the names GTK's quartz
 * backend binds its own application menu to.
 *
 * "win." — what is invoked only from inside the window: the toolbar
 * buttons and the context menus.  Where a toolbar button and a menubar
 * item mean the same thing, the "win." name binds the SAME function.
 * There are no accelerators.
 *
 * The five toggling View items are hidden-when PAIRS: two items, each
 * naming the thing a click DOES, of which the applier for that state
 * enables exactly one (lib_menu_pair_sync).  Both halves of a pair bind
 * the same flipping function, so the pair cannot disagree with the flag.
 * =========================================================================== */

static void on_toggle_done_visible(TaskLibrary *lw);
static void on_toggle_manual_sort(TaskLibrary *lw);
static void on_menu_toggle_compact(TaskLibrary *lw);
static void on_menu_clear_completed(TaskLibrary *lw);
static void on_open_db(TaskLibrary *lw);
static void on_menu_settings(TaskLibrary *lw);
static void on_menu_about(TaskLibrary *lw);
static void on_menu_quit(TaskLibrary *lw);

static const LibCommand APP_COMMANDS[] = {
    { "new-task",         on_new_task             },
    { "new-list",         lib_on_new_list         },
    { "clear-completed",  on_menu_clear_completed },
    { "open-db",          on_open_db              },
    { "preferences",      on_menu_settings        },
    { "about",            on_menu_about           },
    { "quit",             on_menu_quit            },
    { "done-hide",        on_toggle_done_visible  },
    { "done-show",        on_toggle_done_visible  },
    { "sort-manual",      on_toggle_manual_sort   },
    { "sort-auto",        on_toggle_manual_sort   },
    { "sidebar-hide",     lib_on_toggle_sidebar   },
    { "sidebar-show",     lib_on_toggle_sidebar   },
    { "controls-compact", on_menu_toggle_compact  },
    { "controls-full",    on_menu_toggle_compact  },
    { "pane-kanban",      on_toggle_kanban        },
    { "pane-list",        on_toggle_kanban        },
};

static const LibCommand WIN_COMMANDS[] = {
    { "new-task",    on_new_task            },
    { "delete-task", on_delete_task         },
    { "toggle-done", on_toggle_done_visible },
    { "toggle-sort", on_toggle_manual_sort  },
    { "toggle-pane", on_toggle_kanban       },
    { "task-info",   on_task_info           },
};

/* on_win_command() — "activate" of a "win." command: the command rides on
 * the action, `data` is the library.                                       */
static void
on_win_command(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    const LibCommand *cmd = g_object_get_data(G_OBJECT(action), "lib-command");
    cmd->run(data);
}

/* on_app_command() — "activate" of an "app." command: `data` is the
 * TaskApp, since these outlive any one library window; with no library
 * window there is nothing to act on.                                       */
static void
on_app_command(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    TaskLibrary *lw = lib_of(data);
    if (lw == NULL)
        return;
    const LibCommand *cmd = g_object_get_data(G_OBJECT(action), "lib-command");
    cmd->run(lw);
}

/* commands_install() — add `n` parameterless actions from `table` to
 * `map`, each carrying its table entry so one handler serves them all.
 * `table` must outlive the map (every caller's is a static).              */
static void
commands_install(GActionMap *map, const LibCommand *table, gsize n,
                 GCallback handler, gpointer user_data)
{
    for (gsize i = 0; i < n; i++) {
        GSimpleAction *action = g_simple_action_new(table[i].name, NULL);
        g_object_set_data(G_OBJECT(action), "lib-command",
                          (gpointer)&table[i]);
        g_signal_connect(action, "activate", handler, user_data);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    }
}

void
lib_win_commands_install(TaskLibrary *lw, const LibCommand *table, gsize n)
{
    commands_install(G_ACTION_MAP(lw->window), table, n,
                     G_CALLBACK(on_win_command), lw);
}

void
lib_win_action_add(TaskLibrary *lw, const gchar *name,
                   const GVariantType *type, GCallback activate)
{
    GSimpleAction *action = g_simple_action_new(name, type);
    g_signal_connect(action, "activate", activate, lw);
    g_action_map_add_action(G_ACTION_MAP(lw->window), G_ACTION(action));
    g_object_unref(action);
}

void
lib_app_action_set_enabled(TaskLibrary *lw, const gchar *name,
                           gboolean enabled)
{
    GAction *action = g_action_map_lookup_action(
        G_ACTION_MAP(lw->app->gtk_app), name);
    if (action != NULL)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

void
lib_menu_pair_sync(TaskLibrary *lw, const gchar *when_on,
                   const gchar *when_off, gboolean on)
{
    lib_app_action_set_enabled(lw, when_on,  on);
    lib_app_action_set_enabled(lw, when_off, !on);
}

/* ---------------------------------------------------------------------------
 * library_install_actions() — every action of the library: the "app."
 * commands on the application (once — a second library window in one
 * process reuses them), the "win." commands and the parameterised context
 * menu actions on the window, and the sidebar's own.  The list's column
 * actions are added by task_list_build, once the columns exist.
 * ------------------------------------------------------------------------- */
static void
library_install_actions(TaskLibrary *lw)
{
    GActionMap *app_map = G_ACTION_MAP(lw->app->gtk_app);
    if (g_action_map_lookup_action(app_map, APP_COMMANDS[0].name) == NULL)
        commands_install(app_map, APP_COMMANDS, G_N_ELEMENTS(APP_COMMANDS),
                         G_CALLBACK(on_app_command), lw->app);
    lib_win_commands_install(lw, WIN_COMMANDS, G_N_ELEMENTS(WIN_COMMANDS));
    lib_win_action_add(lw, "mark-done",    G_VARIANT_TYPE_BOOLEAN,
                       G_CALLBACK(on_mark_done));
    lib_win_action_add(lw, "set-pinned",   G_VARIANT_TYPE_BOOLEAN,
                       G_CALLBACK(on_set_pinned));
    lib_win_action_add(lw, "set-priority", G_VARIANT_TYPE_BOOLEAN,
                       G_CALLBACK(on_set_priority));
    lib_win_action_add(lw, "move-to-list", G_VARIANT_TYPE_INT64,
                       G_CALLBACK(on_move_to_list));
    task_sidebar_install_actions(lw);
}

/* menu_pair() — append a hidden-when PAIR to `section`: the two faces of
 * one toggling item, each naming the action a click performs; the applier
 * for that state enables exactly one of them (lib_menu_pair_sync).  A
 * dynamic label is never a runtime model edit — see the actions note.     */
static void
menu_pair(GMenu *section, const gchar *label_on, const gchar *action_on,
          const gchar *label_off, const gchar *action_off)
{
    const gchar *labels[]  = { label_on,  label_off  };
    const gchar *actions[] = { action_on, action_off };
    for (gsize i = 0; i < 2; i++) {
        GMenuItem *item = g_menu_item_new(labels[i], actions[i]);
        g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
        g_menu_append_item(section, item);
        g_object_unref(item);
    }
}

/* ---------------------------------------------------------------------------
 * build_menubar() — the File and View menus as a menu model, every item
 * naming an "app." action.  Rendered by GTK: in the native macOS menu bar,
 * or across the top of the GtkApplicationWindow where the shell shows no
 * menubar of its own.  Returns the model (owned by the caller).
 * ------------------------------------------------------------------------- */
static GMenuModel *
build_menubar(void)
{
    GMenu *bar     = g_menu_new();
    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();

    /* File.  ONE separator in this menu, and it goes after the group
     * below.  What acts on the TASKS is New Task, New List and Clear
     * Completed; everything after the rule is about the app or the file it
     * keeps — the database, Settings, About, Quit.  A rule between every
     * pair of items (which is what this was) divides nothing, so it
     * stopped reading as grouping at all.  (On macOS GTK also puts
     * Settings, About and Quit in the application menu it builds from the
     * same three actions.)                                               */
    g_menu_append(section, "New Task",              "app.new-task");
    g_menu_append(section, "New List\xe2\x80\xa6",  "app.new-list");
    g_menu_append(section, "Clear Completed Tasks", "app.clear-completed");
    task_app_menu_section_end(menu, &section);
    g_menu_append(section, "Open Database File\xe2\x80\xa6", "app.open-db");
    g_menu_append(section, "Settings\xe2\x80\xa6",           "app.preferences");
    g_menu_append(section, "About",                          "app.about");
    g_menu_append(section, "Quit",                           "app.quit");
    task_app_menu_section_end(menu, &section);
    g_menu_append_submenu(bar, "File", G_MENU_MODEL(menu));
    g_object_unref(menu);

    /* View.  Above the divider is what the task PANE shows — the completed
     * rows and the sort mode; below it is what the WINDOW looks like.
     * Every item is a PAIR whose applier picks the face on offer:
     * hide_done_icon_refresh, task_pane_mode_apply (sorting AND the pane),
     * lib_sidebar_ui_sync, compact_layout_apply.                          */
    menu = g_menu_new();
    menu_pair(section, DONE_LABEL_TO_HIDE,    "app.done-hide",
                       DONE_LABEL_TO_SHOW,    "app.done-show");
    menu_pair(section, SORT_LABEL_TO_MANUAL,  "app.sort-manual",
                       SORT_LABEL_TO_AUTO,    "app.sort-auto");
    task_app_menu_section_end(menu, &section);
    menu_pair(section, SIDEBAR_LABEL_TO_HIDE, "app.sidebar-hide",
                       SIDEBAR_LABEL_TO_SHOW, "app.sidebar-show");
    menu_pair(section, CTRL_LABEL_TO_COMPACT, "app.controls-compact",
                       CTRL_LABEL_TO_FULL,    "app.controls-full");
    menu_pair(section, PANE_LABEL_TO_KANBAN,  "app.pane-kanban",
                       PANE_LABEL_TO_LIST,    "app.pane-list");
    task_app_menu_section_end(menu, &section);
    g_object_unref(section);
    g_menu_append_submenu(bar, "View", G_MENU_MODEL(menu));
    g_object_unref(menu);

    return G_MENU_MODEL(bar);
}

/* ===========================================================================
 * Row order: the saved-order helpers BOTH panes share, then manual sort's
 * own persistence, drag handlers and mode toggle.
 *
 * lib_row_order_permutation and lib_row_order_key are the shared pair — the list
 * view and the Kanban board keep separate order KEYS, but the spelling of
 * a key and the rule for applying one are the same for both, so they live
 * here rather than in either pane's section.  The permutation used to sit
 * inside the board's, which is only where it happened to be written.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_library_window_new() — build the library window (see header).
 * ------------------------------------------------------------------------- */
GtkWidget *
task_library_window_new(TaskApp *app)
{
    TaskLibrary *lw = g_new0(TaskLibrary, 1);
    lw->app = app;
    /* Seeded here, not left to task_manual_sort_apply at the end of this
     * function: the toolbar icon, tooltip and View-menu check are all built
     * before that call and read the cache.                                 */
    lw->manual_sort =
        task_app_config_get_bool("task_list_manual_sort", FALSE);
    /* Same reason: the View-menu check is built from this cache, and
     * lib_refresh_tasks reads it before the menu handler ever runs.            */
    lw->board.kanban = task_app_config_get_bool("kanban_view", FALSE);
    /* Seeded before the first refresh builds any cards, since
     * kanban_card_new reads it per card.  DEFAULT ON: the shadow is the
     * board's look, and the setting exists to give the paint cost back on
     * a machine that wants it, not as an opt-in.                          */
    lw->board.card_shadow = task_app_config_get_bool("kanban_shadow", TRUE);
    lw->sel_kind = SB_KIND_LIST;     /* refresh falls back to first list    */
    lw->group_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);
    lw->board.kanban_sel     = g_hash_table_new(NULL, NULL);   /* id set          */

    /* A GtkApplicationWindow: that is what gives it the "win." action
     * group the toolbar and the context menus name, and — where the shell
     * shows no menubar of its own (XFCE) — what draws the application's
     * menubar across its top.  On macOS GTK's quartz backend puts the same
     * model in the native menu bar.                                        */
    lw->window = gtk_application_window_new(app->gtk_app);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Tasks");
    library_install_actions(lw);     /* before anything can name one       */
    /* The last session's closing size (win_w/win_h), else the default.     */
    gchar *ww = task_app_config_get("win_w");
    gchar *wh = task_app_config_get("win_h");
    gint w = ww != NULL ? atoi(ww) : 0;
    gint hgt = wh != NULL ? atoi(wh) : 0;
    gtk_window_set_default_size(GTK_WINDOW(lw->window),
                                w > 0 ? w : 980, hgt > 0 ? hgt : 640);
    g_free(ww);
    g_free(wh);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* --- Menubar ---------------------------------------------------------- */
    /* A model over the "app." actions, rendered by GTK: in the native
     * macOS menu bar, or across the top of this window on a desktop whose
     * shell shows none.                                                    */
    GMenuModel *menubar = build_menubar();
    gtk_application_set_menubar(app->gtk_app, menubar);
    g_object_unref(menubar);

    /* --- Toolbar ---------------------------------------------------------- */
    /* A GtkBox with the "toolbar" style class replaces GtkToolbar (gone in
     * GTK4).  Layout: the task pair, a vertical separator, then the sidebar,
     * completed, sort and pane toggles, an expanding spacer, then the search
     * box at the right edge.  ONE separator in the window's own block after
     * the task pair: it separates the buttons that ACT on a task from the
     * controls that change what the pane SHOWS.  The task verbs lead because
     * they are what the window is for; the four toggles are one group behind
     * the rule, the sidebar toggle among them.                              */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(toolbar, "toolbar");
    lw->toolbar = toolbar;           /* Compact Layout hides it whole       */

    tool_button(lw, toolbar, "add", NULL,
                "New Task", "Create a task in the selected list",
                "win.new-task");
    tool_button(lw, toolbar, "remove", NULL,
                "Delete Task", "Delete the selected task",
                "win.delete-task");
    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* ONE face, set here and never swapped — a double-headed arrow names
     * the MOVEMENT rather than a direction, and lib_sidebar_ui_sync says
     * which way the next click goes in the tooltip (see there).          */
    lw->sidebar_item = tool_button(lw, toolbar,
        "left-and-right", "\xe2\x97\xa7", "Sidebar",
        "Show the lists pane", "win.toggle-sidebar");

    lw->hide_done_item = tool_button(lw, toolbar,
        "hidden", "\xf0\x9f\x91\x81", "Completed",
        "Hide completed tasks", "win.toggle-done");
    hide_done_icon_refresh(lw);      /* the persisted state's icon          */
    lw->manual_sort_item = tool_button(lw, toolbar,
        "manual", "\xe2\x89\x8b", "Sort Mode",
        "Switch to manual drag sorting", "win.toggle-sort");
    manual_sort_icon_refresh(lw);    /* the persisted state's tooltip       */
    /* The pane toggle sits with the sort toggle: both change how the tasks
     * are PRESENTED rather than acting on a task.  task_pane_mode_apply
     * gives it its icon and tooltip.                                       */
    lw->pane_item = tool_button(lw, toolbar,
        "menu", "\xe2\x96\xa6", "Kanban",
        "Show the tasks as a Kanban board", "win.toggle-pane");

    /* Expanding spacer pushes the search box to the right edge.             */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);

    /* The search box at the far right.  A GtkSearchEntry brings the
     * magnifier, the clear icon, Escape, and the typing-pause delay.      */
    lw->search_entry = gtk_search_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(lw->search_entry), 18);
    g_object_set(lw->search_entry,
                 "placeholder-text", SEARCH_PLACEHOLDER, NULL);
    task_app_set_tooltip(lw->search_entry, SEARCH_TOOLTIP);
    /* 5 px of air between the box and the window edge (Notes' spacing).    */
    gtk_widget_set_margin_end(lw->search_entry, 5);
    g_signal_connect(lw->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), lw);
    g_signal_connect(lw->search_entry, "activate",
                     G_CALLBACK(on_search_changed), lw);
    g_signal_connect(lw->search_entry, "stop-search",
                     G_CALLBACK(on_search_stopped), lw);
    gtk_box_append(GTK_BOX(toolbar), lw->search_entry);

    gtk_box_append(GTK_BOX(vbox), toolbar);
    /* Thin rule between the toolbar and the panes (Notes look).           */
    lw->toolbar_rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), lw->toolbar_rule);

    /* --- Paned: sidebar | tasks ------------------------------------------ */
    /* The panes sit in a GtkOverlay so Compact Layout's floating button
     * pair can hover over the bottom-right corner of the task area, and
     * so the board's card ghost can slide across the whole pane.  The
     * overlay wraps the panes rather than the whole window box so the
     * float never covers the status bar's event messages.                  */
    GtkWidget *overlay = gtk_overlay_new();
    lw->overlay = overlay;
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(vbox), overlay);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    /* A 6 px divider: wide-handle switches GtkPaned off its hairline
     * style, and the exact width comes from CSS on the handle's own
     * `separator` node (the horizontal paned's separator is vertical, so
     * min-WIDTH is the lever).  Global CSS provider — GTK4 removed
     * per-widget CSS providers.                                           */
    gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
    gtk_widget_add_css_class(paned, "task-paned");
    static gboolean paned_css_done = FALSE;
    if (!paned_css_done) {
        paned_css_done = TRUE;
        task_app_css_install(".task-paned > separator { min-width: 6px; }");
    }
    gchar *sbw = task_app_config_get("sidebar_width");
    lw->sb_width = sbw != NULL ? atoi(sbw) : 220;
    g_free(sbw);
    if (lw->sb_width <= 0)
        lw->sb_width = 220;
    lw->sidebar_paned = paned;
    gtk_paned_set_position(GTK_PANED(paned), lw->sb_width);
    g_signal_connect(paned, "notify::position",
                     G_CALLBACK(on_paned_position), lw);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), paned);

    /* The ghost layer: a GtkFixed overlay child the board moves card ghosts
     * around on.  can_target = FALSE so it never intercepts pointer events. */
    lw->ghost_layer = gtk_fixed_new();
    gtk_widget_set_can_target(lw->ghost_layer, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), lw->ghost_layer);

    /* The Compact Layout float bar; compact_bar_new() is below.             */
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), compact_bar_new(lw));

    task_sidebar_build(lw, paned);
    task_list_build(lw);
    task_kanban_build(lw);

    GtkWidget *task_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(lw->task_scroll,     TRUE);
    gtk_widget_set_vexpand(lw->board.kanban_box, TRUE);
    gtk_box_append(GTK_BOX(task_pane), lw->task_scroll);
    gtk_box_append(GTK_BOX(task_pane), lw->board.kanban_box);
    gtk_paned_set_end_child(GTK_PANED(paned), task_pane);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    /* --- Status bar -------------------------------------------------------- */
    /* Same geometry as the Notes status bar: 8 px side margins,
     * 3 px top/bottom (a border_width would add a pixel more on every
     * edge and read visibly taller).                                       */
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status, 8);
    gtk_widget_set_margin_end(status, 8);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);

    lw->status_left = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_left),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_left, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lw->status_left, TRUE);
    gtk_box_append(GTK_BOX(status), lw->status_left);

    /* The event-message label lives in a GtkRevealer so it crossfades in
     * and out.  CROSSFADE is the closest GTK4 equivalent to the GTK3 Pango
     * alpha fade; the 3 s hold + revealer transition drives the cycle.      */
    lw->status_right = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_right),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_right, GTK_ALIGN_END);
    lw->status_reveal = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(lw->status_reveal),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(lw->status_reveal),
                                         1000);
    gtk_revealer_set_child(GTK_REVEALER(lw->status_reveal),
                           lw->status_right);
    gtk_box_append(GTK_BOX(status), lw->status_reveal);

    /* Both labels 85% of the UI font (Notes size).  CSS font-size: 85%
     * can resolve to zero on Linux when the per-widget provider has no
     * explicit base size in scope; Pango scale attributes are always
     * relative to the actual rendered font and work on every platform.     */
    PangoAttrList *small_attrs = pango_attr_list_new();
    pango_attr_list_insert(small_attrs, pango_attr_scale_new(0.85));
    gtk_label_set_attributes(GTK_LABEL(lw->status_left),  small_attrs);
    gtk_label_set_attributes(GTK_LABEL(lw->status_right), small_attrs);
    pango_attr_list_unref(small_attrs);

    gtk_box_append(GTK_BOX(vbox), status);

    /* --- Hooks + first population ------------------------------------------ */
    gtk_window_set_child(GTK_WINDOW(lw->window), vbox);
    app->library_window = lw->window;
    g_object_set_data(G_OBJECT(lw->window), "task-library", lw);
    lw->listen_changed = task_app_listen_changed(app, notify_changed_hook,
                                                 NULL);
    lw->listen_tasks   = task_app_listen_tasks(app, notify_tasks_hook, NULL);
    lw->listen_status  = task_app_listen_status(app, notify_status_hook,
                                                NULL);
    g_signal_connect(lw->window, "close-request",
                     G_CALLBACK(on_library_close), lw);
    g_signal_connect(lw->window, "destroy",
                     G_CALLBACK(on_library_destroy), lw);

    lib_refresh_sidebar(lw);
    lib_refresh_tasks(lw);

    /* Apply persisted layout states before presenting the window, so the
     * user never sees the default then a shift.  compact_layout_apply also
     * settles the lists pane (HIDDEN by default) and hides the float pair
     * outside compact mode.  task_pane_mode_apply puts the list / Kanban
     * choice back.                                                          */
    compact_layout_apply(lw);
    task_pane_mode_apply(lw);

    gtk_window_present(GTK_WINDOW(lw->window));
    return lw->window;
}
