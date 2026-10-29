#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "search.h"
#include "task_app.h"
#include "task_pane.h"
#include "task_rows.h"

@class SharedDayPlanner;

/*
 * library_window — the main window, and the contract between it and the
 * panes it hosts (the sidebar, the Favorites bar, the task list, the
 * Kanban board, the day planner).
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │ toolbar: Sidebar │ New  Delete │ Completed  Pane  Planner  search │
 *   ├─────────────────────────────────────────────────────────────────┤
 *   │ Favorites bar (TasksFavoritesBar): favorite tags, full width     │
 *   ├──────────┬────────────────────────────────────────────┬─────────┤
 *   │ sidebar  │ task pane: the task list OR the Kanban board│  day    │
 *   │ (Tasks-  │ (TasksTaskList / TasksKanban, one visible)  │ planner │
 *   │ Sidebar):│                                             │ (hidden │
 *   │ views,   │                                             │  by     │
 *   │ tags     │                                             │ default)│
 *   ├──────────┴────────────────────────────────────────────┴─────────┤
 *   │ <view> or #<tag>                            latest event message │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * In COMPACT mode (View > Compact Controls) the window shrinks to a
 * remembered small size, the toolbar, the sidebar and the Favorites bar
 * are hidden, and New
 * and Delete float as a two-button plate at the bottom-right of the task
 * pane.  porting/INTERFACES.md §11 is the specification.
 *
 * THE WINDOW OWNS THE SHARED STATE.  Which tag or view is selected, the
 * search and the Kanban flag all live here, each with ONE writer; panes
 * read them through the properties below and never keep copies.
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

@interface TasksLibraryWindow : NSWindowController <NSUserInterfaceValidations>

/*
 * init_with_app: — build the window (not yet shown), subscribe to the
 * app's four events, and show the default selection's tasks.
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

/* The kanban_view setting: YES while the board is the visible pane.
 * Single writer: toggle_pane (the "toggle-pane" command). */
@property (nonatomic, readonly) BOOL kanban_on;

/* The day planner panel (shared_day_planner.h; its delegate is a
 * TasksPlannerHost, day_planner.h); nil until build_layout. */
@property (nonatomic, readonly) SharedDayPlanner *day_planner;

/* Whether the day planner shows: its split item is not collapsed (the
 * toggle-day-planner command flips it, sliding it as the sidebar slides). */
@property (nonatomic, readonly) BOOL day_planner_shown;

/*
 * manual_sort_live — whether rows may be dropped into a hand-made order
 * right now: no search is active.  A search suspends hand-sorting because
 * saving an order from a filtered pane would discard every hidden row's
 * place (task_order.h).  The list refuses a drop onto itself and the board
 * skips the order half of a drop when this is NO; a drag may still start,
 * since the day planner takes one.
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
 *   selection — the new selection; never the Tags header
 */
- (void)select:(task_sel_t)selection;

/*
 * refresh_tasks — rebuild the task pane for the current selection and
 * search, and update the status bar's location line.  Cheap enough to call
 * after any change to tasks.
 */
- (void)refresh_tasks;

/*
 * selection_changed — the visible pane's selection changed under the user
 * (a click, a Shift- or Command-click, Select All, a drop): the status
 * bar's event message says how many tasks are selected, as Notes' does
 * for notes.  Nothing selected says nothing.  Not called for a refresh
 * putting the selection back.
 */
- (void)selection_changed;

/*
 * refresh_sidebar — rebuild the sidebar and the Favorites bar from the
 * database (a tag was first used or last dropped, or edited).
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
 * fill_menu:with_destinations_of: — rebuild the "Add Tag" submenu: one
 * item per tag, each carrying the tag's id as the command's parameter and
 * targeting this window.  The task context menu fills its submenu with
 * this when it is built; the main menu's submenu is filled each time it
 * opens, so it always shows the tags there are now.
 *
 * Inputs:
 *   menu  — the submenu; its items are replaced
 *   ident — "add-tag": every tag, less those a single selected task
 *           already carries.  Any other command leaves the menu empty.
 */
- (void)fill_menu:(NSMenu *)menu with_destinations_of:(NSString *)ident;

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
 * damaged, is not a stamped shared database (an old tasks.db), or lies
 * inside the GTK app's live data directory is refused with an alert giving
 * the core's reason, and nothing changes — the editors stay open and the
 * old database stays in use.
 * Otherwise the editors are closed (flushing their saves into the OLD
 * database), the databases are swapped, the workers re-armed, every pane
 * refreshed on the default selection, and the new database's health
 * reported.  If the file is not the default already
 * (task_switch_can_be_default) the user is then asked whether both apps
 * should open it at every launch; declining keeps it for this session
 * only, and a refusal by the shared module (while CAMBIO_SHARED_DB is set)
 * is shown in an alert in the module's words.
 *
 * Inputs:
 *   path — absolute path of the database file
 */
- (void)open_database:(NSString *)path;

/* ── Commands (see commands.h) ──────────────────────────────────────── */

/*
 * run_command: — carry out the command an item stands for: the app
 * delegate hands every command it does not own here.  The handler is
 * found in the tables of the window, the sidebar and the task list.
 *
 * Inputs:
 *   sender — an NSMenuItem, NSToolbarItem or button made by commands.h
 */
- (void)run_command:(id)sender;

/*
 * validateUserInterfaceItem: — whether an item's command can run now (the
 * app delegate asks for every item it does not own); the answer also
 * gives the item the command's current look (state_of_command:).
 *
 * Inputs:
 *   item — an item made from the command table
 *
 * Output:
 *   YES to enable it.
 */
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item;

/*
 * perform_command:param: — carry out a command without an item (the Quick
 * Task hotkey, the probe).
 *
 * Inputs:
 *   ident — the command's identifier
 *   param — its parameter, or nil
 */
- (void)perform_command:(NSString *)ident param:(id)param;

/*
 * state_of_command: — how a command looks right now: what validation
 * applies to its menu items, toolbar items and buttons.
 *
 * Inputs:
 *   ident — the command's identifier
 *
 * Output:
 *   new state: the table's defaults adjusted by the command's handler;
 *   disabled for a command no table has, or once the window is shut down.
 */
- (SharedCommandState *)state_of_command:(NSString *)ident;

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
