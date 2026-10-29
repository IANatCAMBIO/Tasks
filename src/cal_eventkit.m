#import "cal_eventkit.h"
#import "ui_util.h"

#include "cal_sync.h"

/* How far either side of its last known start a Google-sync event is
 * looked for: a month (cal_backend.h, find_google). */
static const NSTimeInterval google_window_s = 31 * 24 * 3600;

/* The suffix Google gives an event's iCalendar UID, which the calendar
 * reports as the external identifier of an event a Google account holds. */
static NSString *const google_uid_suffix = @"@google.com";

/*
 * TasksCalendarSettings — the shared calendar settings, read and written
 * in the shared database's shared_settings through the app's connection.
 */
@interface TasksCalendarSettings : NSObject <SharedCalendarSettings>
@property (nonatomic, assign) TaskApp *app;
@end

@implementation TasksCalendarSettings

/*
 * calendar_setting: — see shared_calendar.h.
 */
- (NSString *)
calendar_setting:(const char *)key
{
    char *value = NULL;
    if (self.app->db == NULL ||
        shared_settings_get(self.app->db->shared, key, &value, NULL) != 1) return nil;
    NSString *text = tasks_string(value);
    free(value);
    return text;
}

/*
 * set_calendar_setting:value: — see shared_calendar.h.  A write that
 * fails is reported in the status bar.
 */
- (void)
set_calendar_setting:(const char *)key value:(NSString *)value
{
    char *err = NULL;
    if (self.app->db == NULL) return;
    if (shared_settings_set(self.app->db->shared, key, value.UTF8String, &err) != 0)
        task_app_status(self.app, "The calendar setting was not saved: %s", err != NULL ? err : "?");
    free(err);
}

@end

static TasksCalendarSettings *settings;
static id                     observer;   /* the calendar's changes */

/*
 * tasks_calendar_settings — see cal_eventkit.h.
 */
id<SharedCalendarSettings>
tasks_calendar_settings(void)
{
    return settings;
}

/*
 * cal_eventkit_init — see cal_eventkit.h.
 */
void
cal_eventkit_init(TaskApp *app)
{
    settings     = [[TasksCalendarSettings alloc] init];
    settings.app = app;
    observer     = shared_calendar_observe(^{ cal_sync_calendar_changed(); });
}

/*
 * cal_eventkit_shut_down — see cal_eventkit.h.
 */
void
cal_eventkit_shut_down(void)
{
    shared_calendar_unobserve(observer);
    observer = nil;
}

/*
 * error_text — an EventKit error as the sync reports it.
 *
 * Inputs:
 *   error — the error, or nil
 *
 * Output: the text (g_free).
 */
static gchar *
error_text(NSError *error)
{
    return g_strdup(error != nil ? error.localizedDescription.UTF8String : "the calendar refused");
}

/*
 * event_named — a live event, by id.
 *
 * Inputs:
 *   event_id — the id
 *
 * Output: the event, or nil when there is none (or it was cancelled).
 */
static EKEvent *
event_named(const gchar *event_id)
{
    EKEvent *ev = [shared_calendar_store() eventWithIdentifier:@(event_id)];
    return ev != nil && ev.status != EKEventStatusCanceled ? ev : nil;
}

/*
 * set_session — a task's session on an event: its title and its times.
 *
 * Inputs:
 *   ev       — the event
 *   start    — unix seconds
 *   duration — minutes
 *   title    — the task's title; "" or NULL is "Untitled"
 */
static void
set_session(EKEvent *ev, gint64 start, gint duration, const gchar *title)
{
    ev.title     = title != NULL && *title != '\0' ? @(title) : @"Untitled";
    ev.startDate = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)start];
    ev.endDate   = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)(start + (gint64)duration * 60)];
    ev.allDay    = NO;
}

/*
 * save — write an event.
 *
 * Inputs:
 *   ev  — the event
 *   err — out: the reason on failure (g_free)
 *
 * Output: CAL_OK or CAL_FAILED.
 */
static cal_result_t
save(EKEvent *ev, gchar **err)
{
    NSError *error = nil;
    if ([shared_calendar_store() saveEvent:ev span:EKSpanThisEvent commit:YES error:&error]) return CAL_OK;
    *err = error_text(error);
    return CAL_FAILED;
}

/* ── The backend (cal_backend.h) ─────────────────────────────────────── */

/*
 * ek_ready — the calendar is switched on and may be used.
 *
 * Output: TRUE when it is.
 */
static gboolean
ek_ready(void)
{
    return shared_calendar_on(settings);
}

/*
 * ek_tasks_calendar — the calendar sessions go to.
 *
 * Output: its id (g_free), or NULL.
 */
static gchar *
ek_tasks_calendar(void)
{
    EKCalendar *cal = shared_calendar_for_tasks(settings);
    return cal != nil ? g_strdup(cal.calendarIdentifier.UTF8String) : NULL;
}

/*
 * ek_calendar_exists — whether a calendar exists.
 *
 * Inputs:
 *   calendar_id — its id
 *
 * Output: TRUE when it does.
 */
static gboolean
ek_calendar_exists(const gchar *calendar_id)
{
    return [shared_calendar_store() calendarWithIdentifier:@(calendar_id)] != nil;
}

/*
 * ek_find — an event, by id.
 *
 * Inputs:
 *   event_id — its id
 *   out      — out: its times on CAL_OK
 *   err      — unused: a look-up cannot fail but by finding nothing
 *
 * Output: CAL_OK or CAL_GONE.
 */
static cal_result_t
ek_find(const gchar *event_id, cal_event_t *out, gchar **err)
{
    EKEvent *ev = event_named(event_id);
    if (ev == nil) return CAL_GONE;
    out->start   = (gint64)ev.startDate.timeIntervalSince1970;
    out->end     = (gint64)ev.endDate.timeIntervalSince1970;
    out->all_day = ev.allDay;
    return CAL_OK;
}

/*
 * ek_find_google — the event a Google-sync binding named: an event of any
 * calendar near its last known start whose external identifier is its
 * Google id, or Google's UID for it.
 *
 * Inputs:
 *   google_id   — the id
 *   near        — the session's last known start, unix seconds
 *   event_id    — out: the event's id (g_free) when found
 *   calendar_id — out: its calendar's id (g_free) when found
 *
 * Output: TRUE when found.
 */
static gboolean
ek_find_google(const gchar *google_id, gint64 near, gchar **event_id, gchar **calendar_id)
{
    EKEventStore *store = shared_calendar_store();
    NSString     *id    = @(google_id);
    NSString     *uid   = [id stringByAppendingString:google_uid_suffix];
    NSDate       *at    = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)near];
    NSPredicate  *range = [store predicateForEventsWithStartDate:[at dateByAddingTimeInterval:-google_window_s]
                                                         endDate:[at dateByAddingTimeInterval:google_window_s]
                                                       calendars:nil];
    for (EKEvent *ev in [store eventsMatchingPredicate:range]) {
        NSString *ext = ev.calendarItemExternalIdentifier;
        if (ev.eventIdentifier == nil || !([ext isEqualToString:id] || [ext isEqualToString:uid])) continue;
        *event_id    = g_strdup(ev.eventIdentifier.UTF8String);
        *calendar_id = g_strdup(ev.calendar.calendarIdentifier.UTF8String);
        return TRUE;
    }
    return FALSE;
}

/*
 * ek_insert — create a session's event.
 *
 * Inputs:
 *   calendar_id, start, duration, title — the session and where
 *   event_id — out: the new event's id (g_free) on CAL_OK
 *   err      — out: the reason on CAL_FAILED (g_free)
 *
 * Output: the outcome.
 */
static cal_result_t
ek_insert(const gchar *calendar_id, gint64 start, gint duration, const gchar *title,
          gchar **event_id, gchar **err)
{
    EKEventStore *store = shared_calendar_store();
    EKCalendar   *cal   = [store calendarWithIdentifier:@(calendar_id)];
    if (cal == nil) {
        *err = g_strdup("the calendar for scheduled tasks is gone");
        return CAL_FAILED;
    }
    EKEvent *ev = [EKEvent eventWithEventStore:store];
    ev.calendar = cal;
    set_session(ev, start, duration, title);
    cal_result_t res = save(ev, err);
    if (res == CAL_OK) *event_id = g_strdup(ev.eventIdentifier.UTF8String);
    return res;
}

/*
 * ek_update — write a session to its event.
 *
 * Inputs:
 *   event_id, start, duration, title — the event and the session
 *   err — out: the reason on CAL_FAILED (g_free)
 *
 * Output: the outcome; CAL_GONE when the event no longer exists.
 */
static cal_result_t
ek_update(const gchar *event_id, gint64 start, gint duration, const gchar *title, gchar **err)
{
    EKEvent *ev = event_named(event_id);
    if (ev == nil) return CAL_GONE;
    set_session(ev, start, duration, title);
    return save(ev, err);
}

/*
 * ek_remove — delete an event.
 *
 * Inputs:
 *   event_id — the event
 *   err      — out: the reason on CAL_FAILED (g_free)
 *
 * Output: the outcome; CAL_GONE when it was already gone.
 */
static cal_result_t
ek_remove(const gchar *event_id, gchar **err)
{
    EKEvent *ev = event_named(event_id);
    if (ev == nil) return CAL_GONE;
    NSError *error = nil;
    if ([shared_calendar_store() removeEvent:ev span:EKSpanThisEvent commit:YES error:&error]) return CAL_OK;
    *err = error_text(error);
    return CAL_FAILED;
}

static const cal_backend_t eventkit_backend = {
    .ready           = ek_ready,
    .tasks_calendar  = ek_tasks_calendar,
    .calendar_exists = ek_calendar_exists,
    .find            = ek_find,
    .find_google     = ek_find_google,
    .insert          = ek_insert,
    .update          = ek_update,
    .remove          = ek_remove,
};

/*
 * cal_eventkit_backend — see cal_eventkit.h.
 */
const cal_backend_t *
cal_eventkit_backend(void)
{
    return &eventkit_backend;
}
