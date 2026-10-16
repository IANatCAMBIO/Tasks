#import <Cocoa/Cocoa.h>
#import "library_window.h"

/*
 * task_list — the task pane's LIST presentation: a table of tall rows
 * (done checkbox, the row text, status, due date, completion date).
 *
 * The list OWNS: its columns and their visibility menu, header sorting,
 * row dragging and its saved order (TASK_CFG_FAMILY_MANUAL), the
 * checkbox's in-place update, double-click to open the editor, and its
 * selection.  Header sort and drag coexist: a drag clears the active sort
 * so the dragged position sticks; a header click re-sorts from the saved
 * order.  The row text comes from row_text.h and nowhere else.
 *
 * Behavioural reference: porting/gtk_reference/task_list.c.
 */
/* The pasteboard type of a dragged row: one item per row, its content the
 * task id as decimal text.  Read by the list's own drop (a reorder) and by
 * the day planner (a scheduling). */
extern NSPasteboardType const tasks_row_drag_type;

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

@end

/*
 * tasks_task_list_commands — the list's command handlers: the
 * column-visibility toggles, check-marked while the column shows.  Their
 * target is the list.
 *
 * Inputs:
 *   n — receives how many
 *
 * Output: the table (static).
 */
const shared_command_handler_t *tasks_task_list_commands(size_t *n);
