#import <Cocoa/Cocoa.h>
#include "task_app.h"

/*
 * settings_window — the Settings window: Appearance, New Tasks (Quick
 * Task's system-wide shortcut), notifications, Cleanup, the Calendar.
 * The database itself — its health, its fingerprint, its backups — is
 * camb.io Database Manager's (camb.io_db_mgr.app), not shown here.
 *
 * Every control is write-through: a change lands in the config at once
 * (task_config.h) and there is no OK or Apply.  A setting that changes
 * what the task pane shows is followed by task_app_notify_tasks, and the
 * panes re-read their settings on every refresh — so Settings never needs
 * to reach into another window.
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
