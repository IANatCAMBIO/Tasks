/*
 * task_notify.m — due-date and scheduled-item notifications (task_notify.h).
 *
 * The notification worker runs once per minute (task_worker.h) and:
 *
 *   1. Scans tasks due today or overdue.  Between 8:00 AM and 8:30 AM, and
 *      once at launch, any non-done task not yet reminded this reminder day
 *      gets a notification.  A reminder day runs from 8:00 AM to the next
 *      8:00 AM, so a task still not done is reminded again at 8:00 AM the
 *      day after the user dismissed it.  Key:
 *      "due:<id>:<YYYY-MM-DD due date>:<YYYY-MM-DD reminder day>".

 *   2. Scans tasks scheduled today.  Within a 5-minute window starting
 *      10 minutes before scheduled_start, any task not already notified
 *      gets a reminder.  Key: "sched:<id>:<scheduled_start_unix>".  A
 *      session whose start has passed moves its New task to In Progress
 *      and fires the task-pane event, which refreshes the panes.
 *
 * Notifications that already fired are tracked in a module-level GHashTable
 * for the session.  A reminder the user dismissed (or that went to the
 * Notification Center) is also recorded in the database, so a restart does
 * not bring it back until its due date or scheduled start changes, or (a due
 * reminder) the next reminder day begins at 8:00 AM.
 *
 * Delivery is either a Tasks popup window (notify_window.h) or a
 * UNUserNotificationCenter alert, depending on TASK_CFG_NOTIF_MODE.  Either
 * way the app plays its own chime (resources/), once per burst.
 */

#import "task_notify.h"
#import "notify_window.h"
#import "task_config.h"
#import "task_dates.h"
#import "task_worker.h"
#import "db.h"
#import "ui_util.h"

#import <UserNotifications/UserNotifications.h>

#include <time.h>
#include <glib.h>

/* Due-today notification time: 8:00 AM (minutes past midnight). */
static const gint notify_due_minute = 8 * 60;
/* How long the due-today window stays open (minutes). */
static const gint notify_due_window = 30;
/* Minutes before scheduled_start to fire the reminder. */
static const gint notify_sched_lead = 10;
/* How long a dismissal is remembered (seconds): 90 days. */
static const gint64 notify_dismissed_keep = 90 * 24 * 3600;
/* Width of the scheduled-notification window (minutes).  Must be larger than
 * the worker interval (1 min) to guarantee the alert is never missed. */
static const gint notify_sched_window = 5;
/* The reminder chime: the bundled sound, and the quiet gap (seconds) after
 * one play during which further reminders stay silent, so a burst of them
 * chimes once. */
static NSString *const notify_chime_name = @"tunetank.com_data-select-safely";
static NSString *const notify_chime_type = @"wav";
static const CFAbsoluteTime notify_chime_gap = 5.0;

/* ── Tracking ────────────────────────────────────────────────────────── */

/* Set of notification keys already sent this session.  Keys are g_strdup'd
 * strings; values are just (void*)1 sentinels.                          */
static GHashTable *sent_set;

/* TRUE after the first worker pass has fired startup notifications. */
static gboolean startup_done = FALSE;

/*
 * already_sent — TRUE when this key has been delivered this session or
 * was dismissed before.
 *
 * Inputs:
 *   app — the application context (db open)
 *   key — borrowed string
 */
static gboolean
already_sent(TaskApp *app, const gchar *key)
{
    if (sent_set && g_hash_table_contains(sent_set, key)) return TRUE;
    return task_db_reminder_is_dismissed(app->db, key);
}

/*
 * mark_sent — record a key as delivered.
 *
 * Inputs:
 *   key — ownership transferred to the hash table
 */
static void
mark_sent(gchar *key)
{
    if (!sent_set)
        sent_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert(sent_set, key, (gpointer)1);
}

/* ── Delivery ────────────────────────────────────────────────────────── */

/*
 * play_chime — sound the reminder chime, unless it sounded within the last
 * notify_chime_gap seconds.  A missing or unreadable sound file plays
 * nothing.
 *
 * Runs on the main thread.
 */
static void
play_chime(void)
{
    static NSSound       *chime;
    static CFAbsoluteTime last_played;

    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (last_played != 0 && now - last_played < notify_chime_gap) return;

    if (chime == nil) {
        NSURL *url = [NSBundle.mainBundle URLForResource:notify_chime_name
                                           withExtension:notify_chime_type];
        if (url == nil) return;
        chime = [[NSSound alloc] initWithContentsOfURL:url byReference:YES];
    }
    last_played = now;
    [chime play];
}

/*
 * fire_notification — deliver one notification for the given task.
 *
 * Inputs:
 *   app      — the application context, which the reminder window keeps
 *              to open the task's editor
 *   task     — the task (borrowed): its title and id
 *   subtitle — the context line, e.g. "Due today" (borrowed)
 *   ident    — the notification key, unique for native dedup (borrowed)
 */
static void
fire_notification(TaskApp *app, const Task *task, const gchar *subtitle, const gchar *ident)
{
    NSString *ns_title = tasks_title_string(task, @"");
    NSString *ns_sub   = tasks_string(subtitle);

    gchar *mode = task_config_get(TASK_CFG_NOTIF_MODE);
    gboolean native = mode != NULL && strcmp(mode, TASK_NOTIF_MODE_NATIVE) == 0;
    g_free(mode);

    if (native) {
        UNMutableNotificationContent *content =
            [[UNMutableNotificationContent alloc] init];
        content.title = ns_title;
        content.body  = ns_sub;

        UNTimeIntervalNotificationTrigger *trigger =
            [UNTimeIntervalNotificationTrigger triggerWithTimeInterval:0.1
                                                               repeats:NO];
        NSString *ns_ident = tasks_string(ident);
        UNNotificationRequest *req =
            [UNNotificationRequest requestWithIdentifier:ns_ident
                                                 content:content
                                                 trigger:trigger];
        [[UNUserNotificationCenter currentNotificationCenter]
            addNotificationRequest:req
             withCompletionHandler:nil];
        /* The Notification Center is not told when it is dismissed: delivered
         * is dismissed, so a restart does not deliver it again. */
        task_db_reminder_dismiss(app->db, ident);
    } else {
        tasks_notify_add(app, task->id, tasks_string(ident), ns_title, ns_sub);
    }
    play_chime();
}

/* ── Worker pass ─────────────────────────────────────────────────────── */

/*
 * remind_due — remind about every non-done task due by the end of the
 * current reminder day (overdue ones included) that has not been reminded
 * this reminder day.  A task with no due date is never reminded.
 *
 * Inputs:
 *   app          — the application context (db open)
 *   hi           — end of the reminder day (next local midnight)
 *   today_lo     — this calendar day's local midnight: a task due from then
 *                  is "Due today", an earlier one "Overdue"
 *   reminder_day — the reminder day, "YYYY-MM-DD"
 */
static void
remind_due(TaskApp *app, gint64 hi, gint64 today_lo, const char *reminder_day)
{
    GPtrArray *due = task_db_tasks_due_between(app->db, 1, hi, TRUE);

    for (guint i = 0; i < due->len; i++) {
        const Task *t = due->pdata[i];

        time_t due_t = (time_t)t->due;
        char   due_iso[11];
        strftime(due_iso, sizeof(due_iso), "%Y-%m-%d", localtime(&due_t));

        gchar *key = g_strdup_printf("due:%" G_GINT64_FORMAT ":%s:%s",
                                     t->id, due_iso, reminder_day);
        if (!already_sent(app, key)) {
            fire_notification(app, t, t->due >= today_lo ? "Due today" : "Overdue", key);
            mark_sent(key);
        } else {
            g_free(key);
        }
    }
    task_ptr_array_free_tasks(due);
}

/*
 * notify_run — one notification pass: check due-today and scheduled tasks,
 * fire any that are in their notification window and not yet sent.
 *
 * Runs on the main thread; uses app->db directly.
 *
 * Inputs:
 *   app     — the application context (db must be open)
 *   db_path — ignored; we use the open connection in app
 */
static void
notify_run(TaskApp *app, const gchar *db_path)
{
    (void)db_path;
    if (!app || !app->db) return;

    time_t now = time(NULL);

    /* Copy the tm struct immediately: later localtime() calls reuse its
     * buffer. */
    struct tm now_tm = *localtime(&now);
    gint minutes_now = now_tm.tm_hour * 60 + now_tm.tm_min;

    gint64 today_lo, today_hi;
    task_day_bounds(0, &today_lo, &today_hi);

    /* ── Due reminders: at launch, then in the 8 AM window ───────────── */
    /* A reminder day starts at 8:00 AM, so before then it is still
     * yesterday's: a dismissal made at 7:00 AM does not outlast 8:00 AM. */
    gint64 day_lo, day_hi;
    task_day_bounds(minutes_now < notify_due_minute ? -1 : 0, &day_lo, &day_hi);
    time_t day_t = now - (time_t)notify_due_minute * 60;
    char   reminder_day[11];
    strftime(reminder_day, sizeof(reminder_day), "%Y-%m-%d", localtime(&day_t));

    if (!startup_done) {
        startup_done = TRUE;
        task_db_reminders_prune(app->db, (gint64)now - notify_dismissed_keep);
        remind_due(app, day_hi, today_lo, reminder_day);
    } else if (minutes_now >= notify_due_minute &&
               minutes_now < notify_due_minute + notify_due_window) {
        remind_due(app, day_hi, today_lo, reminder_day);
    }

    /* ── Scheduled sessions: the reminder, then the promotion ─────────── */
    GPtrArray *sched =
        task_db_tasks_scheduled_for_day(app->db, today_lo, today_hi);
    gint       promoted = 0;
    for (guint i = 0; i < sched->len; i++) {
        const Task *t = sched->pdata[i];

        /* A session that has begun moves its task to In Progress — here,
         * not in the day planner, so that it happens whether or not the
         * planner is on screen or showing today. */
        if (t->scheduled_start <= (gint64)now && t->status == TASK_STATUS_NEW) {
            if (task_db_task_set_status(app->db, t->id, TASK_STATUS_IN_PROGRESS)) {
                promoted++;
            } else {
                gchar *what = g_strdup_printf("Starting \xe2\x80\x9c%s\xe2\x80\x9d", t->title);
                task_app_write_failed(app, what);
                g_free(what);
            }
        }

        gint64 alert_at = t->scheduled_start - (gint64)(notify_sched_lead * 60);
        gint64 elapsed  = (gint64)now - alert_at;

        if (elapsed < 0 || elapsed >= (gint64)(notify_sched_window * 60))
            continue;

        gchar *key = g_strdup_printf("sched:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                                     t->id, t->scheduled_start);
        if (!already_sent(app, key)) {
            time_t sched_t = (time_t)t->scheduled_start;
            struct tm slt = *localtime(&sched_t);
            gint sched_min = slt.tm_hour * 60 + slt.tm_min;
            gchar *time_str = task_clock_format(sched_min);
            gchar *subtitle = g_strdup_printf("Starts in %d minutes at %s",
                                              notify_sched_lead, time_str);
            g_free(time_str);
            fire_notification(app, t, subtitle, key);
            g_free(subtitle);
            mark_sent(key);
        } else {
            g_free(key);
        }
    }
    task_ptr_array_free_tasks(sched);
    if (promoted > 0) task_app_notify_tasks(app);
}

/* ── Worker registration ─────────────────────────────────────────────── */

static guint notif_timer_id = 0;

static const TaskWorkerDef notif_worker = {
    "notify",                       /* id           */
    0,                              /* sort         */
    NULL,                           /* enabled_key  (always on) */
    TASK_CFG_NOTIF_CHECK_MIN,       /* interval_key */
    0,                              /* period_min   (the key gives it) */
    TASK_WORKER_INITIAL_ARMED,      /* initial      */
    NULL,                           /* running      */
    &notif_timer_id,                /* timer        */
    notify_run,                     /* run          */
    NULL,                           /* ready        */
};

/*
 * task_notify_init — register the notification worker.
 * Inputs are documented in task_notify.h.
 */
void
task_notify_init(TaskApp *app)
{
    (void)app;
    task_worker_register(&notif_worker);
}

/* ── Native authorization ─────────────────────────────────────────────── */

/*
 * task_notify_request_native_auth — ask for UNUserNotificationCenter
 * authorization.  Called when the user selects native mode in Settings.
 * Inputs are documented in task_notify.h.
 */
void
task_notify_request_native_auth(void)
{
    UNUserNotificationCenter *center =
        [UNUserNotificationCenter currentNotificationCenter];

    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                             UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError *error) {
        (void)granted;
        (void)error;
    }];
}
