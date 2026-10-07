#import <Cocoa/Cocoa.h>
#import "library_window.h"

/*
 * day_planner — a right-side panel showing the hours of the day.  Tasks
 * dragged onto it are scheduled at that hour; existing blocks can be
 * stretched from their bottom edge to adjust the session length.  A
 * horizontal line tracks the current time.  Tasks whose scheduled time
 * arrives are moved to In Progress if they are not already there or Done.
 */
@interface TasksDayPlanner : NSView

/*
 * init_with_library: — build the panel.
 *
 * Inputs:
 *   library — the window hosting it; held weakly
 *
 * Output:
 *   the view; the split view owns it.
 */
- (instancetype)init_with_library:(TasksLibraryWindow *)library NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithFrame:(NSRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

/*
 * reload — re-read the day's scheduled tasks from the database and
 * redraw.  Called on full_refresh and after any schedule change.
 */
- (void)reload;

/*
 * scroll_to_now — scroll the view so the current time is visible.
 */
- (void)scroll_to_now;

/*
 * shut_down — stop timers and release resources.  Idempotent.
 */
- (void)shut_down;

/* The scheduled items array, read by the canvas during drawing. */
@property (nonatomic, readonly) NSMutableArray *items;

/* The hosting library window. */
@property (nonatomic, readonly, weak) TasksLibraryWindow *library;

@end
