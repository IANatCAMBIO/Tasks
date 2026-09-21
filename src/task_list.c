/* ===========================================================================
 * task_list.c — the library window's task LIST pane: the tree view and its
 * columns, the header menu, the manual-sort row drag and the persisted row
 * orders (see library_priv.h).
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include <stdlib.h>
#include <string.h>

/* header_flatten_css() — the column-header CSS: the flat background plus
 * shades of it for :hover / :active, so a sortable header still gives
 * feedback instead of jumping back to the theme's button color.  Quartz
 * only, like its one caller — otherwise it is an unused static.            */
#ifdef GDK_WINDOWING_QUARTZ
static gchar *
header_flatten_css(const GdkRGBA *bg)
{
    gchar *c   = lib_rgb_of(bg);
    gchar *css = g_strdup_printf(
        "button {"
        "  background-image: none;"
        "  background-color: %s;"
        "}"
        "button:hover {"
        "  background-image: none;"
        "  background-color: shade(%s, 0.94);"
        "}"
        "button:active {"
        "  background-image: none;"
        "  background-color: shade(%s, 0.88);"
        "}",
        c, c, c);
    g_free(c);
    return css;
}

#endif /* GDK_WINDOWING_QUARTZ */

/* ---------------------------------------------------------------------------
 * header_button_flatten() — paint a tree-view column header the same color
 * as the status bar.  macOS (quartz) ONLY: elsewhere the platform theme
 * owns the header's look and we leave it completely alone.
 *
 * The status bar sets no background of its own: it shows the window's,
 * which the theme paints from @theme_bg_color.  Headers, by contrast, are
 * real GtkButtons and come with the quartz theme's button gradient, so
 * they read lighter than the rest of the chrome.
 *
 * The provider goes on the header BUTTON, not the tree view: a provider
 * added to a widget's style context styles that widget only, and the
 * header buttons are separate widgets from the view.
 *
 * Gated on GDK_WINDOWING_QUARTZ rather than __APPLE__: the reason to
 * restyle is how the quartz backend draws buttons, so an X11 build on a
 * Mac correctly keeps its GTK theme.
 * ------------------------------------------------------------------------- */
static void
header_button_flatten(GtkWidget *hbtn)
{
#ifndef GDK_WINDOWING_QUARTZ
    (void)hbtn;                      /* Linux/X11: the GTK theme decides    */
#else
    lib_themed_bg_css_apply(hbtn, header_flatten_css);
#endif
}

static gboolean on_column_header_press(GtkWidget *, GdkEventButton *, gpointer);

/* on_task_activated() — double-click opens the editor window.  Mirrored
 * Notes items are ordinary tasks, so they open the ordinary editor.      */
static void
on_task_activated(GtkTreeView *view, GtkTreePath *path,
                  GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    TaskLibrary *lw = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    gint64 id;
    gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
    if (id == 0)                     /* the forecast's "No tasks due"
                                      * placeholder rows                    */
        return;
    task_editor_open(lw->app, id);
}

/* ---------------------------------------------------------------------------
 * on_task_done_toggled() — the ✓ column.  The checkbox is a VIEW of the
 * status, not a field of its own: it shows ticked exactly when the status
 * is Done, and clicking it writes a status back through
 * task_status_apply_done's rule — ticking means Done, unticking means In
 * Progress (a task that was ticked has plainly been worked on, so
 * dropping it back to New would lose that).  New is reachable only from
 * the editor's dropdown.
 * ------------------------------------------------------------------------- */
static void
on_task_done_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                     gpointer data)
{
    (void)cell;
    TaskLibrary *lw = data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(lw->task_store),
                                            &iter, path_str))
        task_rows_toggle_done(lw->app, lw->task_store, &iter);
}

/* task_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme (the Notes
 * notes-list stripes).  data is TaskLibrary * for the task pane columns so
 * the dragged row can be highlighted; NULL is safe (forecast day views).   */
static void
task_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    TaskLibrary *lw = data;            /* may be NULL for forecast day views */
    /* The stripe itself is the renderer's (task_rows.h) — one rule, so a
     * panel and the task pane cannot end up striping differently.  All
     * this adds is the drag highlight, which is the pane's own business. */
    const gchar *bg = task_rows_stripe_color(model, iter);

    /* While dragging, paint the held row amber so it is easy to track.
     *
     * By TASK ID, not by position.  This is a cell data func, so it runs
     * per row per DRAW on every one of the pane's columns — and comparing
     * positions meant building two GtkTreePaths each time (one off the row
     * reference, one off the iter), twelve allocations per row per frame
     * during exactly the gesture where frames are frequent.  The id is
     * already in the model and identity is all the highlight needs;
     * drag_row_ref stays for the motion handler, which genuinely needs the
     * row's live POSITION as the store is reordered underneath it.        */
    if (lw != NULL && lw->drag_active && lw->drag_task_id != 0) {
        gint64 id = 0;
        gtk_tree_model_get(model, iter, TL_ID, &id, -1);
        if (id == lw->drag_task_id)
            bg = DRAG_ROW_TINT;
    }

    g_object_set(cell, "cell-background", bg, NULL);
}

/* due_color_func() — tint the Due cell by urgency at draw time (rolls
 * over at midnight).  Undated rows must reset foreground-set — the
 * renderer is shared.  Also applies the row stripe: a column gets ONE
 * cell data func per renderer, so this one does both jobs.                 */
static void
due_color_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
               GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    task_row_bg_func(col, cell, model, iter, data);
    gint64 due;
    gtk_tree_model_get(model, iter, TL_DUE_RAW, &due, -1);
    const gchar *color = task_due_color(due);
    if (color == NULL)
        g_object_set(cell, "foreground-set", FALSE, NULL);
    else
        g_object_set(cell, "foreground", color, NULL);
}

/* sort_by_due() — soonest first; undated rows always last.                 */
static gint
sort_by_due(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
            gpointer data)
{
    (void)data;
    gint64 da, db;
    gtk_tree_model_get(model, a, TL_DUE_RAW, &da, -1);
    gtk_tree_model_get(model, b, TL_DUE_RAW, &db, -1);
    if (da == 0) da = G_MAXINT64;
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* sort_by_completed() — oldest-completed first; incomplete rows last.      */
static gint
sort_by_completed(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                  gpointer data)
{
    (void)data;
    gint64 da, db;
    gtk_tree_model_get(model, a, TL_COMPLETED_RAW, &da, -1);
    gtk_tree_model_get(model, b, TL_COMPLETED_RAW, &db, -1);
    if (da == 0) da = G_MAXINT64;
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ---------------------------------------------------------------------------
 * on_task_button_press() — right-click on a task row: keep an existing
 * multi-selection when clicked inside it (else select just that row)
 * and show the context menu, whose actions apply to the whole
 * selection.
 * ------------------------------------------------------------------------- */
static gboolean
on_task_button_press(GtkWidget *view, GdkEventButton *event, gpointer data)
{
    TaskLibrary *lw = data;

    /* Left-click in the drag handle column starts a manual reorder.
     * lib_manual_sort_live, not the raw flag: a search hides rows, and the
     * order writer would drop every hidden one (see lib_manual_sort_live).  */
    if (event->button == 1 && lib_manual_sort_live(lw)) {
        GtkTreePath      *path = NULL;
        GtkTreeViewColumn *col = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
            (gint)event->x, (gint)event->y, &path, &col, NULL, NULL)) {
            GtkTreeViewColumn *cdrag =
                g_object_get_data(G_OBJECT(lw->task_view), "task-cdrag");
            if (col == cdrag) {
                GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
                GtkTreeIter it;
                gint64 id = 0;
                if (gtk_tree_model_get_iter(model, &it, path))
                    gtk_tree_model_get(model, &it, TL_ID, &id, -1);
                if (id != 0) {
                    lw->drag_active  = TRUE;
                    lw->drag_task_id = id;
                    if (lw->drag_row_ref != NULL)
                        gtk_tree_row_reference_free(lw->drag_row_ref);
                    lw->drag_row_ref =
                        gtk_tree_row_reference_new(model, path);
                    gtk_widget_queue_draw(view); /* paint amber highlight   */
                    gtk_tree_path_free(path);
                    return TRUE;       /* consume — don't change selection  */
                }
            }
            gtk_tree_path_free(path);
        }
    }

    /* Right-click in the header area: event->window is the header GdkWindow,
     * not the bin_window, regardless of column clickability.  Detect this
     * by window identity and route to the column/sort menu.                */
    if (event->button == 3 &&
        event->window != gtk_tree_view_get_bin_window(GTK_TREE_VIEW(view)))
        return on_column_header_press(view, event, lw);

    if (event->button != 3)
        return FALSE;

    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);

    return task_context_menu_popup(lw, view, event);
}

/* ---------------------------------------------------------------------------
 * lib_row_order_permutation() — the display order a SAVED id list asks for,
 * as indices into `ids`.
 *
 *   ids   — the ids currently on screen, in their current order
 *   n     — how many
 *   saved — the config value: ids, comma separated, in the order the user
 *           dragged them into
 *
 * Returns a new gint[n] (g_free it) holding every index exactly once —
 * a valid permutation, which is what gtk_list_store_reorder requires —
 * or NULL when there is nothing to do.  Ids named by `saved` come first in
 * its sequence; anything it does not mention (a task created since) keeps
 * its current order at the tail.  It is FORGIVING by design: an id that no
 * longer exists matches nothing, and a pre-mirror order still holding
 * "NOTEID:ORD" tokens parses them to 0 and skips them.
 *
 * ONE function for BOTH panes.  The list view and the Kanban board keep
 * separate order KEYS on purpose, but the rule for reading one back is the
 * same rule, and it was written out twice — once over a GPtrArray of
 * tasks and once over the tree model — with a comment on the second
 * admitting it was "the same shape as" the first.  They differ only in
 * where the ids come from and what the caller does with the answer, so
 * that is all each caller now spells.
 *
 * The id lookup is a HASH, not the nested scan both copies used: that was
 * O(saved x rows), a quarter of a million comparisons on a 500-row list,
 * repeated on every refresh.  Keys point into `ids` itself, which outlives
 * the call, so no key is allocated.
 * ------------------------------------------------------------------------- */
gint *
lib_row_order_permutation(const gint64 *ids, gint n, const gchar *saved)
{
    if (ids == NULL || n <= 1 || saved == NULL || *saved == '\0')
        return NULL;

    /* id -> its FIRST index (+1, so a miss reads as NULL/0), plus a chain
     * threading every LATER index carrying the same id.  Built backwards,
     * so `head` ends on the lowest index and `next` runs forward from it.
     *
     * The chain is what makes this exactly the nested scan it replaces:
     * that scan took the first index with a matching id THAT WAS NOT YET
     * PLACED, so a saved list naming an id twice consumed two rows.  Ids
     * in one pane are unique and it cannot arise today — but a hash that
     * remembers only the first index would quietly diverge if that ever
     * stopped being true, and the difference would be a lost drag order,
     * not a crash.  Cheaper to be exact than to rely on the invariant.   */
    GHashTable *head = g_hash_table_new(g_int64_hash, g_int64_equal);
    gint       *next = g_new(gint, n);
    for (gint i = n - 1; i >= 0; i--) {
        gpointer v = g_hash_table_lookup(head, &ids[i]);
        next[i] = v != NULL ? GPOINTER_TO_INT(v) - 1 : -1;
        g_hash_table_insert(head, (gpointer)&ids[i], GINT_TO_POINTER(i + 1));
    }

    gint     *order  = g_new(gint, n);
    gboolean *placed = g_new0(gboolean, n);
    gint      fill   = 0;
    gchar   **parts  = g_strsplit(saved, ",", -1);
    for (gint i = 0; parts[i] != NULL; i++) {
        gint64   id = g_ascii_strtoll(parts[i], NULL, 10);
        gpointer v  = id != 0 ? g_hash_table_lookup(head, &id) : NULL;
        if (v == NULL)
            continue;
        gint j = GPOINTER_TO_INT(v) - 1;
        while (j >= 0 && placed[j])  /* rows this id already gave up       */
            j = next[j];
        if (j < 0)
            continue;
        order[fill++] = j;
        placed[j]     = TRUE;
        /* Advance the head so the NEXT mention of this id starts past the
         * row just taken — the whole walk stays O(n) rather than
         * re-traversing the chain from the top each time.               */
        g_hash_table_insert(head, (gpointer)&ids[j],
                            GINT_TO_POINTER(next[j] + 1));
    }
    g_strfreev(parts);
    g_hash_table_destroy(head);
    g_free(next);

    /* Everything the saved list did not claim, in the order it already
     * had.  This is what makes the result a permutation rather than a
     * subset, however partial or stale `saved` turns out to be.          */
    for (gint i = 0; i < n; i++)
        if (!placed[i])
            order[fill++] = i;
    g_free(placed);
    return order;
}

/* lib_row_order_key() — the order key for a sidebar row that carries its own
 * task order: "<family>_list_<id>" for a real list, "<family>_group_<id>"
 * for a group's aggregate.  NULL for any other row kind.
 *
 * Both families (manual_order and kanban_order) and both key deleters go
 * through here, so the ini spelling exists in ONE place — on_delete_list
 * and on_sb_ctx_delete_group have to name the very keys the pane wrote,
 * and a second copy of the format is how those drift.  New string
 * (g_free).                                                                */
gchar *
lib_row_order_key(const gchar *family, gint kind, gint64 id)
{
    const gchar *noun = kind == SB_KIND_LIST  ? "list"
                      : kind == SB_KIND_GROUP ? "group"
                      : NULL;
    if (noun == NULL)
        return NULL;
    return g_strdup_printf("%s_%s_%" G_GINT64_FORMAT, family, noun, id);
}

/* lib_row_order_keys_drop() — remove BOTH order keys of a sidebar row that is
 * going away.  Nothing else ever would, so the ini otherwise grows a dead
 * entry per family for every list and group ever deleted.                  */
void
lib_row_order_keys_drop(gint kind, gint64 id)
{
    static const gchar *families[] = { "manual_order", "kanban_order" };
    for (gsize i = 0; i < G_N_ELEMENTS(families); i++) {
        gchar *key = lib_row_order_key(families[i], kind, id);
        if (key == NULL)
            continue;
        task_app_config_set(key, NULL);         /* NULL removes the key     */
        g_free(key);
    }
}

/* view_order_key() — the config key for the current view's manual sort
 * order, or NULL if the view doesn't support it.  New string (g_free).     */
static gchar *
view_order_key(TaskLibrary *lw)
{
    gchar *key = lib_row_order_key("manual_order", lw->sel_kind, lw->sel_id);
    if (key != NULL)
        return key;
    return task_view_order_key(lib_sel_view(lw), "manual_order");
}

/* task_view_save_manual_order() — serialize the task pane's current row
 * order to config as a comma-separated list of task ids.  Every row is a
 * real task now (mirrored Notes items included), so the old
 * "NOTEID:ORD" token form is gone; a saved order still holding those
 * tokens simply finds no match and those entries drop out.                 */
static void
task_view_save_manual_order(TaskLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL) return;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    GString      *s     = g_string_new(NULL);
    GtkTreeIter   iter;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gint64 id;
            gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
            if (id != 0) {
                if (s->len > 0) g_string_append_c(s, ',');
                g_string_append_printf(s, "%" G_GINT64_FORMAT, id);
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    task_app_config_set(key, s->str);
    g_string_free(s, TRUE);
    g_free(key);
}

/* task_view_apply_manual_order() — after lib_refresh_tasks populates the store,
 * reorder rows to match the saved manual order for the current view.
 *
 * All this owns is where the ids come from (the model, in display order)
 * and what to do with the answer; the rule itself is
 * lib_row_order_permutation, shared with the Kanban board.                     */
void
task_view_apply_manual_order(TaskLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL) return;
    gchar *saved = task_app_config_get(key);
    g_free(key);
    if (saved == NULL) return;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    gint n = gtk_tree_model_iter_n_children(model, NULL);
    if (n <= 1) { g_free(saved); return; }

    /* Snapshot current row IDs (in display order). */
    gint64  *ids  = g_new(gint64, n);
    GtkTreeIter  iter;
    gtk_tree_model_get_iter_first(model, &iter);
    for (gint i = 0; i < n; i++) {
        gtk_tree_model_get(model, &iter, TL_ID, &ids[i], -1);
        gtk_tree_model_iter_next(model, &iter);
    }

    gint *order = lib_row_order_permutation(ids, n, saved);
    g_free(saved);
    g_free(ids);
    if (order == NULL)
        return;
    gtk_list_store_reorder(lw->task_store, order);
    g_free(order);
}

/* drag_handle_func() — cell data func for the drag handle column.  The row
 * stripe is all it does: the ⠿ glyph and its dimming are constants, so they
 * are set once on the renderer at construction instead of on every draw.
 * Kept as its own function (rather than pointing the column straight at
 * task_row_bg_func) because the column is where a per-row "this row cannot
 * move" state would land if one is ever added.                             */
static void
drag_handle_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    task_row_bg_func(col, cell, model, iter, data);
}

/* ---------------------------------------------------------------------------
 * task_drag_set_cursor() — update the cursor on the task view's GdkWindow:
 * "ns-resize" while over the drag handle column or while dragging, else
 * reset to the window default.
 *
 * Runs on EVERY motion event over the task view, so it holds no allocation:
 * the manual-sort flag comes from lw->manual_sort rather than the ini, and
 * the cursor is made once and kept on lw (created lazily — the display is
 * only reachable from a realized widget).
 * ------------------------------------------------------------------------- */
static void
task_drag_set_cursor(GtkWidget *widget, TaskLibrary *lw, gdouble x, gdouble y)
{
    GdkWindow  *win = gtk_widget_get_window(widget);
    if (win == NULL) return;
    gboolean want_resize = lw->drag_active;
    if (!want_resize && lib_manual_sort_live(lw)) {
        GtkTreeViewColumn *over = NULL;
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
            (gint)x, (gint)y, NULL, &over, NULL, NULL);
        GtkTreeViewColumn *cdrag =
            g_object_get_data(G_OBJECT(lw->task_view), "task-cdrag");
        want_resize = (over != NULL && over == cdrag);
    }
    if (want_resize && lw->drag_cursor == NULL)
        lw->drag_cursor = gdk_cursor_new_from_name(
            gtk_widget_get_display(widget), "ns-resize");
    /* NULL restores the window default — and is also what a display that
     * cannot supply "ns-resize" leaves us with, which is the right
     * fallback rather than a guessed stock cursor.                         */
    gdk_window_set_cursor(win, want_resize ? lw->drag_cursor : NULL);
}

/* on_task_leave_notify() — restore the default cursor when the pointer
 * leaves the task view (e.g. moving to another widget).                    */
static gboolean
on_task_leave_notify(GtkWidget *widget, GdkEventCrossing *ev, gpointer data)
{
    (void)ev; (void)data;
    GdkWindow *win = gtk_widget_get_window(widget);
    if (win) gdk_window_set_cursor(win, NULL);
    return FALSE;
}

/* on_task_drag_motion() — when the pointer enters a different row, swap
 * that row with the dragged row so the dragged item ends up under the
 * cursor.  Uses get_path_at_pos (no hysteresis) so the swap fires the
 * moment the pointer crosses a row boundary.                               */
static gboolean
on_task_drag_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    TaskLibrary *lw = data;
    task_drag_set_cursor(widget, lw, ev->x, ev->y);
    if (!lw->drag_active || lw->drag_row_ref == NULL)
        return FALSE;

    GtkTreePath *at_path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
        1, (gint)ev->y, &at_path, NULL, NULL, NULL);
    if (at_path == NULL)
        return FALSE;

    GtkTreePath *drag_path =
        gtk_tree_row_reference_get_path(lw->drag_row_ref);
    if (drag_path == NULL) { gtk_tree_path_free(at_path); return FALSE; }

    if (gtk_tree_path_compare(at_path, drag_path) == 0) {
        /* Cursor is back on the dragged row — clear the anti-flicker lock
         * so the next row the cursor enters will swap normally.            */
        if (lw->drag_lock_ref != NULL) {
            gtk_tree_row_reference_free(lw->drag_lock_ref);
            lw->drag_lock_ref = NULL;
        }
    } else {
        /* Check whether this is the row we just swapped with.  Row refs
         * auto-update through moves, so lock_path tracks the locked row
         * even after surrounding rows have shifted.                        */
        GtkTreePath *lock_path = lw->drag_lock_ref
            ? gtk_tree_row_reference_get_path(lw->drag_lock_ref) : NULL;
        gboolean locked = lock_path &&
            gtk_tree_path_compare(at_path, lock_path) == 0;
        if (lock_path) gtk_tree_path_free(lock_path);

        if (!locked) {
            GtkTreeIter  at_it, drag_it;
            GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
            if (gtk_tree_model_get_iter(model, &at_it,   at_path) &&
                gtk_tree_model_get_iter(model, &drag_it, drag_path)) {
                gint64 at_id;
                gtk_tree_model_get(model, &at_it, TL_ID, &at_id, -1);

                /* Every row carries a real id now — mirrored Notes
                 * items included — so the old "skip past the contiguous
                 * BN section" dance is gone: any row is a swap target.    */
                if (at_id != 0) {
                    gint drag_idx = gtk_tree_path_get_indices(drag_path)[0];
                    gint at_idx   = gtk_tree_path_get_indices(at_path)[0];
                    /* Lock the target BEFORE the move; the row ref will
                     * auto-update to track it at its new position.         */
                    if (lw->drag_lock_ref != NULL)
                        gtk_tree_row_reference_free(lw->drag_lock_ref);
                    lw->drag_lock_ref =
                        gtk_tree_row_reference_new(model, at_path);
                    if (at_idx < drag_idx)
                        gtk_list_store_move_before(lw->task_store,
                                                  &drag_it, &at_it);
                    else
                        gtk_list_store_move_after(lw->task_store,
                                                 &drag_it, &at_it);
                }
            }
        }
    }

    gtk_tree_path_free(at_path);
    gtk_tree_path_free(drag_path);
    return FALSE;
}

/* on_task_drag_release() — button released: end the drag and persist the
 * new row order.                                                           */
static gboolean
on_task_drag_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    (void)widget; (void)ev;
    TaskLibrary *lw = data;
    if (!lw->drag_active) return FALSE;
    lw->drag_active  = FALSE;
    lw->drag_task_id = 0;
    if (lw->drag_row_ref != NULL) {
        gtk_tree_row_reference_free(lw->drag_row_ref);
        lw->drag_row_ref = NULL;
    }
    if (lw->drag_lock_ref != NULL) {
        gtk_tree_row_reference_free(lw->drag_lock_ref);
        lw->drag_lock_ref = NULL;
    }
    task_view_save_manual_order(lw);
    gtk_widget_queue_draw(widget);   /* clear the amber highlight           */
    GdkWindow *win = gtk_widget_get_window(widget);
    if (win) gdk_window_set_cursor(win, NULL);
    return FALSE;
}

/* task_manual_sort_apply() — sync the task view to the current
 * task_list_manual_sort config: show/hide drag handle, enable/disable
 * column-header click-to-sort, and clear any active sort indicator.
 * ALSO the single writer of lw->manual_sort, the cached copy the
 * per-motion and per-refresh paths read instead of the ini — every writer
 * of the config key calls this straight afterwards, so the cache cannot
 * drift.                                                                   */
void
task_manual_sort_apply(TaskLibrary *lw)
{
    lw->manual_sort =
        task_app_config_get_bool("task_list_manual_sort", FALSE);
    /* What the COLUMNS show is what is actually on offer, which a search
     * suspends (see lib_manual_sort_live) — so the ⠿ handle goes and the
     * headers become clickable again, giving the filtered view the sorting
     * it can still do.  The cached SETTING above is untouched: clearing the
     * box must bring hand-sorting back, not turn it off.                  */
    gboolean manual = lib_manual_sort_live(lw);
    GtkTreeViewColumn *cdrag =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdrag");
    GtkTreeViewColumn *cdone =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdone");
    GtkTreeViewColumn *cdesc =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdesc");
    GtkTreeViewColumn *cstatus =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cstatus");
    GtkTreeViewColumn *cdue  =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdue");
    GtkTreeViewColumn *ccompleted =
        g_object_get_data(G_OBJECT(lw->task_view), "task-ccompleted");
    if (cdrag)      gtk_tree_view_column_set_visible(cdrag, manual);
    if (cdone)      gtk_tree_view_column_set_clickable(cdone,      !manual);
    if (cdesc)      gtk_tree_view_column_set_clickable(cdesc,      !manual);
    if (cstatus)    gtk_tree_view_column_set_clickable(cstatus,    !manual);
    if (cdue)       gtk_tree_view_column_set_clickable(cdue,       !manual);
    if (ccompleted) gtk_tree_view_column_set_clickable(ccompleted, !manual);
    if (manual)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(lw->task_store),
            GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
            GTK_SORT_ASCENDING);
}

/* on_column_toggled() — a column visibility check item was clicked: update
 * the column visibility and persist in config.                             */
static void
on_column_toggled(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    GtkTreeViewColumn *col = g_object_get_data(G_OBJECT(item), "task-col");
    TaskLibrary         *lw  = g_object_get_data(G_OBJECT(item), "task-lw");
    if (!col || !lw) return;
    const gchar *key = g_object_get_data(G_OBJECT(col), "task-colkey");
    gboolean vis = gtk_check_menu_item_get_active(item);
    gtk_tree_view_column_set_visible(col, vis);
    if (key) {
        gchar *cfg = g_strdup_printf("col_%s_visible", key);
        task_app_config_set(cfg, vis ? "1" : "0");
        g_free(cfg);
    }
}

/* task_columns_apply() — restore persisted column visibility.              */
static void
task_columns_apply(TaskLibrary *lw)
{
    GtkTreeViewColumn *cdone =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdone");
    GtkTreeViewColumn *cstatus =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cstatus");
    GtkTreeViewColumn *cdue  =
        g_object_get_data(G_OBJECT(lw->task_view), "task-cdue");
    GtkTreeViewColumn *ccompleted =
        g_object_get_data(G_OBJECT(lw->task_view), "task-ccompleted");
    if (cdone)
        gtk_tree_view_column_set_visible(cdone,
            task_app_config_get_bool("col_done_visible", TRUE));
    /* Status defaults to HIDDEN: the ✓ column already says what most
     * rows need, and the header right-click menu is where anyone who
     * wants the third state on screen turns it on.                        */
    if (cstatus)
        gtk_tree_view_column_set_visible(cstatus,
            task_app_config_get_bool("col_status_visible", FALSE));
    if (cdue)
        gtk_tree_view_column_set_visible(cdue,
            task_app_config_get_bool("col_due_visible", TRUE));
    if (ccompleted)
        gtk_tree_view_column_set_visible(ccompleted,
            task_app_config_get_bool("col_completed_visible", TRUE));
}

/* on_column_header_press() — right-click on any column header pops a menu
 * of check items for the hidable columns (Done, Status, Due Date and
 * Completion Date; Task always shows and has no entry).                    */
static gboolean
on_column_header_press(GtkWidget *btn, GdkEventButton *ev, gpointer data)
{
    (void)btn;
    if (ev->button != 3) return FALSE;
    TaskLibrary *lw = data;
    GtkWidget *menu = gtk_menu_new();

    GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(lw->task_view));
    for (GList *l = cols; l; l = l->next) {
        GtkTreeViewColumn *col   = l->data;
        const gchar       *key   =
            g_object_get_data(G_OBJECT(col), "task-colkey");
        const gchar       *label =
            g_object_get_data(G_OBJECT(col), "task-collabel");
        if (!key) continue;
        GtkWidget *item = gtk_check_menu_item_new_with_label(label);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
            gtk_tree_view_column_get_visible(col));
        g_object_set_data(G_OBJECT(item), "task-col", col);
        g_object_set_data(G_OBJECT(item), "task-lw",  lw);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_column_toggled), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    g_list_free(cols);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_list_build() — build the task list, its columns and lw->task_scroll (see library_priv.h).
 * ------------------------------------------------------------------------- */
void
task_list_build(TaskLibrary *lw)
{
    /* Task pane.                                                           */
    lw->task_store = gtk_list_store_new(TL_N_COLS, G_TYPE_INT64,
                                        G_TYPE_BOOLEAN, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_INT64,
                                        G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_INT64, G_TYPE_INT,
                                        G_TYPE_STRING);
    lw->task_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->task_store));
    g_object_unref(lw->task_store);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(lw->task_view), FALSE);
    /* Multi-select: Ctrl-click (Cmd on macOS — GTK maps the platform's
     * modify-selection modifier) and Shift-click extend; the context
     * menu's actions apply to the whole selection.                         */
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view)),
        GTK_SELECTION_MULTIPLE);
    g_signal_connect(lw->task_view, "row-activated",
                     G_CALLBACK(on_task_activated), lw);
    g_signal_connect(lw->task_view, "button-press-event",
                     G_CALLBACK(on_task_button_press), lw);

    /* Drag handle column — shown only in manual sort mode.  The glyph comes
     * from the renderer itself, not the model and not the data func: it is
     * the same on every row, and a data func runs per DRAW, so setting it
     * there was two property notifications per visible row per redraw.
     * Dimming is Pango `alpha` on the markup, never a fixed gray — a gray
     * is unreadable on the blue selection, while alpha rides whatever
     * foreground the row already has.                                      */
    GtkCellRenderer   *drag_cell = gtk_cell_renderer_text_new();
    g_object_set(drag_cell, "ypad", 8, "xpad", 4,
                 "markup",                       /* ⠿ handle glyph          */
                 "<span alpha=\"55%\">\xe2\xa0\xbf</span>", NULL);
    GtkTreeViewColumn *cdrag     = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(cdrag, "");
    gtk_tree_view_column_pack_start(cdrag, drag_cell, FALSE);
    gtk_tree_view_column_set_cell_data_func(cdrag, drag_cell,
                                            drag_handle_func, lw, NULL);
    gtk_tree_view_column_set_clickable(cdrag, FALSE);
    gtk_tree_view_column_set_sizing(cdrag, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(cdrag, 26);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdrag);

    /* Done checkbox column — a convenience VIEW of the status column two
     * places to its right: ticked means Done, and a click writes Done or
     * In Progress back (on_task_done_toggled).  Every column's renderer
     * also runs the stripe data func — the alternating background must
     * span the row.                                                        */
    GtkCellRenderer *done_cell = gtk_cell_renderer_toggle_new();
    g_signal_connect(done_cell, "toggled",
                     G_CALLBACK(on_task_done_toggled), lw);
    GtkTreeViewColumn *cdone =
        gtk_tree_view_column_new_with_attributes("\xe2\x9c\x93",
            done_cell, "active", TL_DONE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdone, done_cell,
                                            task_row_bg_func, lw, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdone);

    /* Task description column — the tall multi-line markup cell.           */
    GtkCellRenderer *desc_cell = gtk_cell_renderer_text_new();
    g_object_set(desc_cell,
                 "ypad", 8,
                 "ellipsize", PANGO_ELLIPSIZE_END,
                 NULL);
    GtkTreeViewColumn *cdesc =
        gtk_tree_view_column_new_with_attributes("Task", desc_cell,
            "markup", TL_DESC, NULL);
    gtk_tree_view_column_set_cell_data_func(cdesc, desc_cell,
                                            task_row_bg_func, lw, NULL);
    gtk_tree_view_column_set_expand(cdesc, TRUE);
    gtk_tree_view_column_set_resizable(cdesc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdesc);

    /* Status column — New / In Progress / Done, sorted by the enum
     * (TL_STATUS) rather than the label, so the order is the workflow's
     * and not the alphabet's.                                              */
    GtkCellRenderer *status_cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *cstatus =
        gtk_tree_view_column_new_with_attributes("Status", status_cell,
            "text", TL_STATUS_TEXT, NULL);
    gtk_tree_view_column_set_cell_data_func(cstatus, status_cell,
                                            task_row_bg_func, lw, NULL);
    gtk_tree_view_column_set_resizable(cstatus, TRUE);
    gtk_tree_view_column_set_sort_column_id(cstatus, TL_STATUS);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cstatus);

    /* Due Date column, urgency-tinted, sortable (undated last).            */
    GtkCellRenderer *due_cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *cdue =
        gtk_tree_view_column_new_with_attributes("Due Date", due_cell,
            "text", TL_DUE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdue, due_cell,
                                            due_color_func, lw, NULL);
    gtk_tree_view_column_set_resizable(cdue, TRUE);
    gtk_tree_sortable_set_sort_func(
        GTK_TREE_SORTABLE(lw->task_store), TL_DUE_RAW,
        sort_by_due, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(cdue, TL_DUE_RAW);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdue);

    /* Completed column — sortable (incomplete rows last).                  */
    GtkCellRenderer *completed_cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *ccompleted =
        gtk_tree_view_column_new_with_attributes("Completed", completed_cell,
            "text", TL_COMPLETED, NULL);
    gtk_tree_view_column_set_cell_data_func(ccompleted, completed_cell,
                                            task_row_bg_func, lw, NULL);
    gtk_tree_view_column_set_resizable(ccompleted, TRUE);
    gtk_tree_sortable_set_sort_func(
        GTK_TREE_SORTABLE(lw->task_store), TL_COMPLETED_RAW,
        sort_by_completed, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(ccompleted, TL_COMPLETED_RAW);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), ccompleted);

    /* Make Done and Task columns sortable by header click.  Task sorts by
     * the raw title string (TL_TITLE), not the Pango markup (TL_DESC).    */
    gtk_tree_view_column_set_sort_column_id(cdone, TL_DONE);
    gtk_tree_view_column_set_sort_column_id(cdesc, TL_TITLE);

    /* Column hide/show via header right-click.  Done, Status, Due Date and
     * Completed are hidable (Task always shows); task-colkey/task-collabel
     * drive the menu.  Store column refs on the view for task_columns_apply
     * and the realize-time header-button connection.                       */
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdrag",      cdrag);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdone",      cdone);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdesc",      cdesc);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cstatus",    cstatus);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdue",       cdue);
    g_object_set_data(G_OBJECT(lw->task_view), "task-ccompleted", ccompleted);
    g_object_set_data(G_OBJECT(cdone),      "task-colkey",   (gpointer)"done");
    g_object_set_data(G_OBJECT(cdone),      "task-collabel", (gpointer)"Done");
    g_object_set_data(G_OBJECT(cstatus),    "task-colkey",   (gpointer)"status");
    g_object_set_data(G_OBJECT(cstatus),    "task-collabel", (gpointer)"Status");
    g_object_set_data(G_OBJECT(cdue),       "task-colkey",   (gpointer)"due");
    g_object_set_data(G_OBJECT(cdue),       "task-collabel", (gpointer)"Due Date");
    g_object_set_data(G_OBJECT(ccompleted), "task-colkey",   (gpointer)"completed");
    g_object_set_data(G_OBJECT(ccompleted), "task-collabel", (gpointer)"Completion Date");
    GtkTreeViewColumn *header_cols[] = { cdrag, cdone, cdesc, cstatus, cdue,
                                         ccompleted };
    for (gsize i = 0; i < G_N_ELEMENTS(header_cols); i++) {
        GtkWidget *hbtn = gtk_tree_view_column_get_button(header_cols[i]);
        if (hbtn) {
            g_signal_connect(hbtn, "button-press-event",
                             G_CALLBACK(on_column_header_press), lw);
            header_button_flatten(hbtn);   /* match the status bar          */
        }
    }
    task_columns_apply(lw);
    task_manual_sort_apply(lw);   /* show/hide cdrag per persisted setting  */

    /* Motion, release, and leave events for live-drag reorder + cursor. */
    gtk_widget_add_events(lw->task_view,
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(lw->task_view, "motion-notify-event",
                     G_CALLBACK(on_task_drag_motion), lw);
    g_signal_connect(lw->task_view, "button-release-event",
                     G_CALLBACK(on_task_drag_release), lw);
    g_signal_connect(lw->task_view, "leave-notify-event",
                     G_CALLBACK(on_task_leave_notify), lw);

    lw->task_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->task_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lw->task_scroll), lw->task_view);
}
