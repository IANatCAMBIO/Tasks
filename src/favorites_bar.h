#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksFavoritesBar — a 28 pt horizontal scrolling strip of buttons for
 * selecting a task list or virtual view.  Replaces the sidebar in the
 * favorites-bar experiment branch.
 *
 * Virtual views appear first, then standalone lists (not in any group),
 * then group buttons that open a dropdown of the group's member lists.
 * Clicking any item calls [library select:].
 */
@interface TasksFavoritesBar : NSView

/*
 * init_with_library: — build the bar.
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
 * reload — rebuild the buttons from the database and reflect the window's
 * current selection.
 */
- (void)reload;

/*
 * perform_command:param: — carry out one of the sidebar's commands.
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter, or nil
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * update_state:for_command: — adjust the default state of one of the
 * sidebar's commands.
 *
 * Inputs:
 *   state   — the state to adjust in place
 *   command — the command
 */
- (void)update_state:(TasksCommandState *)state for_command:(TasksCommand)command;

/*
 * shut_down — the window is closing: stop responding.  Idempotent.
 */
- (void)shut_down;

@end
