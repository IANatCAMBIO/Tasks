#import <Cocoa/Cocoa.h>
#import "library_window.h"

/*
 * task_list — the task pane's LIST presentation: a table of tall rows
 * (done checkbox, the row text, status, due date, completion date).
 *
 * The list OWNS: its columns and their visibility menu, header sorting,
 * the manual-sort drag and its saved order (TASK_CFG_FAMILY_MANUAL), the
 * checkbox's in-place update, double-click to open the editor, and its
 * selection.  The row text comes from row_text.h and nowhere else.
 *
 * Behavioural reference: porting/gtk_reference/task_list.c.
 */
@interface TasksTaskList : NSView <TasksTaskPane>

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
 * perform_command:param: — carry out one of the list's commands (the
 * column-visibility toggles, TasksCommandOwnerTaskList).
 *
 * Inputs:
 *   command — the command
 *   param   — unused by the current commands
 */
- (void)perform_command:(TasksCommand)command param:(NSNumber *)param;

/*
 * update_state:for_command: — adjust the default state of one of the
 * list's commands (the check mark of a visible column).
 *
 * Inputs:
 *   state   — the state to adjust in place
 *   command — the command
 */
- (void)update_state:(TasksCommandState *)state for_command:(TasksCommand)command;

@end
