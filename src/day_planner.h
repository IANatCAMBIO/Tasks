#import <Cocoa/Cocoa.h>
#import "library_window.h"
#import "shared_day_planner.h"

/*
 * day_planner — Tasks' side of the day planner (shared_day_planner.h, the
 * right-side panel showing the hours of the day).  Tasks dragged onto it
 * from the list or the board are scheduled at that hour; blocks can be
 * stretched from either edge or dragged to move the session; a
 * double-click opens the task's editor; the day's events from the macOS
 * Calendar (shared_calendar.h) are drawn while the calendar is switched
 * on.  Every write kicks the calendar sync.  Moving a task to In Progress
 * when its session begins is the notification worker's job
 * (task_notify.h), which runs whether or not the panel is on screen.
 */
@interface TasksPlannerHost : NSObject <SharedDayPlannerDelegate>

/*
 * init_with_library: — build the panel and its delegate.
 *
 * Inputs:
 *   library — the window hosting it; held weakly
 *
 * Output:
 *   the host; the library window keeps it (the panel holds it weakly).
 */
- (instancetype)init_with_library:(TasksLibraryWindow *)library NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/* The panel; the split view owns it. */
@property (nonatomic, readonly) SharedDayPlanner *view;

@end
