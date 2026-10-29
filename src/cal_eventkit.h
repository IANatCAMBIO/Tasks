#import <Cocoa/Cocoa.h>
#import "shared_calendar.h"

#include "cal_backend.h"
#include "task_app.h"

/*
 * cal_eventkit — Tasks' side of the macOS Calendar: the calendar settings
 * Tasks shares with Notes (shared_calendar.h, kept in the shared
 * database), the EventKit backend the sync writes through
 * (cal_backend.h), and the watch that tells the sync when the calendar
 * changed.  Main thread only.
 */

/*
 * cal_eventkit_init — start watching the calendar.  Call once at startup,
 * before cal_sync_init.
 *
 * Inputs:
 *   app — the application context; it must outlive cal_eventkit_shut_down
 */
void cal_eventkit_init(TaskApp *app);

/*
 * cal_eventkit_shut_down — stop watching.  Call at quit.
 */
void cal_eventkit_shut_down(void);

/*
 * cal_eventkit_backend — the calendar, as the sync uses it.
 *
 * Output: the backend (static storage).
 */
const cal_backend_t *cal_eventkit_backend(void);

/*
 * tasks_calendar_settings — the shared calendar settings, through the
 * open database (nil until cal_eventkit_init).
 *
 * Output: the settings.
 */
id<SharedCalendarSettings> tasks_calendar_settings(void);
