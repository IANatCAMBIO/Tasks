/*
 * notify_window — the Tasks reminders window: a single panel that accumulates
 * pending notifications in a scrollable list, modelled on Microsoft Outlook's
 * reminder window.  The selected reminder is shown in a full-width blue hero
 * area at the top; Snooze and Dismiss split buttons sit at the bottom.
 * Double-clicking a reminder opens its task's editor.
 *
 * MAIN THREAD ONLY.
 */

#import <Cocoa/Cocoa.h>
#include "task_app.h"

/*
 * tasks_notify_add — enqueue one reminder and show the window.
 *
 * Inputs:
 *   app      — the application context, borrowed; it outlives the window
 *   task_id  — the task the reminder is for, which a double-click opens
 *   title    — the task's title
 *   subtitle — the context line (e.g. "Due today", "Starts in 10 minutes at 9:00 AM")
 */
void tasks_notify_add(TaskApp *app, int64_t task_id, NSString *title, NSString *subtitle);
