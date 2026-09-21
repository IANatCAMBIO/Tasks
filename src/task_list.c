/* ===========================================================================
 * task_list.c — the library window's task LIST pane: the GtkColumnView and its
 * columns, the header visibility menu, the manual-sort row drag and the
 * persisted row orders (see library_priv.h).
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include <stdlib.h>
#include <string.h>

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
 * a valid permutation — or NULL when there is nothing to do.  Ids named by
 * `saved` come first in its sequence; anything it does not mention (a task
 * created since) keeps its current order at the tail.  It is FORGIVING by
 * design: an id that no longer exists matches nothing, and a pre-mirror
 * order still holding "NOTEID:ORD" tokens parses them to 0 and skips them.
 *
 * ONE function for BOTH panes.  The list view and the Kanban board keep
 * separate order KEYS on purpose, but the rule for reading one back is the
 * same rule.
 *
 * The id lookup is a HASH, not the nested scan the original code used: that
 * was O(saved × rows), a quarter of a million comparisons on a 500-row
 * list, repeated on every refresh.  Keys point into `ids` itself, which
 * outlives the call, so no key is allocated.
 * ------------------------------------------------------------------------- */
gint *
lib_row_order_permutation(const gint64 *ids, gint n, const gchar *saved)
{
    if (ids == NULL || n <= 1 || saved == NULL || *saved == '\0')
        return NULL;

    /* id → its FIRST index (+1, so a miss reads as NULL/0), plus a chain
     * threading every LATER index carrying the same id.  Built backwards,
     * so `head` ends on the lowest index and `next` runs forward from it.  */
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
        g_hash_table_insert(head, (gpointer)&ids[j],
                            GINT_TO_POINTER(next[j] + 1));
    }
    g_strfreev(parts);
    g_hash_table_destroy(head);
    g_free(next);

    /* Everything the saved list did not claim, in the order it already had. */
    for (gint i = 0; i < n; i++)
        if (!placed[i])
            order[fill++] = i;
    g_free(placed);
    return order;
}

/*
 * lib_row_order_key — the config key for a sidebar row's task order.
 *
 * "<family>_list_<id>" for a real list, "<family>_group_<id>" for a group's
 * aggregate.  NULL for any other row kind.  New string (g_free).
 *
 * Both families (manual_order and kanban_order) and both key deleters go
 * through here, so the ini spelling exists in ONE place.
 *
 * Inputs:
 *   family — "manual_order" or "kanban_order"
 *   kind   — SB_KIND_LIST or SB_KIND_GROUP
 *   id     — the list or group id
 * Output: new key string, or NULL (g_free).
 */
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

/*
 * lib_row_order_keys_drop — remove BOTH order keys of a sidebar row that is
 * going away (both manual_order and kanban_order families).
 *
 * Nothing else removes them, so the ini would otherwise grow one dead entry
 * per family for every list and group ever deleted.
 *
 * Inputs:
 *   kind — SB_KIND_LIST or SB_KIND_GROUP
 *   id   — the list or group id
 * Output: none
 */
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

/*
 * task_view_save_manual_order — serialize the task pane's current row order
 * to config as a comma-separated list of task ids.
 *
 * Iterates task_store (which equals the display order when the sorter is
 * NULL in manual-sort mode).
 *
 * Inputs: lw — the library window
 * Output: none
 */
static void
task_view_save_manual_order(TaskLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL)
        return;
    guint  n = g_list_model_get_n_items(G_LIST_MODEL(lw->task_store));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < n; i++) {
        TaskRow *row = TASK_ROW(g_list_model_get_item(
                                    G_LIST_MODEL(lw->task_store), i));
        if (row != NULL) {
            if (row->id != 0) {
                if (s->len > 0)
                    g_string_append_c(s, ',');
                g_string_append_printf(s, "%" G_GINT64_FORMAT, row->id);
            }
            g_object_unref(row);
        }
    }
    task_app_config_set(key, s->str);
    g_string_free(s, TRUE);
    g_free(key);
}

/*
 * task_view_apply_manual_order — after a refresh populates the store,
 * reorder rows to match the saved manual order for the current view.
 *
 * All this owns is where the ids come from (the store, in its current order)
 * and what to do with the answer; the rule itself is lib_row_order_permutation,
 * shared with the Kanban board.
 *
 * Inputs: lw — the library window
 * Output: none
 */
void
task_view_apply_manual_order(TaskLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL)
        return;
    gchar *saved = task_app_config_get(key);
    g_free(key);
    if (saved == NULL)
        return;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->task_store));
    if (n <= 1) {
        g_free(saved);
        return;
    }

    /* Snapshot current row ids (in display order) and collect the items. */
    gint64  *ids   = g_new(gint64, n);
    gpointer *items = g_new(gpointer, n);
    for (guint i = 0; i < n; i++) {
        TaskRow *row = TASK_ROW(g_list_model_get_item(
                                    G_LIST_MODEL(lw->task_store), i));
        ids[i]   = row ? row->id : 0;
        items[i] = row;              /* holds the ref                       */
    }

    gint *order = lib_row_order_permutation(ids, (gint)n, saved);
    g_free(saved);
    g_free(ids);

    if (order != NULL) {
        /* Build a new items array in the permuted order, then splice
         * the whole store at once.                                         */
        gpointer *new_items = g_new(gpointer, n);
        for (guint i = 0; i < n; i++)
            new_items[i] = items[order[i]];
        g_free(order);

        g_list_store_splice(lw->task_store, 0, n, new_items, n);
        g_free(new_items);
    }

    for (guint i = 0; i < n; i++)
        if (items[i])
            g_object_unref(items[i]);
    g_free(items);
}

/* ---------------------------------------------------------------------------
 * CSS installed once for the task pane.
 * ------------------------------------------------------------------------- */

/* task_list_install_css() — install the task-list stylesheet once per
 * process.  Stripes, urgency tints and drag-state classes all live here
 * so they apply to every column in the view automatically.                  */
static void
task_list_install_css(void)
{
    static gboolean done = FALSE;
    if (done)
        return;
    done = TRUE;

    /* Row stripes: even rows get the pale-blue tint, but NEVER over the
     * selection highlight (the blue would clash).  Urgency tints are on
     * the GtkLabel in the Due Date cell, so they do not bleed to other
     * columns.  Drag classes sit on the handle GtkLabel: task-drag-src
     * (the row being dragged) gets the amber highlight; task-drag-mark
     * (the target row) gets a top border showing where it would land.
     * No focus ring or pressed shadow on rows (same rationale as Notes). */
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_string(prov,
        /* Row stripes */
        "columnview.task-list > listview > row:nth-child(even)"
        ":not(:selected) { background-color: " ROW_TINT "; }"
        /* No focus ring */
        "columnview.task-list > listview > row:focus:focus-visible"
        " { outline-width: 0; transition: none; }"
        /* No pressed-state shadow */
        "columnview.task-list > listview > row:active"
        " { box-shadow: none; }"
        /* Due urgency tints */
        "columnview.task-list label.task-overdue   { color: #c01c28; }"
        "columnview.task-list label.task-due-today { color: #d19a00; }"
        "columnview.task-list label.task-due-ahead { color: #26a269; }"
        /* Drag source: amber handle */
        "columnview.task-list label.task-drag-src"
        " { background: " DRAG_ROW_TINT "; }"
        /* Drag marker: 2 px top rule at target position */
        "columnview.task-list label.task-drag-mark"
        " { border-top: 2px solid @theme_fg_color; }"
        /* Column headers: bottom rule only, no left border doubling */
        "columnview.task-list > header > button"
        " { border-left: none; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

/* ---------------------------------------------------------------------------
 * Column visibility — stateful "win.column-<key>" actions whose state is the
 * column's visibility; the header menu's items name them.  The change-state
 * path is the ONE writer of both the column and its col_<key>_visible key.
 * An `activate` handler on a stateful action does not flip the state, so
 * the check mark would never move — hence change-state.
 * ------------------------------------------------------------------------- */

/*
 * on_column_change_state — toggle a column's visibility and persist it.
 *
 * Inputs:
 *   action — the stateful GSimpleAction being toggled
 *   value  — the new GVariant(bool) state
 *   data   — unused (column is stored on the action)
 * Output: none
 */
static void
on_column_change_state(GSimpleAction *action, GVariant *value, gpointer data)
{
    (void)data;
    GtkColumnViewColumn *col = g_object_get_data(G_OBJECT(action), "task-col");
    const gchar         *key = g_object_get_data(G_OBJECT(col), "task-colkey");
    gboolean vis = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    gtk_column_view_column_set_visible(col, vis);
    gchar *cfg = g_strdup_printf("col_%s_visible", key);
    task_app_config_set(cfg, vis ? "1" : "0");
    g_free(cfg);
}

/* column_action_name() — "column-<key>" name for the GAction.  New string. */
static gchar *
column_action_name(const gchar *key)
{
    return g_strdup_printf("column-%s", key);
}

/*
 * task_list_install_actions — one visibility action per hidable column,
 * seeded from the ini and applied to the column at once.
 *
 * Status defaults to HIDDEN: the ✓ column already conveys what most rows
 * need, and the header right-click menu is where anyone who wants the
 * third state on screen turns it on.
 *
 * Inputs: lw — the library window (columns must already be built)
 * Output: none
 */
void
task_list_install_actions(TaskLibrary *lw)
{
    static const struct {
        const gchar *data_key;       /* column stored on the view           */
        const gchar *key;            /* ini key's middle: col_<key>_…      */
        gboolean     def;            /* default visibility                  */
    } COLUMNS[] = {
        { "task-cdone",      "done",      TRUE  },
        { "task-cstatus",    "status",    FALSE },
        { "task-cdue",       "due",       TRUE  },
        { "task-ccompleted", "completed", TRUE  },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(COLUMNS); i++) {
        GtkColumnViewColumn *col = g_object_get_data(G_OBJECT(lw->task_view),
                                                      COLUMNS[i].data_key);
        gchar *cfg  = g_strdup_printf("col_%s_visible", COLUMNS[i].key);
        gchar *name = column_action_name(COLUMNS[i].key);
        gboolean vis = task_app_config_get_bool(cfg, COLUMNS[i].def);
        gtk_column_view_column_set_visible(col, vis);
        GSimpleAction *action = g_simple_action_new_stateful(
            name, NULL, g_variant_new_boolean(vis));
        g_object_set_data(G_OBJECT(action), "task-col", col);
        g_signal_connect(action, "change-state",
                         G_CALLBACK(on_column_change_state), lw);
        g_action_map_add_action(G_ACTION_MAP(lw->window), G_ACTION(action));
        g_object_unref(action);
        g_free(name);
        g_free(cfg);
    }

    /* Build the shared header menu (same menu on every column) and install
     * it.  Each item names "win.column-<key>" and carries a check mark
     * from the action's state.                                             */
    GListModel *cols = gtk_column_view_get_columns(
                           GTK_COLUMN_VIEW(lw->task_view));
    guint ncols = g_list_model_get_n_items(cols);
    GMenu *menu = g_menu_new();
    for (gsize i = 0; i < G_N_ELEMENTS(COLUMNS); i++) {
        gchar *action = g_strdup_printf("win.column-%s", COLUMNS[i].key);
        /* The label is the collabel stored on the column. */
        GtkColumnViewColumn *col = g_object_get_data(G_OBJECT(lw->task_view),
                                                      COLUMNS[i].data_key);
        const gchar *label = g_object_get_data(G_OBJECT(col), "task-collabel");
        g_menu_append(menu, label != NULL ? label : COLUMNS[i].key, action);
        g_free(action);
    }
    for (guint i = 0; i < ncols; i++) {
        GtkColumnViewColumn *col = g_list_model_get_item(cols, i);
        gtk_column_view_column_set_header_menu(col, G_MENU_MODEL(menu));
        g_object_unref(col);
    }
    g_object_unref(menu);
}

/* ---------------------------------------------------------------------------
 * task_manual_sort_apply() — sync the task view to the current
 * task_list_manual_sort config.
 *
 * When manual sort is active: set the sort model's sorter to NULL (so the
 * store order becomes the display order), and show the drag handle column.
 * When off: reconnect the column view's header sorter.
 *
 * This is the SINGLE writer of lw->manual_sort, so the cached copy the
 * per-motion and per-refresh paths read cannot drift.
 * ------------------------------------------------------------------------- */
void
task_manual_sort_apply(TaskLibrary *lw)
{
    lw->manual_sort =
        task_app_config_get_bool("task_list_manual_sort", FALSE);
    /* lib_manual_sort_live, not the raw flag: a search suspends dragging.   */
    gboolean manual = lib_manual_sort_live(lw);
    if (lw->col_drag)
        gtk_column_view_column_set_visible(lw->col_drag, manual);
    /* In manual mode, disconnect the header sorter so the store order is
     * used directly; in automatic mode, reconnect it so header clicks sort. */
    GtkSorter *sorter = manual
        ? NULL
        : gtk_column_view_get_sorter(GTK_COLUMN_VIEW(lw->task_view));
    gtk_sort_list_model_set_sorter(lw->task_sorted, sorter);
}

/* ---------------------------------------------------------------------------
 * Factory callbacks — the per-column setup/bind pairs that build and fill
 * each cell widget.  "setup" runs once per recycled slot; "bind" runs on
 * every item change (and again via "changed" when task_row_touch fires).
 * ------------------------------------------------------------------------- */

/* --- Drag handle column (col_drag) ----------------------------------------
 * A GtkLabel carrying the ⠿ glyph, with a GtkGestureDrag attached.
 * Visible only in manual-sort mode; drives the row-reorder gesture.        */

static void on_handle_drag_begin(GtkGestureDrag *, gdouble, gdouble, gpointer);
static void on_handle_drag_update(GtkGestureDrag *, gdouble, gdouble, gpointer);
static void on_handle_drag_end(GtkGestureDrag *, gdouble, gdouble, gpointer);
static void on_done_toggled(GtkCheckButton *, gpointer);

/*
 * on_drag_handle_setup — create the handle label and attach the drag gesture.
 * Inputs: standard GtkSignalListItemFactory "setup" arguments + TaskLibrary *
 * Output: none
 */
static void
on_drag_handle_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    TaskLibrary *lw = data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label),
                         "<span alpha=\"55%\">\xe2\xa0\xbf</span>");
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_widget_set_size_request(label, 26, -1);

    /* Cursor: ns-resize while hovered — built once and kept.               */
    if (lw->drag_cursor == NULL)
        lw->drag_cursor = gdk_cursor_new_from_name("ns-resize", NULL);
    gtk_widget_set_cursor(label, lw->drag_cursor);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin",
                     G_CALLBACK(on_handle_drag_begin),  lw);
    g_signal_connect(drag, "drag-update",
                     G_CALLBACK(on_handle_drag_update), lw);
    g_signal_connect(drag, "drag-end",
                     G_CALLBACK(on_handle_drag_end),    lw);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(drag));

    gtk_list_item_set_child(item, label);
}

/*
 * on_drag_handle_bind — update the handle label's drag-state CSS classes.
 *
 * The dragged row wears task-drag-src (amber); the current target row wears
 * task-drag-mark (top border separator).  Both are removed first so a stale
 * class from a previous bind is never left on a recycled slot.
 *
 * Inputs: standard GtkSignalListItemFactory "bind" arguments + TaskLibrary *
 * Output: none
 */
static void
on_drag_handle_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    TaskLibrary *lw  = data;
    GtkWidget   *lbl = gtk_list_item_get_child(item);
    TaskRow     *row = TASK_ROW(gtk_list_item_get_item(item));
    guint        pos = gtk_list_item_get_position(item);

    /* Record position so drag-begin can find it.                           */
    g_object_set_data(G_OBJECT(lbl), "task-drag-pos", GUINT_TO_POINTER(pos));

    gtk_widget_remove_css_class(lbl, "task-drag-src");
    gtk_widget_remove_css_class(lbl, "task-drag-mark");
    if (lw->drag_active && row != NULL) {
        if (row->id == lw->drag_task_id)
            gtk_widget_add_css_class(lbl, "task-drag-src");
        if ((gint)pos == lw->drag_mark_pos)
            gtk_widget_add_css_class(lbl, "task-drag-mark");
    }
}

/* find_widget_at_task_y() — walk up from the widget found by gtk_widget_pick
 * at (task_view_x, task_view_y) to the first ancestor that carries the
 * "task-drag-pos" object data.  Returns the widget or NULL.                */
static GtkWidget *
find_widget_at_task_y(GtkWidget *task_view, gdouble vx, gdouble vy)
{
    GtkWidget *hit = gtk_widget_pick(task_view, vx, vy, GTK_PICK_DEFAULT);
    while (hit != NULL && hit != task_view) {
        if (g_object_get_data(G_OBJECT(hit), "task-drag-pos") != NULL)
            return hit;
        hit = gtk_widget_get_parent(hit);
    }
    return NULL;
}

/* touch_row_at() — emit task_row_touch for the item at `pos` in task_store
 * so its bound factories rebind (which updates drag-state CSS classes).     */
static void
touch_row_at(TaskLibrary *lw, guint pos)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->task_store));
    if (pos >= n)
        return;
    TaskRow *row = TASK_ROW(g_list_model_get_item(G_LIST_MODEL(lw->task_store),
                                                   pos));
    if (row != NULL) {
        task_row_touch(lw->task_store, row);
        g_object_unref(row);
    }
}

/*
 * on_handle_drag_begin — arm the manual-sort row drag.
 *
 * Records the dragged task id and its starting position; sets drag_active.
 * The row is NOT moved yet — it only moves on drag-end.
 *
 * Inputs:
 *   gesture — the GtkGestureDrag on the handle label
 *   sx, sy  — press position in the label's coordinates
 *   data    — TaskLibrary *
 * Output: none
 */
static void
on_handle_drag_begin(GtkGestureDrag *gesture, gdouble sx, gdouble sy,
                     gpointer data)
{
    (void)sx; (void)sy;
    TaskLibrary *lw  = data;
    GtkWidget   *lbl = gtk_event_controller_get_widget(
                           GTK_EVENT_CONTROLLER(gesture));
    guint pos = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(lbl),
                                                    "task-drag-pos"));
    guint n   = g_list_model_get_n_items(G_LIST_MODEL(lw->task_store));
    if (pos >= n)
        return;

    TaskRow *row = TASK_ROW(g_list_model_get_item(G_LIST_MODEL(lw->task_store),
                                                   pos));
    if (row == NULL || row->id == 0) {
        g_clear_object(&row);
        return;
    }
    gint64 id = row->id;
    g_object_unref(row);

    /* lib_manual_sort_live, not the raw setting: a search suspends drags.  */
    if (!lib_manual_sort_live(lw))
        return;

    lw->drag_active   = TRUE;
    lw->drag_task_id  = id;
    lw->drag_from     = pos;
    lw->drag_mark_pos = (gint)pos;
    lw->drag_mark_row = NULL;

    /* Highlight the dragged row immediately.                               */
    touch_row_at(lw, pos);
}

/*
 * on_handle_drag_update — track pointer movement and update the drop marker.
 *
 * Converts the cumulative drag offset to the column view's coordinate
 * space, hit-tests for the handle widget there, and updates drag_mark_pos
 * to show where the row would land.
 *
 * Inputs:
 *   gesture       — the GtkGestureDrag
 *   offset_x/y    — cumulative offset from the press point, in label coords
 *   data          — TaskLibrary *
 * Output: none
 */
static void
on_handle_drag_update(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y,
                      gpointer data)
{
    (void)offset_x;
    TaskLibrary *lw = data;
    if (!lw->drag_active)
        return;

    GtkWidget *lbl = gtk_event_controller_get_widget(
                         GTK_EVENT_CONTROLLER(gesture));
    gdouble sx, sy;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);

    /* Convert to task_view coordinates.                                    */
    graphene_point_t src = { (float)(sx + offset_x), (float)(sy + offset_y) };
    graphene_point_t dst;
    if (!gtk_widget_compute_point(lbl, lw->task_view, &src, &dst))
        return;
    gdouble vx = dst.x, vy = dst.y;

    GtkWidget *target = find_widget_at_task_y(lw->task_view, vx, vy);
    if (target == NULL)
        return;

    guint tpos = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(target),
                                                      "task-drag-pos"));
    gint old_mark = lw->drag_mark_pos;
    lw->drag_mark_pos = (gint)tpos;

    if (lw->drag_mark_pos != old_mark) {
        /* Rebind the old and new mark rows to update their CSS classes.    */
        if (old_mark >= 0)
            touch_row_at(lw, (guint)old_mark);
        touch_row_at(lw, tpos);
    }
}

/*
 * on_handle_drag_end — commit the row move and persist the new order.
 *
 * Moves the row from drag_from to drag_mark_pos in task_store via a single
 * g_list_store_splice, which keeps the change atomic.
 *
 * Inputs:
 *   gesture       — the GtkGestureDrag
 *   offset_x/y    — final cumulative offset (not used for the move)
 *   data          — TaskLibrary *
 * Output: none
 */
static void
on_handle_drag_end(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y,
                   gpointer data)
{
    (void)gesture; (void)offset_x; (void)offset_y;
    TaskLibrary *lw = data;
    if (!lw->drag_active)
        return;

    guint from   = lw->drag_from;
    gint  to_int = lw->drag_mark_pos;

    lw->drag_active   = FALSE;
    lw->drag_task_id  = 0;
    lw->drag_mark_pos = -1;
    lw->drag_mark_row = NULL;

    if (to_int < 0)
        goto clear_drag;
    guint to = (guint)to_int;
    guint n  = g_list_model_get_n_items(G_LIST_MODEL(lw->task_store));

    if (from >= n || to >= n || from == to)
        goto clear_drag;

    /* Move: remove the item at `from` and reinsert at `to`.               */
    TaskRow *row = TASK_ROW(g_list_model_get_item(G_LIST_MODEL(lw->task_store),
                                                   from));
    if (row == NULL)
        goto clear_drag;

    /* Splice the row out, then back in at the target.  Build a temp array
     * of ALL items in the new order to do it as one atomic replace.        */
    gpointer *new_items = g_new(gpointer, n);
    guint fill = 0;
    if (from < to) {
        for (guint i = 0; i < n; i++) {
            if (i == from) continue;
            new_items[fill++] = g_list_model_get_item(G_LIST_MODEL(lw->task_store), i);
            if (i == to)
                new_items[fill++] = g_object_ref(row);
        }
    } else {
        for (guint i = 0; i < n; i++) {
            if (i == to)
                new_items[fill++] = g_object_ref(row);
            if (i == from) continue;
            new_items[fill++] = g_list_model_get_item(G_LIST_MODEL(lw->task_store), i);
        }
    }
    g_object_unref(row);
    g_list_store_splice(lw->task_store, 0, n, new_items, fill);
    /* Release the extra refs taken by get_item above.                      */
    for (guint i = 0; i < fill; i++)
        g_object_unref(new_items[i]);
    g_free(new_items);

    task_view_save_manual_order(lw);

clear_drag:
    /* Clear the drag-state CSS on the former source row.                   */
    touch_row_at(lw, from);
}

/* --- Done column ----------------------------------------------------------
 * A GtkCheckButton: ticked = Done, unticked = In Progress.                 */

/*
 * on_done_setup — create the checkbox for the ✓ column.
 * Inputs: standard factory "setup" arguments + TaskLibrary *
 * Output: none
 */
static void
on_done_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    TaskLibrary *lw = data;
    GtkWidget *cb = gtk_check_button_new();
    gtk_widget_set_margin_top(cb, 8);
    gtk_widget_set_margin_bottom(cb, 8);
    gtk_widget_set_margin_start(cb, 4);
    gtk_widget_set_margin_end(cb, 4);
    /* Clicking the checkbox should not trigger row selection — that causes a
     * white→blue flash on unselected rows: GtkColumnView selects the row on
     * press, then the post-toggle refresh clears selection, producing a
     * one-frame blue flicker.  With selectable=FALSE the internal selection
     * gesture is skipped for this cell; the row is still selected (and the
     * CSS :selected state still applies here) when the user clicks any other
     * column's cell.                                                        */
    gtk_list_item_set_selectable(item, FALSE);
    gtk_list_item_set_child(item, cb);
    g_signal_connect(cb, "toggled", G_CALLBACK(on_done_toggled), lw);
}

/*
 * on_done_toggled — write the status change, then update the affected row.
 *
 * The row's GObject is updated IN PLACE and task_row_touch() is called.
 * task_row_touch() emits "changed" (re-running the bind function for every
 * column widget via row_factory_bind_again) and then calls
 * g_list_model_items_changed so a sorted model can re-sort if needed.
 *
 * Because the GObject identity does not change, GtkColumnView finds
 * item == old_item and skips its own internal unbind+rebind.  Only the
 * row_factory_bind_again trampolines run — no GTK CSS state is reset, so
 * the row's :hover background is preserved throughout the update.
 *
 * A full notify_changed is still triggered when the task must DISAPPEAR
 * (Done + show_completed off) — the row is going away anyway.
 *
 * Inputs: cb — the GtkCheckButton; data — TaskLibrary *
 * Output: none
 */
static void
on_done_toggled(GtkCheckButton *cb, gpointer data)
{
    TaskLibrary *lw   = data;
    GtkListItem *item = g_object_get_data(G_OBJECT(cb), "task-list-item");
    if (item == NULL)
        return;
    TaskRow *row = TASK_ROW(gtk_list_item_get_item(item));
    if (row == NULL || row->id == 0)
        return;

    gint64     task_id    = row->id;
    gboolean   was_done   = task_row_done(row);
    TaskStatus new_status = was_done ? TASK_STATUS_IN_PROGRESS
                                     : TASK_STATUS_DONE;

    task_db_task_set_status(lw->app->db, task_id, new_status);

    if (!was_done)
        task_app_status(lw->app,
                        "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 Completed",
                        *row->title != '\0' ? row->title : "Untitled Task");

    /* If the task should now be hidden (Done + show_completed=off), a full
     * refresh is needed to remove the row.                                  */
    gboolean show_done = task_app_config_get_bool("show_completed", TRUE);
    if (!show_done && new_status == TASK_STATUS_DONE) {
        task_app_notify_changed(lw->app);
        return;
    }

    /* Re-fetch to get the updated completed_at, then rebuild the markup.    */
    Task *t = task_db_task_get(lw->app->db, task_id);
    if (t == NULL) {
        task_app_notify_changed(lw->app);
        return;
    }

    const TaskView *sel_view = lib_sel_view(lw);
    gboolean virtual_view = (sel_view != NULL) ? sel_view->virtual_rows
                          : (lw->sel_kind == SB_KIND_GROUP);

    TaskRowCtx ctx;
    task_row_ctx_init(lw->app, &ctx, virtual_view);

    GPtrArray *subs = t->parent_id == 0
        ? g_hash_table_lookup(ctx.subs_by_parent, GINT_TO_POINTER(t->id))
        : NULL;
    const gchar *list_name = ctx.list_names != NULL
        ? g_hash_table_lookup(ctx.list_names, GINT_TO_POINTER(t->list_id))
        : NULL;
    gint att_count = GPOINTER_TO_INT(
        g_hash_table_lookup(ctx.att_counts, GINT_TO_POINTER(t->id)));

    /* Update existing row's fields in place.  Title and due do not change
     * on a status toggle.                                                    */
    row->status = t->status;
    g_free(row->status_text);
    row->status_text  = g_strdup(task_status_label(t->status));
    g_free(row->markup);
    row->markup       = task_rows_desc_markup(t, list_name, att_count,
                                              subs, &ctx);
    row->completed_at = t->completed_at;
    g_free(row->completed_text);
    row->completed_text = task_due_format(t->completed_at);

    task_row_ctx_clear(&ctx);
    task_free(t);

    /* Touch the row: emits "changed" (rebinds every column widget in place
     * via row_factory_bind_again) and tells the store so a sort model can
     * re-order when status is the active sort key.  GtkColumnView skips its
     * own unbind+rebind because the GObject identity is unchanged — hover
     * and selection survive.                                                 */
    task_row_touch(lw->task_store, row);

    /* Notify editors (a completed subtask may have started its parent).    */
    task_editor_refresh_all(lw->app);
}

/*
 * on_done_bind — sync the checkbox with the row's done state.
 * Inputs: standard factory "bind" arguments + TaskLibrary *
 * Output: none
 */
static void
on_done_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    TaskLibrary *lw  = data;
    GtkWidget   *cb  = gtk_list_item_get_child(item);
    TaskRow     *row = TASK_ROW(gtk_list_item_get_item(item));

    /* Store the list item on the checkbox so on_done_toggled can reach it. */
    g_object_set_data(G_OBJECT(cb), "task-list-item", item);

    /* Block the toggled signal while we set the state programmatically, so
     * a bind does not fire a write.                                         */
    g_signal_handlers_block_by_func(cb, on_done_toggled, lw);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb),
                                row != NULL && task_row_done(row));
    g_signal_handlers_unblock_by_func(cb, on_done_toggled, lw);
}

/* --- Task description column ----------------------------------------------
 * A GtkLabel showing the tall multi-line Pango markup.                     */

/*
 * on_task_activated — Enter or double-click opens the editor.
 *
 * Connected to the GtkColumnView "activate" signal; position indexes
 * task_sorted (the displayed order), not task_store.
 *
 * Inputs: view — the GtkColumnView; position — display index; data — lw
 * Output: none
 */
static void
on_task_activated(GtkColumnView *view, guint position, gpointer data)
{
    (void)view;
    TaskLibrary *lw = data;
    GListModel  *model = G_LIST_MODEL(lw->task_sorted);
    TaskRow     *row   = TASK_ROW(g_list_model_get_item(model, position));
    if (row == NULL)
        return;
    if (row->id != 0)
        task_editor_open(lw->app, row->id);
    g_object_unref(row);
}

/* --- Status column --------------------------------------------------------
 * A GtkLabel showing "New" / "In Progress" / "Done".                       */

/*
 * on_status_setup — create the label for the Status column.
 * Inputs: standard factory "setup" + unused
 * Output: none
 */
static void
on_status_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_list_item_set_child(item, label);
    task_app_select_on_press(label, item);
}

/*
 * on_status_bind — fill the Status label.
 * Inputs: standard factory "bind" + unused
 * Output: none
 */
static void
on_status_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_list_item_get_child(item);
    TaskRow   *row   = TASK_ROW(gtk_list_item_get_item(item));
    gtk_label_set_text(GTK_LABEL(label),
                       row != NULL && row->status_text != NULL
                           ? row->status_text : "");
}

/* --- Due Date column ------------------------------------------------------
 * A GtkLabel with urgency tints via CSS classes.                           */

/*
 * on_due_setup — create the label for the Due Date column.
 * Inputs: standard factory "setup" + unused
 * Output: none
 */
static void
on_due_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_list_item_set_child(item, label);
    task_app_select_on_press(label, item);
}

/* DUE_CLASSES[i] — the urgency class for tint index i (task_due_color
 * returns a colour string we match against the canonical palette).          */
static const gchar *const DUE_CLASSES[] = {
    "task-overdue",       /* #c01c28 */
    "task-due-today",     /* #d19a00 */
    "task-due-ahead",     /* #26a269 */
};

/*
 * on_due_bind — fill the Due Date label and apply the urgency CSS class.
 *
 * task_due_color returns one of three colour strings, or NULL for no tint.
 * Map each colour to its CSS class; clear all three first so a recycled
 * widget never keeps a stale class.
 *
 * Inputs: standard factory "bind" + unused
 * Output: none
 */
static void
on_due_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget  *label = gtk_list_item_get_child(item);
    TaskRow    *row   = TASK_ROW(gtk_list_item_get_item(item));

    /* Clear all urgency classes first.                                     */
    for (gsize i = 0; i < G_N_ELEMENTS(DUE_CLASSES); i++)
        gtk_widget_remove_css_class(label, DUE_CLASSES[i]);

    const gchar *text  = "";
    gint64       due_instant = 0;
    if (row != NULL) {
        if (row->due_text != NULL)
            text = row->due_text;
        due_instant = row->due_instant;
    }
    gtk_label_set_text(GTK_LABEL(label), text);

    const gchar *color = task_due_color(due_instant);
    if (color == NULL)
        return;
    /* Match the canonical palette strings to classes.                      */
    if (strcmp(color, "#c01c28") == 0)
        gtk_widget_add_css_class(label, DUE_CLASSES[0]);
    else if (strcmp(color, "#d19a00") == 0)
        gtk_widget_add_css_class(label, DUE_CLASSES[1]);
    else if (strcmp(color, "#26a269") == 0)
        gtk_widget_add_css_class(label, DUE_CLASSES[2]);
}

/* --- Completed column -----------------------------------------------------
 * A GtkLabel showing the completion date.                                  */

/*
 * on_completed_setup — create the label for the Completed column.
 * Inputs: standard factory "setup" + unused
 * Output: none
 */
static void
on_completed_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_margin_end(label, 4);
    gtk_list_item_set_child(item, label);
    task_app_select_on_press(label, item);
}

/*
 * on_completed_bind — fill the Completed label.
 * Inputs: standard factory "bind" + unused
 * Output: none
 */
static void
on_completed_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_list_item_get_child(item);
    TaskRow   *row   = TASK_ROW(gtk_list_item_get_item(item));
    gtk_label_set_text(GTK_LABEL(label),
                       row != NULL && row->completed_text != NULL
                           ? row->completed_text : "");
}

/* ---------------------------------------------------------------------------
 * Right-click context menu on the task list.
 * ------------------------------------------------------------------------- */

/*
 * on_task_right_click — show the context menu; select the clicked row if it
 * is not already part of the selection.
 *
 * Attached to each task description label's GtkGestureClick (SECONDARY,
 * CAPTURE phase).
 *
 * Inputs:
 *   gesture — the click gesture
 *   n, x, y — press count and position in the label's coordinates
 *   data    — TaskLibrary *
 * Output: none
 */
static void
on_task_right_click(GtkGestureClick *gesture, gint n, gdouble x, gdouble y,
                    gpointer data)
{
    (void)n;
    TaskLibrary *lw   = data;
    GtkWidget   *lbl  = gtk_event_controller_get_widget(
                            GTK_EVENT_CONTROLLER(gesture));
    GtkListItem *item = g_object_get_data(G_OBJECT(lbl), "task-rclick-item");
    if (item == NULL)
        return;

    guint pos = gtk_list_item_get_position(item);
    if (!gtk_selection_model_is_selected(
            GTK_SELECTION_MODEL(lw->task_sel), pos))
        gtk_selection_model_select_item(
            GTK_SELECTION_MODEL(lw->task_sel), pos, TRUE);

    /* Convert label coordinates to window (attachment widget) coordinates. */
    graphene_point_t lsrc = { (float)x, (float)y };
    graphene_point_t ldst;
    if (gtk_widget_compute_point(lbl, lw->window, &lsrc, &ldst))
        task_context_menu_popup(lw, lw->window, ldst.x, ldst.y);
}

/*
 * on_task_right_click_setup — attach a right-click gesture to a Task label.
 * Inputs: standard factory "setup" + TaskLibrary *
 * Output: none; called from on_task_setup via the existing setup chain
 *
 * NOTE: This is called from a separate factory "setup" callback for the
 * description column — the gesture is installed in on_task_setup_rclick
 * which on_task_setup delegates to.
 */

/*
 * on_task_rclick_setup — create the Task label with both select-on-press and
 * the right-click gesture.
 * Inputs: standard factory "setup" + TaskLibrary *
 * Output: none
 */
static void
on_task_rclick_setup(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    TaskLibrary *lw   = data;
    GtkWidget   *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_widget_set_margin_start(label, 4);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(item, label);
    task_app_select_on_press(label, item);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                   GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_task_right_click), lw);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
}

/*
 * on_task_rclick_bind — fill the Task label and store the list item for
 * right-click use.
 * Inputs: standard factory "bind" + TaskLibrary *
 * Output: none
 */
static void
on_task_rclick_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkWidget *label = gtk_list_item_get_child(item);
    TaskRow   *row   = TASK_ROW(gtk_list_item_get_item(item));
    /* Store item for the right-click handler.                              */
    g_object_set_data(G_OBJECT(label), "task-rclick-item", item);
    gtk_label_set_markup(GTK_LABEL(label),
                         row != NULL && row->markup != NULL ? row->markup : "");
}

/* ---------------------------------------------------------------------------
 * Column comparators — GCompareDataFunc for GtkCustomSorter.
 * a and b are TaskRow * (from the GListStore); data is unused.
 * ------------------------------------------------------------------------- */

/* cmp_title — sort by plain title, case-insensitive.                       */
static gint
cmp_title(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    const TaskRow *ra = a;
    const TaskRow *rb = b;
    gchar *fa = ra->title ? g_utf8_casefold(ra->title, -1) : g_strdup("");
    gchar *fb = rb->title ? g_utf8_casefold(rb->title, -1) : g_strdup("");
    gint cmp = strcmp(fa, fb);
    g_free(fa);
    g_free(fb);
    return cmp;
}

/* cmp_done — sort Done after non-Done.                                     */
static gint
cmp_done(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    gboolean da = task_row_done((const TaskRow *)a);
    gboolean db = task_row_done((const TaskRow *)b);
    return (da > db) - (da < db);
}

/* cmp_status — sort by status enum (New < In Progress < Done).             */
static gint
cmp_status(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    gint sa = (gint)((const TaskRow *)a)->status;
    gint sb = (gint)((const TaskRow *)b)->status;
    return (sa > sb) - (sa < sb);
}

/* cmp_due — soonest first; undated rows always last.                       */
static gint
cmp_due(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    gint64 da = ((const TaskRow *)a)->due_instant;
    gint64 db = ((const TaskRow *)b)->due_instant;
    if (da == 0) da = G_MAXINT64;
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* cmp_completed — oldest-completed first; incomplete rows last.            */
static gint
cmp_completed(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    gint64 da = ((const TaskRow *)a)->completed_at;
    gint64 db = ((const TaskRow *)b)->completed_at;
    if (da == 0) da = G_MAXINT64;
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ---------------------------------------------------------------------------
 * task_list_build() — build the task list, its columns and lw->task_scroll.
 * ------------------------------------------------------------------------- */

/*
 * col_new() — build a GtkColumnViewColumn, attach a sorter and add it to
 * the view.  The view holds a ref on the returned column.
 *
 * Inputs:
 *   cv     — the GtkColumnView to add the column to
 *   title  — column header title (may be NULL for the drag handle)
 *   f      — the factory [transfer full]: gtk_column_view_column_new()
 *             takes ownership, so do NOT unref f after calling col_new
 *   cmp    — the comparator for GtkCustomSorter (NULL for unsortable)
 *   expand — TRUE to make the column expand to fill available space
 * Output: the column (the view holds one ref; store it if you need it).
 */
static GtkColumnViewColumn *
col_new(GtkColumnView *cv, const gchar *title, GtkListItemFactory *f,
        GCompareDataFunc cmp, gboolean expand)
{
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
    if (cmp != NULL) {
        GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(cmp, NULL, NULL));
        gtk_column_view_column_set_sorter(col, sorter);
        g_object_unref(sorter);
    }
    gtk_column_view_column_set_resizable(col, title != NULL);
    gtk_column_view_column_set_expand(col, expand);
    gtk_column_view_append_column(cv, col);
    return col;                        /* view holds ref; caller stores it  */
}

/*
 * task_list_build — build the task column view, its store chain, all columns
 * and the scroll container.  Called once from task_library_window_new.
 *
 * Inputs: lw — the library window (fields filled in here)
 * Output: none (lw->task_scroll is ready to pack)
 */
void
task_list_build(TaskLibrary *lw)
{
    task_list_install_css();

    /* Store chain: GListStore → GtkSortListModel → GtkMultiSelection →
     * GtkColumnView.  The sort model's sorter is driven by the column view's
     * header click sorter; in manual-sort mode task_manual_sort_apply sets
     * it to NULL so the store order is used directly.                       */
    lw->task_store  = g_list_store_new(TASK_TYPE_ROW);
    lw->task_sorted = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(lw->task_store)), NULL);
    lw->task_sel    = gtk_multi_selection_new(
        G_LIST_MODEL(g_object_ref(lw->task_sorted)));

    GtkColumnView *cv = GTK_COLUMN_VIEW(
        gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(lw->task_sel))));
    lw->task_view = GTK_WIDGET(cv);
    gtk_widget_add_css_class(lw->task_view, "task-list");
    gtk_column_view_set_reorderable(cv, FALSE);

    /* Wire the column view's combined sorter into the sort model.           */
    gtk_sort_list_model_set_sorter(lw->task_sorted,
                                   gtk_column_view_get_sorter(cv));

    /* Activate (double-click or Enter) opens the editor.                    */
    g_signal_connect(cv, "activate", G_CALLBACK(on_task_activated), lw);

    /* Drag handle column — shown only in manual-sort mode.
     * The ⠿ glyph and its dimming are set once in setup; only drag-state
     * CSS classes change in bind.  Width is fixed at 26 px.               */
    /* gtk_column_view_column_new() is [transfer full] for the factory: the
     * column takes ownership of our reference.  Do NOT g_object_unref the
     * factory after passing it — that would be a double-free.               */
    lw->col_drag = col_new(cv, NULL,
        task_row_factory_new(G_CALLBACK(on_drag_handle_setup),
                             G_CALLBACK(on_drag_handle_bind), lw),
        NULL, FALSE);
    gtk_column_view_column_set_fixed_width(lw->col_drag, 26);

    /* Done (✓) column — a GtkCheckButton view of the status column.        */
    GtkColumnViewColumn *cdone = col_new(cv, "\xe2\x9c\x93",
        task_row_factory_new(G_CALLBACK(on_done_setup),
                             G_CALLBACK(on_done_bind), lw),
        cmp_done, FALSE);

    /* Task description column — the tall multi-line markup label with a
     * right-click gesture for the context menu.                            */
    GtkColumnViewColumn *cdesc = col_new(cv, "Task",
        task_row_factory_new(G_CALLBACK(on_task_rclick_setup),
                             G_CALLBACK(on_task_rclick_bind), lw),
        cmp_title, TRUE);

    /* Status column — "New" / "In Progress" / "Done", sorted by enum.     */
    GtkColumnViewColumn *cstatus = col_new(cv, "Status",
        task_row_factory_new(G_CALLBACK(on_status_setup),
                             G_CALLBACK(on_status_bind), lw),
        cmp_status, FALSE);

    /* Due Date column — urgency-tinted, soonest first, undated last.       */
    GtkColumnViewColumn *cdue = col_new(cv, "Due Date",
        task_row_factory_new(G_CALLBACK(on_due_setup),
                             G_CALLBACK(on_due_bind), lw),
        cmp_due, FALSE);

    /* Completed column — sortable, incomplete rows last.                   */
    GtkColumnViewColumn *ccompleted = col_new(cv, "Completed",
        task_row_factory_new(G_CALLBACK(on_completed_setup),
                             G_CALLBACK(on_completed_bind), lw),
        cmp_completed, FALSE);

    /* Tag columns on the view so install_actions and manual_sort_apply can
     * reach them without walking the column model every time.              */
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdone",      cdone);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdesc",      cdesc);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cstatus",    cstatus);
    g_object_set_data(G_OBJECT(lw->task_view), "task-cdue",       cdue);
    g_object_set_data(G_OBJECT(lw->task_view), "task-ccompleted", ccompleted);

    /* Collabels for the header menu.                                        */
    g_object_set_data(G_OBJECT(cdone),      "task-colkey",   (gpointer)"done");
    g_object_set_data(G_OBJECT(cdone),      "task-collabel", (gpointer)"Done");
    g_object_set_data(G_OBJECT(cstatus),    "task-colkey",   (gpointer)"status");
    g_object_set_data(G_OBJECT(cstatus),    "task-collabel", (gpointer)"Status");
    g_object_set_data(G_OBJECT(cdue),       "task-colkey",   (gpointer)"due");
    g_object_set_data(G_OBJECT(cdue),       "task-collabel", (gpointer)"Due Date");
    g_object_set_data(G_OBJECT(ccompleted), "task-colkey",   (gpointer)"completed");
    g_object_set_data(G_OBJECT(ccompleted), "task-collabel",
                       (gpointer)"Completion Date");

    /* Visibility actions and the shared header menu.                        */
    task_list_install_actions(lw);

    /* Manual sort: show/hide col_drag per persisted setting.               */
    task_manual_sort_apply(lw);

    lw->task_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->task_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lw->task_scroll),
                                  lw->task_view);
}
