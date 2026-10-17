#import <Cocoa/Cocoa.h>
#import "commands.h"
#include "task_pane.h"

@class TasksLibraryWindow;

/*
 * TasksFavoritesBar — a 28 pt horizontal scrolling strip across the whole
 * window, under the toolbar (the sidebar and the tasks start below it),
 * with one button per FAVORITE tag (TaskTag.favorite; SharedFavoriteButton)
 * in the order the user dragged them into (TASK_CFG_FAVORITES_ORDER), new
 * favorites at the end by name.  Clicking a button selects its tag through
 * [library select:]; the button of the selected tag is lit.  Dragging a
 * button along the bar reorders it.  A right-click on a button offers Edit
 * Tag… (its emoji, the "edit-tag" command) and Remove from Favorites.
 * With no favorite tag the bar says how to make one.
 *
 * It does NOT own the selection — that lives on the window
 * (TasksLibraryWindow selection) — nor the tags: they are the sidebar's
 * (sidebar.h) and the database's.
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
 * update_selection — reflect the window's current selection without
 * rebuilding the buttons.
 */
- (void)update_selection;

/*
 * shut_down — the window is closing: stop responding.  Idempotent.
 */
- (void)shut_down;

@end
