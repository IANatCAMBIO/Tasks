/* ===========================================================================
 * library_priv.h — the library window's state, shared by its four files.
 *
 * PRIVATE to library_window.c, sidebar.c, task_list.c and kanban.c: the
 * TaskLibrary struct and the handful of functions each part calls on the
 * others.  Nothing else includes it; the public face is library_window.h.
 * The shared functions carry a lib_ prefix so a reader of any one file can
 * tell a call into another part from a call to a local helper.
 * =========================================================================== */

#ifndef TASK_LIBRARY_PRIV_H
#define TASK_LIBRARY_PRIV_H

#include "library_window.h"
#include "task_view.h"
#include "task_rows.h"
#include "search.h"

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
                                      * list id (see task_view.h)          */
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
                                      * with the board (visibility)         */
    /* ---------------------------------------------------------------------
     * The Kanban board — the THIRD task-pane variant, one lane per
     * TaskStatus.  Lane INDEX IS the status value, which is what lets a
     * drop read its target status straight off the lane it landed on.
     *
     * GROUPED so that ownership is stated rather than remembered: these
     * are a THIRD of TaskLibrary's fields and NOTHING outside the board's
     * own section reads them, but until they were nested that was a
     * convention the compiler could not hold anyone to — a sidebar
     * handler poking card_mark_slot looked exactly like legitimate code.
     * `lw->board.` now says whose it is at every use.
     *
     * The members keep their kanban_/card_ prefixes even though the
     * struct name now repeats them.  That is deliberate: nesting alone is
     * a pure move the compiler verifies completely, and renaming on top
     * of it would mix a mechanical change with an editorial one.  Dropping
     * the prefixes later is its own equally mechanical step.
     * ------------------------------------------------------------------- */
    struct {
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
        gboolean      card_shadow;       /* the kanban_shadow config flag,
                                          * cached the same way: kanban_card_new
                                          * reads it PER CARD, and a board is
                                          * hundreds of them.
                                          * task_library_apply_kanban_shadow is
                                          * the single writer                   */
        gboolean      done_show_all;     /* the Done lane's "Show All" link has
                                          * been clicked.  TRANSIENT — not a
                                          * config key: it is reset whenever the
                                          * sidebar selection moves, so leaving a
                                          * list and coming back does not bring
                                          * a thousand completed cards with it  */
        GtkWidget    *kanban_drops[TASK_STATUS_N_VALUES];  /* lane hit boxes    */
        guint         kanban_counts[TASK_STATUS_N_VALUES]; /* what each lane
                                          * STOOD FOR at the last render (the
                                          * heading's number, not the number of
                                          * cards drawn — the Done lane is
                                          * capped).  Half of the test that
                                          * lets a refresh skip the rebuild;
                                          * see kanban_plan_matches            */
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
    } board;

    GtkWidget    *sidebar_box;       /* for the toolbar show/hide toggle    */
    GtkWidget    *toolbar;           /* hidden by Compact Layout            */
    GtkWidget    *toolbar_rule;      /* the thin rule under the toolbar     */
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
    GtkWidget    *sidebar_item;      /* lists-pane show/hide toggle button  */
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
    gint64               drag_task_id;   /* … and that row's task, so the
                                          * per-draw highlight can ask
                                          * "is this it?" without building
                                          * a GtkTreePath (see
                                          * task_row_bg_func)              */
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

/* ThemedCssFunc — build a widget's CSS from the resolved background color.
 * New string (the caller g_frees it).                                      */
typedef gchar *(*ThemedCssFunc)(const GdkRGBA *bg);

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

/* ---------------------------------------------------------------------------
 * The three builders task_library_window_new calls, in the order the
 * panes are packed.
 * ------------------------------------------------------------------------- */
void task_sidebar_build(TaskLibrary *lw, GtkWidget *paned);
void task_list_build(TaskLibrary *lw);
void task_kanban_build(TaskLibrary *lw);

/* ---------------------------------------------------------------------------
 * Cross-file calls.  Each is documented at its definition.
 * ------------------------------------------------------------------------- */
void lib_card_drag_stop(TaskLibrary *lw);
GArray *lib_card_sel_ids(TaskLibrary *lw);
void lib_full_refresh(TaskLibrary *lw);
gchar *lib_list_label(const TaskList *l);
gboolean lib_manual_sort_live(TaskLibrary *lw);
TaskLibrary *lib_of(TaskApp *app);          /* the TaskLibrary behind app->library_window */
void lib_on_new_list(GtkWidget *w, gpointer data);
void lib_on_toggle_sidebar(GtkWidget *widget, gpointer data);
guint lib_refresh_kanban(TaskLibrary *lw, GPtrArray *tasks, const TaskRowCtx *ctx);
void lib_refresh_sidebar(TaskLibrary *lw);
void lib_refresh_tasks(TaskLibrary *lw);
gchar *lib_rgb_of(const GdkRGBA *c);
gchar *lib_row_order_key(const gchar *family, gint kind, gint64 id);
void lib_row_order_keys_drop(gint kind, gint64 id);
gint *lib_row_order_permutation(const gint64 *ids, gint n, const gchar *saved);
void lib_scroll_keep_queue(GtkWidget *view);
void lib_scroll_keep_queue_win(GtkWidget *scroll);
const TaskView *lib_sel_view(TaskLibrary *lw);
gint64 lib_selected_list_id(TaskLibrary *lw);
gboolean lib_sidebar_show_pinned(TaskLibrary *lw);
void lib_sidebar_ui_sync(TaskLibrary *lw);

/* ---------------------------------------------------------------------------
 *lib_themed_bg_css_apply() — style `w` from the theme's @theme_bg_color, and
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
void lib_themed_bg_css_apply(GtkWidget *w, ThemedCssFunc build);
gboolean lib_view_refuse(TaskLibrary *lw, const gchar *alternative);
gboolean task_context_menu_popup(TaskLibrary *lw, GtkWidget *anchor,
                                 GdkEventButton *event);
void task_manual_sort_apply(TaskLibrary *lw);
void task_view_apply_manual_order(TaskLibrary *lw);

#endif /* TASK_LIBRARY_PRIV_H */
