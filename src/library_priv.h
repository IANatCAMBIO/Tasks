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
#include "list_rows.h"
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

/* Sidebar row kinds (TaskSbRow.kind).                                      */
enum {
    SB_KIND_VIEW = 0,                /* a registered virtual view; the row's
                                      * id holds its registry INDEX, not a
                                      * list id (see task_view.h)          */
    SB_KIND_HEADER,                  /* the "Lists" section header          */
    SB_KIND_LIST,                    /* a real list                         */
    SB_KIND_GROUP                    /* a list-group sub-header             */
};

/* ---------------------------------------------------------------------------
 * TaskLibrary — the window's state.
 *   sel_kind/sel_id — current sidebar selection (survives refreshes).
 *   populating      — guards the sidebar selection handler during rebuilds.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp             *app;
    GtkWidget           *window;     /* a GtkApplicationWindow              */

    /* --- Sidebar (sidebar.c) — a GtkListView over a GtkTreeListModel --- */
    GListStore          *sb_store;   /* the top-level TaskSbRows; a row's own
                                      * `children` store is what expands   */
    GtkTreeListModel    *sb_tree;    /* the flattened tree the view shows  */
    GtkSingleSelection  *sb_sel;     /* one row, never none, header rows
                                      * refused by reverting                */
    GtkWidget           *sb_view;    /* the GtkListView                     */
    GtkWidget           *sidebar_box;/* the pane the toolbar toggle shows   */

    /* --- Task list (task_list.c) — a GtkColumnView over a GListStore ---- */
    GListStore          *task_store; /* TaskRows, rebuilt per refresh as ONE
                                      * splice; empty while the board is up */
    GtkSortListModel    *task_sorted;/* driven by the column view's sorter  */
    GtkMultiSelection   *task_sel;   /* Ctrl/Cmd- and Shift-click extend    */
    GtkWidget           *task_view;  /* the GtkColumnView                   */
    GtkWidget           *task_scroll;/* its scroller: the regular task pane,
                                      * swapped with the board (visibility) */
    GtkColumnViewColumn *col_drag;   /* the ⠿ handle column, visible only
                                      * in manual sort mode                 */
    gboolean             manual_sort;/* task_list_manual_sort, cached: read
                                      * per motion event and per refresh;
                                      * task_manual_sort_apply is the
                                      * single writer                       */
    /* The manual-sort row drag: hand-rolled on a GtkGestureDrag on the
     * handle cell (see task_list.c).  The row is not moved until the
     * release; the marker says where it would land.                       */
    gboolean             drag_active;/* a row drag is in progress           */
    gint64               drag_task_id;/* the task being dragged            */
    guint                drag_from;  /* its position in task_store          */
    gint                 drag_mark_pos;/* where the marker sits: the index
                                      * the row would take, -1 for none     */
    GtkWidget           *drag_mark_row;/* the row widget wearing the marker
                                      * class, or NULL                      */
    GdkCursor           *drag_cursor;/* the "ns-resize" cursor, made once   */

    /* --- The Kanban board (kanban.c) — the THIRD task-pane variant ------- */
    /* One lane per TaskStatus; lane INDEX IS the status value, which is
     * what lets a drop read its target status straight off the lane it
     * landed on.  GROUPED so ownership is stated: nothing outside the
     * board's own file touches these.  The card drag is hand-rolled on a
     * GtkGestureDrag on the ⠿ grip; the ghost is an OVERLAY CHILD (a
     * GtkPicture of a static snapshot of the card, on `ghost_layer`), and
     * every position is in the OVERLAY's coordinate space.               */
    struct {
        GtkWidget    *kanban_box;    /* the board's outer scroller          */
        GtkWidget    *kanban_labels[TASK_STATUS_N_VALUES];  /* lane headings */
        GtkWidget    *kanban_lanes[TASK_STATUS_N_VALUES];   /* card boxes   */
        GtkWidget    *kanban_drops[TASK_STATUS_N_VALUES];   /* lane bodies:
                                      * the drop targets, hit-tested by
                                      * bounds in the overlay's space       */
        guint         kanban_counts[TASK_STATUS_N_VALUES];  /* what each lane
                                      * STOOD FOR at the last render (the
                                      * heading's number); half of the test
                                      * that lets a refresh skip the rebuild */
        GHashTable   *kanban_sel;    /* SET of selected task ids (keys are
                                      * GSIZE_TO_POINTER'd) — the board's
                                      * answer to the list's multi-selection.
                                      * Created with the window; never NULL */
        gint64        kanban_anchor; /* last plainly-clicked card: the fixed
                                      * end of a shift-click range          */
        gboolean      kanban;        /* the kanban_view config flag, cached;
                                      * on_toggle_kanban is the single writer*/
        gboolean      card_shadow;   /* the kanban_shadow config flag,
                                      * cached: kanban_card_new reads it PER
                                      * CARD; task_library_apply_kanban_shadow
                                      * is the single writer                */
        gboolean      done_show_all; /* the Done lane's "Show All" link was
                                      * clicked.  TRANSIENT — reset when the
                                      * sidebar selection moves             */
        GdkCursor    *card_grab;     /* "grab" — hovering a grip            */
        GdkCursor    *card_grabbing; /* "grabbing" — dragging one.  Both made
                                      * ONCE and kept: a card is realized per
                                      * refresh                             */
        GtkWidget    *card_drag_src; /* the card in flight, or NULL         */
        gint64        card_drag_id;  /* its task                            */
        gboolean      card_dragging; /* past the threshold                  */
        gdouble       card_hot_x;    /* pointer offset inside the card, so  */
        gdouble       card_hot_y;    /* the ghost sits where it was gripped */
        gdouble       card_press_x;  /* press position in OVERLAY coords —  */
        gdouble       card_press_y;  /* the threshold is measured from it   */
        GtkWidget    *card_ghost;    /* the translucent copy on ghost_layer,
                                      * or NULL                             */
        GtkWidget    *card_mark;     /* insertion marker, or NULL           */
        gint          card_mark_lane;/* where the marker currently sits —   */
        gint          card_mark_slot;/* only a CHANGE moves it              */
        GtkEventController *card_key;/* the Escape controller on the window,
                                      * added at drag start, removed at
                                      * card_drag_stop                      */
    } board;
    GtkWidget           *overlay;    /* the GtkOverlay around the paned,
                                      * host of the compact float bar AND
                                      * of ghost_layer                      */
    GtkWidget           *ghost_layer;/* a GtkFixed overlay child, can_target
                                      * FALSE, that the board's ghost is
                                      * moved around on                     */

    /* --- Chrome (library_window.c) --------------------------------------- */
    GtkWidget           *toolbar;    /* a GtkBox.toolbar; hidden by Compact
                                      * Controls                            */
    GtkWidget           *toolbar_rule;/* the thin rule under it             */
    GtkWidget           *float_bar;  /* Compact Controls' floating New /
                                      * Delete pair (overlay child)         */
    GtkWidget           *search_entry;/* the toolbar's search box           */
    TaskSearch          *search;     /* its parsed query, or NULL for "no
                                      * filter" — the ONE test for whether a
                                      * search is active (see search.h)     */
    GtkWidget           *status_left;/* selection info label                */
    GtkWidget           *status_right;/* latest event message label         */
    GtkWidget           *status_reveal;/* the GtkRevealer around it: shown
                                      * on a message, crossfaded away after
                                      * the hold                            */
    guint                status_hide_source;/* the hold timer, or 0         */
    guint                listen_changed;/* TaskApp event subscriptions —    */
    guint                listen_tasks;  /* dropped in on_library_destroy    */
    guint                listen_status; /* BEFORE the editors close         */
    GtkWidget           *sidebar_item;  /* lists-pane show/hide button      */
    GtkWidget           *hide_done_item;/* completed-visibility button      */
    GtkWidget           *manual_sort_item;/* manual-sort mode button        */
    GtkWidget           *pane_item;     /* list <-> Kanban pane button      */
    gint                 sel_kind;
    gint64               sel_id;
    gboolean             populating;
    gboolean             sb_populated;  /* first population expands Lists   */
    gboolean             pinned_row_shown;/* Favorites row exists (hidden
                                      * while nothing is pinned)            */
    GHashTable          *group_expanded;/* group id (ptr) → expanded gboolean*/
    gint                 sb_width;      /* live divider position (persisted
                                      * at close as sidebar_width)          */
    gint                 win_w, win_h;  /* live size (persisted at close)   */
} TaskLibrary;

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
 * Actions.  Every command is a GAction: the menubar names "app." actions
 * (they must work whichever window is focused — on macOS the native bar
 * is the only bar), the toolbar and the context menus name "win." actions
 * on the library window.  Menus, buttons and shortcuts only NAME actions;
 * there are no GtkMenuItem callbacks and no toolbar "clicked" handlers.
 * A dynamic menu label is TWO items with hidden-when=action-disabled, and
 * lib_menu_pair_sync is what decides which one is on offer.
 * ------------------------------------------------------------------------- */

/* LibCommand — one parameterless command: the action name (without its
 * "app." / "win." prefix) and what it does for the library window.        */
typedef struct {
    const gchar *name;
    void       (*run)(TaskLibrary *lw);
} LibCommand;

/* lib_win_commands_install() — add `n` commands from `table` to the
 * window's "win." group.  `table` must outlive the window (a static).     */
void lib_win_commands_install(TaskLibrary *lw, const LibCommand *table,
                              gsize n);

/* lib_win_action_add() — add one PARAMETERISED "win." action; `activate`
 * has the GSimpleAction "activate" signature and receives `lw`.          */
void lib_win_action_add(TaskLibrary *lw, const gchar *name,
                        const GVariantType *type, GCallback activate);

/* lib_menu_pair_sync() — a hidden-when PAIR of "app." items: enable
 * `when_on` and disable `when_off` while `on`, the reverse otherwise, so
 * exactly one of the two is ever on the menu.                             */
void lib_menu_pair_sync(TaskLibrary *lw, const gchar *when_on,
                        const gchar *when_off, gboolean on);

/* lib_app_action_set_enabled() — grey (or ungrey) one "app." action.     */
void lib_app_action_set_enabled(TaskLibrary *lw, const gchar *name,
                                gboolean enabled);

/* The parts' own actions, installed by task_library_window_new before the
 * panes are built.                                                        */
void task_sidebar_install_actions(TaskLibrary *lw);
void task_list_install_actions(TaskLibrary *lw);

/* ---------------------------------------------------------------------------
 * Cross-file calls.  Each is documented at its definition.
 * ------------------------------------------------------------------------- */
void lib_card_drag_stop(TaskLibrary *lw);
GArray *lib_card_sel_ids(TaskLibrary *lw);
void lib_full_refresh(TaskLibrary *lw);
gchar *lib_list_label(const TaskList *l);
gboolean lib_manual_sort_live(TaskLibrary *lw);
TaskLibrary *lib_of(TaskApp *app);          /* the TaskLibrary behind app->library_window */
void lib_on_new_list(TaskLibrary *lw);
void lib_on_toggle_sidebar(TaskLibrary *lw);
guint lib_refresh_kanban(TaskLibrary *lw, GPtrArray *tasks, const TaskRowCtx *ctx);
void lib_refresh_sidebar(TaskLibrary *lw);
void lib_refresh_tasks(TaskLibrary *lw);
gchar *lib_row_order_key(const gchar *family, gint kind, gint64 id);
void lib_row_order_keys_drop(gint kind, gint64 id);
gint *lib_row_order_permutation(const gint64 *ids, gint n, const gchar *saved);
void lib_scroll_keep_queue(GtkWidget *view);
void lib_scroll_keep_queue_win(GtkWidget *scroll);
const TaskView *lib_sel_view(TaskLibrary *lw);
gint64 lib_selected_list_id(TaskLibrary *lw);
gboolean lib_sidebar_show_pinned(TaskLibrary *lw);
void lib_sidebar_ui_sync(TaskLibrary *lw);

gboolean lib_view_refuse(TaskLibrary *lw, const gchar *alternative);
/* task_context_menu_popup() — the task context menu for the CURRENT
 * selection, at (x, y) in `at`'s coordinates (a GtkGestureClick press).
 * Returns TRUE when a menu was shown.                                     */
gboolean task_context_menu_popup(TaskLibrary *lw, GtkWidget *at,
                                 gdouble x, gdouble y);
void task_manual_sort_apply(TaskLibrary *lw);
void task_view_apply_manual_order(TaskLibrary *lw);

#endif /* TASK_LIBRARY_PRIV_H */
