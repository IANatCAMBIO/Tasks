#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksListsBar — a 28 pt horizontal scrolling strip of buttons for
 * selecting a task list or virtual view.
 *
 * Virtual views appear first, then standalone lists (not in any group),
 * then group buttons that open a dropdown of the group's member lists.
 * Clicking any item calls [library select:].
 */
@interface TasksListsBar : NSView

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
 * update_selection — reflect the window's current selection without
 * rebuilding the buttons.  Call this after a selection change that does
 * not alter the button list.
 */
- (void)update_selection;

/*
 * perform_command:param: — carry out one of the bar's commands.
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter, or nil
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * update_state:for_command: — adjust the default state of one of the
 * bar's commands.
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
