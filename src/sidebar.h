#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksSidebar — the window's left column: the views (Favorites, All
 * Tasks, 3-Day Forecast — task_view.h), then a "Tags" header and every
 * tag, alphabetically, as a source list.  Selecting a row selects it in
 * the window through [library select:]; a row is selected on mouse-down
 * (TasksTable).
 *
 * The sidebar OWNS the Edit Tag sheet (TasksCommandEditTag): a tag's
 * emoji and its favorite flag.  A tag's NAME never changes, and a tag is
 * never made or deleted here: it exists while a task carries it.  A
 * right-click on a tag offers Edit Tag…; the command's other callers (the
 * main menu, the Favorites bar) reach it through the window.
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
 * show_tag_sheet: — the Edit Tag sheet for a tag: its name (fixed), an
 * emoji slot, and Favorite.  Saving writes the two and refreshes the
 * window.  Public so it can be driven without a menu.
 *
 * Inputs:
 *   tag_id — the tag; a tag that has gone shows nothing
 */
- (void)show_tag_sheet:(int64_t)tag_id;

/*
 * perform_command:param: — carry out one of the sidebar's commands.
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter: the tag (the window gives a command sent
 *             without one the selected tag, commands.h)
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * shut_down — the window is closing: stop responding.  Idempotent.
 */
- (void)shut_down;

@end
