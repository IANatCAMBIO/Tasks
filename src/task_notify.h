/*
 * task_notify — due-date and scheduled-item notifications.
 *
 * Two kinds of notifications are fired:
 *
 *   1. Due-today: at 8:00 AM (30-minute window) for every non-done task
 *      whose due date is today.
 *
 *   2. Scheduled: 10 minutes before a task's scheduled_start (2-minute
 *      window around that moment).
 *
 * The delivery mode is controlled by TASK_CFG_NOTIF_MODE and toggled in
 * the Settings window:
 *
 *   "tasks"  — a small popup in the lower-right corner of the screen
 *              (the default, similar to Outlook meeting reminders).
 *   "native" — the macOS Notification Center via UNUserNotificationCenter.
 *
 * MAIN THREAD ONLY.
 */

#ifndef TASK_NOTIFY_H
#define TASK_NOTIFY_H

#include "task_app.h"

/* Config key for the delivery mode.  Value: TASK_NOTIF_MODE_TASKS or
 * TASK_NOTIF_MODE_NATIVE.  Default: TASK_NOTIF_MODE_TASKS.           */
#define TASK_CFG_NOTIF_MODE      "notifications_mode"

/* Internal: how often the notification worker scans for due tasks, in
 * minutes.  Default 1.  Not shown in the Settings window.            */
#define TASK_CFG_NOTIF_CHECK_MIN "notifications_check_min"

/* Values for TASK_CFG_NOTIF_MODE. */
#define TASK_NOTIF_MODE_TASKS  "tasks"
#define TASK_NOTIF_MODE_NATIVE "native"

/*
 * task_notify_init — register the notification worker with the shared
 * scheduler (task_worker.h).  Call once at startup, before any window
 * or thread exists, alongside task_recur_init and task_backup_init.
 *
 * Inputs:
 *   app — the application context
 */
void task_notify_init(TaskApp *app);

/*
 * task_notify_request_native_auth — request UNUserNotificationCenter
 * authorization from the user.  Called by the Settings window when the
 * user selects native mode.  No-op when already granted or denied.
 */
void task_notify_request_native_auth(void);

#endif /* TASK_NOTIFY_H */
