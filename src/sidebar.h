#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * sidebar — the library window's left pane: the registered views
 * (Favorites, All Tasks, Due Today), the Lists header, list groups and
 * lists.
 *
 * The sidebar OWNS: the outline and its rebuilds (selection and expansion
 * restored), the list and group dialogs, its context menus, drag-reorder
 * of lists, and its own width fitting.  It does NOT own the selection —
 * that lives on the window (TasksLibraryWindow selection); the sidebar
 * reports a change with [library select:].
 *
 * Behavioural reference: porting/gtk_reference/sidebar.c.
 */
@interface TasksSidebar : NSView

/*
 * init_with_library: — build the pane.
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
 * reload — rebuild the rows from the database and show the window's
 * selection as selected.  If that row no longer exists, move the selection
 * (same kind, then the first list, then the first selectable row) and
 * report it with [library select:].  Exactly one row is selected
 * afterwards; the Lists header never is.
 */
- (void)reload;

/*
 * perform_command:param: — carry out one of the sidebar's commands
 * (TasksCommandOwnerSidebar in the command table).
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter (a group id), or nil
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * update_state:for_command: — adjust the default state of one of the
 * sidebar's commands (disable Edit List when no list is selected, ...).
 *
 * Inputs:
 *   state   — the state to adjust in place
 *   command — the command
 */
- (void)update_state:(TasksCommandState *)state for_command:(TasksCommand)command;

/*
 * shut_down — the window is closing: remove observers, stop timers.
 * Idempotent.
 */
- (void)shut_down;

@end
