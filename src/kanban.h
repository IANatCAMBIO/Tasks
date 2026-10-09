#import <Cocoa/Cocoa.h>
#import "library_window.h"

/*
 * kanban — the task pane's BOARD presentation: three lanes, New / In
 * Progress / Done, of cards.  The lane index IS the TaskStatus value.
 *
 * The board OWNS: lanes and cards, its selection set, the card drag (ghost,
 * insertion marker, Escape to cancel), what a drop does (the status change
 * and, separately, the saved card order, TASK_CFG_FAMILY_KANBAN), the Done
 * lane's cap, and the card shadow (the kanban_shadow setting, re-read on
 * every show_rows:).  A card's text comes from row_text.h and nowhere else.
 *
 * Behavioural reference: porting/gtk_reference/kanban.c.
 */
/* The pasteboard type of a card drag: one item, its content the dragged
 * task ids as comma-separated decimal text.  The board's own drop goes by
 * its drag_ids; the day planner reads the ids when a card is dropped on
 * it (a scheduling). */
extern NSPasteboardType const tasks_card_drag_type;

@interface TasksKanban : NSView <TasksTaskPane>

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
