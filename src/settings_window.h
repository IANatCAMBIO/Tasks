#import <Cocoa/Cocoa.h>
#include "task_app.h"

/*
 * settings_window — the Settings window: Appearance, New Tasks (the
 * default tag, Quick Task's system-wide shortcut), notifications, the
 * database health plate, rotating backups, Google Calendar.
 *
 * Every control is write-through: a change lands in the config at once
 * (task_config.h) and there is no OK or Apply.  A setting that changes
 * what the task pane shows is followed by task_app_notify_tasks, and the
 * panes re-read their settings on every refresh — so Settings never needs
 * to reach into another window.  The backup settings are the shared
 * database's (backup.h), which Notes follows too; the periodic poll reads
 * them afresh every time, so nothing needs re-arming.
 *
 * The module owns its one window: created on first open, released when it
 * closes.
 *
 * Behavioural reference: porting/gtk_reference/settings_window.c.
 * Main thread only.
 */

/*
 * tasks_settings_window_open — show the Settings window, or bring the
 * existing one to the front.
 *
 * Inputs:
 *   app — the application context (its open database is what the
 *         Database section describes)
 */
void tasks_settings_window_open(TaskApp *app);
