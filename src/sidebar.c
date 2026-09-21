/* ===========================================================================
 * sidebar.c — the library window's sidebar: the virtual views, the Lists
 * section with its groups, the list dialogs and the sidebar's context menu
 * (see library_priv.h).
 *
 * GTK4 port: GtkTreeStore/GtkTreeView replaced by GListStore of TaskSbRow
 * objects under a GtkTreeListModel, shown in a GtkListView driven by a
 * GtkSingleSelection.  All blocking gtk_dialog_run() calls converted to
 * the async task_app_dialog_new() / task_app_confirm() pattern.  The
 * button-press-event right-click handler becomes a per-row GtkGestureClick
 * added in the factory setup callback.
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Context structs for async dialogs.
 * ------------------------------------------------------------------------- */

/* ListDialogCtx — shared by the New List and Edit List dialogs.             */
typedef struct {
    TaskLibrary *lw;
    GtkWidget   *name_entry;
    GtkWidget   *emoji_entry;
    gint64       edit_id;   /* 0 = create; non-zero = update existing list  */
} ListDialogCtx;

/* GroupDialogCtx — New Group dialog.                                        */
typedef struct {
    TaskLibrary *lw;
    GtkWidget   *entry;
} GroupDialogCtx;

/* RenameGroupCtx — Rename Group dialog.                                     */
typedef struct {
    TaskLibrary *lw;
    GtkWidget   *entry;
    gint64       group_id;
} RenameGroupCtx;

/* GroupDeleteCtx — Remove Group confirmation.                               */
typedef struct {
    TaskLibrary *lw;
    gint64       gid;
} GroupDeleteCtx;

/* ListDeleteCtx — Delete List confirmation.                                 */
typedef struct {
    TaskLibrary *lw;
    gint64       id;
    gchar       *name;
} ListDeleteCtx;

/* ---------------------------------------------------------------------------
 * Forward declarations.
 * ------------------------------------------------------------------------- */
static void on_edit_list(TaskLibrary *lw);
static void on_delete_list(TaskLibrary *lw);
static void on_new_group(TaskLibrary *lw);
static void group_delete_confirm(TaskLibrary *lw, gint64 gid);
static GArray *selected_list_ids(TaskLibrary *lw);

/* ---------------------------------------------------------------------------
 * View-visibility helpers.
 * ------------------------------------------------------------------------- */

/* view_visible() — whether a view's sidebar row should exist right now.     */
static gboolean
view_visible(TaskLibrary *lw, const TaskView *v)
{
    return v->visible == NULL || v->visible(lw->app, v->user_data);
}

/* lib_sidebar_show_pinned() — the Favorites row's visibility; the light
 * notify hook watches for a 0 ↔ nonzero transition.                         */
gboolean
lib_sidebar_show_pinned(TaskLibrary *lw)
{
    const TaskView *v = task_view_find("pinned");
    return v != NULL && view_visible(lw, v);
}

/* ---------------------------------------------------------------------------
 * CSS installation — called once per screen from task_sidebar_build().
 * ------------------------------------------------------------------------- */
static void
sb_css_install(void)
{
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;

    task_app_css_install(
        /* Sidebar list-view backdrop: shade of the window background, muted
         * text, and a blue selection bar.  `listview` is GTK4's CSS node
         * for GtkListView; `row` is each GtkListItem's widget.              */
        "listview.task-sidebar {"
        "  background-color: shade(@window_bg_color, " SB_BG_SHADE ");"
        "  color: rgb(65,65,65);"
        "}"
        "listview.task-sidebar row:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}"
        /* The top-padding strip painted in the same shade.                  */
        "box.task-sidebar-pad {"
        "  background-color: shade(@window_bg_color, " SB_BG_SHADE ");"
        "}");
}

/* ---------------------------------------------------------------------------
 * GtkTreeListModel child-model callback.
 * ------------------------------------------------------------------------- */

/* sb_create_children() — called by GtkTreeListModel to expand a row; returns
 * the TaskSbRow's own children store, or NULL for a leaf.                   */
static GListModel *
sb_create_children(gpointer item, gpointer data)
{
    (void)data;
    TaskSbRow *row = TASK_SB_ROW(item);
    if (row->children != NULL)
        return G_LIST_MODEL(g_object_ref(row->children));
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Factory helpers — walk the flat sb_tree to find/expand rows.
 * ------------------------------------------------------------------------- */

/* sb_set_row_expanded() — find the row (kind, id) in sb_tree and
 * set its expanded state.  Returns TRUE when the row was found.             */
static gboolean
sb_set_row_expanded(TaskLibrary *lw, gint kind, gint64 id, gboolean expand)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *trow =
            g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
        TaskSbRow *srow = TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
        if (srow->kind == kind && srow->id == id) {
            gtk_tree_list_row_set_expanded(trow, expand);
            g_object_unref(trow);
            return TRUE;
        }
        g_object_unref(trow);
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * GtkSignalListItemFactory callbacks.
 * ------------------------------------------------------------------------- */

/* on_sb_row_pressed() — right-click handler attached in on_sb_setup();
 * selects the pressed row (if not a header) and pops the context menu.
 * Forward-declared here; definition follows on_sb_bind().                   */
static void on_sb_row_pressed(GtkGestureClick *gesture, gint n_press,
                               gdouble x, gdouble y, gpointer data);

/* on_sb_setup() — "setup" signal: build the per-row widget tree and attach
 * the right-click gesture.  Called once per list item widget.               */
static void
on_sb_setup(GtkListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory;
    TaskLibrary *lw = data;

    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *label    = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
    gtk_list_item_set_child(item, expander);

    /* Right-click gesture: capture phase so it fires before GTK's selection
     * gesture, giving us a chance to select the row first.                  */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    /* Store a reference to the GtkListItem so the pressed handler can read
     * the item's current row.  The item owns this gesture, so the lifecycle
     * is fine (item lives at least as long as the gesture).                 */
    g_object_set_data(G_OBJECT(click), "task-sb-item", item);
    g_signal_connect(click, "pressed", G_CALLBACK(on_sb_row_pressed), lw);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(click));
}

/* on_sb_bind() — "bind" signal: update the row widget from the current item.
 * Called each time a GtkListItem is recycled for a new data row.            */
static void
on_sb_bind(GtkListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; (void)data;
    GObject        *obj      = G_OBJECT(gtk_list_item_get_item(item));
    GtkTreeListRow *tree_row = GTK_TREE_LIST_ROW(obj);
    TaskSbRow      *row      = TASK_SB_ROW(gtk_tree_list_row_get_item(tree_row));

    GtkWidget *expander = gtk_list_item_get_child(item);
    GtkWidget *label    = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));

    /* The expander must know its tree row to draw the toggle arrow.         */
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), tree_row);

    /* Bold for meta views, the Lists header and group headers.              */
    if (row->bold) {
        gchar *markup = g_markup_printf_escaped("<b>%s</b>", row->label);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), row->label);
    }

    /* The Lists header row is non-selectable: clicking it just
     * expands/collapses, never changes sel_kind/sel_id.                     */
    gtk_list_item_set_selectable(item, row->kind != SB_KIND_HEADER);
}

/* ---------------------------------------------------------------------------
 * Right-click context menu.
 * ------------------------------------------------------------------------- */

/* on_sb_row_pressed() — secondary-button press on a sidebar row.  Selects
 * the row (unless it is the header) and pops the appropriate context menu.  */
static void
on_sb_row_pressed(GtkGestureClick *gesture, gint n_press,
                  gdouble x, gdouble y, gpointer data)
{
    (void)n_press;
    TaskLibrary *lw   = data;
    GtkListItem *item = g_object_get_data(G_OBJECT(gesture), "task-sb-item");

    /* item may have been recycled and now holds a different row, but the
     * gesture is always on the widget that was pressed, so the item's
     * current position is the one under the pointer.                        */
    GObject        *obj      = G_OBJECT(gtk_list_item_get_item(item));
    if (obj == NULL) return;
    GtkTreeListRow *tree_row = GTK_TREE_LIST_ROW(obj);
    TaskSbRow      *row      = TASK_SB_ROW(gtk_tree_list_row_get_item(tree_row));

    gint   kind = row->kind;
    gint64 id   = row->id;

    /* Select the pressed row (unless it is the non-selectable header).     */
    if (kind != SB_KIND_HEADER) {
        guint pos = gtk_list_item_get_position(item);
        if (gtk_single_selection_get_selected(lw->sb_sel) != pos)
            gtk_single_selection_set_selected(lw->sb_sel, pos);
    }

    GtkWidget *widget =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();
    g_menu_append(section, "New List",  "win.new-list");
    g_menu_append(section, "New Group", "win.new-group");
    task_app_menu_section_end(menu, &section);

    if (kind == SB_KIND_LIST) {
        g_menu_append(section, "Edit List",   "win.edit-list");
        g_menu_append(section, "Delete List", "win.delete-list");
        task_app_menu_section_end(menu, &section);

        /* Move to Group / Remove from Group items.                          */
        GArray *ids = selected_list_ids(lw);
        gboolean any_grouped = FALSE;
        for (guint i = 0; i < ids->len; i++) {
            TaskList *l = task_db_list_get(lw->app->db,
                                           g_array_index(ids, gint64, i));
            if (l != NULL) {
                if (l->group_id != 0) any_grouped = TRUE;
                task_list_free(l);
            }
        }
        g_array_unref(ids);

        GPtrArray *groups = task_db_groups(lw->app->db);
        if (groups->len > 0) {
            GMenu *sub = g_menu_new();
            for (guint gi = 0; gi < groups->len; gi++) {
                TaskGroup *g  = g_ptr_array_index(groups, gi);
                GMenuItem *mi = g_menu_item_new(g->name, NULL);
                g_menu_item_set_action_and_target(mi, "win.move-to-group",
                                                  "x", g->id);
                g_menu_append_item(sub, mi);
                g_object_unref(mi);
            }
            g_menu_append_submenu(section, "Move to Group",
                                  G_MENU_MODEL(sub));
            g_object_unref(sub);
        }
        if (any_grouped)
            g_menu_append(section, "Remove from Group",
                          "win.remove-from-group");
        task_ptr_array_free_groups(groups);
        if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
            task_app_menu_section_end(menu, &section);
    } else if (kind == SB_KIND_GROUP) {
        GMenuItem *mi = g_menu_item_new("Rename Group", NULL);
        g_menu_item_set_action_and_target(mi, "win.rename-group", "x", id);
        g_menu_append_item(section, mi);
        g_object_unref(mi);
        mi = g_menu_item_new("Remove Group", NULL);
        g_menu_item_set_action_and_target(mi, "win.delete-group", "x", id);
        g_menu_append_item(section, mi);
        g_object_unref(mi);
        task_app_menu_section_end(menu, &section);
    }
    g_object_unref(section);

    task_app_menu_popup(widget, G_MENU_MODEL(menu), x, y);
}

/* ---------------------------------------------------------------------------
 * Sidebar selection helper.
 * ------------------------------------------------------------------------- */

/* selected_list_ids() — ids of selected LIST rows; a view, header or group
 * contributes nothing.  With GtkSingleSelection there is at most one.
 * Free with g_array_unref.                                                  */
static GArray *
selected_list_ids(TaskLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeListRow *trow = gtk_single_selection_get_selected_item(lw->sb_sel);
    if (trow == NULL) return ids;
    TaskSbRow *row = TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
    if (row->kind == SB_KIND_LIST)
        g_array_append_val(ids, row->id);
    return ids;
}

/* lists_set_group() — file the selected list(s) under group_id (0 =
 * ungrouped), then refresh.                                                  */
static void
lists_set_group(TaskLibrary *lw, gint64 group_id)
{
    GArray *ids = selected_list_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        task_db_list_set_group(lw->app->db,
                               g_array_index(ids, gint64, i), group_id);
    g_array_unref(ids);
    lib_full_refresh(lw);
}

/* ---------------------------------------------------------------------------
 * lib_refresh_sidebar() — rebuild the GListStore from the DB and restore
 * the previous expansion / selection state.
 * ------------------------------------------------------------------------- */
void
lib_refresh_sidebar(TaskLibrary *lw)
{
    lw->populating = TRUE;
    lib_scroll_keep_queue(lw->sb_view);

    /* --- Snapshot expansion state ----------------------------------------- */
    /* Walk the FLAT tree model to record which header / group rows are
     * currently expanded.  GtkTreeListModel emits items-changed when a row
     * is expanded, so only currently-visible rows appear in the model.      */
    gboolean lists_expanded = TRUE;   /* default: expand on first population */
    if (lw->sb_populated) {
        lists_expanded = FALSE;
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
        for (guint i = 0; i < n; i++) {
            GtkTreeListRow *trow =
                g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
            TaskSbRow *srow =
                TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
            if (srow->kind == SB_KIND_HEADER) {
                lists_expanded = gtk_tree_list_row_get_expanded(trow);
            } else if (srow->kind == SB_KIND_GROUP) {
                gboolean exp = gtk_tree_list_row_get_expanded(trow);
                g_hash_table_insert(lw->group_expanded,
                    GINT_TO_POINTER((gint)srow->id),
                    GINT_TO_POINTER(exp ? 1 : 0));
            }
            g_object_unref(trow);
        }
    }

    /* --- Rebuild the model ------------------------------------------------ */
    g_list_store_remove_all(lw->sb_store);

    lw->pinned_row_shown = lib_sidebar_show_pinned(lw);

    /* Virtual views from the registry.                                      */
    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *v = task_view_nth(i);
        if (!view_visible(lw, v)) continue;
        TaskSbRow *r = task_sb_row_new(SB_KIND_VIEW, (gint64)i,
                                       v->label, TRUE, FALSE);
        g_list_store_append(lw->sb_store, r);
        g_object_unref(r);
    }

    /* The collapsible "Lists" header — it is expandable (has children).     */
    TaskSbRow *header = task_sb_row_new(SB_KIND_HEADER, 0, "Lists",
                                        TRUE, TRUE);
    g_list_store_append(lw->sb_store, header);

    GPtrArray *groups = task_db_groups(lw->app->db);
    GPtrArray *lists  = task_db_lists(lw->app->db, FALSE);

    /* Find the group of the currently selected list (needed to force-expand
     * the group that contains the selection).                               */
    gint64 sel_list_group_id = 0;
    if (lw->sel_kind == SB_KIND_LIST && lw->sel_id != 0) {
        TaskList *sl = task_db_list_get(lw->app->db, lw->sel_id);
        if (sl != NULL) {
            sel_list_group_id = sl->group_id;
            task_list_free(sl);
        }
    }

    /* First pass: groups and their lists, into the header's children store. */
    for (guint gi = 0; gi < groups->len; gi++) {
        TaskGroup *grp = g_ptr_array_index(groups, gi);
        TaskSbRow *grp_row = task_sb_row_new(SB_KIND_GROUP, grp->id,
                                              grp->name, TRUE, TRUE);
        g_list_store_append(header->children, grp_row);

        for (guint li = 0; li < lists->len; li++) {
            TaskList *l = g_ptr_array_index(lists, li);
            if (l->group_id != grp->id) continue;
            gchar *label = lib_list_label(l);
            TaskSbRow *lr = task_sb_row_new(SB_KIND_LIST, l->id,
                                             label, FALSE, FALSE);
            g_list_store_append(grp_row->children, lr);
            g_object_unref(lr);
            g_free(label);
        }
        g_object_unref(grp_row);
    }

    /* Second pass: ungrouped lists directly under the header.               */
    for (guint li = 0; li < lists->len; li++) {
        TaskList *l = g_ptr_array_index(lists, li);
        if (l->group_id != 0) continue;
        gchar *label = lib_list_label(l);
        TaskSbRow *lr = task_sb_row_new(SB_KIND_LIST, l->id,
                                         label, FALSE, FALSE);
        g_list_store_append(header->children, lr);
        g_object_unref(lr);
        g_free(label);
    }

    task_ptr_array_free_groups(groups);
    task_ptr_array_free_lists(lists);
    g_object_unref(header);

    /* --- Restore expansion state ------------------------------------------ */
    /* Expand the Lists section header when it was open, or when the
     * selection lives inside (a visible selection is required).             */
    gboolean sel_in_lists = (lw->sel_kind == SB_KIND_LIST ||
                             lw->sel_kind == SB_KIND_GROUP);
    if (lists_expanded || sel_in_lists)
        sb_set_row_expanded(lw, SB_KIND_HEADER, 0, TRUE);

    /* Now that the header is expanded its children are in sb_tree.
     * Walk once more to expand/collapse groups according to the snapshot.  */
    {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
        for (guint i = 0; i < n; i++) {
            GtkTreeListRow *trow =
                g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
            TaskSbRow *srow =
                TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
            if (srow->kind == SB_KIND_GROUP) {
                gpointer snap = g_hash_table_lookup(lw->group_expanded,
                    GINT_TO_POINTER((gint)srow->id));
                /* Default: expanded on first population; use snapshot after. */
                gboolean was_expanded = (snap == NULL) ? TRUE
                                      : GPOINTER_TO_INT(snap) != 0;
                gboolean force_open = (sel_list_group_id == srow->id ||
                                       (lw->sel_kind == SB_KIND_GROUP &&
                                        lw->sel_id == srow->id));
                if (was_expanded || force_open)
                    gtk_tree_list_row_set_expanded(trow, TRUE);
            }
            g_object_unref(trow);
        }
    }

    /* --- Restore selection ------------------------------------------------ */
    guint sel_pos       = GTK_INVALID_LIST_POSITION;
    guint first_list    = GTK_INVALID_LIST_POSITION;
    gint64 first_list_id = 0;

    {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
        for (guint i = 0; i < n; i++) {
            GtkTreeListRow *trow =
                g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
            TaskSbRow *srow =
                TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
            if (srow->kind == SB_KIND_LIST &&
                first_list == GTK_INVALID_LIST_POSITION) {
                first_list    = i;
                first_list_id = srow->id;
            }
            if (srow->kind == lw->sel_kind && srow->id == lw->sel_id)
                sel_pos = i;
            g_object_unref(trow);
        }
    }

    /* If the previously selected kind/id was a virtual view that vanished,
     * scan again for any matching view row (kind only, as a last resort).   */
    if (sel_pos == GTK_INVALID_LIST_POSITION &&
        lw->sel_kind != SB_KIND_LIST && lw->sel_kind != SB_KIND_GROUP) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
        for (guint i = 0; i < n; i++) {
            GtkTreeListRow *trow =
                g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
            TaskSbRow *srow =
                TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
            if (srow->kind == lw->sel_kind) {
                sel_pos = i;
                lw->sel_id = srow->id;
                g_object_unref(trow);
                break;
            }
            g_object_unref(trow);
        }
    }

    /* Fall back to the first real list.                                     */
    if (sel_pos == GTK_INVALID_LIST_POSITION &&
        first_list != GTK_INVALID_LIST_POSITION) {
        sel_pos = first_list;
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id   = first_list_id;
    }

    /* Ultimate fallback: the first selectable row (the first virtual view). */
    if (sel_pos == GTK_INVALID_LIST_POSITION) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->sb_tree));
        for (guint i = 0; i < n; i++) {
            GtkTreeListRow *trow =
                g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), i);
            TaskSbRow *srow =
                TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
            if (srow->kind != SB_KIND_HEADER) {
                sel_pos = i;
                lw->sel_kind = srow->kind;
                lw->sel_id   = srow->id;
                g_object_unref(trow);
                break;
            }
            g_object_unref(trow);
        }
    }

    if (sel_pos != GTK_INVALID_LIST_POSITION)
        gtk_single_selection_set_selected(lw->sb_sel, sel_pos);

    lw->sb_populated = TRUE;
    lw->populating   = FALSE;
}

/* ---------------------------------------------------------------------------
 * Selection-changed handler.
 * ------------------------------------------------------------------------- */

/* on_sidebar_changed() — "notify::selected-item" on sb_sel: update
 * sel_kind/sel_id and refresh the task pane.  Header rows cannot be
 * selected (gtk_list_item_set_selectable returns FALSE for them in bind).   */
static void
on_sidebar_changed(GtkSingleSelection *sel, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    TaskLibrary *lw = data;
    if (lw->populating) return;

    GtkTreeListRow *trow = gtk_single_selection_get_selected_item(sel);
    if (trow == NULL) return;
    TaskSbRow *row = TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
    /* Header is non-selectable; this handler should never fire for one, but
     * guard anyway.                                                          */
    if (row->kind == SB_KIND_HEADER) return;

    lw->sel_kind = row->kind;
    lw->sel_id   = row->id;

    /* A new view starts with the Done lane un-expanded.                     */
    lw->board.done_show_all = FALSE;
    lib_refresh_tasks(lw);
}

/* lib_selected_list_id() — the currently selected REAL list, or 0.         */
gint64
lib_selected_list_id(TaskLibrary *lw)
{
    return lw->sel_kind == SB_KIND_LIST ? lw->sel_id : 0;
}

/* ---------------------------------------------------------------------------
 * Sidebar-pane show/hide.
 * ------------------------------------------------------------------------- */

/* lib_sidebar_ui_sync() — point the toolbar button's tooltip and the View
 * menu pair at the pane's LIVE visibility.  The button's icon is symmetric
 * and unchanged by state; only the words (tooltip, menu label) move.       */
void
lib_sidebar_ui_sync(TaskLibrary *lw)
{
    gboolean shown = gtk_widget_get_visible(lw->sidebar_box);
    if (lw->sidebar_item != NULL)
        task_app_set_tooltip(lw->sidebar_item,
            shown ? "Hide the lists pane" : "Show the lists pane");
    lib_menu_pair_sync(lw, "sidebar-hide", "sidebar-show", shown);
}

/* sidebar_set_visible() — show or hide, persist the choice, sync the UI.   */
static void
sidebar_set_visible(TaskLibrary *lw, gboolean show)
{
    gtk_widget_set_visible(lw->sidebar_box, show);
    task_app_config_set("sidebar_visible", show ? "1" : "0");
    lib_sidebar_ui_sync(lw);
}

/* lib_on_toggle_sidebar() — toolbar button: flip the lists pane.           */
void
lib_on_toggle_sidebar(TaskLibrary *lw)
{
    sidebar_set_visible(lw, !gtk_widget_get_visible(lw->sidebar_box));
}

/* ---------------------------------------------------------------------------
 * Emoji entry helper (for the list dialog).
 * In GTK4 popovers can escape their toplevel, so no window-resize trick is
 * needed.  A click on the emoji entry opens the GTK emoji chooser.
 * ------------------------------------------------------------------------- */

/* on_emoji_entry_pressed() — primary click on the emoji entry: clear any
 * previous pick and open the chooser.                                       */
static void
on_emoji_entry_pressed(GtkGestureClick *gesture, gint n_press,
                       gdouble x, gdouble y, gpointer entry)
{
    (void)gesture; (void)n_press; (void)x; (void)y;
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    g_signal_emit_by_name(entry, "insert-emoji");
}

/* ---------------------------------------------------------------------------
 * List dialog (New List / Edit List) — async via task_app_dialog_new().
 * ------------------------------------------------------------------------- */

/* on_list_dialog_done() — called when the user accepts or cancels.         */
static void
on_list_dialog_done(gboolean accepted, GtkWindow *dialog, gpointer data)
{
    (void)dialog;
    ListDialogCtx *ctx = data;
    TaskLibrary   *lw  = ctx->lw;

    if (accepted) {
        gchar *name  = g_strstrip(g_strdup(
            gtk_editable_get_text(GTK_EDITABLE(ctx->name_entry))));
        gchar *emoji = g_strstrip(g_strdup(
            gtk_editable_get_text(GTK_EDITABLE(ctx->emoji_entry))));

        if (*name != '\0') {
            if (ctx->edit_id == 0) {
                /* New list. */
                gint64 id = task_db_list_create(lw->app->db, name, emoji);
                if (id == 0) {
                    task_app_status(lw->app,
                        "Could not create the list \xe2\x80\x94 "
                        "database write failed");
                } else {
                    lw->sel_kind = SB_KIND_LIST;
                    lw->sel_id   = id;
                    lib_full_refresh(lw);
                    task_app_status(lw->app,
                        "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name);
                }
            } else {
                /* Edit existing list. */
                task_db_list_update(lw->app->db, ctx->edit_id, name, emoji);
                lib_full_refresh(lw);
                task_app_status(lw->app,
                    "Updated list \xe2\x80\x9c%s\xe2\x80\x9d", name);
            }
        }
        g_free(name);
        g_free(emoji);
    }
    g_free(ctx);
}

/* open_list_dialog() — build and show the list dialog.
 *   edit_id       — 0 for a new list, or the list id being edited.
 *   initial_name  — NULL or the current name.
 *   initial_emoji — NULL or the current emoji.                              */
static void
open_list_dialog(TaskLibrary *lw, const gchar *title, gint64 edit_id,
                 const gchar *initial_name, const gchar *initial_emoji)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    /* Emoji row. */
    GtkWidget *emoji_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *emoji_lbl = gtk_label_new("List Emoji:");
    gtk_label_set_xalign(GTK_LABEL(emoji_lbl), 0.0f);
    gtk_box_append(GTK_BOX(emoji_row), emoji_lbl);
    GtkWidget *emoji_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(emoji_entry), 4);
    gtk_editable_set_width_chars(GTK_EDITABLE(emoji_entry), 2);
    gtk_entry_set_alignment(GTK_ENTRY(emoji_entry), 0.5f);
    gtk_widget_set_halign(emoji_entry, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(emoji_entry,
        "Optional emoji \xe2\x80\x94 click to pick");
    if (initial_emoji != NULL)
        gtk_editable_set_text(GTK_EDITABLE(emoji_entry), initial_emoji);
    /* Primary-button click opens the emoji chooser. */
    GtkGesture *emoji_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(emoji_click),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(emoji_click, "pressed",
                     G_CALLBACK(on_emoji_entry_pressed), emoji_entry);
    gtk_widget_add_controller(emoji_entry,
                              GTK_EVENT_CONTROLLER(emoji_click));
    gtk_box_append(GTK_BOX(emoji_row), emoji_entry);
    gtk_box_append(GTK_BOX(box), emoji_row);

    /* Name row. */
    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name_lbl = gtk_label_new("List name:");
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
    gtk_box_append(GTK_BOX(name_row), name_lbl);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(name_entry), 28);
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    if (initial_name != NULL)
        gtk_editable_set_text(GTK_EDITABLE(name_entry), initial_name);
    gtk_widget_set_hexpand(name_entry, TRUE);
    gtk_box_append(GTK_BOX(name_row), name_entry);
    gtk_box_append(GTK_BOX(box), name_row);

    ListDialogCtx *ctx  = g_new0(ListDialogCtx, 1);
    ctx->lw             = lw;
    ctx->name_entry     = name_entry;
    ctx->emoji_entry    = emoji_entry;
    ctx->edit_id        = edit_id;

    GtkWindow *dlg = task_app_dialog_new(GTK_WINDOW(lw->window), title,
                                          box, "_OK",
                                          on_list_dialog_done, ctx);
    gtk_widget_grab_focus(name_entry);
    (void)dlg;
}

/* ---------------------------------------------------------------------------
 * Group name dialog — shared by New Group and Rename Group.
 * ------------------------------------------------------------------------- */

/* on_new_group_done() — New Group dialog accepted/cancelled.                */
static void
on_new_group_done(gboolean accepted, GtkWindow *dialog, gpointer data)
{
    (void)dialog;
    GroupDialogCtx *ctx = data;
    if (accepted) {
        const gchar *name =
            gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
        if (name && *name) {
            gint64 gid = task_db_group_create(ctx->lw->app->db, name);
            if (gid == 0)
                task_app_status(ctx->lw->app, "Failed to create group");
            else
                lib_full_refresh(ctx->lw);
        }
    }
    g_free(ctx);
}

/* on_new_group() — win.new-group: prompt for a name and create the group.  */
static void
on_new_group(TaskLibrary *lw)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Group name:"));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Group name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    GroupDialogCtx *ctx = g_new0(GroupDialogCtx, 1);
    ctx->lw    = lw;
    ctx->entry = entry;

    task_app_dialog_new(GTK_WINDOW(lw->window), "New Group", box,
                        "Create", on_new_group_done, ctx);
}

/* on_rename_group_done() — Rename Group dialog accepted/cancelled.          */
static void
on_rename_group_done(gboolean accepted, GtkWindow *dialog, gpointer data)
{
    (void)dialog;
    RenameGroupCtx *ctx = data;
    if (accepted) {
        const gchar *name =
            gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
        if (name && *name) {
            task_db_group_rename(ctx->lw->app->db, ctx->group_id, name);
            lib_full_refresh(ctx->lw);
        }
    }
    g_free(ctx);
}

/* on_rename_group() — win.rename-group(x): open the rename dialog.         */
static void
on_rename_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw       = data;
    gint64       group_id = g_variant_get_int64(param);
    TaskGroup   *grp      = task_db_group_get(lw->app->db, group_id);
    const gchar *current  = (grp != NULL) ? grp->name : "";

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Group name:"));
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), current);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    task_group_free(grp);

    RenameGroupCtx *ctx = g_new0(RenameGroupCtx, 1);
    ctx->lw       = lw;
    ctx->entry    = entry;
    ctx->group_id = group_id;

    task_app_dialog_new(GTK_WINDOW(lw->window), "Rename Group", box,
                        "Rename", on_rename_group_done, ctx);
}

/* ---------------------------------------------------------------------------
 * Group deletion — async via task_app_confirm().
 * ------------------------------------------------------------------------- */

/* on_group_delete_confirmed() — confirm result for Remove Group.            */
static void
on_group_delete_confirmed(gboolean yes, gpointer data)
{
    GroupDeleteCtx *ctx = data;
    if (yes) {
        TaskLibrary *lw = ctx->lw;
        if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == ctx->gid) {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id   = 0;
        }
        task_db_group_delete(lw->app->db, ctx->gid);
        lib_row_order_keys_drop(SB_KIND_GROUP, ctx->gid);
        lib_full_refresh(lw);
    }
    g_free(ctx);
}

/* group_delete_confirm() — ask, then remove a group.  Its lists become
 * ungrouped; the saved aggregate orders go with it.                         */
static void
group_delete_confirm(TaskLibrary *lw, gint64 gid)
{
    GroupDeleteCtx *ctx = g_new0(GroupDeleteCtx, 1);
    ctx->lw  = lw;
    ctx->gid = gid;
    task_app_confirm(GTK_WINDOW(lw->window), "Remove Group",
                     "Remove this group? Its lists will become ungrouped.",
                     on_group_delete_confirmed, ctx);
}

/* on_delete_group() — win.delete-group(x): context menu's Remove Group.    */
static void
on_delete_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    group_delete_confirm(data, g_variant_get_int64(param));
}

/* ---------------------------------------------------------------------------
 * Group / list move actions.
 * ------------------------------------------------------------------------- */

/* on_move_to_group() — win.move-to-group(x).                               */
static void
on_move_to_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    lists_set_group(data, g_variant_get_int64(param));
}

/* on_remove_from_group() — win.remove-from-group.                          */
static void
on_remove_from_group(TaskLibrary *lw)
{
    lists_set_group(lw, 0);
}

/* ---------------------------------------------------------------------------
 * List dialog entry points.
 * ------------------------------------------------------------------------- */

/* lib_on_new_list() — win.new-list: open the New List dialog.              */
void
lib_on_new_list(TaskLibrary *lw)
{
    open_list_dialog(lw, "New List", 0, NULL, NULL);
}

/* on_edit_list() — win.edit-list: open the Edit List dialog for the
 * currently selected list.                                                  */
static void
on_edit_list(TaskLibrary *lw)
{
    if (lib_view_refuse(lw, "edit the list each item lives in"))
        return;
    gint64 id = lib_selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app, "Select a list to edit");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL) return;
    open_list_dialog(lw, "Edit List", id, l->name, l->emoji);
    task_list_free(l);
}

/* ---------------------------------------------------------------------------
 * List deletion — async via task_app_confirm().
 * ------------------------------------------------------------------------- */

/* on_list_delete_confirmed() — confirm result for Delete List.             */
static void
on_list_delete_confirmed(gboolean yes, gpointer data)
{
    ListDeleteCtx *ctx = data;
    if (yes) {
        TaskLibrary *lw = ctx->lw;
        task_db_list_delete(lw->app->db, ctx->id);
        lib_row_order_keys_drop(SB_KIND_LIST, ctx->id);
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id   = 0;
        lib_full_refresh(lw);
        task_app_status(lw->app,
            "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", ctx->name);
    }
    g_free(ctx->name);
    g_free(ctx);
}

/* on_delete_list() — win.delete-list: confirm + tombstone the selected list;
 * when a group is selected, delegates to group_delete_confirm().            */
static void
on_delete_list(TaskLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_GROUP) {
        group_delete_confirm(lw, lw->sel_id);
        return;
    }
    if (lib_view_refuse(lw,
            "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6"))
        return;
    gint64 id = lib_selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app, "Select a list to delete");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL) return;

    ListDeleteCtx *ctx = g_new0(ListDeleteCtx, 1);
    ctx->lw   = lw;
    ctx->id   = id;
    ctx->name = g_strdup(l->name);

    gchar *msg = g_strdup_printf(
        "Delete the list \xe2\x80\x9c%s\xe2\x80\x9d and all of its tasks?",
        l->name);
    task_list_free(l);
    task_app_confirm(GTK_WINDOW(lw->window), "Delete List", msg,
                     on_list_delete_confirmed, ctx);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * Row activation (double-click).
 * ------------------------------------------------------------------------- */

/* on_sidebar_activated() — "activate" on GtkListView: double-click on a
 * real list opens the Edit List dialog.  Meta rows, the header and group
 * rows do nothing (the GtkTreeExpander handles expand/collapse).           */
static void
on_sidebar_activated(GtkListView *view, guint pos, gpointer data)
{
    (void)view;
    TaskLibrary    *lw   = data;
    GtkTreeListRow *trow =
        g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), pos);
    if (trow == NULL) return;
    TaskSbRow *row = TASK_SB_ROW(gtk_tree_list_row_get_item(trow));
    if (row->kind == SB_KIND_LIST)
        on_edit_list(lw);
    g_object_unref(trow);
}

/* ---------------------------------------------------------------------------
 * task_sidebar_build() — build the sidebar pane and install it into `paned`
 * as the start child (see library_priv.h).
 * ------------------------------------------------------------------------- */
void
task_sidebar_build(TaskLibrary *lw, GtkWidget *paned)
{
    sb_css_install();

    /* GListStore of top-level TaskSbRows.  Each expandable row carries its
     * own children GListStore; GtkTreeListModel flattens the tree.          */
    lw->sb_store = g_list_store_new(TASK_TYPE_SB_ROW);
    lw->sb_tree  = gtk_tree_list_model_new(
        G_LIST_MODEL(g_object_ref(lw->sb_store)),
        FALSE,  /* passthrough=FALSE: items in sb_tree are GtkTreeListRows   */
        FALSE,  /* autoexpand=FALSE: we manage expansion in lib_refresh_sidebar */
        sb_create_children, NULL, NULL);

    lw->sb_sel = gtk_single_selection_new(
        G_LIST_MODEL(g_object_ref(lw->sb_tree)));
    gtk_single_selection_set_autoselect(lw->sb_sel, FALSE);
    gtk_single_selection_set_can_unselect(lw->sb_sel, FALSE);

    g_signal_connect(lw->sb_sel, "notify::selected-item",
                     G_CALLBACK(on_sidebar_changed), lw);

    GtkListItemFactory *factory = task_row_factory_new(
        G_CALLBACK(on_sb_setup), G_CALLBACK(on_sb_bind), lw);

    lw->sb_view = gtk_list_view_new(GTK_SELECTION_MODEL(lw->sb_sel),
                                    factory);
    gtk_widget_add_css_class(lw->sb_view, "task-sidebar");
    g_signal_connect(lw->sb_view, "activate",
                     G_CALLBACK(on_sidebar_activated), lw);

    GtkWidget *sb_scroll = gtk_scrolled_window_new();
    /* EXTERNAL horizontally: NEVER makes the scroller demand its child's
     * full width as its own minimum, which floors the divider; EXTERNAL
     * scrolls without showing a bar, letting the pane go narrower.         */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll),
                                   GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sb_scroll),
                                  lw->sb_view);

    /* Sidebar column: a fixed spacer strip at the top so the first row's
     * text aligns with the task list's column-header text.  Painted in the
     * sidebar shade so it reads as part of the pane.                       */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *sidebar_pad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_pad, -1, SB_TOP_PAD);
    gtk_widget_add_css_class(sidebar_pad, "task-sidebar-pad");
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_pad);
    gtk_widget_set_vexpand(sb_scroll, TRUE);
    gtk_box_append(GTK_BOX(sidebar_box), sb_scroll);

    /* shrink_start_child=TRUE: the pane may allocate less than the minimum
     * (same as old gtk_paned_pack1's shrink=TRUE).                         */
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_box);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);
    lw->sidebar_box = sidebar_box;
}

/* ---------------------------------------------------------------------------
 * task_sidebar_install_actions() — register all "win." actions the sidebar
 * uses (see library_priv.h).
 * ------------------------------------------------------------------------- */
static const LibCommand SIDEBAR_COMMANDS[] = {
    { "new-list",          lib_on_new_list       },
    { "new-group",         on_new_group          },
    { "edit-list",         on_edit_list          },
    { "delete-list",       on_delete_list        },
    { "remove-from-group", on_remove_from_group  },
    { "toggle-sidebar",    lib_on_toggle_sidebar },
};

void
task_sidebar_install_actions(TaskLibrary *lw)
{
    lib_win_commands_install(lw, SIDEBAR_COMMANDS,
                             G_N_ELEMENTS(SIDEBAR_COMMANDS));
    lib_win_action_add(lw, "move-to-group", G_VARIANT_TYPE_INT64,
                       G_CALLBACK(on_move_to_group));
    lib_win_action_add(lw, "rename-group",  G_VARIANT_TYPE_INT64,
                       G_CALLBACK(on_rename_group));
    lib_win_action_add(lw, "delete-group",  G_VARIANT_TYPE_INT64,
                       G_CALLBACK(on_delete_group));
}
