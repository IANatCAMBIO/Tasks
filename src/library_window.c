/* ===========================================================================
 * library_window.c — the main Tasks window (see library_window.h)
 * =========================================================================== */

#include "library_window.h"
#include "editor_window.h"
#include "task_ops.h"
#include "backup.h"
#include "task_worker.h"
#include "plugin_loader.h"
#include "task_view.h"
#include "task_rows.h"
#include "task_ui.h"
#include "search.h"
#include "settings_window.h"
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* Odd-row stripe tint of the task list (the Notes list palette).          */
/* Background applied to the row currently held during a manual drag.       */
#define DRAG_ROW_TINT "#fde68a"

/* Blank strip above the sidebar tree, to line its first row's text up with
 * the task list's column-header text (see task_library_window_new).        */
#define SB_TOP_PAD 3

/* How far the sidebar backdrop sits below the toolbar/window background it
 * is shaded from — a CSS shade() factor, < 1 darkens.  0.96 turns Adwaita's
 * rgb(246,245,244) into rgb(238,236,234).  A string, not a number: it is
 * pasted into two CSS declarations in task_library_window_new.             */
#define SB_BG_SHADE "0.96"

/* Sidebar row kinds (SB_KIND column).                                      */
enum {
    SB_KIND_VIEW = 0,                /* a registered virtual view; SB_ID
                                      * holds its registry INDEX, not a
                                      * list id (see task_view.h).  Panel
                                      * views (the Weekly Forecast) are
                                      * registered like any other          */
    SB_KIND_HEADER,                  /* the "Lists" section header          */
    SB_KIND_LIST,                    /* a real list                         */
    SB_KIND_GROUP                    /* a list-group sub-header             */
};

/* Sidebar store columns.                                                   */
enum {
    SB_KIND = 0,                     /* gint: one of SB_KIND_*              */
    SB_ID,                           /* gint64: list id (SB_KIND_LIST)      */
    SB_LABEL,                        /* gchar*: display text                */
    SB_WEIGHT,                       /* gint: Pango weight (bold metas)     */
    SB_N_COLS
};


/* ---------------------------------------------------------------------------
 * TaskLibrary — the window's state.
 *   sel_kind/sel_id — current sidebar selection (survives refreshes).
 *   populating      — guards the sidebar changed handler during rebuilds.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp        *app;
    GtkWidget    *window;
    GtkTreeStore *sb_store;
    GtkWidget    *sb_view;
    GtkListStore *task_store;
    GtkWidget    *task_view;
    GtkWidget    *task_scroll;       /* the regular task pane; swapped
                                      * with forecast_box (visibility)      */
    /* Panel panes, keyed by the TaskView POINTER rather than by its
     * registry index.  The index is not a stable identity: registering a
     * view re-sorts the registry, so a plugin enabled at run time
     * renumbers every view after it and an index-keyed table would hand
     * back the wrong pane.  Built LAZILY — a view registered after this
     * window was constructed has to be able to make its pane too.        */
    GHashTable   *panels;            /* const TaskView* -> GtkWidget*      */
    GtkWidget    *task_pane;         /* the box a new panel packs into     */
    /* Kanban board — the THIRD task-pane variant, one lane per
     * TaskStatus.  Lane INDEX IS the status value, which is what lets a
     * drop read its target status straight off the lane it landed on.      */
    GtkWidget    *kanban_box;        /* the board's outer scroller          */
    GtkWidget    *kanban_labels[TASK_STATUS_N_VALUES];  /* lane headings    */
    GtkWidget    *kanban_lanes[TASK_STATUS_N_VALUES];   /* card containers  */
    GHashTable   *kanban_sel;        /* SET of selected task ids (keys are
                                      * GSIZE_TO_POINTER'd) — the board's
                                      * answer to the tree view's
                                      * multi-selection, so Delete Task and
                                      * the context menu have something to
                                      * act on.  Created with the window;
                                      * never NULL.                         */
    gint64        kanban_anchor;     /* last plainly-clicked card: the fixed
                                      * end of a shift-click range          */
    gboolean      kanban;            /* the kanban_view config flag, cached
                                      * like manual_sort; kanban_apply is
                                      * the single writer                   */
    GtkWidget    *kanban_drops[TASK_STATUS_N_VALUES];  /* lane hit boxes    */
    GdkCursor    *card_grab;         /* "grab" — hovering a card            */
    GdkCursor    *card_grabbing;     /* "grabbing" — dragging one.  Both
                                      * made ONCE and kept, like
                                      * drag_cursor: a card is realized per
                                      * refresh, so building one per card
                                      * would allocate on every rebuild     */
    /* The hand-rolled card drag (GTK DnD is not used on the board — see
     * the Kanban banner).  `card_armed` is the window between the press
     * and the motion threshold, where it is still only a click.           */
    GtkWidget    *card_drag_src;     /* card under the pointer, or NULL     */
    GtkWidget    *card_drag_handle;  /* its ⠿ grip: the grab window and the
                                      * only place a drag can start from    */
    gint64        card_drag_id;      /* its task                            */
    gboolean      card_armed;        /* pressed, not yet a drag             */
    gboolean      card_dragging;     /* past the threshold, grab held       */
    gint          card_hot_x;        /* pointer offset inside the card, so  */
    gint          card_hot_y;        /* the ghost sits where it was picked  */
    gdouble       card_press_rx;     /* press position in ROOT coords —     */
    gdouble       card_press_ry;     /* the threshold is measured from it   */
    GtkWidget    *card_ghost;        /* the floating translucent copy       */
    GtkWidget    *card_mark;         /* insertion marker, or NULL           */
    gint          card_mark_lane;    /* where the marker currently sits —   */
    gint          card_mark_slot;    /* only a CHANGE moves it, so the
                                      * pointer can wander inside a slot
                                      * without any widget churn            */
    gulong        card_key_handler;  /* Escape-cancels handler on the
                                      * toplevel, live only while dragging  */
    GtkWidget    *sidebar_box;       /* for the toolbar show/hide toggle    */
    GtkWidget    *toolbar;           /* hidden by Compact Layout            */
    GtkWidget    *toolbar_rule;      /* the thin rule under the toolbar     */
    GtkWidget    *ui_tool_rule;      /* divider before contributed buttons  */
    GtkWidget    *float_bar;         /* Compact Layout's floating New /
                                      * Delete Task pair (overlay child)    */
    GtkWidget    *search_entry;      /* the toolbar's search box, at the
                                      * right edge where Notes keeps its    */
    TaskSearch   *search;            /* its parsed query, or NULL for "no
                                      * filter" — the ONE test for whether
                                      * a search is active (see search.h)   */
    GtkWidget    *status_left;       /* selection info label                */
    GtkWidget    *status_right;      /* latest event message label          */
    guint         listen_changed;    /* TaskApp event subscriptions —       */
    guint         listen_tasks;      /* dropped in on_library_destroy       */
    guint         listen_status;     /* BEFORE the editors close            */
    GtkWidget    *hide_done_item;    /* completed-visibility toggle button  */
    GtkWidget    *manual_sort_item;  /* manual-sort mode toggle button      */
    GtkWidget    *pane_item;         /* list <-> Kanban pane toggle button  */
    /* Every toggling View item is an ACTION item, not a check item: its
     * LABEL is the action a click performs (see the *_LABEL_TO_* macros),
     * so none of them carries the current state to read back — every
     * handler flips the config or the cache instead.                      */
    GtkWidget    *view_show_done_item;  /* Show / Hide Completed            */
    GtkWidget    *view_kanban_item;     /* Kanban View / List View          */
    GtkWidget    *view_manual_sort_item;/* Manual / Automatic Sorting       */
    GtkWidget    *view_compact_item;    /* Compact / Full Controls          */
    GtkWidget    *view_sidebar_item;    /* Show / Hide Sidebar              */
    gint          sel_kind;
    gint64        sel_id;
    gboolean      populating;
    gboolean      sb_populated;      /* first population expands Lists      */
    gboolean      pinned_row_shown;  /* Pinned Tasks row exists (hidden
                                      * while nothing is pinned)            */
    GHashTable   *group_expanded;    /* group id (ptr) → expanded gboolean  */
    gint          sb_width;          /* live divider position (persisted
                                      * at close as sidebar_width)          */
    gint          win_w, win_h;      /* live client size (persisted at
                                      * close as the next launch's size)    */
    gboolean             manual_sort;    /* task_list_manual_sort, cached:
                                          * read per motion event and per
                                          * refresh, so it must not cost a
                                          * GKeyFile lookup + strdup each
                                          * time.  task_manual_sort_apply
                                          * is the single writer.          */
    gboolean             drag_active;    /* live task-row drag in progress  */
    GtkTreeRowReference *drag_row_ref;   /* auto-updating ref to drag row  */
    GtkTreeRowReference *drag_lock_ref;  /* row just swapped; locked until
                                          * cursor re-enters drag row      */
    GdkCursor           *drag_cursor;    /* the "ns-resize" cursor, made
                                          * once (owned; the motion path
                                          * would otherwise allocate one
                                          * per event)                     */
    gint                 pending_fades;       /* active fade-out animations */
    guint                status_fade_source;  /* delay before fade starts   */
    guint                status_fade_step_source; /* per-step fade timer    */
    gint                 status_fade_step;    /* current step               */
    gchar               *status_fade_text;   /* plain text being faded      */
} TaskLibrary;

/* ---------------------------------------------------------------------------
 * manual_sort_live() — may rows be hand-reordered RIGHT NOW?
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
 * refresh_tasks still applies it to whatever survived the filter, which
 * keeps the matches in the order the user put them in.
 *
 * The board has no handle column to hide, so card_drop_apply skips its
 * ORDER half instead and lets the status change through — that drag is not
 * lossy, and the two halves are already independent there.
 * ------------------------------------------------------------------------- */
static gboolean
manual_sort_live(TaskLibrary *lw)
{
    return lw->manual_sort && lw->search == NULL;
}

/* list_label() — a list's display label: the optional emoji prefixes
 * the name, set off by two spaces.  New string (g_free).                   */
static gchar *
list_label(const TaskList *l)
{
    return *l->emoji != '\0'
        ? g_strdup_printf("%s  %s", l->emoji, l->name)
        : g_strdup(l->name);
}

/* lib_of() — the TaskLibrary behind app->library_window.                   */
static TaskLibrary *
lib_of(TaskApp *app)
{
    if (app->library_window == NULL)
        return NULL;
    return g_object_get_data(G_OBJECT(app->library_window), "task-library");
}

/* ===========================================================================
 * Chrome that has to match the window background (@theme_bg_color).
 * =========================================================================== */

/* ThemedCssFunc — build a widget's CSS from the resolved background color.
 * New string (the caller g_frees it).                                      */
typedef gchar *(*ThemedCssFunc)(const GdkRGBA *bg);

/* ---------------------------------------------------------------------------
 * themed_bg_css_apply() — style `w` from the theme's @theme_bg_color, and
 * keep it in step when the theme changes (a macOS light/dark switch, or a
 * GTK theme swap on Linux).
 *
 * Resolving the NAMED color rather than hardcoding a gray is the whole
 * point: it is what makes these widgets match the window and the status
 * bar, whatever the theme paints them.  A theme that doesn't name the
 * color is the one case we leave alone rather than guess — the widget
 * keeps its default look.
 *
 * The provider is created once and RELOADED in place, kept on the widget as
 * object data: task_app_widget_add_css would stack a fresh provider on every
 * theme change.  The last color written is stored alongside it, which is
 * also what stops the recursion — our own reload re-emits "style-updated",
 * and the second pass resolves the same color and returns without writing.
 * (@theme_bg_color comes from the theme's provider, not ours, so the
 * resolved value really is stable across our own reload.)
 * ------------------------------------------------------------------------- */
static void themed_bg_css_apply(GtkWidget *w, ThemedCssFunc build);

/* on_themed_style_updated() — the theme moved: recompute from the builder
 * stashed on the widget.                                                   */
static void
on_themed_style_updated(GtkWidget *w, gpointer data)
{
    (void)data;
    themed_bg_css_apply(w, (ThemedCssFunc)g_object_get_data(
                               G_OBJECT(w), "task-themed-build"));
}

static void
themed_bg_css_apply(GtkWidget *w, ThemedCssFunc build)
{
    if (build == NULL)
        return;
    GdkRGBA bg;                      /* the window/status-bar background    */
    if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w),
                                        "theme_bg_color", &bg))
        return;

    GdkRGBA *last = g_object_get_data(G_OBJECT(w), "task-themed-rgba");
    if (last != NULL && gdk_rgba_equal(last, &bg))
        return;                      /* unchanged — and ends the recursion  */

    GtkCssProvider *provider =
        g_object_get_data(G_OBJECT(w), "task-themed-css");
    if (provider == NULL) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider(gtk_widget_get_style_context(w),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_set_data_full(G_OBJECT(w), "task-themed-css", provider,
                               g_object_unref);
        g_object_set_data(G_OBJECT(w), "task-themed-build", (gpointer)build);
        g_signal_connect(w, "style-updated",
                         G_CALLBACK(on_themed_style_updated), NULL);
    }
    g_object_set_data_full(G_OBJECT(w), "task-themed-rgba",
                           gdk_rgba_copy(&bg),
                           (GDestroyNotify)gdk_rgba_free);
    gchar *css = build(&bg);
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    g_free(css);
}

/* rgb_of() — a GdkRGBA as a CSS "rgb(r,g,b)" literal (new string).         */
static gchar *
rgb_of(const GdkRGBA *c)
{
    return g_strdup_printf("rgb(%d,%d,%d)",
                           (gint)(c->red   * 255 + 0.5),
                           (gint)(c->green * 255 + 0.5),
                           (gint)(c->blue  * 255 + 0.5));
}

/* header_flatten_css() — the column-header CSS: the flat background plus
 * shades of it for :hover / :active, so a sortable header still gives
 * feedback instead of jumping back to the theme's button color.  Quartz
 * only, like its one caller — otherwise it is an unused static.            */
#ifdef GDK_WINDOWING_QUARTZ
static gchar *
header_flatten_css(const GdkRGBA *bg)
{
    gchar *c   = rgb_of(bg);
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
    themed_bg_css_apply(hbtn, header_flatten_css);
#endif
}


/* ===========================================================================
 * Refreshes.
 * =========================================================================== */

/* scroll_keep_queue() — restore a scrolled window's vertical position
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

/* scroll_keep_queue_win() — the same, given the scrolled window itself
 * (the Weekly Forecast's one outer scroller wraps a box, not a view).      */
static void
scroll_keep_queue_win(GtkWidget *scroll)
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

/* task_library_scroll_keep() — the same, exported for panel plugins (see
 * library_window.h).                                                       */
void
task_library_scroll_keep(GtkWidget *scroll)
{
    scroll_keep_queue_win(scroll);
}

/* task_library_set_location() — see library_window.h.  Re-resolves the
 * window, so it is safe from a panel that outlived it.                     */
void
task_library_set_location(TaskApp *app, const gchar *text)
{
    TaskLibrary *lw = lib_of(app);
    if (lw != NULL && lw->status_left != NULL)
        gtk_label_set_text(GTK_LABEL(lw->status_left), text);
}

static void
scroll_keep_queue(GtkWidget *view)
{
    scroll_keep_queue_win(gtk_widget_get_parent(view));
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the sidebar and restore the selection.
 * ------------------------------------------------------------------------- */

static void     task_view_apply_manual_order(TaskLibrary *lw);
static void     task_manual_sort_apply(TaskLibrary *lw);
static gchar   *list_order_key(gint64 list_id);
static gboolean on_column_header_press(GtkWidget *, GdkEventButton *, gpointer);
static void     on_toggle_kanban(GtkWidget *, gpointer);
static void     full_refresh(TaskLibrary *lw);
static void     on_ui_task_menu_activated(GtkWidget *, gpointer);
static void     scroll_keep_queue_win(GtkWidget *scroll);

/* sel_view() — the registered view the sidebar is sitting on, or NULL
 * when the selection is a list or a group.                                 */
static const TaskView *
sel_view(TaskLibrary *lw)
{
    if (lw->sel_kind != SB_KIND_VIEW)
        return NULL;
    return task_view_nth((guint)lw->sel_id);
}

/* view_refuse() — a virtual view is not a list, so Edit List / Delete
 * List have nothing to act on.  Posts the view's own explanation (or a
 * generic one) and returns TRUE when the caller should stop.
 *
 * `alternative` completes the sentence for a view that did not supply
 * its own `not_a_list` text.                                              */
static gboolean
view_refuse(TaskLibrary *lw, const gchar *alternative)
{
    const TaskView *v = sel_view(lw);
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

/* panel_widget() — the pane a panel view owns, building it on first use.
 *
 * NULL for a query view, and for a panel view before the window's own
 * pane box exists.  Lazy because a view can arrive at ANY time: enabling
 * a plugin registers its views immediately, and one registered after
 * this window was built would otherwise have no pane and show an empty
 * area for the rest of the session.                                      */
static GtkWidget *
panel_widget(TaskLibrary *lw, const TaskView *v)
{
    if (lw->panels == NULL || v == NULL || !task_view_is_panel(v))
        return NULL;
    GtkWidget *w = g_hash_table_lookup(lw->panels, v);
    if (w != NULL)
        return w;
    if (lw->task_pane == NULL)
        return NULL;                 /* too early: no box to pack into     */
    w = v->panel_new(lw->app, v->user_data);
    if (w == NULL)
        return NULL;
    g_hash_table_insert(lw->panels, (gpointer)v, w);
    /* Packed BEFORE the Kanban box so the panes keep their construction
     * order, and shown explicitly: the window's show_all has long since
     * run, so a widget added now starts hidden.                          */
    gtk_box_pack_start(GTK_BOX(lw->task_pane), w, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(lw->task_pane), w, 1);
    gtk_widget_show_all(w);
    return w;
}

/* panels_prune() — destroy panes whose view has left the registry.
 *
 * A plugin switched off takes its views with it, but the pane it built
 * is a child of this window and would otherwise sit there for the rest
 * of the session, hidden but alive, holding whatever it cached.         */
static void
panels_prune(TaskLibrary *lw)
{
    if (lw->panels == NULL)
        return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, lw->panels);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        gboolean live = FALSE;
        for (guint i = 0; i < task_view_count() && !live; i++)
            live = task_view_nth(i) == key;
        if (!live) {
            gtk_widget_destroy(val);
            g_hash_table_iter_remove(&it);
        }
    }
}

/* view_visible() — whether a view's sidebar row should exist right now.   */
static gboolean
view_visible(TaskLibrary *lw, const TaskView *v)
{
    return v->visible == NULL || v->visible(lw->app, v->user_data);
}

/* sidebar_show_pinned() — the Favorites row's visibility, which the
 * light notify hook watches for a 0 <-> nonzero transition.               */
static gboolean
sidebar_show_pinned(TaskLibrary *lw)
{
    const TaskView *v = task_view_find("pinned");
    return v != NULL && view_visible(lw, v);
}

static void
refresh_sidebar(TaskLibrary *lw)
{
    lw->populating = TRUE;
    scroll_keep_queue(lw->sb_view);

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
    lw->pinned_row_shown = sidebar_show_pinned(lw);
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
            gchar *label = list_label(l);
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
        gchar *label = list_label(l);
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


/* ===========================================================================
 * Kanban board — the third task-pane variant.
 *
 * Built from the Weekly Forecast's parts (a heading label over a framed
 * body, everything at natural height inside ONE outer scroller so the
 * whole board scrolls together), with the sections turned through 90°:
 * three side-by-side lanes, one per TaskStatus, holding CARDS rather
 * than list rows.  Lane index IS the status value, so a drop reads its
 * target status straight off the lane it landed on.
 *
 * Cards are real widgets, not cell renderers, because they have to be
 * dragged; the tree view's own row DnD is the thing gotcha 13 says to
 * stay away from on quartz.
 *
 * The drag is HAND-ROLLED — a pointer grab plus a floating ghost window —
 * not GTK's drag-and-drop.  GTK DnD was tried first and rejected
 * (2026-08-25) for one reason: on quartz it hands the gesture to
 * AppKit's NSDraggingSession, which owns the cursor for the duration and
 * paints its own arrow-plus-green-plus badge.  NOTHING in GTK can
 * override that — gdk_window_set_cursor on the card or the toplevel is
 * simply ignored while the session runs — so the closed-hand cursor is
 * unreachable through it.  Owning the gesture gets both halves of what
 * a drag should look like: the grab's own cursor, and a translucent copy
 * of the card itself following the pointer instead of a system badge.
 * It also keeps the board clear of quartz's DnD entirely, which gotchas
 * 12 and 13 both come from.
 *
 * The manual-sort row drag in the task pane works the same way (motion
 * events, no GTK DnD), so this is the established shape here.
 * =========================================================================== */

/* How far the pointer must travel before a press becomes a drag rather
 * than a click.  gtk_drag_check_threshold uses the platform's own value,
 * so a click that wobbles a pixel still selects rather than dragging.     */

/* The ghost's opacity: enough to read the card through it, enough to see
 * the lane underneath.                                                    */
#define CARD_GHOST_ALPHA 0.65

/* Every toggling View-menu item names the thing a click DOES, not the state
 * in force — the same idiom as the completed-visibility toolbar button,
 * whose icon shows the action it offers.  One item, one click, no guessing
 * what "unchecked" would have meant.  Each pair is TO_<destination>.       */
#define SORT_LABEL_TO_MANUAL  "Manual Sorting"
#define SORT_LABEL_TO_AUTO    "Automatic Sorting"
#define DONE_LABEL_TO_HIDE    "Hide Completed"
#define DONE_LABEL_TO_SHOW    "Show Completed"
#define SIDEBAR_LABEL_TO_HIDE "Hide Sidebar"
#define SIDEBAR_LABEL_TO_SHOW "Show Sidebar"
/* "List View", singular: "Lists" in this app is the sidebar's data type
 * (the user's task lists), so "Lists View" would read as "show me the
 * lists" rather than "put the tasks back in a list".                      */
#define PANE_LABEL_TO_KANBAN  "Kanban View"
#define PANE_LABEL_TO_LIST    "List View"
/* Compact Layout names the CONTROLS, not the layout: what the setting
 * actually does is swap the toolbar for the floating New/Delete pair, and
 * "Full Layout" would promise something about the window it does not
 * change (the sidebar follows its own item in both modes).                */
#define CTRL_LABEL_TO_COMPACT "Compact Controls"
#define CTRL_LABEL_TO_FULL    "Full Controls"

/* The search box's own text.  The PLACEHOLDER says what is searched, not
 * merely "Search": the box narrows the SELECTED view rather than sweeping
 * the database, and a user who reads "Search all tasks" (Notes' wording,
 * for a box that really does search everything) would read an empty result
 * in one list as "that task does not exist".  All Tasks is a view like any
 * other, so selecting it and typing IS the search-everything case.
 *
 * The TOOLTIP is the only place the operators are written down, so it
 * spells both out with an example rather than naming them.  It is also set
 * from task_pane_mode_apply, which swaps in a reason while the box is
 * greyed — hence a macro rather than a string at the construction site.   */
#define SEARCH_PLACEHOLDER "Search this view"
#define SEARCH_TOOLTIP \
    "Search the selected view's task titles, notes and subtasks.\n" \
    "\"quoted words\" matches the phrase; -word leaves out tasks " \
    "containing it."

/* Thickness of the insertion marker, in logical px.  Thin on purpose: it
 * occupies a slot in the lane, so anything chunky would shove the cards
 * around as it moves between slots.                                       */
#define CARD_MARK_H 3

/* Breathing room either side of the ⠿ grip glyph.  Narrow on purpose: the
 * grip is a grab target, not a column.                                    */
#define CARD_GRIP_PAD 5

/* Inner insets, applied as WIDGET MARGINS on the child — neither CSS
 * padding nor border_width works on a visible-window GtkEventBox, see the
 * note in kanban_css_install.                                             */
#define CARD_PAD 8               /* card border → its text               */
#define LANE_PAD 6               /* lane frame → the cards inside it     */

/* pad_widget() — inset a widget from its parent on all four sides.        */
static void
pad_widget(GtkWidget *w, gint pad)
{
    gtk_widget_set_margin_start(w, pad);
    gtk_widget_set_margin_end(w, pad);
    gtk_widget_set_margin_top(w, pad);
    gtk_widget_set_margin_bottom(w, pad);
}

/* kanban_css_install() — the board's look, installed ONCE for the whole
 * screen and keyed off style classes.
 *
 * Screen-wide rather than the per-widget themed_bg_css_apply the float bar
 * and column headers use, for two reasons: there is one provider instead
 * of one per card (a busy board is hundreds), and every color here is a
 * NAMED theme color, so GTK re-resolves them itself on a light/dark switch
 * — the staleness that helper exists to work around only arises because it
 * bakes a resolved literal into its CSS from C.
 * ------------------------------------------------------------------------- */
static void
kanban_css_install(void)
{
    static gboolean done = FALSE;    /* one provider per process            */
    if (done)
        return;
    done = TRUE;
    GtkCssProvider *p = gtk_css_provider_new();
    /* SQUARE corners throughout, matching the Weekly Forecast's framed day
     * sections (a plain GTK_SHADOW_IN GtkFrame, which has none): a rounded
     * tint inside a square frame reads as a mistake, and rounded cards
     * inside that made the board the odd view out.  No border-radius here
     * is deliberate — don't add one back.                                  */
    /* No `padding` here, deliberately.  Both classes land on a
     * GtkEventBox, and a visible-window event box honors NEITHER CSS
     * padding NOR gtk_container_set_border_width for its own size — both
     * were tried, and the card came out exactly as tall as its label
     * (measured 214x15 against a 15 px label), text hard against the
     * border.  The inset is set with WIDGET MARGINS on the child instead,
     * which GTK's size machinery always folds into the preferred size, so
     * the card grows by them and the background and border still paint at
     * the widget's own edge.                                              */
    gtk_css_provider_load_from_data(p,
        ".task-lane {"
        "  background-color: alpha(@theme_fg_color, 0.05);"
        "}"
        ".task-card {"
        "  background-color: @theme_base_color;"
        "  border: 1px solid alpha(@theme_fg_color, 0.22);"
        "}"
        ".task-card:hover {"
        "  border-color: alpha(@theme_fg_color, 0.45);"
        "}"
        /* The landing indicator, in two parts: the lane tint says which
         * COLUMN, the marker bar says which SLOT within it.  .task-lane-target
         * is listed AFTER .task-lane so it wins at equal specificity (both
         * classes sit on the same widget).                               */
        ".task-lane-target {"
        "  background-color: alpha(@theme_selected_bg_color, 0.22);"
        "}"
        ".task-card-mark {"
        "  background-color: @theme_selected_bg_color;"
        "}"
        /* The ⠿ grip strip down the card's left edge.  Only this area
         * starts a drag, and only it wears the hand cursor — the rest of
         * the card clicks and selects like an ordinary row.               */
        ".task-card-handle:hover {"
        "  background-color: alpha(@theme_fg_color, 0.10);"
        "}"
        /* The original card stays in place while its ghost is carried
         * around, dimmed so it reads as "this is the one in flight".     */
        ".task-card-dragging {"
        "  opacity: 0.40;"
        "}"
        /* The board's stand-in for a tree selection: what Delete Task and
         * the status bar are talking about.                                */
        ".task-card-selected {"
        "  border-color: @theme_selected_bg_color;"
        "  background-color: alpha(@theme_selected_bg_color, 0.16);"
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* lane_clear() — destroy a lane's cards.  The board's equivalent of the
 * forecast's gtk_list_store_clear: cards are widgets, so emptying a lane
 * means destroying its children (which also drops their drag sources).    */
static void
lane_clear(GtkWidget *lane)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(lane));
    for (GList *k = kids; k != NULL; k = k->next)
        gtk_widget_destroy(GTK_WIDGET(k->data));
    g_list_free(kids);
}

/* ---------------------------------------------------------------------------
 * card_cursor() — one of the board's two cached cursors, built on first
 * use from `name` and kept on `slot`.
 *
 * Returns NULL when the display cannot supply that name, which callers
 * pass straight to gdk_window_set_cursor: the window default is the right
 * fallback, not a guessed stock cursor.  (Same contract as the task
 * view's "ns-resize" cursor.)
 * ------------------------------------------------------------------------- */
static GdkCursor *
card_cursor(GtkWidget *w, GdkCursor **slot, const gchar *name)
{
    if (*slot == NULL)
        *slot = gdk_cursor_new_from_name(gtk_widget_get_display(w), name);
    return *slot;
}

/* card_set_cursor() — point a realized widget's window at `cursor`.        */
static void
card_set_cursor(GtkWidget *w, GdkCursor *cursor)
{
    GdkWindow *win = gtk_widget_get_window(w);
    if (win != NULL)
        gdk_window_set_cursor(win, cursor);
}

/* on_handle_realize() — the ⠿ grip just got its GdkWindow: give it the
 * open hand.  Set on the WINDOW rather than tracked with enter/leave
 * handlers, so hovering costs nothing per motion event and the cursor is
 * simply a property of the grip's own area — which is precisely why the
 * grip is a separate widget: the rest of the card keeps the default
 * arrow because it never gets a cursor of its own.                        */
static void
on_handle_realize(GtkWidget *handle, gpointer data)
{
    TaskLibrary *lw = data;
    card_set_cursor(handle, card_cursor(handle, &lw->card_grab, "grab"));
}

/* Defined below with the rest of the drag engine; on_card_press needs the
 * first to cancel the press its own click armed, and card_drag_stop needs
 * the second to clear the landing indicator on the way out.               */
static void card_drag_stop(TaskLibrary *lw);
static void card_lane_highlight(TaskLibrary *lw, gint lane);
static void card_mark_clear(TaskLibrary *lw);
static GArray *lane_card_ids(TaskLibrary *lw, gint s);
static void on_handle_realize(GtkWidget *handle, gpointer data);
static gboolean on_handle_press(GtkWidget *handle, GdkEventButton *ev,
                                gpointer data);
static gboolean task_context_menu_popup(TaskLibrary *lw, GtkWidget *anchor,
                                        GdkEventButton *event);

/* card_task_id() — the task a card stands for (0 if somehow unset).        */
static gint64
card_task_id(GtkWidget *card)
{
    return (gint64)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(card), "task-task-id"));
}

/* ---------------------------------------------------------------------------
 * on_handle_press() — a press on the ⠿ grip ARMS a drag.
 *
 * Arming rather than dragging immediately is what keeps a click a click:
 * the press only becomes a drag once on_card_motion sees the pointer pass
 * the platform's threshold.
 *
 * Returns FALSE so the press keeps propagating to the CARD, which selects
 * — clicking the grip should select the card like clicking anywhere else
 * on it, and a double-click on the grip should still open the editor.
 *
 * The hot spot is translated into the CARD's coordinates: the ghost is a
 * picture of the whole card, so it has to hang off the pointer where the
 * card was gripped, not where the grip was.
 * ------------------------------------------------------------------------- */
static gboolean
on_handle_press(GtkWidget *handle, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1)
        return FALSE;
    GtkWidget *card = g_object_get_data(G_OBJECT(handle), "task-card");
    gint64 id = card != NULL ? card_task_id(card) : 0;
    if (id == 0)
        return FALSE;

    gint cx = 0, cy = 0;
    gtk_widget_translate_coordinates(handle, card, (gint)ev->x, (gint)ev->y,
                                     &cx, &cy);
    lw->card_armed       = TRUE;
    lw->card_drag_src    = card;
    lw->card_drag_handle = handle;
    lw->card_drag_id     = id;
    lw->card_press_rx    = ev->x_root;
    lw->card_press_ry    = ev->y_root;
    lw->card_hot_x       = cx;
    lw->card_hot_y       = cy;
    return FALSE;                    /* let the card select as well        */
}


/* ---------------------------------------------------------------------------
 * The board's selection: a SET of task ids, mirroring the task view's
 * GTK_SELECTION_MULTIPLE.
 *
 * Order is never stored.  Every consumer that needs a sequence takes it
 * from the board's DISPLAY order (card_sel_ids), so a multi-selection acts
 * top-to-bottom, lane by lane, the way it looks on screen.
 * ------------------------------------------------------------------------- */

/* card_sel_has() — is `id` selected?                                       */
static gboolean
card_sel_has(TaskLibrary *lw, gint64 id)
{
    return lw->kanban_sel != NULL &&
           g_hash_table_contains(lw->kanban_sel,
                                 GSIZE_TO_POINTER((gsize)id));
}

static void
card_sel_add(TaskLibrary *lw, gint64 id)
{
    if (id != 0)
        g_hash_table_add(lw->kanban_sel, GSIZE_TO_POINTER((gsize)id));
}

static void
card_sel_remove(TaskLibrary *lw, gint64 id)
{
    g_hash_table_remove(lw->kanban_sel, GSIZE_TO_POINTER((gsize)id));
}

static guint
card_sel_count(TaskLibrary *lw)
{
    return lw->kanban_sel != NULL ? g_hash_table_size(lw->kanban_sel) : 0;
}

/* ---------------------------------------------------------------------------
 * card_restyle() — paint the selection onto the cards.
 *
 * Runs IN PLACE rather than through a refresh: a refresh here would
 * destroy the very widget a drag is about to start from, and the click
 * would never become one.
 * ------------------------------------------------------------------------- */
static void
card_restyle(TaskLibrary *lw)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (lw->kanban_lanes[s] == NULL)
            continue;
        GList *kids =
            gtk_container_get_children(GTK_CONTAINER(lw->kanban_lanes[s]));
        for (GList *k = kids; k != NULL; k = k->next) {
            GtkWidget *card = GTK_WIDGET(k->data);
            gint64 id = card_task_id(card);
            if (id == 0)
                continue;            /* the marker / empty placeholder      */
            GtkStyleContext *sc = gtk_widget_get_style_context(card);
            if (card_sel_has(lw, id))
                gtk_style_context_add_class(sc, "task-card-selected");
            else
                gtk_style_context_remove_class(sc, "task-card-selected");
        }
        g_list_free(kids);
    }
}

/* card_select() — collapse the selection to just `id`.                     */
static void
card_select(TaskLibrary *lw, gint64 id)
{
    g_hash_table_remove_all(lw->kanban_sel);
    card_sel_add(lw, id);
    lw->kanban_anchor = id;
    card_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * card_sel_ids() — the selected ids in BOARD DISPLAY ORDER (lane by lane,
 * top to bottom), skipping any whose card is no longer on screen.
 *
 * Display order rather than hash order so a bulk action reads the way the
 * board looks — and so a multi-card drag keeps the cards' relative order
 * when it re-inserts them.  Free with g_array_unref.
 * ------------------------------------------------------------------------- */
static GArray *
card_sel_ids(TaskLibrary *lw)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GArray *lane = lane_card_ids(lw, s);
        for (guint i = 0; i < lane->len; i++) {
            gint64 id = g_array_index(lane, gint64, i);
            if (card_sel_has(lw, id))
                g_array_append_val(out, id);
        }
        g_array_unref(lane);
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * card_sel_range() — select from the shift anchor to `id`.
 *
 * WITHIN ONE LANE only.  A run down a column is the obvious meaning of
 * shift-click on a board; "everything between" two cards in DIFFERENT
 * columns is not, and would quietly select a screenful.  So a cross-lane
 * shift-click behaves like a modify-click and just adds the card, which is
 * the least surprising thing that is still useful.
 * ------------------------------------------------------------------------- */
static void
card_sel_range(TaskLibrary *lw, gint64 id)
{
    gint64 anchor = lw->kanban_anchor;
    if (anchor == 0 || anchor == id) {
        card_sel_add(lw, id);
        card_restyle(lw);
        return;
    }
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GArray *lane = lane_card_ids(lw, s);
        gint ai = -1, bi = -1;
        for (guint i = 0; i < lane->len; i++) {
            gint64 v = g_array_index(lane, gint64, i);
            if (v == anchor) ai = (gint)i;
            if (v == id)     bi = (gint)i;
        }
        if (ai >= 0 && bi >= 0) {    /* both in THIS lane: take the run    */
            gint lo = MIN(ai, bi), hi = MAX(ai, bi);
            for (gint i = lo; i <= hi; i++)
                card_sel_add(lw, g_array_index(lane, gint64, (guint)i));
            g_array_unref(lane);
            card_restyle(lw);
            return;
        }
        g_array_unref(lane);
    }
    card_sel_add(lw, id);            /* different lanes: just add it        */
    card_restyle(lw);
}

/* on_card_press() — click SELECTS; double-click opens the editor;
 * right-click raises the shared context menu.  It does NOT arm a drag:
 * that belongs to the ⠿ grip alone (on_handle_press), so the card body
 * behaves like an ordinary clickable row.  Returns FALSE on the first
 * click of a double so GTK still delivers the second.                     */
static gboolean
on_card_press(GtkWidget *card, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    gint64 id = card_task_id(card);
    if (id == 0)
        return FALSE;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        card_drag_stop(lw);          /* the first press armed one          */
        task_editor_open(lw->app, id);
        return TRUE;
    }
    /* ---- selection -----------------------------------------------------
     * The two modifiers come from GTK, not hardcoded: MODIFY_SELECTION is
     * Ctrl on X11 and Cmd on quartz, and asking the widget is the only way
     * to be right on both.
     *
     * A RIGHT-click inside an existing selection LEAVES IT ALONE, so a
     * bulk action can be reached from any of the selected cards — the same
     * rule the task view follows.  Outside it, it collapses first.        */
    GdkModifierType mod_mask = gtk_widget_get_modifier_mask(card,
        GDK_MODIFIER_INTENT_MODIFY_SELECTION);
    GdkModifierType ext_mask = gtk_widget_get_modifier_mask(card,
        GDK_MODIFIER_INTENT_EXTEND_SELECTION);
    gboolean modify = (ev->state & mod_mask) != 0;
    gboolean extend = (ev->state & ext_mask) != 0;

    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        if (!card_sel_has(lw, id))
            card_select(lw, id);
        /* The SAME menu the list view's rows show, from the same function
         * against the same selection — so a multi-selection gets the
         * multi variant ("Delete 3 Tasks") for free.  Anchored to
         * kanban_box, never the card: an attached menu dies with its
         * widget, and every action here refreshes the board and destroys
         * the card underneath it.                                        */
        return task_context_menu_popup(lw, lw->kanban_box, ev);
    }

    if (ev->type == GDK_BUTTON_PRESS) {
        if (extend) {
            card_sel_range(lw, id);
        } else if (modify) {
            if (card_sel_has(lw, id))
                card_sel_remove(lw, id);
            else
                card_sel_add(lw, id);
            lw->kanban_anchor = id;
            card_restyle(lw);
        } else {
            /* A plain click INSIDE the selection keeps it: that is what
             * lets a multi-card drag start from any of its cards.  The
             * collapse happens on release instead (on_card_release), only
             * when no drag took place.
             *
             * The ANCHOR moves either way.  It has to: it means "the last
             * card plainly clicked", and a shift-click straight after this
             * one must measure its run from HERE.  Tying it to the
             * collapse instead left it pointing at a card the user last
             * touched several clicks ago, and the run came out wrong.    */
            if (!card_sel_has(lw, id))
                card_select(lw, id);
            else
                lw->kanban_anchor = id;
        }
    }

    return FALSE;
}

/* ---------------------------------------------------------------------------
 * The hand-rolled card drag.
 *
 * card_drag_stop() is the ONE way out — release, Escape, a broken grab
 * and window teardown all funnel through it, so the grab can never be
 * left held and the ghost can never be orphaned.
 * ------------------------------------------------------------------------- */

/* card_lane_at_root() — which lane's drop box contains this ROOT point,
 * or -1.  Root coordinates because the pointer spends the drag over other
 * widgets, and every lane box is realized, so each has a window origin to
 * measure from.                                                            */
static gint
card_lane_at_root(TaskLibrary *lw, gint rx, gint ry)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->kanban_drops[s];
        if (box == NULL || !gtk_widget_get_mapped(box))
            continue;
        GdkWindow *win = gtk_widget_get_window(box);
        if (win == NULL)
            continue;
        gint ox, oy;
        gdk_window_get_origin(win, &ox, &oy);
        GtkAllocation a;
        gtk_widget_get_allocation(box, &a);
        if (rx >= ox && rx < ox + a.width && ry >= oy && ry < oy + a.height)
            return s;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * on_ghost_draw() — paint the ghost: the snapshot, at CARD_GHOST_ALPHA.
 *
 * The window is app-paintable and draws NOTHING else, which is what keeps
 * the theme's own window background from showing as a grey plate around
 * the card.  On a composited screen the surface is cleared to fully
 * transparent first (OPERATOR_SOURCE, so it replaces rather than blends)
 * and the card painted over it with alpha, giving real see-through.
 * Without a compositor that clear would land as BLACK, so there the card
 * is painted opaque instead — a solid card that follows the pointer,
 * which is the honest degradation rather than a black rectangle.
 * ------------------------------------------------------------------------- */
static gboolean
on_ghost_draw(GtkWidget *ghost, cairo_t *cr, gpointer data)
{
    (void)data;
    cairo_surface_t *surf = g_object_get_data(G_OBJECT(ghost), "task-surface");
    if (surf == NULL)
        return FALSE;
    gboolean composited = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(ghost), "task-composited"));
    if (composited) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint_with_alpha(cr, composited ? CARD_GHOST_ALPHA : 1.0);

    /* More than one card in flight?  Say so ON the ghost.  A snapshot of
     * the gripped card alone would claim a single-card move, and the drop
     * is about to touch several.                                          */
    gint n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ghost), "task-n"));
    if (n > 1) {
        gchar *txt = g_strdup_printf("%d", n);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, txt, &ext);
        gdouble pad = 5.0;
        gdouble w = ext.width + pad * 2, h = 18.0;
        gint aw = gtk_widget_get_allocated_width(ghost);
        gdouble bx = aw - w - 4.0, by = 4.0;
        cairo_set_source_rgba(cr, 0.18, 0.36, 0.75, 0.95);
        cairo_rectangle(cr, bx, by, w, h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, bx + pad - ext.x_bearing,
                      by + (h - ext.height) / 2 - ext.y_bearing);
        cairo_show_text(cr, txt);
        g_free(txt);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * card_ghost_new() — a translucent copy of `card` in a popup window.
 *
 * The snapshot is drawn into a surface made from the card's OWN window, so
 * it inherits the display's scale factor and stays sharp on HiDPI.
 *
 * Translucency is a PAINTED alpha on an RGBA visual, not
 * gtk_widget_set_opacity: window opacity is a compositor feature that
 * several X11 setups (and quartz popups) quietly ignore, which is exactly
 * how this shipped opaque the first time.  Painting it ourselves also
 * means the window has no background of its own to leak round the edges.
 * ------------------------------------------------------------------------- */
static GtkWidget *
card_ghost_new(GtkWidget *card, gint n_moving)
{
    GtkAllocation a;
    gtk_widget_get_allocation(card, &a);
    GdkWindow *cw = gtk_widget_get_window(card);
    if (cw == NULL || a.width <= 0 || a.height <= 0)
        return NULL;

    cairo_surface_t *surf = gdk_window_create_similar_surface(
        cw, CAIRO_CONTENT_COLOR_ALPHA, a.width, a.height);
    cairo_t *cr = cairo_create(surf);
    gtk_widget_draw(card, cr);
    cairo_destroy(cr);

    GtkWidget *ghost = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(ghost), GDK_WINDOW_TYPE_HINT_DND);
    gtk_widget_set_app_paintable(ghost, TRUE);   /* no theme background   */

    /* An RGBA visual is what makes per-pixel alpha possible at all; a
     * screen with no compositor running cannot honor it, and the draw
     * handler falls back to opaque rather than painting onto black.      */
    GdkScreen *screen = gtk_widget_get_screen(ghost);
    GdkVisual *rgba   = gdk_screen_get_rgba_visual(screen);
    gboolean composited = (rgba != NULL && gdk_screen_is_composited(screen));
    if (composited)
        gtk_widget_set_visual(ghost, rgba);

    g_object_set_data_full(G_OBJECT(ghost), "task-surface", surf,
                           (GDestroyNotify)cairo_surface_destroy);
    g_object_set_data(G_OBJECT(ghost), "task-composited",
                      GINT_TO_POINTER(composited));
    g_object_set_data(G_OBJECT(ghost), "task-n",
                      GINT_TO_POINTER(n_moving));
    g_signal_connect(ghost, "draw", G_CALLBACK(on_ghost_draw), NULL);
    gtk_widget_set_size_request(ghost, a.width, a.height);
    gtk_widget_show(ghost);
    return ghost;
}

/* card_drag_stop() — end a drag (or a merely armed press) and put
 * everything back.  Safe to call when nothing is in flight.               */
static void
card_drag_stop(TaskLibrary *lw)
{
    if (lw->card_dragging) {
        GdkDisplay *dpy = gtk_widget_get_display(lw->window);
        gdk_seat_ungrab(gdk_display_get_default_seat(dpy));
        /* Put the window cursors back.  The card keeps the OPEN hand (the
         * pointer may still be over it); the toplevel goes back to its
         * default so every other widget inherits normally again.          */
        card_set_cursor(lw->window, NULL);
        /* The grip keeps the OPEN hand (the pointer may still be over it);
         * the card loses its dimming.  Two different widgets, so two
         * different restorations.                                         */
        if (lw->card_drag_handle != NULL)
            card_set_cursor(lw->card_drag_handle,
                            card_cursor(lw->card_drag_handle,
                                        &lw->card_grab, "grab"));
        if (lw->card_drag_src != NULL)
            gtk_style_context_remove_class(
                gtk_widget_get_style_context(lw->card_drag_src),
                "task-card-dragging");
        card_lane_highlight(lw, -1);
        card_mark_clear(lw);
    }
    if (lw->card_key_handler != 0) {
        g_signal_handler_disconnect(lw->window, lw->card_key_handler);
        lw->card_key_handler = 0;
    }
    g_clear_pointer(&lw->card_ghost, gtk_widget_destroy);
    lw->card_dragging    = FALSE;
    lw->card_armed       = FALSE;
    lw->card_drag_src    = NULL;
    lw->card_drag_handle = NULL;
    lw->card_drag_id     = 0;
}

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

/* kanban_order_key() — the current view's card-order key, or NULL for a
 * view that has no board (the forecast).  New string (g_free).            */
static gchar *
kanban_order_key(TaskLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_LIST)
        return g_strdup_printf("kanban_order_list_%" G_GINT64_FORMAT,
                               lw->sel_id);
    return task_view_order_key(sel_view(lw), "kanban_order");
}

/* ---------------------------------------------------------------------------
 * kanban_order_apply() — reorder `tasks` in place to match the saved
 * order: saved ids first in their saved sequence, then anything the saved
 * list does not mention (a task created since) appended in query order.
 *
 * The same shape as task_view_apply_manual_order, and forgiving in the
 * same way: an id that no longer exists simply matches nothing.
 * ------------------------------------------------------------------------- */
static void
kanban_order_apply(TaskLibrary *lw, GPtrArray *tasks)
{
    gchar *key = kanban_order_key(lw);
    if (key == NULL)
        return;
    gchar *saved = task_app_config_get(key);
    g_free(key);
    if (saved == NULL || *saved == '\0' || tasks->len < 2) {
        g_free(saved);
        return;
    }

    GPtrArray *out    = g_ptr_array_sized_new(tasks->len);
    gboolean  *placed = g_new0(gboolean, tasks->len);
    gchar    **parts  = g_strsplit(saved, ",", -1);
    g_free(saved);
    for (gint i = 0; parts[i] != NULL; i++) {
        gint64 id = g_ascii_strtoll(parts[i], NULL, 10);
        if (id == 0)
            continue;
        for (guint j = 0; j < tasks->len; j++) {
            Task *t = g_ptr_array_index(tasks, j);
            if (!placed[j] && t->id == id) {
                g_ptr_array_add(out, t);
                placed[j] = TRUE;
                break;
            }
        }
    }
    g_strfreev(parts);
    for (guint j = 0; j < tasks->len; j++)
        if (!placed[j])
            g_ptr_array_add(out, g_ptr_array_index(tasks, j));
    g_free(placed);

    /* Same elements, new sequence — the array does not own the tasks, so
     * this is a pure permutation and nothing is freed.                     */
    for (guint j = 0; j < tasks->len; j++)
        tasks->pdata[j] = out->pdata[j];
    g_ptr_array_free(out, TRUE);
}

/* lane_card_ids() — the task ids currently shown in lane `s`, in display
 * order, skipping the marker and the empty-lane placeholder (neither
 * carries a task id).  Free with g_array_unref.                           */
static GArray *
lane_card_ids(TaskLibrary *lw, gint s)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (lw->kanban_lanes[s] == NULL)
        return ids;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->kanban_lanes[s]));
    for (GList *k = kids; k != NULL; k = k->next) {
        gint64 id = card_task_id(GTK_WIDGET(k->data));
        if (id != 0)
            g_array_append_val(ids, id);
    }
    g_list_free(kids);
    return ids;
}

/* ---------------------------------------------------------------------------
 * card_slot_at() — which SLOT in lane `s` the pointer at root-y `ry` is
 * pointing at: 0 before the first card, n after the last.
 *
 * Measured against each card's vertical MIDPOINT, and the dragged card is
 * counted like any other so the slot it already occupies is reachable
 * (that is what makes "put it back" a no-op rather than a move).  The
 * marker carries no task id and is skipped.
 * ------------------------------------------------------------------------- */
static gint
card_slot_at(TaskLibrary *lw, gint s, gint ry)
{
    gint slot = 0;
    if (lw->kanban_lanes[s] == NULL)
        return 0;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->kanban_lanes[s]));
    for (GList *k = kids; k != NULL; k = k->next) {
        GtkWidget *w = GTK_WIDGET(k->data);
        if (card_task_id(w) == 0)
            continue;                /* marker / placeholder               */
        GdkWindow *win = gtk_widget_get_window(w);
        if (win == NULL)
            continue;
        gint ox, oy;
        gdk_window_get_origin(win, &ox, &oy);
        (void)ox;
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        if (ry < oy + a.height / 2)
            break;                   /* above this card's middle           */
        slot++;
    }
    g_list_free(kids);
    return slot;
}

/* card_mark_clear() — take the insertion marker off screen.                */
static void
card_mark_clear(TaskLibrary *lw)
{
    g_clear_pointer(&lw->card_mark, gtk_widget_destroy);
    lw->card_mark_lane = -1;
    lw->card_mark_slot = -1;
}

/* ---------------------------------------------------------------------------
 * card_mark_place() — show the insertion marker at (lane, slot).
 *
 * Rebuilt on a CHANGE only, never per motion event: the marker takes up
 * room in the lane, so re-inserting it on every event would shuffle the
 * cards under the pointer continuously.  Rebuilding rather than
 * reparenting keeps the ref juggling out of it — gtk_container_remove
 * would drop the last reference and destroy the thing we meant to move.
 * ------------------------------------------------------------------------- */
static void
card_mark_place(TaskLibrary *lw, gint lane, gint slot)
{
    if (lane == lw->card_mark_lane && slot == lw->card_mark_slot)
        return;
    card_mark_clear(lw);
    if (lane < 0 || lane >= TASK_STATUS_N_VALUES ||
        lw->kanban_lanes[lane] == NULL)
        return;

    GtkWidget *mark = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mark),
                                "task-card-mark");
    gtk_widget_set_size_request(mark, -1, CARD_MARK_H);
    gtk_box_pack_start(GTK_BOX(lw->kanban_lanes[lane]), mark,
                       FALSE, FALSE, 0);
    /* Translate the CARD slot into a child index: the placeholder label
     * of an empty lane is a child too, so count real cards.               */
    gint child_idx = 0, seen = 0;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->kanban_lanes[lane]));
    for (GList *k = kids; k != NULL; k = k->next, child_idx++) {
        GtkWidget *w = GTK_WIDGET(k->data);
        if (w == mark)
            continue;
        if (card_task_id(w) != 0) {
            if (seen == slot)
                break;
            seen++;
        }
    }
    g_list_free(kids);
    gtk_box_reorder_child(GTK_BOX(lw->kanban_lanes[lane]), mark, child_idx);
    gtk_widget_show(mark);

    lw->card_mark      = mark;
    lw->card_mark_lane = lane;
    lw->card_mark_slot = slot;
}

/* card_lane_highlight() — mark the lane the card would land in, and only
 * that one.  `lane` of -1 clears every highlight (pointer outside the
 * board, or the drag ending).                                             */
static void
card_lane_highlight(TaskLibrary *lw, gint lane)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->kanban_drops[s];
        if (box == NULL)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(box);
        if (s == lane)
            gtk_style_context_add_class(sc, "task-lane-target");
        else
            gtk_style_context_remove_class(sc, "task-lane-target");
    }
}

/* card_drag_move() — put the ghost under the pointer, offset so the card
 * stays gripped where it was picked up, and light up the lane it would
 * land in so the drop is never a guess.                                   */
static void
card_drag_move(TaskLibrary *lw, gint rx, gint ry)
{
    if (lw->card_ghost != NULL)
        gtk_window_move(GTK_WINDOW(lw->card_ghost),
                        rx - lw->card_hot_x, ry - lw->card_hot_y);
    gint lane = card_lane_at_root(lw, rx, ry);
    card_lane_highlight(lw, lane);
    /* The marker is placed BEFORE the slot is read back at drop time, so
     * what the user sees is exactly what the release will do.             */
    card_mark_place(lw, lane, lane >= 0 ? card_slot_at(lw, lane, ry) : -1);
}

/* on_card_drag_key() — Escape abandons the drag, changing nothing.        */
static gboolean
on_card_drag_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    if (ev->keyval != GDK_KEY_Escape)
        return FALSE;
    card_drag_stop(lw);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * card_order_save() — the ORDER half of a board drop: rewrite the view's
 * kanban_order_<id> key with every lane's cards in display order, the
 * dragged ids lifted out of wherever they were and re-inserted into `lane`
 * at `slot` with their relative order intact.  Removing before inserting is
 * what makes `slot` — measured against the cards on screen, the dragged
 * ones included — land where the marker was.
 *
 * Returns TRUE when the saved order actually changed.
 *
 * REFUSES OUTRIGHT while a search is up, and that is the point of it being
 * separate: lane_card_ids reads the cards ON SCREEN, which under a filter
 * is only the matches, so writing that back would drop every hidden task
 * out of the saved order for good.  Same trap as the list view's drag, and
 * manual_sort_live carries the full reasoning.  The caller's STATUS half
 * still runs — dragging a card to Done while searching is a perfectly good
 * thing to do and loses nothing.
 *   lw      — the library window.
 *   to_move — the dragged task ids, in the order they should land.
 *   lane    — the destination lane (its index IS the TaskStatus).
 *   slot    — the position within that lane.
 * ------------------------------------------------------------------------- */
static gboolean
card_order_save(TaskLibrary *lw, GArray *to_move, gint lane, gint slot)
{
    if (lw->search != NULL)
        return FALSE;                /* filtered: not ours to rewrite      */

    GString *order = g_string_new(NULL);
    for (gint sl = 0; sl < TASK_STATUS_N_VALUES; sl++) {
        GArray *ids = lane_card_ids(lw, sl);
        for (guint m = 0; m < to_move->len; m++) {
            gint64 id = g_array_index(to_move, gint64, m);
            for (guint i = 0; i < ids->len; i++)
                if (g_array_index(ids, gint64, i) == id) {
                    g_array_remove_index(ids, i);
                    break;
                }
        }
        if (sl == lane) {
            gint at = CLAMP(slot, 0, (gint)ids->len);
            for (guint m = 0; m < to_move->len; m++) {
                gint64 id = g_array_index(to_move, gint64, m);
                g_array_insert_val(ids, at + (gint)m, id);
            }
        }
        for (guint i = 0; i < ids->len; i++) {
            if (order->len > 0)
                g_string_append_c(order, ',');
            g_string_append_printf(order, "%" G_GINT64_FORMAT,
                                   g_array_index(ids, gint64, i));
        }
        g_array_unref(ids);
    }

    gchar *key   = kanban_order_key(lw);
    gchar *saved = key != NULL ? task_app_config_get(key) : NULL;
    gboolean changed = (g_strcmp0(saved, order->str) != 0);
    g_free(saved);
    if (changed && key != NULL)
        task_app_config_set(key, order->str);
    g_free(key);
    g_string_free(order, TRUE);
    return changed;
}

/* ---------------------------------------------------------------------------
 * card_drop_apply() — the drop: put the dragged task(s) in `lane` at
 * `slot`, keeping their relative order.  `moving` is the whole dragged
 * selection, so one card and twenty take the same path.
 *
 * Two independent halves, either of which may be a no-op:
 *
 *   the STATUS, when the lane changed — a real database write that stamps
 *     updated_at and syncs;
 *   the ORDER, always — local-only, config, never touches the row, and
 *     handed to card_order_save, which refuses it while a search is up.
 *
 * A drag that lands the cards exactly where they already were does
 * NEITHER, which is what keeps "pick up and put back" from buying a sync
 * round trip.  Returns TRUE when anything changed (so the caller
 * refreshes).
 * ------------------------------------------------------------------------- */
static gboolean
card_drop_apply(TaskLibrary *lw, GArray *moving, gint lane, gint slot)
{
    if (moving == NULL || moving->len == 0 ||
        lane < 0 || lane >= TASK_STATUS_N_VALUES)
        return FALSE;
    TaskStatus want = (TaskStatus)lane;

    /* Which of the dragged tasks actually need a status write?  A
     * multi-card drag routinely mixes lanes, and only the ones arriving
     * from elsewhere are a real change.                                   */
    GArray *to_move = g_array_new(FALSE, FALSE, sizeof(gint64));
    gchar  *one_title = NULL;        /* for the single-task status message */
    guint   n_status  = 0;
    for (guint i = 0; i < moving->len; i++) {
        gint64 id = g_array_index(moving, gint64, i);
        Task *t = task_db_task_get(lw->app->db, id);
        if (t == NULL || t->deleted) {
            task_free(t);
            continue;
        }
        g_array_append_val(to_move, id);
        if (t->status != want) {
            n_status++;
            if (one_title == NULL)
                one_title = g_strdup(t->title);
        }
        task_free(t);
    }
    if (to_move->len == 0) {
        g_array_unref(to_move);
        g_free(one_title);
        return FALSE;
    }

    gboolean order_change = card_order_save(lw, to_move, lane, slot);

    if (n_status == 0 && !order_change) {
        g_array_unref(to_move);
        g_free(one_title);
        return FALSE;                /* put back exactly where it was      */
    }

    for (guint i = 0; i < to_move->len; i++)
        task_db_task_set_status(lw->app->db,
                                g_array_index(to_move, gint64, i), want);

    /* Keep the moved cards selected across the rebuild.                   */
    g_hash_table_remove_all(lw->kanban_sel);
    for (guint i = 0; i < to_move->len; i++)
        card_sel_add(lw, g_array_index(to_move, gint64, i));
    lw->kanban_anchor = g_array_index(to_move, gint64, 0);

    /* Only announce a STATUS move: a reorder is its own feedback (the
     * cards are visibly somewhere else) and would otherwise spam the
     * status bar for every nudge within a lane.                           */
    if (n_status == 1 && one_title != NULL)
        task_app_status(lw->app, "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 %s",
                        *one_title != '\0' ? one_title : "Untitled Task",
                        task_status_label(want));
    else if (n_status > 1)
        task_app_status(lw->app, "%u tasks \xe2\x80\x94 %s", n_status,
                        task_status_label(want));
    g_free(one_title);
    g_array_unref(to_move);
    return TRUE;
}

/* card_refresh_idle() — rebuild the board from an idle callback.
 *
 * Deferred deliberately: the drop happens inside the dragged CARD's own
 * event handler, and full_refresh destroys every card including that one,
 * so refreshing inline would return into a freed widget.  Re-resolves the
 * library (the window may close first) rather than capturing it, the same
 * rule every async callback here follows.                                 */
static gboolean
card_refresh_idle(gpointer data)
{
    TaskLibrary *lw = lib_of(data);
    if (lw != NULL)
        full_refresh(lw);
    return G_SOURCE_REMOVE;
}

/* on_card_motion() — start the drag once the pointer has travelled far
 * enough, then track it.  Connected to the ⠿ GRIP, so `w` is the grip and
 * the grab lands on its window — which is what makes the grip the only
 * place a drag can begin.                                                  */
static gboolean
on_card_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (!lw->card_armed && !lw->card_dragging)
        return FALSE;

    if (!lw->card_dragging) {
        if (!gtk_drag_check_threshold(w,
                (gint)lw->card_press_rx, (gint)lw->card_press_ry,
                (gint)ev->x_root, (gint)ev->y_root))
            return FALSE;            /* still just a click                  */

        /* The ghost is a picture of the whole CARD; the grab goes on the
         * GRIP, which is the window the press came from and therefore the
         * one motion and release will be delivered to.                    */
        GtkWidget *card = lw->card_drag_src;
        /* How many cards this drag will move: the whole selection when
         * the gripped card is in it, else just the one.                    */
        gint n_moving = card_sel_has(lw, lw->card_drag_id)
                        ? (gint)card_sel_count(lw) : 1;
        lw->card_ghost = card_ghost_new(card, n_moving);
        GdkDisplay *dpy  = gtk_widget_get_display(w);
        GdkSeat    *seat = gdk_display_get_default_seat(dpy);
        GdkCursor  *grabbing =
            card_cursor(w, &lw->card_grabbing, "grabbing");
        if (gdk_seat_grab(seat, gtk_widget_get_window(w),
                          GDK_SEAT_CAPABILITY_ALL_POINTING, FALSE,
                          grabbing, (GdkEvent *)ev, NULL,
                          NULL) != GDK_GRAB_SUCCESS) {
            card_drag_stop(lw);      /* no grab: stay a click               */
            return FALSE;
        }
        /* Belt AND braces on the closed hand.  The grab's cursor argument
         * is the portable lever and is what X11 honors; some backends
         * apply the CURSOR OF THE WINDOW the pointer is over instead, so
         * the same cursor goes on the grip and on the toplevel as well.
         * Setting all three costs nothing and leaves no backend showing
         * an arrow mid-drag.  card_drag_stop puts them all back.          */
        card_set_cursor(w, grabbing);
        card_set_cursor(lw->window, grabbing);
        /* Dim the original in place — the ghost is the one moving.  It is
         * NOT hidden: its GdkWindow is the grab window, and unmapping
         * that would break the grab and end the drag on the spot.        */
        gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                    "task-card-dragging");
        lw->card_mark_lane = -1;     /* force the first placement          */
        lw->card_mark_slot = -1;
        lw->card_dragging  = TRUE;
        lw->card_key_handler =
            g_signal_connect(lw->window, "key-press-event",
                             G_CALLBACK(on_card_drag_key), lw);
    }
    card_drag_move(lw, (gint)ev->x_root, (gint)ev->y_root);
    return TRUE;
}

/* on_card_release() — drop: whichever lane the pointer is over wins.      */
static gboolean
on_card_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (!lw->card_dragging) {
        /* A plain click that never became a drag.  The PRESS deliberately
         * left an existing multi-selection alone (so a drag could start
         * from any of its cards); now that we know it was only a click,
         * collapse to the clicked card — unless a modifier was held, which
         * means the press already did the right thing.                    */
        if (lw->card_armed && lw->card_drag_id != 0 && ev->button == 1) {
            GdkModifierType mod = gtk_widget_get_modifier_mask(w,
                GDK_MODIFIER_INTENT_MODIFY_SELECTION);
            GdkModifierType ext = gtk_widget_get_modifier_mask(w,
                GDK_MODIFIER_INTENT_EXTEND_SELECTION);
            if ((ev->state & (mod | ext)) == 0 && card_sel_count(lw) > 1)
                card_select(lw, lw->card_drag_id);
        }
        lw->card_armed = FALSE;
        return FALSE;
    }
    gint   lane = card_lane_at_root(lw, (gint)ev->x_root, (gint)ev->y_root);
    /* Read the slot from the MARKER, not by re-measuring: the marker is
     * what the user was looking at, and re-measuring now would answer
     * against a lane whose geometry the marker itself has shifted.        */
    gint   slot = (lane >= 0 && lane == lw->card_mark_lane)
                  ? lw->card_mark_slot
                  : (lane >= 0 ? card_slot_at(lw, lane, (gint)ev->y_root)
                               : -1);
    /* WHAT moves: the whole selection when the gripped card is part of it,
     * otherwise just that card.  Snapshot it BEFORE card_drag_stop, which
     * clears the drag state.                                              */
    GArray *moving = card_sel_ids(lw);
    if (moving->len == 0 ||
        !card_sel_has(lw, lw->card_drag_id)) {
        g_array_set_size(moving, 0);
        g_array_append_val(moving, lw->card_drag_id);
    }
    card_drag_stop(lw);              /* ungrab BEFORE touching the model   */
    if (card_drop_apply(lw, moving, lane, slot))
        g_idle_add(card_refresh_idle, lw->app);
    g_array_unref(moving);
    return TRUE;
}

/* on_card_grab_broken() — the compositor or another grab took the pointer
 * away mid-drag; abandon quietly rather than leaving a ghost on screen.   */
static gboolean
on_card_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev, gpointer data)
{
    (void)w; (void)ev;
    card_drag_stop(data);
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * kanban_card_new() — one task as a card: the same Pango markup the list
 * rows and the forecast use (so a task reads identically in all three
 * views), wrapped in an event box that can be clicked and dragged.
 * ------------------------------------------------------------------------- */
static GtkWidget *
kanban_card_new(TaskLibrary *lw, const Task *t, const gchar *markup,
                gboolean selected)
{
    GtkWidget *card = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(card), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "task-card");
    if (selected)
        gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                    "task-card-selected");
    g_object_set_data(G_OBJECT(card), "task-task-id",
                      GSIZE_TO_POINTER((gsize)t->id));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(card), row);

    /* The ⠿ GRIP.  Its own event box, because a different cursor needs a
     * different GdkWindow — and because it is the only place a drag may
     * start from, exactly like the list view's handle column.  The glyph
     * is dimmed with Pango ALPHA, never a fixed gray: a gray stays gray
     * on the selection tint and goes unreadable.                          */
    GtkWidget *handle = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(handle),
                                "task-card-handle");
    GtkWidget *grip = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(grip),
                         "<span alpha=\"55%\">\xe2\xa0\xbf</span>");
    gtk_widget_set_margin_start(grip, CARD_GRIP_PAD);
    gtk_widget_set_margin_end(grip, CARD_GRIP_PAD);
    gtk_container_add(GTK_CONTAINER(handle), grip);
    gtk_box_pack_start(GTK_BOX(row), handle, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    /* A lane is a third of the pane; without this a long unbroken title
     * would set the card's natural width and push the board wider than
     * the (horizontally unscrollable) viewport.                           */
    gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
    pad_widget(label, CARD_PAD);     /* text off the card's border        */
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);

    /* The CARD takes clicks: select, double-click to open, right-click for
     * the context menu.  It gets NO cursor, so the pointer stays the
     * ordinary arrow over the text.                                       */
    gtk_widget_add_events(card, GDK_BUTTON_PRESS_MASK |
                                GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(card, "button-press-event",
                     G_CALLBACK(on_card_press), lw);
    /* Release on the card too, not just the grip: press the grip, drift a
     * couple of pixels onto the text, let go — without a grab that release
     * lands HERE, and the armed flag would otherwise be left set.          */
    g_signal_connect(card, "button-release-event",
                     G_CALLBACK(on_card_release), lw);

    /* The GRIP takes the drag.  Press arms, motion past the threshold
     * starts it, release drops.  Its press handler returns FALSE so the
     * card still sees it and selects — clicking the grip selects too.
     * "realize" rather than a one-off call: there is no GdkWindow to put a
     * cursor on until then, which happens after refresh_kanban's show_all
     * (and not at all while the board is hidden).  The CLOSED hand comes
     * from the pointer grab in on_card_motion.                            */
    gtk_widget_add_events(handle, GDK_BUTTON_PRESS_MASK |
                                  GDK_BUTTON_RELEASE_MASK |
                                  GDK_BUTTON1_MOTION_MASK);
    g_object_set_data(G_OBJECT(handle), "task-card", card);
    g_signal_connect(handle, "button-press-event",
                     G_CALLBACK(on_handle_press), lw);
    g_signal_connect(handle, "motion-notify-event",
                     G_CALLBACK(on_card_motion), lw);
    g_signal_connect(handle, "button-release-event",
                     G_CALLBACK(on_card_release), lw);
    g_signal_connect(handle, "grab-broken-event",
                     G_CALLBACK(on_card_grab_broken), lw);
    g_signal_connect(handle, "realize",
                     G_CALLBACK(on_handle_realize), lw);
    return card;
}

/* ---------------------------------------------------------------------------
 * refresh_kanban() — rebuild the board from `tasks` (already collected for
 * the current view by refresh_tasks, so every view that has a task list
 * can be shown as a board).  Returns the number of cards placed.
 *
 * The lanes are emptied and refilled per refresh, like the forecast's
 * stores: cards are widgets, so "clear" means destroying the children.
 * ------------------------------------------------------------------------- */
static guint
refresh_kanban(TaskLibrary *lw, GPtrArray *tasks, const TaskRowCtx *ctx)
{
    scroll_keep_queue_win(lw->kanban_box);

    /* The saved slot order, applied before the tasks are handed out to
     * lanes — one list for the whole view, which the status filter below
     * projects onto each lane (see kanban_order_key).                     */
    kanban_order_apply(lw, tasks);

    /* A rebuild destroys the marker along with everything else; drop the
     * dangling pointer so card_mark_place does not reorder freed memory
     * if a refresh lands mid-drag (an editor autosave can do that).       */
    lw->card_mark      = NULL;
    lw->card_mark_lane = -1;
    lw->card_mark_slot = -1;

    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        lane_clear(lw->kanban_lanes[s]);

    /* Selections for tasks that have since vanished must not survive the
     * rebuild — Delete Task would act on a tombstone.  Collect the ones
     * that DID come back and keep only those.                             */
    GHashTable *alive = g_hash_table_new(NULL, NULL);
    guint    per_lane[TASK_STATUS_N_VALUES] = { 0 };
    guint    shown = 0;

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        gboolean done = t->status == TASK_STATUS_DONE;
        /* The completed-visibility toggle applies here exactly as it does
         * to every other view: with completed hidden the Done lane simply
         * empties.  It stays on screen as a drop target, so ticking a task
         * off by dragging still works — and the card vanishing afterwards
         * is the same behavior as the list's fade-out.                     */
        if (!ctx->show_done && done)
            continue;
        gint lane = (gint)t->status;
        if (lane < 0 || lane >= TASK_STATUS_N_VALUES)
            lane = TASK_STATUS_NEW;    /* a status off disk, clamped        */

        GPtrArray *subs = t->parent_id == 0
            ? g_hash_table_lookup(ctx->subs_by_parent,
                                  GINT_TO_POINTER(t->id))
            : NULL;
        const gchar *list_name = ctx->list_names != NULL
            ? g_hash_table_lookup(ctx->list_names,
                                  GINT_TO_POINTER(t->list_id))
            : NULL;
        gint att = GPOINTER_TO_INT(
            g_hash_table_lookup(ctx->att_counts, GINT_TO_POINTER(t->id)));
        gchar *markup = task_rows_desc_markup(t, list_name, att, subs, ctx);
        gboolean selected = card_sel_has(lw, t->id);
        if (selected)
            g_hash_table_add(alive, GSIZE_TO_POINTER((gsize)t->id));
        gtk_box_pack_start(GTK_BOX(lw->kanban_lanes[lane]),
                           kanban_card_new(lw, t, markup, selected),
                           FALSE, FALSE, 0);
        g_free(markup);
        per_lane[lane]++;
        shown++;
    }
    /* Replace the selection with the survivors.                          */
    g_hash_table_remove_all(lw->kanban_sel);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, alive);
    while (g_hash_table_iter_next(&it, &key, NULL))
        g_hash_table_add(lw->kanban_sel, key);
    g_hash_table_destroy(alive);
    if (card_sel_count(lw) == 0)
        lw->kanban_anchor = 0;

    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        gchar *hdr = g_strdup_printf(
            "<b>%s</b>\n<small><span alpha=\"60%%\">%u task%s</span>"
            "</small>", task_status_label((TaskStatus)s), per_lane[s],
            per_lane[s] == 1 ? "" : "s");
        gtk_label_set_markup(GTK_LABEL(lw->kanban_labels[s]), hdr);
        g_free(hdr);

        /* An empty lane still needs to say so — and still needs to be a
         * drop target, which it is: the DEST is the lane box itself, not
         * its cards.                                                       */
        if (per_lane[s] == 0) {
            GtkWidget *empty = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(empty),
                "<i><span alpha=\"55%\">Drop a task here</span></i>");
            gtk_widget_set_margin_top(empty, 10);
            gtk_widget_set_margin_bottom(empty, 10);
            gtk_box_pack_start(GTK_BOX(lw->kanban_lanes[s]), empty,
                               FALSE, FALSE, 0);
        }
        gtk_widget_show_all(lw->kanban_lanes[s]);
    }
    return shown;
}

/* ---------------------------------------------------------------------------
 * kanban_lane_new() — one lane: a heading label over a framed, padded
 * body that holds the cards and accepts drops.  Mirrors
 * forecast_day_section's shape (label + framed body, natural height, no
 * scroller of its own).  Fills lw->kanban_labels / kanban_lanes [status].
 *
 * The drop target is an EVENT BOX wrapping the card box, not the card box
 * itself: a GtkBox is a no-window widget, and a drag destination needs a
 * real GdkWindow to receive the platform's drag events reliably.  The
 * event box is also what paints the lane's tint, for the same reason —
 * a windowless widget has no surface of its own to fill.
 * ------------------------------------------------------------------------- */
static GtkWidget *
kanban_lane_new(TaskLibrary *lw, TaskStatus status)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    lw->kanban_labels[status] = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(lw->kanban_labels[status]),
                          GTK_JUSTIFY_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(lw->kanban_labels[status]),
                            PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(col), lw->kanban_labels[status],
                       FALSE, FALSE, 2);

    GtkWidget *drop = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(drop), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(drop),
                                "task-lane");
    /* Remembered for the drop hit-test: card_lane_at_root measures the
     * pointer's ROOT position against each of these boxes.  No GTK drag
     * destination — the board owns its own drag (see the banner).         */
    lw->kanban_drops[status] = drop;

    /* The cards themselves.  Kept separate from the event box so
     * lane_clear can empty it without disturbing the drop target.          */
    GtkWidget *lane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    pad_widget(lane, LANE_PAD);      /* cards off the lane's frame        */
    gtk_container_add(GTK_CONTAINER(drop), lane);
    lw->kanban_lanes[status] = lane;

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), drop);
    /* expand=TRUE so the frame (and the event box inside it) fills the
     * column's height: a short lane must still be a drop target all the
     * way down, not just behind the cards it happens to hold.              */
    gtk_box_pack_start(GTK_BOX(col), frame, TRUE, TRUE, 0);
    return col;
}

/* ---------------------------------------------------------------------------
 * task_pane_mode_apply() — show exactly ONE of the three task-pane
 * variants.  The single place that answers "which pane is on screen":
 * refresh_tasks calls it, and so does the construction path after
 * show_all has made all three visible at once.
 *
 * The Weekly Forecast OUTRANKS Kanban.  It is its own panel of seven
 * dated day views, not a task list with a layout — there is nothing for
 * a board to lay out, so turning Kanban on does not disturb it, and
 * leaving the forecast puts the board back.
 * ------------------------------------------------------------------------- */
static void
task_pane_mode_apply(TaskLibrary *lw)
{
    const TaskView *view  = sel_view(lw);
    gboolean        panel = task_view_is_panel(view);
    gboolean        kanban = !panel && lw->kanban;
    gtk_widget_set_visible(lw->task_scroll, !panel && !kanban);
    gtk_widget_set_visible(lw->kanban_box,   kanban);
    /* Exactly one panel at most: show the selected view's, hide the rest.
     * Pruned first, so a pane belonging to a view that has just been
     * unregistered is gone rather than merely hidden.                    */
    panels_prune(lw);
    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *v = task_view_nth(i);
        /* Only the SELECTED one is built: asking for the others would
         * construct every panel view's pane just to hide it.             */
        GtkWidget *w = (panel && v == view) ? panel_widget(lw, v)
                                            : g_hash_table_lookup(lw->panels, v);
        if (w != NULL)
            gtk_widget_set_visible(w, panel && v == view);
    }

    /* The sort toggle is INERT while Kanban View is on: the board is
     * always drag-sorted (its own per-lane order, kanban_order_*), and the
     * list view it governs is not reachable at all in that mode.  So grey
     * it out rather than leaving a control that silently does nothing.
     *
     * Keyed on lw->kanban, NOT on `kanban` above: with the board on and
     * the Weekly Forecast selected the list view is still unreachable, and
     * flickering the item's sensitivity as the sidebar selection moves
     * would be worse than a steady "unavailable while Kanban is on".
     *
     * The TOOLBAR twin greys with it — one control in two places, and
     * leaving the button live would let a click change a setting the menu
     * has just declared unavailable.                                       */
    /* The pane controls name the pane a click switches TO, and this is the
     * single place that answers "which pane is on screen" — so both the
     * menu label and the toolbar button's icon are set here rather than in
     * the handler, and a kanban flag changed by any other route still
     * reaches them.  Keyed on lw->kanban like the greying below: the
     * forecast outranks the board without turning it off, so the controls
     * must still offer the way back to the list.
     *
     * The ICON names the action too, the same rule the completed-visibility
     * button follows — and BOTH faces come from menu.png, the bulleted
     * list: upright it offers the list, turned a quarter turn clockwise
     * its bullets sit on top of three vertical bars, which is a board.
     * One image, so the two faces cannot drift apart, and the turn is a
     * whole quarter on the pixbuf so nothing is resampled.  (menu.png is
     * also why the sort button wears neither of its old pictures — two
     * buttons wearing the same image read as one control.)              */
    if (lw->view_kanban_item != NULL)
        gtk_menu_item_set_label(GTK_MENU_ITEM(lw->view_kanban_item),
            lw->kanban ? PANE_LABEL_TO_LIST : PANE_LABEL_TO_KANBAN);
    if (lw->pane_item != NULL) {
        GtkWidget *icon = task_app_icon_image_rotated(lw->app, "menu", 24,
            lw->kanban ? GDK_PIXBUF_ROTATE_NONE
                       : GDK_PIXBUF_ROTATE_CLOCKWISE);
        if (icon != NULL) {
            gtk_widget_show(icon);
            gtk_tool_button_set_icon_widget(
                GTK_TOOL_BUTTON(lw->pane_item), icon);
        }
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(lw->pane_item),
            lw->kanban ? "List" : "Kanban");
        gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->pane_item),
            lw->kanban ? "Show the tasks as a list"
                       : "Show the tasks as a Kanban board");
    }

    /* Two reasons the sort control can be unavailable, and they get
     * DIFFERENT tooltips: one control greyed for two unrelated causes is
     * only honest if it says which one is in force.                       */
    gboolean sortable = !lw->kanban && lw->search == NULL;
    if (lw->view_manual_sort_item != NULL) {
        gtk_widget_set_sensitive(lw->view_manual_sort_item, sortable);
        gtk_widget_set_tooltip_text(lw->view_manual_sort_item,
            sortable      ? NULL
            : lw->kanban  ? "The Kanban board is always drag-sorted \xe2\x80\x94 "
                            "turn Kanban View off to change list sorting"
                          : "A search hides rows, and saving an order from "
                            "a filtered list would lose the hidden tasks' "
                            "places \xe2\x80\x94 clear the search box to "
                            "drag-sort again");
    }
    if (lw->manual_sort_item != NULL)
        gtk_widget_set_sensitive(GTK_WIDGET(lw->manual_sort_item), sortable);

    /* The search box filters the task LIST the core lays out, and a panel
     * view has none — the Weekly Forecast owns its seven day views and
     * what goes in them.  So grey the box out there rather than leave a
     * control that silently does nothing, the same call the sort toggle
     * makes above.  Keyed on `panel`, not on lw->kanban: unlike the board,
     * a panel is only ever up while its own row is selected, so the
     * sensitivity tracks something the user can see.                       */
    if (lw->search_entry != NULL) {
        gtk_widget_set_sensitive(lw->search_entry, !panel);
        gtk_widget_set_tooltip_text(lw->search_entry,
            panel ? "The Weekly Forecast lays out its own days \xe2\x80\x94 "
                    "pick a list or All Tasks to search"
                  : SEARCH_TOOLTIP);
    }
}

/* ---------------------------------------------------------------------------
 * refresh_tasks() — rebuild the task pane for the current selection.
 * The Weekly Forecast has its own panel of seven day views; selecting
 * it swaps that panel in for the regular task list (and back).  With
 * Kanban View on, every OTHER view renders its tasks as a board instead
 * of a list — the collection below is shared, only the presentation
 * differs.
 * ------------------------------------------------------------------------- */
static void
refresh_tasks(TaskLibrary *lw)
{
    const TaskView *panel_view = sel_view(lw);
    if (!task_view_is_panel(panel_view))
        panel_view = NULL;
    gboolean kanban = panel_view == NULL && lw->kanban;
    task_pane_mode_apply(lw);
    if (panel_view != NULL) {
        /* Drop the hidden regular pane's rows: a stale selection there
         * would still feed the toolbar's Delete Task.                      */
        gtk_list_store_clear(lw->task_store);
        GtkWidget *w = panel_widget(lw, panel_view);
        if (w != NULL && panel_view->panel_refresh != NULL)
            panel_view->panel_refresh(lw->app, w, panel_view->user_data);
        return;
    }

    if (!kanban)
        scroll_keep_queue(lw->task_view);
    /* Cleared in BOTH modes, for the same reason the forecast clears it:
     * a selection left in the hidden list would still feed Delete Task.
     * On the board that job belongs to lw->kanban_sel.                     */
    gtk_list_store_clear(lw->task_store);

    /* Collect the tasks of the current view.  A registered view answers
     * for itself (see task_view.h); anything else is a real list.          */
    const TaskView *view = sel_view(lw);
    GPtrArray *tasks;                /* Task* rows to show                */
    gboolean virtual_view;           /* show the "in <list>" line           */
    const gchar *view_name = "";
    const gchar *unit      = "task";
    if (view != NULL) {
        tasks        = view->query(lw->app, view->user_data);
        virtual_view = view->virtual_rows;
        view_name    = view->name != NULL ? view->name : "";
        if (view->unit != NULL)
            unit = view->unit;
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
        ? refresh_kanban(lw, rows, &ctx)
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

    task_ptr_array_free_tasks(tasks);
}

/* full_refresh() — sidebar + task pane + open editors, plus the Sync
 * button's visibility (hidden while the Google master switch is off —
 * Settings fires a full notify when it flips).                             */
static void
full_refresh(TaskLibrary *lw)
{
    refresh_sidebar(lw);
    refresh_tasks(lw);
    /* Contributed toolbar buttons decide their own visibility (see
     * task_ui.h).  The window used to own a Sync button and gate it on
     * Google's setting while it also ran the Notes mirror; each
     * integration now brings its own button and answers for it.         */
    task_ui_tools_apply_visibility(lw->app);
    if (lw->ui_tool_rule != NULL)
        gtk_widget_set_visible(lw->ui_tool_rule,
                               task_ui_any_tool_visible(lw->app));
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
    lw->search = task_search_parse(gtk_entry_get_text(GTK_ENTRY(entry)));
    /* Hand-sorting is suspended while a filter is up and comes back when
     * it clears (manual_sort_live says why), so the ⠿ handle column has to
     * be re-applied on the way through — BEFORE the rows are rebuilt, so
     * the pane is drawn once, in the shape it is about to keep.           */
    task_manual_sort_apply(lw);
    refresh_tasks(lw);
}

/* on_search_stopped() — Escape in the search box: empty it, which fires
 * "search-changed" and so drops the filter through the one path above.
 * GtkSearchEntry raises the signal but does not clear itself — that is
 * GtkSearchBar's job, and there is no search bar here.                     */
static void
on_search_stopped(GtkWidget *entry, gpointer data)
{
    (void)data;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
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
    GtkWidget *icon = task_app_icon_image_sized(lw->app,
        show ? "hidden" : "visible", 24);
    if (icon != NULL) {
        gtk_widget_show(icon);
        gtk_tool_button_set_icon_widget(
            GTK_TOOL_BUTTON(lw->hide_done_item), icon);
    }
    gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->hide_done_item),
        show ? "Hide completed tasks" : "Show completed tasks");
    /* The menu twin says the same thing in words.  No handler blocking:
     * setting a label cannot emit "activate", where set_active on a check
     * item would have (same reason as manual_sort_icon_refresh).          */
    if (lw->view_show_done_item != NULL)
        gtk_menu_item_set_label(GTK_MENU_ITEM(lw->view_show_done_item),
                                show ? DONE_LABEL_TO_HIDE
                                     : DONE_LABEL_TO_SHOW);
}

/* on_toggle_done_visible() — the toolbar toggle behind it: flip the
 * persisted show_completed flag and rebuild the task pane.                 */
static void
on_toggle_done_visible(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gboolean show = !task_app_config_get_bool("show_completed", TRUE);
    task_app_config_set("show_completed", show ? "1" : "0");
    hide_done_icon_refresh(lw);
    refresh_tasks(lw);
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
    GtkWidget *icon = task_app_icon_image_sized(lw->app,
        manual ? "automatic" : "manual", 24);
    if (icon) {
        gtk_widget_show(icon);
        gtk_tool_button_set_icon_widget(
            GTK_TOOL_BUTTON(lw->manual_sort_item), icon);
    }
    gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->manual_sort_item),
        manual ? "Switch to automatic sorting"
               : "Switch to manual drag sorting");
    /* The menu item is a plain action item whose LABEL is the destination
     * mode, so it needs no handler blocking: setting a label cannot emit
     * "activate", where set_active on a check item would have.            */
    if (lw->view_manual_sort_item != NULL)
        gtk_menu_item_set_label(GTK_MENU_ITEM(lw->view_manual_sort_item),
                                manual ? SORT_LABEL_TO_AUTO
                                       : SORT_LABEL_TO_MANUAL);
}

/* on_toggle_manual_sort() — toolbar button that flips task_list_manual_sort
 * and refreshes the pane.                                                  */
static void
on_toggle_manual_sort(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gboolean manual = !lw->manual_sort;
    task_app_config_set("task_list_manual_sort", manual ? "1" : "0");
    task_manual_sort_apply(lw);
    manual_sort_icon_refresh(lw);
    refresh_tasks(lw);
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
        full_refresh(lw);
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
    if (sidebar_show_pinned(lw) != lw->pinned_row_shown)
        refresh_sidebar(lw);
    refresh_tasks(lw);
}

/* ---------------------------------------------------------------------------
 * Status-bar fade: 3 s hold then a 1 s fade-out (20 × 50 ms).
 * ------------------------------------------------------------------------- */
#define STATUS_FADE_STEPS    20
#define STATUS_FADE_INTERVAL 50   /* ms */
#define STATUS_FADE_HOLD     3000 /* ms before fade starts */

static void
status_fade_cancel(TaskLibrary *lw)
{
    if (lw->status_fade_source != 0) {
        g_source_remove(lw->status_fade_source);
        lw->status_fade_source = 0;
    }
    if (lw->status_fade_step_source != 0) {
        g_source_remove(lw->status_fade_step_source);
        lw->status_fade_step_source = 0;
    }
    lw->status_fade_step = 0;
    g_clear_pointer(&lw->status_fade_text, g_free);
}

static gboolean
status_fade_step_cb(gpointer data)
{
    TaskLibrary *lw = lib_of((TaskApp *)data);
    if (lw == NULL || lw->status_fade_text == NULL) {
        if (lw != NULL)
            lw->status_fade_step_source = 0;
        return G_SOURCE_REMOVE;
    }
    lw->status_fade_step++;
    if (lw->status_fade_step >= STATUS_FADE_STEPS) {
        gtk_label_set_text(GTK_LABEL(lw->status_right), "");
        lw->status_fade_step_source = 0;
        g_clear_pointer(&lw->status_fade_text, g_free);
        return G_SOURCE_REMOVE;
    }
    gint alpha = 100 - (lw->status_fade_step * 100 / STATUS_FADE_STEPS);
    gchar *escaped = g_markup_escape_text(lw->status_fade_text, -1);
    gchar *markup  = g_strdup_printf("<span alpha=\"%d%%\">%s</span>",
                                     alpha, escaped);
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(lw->status_right), markup);
    g_free(markup);
    return G_SOURCE_CONTINUE;
}

static gboolean
status_fade_start_cb(gpointer data)
{
    TaskLibrary *lw = lib_of((TaskApp *)data);
    if (lw == NULL)
        return G_SOURCE_REMOVE;
    lw->status_fade_source = 0;
    lw->status_fade_text   = g_strdup(gtk_label_get_text(
                                          GTK_LABEL(lw->status_right)));
    lw->status_fade_step   = 0;
    lw->status_fade_step_source =
        g_timeout_add(STATUS_FADE_INTERVAL, status_fade_step_cb, data);
    return G_SOURCE_REMOVE;
}

static void
notify_status_hook(TaskApp *app, const gchar *message, gpointer user_data)
{
    (void)user_data;
    TaskLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    status_fade_cancel(lw);
    gtk_label_set_text(GTK_LABEL(lw->status_right), message);
    lw->status_fade_source =
        g_timeout_add(STATUS_FADE_HOLD, status_fade_start_cb, app);
}

/* ===========================================================================
 * Sidebar behavior.
 * =========================================================================== */

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
 * selection the cursor row (last pressed) drives sel_kind/sel_id; group
 * rows are tracked but don't switch the task pane.                         */
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
    if (lw->sel_kind != SB_KIND_GROUP)
        refresh_tasks(lw);
}

/* selected_list_id() — the currently selected REAL list, or 0.             */
static gint64
selected_list_id(TaskLibrary *lw)
{
    return lw->sel_kind == SB_KIND_LIST ? lw->sel_id : 0;
}


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
    /* A panel view owns its pane, so only it can answer.  NULL is a fine
     * answer — the forecast's day views deliberately have no selection.   */
    const TaskView *view = sel_view(lw);
    if (task_view_is_panel(view)) {
        GArray *ids = NULL;
        GtkWidget *w = panel_widget(lw, view);
        if (w != NULL && view->panel_selection != NULL)
            ids = view->panel_selection(lw->app, w, view->user_data);
        return ids != NULL ? ids
                           : g_array_new(FALSE, FALSE, sizeof(gint64));
    }
    if (lw->kanban)
        return card_sel_ids(lw);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view));
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
    for (GList *l = rows; l != NULL; l = l->next) {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, l->data)) {
            gint64 id;
            gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
            if (id != 0)
                g_array_append_val(ids, id);
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

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

    /* While dragging, paint the held row amber so it is easy to track.     */
    if (lw && lw->drag_active && lw->drag_row_ref) {
        GtkTreePath *drag_path =
            gtk_tree_row_reference_get_path(lw->drag_row_ref);
        if (drag_path) {
            GtkTreePath *path = gtk_tree_model_get_path(model, iter);
            if (gtk_tree_path_compare(path, drag_path) == 0)
                bg = DRAG_ROW_TINT;
            gtk_tree_path_free(path);
            gtk_tree_path_free(drag_path);
        }
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

/* ===========================================================================
 * Toolbar actions.
 * =========================================================================== */

/* sidebar_menu_sync() — point the View menu's sidebar item at the ACTION
 * it offers, from the pane's LIVE visibility: "Hide Sidebar" while the
 * lists pane is up, "Show Sidebar" while it is not.  No handler blocking
 * is needed — an action item's label carries no state to feed back, and
 * set_label cannot emit "activate" (same protocol as
 * hide_done_icon_refresh and manual_sort_icon_refresh).                    */
static void
sidebar_menu_sync(TaskLibrary *lw)
{
    if (lw->view_sidebar_item == NULL)
        return;
    gtk_menu_item_set_label(GTK_MENU_ITEM(lw->view_sidebar_item),
        gtk_widget_get_visible(lw->sidebar_box) ? SIDEBAR_LABEL_TO_HIDE
                                               : SIDEBAR_LABEL_TO_SHOW);
}

/* sidebar_set_visible() — show or hide the lists pane, persist the
 * choice in `sidebar_visible` and keep the View menu check in step.
 * Both the toolbar button and the menu item route through here.            */
static void
sidebar_set_visible(TaskLibrary *lw, gboolean show)
{
    gtk_widget_set_visible(lw->sidebar_box, show);
    task_app_config_set("sidebar_visible", show ? "1" : "0");
    sidebar_menu_sync(lw);
}

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
    sidebar_menu_sync(lw);

    if (lw->view_compact_item != NULL)
        gtk_menu_item_set_label(GTK_MENU_ITEM(lw->view_compact_item),
            compact ? CTRL_LABEL_TO_FULL : CTRL_LABEL_TO_COMPACT);

    /* Compact Controls takes the whole toolbar away, search box included,
     * so a filter left running would go on hiding tasks with nothing left
     * on screen to say why or to switch it off — a worse trap than a
     * control that does nothing, because the pane looks like the data.
     * Emptying the box drops the filter through the ordinary
     * "search-changed" path, so there is no second place that knows how to
     * clear a search.  Only when there is one: an unconditional set_text
     * would refresh the pane on every layout toggle.                       */
    if (compact && lw->search != NULL && lw->search_entry != NULL)
        gtk_entry_set_text(GTK_ENTRY(lw->search_entry), "");
}

/* on_toggle_sidebar() — toolbar show/hide button for the lists pane:
 * the task view takes the whole window while it is hidden (mirrors the
 * Notes "Folders" toggle).                                                */
static void
on_toggle_sidebar(GtkWidget *widget, gpointer data)
{
    (void)widget;
    TaskLibrary *lw = data;
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

/* ---------------------------------------------------------------------------
 * Group context-menu actions (forward-declared; menu built in
 * on_sb_button_press below).
 * ------------------------------------------------------------------------- */
static void
on_sb_ctx_move_to_group(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw   = data;
    GArray    *ids  = g_object_get_data(G_OBJECT(item), "task-ids");
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "task-group-id");
    for (guint i = 0; i < ids->len; i++)
        task_db_list_set_group(lw->app->db,
                               g_array_index(ids, gint64, i), group_id);
    full_refresh(lw);
}

static void
on_sb_ctx_remove_from_group(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray   *ids = g_object_get_data(G_OBJECT(item), "task-ids");
    for (guint i = 0; i < ids->len; i++)
        task_db_list_set_group(lw->app->db,
                               g_array_index(ids, gint64, i), 0);
    full_refresh(lw);
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

static void
on_sb_ctx_rename_group(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw   = data;
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "task-group-id");
    GPtrArray *groups  = task_db_groups(lw->app->db);
    gchar     *current = NULL;
    for (guint i = 0; i < groups->len; i++) {
        TaskGroup *g = g_ptr_array_index(groups, i);
        if (g->id == group_id) { current = g_strdup(g->name); break; }
    }
    task_ptr_array_free_groups(groups);
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "Rename Group", "Rename",
                              current ? current : "", &name)) {
        task_db_group_rename(lw->app->db, group_id, name);
        full_refresh(lw);
        g_free(name);
    }
    g_free(current);
}

static void
on_sb_ctx_delete_group(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw   = data;
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "task-group-id");
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Remove this group? Its lists will become ungrouped.");
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_OK) {
        if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == group_id) {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id   = 0;
        }
        task_db_group_delete(lw->app->db, group_id);
        full_refresh(lw);
    }
}

static void on_new_list(GtkWidget *, gpointer);
static void on_new_group(GtkWidget *, gpointer);
static void on_edit_list(GtkWidget *, gpointer);
static void on_delete_list(GtkWidget *, gpointer);

/* on_sb_button_press() — right-click on the sidebar: always offers New List
 * and New Group; adds Edit/Delete for SB_KIND_LIST, Rename/Remove for
 * SB_KIND_GROUP, and group-assignment items when groups exist.  Right-clicking
 * inside an existing multi-selection keeps it; outside collapses to the
 * clicked row first.                                                       */
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

    GtkWidget *menu = gtk_menu_new();
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    /* New List and New Group are always available. */
    GtkWidget *new_list = gtk_menu_item_new_with_label("New List");
    g_signal_connect(new_list, "activate", G_CALLBACK(on_new_list), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_list);

    GtkWidget *new_grp = gtk_menu_item_new_with_label("New Group");
    g_signal_connect(new_grp, "activate", G_CALLBACK(on_new_group), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_grp);

    if (kind == SB_KIND_LIST) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

        GtkWidget *edit = gtk_menu_item_new_with_label("Edit List");
        g_signal_connect(edit, "activate", G_CALLBACK(on_edit_list), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit);

        GtkWidget *del = gtk_menu_item_new_with_label("Delete List");
        g_signal_connect(del, "activate", G_CALLBACK(on_delete_list), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);

        /* Collect selected list ids and group membership for move items. */
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
        gboolean any_grouped = FALSE;
        for (GList *r = rows; r; r = r->next) {
            GtkTreeIter ri;
            if (!gtk_tree_model_get_iter(model, &ri, r->data)) continue;
            gint k; gint64 lid;
            gtk_tree_model_get(model, &ri, SB_KIND, &k, SB_ID, &lid, -1);
            if (k != SB_KIND_LIST) continue;
            g_array_append_val(ids, lid);
            TaskList *l = task_db_list_get(lw->app->db, lid);
            if (l) {
                if (l->group_id != 0) any_grouped = TRUE;
                task_list_free(l);
            }
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

        GPtrArray *groups = task_db_groups(lw->app->db);
        if (groups->len > 0 || any_grouped) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                                  gtk_separator_menu_item_new());
            if (groups->len > 0) {
                GtkWidget *move = gtk_menu_item_new_with_label("Move to Group");
                GtkWidget *sub  = gtk_menu_new();
                for (guint i = 0; i < groups->len; i++) {
                    TaskGroup *g = g_ptr_array_index(groups, i);
                    GtkWidget *gi = gtk_menu_item_new_with_label(g->name);
                    g_object_set_data_full(G_OBJECT(gi), "task-ids",
                                           g_array_ref(ids),
                                           (GDestroyNotify)g_array_unref);
                    g_object_set_data(G_OBJECT(gi), "task-group-id",
                                      (gpointer)(gintptr)g->id);
                    g_signal_connect(gi, "activate",
                                     G_CALLBACK(on_sb_ctx_move_to_group), lw);
                    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gi);
                }
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(move), sub);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), move);
            }
            if (any_grouped) {
                GtkWidget *rem =
                    gtk_menu_item_new_with_label("Remove from Group");
                g_object_set_data_full(G_OBJECT(rem), "task-ids",
                                       g_array_ref(ids),
                                       (GDestroyNotify)g_array_unref);
                g_signal_connect(rem, "activate",
                                 G_CALLBACK(on_sb_ctx_remove_from_group), lw);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), rem);
            }
        }
        task_ptr_array_free_groups(groups);
        g_array_unref(ids);

    } else if (kind == SB_KIND_GROUP) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

        GtkWidget *rename = gtk_menu_item_new_with_label("Rename Group");
        g_object_set_data(G_OBJECT(rename), "task-group-id",
                          (gpointer)(gintptr)id);
        g_signal_connect(rename, "activate",
                         G_CALLBACK(on_sb_ctx_rename_group), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), rename);

        GtkWidget *del = gtk_menu_item_new_with_label("Remove Group");
        g_object_set_data(G_OBJECT(del), "task-group-id",
                          (gpointer)(gintptr)id);
        g_signal_connect(del, "activate",
                         G_CALLBACK(on_sb_ctx_delete_group), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* on_new_group() — prompt for a name and create a new list group.          */
static void
on_new_group(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "New Group", "Create", NULL, &name)) {
        gint64 gid = task_db_group_create(lw->app->db, name);
        if (gid == 0)
            task_app_status(lw->app, "Failed to create group");
        else
            full_refresh(lw);
        g_free(name);
    }
}

/* on_new_list() — prompt (name + optional emoji), create, select.          */
static void
on_new_list(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
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
            full_refresh(lw);
            task_app_status(lw->app,
                            "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name);
        }
    }
    g_free(name);
    g_free(emoji);
}

/* on_edit_list() — change the selected list's name and/or emoji.           */
static void
on_edit_list(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    if (view_refuse(lw, "edit the list each item lives in"))
        return;
    gint64 id = selected_list_id(lw);
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
        full_refresh(lw);
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
        on_edit_list(NULL, lw);
}

/* on_delete_list() — confirm + tombstone the selected real list; when a
 * group is selected, delegate to on_sb_ctx_delete_group.                   */
static void
on_delete_list(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    if (lw->sel_kind == SB_KIND_GROUP) {
        gint64 gid = lw->sel_id;
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
            "Remove this group? Its lists will become ungrouped.");
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (resp == GTK_RESPONSE_OK) {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id   = 0;
            task_db_group_delete(lw->app->db, gid);
            full_refresh(lw);
        }
        return;
    }
    if (view_refuse(lw, "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6"))
        return;
    gint64 id = selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app, "Select a list to delete");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    /* Ask every registered veto first — an integration may know its
     * remote side will refuse the delete (see task_ops.h).                */
    gchar *why = NULL;
    if (!task_ops_list_can_delete(lw->app, l, &why)) {
        task_app_status(lw->app, "%s", why != NULL ? why
                        : "That list cannot be deleted");
        g_free(why);
        task_list_free(l);
        return;
    }
    gboolean yes = task_app_confirm(GTK_WINDOW(lw->window), "Delete List",
        "Delete the list \xe2\x80\x9c%s\xe2\x80\x9d and all of its "
        "tasks?", l->name);
    if (yes) {
        task_db_list_delete(lw->app->db, id);
        /* Drop the list's order keys with it — nothing else ever would, so
         * the ini otherwise grows a dead manual_order_list_<id> AND
         * kanban_order_list_<id> entry for every list ever deleted.  Both
         * families are per-list, so both need this.                        */
        gchar *order_key = list_order_key(id);
        task_app_config_set(order_key, NULL);   /* NULL removes the key     */
        g_free(order_key);
        gchar *kb_key = g_strdup_printf(
            "kanban_order_list_%" G_GINT64_FORMAT, id);
        task_app_config_set(kb_key, NULL);
        g_free(kb_key);
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id = 0;              /* falls back to the first list        */
        full_refresh(lw);
        task_app_status(lw->app,
                        "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", l->name);
    }
    task_list_free(l);
}

/* on_new_task() — create an empty task in the selected list and open its
 * editor.  The virtual views cannot hold new tasks.                        */
static void
on_new_task(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gint64 list_id = selected_list_id(lw);
    if (list_id == 0) {
        task_app_status(lw->app,
                        "Select a list first \xe2\x80\x94 tasks cannot be "
                      "created in the virtual views");
        return;
    }
    gint64 id = task_db_task_create(lw->app->db, list_id, 0, "New Task");
    if (id == 0) {                   /* write failed (logged by the db)     */
        task_app_status(lw->app, "Could not create the task \xe2\x80\x94 "
                        "database write failed");
        return;
    }
    full_refresh(lw);
    task_editor_open_new(lw->app, id);  /* the Save / Cancel variant        */
}

/* on_delete_task() — confirm + tombstone the selected task.                */
static void
on_delete_task(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    /* Mirrored Notes items delete like any other task: the row is
     * tombstoned and its uid parked in bn_deleted, so the next mirror
     * pass does not helpfully re-create what was just deleted.             */
    GArray *ids = selected_task_ids(lw);
    if (ids->len == 0) {
        task_app_status(lw->app, "Select a task to delete");
        g_array_unref(ids);
        return;
    }

    gboolean yes;                    /* confirmed?                          */
    if (ids->len == 1) {
        Task *t = task_db_task_get(lw->app->db,
                                   g_array_index(ids, gint64, 0));
        if (t == NULL) {
            g_array_unref(ids);
            return;
        }
        yes = task_app_confirm(GTK_WINDOW(lw->window), "Delete Task",
            "Delete \xe2\x80\x9c%s\xe2\x80\x9d%s?",
            *t->title != '\0' ? t->title : "Untitled Task",
            t->parent_id == 0 ? " and its subtasks" : "");
        task_free(t);
    } else {
        yes = task_app_confirm(GTK_WINDOW(lw->window), "Delete Tasks",
            "Delete the %u selected tasks (and their subtasks)?",
            ids->len);
    }
    if (yes) {
        for (guint i = 0; i < ids->len; i++) {
            gint64 id = g_array_index(ids, gint64, i);
            GtkWindow *editor =
                g_hash_table_lookup(lw->app->editors, &id);
            if (editor != NULL)
                gtk_widget_destroy(GTK_WIDGET(editor));
            task_db_task_delete(lw->app->db, id);
        }
        full_refresh(lw);
        task_app_status(lw->app, "Deleted %u task%s", ids->len,
                        ids->len == 1 ? "" : "s");
    }
    g_array_unref(ids);
}

/* ===========================================================================
 * Task context menu — Open in Google Tasks, Move to List, Delete.
 * =========================================================================== */

static GtkWidget *menu_item(GtkWidget *menu, const gchar *label,
                            GCallback cb, gpointer data);
static GArray    *item_ids(GtkWidget *item);

/* on_ctx_info() — open the task editor (same as double-clicking the row).  */
static void
on_ctx_info(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray *ids = item_ids(item);
    if (ids == NULL || ids->len == 0)
        return;
    task_editor_open(lw->app, g_array_index(ids, gint64, 0));
}

/* item_ids() — the gint64 id array stashed on a context-menu item.         */
static GArray *
item_ids(GtkWidget *item)
{
    return g_object_get_data(G_OBJECT(item), "task-ids");
}

/* on_ctx_set_done() — Mark Complete / Mark Incomplete on the selection.
 * These are the checkbox's two verbs in menu form and take the same
 * route: Complete → Done, Incomplete → In Progress.  Per row, so a
 * multi-row "Mark All Incomplete" over a mixed selection settles every
 * one of them on In Progress rather than half-reverting.                   */
static void
on_ctx_set_done(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean done = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "task-done"));
    TaskStatus status = done ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS;
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_status(lw->app->db,
                                g_array_index(ids, gint64, i), status);
    full_refresh(lw);
    task_app_status(lw->app, "Marked %u task%s %s", ids->len,
                    ids->len == 1 ? "" : "s",
                    done ? "complete" : "incomplete");
}

/* ctx_done_item() — one Mark (All) Complete / Incomplete context-menu
 * item: the selection rides on the item as its own g_array_ref, the
 * complete/incomplete flag as "task-done".                                 */
static void
ctx_done_item(TaskLibrary *lw, GtkWidget *menu, GArray *ids,
              gboolean single, gboolean done)
{
    GtkWidget *item = gtk_menu_item_new_with_label(
        done ? (single ? "Mark Complete"   : "Mark All Complete")
             : (single ? "Mark Incomplete" : "Mark All Incomplete"));
    g_object_set_data_full(G_OBJECT(item), "task-ids", g_array_ref(ids),
                           (GDestroyNotify)g_array_unref);
    g_object_set_data(G_OBJECT(item), "task-done", GINT_TO_POINTER(done));
    g_signal_connect(item, "activate", G_CALLBACK(on_ctx_set_done), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* on_ctx_set_pinned() — Pin / Unpin on the selection (local-only; the
 * sidebar's Pinned Tasks row follows via full_refresh).                    */
static void
on_ctx_set_pinned(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean pinned = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "task-flag"));
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_pinned(lw->app->db,
                                g_array_index(ids, gint64, i), pinned);
    full_refresh(lw);
    task_app_status(lw->app, "%s %u task%s",
                    pinned ? "Added to Favorites" : "Removed from Favorites",
                    ids->len, ids->len == 1 ? "" : "s");
}

/* on_ctx_set_priority() — Set / Clear High Priority on the selection
 * (local-only; the views re-sort via full_refresh).                        */
static void
on_ctx_set_priority(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean priority = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "task-flag"));
    for (guint i = 0; i < ids->len; i++)
        task_db_task_set_priority(lw->app->db,
                                  g_array_index(ids, gint64, i), priority);
    full_refresh(lw);
    task_app_status(lw->app, "%s high priority on %u task%s",
                    priority ? "Set" : "Cleared",
                    ids->len, ids->len == 1 ? "" : "s");
}

/* ctx_flag_item() — one bulk context-menu item: the selection rides on
 * the item as its own g_array_ref ("task-ids"), the boolean to apply as
 * "task-flag".                                                             */
static void
ctx_flag_item(TaskLibrary *lw, GtkWidget *menu, GArray *ids,
              const gchar *label, gboolean flag, GCallback cb)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(item), "task-ids", g_array_ref(ids),
                           (GDestroyNotify)g_array_unref);
    g_object_set_data(G_OBJECT(item), "task-flag", GINT_TO_POINTER(flag));
    g_signal_connect(item, "activate", cb, lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* on_ctx_move() — a destination picked in the Move to List menu: move
 * every selected TOP-LEVEL task not already there (subtasks travel with
 * their parents; a selected subtask on its own cannot move).               */
static void
on_ctx_move(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    GArray *ids = item_ids(item);
    gint64 dest_id = *(gint64 *)g_object_get_data(G_OBJECT(item),
                                                  "task-dest-id");
    guint moved = 0;                 /* how many actually went              */
    for (guint i = 0; i < ids->len; i++) {
        gint64 id = g_array_index(ids, gint64, i);
        if (task_ops_move_to_list(lw->app, id, dest_id))
            moved++;                 /* it declines subtasks and no-op moves */
    }
    if (moved > 0) {
        full_refresh(lw);
        task_app_status(lw->app, "Moved %u task%s", moved,
                        moved == 1 ? "" : "s");
    } else {
        task_app_status(lw->app, "Nothing to move (subtasks move with "
                        "their parent task)");
    }
}

/* ---------------------------------------------------------------------------
 * task_context_menu_popup() — build and pop the task context menu for the
 * CURRENT selection, whatever produced it.
 *
 * Shared by the list view's rows and the Kanban board's cards, so the two
 * can never drift apart.  It reads the selection through
 * selected_task_ids, which already answers with the board's single card
 * selection while the board is up — so nothing here needs to know which
 * pane the click came from.
 *
 *   anchor — a LONG-LIVED widget to attach the menu to.  Not the clicked
 *            card: an attached menu dies with its widget, and a card is
 *            destroyed by the next refresh, which any of these actions
 *            triggers.
 *
 * Returns TRUE when a menu was shown (the click is consumed).
 * ------------------------------------------------------------------------- */
static gboolean
task_context_menu_popup(TaskLibrary *lw, GtkWidget *anchor,
                        GdkEventButton *event)
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

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), anchor, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    /* Info… — single row only; opens the editor (same as double-click).    */
    if (single) {
        GtkWidget *info_item = gtk_menu_item_new_with_label("Info\xe2\x80\xa6");
        g_object_set_data_full(G_OBJECT(info_item), "task-ids",
                               g_array_ref(ids),
                               (GDestroyNotify)g_array_unref);
        g_signal_connect(info_item, "activate",
                         G_CALLBACK(on_ctx_info), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), info_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    }

    /* Mark Complete / Mark Incomplete — single row: only the applicable
     * direction; multi: both (selection may be mixed).                     */
    if (single && t != NULL)
        ctx_done_item(lw, menu, ids, TRUE,
                      t->status != TASK_STATUS_DONE);
    else {
        ctx_done_item(lw, menu, ids, FALSE, TRUE);
        ctx_done_item(lw, menu, ids, FALSE, FALSE);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* Pin / Unpin and High Priority: a single row gets just the action
     * that applies to it; a multi-selection (possibly mixed states)
     * gets both directions.                                                */
    if (single && t != NULL) {
        ctx_flag_item(lw, menu, ids,
                      t->pinned ? "Remove from Favorites" : "Add to Favorites",
                      !t->pinned, G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids,
                      t->priority ? "Clear High Priority"
                                  : "Set High Priority",
                      !t->priority, G_CALLBACK(on_ctx_set_priority));
    } else {
        ctx_flag_item(lw, menu, ids, "Add All to Favorites", TRUE,
                      G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids, "Remove All from Favorites", FALSE,
                      G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids, "Set All High Priority", TRUE,
                      G_CALLBACK(on_ctx_set_priority));
        ctx_flag_item(lw, menu, ids, "Clear All High Priority", FALSE,
                      G_CALLBACK(on_ctx_set_priority));
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* Contributed items (see task_ui.h).  An item that does not apply to
     * this selection is GREYED, not hidden: the menu keeps its shape
     * between right-clicks rather than moving under the pointer.          */
    for (guint i = 0; i < task_ui_task_menu_count(); i++) {
        const TaskUiTaskMenuDef *d = task_ui_task_menu_nth(i);
        GtkWidget *item = gtk_menu_item_new_with_label(d->label);
        gboolean on = d->enabled == NULL ||
                      d->enabled(lw->app, ids, d->user_data);
        if (on) {
            g_object_set_data(G_OBJECT(item), "task-ui-def", (gpointer)d);
            /* The ids array outlives the menu: it is ref'd onto the item
             * exactly as the app's own bulk actions do.                   */
            g_object_set_data_full(G_OBJECT(item), "task-ids",
                                   g_array_ref(ids),
                                   (GDestroyNotify)g_array_unref);
            g_signal_connect(item, "activate",
                             G_CALLBACK(on_ui_task_menu_activated), lw);
        } else {
            gtk_widget_set_sensitive(item, FALSE);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    /* Move to List — applies to the selection's top-level tasks.           */
    GtkWidget *move_item = gtk_menu_item_new_with_label("Move to List");
    GtkWidget *submenu = gtk_menu_new();
    GPtrArray *lists = task_db_lists(lw->app->db, FALSE);
    guint added = 0;                 /* destinations offered                */
    for (guint i = 0; i < lists->len; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        /* For a single selection its own list is pointless; keep every
         * destination for multi (rows may span lists in virtual views).    */
        if (single && t != NULL && l->id == t->list_id)
            continue;
        gchar *label = list_label(l);
        GtkWidget *dest = gtk_menu_item_new_with_label(label);
        g_free(label);
        gint64 *did = g_new(gint64, 1);
        *did = l->id;
        g_object_set_data_full(G_OBJECT(dest), "task-dest-id", did,
                               g_free);
        g_object_set_data_full(G_OBJECT(dest), "task-ids",
                               g_array_ref(ids),
                               (GDestroyNotify)g_array_unref);
        g_signal_connect(dest, "activate",
                         G_CALLBACK(on_ctx_move), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), dest);
        added++;
    }
    task_ptr_array_free_lists(lists);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(move_item), submenu);
    gtk_widget_set_sensitive(move_item, added > 0 &&
        !(single && t != NULL && t->parent_id != 0));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), move_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    gchar *del_label = single
        ? g_strdup("Delete Task")
        : g_strdup_printf("Delete %u Tasks", ids->len);
    menu_item(menu, del_label, G_CALLBACK(on_delete_task), lw);
    g_free(del_label);

    task_free(t);
    g_array_unref(ids);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
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
     * manual_sort_live, not the raw flag: a search hides rows, and the
     * order writer would drop every hidden one (see manual_sort_live).  */
    if (event->button == 1 && manual_sort_live(lw)) {
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
                    lw->drag_active = TRUE;
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
 * There is no on_sync() here any more, and no File → Sync Now.
 *
 * SYNC IS ENTIRELY PLUGIN BUSINESS, and the window cannot describe it.
 * That item ran every registered worker, which made its label a promise
 * it could not keep: with no integration installed it did nothing, with
 * two it did two different things, and either way "Sync Now" in File
 * could not say WHAT was about to be synced.  Each integration now
 * offers its own — Google's is Google → Sync Now (see task_ui.h's
 * TASK_UI_MENU_OWN) — so the label names the thing it acts on.
 *
 * task_worker_run_all() still exists as the run-everything call for
 * whoever wants it; nothing in the core's chrome reaches for it, because
 * the core is not the one who knows what "everything" is.
 * ------------------------------------------------------------------------- */

/* ===========================================================================
 * Menu actions.
 * =========================================================================== */

/* on_menu_settings() — File → Settings…                                    */
static void
on_menu_settings(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    task_settings_window_open(lw->app, GTK_WINDOW(lw->window), lw->app->db->path);
}

/* on_menu_clear_completed() — File → Clear Completed Tasks: archive the
 * selected list's done tasks (Google's tasks.clear when synced).           */
static void
on_menu_clear_completed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gint64 id = selected_list_id(lw);
    if (id == 0) {
        task_app_status(lw->app,
                        "Select a list to clear its completed tasks");
        return;
    }
    TaskList *l = task_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    if (task_app_confirm(GTK_WINDOW(lw->window), "Clear Completed",
                         "Remove all completed tasks from \xe2\x80\x9c%s"
                       "\xe2\x80\x9d?", l->name)) {
        guint n = task_ops_clear_completed(lw->app, id);
        task_app_status(lw->app, "Cleared %u completed task%s", n,
                        n == 1 ? "" : "s");
        full_refresh(lw);
    }
    task_list_free(l);
}

/* find_gtk_image() — first GtkImage in a widget subtree (depth-first).
 * Used to reach GtkAboutDialog's internal logo image, which the public
 * API only feeds with a plain (blurry-on-Retina) GdkPixbuf.                */
static GtkWidget *
find_gtk_image(GtkWidget *widget)
{
    if (GTK_IS_IMAGE(widget))
        return widget;
    GtkWidget *hit = NULL;           /* first image found in the subtree    */
    if (GTK_IS_CONTAINER(widget)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = kids; l != NULL && hit == NULL; l = l->next)
            hit = find_gtk_image(l->data);
        g_list_free(kids);
    }
    return hit;
}

/* ---------------------------------------------------------------------------
 * on_open_db() — File → Open Database File…: pick a .db file and open it as
 * the new default or for this session only.
 * ------------------------------------------------------------------------- */
static void
on_open_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    TaskLibrary *lw  = user_data;
    TaskApp     *app = lw->app;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Open Database", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "SQLite Database (*.db)");
    gtk_file_filter_add_pattern(ff, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

    gchar *file_path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        file_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (file_path == NULL)
        return;

    if (g_strcmp0(file_path, app->db->path) == 0) { /* already open         */
        g_free(file_path);
        return;
    }

    /* Ask: permanent default or this session only? */
    gchar *display = g_path_get_basename(file_path);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Open \xe2\x80\x9c%s\xe2\x80\x9d as your new default database, "
        "or for this session only?", display);
    g_free(display);
    gtk_window_set_title(GTK_WINDOW(dlg), "Tasks - Open Database");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "_Cancel",         GTK_RESPONSE_CANCEL,
        "_Session Only",   1,
        "Set as _Default", 2,
        NULL);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
        g_free(file_path);
        return;
    }
    gboolean set_default = (resp == 2);

    task_editor_close_all(app);
    gchar *old_path = g_strdup(app->db->path);
    task_plugins_db_closing(app, app->db);   /* plugin tables live here too */
    task_db_close(app->db);
    GError *gerr = NULL;
    app->db = task_db_open(file_path, &gerr);

    if (app->db == NULL) {
        task_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_ERROR,
                        "Tasks - Database Error",
                        "Could not open:\n%s\n\n%s",
                        file_path,
                        gerr != NULL ? gerr->message : "Unknown error");
        g_clear_error(&gerr);
        app->db = task_db_open(old_path, &gerr); /* revert                  */
        if (app->db == NULL)
            g_critical("on_open_db: cannot revert to %s: %s", old_path,
                       gerr != NULL ? gerr->message : "?");
        else
            task_plugins_db_open(app, app->db);   /* reverted, but OPEN     */
        g_clear_error(&gerr);
        g_free(old_path);
        g_free(file_path);
        return;
    }
    task_plugins_db_open(app, app->db);

    if (set_default) {
        gchar *dir = g_path_get_dirname(file_path);
        g_free(app->db_dir);
        app->db_dir = g_strdup(dir);
        task_app_config_set("db_dir", dir);
        g_free(dir);
    }

    g_free(old_path);
    g_free(file_path);

    /* Every timer carries the db path it was armed with, so a switch must
     * re-arm all of them or that worker keeps writing to the file we just
     * moved away from.  This site used to name them one by one and had
     * silently fallen one short of task_app_switch_database's list.       */
    task_worker_arm_all(app, app->db->path);
    task_app_notify_changed(app);
    task_app_status(app, "Opened %s", app->db->path);
}

/* The View menu's Completed, Sorting and Sidebar items are wired straight
 * to their TOOLBAR twins (on_toggle_done_visible, on_toggle_manual_sort,
 * on_toggle_sidebar).  Each of those already flips the persisted state and
 * calls the one refresh that re-labels both controls, so a separate menu
 * handler would only be the same three lines under another name — and two
 * copies of "what does this toggle do" is how the two controls drift.
 * There is no state to read off the widget either way: the label says
 * where a click GOES, so every handler flips the config or the cache.     */

/* ---------------------------------------------------------------------------
 * on_toggle_kanban() — the pane toggle, shared by View → Kanban View /
 * List View and its TOOLBAR twin: persist the flag, refresh the cached
 * copy, and rebuild the pane in the other presentation.  refresh_tasks
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
on_toggle_kanban(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    lw->kanban = !lw->kanban;
    task_app_config_set("kanban_view", lw->kanban ? "1" : "0");
    gtk_tree_selection_unselect_all(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view)));
    g_hash_table_remove_all(lw->kanban_sel);
    lw->kanban_anchor = 0;
    refresh_tasks(lw);
}

/* on_menu_toggle_compact() — View → Compact Controls / Full Controls:
 * FLIP the persisted flag and re-apply the layout (toolbar out, floating
 * New/Delete pair in).  Flips rather than reading the widget: the label
 * names the controls a click switches TO, so the item carries no state.
 * compact_layout_apply re-labels it.                                       */
static void
on_menu_toggle_compact(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    task_app_config_set("compact_layout",
        task_app_config_get_bool("compact_layout", FALSE) ? "0" : "1");
    compact_layout_apply(lw);
}

/* on_menu_about() — File → About and the toolbar About button: the
 * standard about dialog with the app logo, version, database vitals and
 * a link to the BSD license (the Notes About, retinted).                  */
static void
on_menu_about(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;

    /* 128x128-logical logo from document.png, decoded at the display's
     * scale factor so it stays sharp on Retina.                            */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    gchar *icon_path = g_build_filename(lw->app->icons_dir,
                                        "document.png", NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path,
                                                       128 * sf, 128 * sf,
                                                       NULL);
    g_free(icon_path);

    const gchar *authors[] = { "Ian Campbell", "Claude", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Tasks");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), TASK_VERSION);
    if (logo != NULL) {
        /* set_logo() first (it makes the internal image visible and
         * sized), then swap that image's content for a cairo surface
         * with the device scale — the pixbuf API renders 1 buffer px
         * per logical px and looks soft on HiDPI.                          */
        GdkPixbuf *at_128 = (sf > 1)
            ? gdk_pixbuf_scale_simple(logo, 128, 128, GDK_INTERP_BILINEAR)
            : g_object_ref(logo);
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), at_128);
        g_object_unref(at_128);

        if (sf > 1) {
            GtkWidget *img = find_gtk_image(
                gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
            if (img != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(logo, sf, NULL);
                gtk_image_set_from_surface(GTK_IMAGE(img), surface);
                cairo_surface_destroy(surface);
            }
        }
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* Database vitals: task/list counts, location, on-disk size.           */
    gint n_tasks, n_lists;           /* totals across the database          */
    task_db_totals(lw->app->db, &n_tasks, &n_lists);
    GStatBuf st;                     /* for the database file size          */
    const gchar *db_path = lw->app->db->path;
    gchar *size_str = (g_stat(db_path, &st) == 0)
                      ? g_format_size((guint64)st.st_size)
                      : g_strdup("unknown");

    /* __DATE__/__TIME__ expand when this file is compiled — the closest
     * portable thing to a "last compiled" stamp.                           */
    gchar *comments = g_strdup_printf(
        "Gettin' shit done since 2026!\n\n"
        "Compiled " __DATE__ " " __TIME__ "\n\n"
        "Database: %s\n"
        "%d tasks in %d lists \xe2\x80\x94 %s on disk",
        db_path, n_tasks, n_lists, size_str);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments);
    g_free(comments);
    g_free(size_str);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD_3);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* on_menu_quit() — File → Quit.                                            */
static void
on_menu_quit(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    gtk_widget_destroy(lw->window);
}

/* menu_item() — build one wired menu item.                                 */
static GtkWidget *
menu_item(GtkWidget *menu, const gchar *label, GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* ---------------------------------------------------------------------------
 * task_library_apply_native_menubar() — move the library menu into (or out
 * of) the native macOS menu bar (see header).  Mirrors Notes: the
 * SAME menu shell drives the macOS bar — the in-window widget just has
 * to be hidden; leaving native mode hands macOS an empty bar so the app
 * menu stays functional.
 * ------------------------------------------------------------------------- */
void
task_library_apply_native_menubar(TaskApp *app, gboolean native)
{
#ifdef HAVE_GTKOSX
    if (app->library_window == NULL)
        return;
    GtkWidget *menubar =             /* the in-window GtkMenuBar            */
        g_object_get_data(G_OBJECT(app->library_window), "task-menubar");
    if (menubar == NULL)
        return;

    GtkosxApplication *osx = gtkosx_application_get();
    if (native) {
        gtk_widget_hide(menubar);
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(menubar));
    } else {
        gtk_widget_show(menubar);
        GtkWidget *empty = gtk_menu_bar_new();
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(empty));
    }
    gtkosx_application_sync_menubar(osx);
#else
    (void)app; (void)native;
#endif
}

/* ===========================================================================
 * Construction.
 * =========================================================================== */

/* on_ui_tool_clicked() — a contributed toolbar button was pressed.  The
 * definition rides on the widget, so one handler serves every item.     */
static void
on_ui_tool_clicked(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    const TaskUiToolDef *d = g_object_get_data(G_OBJECT(item),
                                               "task-ui-def");
    if (d != NULL && d->clicked != NULL)
        d->clicked(lw->app, d->user_data);
}

/* on_ui_menu_activated() — the same for a contributed menu item.        */
static void
on_ui_menu_activated(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    const TaskUiMenuDef *d = g_object_get_data(G_OBJECT(item),
                                               "task-ui-def");
    if (d != NULL && d->activate != NULL)
        d->activate(lw->app, d->user_data);
}

/* on_ui_task_menu_activated() — a contributed task context-menu item.
 * Both the definition and the selection ride on the widget.             */
static void
on_ui_task_menu_activated(GtkWidget *item, gpointer data)
{
    TaskLibrary *lw = data;
    const TaskUiTaskMenuDef *d = g_object_get_data(G_OBJECT(item),
                                                   "task-ui-def");
    GArray *ids = g_object_get_data(G_OBJECT(item), "task-ids");
    if (d != NULL && d->activate != NULL && ids != NULL)
        d->activate(lw->app, ids, d->user_data);
}

/* ui_menu_items() — append every contributed item for `which`.
 *
 * `rule` adds a separator AFTER them so the group reads as its own
 * section; pass FALSE where the caller's own grouping already says where
 * the group ends (File puts them at the head of its second group, which
 * a rule of their own would then split in two).  Either way this appends
 * NOTHING when nothing is contributed — rule included — which keeps an
 * app with no plugins looking exactly as it did.                         */
static void
ui_menu_items(TaskLibrary *lw, GtkWidget *menu, TaskUiMenu which,
              gboolean rule)
{
    gboolean any = FALSE;
    for (guint i = 0; i < task_ui_menu_count(); i++) {
        const TaskUiMenuDef *d = task_ui_menu_nth(i);
        if (d->menu != which)
            continue;
        GtkWidget *item = menu_item(menu, d->label,
                                    G_CALLBACK(on_ui_menu_activated), lw);
        g_object_set_data(G_OBJECT(item), "task-ui-def", (gpointer)d);
        any = TRUE;
    }
    if (any && rule)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
}

/* ---------------------------------------------------------------------------
 * ui_own_menus() — build the TOP-LEVEL menus contributed items asked for
 * (TASK_UI_MENU_OWN, see task_ui.h) and append them to the menu bar.
 *
 * One menu per distinct `menu_title`, created when its first item is
 * reached — so the registry order (which is `sort` order) decides both
 * the items within a menu and the menus among themselves, and an
 * integration with two items gets one menu rather than two.  Titles are
 * compared by CONTENT, not pointer: two plugins are two shared objects,
 * so the same title is a different string in each.
 *
 * Appends nothing when nothing is contributed, which is what keeps the
 * bar at File + View for an app with no plugins.
 * ------------------------------------------------------------------------- */
static void
ui_own_menus(TaskLibrary *lw, GtkWidget *menubar)
{
    GHashTable *by_title = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < task_ui_menu_count(); i++) {
        const TaskUiMenuDef *d = task_ui_menu_nth(i);
        if (d->menu != TASK_UI_MENU_OWN || d->label == NULL)
            continue;
        /* A menu with no name has nowhere to go — skip it rather than
         * putting an untitled menu in the bar.                           */
        if (d->menu_title == NULL || *d->menu_title == '\0')
            continue;
        GtkWidget *menu = g_hash_table_lookup(by_title, d->menu_title);
        if (menu == NULL) {
            menu = gtk_menu_new();
            GtkWidget *top = gtk_menu_item_new_with_label(d->menu_title);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
            gtk_menu_shell_append(GTK_MENU_SHELL(menubar), top);
            g_hash_table_insert(by_title, (gpointer)d->menu_title, menu);
        }
        GtkWidget *item = menu_item(menu, d->label,
                                    G_CALLBACK(on_ui_menu_activated), lw);
        g_object_set_data(G_OBJECT(item), "task-ui-def", (gpointer)d);
    }
    g_hash_table_destroy(by_title);
}

/* tool_button() — a style-aware toolbar button (local icon + label)
 * wired to `cb` and appended to `bar`.                                     */
static GtkToolItem *
tool_button(TaskLibrary *lw, GtkToolbar *bar, const gchar *icon,
            const gchar *fallback_markup, const gchar *label,
            const gchar *tooltip, GCallback cb)
{
    GtkToolItem *item = task_app_tool_item_new(lw->app, icon,
                                               fallback_markup, label,
                                               tooltip);
    g_signal_connect(item, "clicked", cb, lw);
    gtk_toolbar_insert(bar, item, -1);
    return item;
}

/* compact_bar_button() — one floating-bar button: the 24 px local icon
 * (Pango-markup glyph when the PNG is missing, matching the toolbar's
 * fallback rule) wired to `cb`, appended to `box`.                         */
static void
compact_bar_button(TaskLibrary *lw, GtkWidget *box, const gchar *icon,
                   const gchar *fallback_markup, const gchar *tooltip,
                   GCallback cb)
{
    GtkWidget *btn   = gtk_button_new();
    GtkWidget *image = task_app_icon_image_sized(lw->app, icon, 24);
    if (image == NULL) {
        image = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(image), fallback_markup);
    }
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(btn), image);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", cb, lw);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
}

/* float_bar_css() — the floating pill's plate: the window background, with
 * a border shaded off the same color so it reads as a raised object in
 * either a light or a dark theme.                                          */
static gchar *
float_bar_css(const GdkRGBA *bg)
{
    /* Light themes want a DARKER border than the plate, dark themes a
     * lighter one — pick the direction from the plate's own luminance so
     * the edge stays visible either way.                                   */
    gdouble lum = 0.299 * bg->red + 0.587 * bg->green + 0.114 * bg->blue;
    gchar *c   = rgb_of(bg);
    gchar *css = g_strdup_printf(
        "box {"
        "  background-color: %s;"
        "  border: 1px solid shade(%s, %s);"
        "  border-radius: 8px;"
        "  padding: 2px;"
        "}",
        c, c, lum > 0.5 ? "0.80" : "1.35");
    g_free(c);
    return css;
}

/* ---------------------------------------------------------------------------
 * compact_bar_new() — Compact Layout's floating toolbar: New Task and
 * Delete Task as a two-button pill pinned 20 px in from the bottom and
 * right edges of the task area.  Same icons and actions as the top
 * toolbar's pair, so the compact window keeps both task verbs.
 *
 * Returned as an overlay child (halign/valign END + 20 px margins do the
 * pinning); also stored as lw->float_bar, which compact_layout_apply
 * shows and hides.  The bar is NOT registered with
 * task_app_register_toolbar — it is icons-only by design and must not grow
 * labels when the toolbar style changes.
 * ------------------------------------------------------------------------- */
static GtkWidget *
compact_bar_new(TaskLibrary *lw)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    /* A rounded, bordered plate so the buttons read as one floating
     * object over the task rows rather than two loose glyphs.  Both colors
     * come from the theme's @theme_bg_color (the border a shade of it), the
     * same resolution the column headers use — hardcoding the light-theme
     * grays put a white slab over a dark theme's task rows.                */
    themed_bg_css_apply(bar, float_bar_css);
    compact_bar_button(lw, bar, "add2", "+", "Create a task in the "
                       "selected list", G_CALLBACK(on_new_task));
    compact_bar_button(lw, bar, "remove", "\xe2\x88\x92",
                       "Delete the selected task",
                       G_CALLBACK(on_delete_task));

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

/* on_library_configure() — track the live client size for persistence.     */
static gboolean
on_library_configure(GtkWidget *w, GdkEventConfigure *event, gpointer data)
{
    (void)w; (void)event;
    TaskLibrary *lw = data;
    gtk_window_get_size(GTK_WINDOW(lw->window), &lw->win_w, &lw->win_h);
    return FALSE;                    /* propagate                           */
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
    /* The panes themselves are children of the window and are destroyed
     * with it; only the table goes here.                                 */
    g_clear_pointer(&lw->panels, (GDestroyNotify)g_hash_table_destroy);
    /* The entry is a child of the toolbar and goes with the window; the
     * PARSED query is ours and does not.                                   */
    g_clear_pointer(&lw->search, (GDestroyNotify)task_search_free);
    task_ui_tool_forget_all();   /* the toolbar destroyed them */
    task_app_unlisten(lw->app, lw->listen_changed);
    task_app_unlisten(lw->app, lw->listen_tasks);
    task_app_unlisten(lw->app, lw->listen_status);
    lw->listen_changed = lw->listen_tasks = lw->listen_status = 0;
    lw->app->library_window = NULL;
    status_fade_cancel(lw);
    /* A card drag in flight holds a POINTER GRAB and owns a ghost window.
     * Both must come down before the library does, or the grab outlives
     * the widget it was taken on and the pointer is dead app-wide.        */
    card_drag_stop(lw);
    task_editor_close_all(lw->app);
    if (lw->drag_row_ref  != NULL)
        gtk_tree_row_reference_free(lw->drag_row_ref);
    if (lw->drag_lock_ref != NULL)
        gtk_tree_row_reference_free(lw->drag_lock_ref);
    g_clear_object(&lw->drag_cursor);
    g_clear_object(&lw->card_grab);
    g_clear_object(&lw->card_grabbing);
    if (lw->group_expanded != NULL)
        g_hash_table_destroy(lw->group_expanded);
    if (lw->kanban_sel != NULL)
        g_hash_table_destroy(lw->kanban_sel);
    g_free(lw);
}

/* ===========================================================================
 * Manual sort: order persistence, drag handlers, mode toggle.
 * =========================================================================== */

/* list_order_key() — a real list's manual-order config key.  Its own
 * function so on_delete_list can name the key it has to remove without
 * repeating the format string.  New string (g_free).                       */
static gchar *
list_order_key(gint64 list_id)
{
    return g_strdup_printf("manual_order_list_%" G_GINT64_FORMAT, list_id);
}

/* view_order_key() — the config key for the current view's manual sort
 * order, or NULL if the view doesn't support it.  New string (g_free).     */
static gchar *
view_order_key(TaskLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_LIST)
        return list_order_key(lw->sel_id);
    return task_view_order_key(sel_view(lw), "manual_order");
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

/* task_view_apply_manual_order() — after refresh_tasks populates the store,
 * reorder rows to match the saved manual order for the current view.  Tasks
 * absent from the saved list appear at the tail; id=0 rows follow them.    */
static void
task_view_apply_manual_order(TaskLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL) return;
    gchar *saved = task_app_config_get(key);
    g_free(key);
    if (saved == NULL || *saved == '\0') { g_free(saved); return; }
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

    /* Build new_order: saved entries first (in saved sequence), remainder
     * (new rows not yet in saved list) appended at tail.  A pre-mirror
     * order may still hold "NOTEID:ORD" tokens; they parse to 0, match
     * nothing, and are skipped.                                            */
    gint     *new_order = g_new(gint, n);
    gboolean *placed    = g_new0(gboolean, n);
    gint      fill      = 0;
    gchar   **parts     = g_strsplit(saved, ",", -1);
    g_free(saved);
    for (gint i = 0; parts[i] != NULL; i++) {
        gint64 id = g_ascii_strtoll(parts[i], NULL, 10);
        if (id == 0)
            continue;
        for (gint j = 0; j < n; j++) {
            if (ids[j] == id && !placed[j]) {
                new_order[fill++] = j;
                placed[j]         = TRUE;
                break;
            }
        }
    }
    g_strfreev(parts);
    for (gint i = 0; i < n; i++)
        if (!placed[i])
            new_order[fill++] = i;
    gtk_list_store_reorder(lw->task_store, new_order);
    g_free(new_order);
    g_free(placed);
    g_free(ids);
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
    if (!want_resize && manual_sort_live(lw)) {
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
    lw->drag_active = FALSE;
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
static void
task_manual_sort_apply(TaskLibrary *lw)
{
    lw->manual_sort =
        task_app_config_get_bool("task_list_manual_sort", FALSE);
    /* What the COLUMNS show is what is actually on offer, which a search
     * suspends (see manual_sort_live) — so the ⠿ handle goes and the
     * headers become clickable again, giving the filtered view the sorting
     * it can still do.  The cached SETTING above is untouched: clearing the
     * box must bring hand-sorting back, not turn it off.                  */
    gboolean manual = manual_sort_live(lw);
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
     * refresh_tasks reads it before the menu handler ever runs.            */
    lw->kanban = task_app_config_get_bool("kanban_view", FALSE);
    lw->sel_kind = SB_KIND_LIST;     /* refresh falls back to first list    */
    lw->group_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);
    lw->kanban_sel     = g_hash_table_new(NULL, NULL);   /* id set          */

    lw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Tasks");
    /* The last session's closing size (win_w/win_h), else the default.     */
    gchar *ww = task_app_config_get("win_w");
    gchar *wh = task_app_config_get("win_h");
    gint w = ww != NULL ? atoi(ww) : 0;
    gint hgt = wh != NULL ? atoi(wh) : 0;
    gtk_window_set_default_size(GTK_WINDOW(lw->window),
                                w > 0 ? w : 980, hgt > 0 ? hgt : 640);
    g_free(ww);
    g_free(wh);
    g_signal_connect(lw->window, "configure-event",
                     G_CALLBACK(on_library_configure), lw);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(lw->window));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(lw->window), vbox);

    /* --- Menubar ---------------------------------------------------------- */
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    /* ONE separator in this menu, and it goes after the group below.
     * What acts on the TASKS is New Task, New List and Clear Completed;
     * everything after the rule is about the app or the file it keeps —
     * the database, Settings, About, Quit.  A rule between every pair of
     * items (which is what this was) divides nothing, so it stopped
     * reading as grouping at all.
     *
     * No Sync Now here either — an integration contributes its own, in a
     * menu of its own (see the note where on_sync used to be, and
     * task_ui.h).                                                        */
    menu_item(file_menu, "New Task", G_CALLBACK(on_new_task), lw);
    menu_item(file_menu, "New List\xe2\x80\xa6", G_CALLBACK(on_new_list), lw);
    menu_item(file_menu, "Clear Completed Tasks",
              G_CALLBACK(on_menu_clear_completed), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    /* Contributed items lead the second group WITHOUT a rule of their
     * own — one more rule is exactly what this menu is losing.           */
    ui_menu_items(lw, file_menu, TASK_UI_MENU_FILE, FALSE);
    menu_item(file_menu, "Open Database File\xe2\x80\xa6",
              G_CALLBACK(on_open_db), lw);
    menu_item(file_menu, "Settings\xe2\x80\xa6",
              G_CALLBACK(on_menu_settings), lw);
    menu_item(file_menu, "About", G_CALLBACK(on_menu_about), lw);
    menu_item(file_menu, "Quit", G_CALLBACK(on_menu_quit), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    /* Built with the label the persisted state calls for;
     * hide_done_icon_refresh keeps it in step with the toolbar twin from
     * then on.  Wired straight to that twin's handler.                    */
    lw->view_show_done_item = gtk_menu_item_new_with_label(
        task_app_config_get_bool("show_completed", TRUE)
            ? DONE_LABEL_TO_HIDE : DONE_LABEL_TO_SHOW);
    g_signal_connect(lw->view_show_done_item, "activate",
                     G_CALLBACK(on_toggle_done_visible), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_show_done_item);
    /* The sort toggle, as an ACTION item: its label is the mode a click
     * switches TO — "Manual Sorting" while sorting is automatic (the
     * column headers doing it), "Automatic Sorting" while dragging is.
     * Built with the right label already, and
     * manual_sort_icon_refresh keeps it in step with the toolbar twin.     */
    lw->view_manual_sort_item = gtk_menu_item_new_with_label(
        lw->manual_sort ? SORT_LABEL_TO_AUTO : SORT_LABEL_TO_MANUAL);
    g_signal_connect(lw->view_manual_sort_item, "activate",
                     G_CALLBACK(on_toggle_manual_sort), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_manual_sort_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());

    /* Contributed View items, between the two groups: a plugin's way of
     * LOOKING at the tasks belongs with the app's own.  This call was
     * missing — TASK_UI_MENU_VIEW was a registry value the window never
     * read, so anything registered for it went nowhere at all.  It
     * appends nothing (not even its rule) when nothing is contributed,
     * so the menu is unchanged for an app with no plugins.               */
    ui_menu_items(lw, view_menu, TASK_UI_MENU_VIEW, TRUE);

    /* Below the divider: what the WINDOW looks like.  Show/Hide Sidebar
     * mirrors the toolbar's Sidebar button (both write `sidebar_visible`);
     * Compact / Full Controls swaps the toolbar for the floating
     * New/Delete pair, leaving the sidebar to that item in either mode;
     * Kanban View / List View swaps the task pane for the board.  All are applied after the construction-time
     * show_all, at the end of this function.                               */
    lw->view_sidebar_item = gtk_menu_item_new_with_label(
        task_app_config_get_bool("sidebar_visible", FALSE)
            ? SIDEBAR_LABEL_TO_HIDE : SIDEBAR_LABEL_TO_SHOW);
    g_signal_connect(lw->view_sidebar_item, "activate",
                     G_CALLBACK(on_toggle_sidebar), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_sidebar_item);
    lw->view_compact_item = gtk_menu_item_new_with_label(
        task_app_config_get_bool("compact_layout", FALSE)
            ? CTRL_LABEL_TO_FULL : CTRL_LABEL_TO_COMPACT);
    g_signal_connect(lw->view_compact_item, "activate",
                     G_CALLBACK(on_menu_toggle_compact), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_compact_item);
    lw->view_kanban_item = gtk_menu_item_new_with_label(
        lw->kanban ? PANE_LABEL_TO_LIST : PANE_LABEL_TO_KANBAN);
    g_signal_connect(lw->view_kanban_item, "activate",
                     G_CALLBACK(on_toggle_kanban), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_kanban_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    /* Contributed top-level menus come after the app's own: File and View
     * are the window's, and an integration's menu is about the
     * integration.  Built here, once, like the rest of the bar — a plugin
     * switched on while the app is running gets its menu at the next
     * launch, the same as its File items always have.                     */
    ui_own_menus(lw, menubar);

    /* Remembered so the menu can be moved into the native macOS menu
     * bar (see task_library_apply_native_menubar).                         */
    g_object_set_data(G_OBJECT(lw->window), "task-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* --- Toolbar ---------------------------------------------------------- */
    /* Icon names are icons/-relative paths; the curated set lives in
     * icons/ (case-exact for Linux).  Layout: sidebar toggle, a drawn
     * divider, Sync + the completed, sort and pane toggles, a divider,
     * then the task pair — and the About button pushed to the far right.   */
    GtkWidget *toolbar = gtk_toolbar_new();
    lw->toolbar = toolbar;           /* Compact Layout hides it whole       */
    /* Small-toolbar metrics — the Notes bar height.                       */
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool_button(lw, GTK_TOOLBAR(toolbar), "sidebar",
                "\xe2\x97\xa7", "Sidebar", "Show or hide the lists pane",
                G_CALLBACK(on_toggle_sidebar));
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    lw->hide_done_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "hidden", "\xf0\x9f\x91\x81", "Completed",
        "Hide completed tasks", G_CALLBACK(on_toggle_done_visible)));
    hide_done_icon_refresh(lw);      /* the persisted state's icon          */
    lw->manual_sort_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "manual", "\xe2\x89\x8b", "Sort Mode",
        "Switch to manual drag sorting", G_CALLBACK(on_toggle_manual_sort)));
    manual_sort_icon_refresh(lw);    /* the persisted state's tooltip       */
    /* The pane toggle sits with the sort toggle, not with the task
     * buttons: both of them change how the tasks are PRESENTED rather than
     * acting on a task, and the sort toggle is the control it is most
     * often used with (the board is always drag-sorted, which is why that
     * one greys out while this one is on).  task_pane_mode_apply gives it
     * its icon, label and tooltip — it is called after the
     * construction-time show_all.                                        */
    lw->pane_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "menu", "\xe2\x96\xa6", "Kanban",
        "Show the tasks as a Kanban board", G_CALLBACK(on_toggle_kanban)));
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    tool_button(lw, GTK_TOOLBAR(toolbar), "add2", NULL,
                "New Task", "Create a task in the selected list",
                G_CALLBACK(on_new_task));
    tool_button(lw, GTK_TOOLBAR(toolbar), "remove", NULL,
                "Delete Task", "Delete the selected task",
                G_CALLBACK(on_delete_task));

    /* Contributed toolbar items (see task_ui.h) sit LAST, behind their
     * own divider: an integration's button is neither one of the view
     * controls nor one of the task actions, and grouping it with either
     * would say it was.  The divider is added only when there is
     * something to divide, so an app with no plugins keeps exactly the
     * toolbar it had.
     *
     * The divider follows the BUTTONS, not the registry: an item can be
     * registered and hidden (an integration switched off), and a rule
     * with nothing after it reads as a mistake.  It is kept in
     * lw->ui_tool_rule and hidden with them.                              */
    task_ui_tool_forget_all();
    if (task_ui_tool_count() > 0) {
        GtkToolItem *rule = gtk_separator_tool_item_new();
        lw->ui_tool_rule = GTK_WIDGET(rule);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar), rule, -1);
    }
    for (guint i = 0; i < task_ui_tool_count(); i++) {
        const TaskUiToolDef *d = task_ui_tool_nth(i);
        GtkToolItem *item = task_app_tool_item_new(lw->app, d->icon,
                                                   d->fallback_markup,
                                                   d->label, d->tooltip);
        g_object_set_data(G_OBJECT(item), "task-ui-def", (gpointer)d);
        g_signal_connect(item, "clicked", G_CALLBACK(on_ui_tool_clicked),
                         lw);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
        task_ui_tool_bind(d->id, GTK_WIDGET(item));
    }

    /* Expanding blank separator pushes the search box to the right edge
     * (the Notes layout, which keeps its own search box there).           */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer),
                                     FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    /* The search box at the far right, where Notes keeps its own.  It took
     * the About button's place rather than crowding in beside it: the
     * button was a SECOND way to reach a dialog File → About Tasks
     * already opens, and a search box is worth more at the one spot on the
     * toolbar a user's eye goes looking for one.  Nothing was lost with it
     * — the menu item is unchanged, and on_menu_about still serves it.
     *
     * A GtkSearchEntry rather than a plain GtkEntry: it brings the
     * magnifier, the clear icon, Escape, and the typing-pause delay that
     * keeps a keystroke from rebuilding the pane.  It is NOT registered
     * with task_app_register_toolbar — that system swaps icons for
     * labels, and an entry has neither.                                    */
    lw->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(lw->search_entry),
                                   SEARCH_PLACEHOLDER);
    gtk_widget_set_tooltip_text(lw->search_entry, SEARCH_TOOLTIP);
    gtk_entry_set_width_chars(GTK_ENTRY(lw->search_entry), 18);
    /* 5 px of air between the box and the window edge (Notes' spacing).    */
    gtk_widget_set_margin_end(lw->search_entry, 5);
    g_signal_connect(lw->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), lw);
    g_signal_connect(lw->search_entry, "activate",
                     G_CALLBACK(on_search_changed), lw);
    g_signal_connect(lw->search_entry, "stop-search",
                     G_CALLBACK(on_search_stopped), lw);

    GtkToolItem *search_item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(search_item), lw->search_entry);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), search_item, -1);

    task_app_register_toolbar(app, toolbar);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    /* Thin rule between the toolbar and the panes (Notes look).           */
    lw->toolbar_rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), lw->toolbar_rule, FALSE, FALSE, 0);

    /* --- Paned: sidebar | tasks ------------------------------------------ */
    /* The panes sit in a GtkOverlay so Compact Layout's floating button
     * pair can hover over the bottom-right corner of the task area.  The
     * overlay wraps the panes rather than the whole window box so the
     * float never covers the status bar's event messages.                  */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(vbox), overlay, TRUE, TRUE, 0);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    /* A 6 px divider: wide-handle switches GtkPaned off its hairline
     * style, and the exact width comes from CSS on the handle's own
     * `separator` node (the horizontal paned's separator is vertical, so
     * min-WIDTH is the lever).                                             */
    gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
    task_app_widget_add_css(paned,
        "paned > separator { min-width: 6px; }");
    gchar *sbw = task_app_config_get("sidebar_width");
    lw->sb_width = sbw != NULL ? atoi(sbw) : 220;
    g_free(sbw);
    if (lw->sb_width <= 0)
        lw->sb_width = 220;
    gtk_paned_set_position(GTK_PANED(paned), lw->sb_width);
    g_signal_connect(paned, "notify::position",
                     G_CALLBACK(on_paned_position), lw);
    gtk_container_add(GTK_CONTAINER(overlay), paned);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), compact_bar_new(lw));

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

    /* Panel panes are built ON DEMAND by panel_widget(), not here: a view
     * can be registered at any time (enabling a plugin does it), so there
     * is no moment at which "every panel view" is a closed set.  The
     * table is keyed by the view itself for the same reason.             */
    lw->panels = g_hash_table_new(g_direct_hash, g_direct_equal);

    /* The Kanban board: three equal lanes side by side, 6 px apart, in
     * one outer scroller — the forecast's construction with the sections
     * turned through 90°.  Homogeneous so a lane holding one card is as
     * wide as a lane holding thirty; NEVER horizontally scrollable so the
     * board always fits the pane and only ever grows downwards.            */
    kanban_css_install();
    GtkWidget *board = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(board), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(board), 6);
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        gtk_box_pack_start(GTK_BOX(board),
                           kanban_lane_new(lw, (TaskStatus)s),
                           TRUE, TRUE, 0);
    lw->kanban_box = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->kanban_box),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lw->kanban_box), board);

    GtkWidget *task_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    lw->task_pane = task_pane;       /* panel_widget() packs into this     */
    gtk_box_pack_start(GTK_BOX(task_pane), lw->task_scroll,
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(task_pane), lw->kanban_box,
                       TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), task_pane, TRUE, FALSE);

    /* --- Status bar -------------------------------------------------------- */
    /* Same geometry as the Notes status bar: 8 px side margins,
     * 3 px top/bottom (a border_width would add a pixel more on every
     * edge and read visibly taller).                                       */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status, 8);
    gtk_widget_set_margin_end(status, 8);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);
    lw->status_left = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_left),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_left, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status), lw->status_left, TRUE, TRUE, 0);
    lw->status_right = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_right),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_right, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(status), lw->status_right, FALSE, FALSE, 0);
    /* Both labels 85% of the UI font (Notes size).  CSS font-size: 85%
     * can resolve to zero on Linux when the per-widget provider has no
     * explicit base size in scope; Pango scale attributes are always
     * relative to the actual rendered font and work on every platform.     */
    PangoAttrList *small_attrs = pango_attr_list_new();
    pango_attr_list_insert(small_attrs, pango_attr_scale_new(0.85));
    gtk_label_set_attributes(GTK_LABEL(lw->status_left),  small_attrs);
    gtk_label_set_attributes(GTK_LABEL(lw->status_right), small_attrs);
    pango_attr_list_unref(small_attrs);
    gtk_box_pack_end(GTK_BOX(vbox), status, FALSE, FALSE, 0);
    /* Matching thin rule above the status bar.                             */
    gtk_box_pack_end(GTK_BOX(vbox),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                     FALSE, FALSE, 0);

    /* --- Hooks + first population ------------------------------------------ */
    app->library_window = lw->window;
    g_object_set_data(G_OBJECT(lw->window), "task-library", lw);
    lw->listen_changed = task_app_listen_changed(app, notify_changed_hook,
                                                 NULL);
    lw->listen_tasks   = task_app_listen_tasks(app, notify_tasks_hook, NULL);
    lw->listen_status  = task_app_listen_status(app, notify_status_hook,
                                                NULL);
    g_signal_connect(lw->window, "destroy",
                     G_CALLBACK(on_library_destroy), lw);

    refresh_sidebar(lw);
    refresh_tasks(lw);
    gtk_widget_show_all(lw->window);
    /* show_all made the whole chrome visible — apply the persisted
     * Compact Layout state, which also settles the lists pane (HIDDEN by
     * default; the Sidebar button and View → Show Sidebar bring it back)
     * and hides the floating button pair outside compact mode.             */
    compact_layout_apply(lw);
    /* show_all made ALL THREE task-pane variants visible — put the
     * regular-list / Weekly Forecast / Kanban choice back.                 */
    task_pane_mode_apply(lw);
    /* show_all revealed every contributed toolbar button; let each one
     * answer for itself (see task_ui.h).  Same reason the pane choice is
     * re-applied above.                                                   */
    task_ui_tools_apply_visibility(lw->app);
    if (lw->ui_tool_rule != NULL)
        gtk_widget_set_visible(lw->ui_tool_rule,
                               task_ui_any_tool_visible(lw->app));
    return lw->window;
}
