/* ===========================================================================
 * sidebar.c — the library window's sidebar: the virtual views, the Lists
 * section with its groups, the list dialogs and the sidebar's context menu
 * (see library_priv.h).
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include <stdlib.h>
#include <string.h>

/* view_visible() — whether a view's sidebar row should exist right now.   */
static gboolean
view_visible(TaskLibrary *lw, const TaskView *v)
{
    return v->visible == NULL || v->visible(lw->app, v->user_data);
}

/* lib_sidebar_show_pinned() — the Favorites row's visibility, which the
 * light notify hook watches for a 0 <-> nonzero transition.               */
gboolean
lib_sidebar_show_pinned(TaskLibrary *lw)
{
    const TaskView *v = task_view_find("pinned");
    return v != NULL && view_visible(lw, v);
}

void
lib_refresh_sidebar(TaskLibrary *lw)
{
    lw->populating = TRUE;
    lib_scroll_keep_queue(lw->sb_view);

    /* Snapshot the Lists section's expansion BEFORE the clear — every
     * model rebuild collapses it otherwise (Notes gotcha #14).
     * The first population expands it; after that the user's choice
     * is preserved.                                                        */
    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    gboolean lists_expanded = TRUE;
    GtkTreeIter iter;
    if (lw->sb_populated) {
        lists_expanded = FALSE;
        if (gtk_tree_model_get_iter_first(model, &iter)) {
            do {
                gint kind;
                gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
                if (kind == SB_KIND_HEADER) {
                    GtkTreePath *p = gtk_tree_model_get_path(model, &iter);
                    lists_expanded = gtk_tree_view_row_expanded(
                        GTK_TREE_VIEW(lw->sb_view), p);
                    gtk_tree_path_free(p);
                    /* Also snapshot group expansion states from children. */
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &iter)) {
                        do {
                            gint ck; gint64 cid;
                            gtk_tree_model_get(model, &child,
                                               SB_KIND, &ck, SB_ID, &cid, -1);
                            if (ck == SB_KIND_GROUP) {
                                GtkTreePath *gp =
                                    gtk_tree_model_get_path(model, &child);
                                gboolean exp = gtk_tree_view_row_expanded(
                                    GTK_TREE_VIEW(lw->sb_view), gp);
                                gtk_tree_path_free(gp);
                                g_hash_table_insert(lw->group_expanded,
                                    GINT_TO_POINTER(cid),
                                    GINT_TO_POINTER(exp ? 1 : 0));
                            }
                        } while (gtk_tree_model_iter_next(model, &child));
                    }
                    break;
                }
            } while (gtk_tree_model_iter_next(model, &iter));
        }
    }
    gtk_tree_store_clear(lw->sb_store);
    /* The virtual views, straight from the registry — each one decides
     * for itself whether it exists right now.  SB_ID carries the view's
     * registry INDEX so the selection handler can find it again.          */
    lw->pinned_row_shown = lib_sidebar_show_pinned(lw);
    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *v = task_view_nth(i);
        if (!view_visible(lw, v))
            continue;
        gtk_tree_store_append(lw->sb_store, &iter, NULL);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, SB_KIND_VIEW,
                           SB_ID, (gint64)i,
                           SB_LABEL, v->label,
                           SB_WEIGHT, PANGO_WEIGHT_BOLD,
                           -1);
    }
    GtkTreeIter header;              /* the collapsible "Lists" section     */
    gtk_tree_store_append(lw->sb_store, &header, NULL);
    gtk_tree_store_set(lw->sb_store, &header,
                       SB_KIND, SB_KIND_HEADER,
                       SB_ID, (gint64)0,
                       SB_LABEL, "Lists",
                       SB_WEIGHT, PANGO_WEIGHT_BOLD,
                       -1);

    GPtrArray *groups = task_db_groups(lw->app->db);
    GPtrArray *lists  = task_db_lists(lw->app->db, FALSE);
    GtkTreeIter selected;            /* the row to reselect                 */
    gboolean have_selected = FALSE;
    GtkTreeIter first_list;          /* fallback selection                  */
    gboolean have_first = FALSE;
    gboolean sel_in_group = FALSE;   /* selected list is inside a group     */

    /* First pass: groups and their lists.                                  */
    for (guint gi = 0; gi < groups->len; gi++) {
        TaskGroup *grp = g_ptr_array_index(groups, gi);
        gchar *glabel = g_strdup(grp->name);
        GtkTreeIter grp_iter;
        gtk_tree_store_append(lw->sb_store, &grp_iter, &header);
        gtk_tree_store_set(lw->sb_store, &grp_iter,
                           SB_KIND, SB_KIND_GROUP,
                           SB_ID, grp->id,
                           SB_LABEL, glabel,
                           SB_WEIGHT, PANGO_WEIGHT_BOLD,
                           -1);
        g_free(glabel);

        gboolean grp_has_selected = FALSE;
        for (guint li = 0; li < lists->len; li++) {
            TaskList *l = g_ptr_array_index(lists, li);
            if (l->group_id != grp->id) continue;
            gchar *label = lib_list_label(l);
            gtk_tree_store_append(lw->sb_store, &iter, &grp_iter);
            gtk_tree_store_set(lw->sb_store, &iter,
                               SB_KIND, SB_KIND_LIST,
                               SB_ID, l->id,
                               SB_LABEL, label,
                               SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                               -1);
            g_free(label);
            if (!have_first) { first_list = iter; have_first = TRUE; }
            if (lw->sel_kind == SB_KIND_LIST && lw->sel_id == l->id) {
                selected = iter;
                have_selected = TRUE;
                grp_has_selected = TRUE;
                sel_in_group = TRUE;
            }
        }
        if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == grp->id) {
            selected = grp_iter;
            have_selected = TRUE;
        }

        /* Expand the group: default TRUE on first population, then use the
         * snapshot; force open when the selected list lives inside.        */
        gpointer snap = g_hash_table_lookup(lw->group_expanded,
                                            GINT_TO_POINTER(grp->id));
        gboolean was_expanded = (snap == NULL) ? TRUE
                                               : GPOINTER_TO_INT(snap) != 0;
        if (was_expanded || grp_has_selected) {
            GtkTreePath *gp = gtk_tree_model_get_path(model, &grp_iter);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(lw->sb_view), gp, FALSE);
            gtk_tree_path_free(gp);
        }
    }

    /* Second pass: ungrouped lists directly under the header.              */
    for (guint li = 0; li < lists->len; li++) {
        TaskList *l = g_ptr_array_index(lists, li);
        if (l->group_id != 0) continue;
        gchar *label = lib_list_label(l);
        gtk_tree_store_append(lw->sb_store, &iter, &header);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, SB_KIND_LIST,
                           SB_ID, l->id,
                           SB_LABEL, label,
                           SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                           -1);
        g_free(label);
        if (!have_first) { first_list = iter; have_first = TRUE; }
        if (lw->sel_kind == SB_KIND_LIST && lw->sel_id == l->id) {
            selected = iter;
            have_selected = TRUE;
        }
    }
    task_ptr_array_free_groups(groups);
    task_ptr_array_free_lists(lists);

    /* Reselect: same list/group, or same meta row, or the first list.      */
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    if (!have_selected && lw->sel_kind != SB_KIND_LIST &&
        lw->sel_kind != SB_KIND_GROUP &&
        gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gint   kind;
            gint64 id;
            gtk_tree_model_get(model, &iter, SB_KIND, &kind, SB_ID, &id, -1);
            /* SB_ID matters as much as the kind now: every virtual view
             * shares SB_KIND_VIEW and is told apart by its registry index,
             * so matching on kind alone would reselect whichever view
             * happened to come first.                                      */
            if (kind == lw->sel_kind && id == lw->sel_id) {
                selected = iter;
                have_selected = TRUE;
                break;
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    if (!have_selected && have_first) {
        selected = first_list;
        have_selected = TRUE;
        lw->sel_kind = SB_KIND_LIST;
        gtk_tree_model_get(model, &first_list, SB_ID, &lw->sel_id, -1);
    }
    if (!have_selected &&            /* no lists at all: fall back to the   */
        gtk_tree_model_get_iter_first(model, &iter)) {
        selected = iter;             /* first meta row                      */
        have_selected = TRUE;
        gtk_tree_model_get(model, &iter, SB_KIND, &lw->sel_kind,
                           SB_ID, &lw->sel_id, -1);
    }

    /* Restore the Lists section expansion and force it open when the
     * selection lives inside (a selection must be visible).                */
    if (lists_expanded ||
        (have_selected && (lw->sel_kind == SB_KIND_LIST ||
                           lw->sel_kind == SB_KIND_GROUP))) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &header);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(lw->sb_view), p, FALSE);
        gtk_tree_path_free(p);
    }
    (void)sel_in_group;              /* groups expand themselves above      */
    if (have_selected) {
        gtk_tree_selection_select_iter(sel, &selected);
        GtkTreePath *sp = gtk_tree_model_get_path(model, &selected);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(lw->sb_view), sp, NULL, FALSE);
        gtk_tree_path_free(sp);
    }
    lw->sb_populated = TRUE;
    lw->populating = FALSE;
}

/* sb_row_selectable() — the "Lists" header row cannot be selected.         */
static gboolean
sb_row_selectable(GtkTreeSelection *sel, GtkTreeModel *model,
                  GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)selected; (void)data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return FALSE;
    gint kind;
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    return kind != SB_KIND_HEADER;
}

/* on_sidebar_changed() — selection drives the task pane.  With MULTIPLE
 * selection the cursor row (last pressed) drives sel_kind/sel_id.  A GROUP
 * row refreshes like any other: it shows its lists' tasks aggregated (see
 * lib_refresh_tasks).  Only the "Lists" header selects nothing, and
 * sb_row_selectable already refuses it.                                    */
static void
on_sidebar_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    TaskLibrary *lw = data;
    if (lw->populating)
        return;
    GtkTreePath *cursor = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(lw->sb_view), &cursor, NULL);
    if (cursor == NULL)
        return;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, cursor))
        gtk_tree_model_get(model, &iter,
                           SB_KIND, &lw->sel_kind,
                           SB_ID,   &lw->sel_id,
                           -1);
    gtk_tree_path_free(cursor);
    /* A new view starts with the Done lane capped again: an expansion
     * answers "show me more of THIS view", it is not a mode.              */
    lw->board.done_show_all = FALSE;
    lib_refresh_tasks(lw);
}

/* lib_selected_list_id() — the currently selected REAL list, or 0.             */
gint64
lib_selected_list_id(TaskLibrary *lw)
{
    return lw->sel_kind == SB_KIND_LIST ? lw->sel_id : 0;
}

/* lib_sidebar_ui_sync() — point ALL of the sidebar's controls at the state
 * the pane is in, from its LIVE visibility: the toolbar button's icon and
 * tooltip, and the View menu item's matching LABEL ("Hide Sidebar" while
 * the lists pane is up, "Show Sidebar" while it is not).
 *
 * The ICON is ONE face, left-and-right.png — a double-headed arrow, so it
 * says "this moves the pane in and out" without naming a direction.  It
 * is therefore set ONCE where the button is built and NOT swapped here:
 * the glyph is symmetric about its vertical axis, so mirroring it by
 * state would change nothing a user could see, and turning it would only
 * point it at the wrong axis.
 *
 * That makes this the one toggle on the bar whose icon does not name the
 * ACTION, unlike the completed and sort toggles beside it — the tooltip
 * and the menu label are what say which way a click goes, which is why
 * this function still runs on every change.
 *
 * No handler blocking is needed for any of them: an action item's label
 * carries no state to feed back, and neither set_label nor swapping an
 * icon widget can emit "activate" (same protocol as
 * hide_done_icon_refresh and manual_sort_icon_refresh).                    */
void
lib_sidebar_ui_sync(TaskLibrary *lw)
{
    gboolean shown = gtk_widget_get_visible(lw->sidebar_box);

    if (lw->sidebar_item != NULL)
        gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->sidebar_item),
            shown ? "Hide the lists pane" : "Show the lists pane");

    /* The menu twin is a hidden-when PAIR: offer the one that names what a
     * click will do (see library_priv.h).                                 */
    lib_menu_pair_sync(lw, "sidebar-hide", "sidebar-show", shown);
}

/* sidebar_set_visible() — show or hide the lists pane, persist the
 * choice in `sidebar_visible` and point both of its controls at what a
 * click now offers.  Both the toolbar button and the menu item route
 * through here.                                                           */
static void
sidebar_set_visible(TaskLibrary *lw, gboolean show)
{
    gtk_widget_set_visible(lw->sidebar_box, show);
    task_app_config_set("sidebar_visible", show ? "1" : "0");
    lib_sidebar_ui_sync(lw);
}

/* lib_on_toggle_sidebar() — toolbar show/hide button for the lists pane:
 * the task view takes the whole window while it is hidden (mirrors the
 * Notes "Folders" toggle).                                                */
void
lib_on_toggle_sidebar(TaskLibrary *lw)
{
    sidebar_set_visible(lw, !gtk_widget_get_visible(lw->sidebar_box));
}

/* on_emoji_chooser_closed() — picker dismissed: shrink the dialog back
 * to its natural size (see on_emoji_box_pressed).                          */
static void
on_emoji_chooser_closed(GtkPopover *chooser, gpointer dlg)
{
    (void)chooser;
    gtk_window_resize(GTK_WINDOW(dlg), 1, 1);
}

/* emoji_open_idle() — open the chooser AFTER the dialog's grow-resize
 * has landed, so the popover measures against the enlarged window.         */
static gboolean
emoji_open_idle(gpointer entry)
{
    g_signal_emit_by_name(entry, "insert-emoji");

    /* GtkEntry keeps its chooser as "gtk-emoji-chooser" object data;
     * hook its close (once) to give the dialog its size back.              */
    GtkWidget *chooser =
        g_object_get_data(G_OBJECT(entry), "gtk-emoji-chooser");
    GtkWidget *dlg = g_object_get_data(G_OBJECT(entry), "task-dialog");
    if (chooser != NULL && dlg != NULL &&
        g_object_get_data(G_OBJECT(chooser), "task-close-hooked") == NULL) {
        g_signal_connect(chooser, "closed",
                         G_CALLBACK(on_emoji_chooser_closed), dlg);
        g_object_set_data(G_OBJECT(chooser), "task-close-hooked",
                          GINT_TO_POINTER(1));
    }
    return G_SOURCE_REMOVE;
}

/* on_emoji_box_pressed() — clicking the emoji box opens GTK's emoji
 * chooser on the entry (clearing any previous pick, so choosing always
 * replaces).  GTK3 popovers render INSIDE their toplevel and clip at
 * its edges, so the dialog is grown first to give the chooser room; it
 * shrinks back to natural size when the chooser closes.                    */
static gboolean
on_emoji_box_pressed(GtkWidget *entry, GdkEventButton *event,
                     gpointer data)
{
    (void)event; (void)data;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    GtkWidget *dlg = g_object_get_data(G_OBJECT(entry), "task-dialog");
    if (dlg != NULL) {
        gint w, h;                   /* current dialog frame                */
        gtk_window_get_size(GTK_WINDOW(dlg), &w, &h);
        gtk_window_resize(GTK_WINDOW(dlg), MAX(w, 440), 470);
    }
    g_idle_add(emoji_open_idle, entry);
    return TRUE;                     /* the chooser owns this click         */
}

/* ---------------------------------------------------------------------------
 * run_list_dialog() — the shared New List / Edit List dialog: an emoji
 * box (click opens the picker) and a name entry, prefilled from the
 * name/emoji in-out parameters when editing.  On OK with a non-empty
 * name the trimmed values replace them (caller g_frees) and TRUE
 * returns.
 * ------------------------------------------------------------------------- */
static gboolean
run_list_dialog(TaskLibrary *lw, const gchar *title,
                gchar **name, gchar **emoji)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title,
        GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget *emoji_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(emoji_row),
                       gtk_label_new("List Emoji:"), FALSE, FALSE, 0);
    GtkWidget *emoji_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(emoji_entry), 2);
    gtk_entry_set_max_length(GTK_ENTRY(emoji_entry), 4);
    gtk_entry_set_alignment(GTK_ENTRY(emoji_entry), 0.5f);
    gtk_widget_set_halign(emoji_entry, GTK_ALIGN_START);
    task_app_widget_add_css(emoji_entry, "entry { font-size: 18px; }");
    gtk_widget_set_tooltip_text(emoji_entry,
        "Optional emoji \xe2\x80\x94 click to pick");
    if (*emoji != NULL)
        gtk_entry_set_text(GTK_ENTRY(emoji_entry), *emoji);
    g_signal_connect(emoji_entry, "button-press-event",
                     G_CALLBACK(on_emoji_box_pressed), NULL);
    gtk_box_pack_start(GTK_BOX(emoji_row), emoji_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), emoji_row, FALSE, FALSE, 0);

    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(name_row), gtk_label_new("List name:"),
                       FALSE, FALSE, 0);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(name_entry), 28);
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    if (*name != NULL)
        gtk_entry_set_text(GTK_ENTRY(name_entry), *name);
    gtk_box_pack_start(GTK_BOX(name_row), name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), name_row, FALSE, FALSE, 0);

    /* The click handler grows the dialog so the chooser popover fits.      */
    g_object_set_data(G_OBJECT(emoji_entry), "task-dialog", dlg);

    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
        box, TRUE, TRUE, 0);
    gtk_widget_grab_focus(name_entry);
    gtk_widget_show_all(dlg);

    gboolean ok = FALSE;             /* accepted with a usable name         */
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gchar *new_name = g_strstrip(
            g_strdup(gtk_entry_get_text(GTK_ENTRY(name_entry))));
        gchar *new_emoji = g_strstrip(
            g_strdup(gtk_entry_get_text(GTK_ENTRY(emoji_entry))));
        if (*new_name != '\0') {
            g_free(*name);
            g_free(*emoji);
            *name = new_name;
            *emoji = new_emoji;
            ok = TRUE;
        } else {
            g_free(new_name);
            g_free(new_emoji);
        }
    }
    gtk_widget_destroy(dlg);
    return ok;
}

/* run_group_name_dialog() — modal entry for a group name; fills *out and
 * returns TRUE on accept with non-empty text, FALSE otherwise.             */
static gboolean
run_group_name_dialog(TaskLibrary *lw, const gchar *title, const gchar *button,
                      const gchar *initial, gchar **out)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        button, GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    if (initial && *initial)
        gtk_entry_set_text(GTK_ENTRY(entry), initial);
    else
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Group name");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Group name:"),
                       FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);
    gboolean accepted = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (name && *name) {
            *out     = g_strdup(name);
            accepted = TRUE;
        }
    }
    gtk_widget_destroy(dlg);
    return accepted;
}

/* ---------------------------------------------------------------------------
 * The sidebar's context menu and its actions.  Every item names a "win."
 * action (see library_priv.h); the ones that act on lists read the
 * sidebar's CURRENT selection, which is what the menu was opened on, and
 * the ones that act on a group carry its id as the action target.
 * ------------------------------------------------------------------------- */

/* selected_list_ids() — the ids of every selected LIST row; a view, the
 * header or a group row contributes nothing.  Free with g_array_unref.    */
static GArray *
selected_list_ids(TaskLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
    for (GList *r = rows; r != NULL; r = r->next) {
        GtkTreeIter it;
        if (!gtk_tree_model_get_iter(model, &it, r->data))
            continue;
        gint   k;
        gint64 lid;
        gtk_tree_model_get(model, &it, SB_KIND, &k, SB_ID, &lid, -1);
        if (k == SB_KIND_LIST)
            g_array_append_val(ids, lid);
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

/* lists_set_group() — file every selected list under `group_id` (0 =
 * ungrouped), then refresh.                                                */
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

/* on_move_to_group() — win.move-to-group(x): Move to Group → <group>.     */
static void
on_move_to_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    lists_set_group(data, g_variant_get_int64(param));
}

/* on_remove_from_group() — win.remove-from-group.                         */
static void
on_remove_from_group(TaskLibrary *lw)
{
    lists_set_group(lw, 0);
}

/* on_rename_group() — win.rename-group(x): the group-name dialog, seeded
 * with the current name.                                                   */
static void
on_rename_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    TaskLibrary *lw = data;
    gint64 group_id = g_variant_get_int64(param);
    TaskGroup *grp     = task_db_group_get(lw->app->db, group_id);
    gchar     *current = grp != NULL ? g_strdup(grp->name) : NULL;
    task_group_free(grp);
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "Rename Group", "Rename",
                              current ? current : "", &name)) {
        task_db_group_rename(lw->app->db, group_id, name);
        lib_full_refresh(lw);
        g_free(name);
    }
    g_free(current);
}

/* ---------------------------------------------------------------------------
 * group_delete_confirm() — ask, then remove group `gid`: its lists become
 * ungrouped, its aggregate's saved orders go with it, and a selection
 * sitting on it falls back to the first list.  Both routes to removing a
 * group come here — the context menu's Remove Group and Delete List with
 * a group row selected.
 * ------------------------------------------------------------------------- */
static void
group_delete_confirm(TaskLibrary *lw, gint64 gid)
{
    if (!task_app_confirm(GTK_WINDOW(lw->window), "Remove Group",
                          "Remove this group? Its lists will become "
                          "ungrouped."))
        return;
    if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == gid) {
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id   = 0;
    }
    task_db_group_delete(lw->app->db, gid);
    lib_row_order_keys_drop(SB_KIND_GROUP, gid);
    lib_full_refresh(lw);
}

/* on_delete_group() — win.delete-group(x): the context menu's Remove
 * Group.                                                                   */
static void
on_delete_group(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    group_delete_confirm(data, g_variant_get_int64(param));
}

static void on_new_group(TaskLibrary *lw);
static void on_edit_list(TaskLibrary *lw);
static void on_delete_list(TaskLibrary *lw);

/* ---------------------------------------------------------------------------
 * on_sb_button_press() — right-click on the sidebar: select the row under
 * the pointer (unless it is already part of the selection) and pop the
 * context menu that fits its kind.  New List and New Group are always on
 * it; a list row adds Edit / Delete and the group items; a group row adds
 * Rename / Remove.
 * ------------------------------------------------------------------------- */
static gboolean
on_sb_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    if (event->button != 3) return FALSE;
    TaskLibrary *lw = data;

    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                  (gint)event->x, (gint)event->y,
                                  &path, NULL, NULL, NULL);
    gint   kind = -1;
    gint64 id   = 0;
    if (path) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(model, &it, path))
            gtk_tree_model_get(model, &it, SB_KIND, &kind, SB_ID, &id, -1);
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, NULL, FALSE);
        }
        gtk_tree_path_free(path);
    }

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();
    g_menu_append(section, "New List",  "win.new-list");
    g_menu_append(section, "New Group", "win.new-group");
    task_app_menu_section_end(menu, &section);

    if (kind == SB_KIND_LIST) {
        g_menu_append(section, "Edit List",   "win.edit-list");
        g_menu_append(section, "Delete List", "win.delete-list");
        task_app_menu_section_end(menu, &section);

        /* The group items: Move to Group offers every group; Remove from
         * Group appears when any selected list is in one.                  */
        GArray *ids = selected_list_ids(lw);
        gboolean any_grouped = FALSE;
        for (guint i = 0; i < ids->len; i++) {
            TaskList *l = task_db_list_get(lw->app->db,
                                           g_array_index(ids, gint64, i));
            if (l != NULL) {
                if (l->group_id != 0)
                    any_grouped = TRUE;
                task_list_free(l);
            }
        }
        g_array_unref(ids);
        GPtrArray *groups = task_db_groups(lw->app->db);
        if (groups->len > 0) {
            GMenu *sub = g_menu_new();
            for (guint i = 0; i < groups->len; i++) {
                TaskGroup *g = g_ptr_array_index(groups, i);
                GMenuItem *gi = g_menu_item_new(g->name, NULL);
                g_menu_item_set_action_and_target(gi, "win.move-to-group",
                                                  "x", g->id);
                g_menu_append_item(sub, gi);
                g_object_unref(gi);
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
        GMenuItem *item = g_menu_item_new("Rename Group", NULL);
        g_menu_item_set_action_and_target(item, "win.rename-group", "x", id);
        g_menu_append_item(section, item);
        g_object_unref(item);
        item = g_menu_item_new("Remove Group", NULL);
        g_menu_item_set_action_and_target(item, "win.delete-group", "x", id);
        g_menu_append_item(section, item);
        g_object_unref(item);
        task_app_menu_section_end(menu, &section);
    }
    g_object_unref(section);

    task_app_menu_popup(lw->window, G_MENU_MODEL(menu), event);
    return TRUE;
}

/* on_new_group() — prompt for a name and create a new list group.          */
static void
on_new_group(TaskLibrary *lw)
{
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "New Group", "Create", NULL, &name)) {
        gint64 gid = task_db_group_create(lw->app->db, name);
        if (gid == 0)
            task_app_status(lw->app, "Failed to create group");
        else
            lib_full_refresh(lw);
        g_free(name);
    }
}

/* lib_on_new_list() — prompt (name + optional emoji), create, select.          */
void
lib_on_new_list(TaskLibrary *lw)
{
    gchar *name = NULL;              /* dialog in/out values                */
    gchar *emoji = NULL;
    if (run_list_dialog(lw, "New List", &name, &emoji)) {
        gint64 id = task_db_list_create(lw->app->db, name, emoji);
        if (id == 0) {               /* write failed (logged by the db)     */
            task_app_status(lw->app, "Could not create the list \xe2\x80\x94 "
                            "database write failed");
        } else {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id = id;
            lib_full_refresh(lw);
            task_app_status(lw->app,
                            "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name);
        }
    }
    g_free(name);
    g_free(emoji);
}

/* on_edit_list() — change the selected list's name and/or emoji.           */
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
    if (l == NULL)
        return;
    gchar *name  = g_strdup(l->name);
    gchar *emoji = g_strdup(l->emoji);
    task_list_free(l);
    if (run_list_dialog(lw, "Edit List", &name, &emoji)) {
        task_db_list_update(lw->app->db, id, name, emoji);
        lib_full_refresh(lw);
        task_app_status(lw->app,
                        "Updated list \xe2\x80\x9c%s\xe2\x80\x9d", name);
    }
    g_free(name);
    g_free(emoji);
}

/* on_sidebar_activated() — double-click on a real list opens the Edit
 * List dialog (the first click of the pair already settled the
 * selection on the row).  Metas, the Lists header (which keeps its
 * default expand/collapse) and the Notes row do nothing.                  */
static void
on_sidebar_activated(GtkTreeView *view, GtkTreePath *path,
                     GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    TaskLibrary *lw = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    gint kind;
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    if (kind == SB_KIND_LIST)
        on_edit_list(lw);
}

/* on_delete_list() — confirm + tombstone the selected real list; when a
 * group is selected, delegate to on_sb_ctx_delete_group.                   */
static void
on_delete_list(TaskLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_GROUP) {
        group_delete_confirm(lw, lw->sel_id);
        return;
    }
    if (lib_view_refuse(lw, "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6"))
        return;
    gint64 id = lib_selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app, "Select a list to delete");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    gboolean yes = task_app_confirm(GTK_WINDOW(lw->window), "Delete List",
        "Delete the list \xe2\x80\x9c%s\xe2\x80\x9d and all of its "
        "tasks?", l->name);
    if (yes) {
        task_db_list_delete(lw->app->db, id);
        lib_row_order_keys_drop(SB_KIND_LIST, id);
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id = 0;              /* falls back to the first list        */
        lib_full_refresh(lw);
        task_app_status(lw->app,
                        "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", l->name);
    }
    task_list_free(l);
}

/* ---------------------------------------------------------------------------
 * task_sidebar_build() — build the sidebar pane into `paned` (see library_priv.h).
 * ------------------------------------------------------------------------- */
void
task_sidebar_build(TaskLibrary *lw, GtkWidget *paned)
{
    /* Sidebar.                                                             */
    lw->sb_store = gtk_tree_store_new(SB_N_COLS, G_TYPE_INT,
                                      G_TYPE_INT64, G_TYPE_STRING,
                                      G_TYPE_INT);
    lw->sb_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->sb_store));
    g_object_unref(lw->sb_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(lw->sb_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(lw->sb_view), FALSE);
    GtkCellRenderer *sb_cell = gtk_cell_renderer_text_new();
    /* Ellipsize so a narrowed sidebar reads "Weekly Fore…" rather than
     * slicing a label mid-glyph; the renderer needs a width to ellipsize
     * against, which the FIXED column below gives it.                      */
    g_object_set(sb_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *sb_col =
        gtk_tree_view_column_new_with_attributes("Lists", sb_cell,
            "text", SB_LABEL, "weight", SB_WEIGHT, NULL);
    /* FIXED, not the default GROW_ONLY: GROW_ONLY ratchets — once a long
     * name has been shown the column keeps that width even after the row
     * is gone, so the floor only ever went up.                             */
    gtk_tree_view_column_set_sizing(sb_col, GTK_TREE_VIEW_COLUMN_FIXED);
    /* FIXED sizing needs an explicit width or the column has none; keep
     * it small and let expand=TRUE fill whatever the pane actually is.
     * The 40 px is a floor on the TREE VIEW's request only — the
     * EXTERNAL scroller does not pass that up to the pane.                 */
    gtk_tree_view_column_set_fixed_width(sb_col, 40);
    gtk_tree_view_column_set_expand(sb_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->sb_view), sb_col);
    /* Sidebar palette (Notes): the backdrop (rows AND the empty area below
     * them — the tree view paints the whole widget) is the theme's window/
     * toolbar background taken down a step, so the pane sits just behind
     * the toolbar above it and reads as distinct from the white task list
     * without pinning a grey of its own.  A tree view left alone would
     * paint the white theme BASE colour instead.  Both CSS colour functions
     * work from this widget-scoped provider (verified on GTK 3.24 /
     * Adwaita: @theme_bg_color = rgb(246,245,244), exactly what the toolbar
     * renders, and shade(…, 0.96) = rgb(238,236,234)); beware that an
     * UNDEFINED colour name is NOT a parse error here — it silently renders
     * transparent.  Then muted grey text and a blue selection bar with
     * white text.                                                          */
    task_app_widget_add_css(lw->sb_view,
        "treeview.view {"
        "  background-color: shade(@theme_bg_color, " SB_BG_SHADE ");"
        "  color: rgb(65,65,65);"
        "}"
        "treeview.view:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}");
    GtkTreeSelection *sb_sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    gtk_tree_selection_set_mode(sb_sel, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(sb_sel, sb_row_selectable,
                                           lw, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_changed), lw);
    g_signal_connect(lw->sb_view, "row-activated",
                     G_CALLBACK(on_sidebar_activated), lw);
    g_signal_connect(lw->sb_view, "button-press-event",
                     G_CALLBACK(on_sb_button_press), lw);
    GtkWidget *sb_scroll = gtk_scrolled_window_new(NULL, NULL);
    /* EXTERNAL, not NEVER, horizontally: NEVER makes the scroller demand
     * its child's FULL width as a minimum, so the widest row (a long
     * list name) became a floor the divider could not be dragged past.
     * EXTERNAL scrolls without ever showing a scrollbar, which is what
     * lets the pane go narrower than the content.                          */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll),
                                   GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sb_scroll), lw->sb_view);

    /* Sidebar column: a fixed spacer, then the tree.  Top padding, so the
     * first row's text sits level with the text in the task list's column
     * headers (the sidebar has none of its own).  It is a SPACER WIDGET
     * rather than CSS padding: GtkScrolledWindow ignores padding when
     * allocating its child, and a margin on the tree view would scroll away
     * with it.  Painted in the sidebar grey so the strip reads as part of
     * the pane.  A GtkBox has no background of its own, so it repeats the
     * tree view's backdrop expression verbatim — keep the two in step.     */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *sidebar_pad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_pad, -1, SB_TOP_PAD);
    task_app_widget_add_css(sidebar_pad,
        "box { background-color: shade(@theme_bg_color, "
        SB_BG_SHADE "); }");
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_pad, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), sb_scroll, TRUE, TRUE, 0);

    /* shrink=TRUE (4th arg): the pane may allocate the sidebar LESS than
     * its minimum.  With shrink=FALSE the divider stops at that minimum
     * no matter what the scroll policy says — both are needed.            */
    gtk_paned_pack1(GTK_PANED(paned), sidebar_box, FALSE, TRUE);
    lw->sidebar_box = sidebar_box;   /* for the toolbar show/hide toggle    */
}

/* ---------------------------------------------------------------------------
 * task_sidebar_install_actions() — the "win." actions the sidebar's
 * context menu, the toolbar's Sidebar button and File → New List name
 * (see library_priv.h).  Installed by task_library_window_new before any
 * menu can be shown.
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
