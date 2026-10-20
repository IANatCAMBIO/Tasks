#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksSidebar — the window's left column: the views (All Tasks, All
 * Favorites, 3-Day Forecast — task_view.h), then a "Tags" header and every
 * tag, alphabetically, as a source list.  While TASK_CFG_SIDEBAR_COUNTS is
 * on, each view shows how many open tasks it lists and each tag how many
 * carry it, at the row's right (as Notes' sidebar).  Selecting a row selects it in
 * the window through [library select:]; a row is selected on mouse-down
 * (SharedTable).
 *
 * The sidebar OWNS the Edit Tag panel (the "edit-tag" command): a tag's
 * emoji and its favorite flag.  A tag's NAME never changes, and a tag is
 * never made or deleted here: it exists while a task carries it.  A
 * right-click on a tag offers Edit Tag… and Clear from all Tasks (after a
 * confirmation, the tag comes off every task, so it leaves this sidebar; it
 * is deleted once no note carries it either); the command's other callers
 * (the main menu, the Favorites bar) reach it through the window.
 *
 * It does NOT own the selection — that lives on the window
 * (TasksLibraryWindow selection); a rebuild that finds the selected row
 * gone moves the selection by the sidebar's rules (task_sidebar.h).
 */
@interface TasksSidebar : NSView

/*
 * init_with_library: — build the sidebar.
 *
 * Inputs:
 *   library — the window hosting it; held weakly
 *
 * Output:
 *   the view; the window owns it.
 */
- (instancetype)init_with_library:(TasksLibraryWindow *)library NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithFrame:(NSRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

/*
 * reload — rebuild the rows from the database and reflect the window's
 * current selection, moving it (task_sidebar_pick) when its row has gone.
 */
- (void)reload;

/*
 * update_selection — reflect the window's current selection without
 * rebuilding the rows.
 */
- (void)update_selection;

/*
 * refresh_counts — re-read the tags' open-task counts (and whether they
 * are shown) and redraw them, leaving the rows and the selection as they
 * are.  For a task change that did not change the tags.
 */
- (void)refresh_counts;

/*
 * show_tag_panel: — the Edit Tag tool panel for a tag (the same panel as
 * Notes'): its name (fixed), an emoji slot, and Favorite.  OK writes the
 * two and refreshes the window; a failed write is reported in the panel.
 * Public so it can be driven without a menu.
 *
 * Inputs:
 *   tag_id — the tag; a tag that has gone shows nothing
 */
- (void)show_tag_panel:(int64_t)tag_id;

/*
 * tasks_sidebar_commands — the sidebar's command handlers (Edit Tag…, Add
 * to Favorites, Remove from Favorites, Clear from all Tasks: the parameter names the tag;
 * without one the command means the tag selected in the sidebar, and is
 * disabled when none is).  Their target is the sidebar.
 *
 * Inputs:
 *   n — receives how many
 *
 * Output: the table (static).
 */
const shared_command_handler_t *tasks_sidebar_commands(size_t *n);

/*
 * shut_down — the window is closing: stop responding.  Idempotent.
 */
- (void)shut_down;

@end
