#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksListsBar — a 28 pt horizontal scrolling strip of buttons for
 * selecting a task list or virtual view.
 *
 * Virtual views appear first, then group buttons that open a menu of All
 * Tasks (the group's aggregate view) and the group's member lists (with
 * Rename Group and Remove Group below them),
 * then the lists in no group.  Clicking any item calls [library select:].
 *
 * The bar OWNS: the list and group sheets (New List, Edit List, New Group,
 * Rename Group), the confirmations of Delete List and Remove Group, Move
 * to Group / Remove from Group, its context menus (a right-click on a
 * button, or on the empty space; a group's menu carries the commands of
 * its selected list, which has no button of its own), and the drag of a
 * list's button along the bar — in front of or after another list, or
 * onto a group to join it — which saves the lists' one order
 * (task_sidebar.h, task_db_lists_reorder).  It does NOT own the selection
 * — that lives on the window (TasksLibraryWindow selection); the bar
 * reports a change with [library select:], and moves a selection whose
 * row has gone by the sidebar's rules (task_sidebar.h).
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
 * drop_list:at: — what dropping a list at a point of the bar does: on a
 * group's button the list joins that group at its end; anywhere else it
 * lands among the ungrouped lists, in front of the list whose button's
 * middle is the first to the right of the point (the left half of a
 * button means before it, the right half after it; the space past the
 * last button means the end).  The group change and the lists' new order
 * are saved, and the window refreshed on the next run-loop pass.  The
 * drag's destination methods call this; it is public so the drop can be
 * exercised without a pointer.
 *
 * Inputs:
 *   list_id — the list, grouped or not
 *   point   — the drop point, in the bar's coordinates
 *
 * Output:
 *   YES when the list moved; NO for a drop that would leave it where it
 *   is, or an unknown list.
 */
- (BOOL)drop_list:(int64_t)list_id at:(NSPoint)point;

/*
 * menu_for_group: — the menu a click on a group's button pops up: All
 * Tasks (the group itself: every member list's tasks together), the
 * group's lists, then Rename Group and Remove Group.  The item for the
 * current selection carries a check mark.  Public so the menu can be
 * exercised without popping it up.
 *
 * Inputs:
 *   group_id — the group
 *
 * Output:
 *   the menu; empty once the bar is shut down.
 */
- (NSMenu *)menu_for_group:(int64_t)group_id;

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
