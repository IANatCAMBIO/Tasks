#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "search.h"
#include "task_app.h"
#include "task_pane.h"
#include "task_rows.h"

@class TasksDayPlanner;

/*
 * library_window — the main window, and the contract between it and the
 * three panes it hosts (the sidebar, the task list, the Kanban board).
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ toolbar: New  Delete │ Sidebar  Completed  Sort  Pane  search │
 *   ├───────────────┬──────────────────────────────────────────────┤
 *   │ sidebar       │ task pane: the task list OR the Kanban board  │
 *   │ (TasksSidebar)│ (TasksTaskList / TasksKanban, one visible)    │
 *   ├───────────────┴──────────────────────────────────────────────┤
 *   │ <view> - N tasks                         latest event message │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * In COMPACT mode (View > Compact Controls) the window shrinks to a
 * remembered small size, the toolbar and the sidebar are hidden, and New
 * and Delete float as a two-button plate at the bottom-right of the task
 * pane.  porting/INTERFACES.md §11 is the specification.
 *
 * THE WINDOW OWNS THE SHARED STATE.  Which sidebar row is selected, the
 * search, the manual-sort and Kanban flags all live here, each with ONE
 * writer; panes read them through the properties below and never keep
 * copies.
 *
 * THE WINDOW OWNS THE REFRESH.  A pane never queries the database for the
 * rows it shows: refresh_tasks collects the selection's tasks through the
 * core (task_pane.h), filters them by the search, and hands the same rows
 * to whichever pane is visible.  That is what gives the board the search
 * filter and the completed-visibility rule for nothing.
 *
 * Everything here is main thread only.
 */

@class TasksLibraryWindow;

/*
 * TasksTaskPane — what the window needs from a pane that shows tasks.  The
 * task list and the Kanban board both implement it, and the window talks
 * to whichever one is visible through it.  (The app's one custom protocol:
 * two interchangeable classes behind a single type.)
 */
@protocol TasksTaskPane <NSObject>

/*
 * show_rows: — replace what the pane shows.
 *
 * Inputs:
 *   rows — the rows to show, in query order; the pane TAKES OWNERSHIP and
 *          frees them with task_row_list_free when it replaces them or
 *          shuts down.  NULL means the pane is being hidden: drop
 *          everything, selection included.
 *
 * The pane applies its own saved order (task_order.h), re-reads the
 * settings it draws by (so a Settings change shows on the next refresh),
 * keeps its selection and scroll position where the same tasks are still
 * present, and must cope with being called from inside one of its own
 * event handlers (defer any teardown of the view that is handling the
 * event to the next run-loop pass).
 */
- (void)show_rows:(task_row_list_t *)rows;

/*
 * selected_task_ids — the tasks every command acts on.
 *
 * Output:
 *   the selected task ids in display order; empty when nothing is selected.
 */
- (NSArray<NSNumber *> *)selected_task_ids;

/*
 * view_changed — the sidebar selection moved to another row.  Drop what
 * belonged to the old view: the selection, a scroll position, the board's
 * "show all completed" expansion.  refresh_tasks follows.
 */
- (void)view_changed;

/*
 * cancel_drag — abandon any drag in progress, restoring the pane as it was.
 * Called before the window tears down and before a pane is hidden.
 */
- (void)cancel_drag;

/*
 * shut_down — the window is closing: free the rows, stop timers, remove
 * observers.  Idempotent.  Nothing may call back into the window after it.
 */
- (void)shut_down;

@end

@interface TasksLibraryWindow : NSWindowController <NSMenuItemValidation>

/*
 * init_with_app: — build the window (not yet shown), subscribe to the
 * app's three events, and show the default selection's tasks.
 *
 * Inputs:
 *   app — the application context, its database open; borrowed, and must
 *         outlive the window (the app delegate frees it after shut_down)
 *
 * Output:
 *   the controller; the app delegate owns it.
 */
- (instancetype)init_with_app:(TaskApp *)app NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithWindow:(NSWindow *)window NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;
- (instancetype)init NS_UNAVAILABLE;

/* ── Shared state (read-only to the panes) ──────────────────────────── */

/* The application context: the database and the event bus. */
@property (nonatomic, readonly) TaskApp *app;

/* The sidebar selection.  Single writer: select:. */
@property (nonatomic, readonly) task_sel_t selection;

/* The parsed search, or NULL for "no filter" — the ONE test for whether a
 * search is active.  Single writer: the search field's action. */
@property (nonatomic, readonly) const TaskSearch *search;

/* The task_list_manual_sort setting.  Single writer: TasksCommandToggleSort. */
@property (nonatomic, readonly) BOOL manual_sort;

/* The kanban_view setting: YES while the board is the visible pane.
 * Single writer: TasksCommandTogglePane. */
@property (nonatomic, readonly) BOOL kanban_on;

/* The day planner panel; nil until build_layout. */
@property (nonatomic, readonly) TasksDayPlanner *day_planner;

/*
 * manual_sort_live — whether rows may be dragged into a hand-made order
 * right now: manual sort is on AND no search is active.  A search suspends
 * hand-sorting because saving an order from a filtered pane would discard
 * every hidden row's place (task_order.h).  The list hides its drag handle
 * and the board skips the order half of a drop when this is NO.
 *
 * Output:
 *   YES when an order may be saved.
 */
- (BOOL)manual_sort_live;

/*
 * selected_task_ids — the visible pane's selection; what every task command
 * acts on, whichever pane is up.
 *
 * Output:
 *   task ids in display order; empty when nothing is selected.
 */
- (NSArray<NSNumber *> *)selected_task_ids;

/* ── What the panes call ────────────────────────────────────────────── */

/*
 * select: — the sidebar's selection changed (the user clicked a row, or a
 * rebuild had to move the selection).  Records it, tells both panes
 * (view_changed) and refreshes the task pane.  A selection equal to the
 * current one only refreshes.
 *
 * Inputs:
 *   selection — the new selection; never the Lists header
 */
- (void)select:(task_sel_t)selection;

/*
 * refresh_tasks — rebuild the task pane for the current selection and
 * search, and update the status bar's count.  Cheap enough to call after
 * any change to tasks.
 */
- (void)refresh_tasks;

/*
 * refresh_sidebar — rebuild the sidebar from the database (lists or groups
 * were created, renamed, moved or deleted).
 */
- (void)refresh_sidebar;

/*
 * full_refresh — the sidebar, the task pane and every open editor.  What
 * the app's "changed" event does.
 */
- (void)full_refresh;

/*
 * task_context_menu — the context menu for the CURRENT selection, shared
 * by the list and the board so the two cannot drift apart.  A pane first
 * makes the clicked row or card part of the selection (a right-click
 * inside the selection keeps it; outside collapses to the clicked one),
 * then returns this from menuForEvent:.
 *
 * Output:
 *   a new menu whose items target this window, or nil when nothing is
 *   selected.
 */
- (NSMenu *)task_context_menu;

/*
 * fill_menu:with_destinations_of: — rebuild a "Move to" submenu: one item
 * per place the command can send things, each carrying that place's id as
 * the command's parameter and targeting this window.  The task context
 * menu fills its submenu with this when it is built; the main menu's
 * submenus are filled each time they open, so they always show the lists
 * and groups there are now.
 *
 * Inputs:
 *   menu    — the submenu; its items are replaced
 *   command — TasksCommandMoveToList: every list, less the list a single
 *             selected task is already in.  TasksCommandMoveToGroup: every
 *             group.  Any other command leaves the menu empty.
 */
- (void)fill_menu:(NSMenu *)menu with_destinations_of:(TasksCommand)command;

/* ── The database ───────────────────────────────────────────────────── */

/*
 * report_database_health — run the integrity check on the open database
 * and say what it found: a status message when it is sound, an alert on
 * this window when it is not.  Called after every open — at launch by the
 * app delegate, and by open_database: — because the check is the only
 * thing that catches a file gone bad before more is written into it.
 */
- (void)report_database_health;

/*
 * open_database: — switch the running app to another database file (what
 * File > Open Database File... does with the file the user picked).
 *
 * The file is checked first, read-only (task_switch.h): one that is
 * damaged, is not a database, or lies inside the GTK app's live data
 * directory is refused with an alert giving the core's reason, and nothing
 * changes — the editors stay open and the old database stays in use.
 * Otherwise the editors are closed (flushing their saves into the OLD
 * database), the databases are swapped, the workers re-armed, every pane
 * refreshed on the default selection, and the new database's health
 * reported.  If the file can be the default (it is named tasks.db and is
 * not the default already) the user is then asked whether to open it at
 * every launch; declining keeps it for this session only.
 *
 * Inputs:
 *   path — absolute path of the database file
 */
- (void)open_database:(NSString *)path;

/* ── Commands (see commands.h) ──────────────────────────────────────── */

/*
 * run_command: — the action every command control sends.  Reads the
 * command from the sender's tag and its parameter from the sender's
 * representedObject, and performs it.
 *
 * Inputs:
 *   sender — an NSMenuItem or NSToolbarItem made by commands.h
 */
- (void)run_command:(id)sender;

/*
 * perform_command:param: — carry out a command, dispatching to its owner.
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter, or nil
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * state_of_command: — how a command looks right now: the single answer
 * both menu and toolbar validation use.
 *
 * Inputs:
 *   command — the command
 *
 * Output:
 *   new state: the table's defaults adjusted by the command's owner.
 */
- (TasksCommandState *)state_of_command:(TasksCommand)command;

/* ── Lifetime ───────────────────────────────────────────────────────── */

/*
 * shut_down — tear the window's connections down, in the order that
 * matters: persist the window's place and size (to the Compact size as
 * well when it closes in Compact mode); drop the app-event listeners; cancel
 * any drag; close every editor (each flushes its pending save); shut the
 * panes down.  The listeners go BEFORE the editors because a closing
 * editor's final save fires the task event, which must not reach a window
 * in mid-teardown.  Called when the window closes and again at quit;
 * idempotent.
 */
- (void)shut_down;

@end
