/*
 * notify_window — the Tasks reminders window: a single panel that accumulates
 * pending notifications in a scrollable list, modelled on Microsoft Outlook's
 * reminder window.  The selected reminder is shown in a full-width blue hero
 * area at the top; Snooze and Dismiss popup buttons sit at the bottom.
 *
 * MAIN THREAD ONLY.
 */

#import <Cocoa/Cocoa.h>

/*
 * tasks_notify_add — enqueue one reminder and show the window.
 *
 * Inputs:
 *   title    — the task's title
 *   subtitle — the context line (e.g. "Due today", "Starts in 10 minutes at 9:00 AM")
 */
void tasks_notify_add(NSString *title, NSString *subtitle);
