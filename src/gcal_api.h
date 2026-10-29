/* ===========================================================================
 * gcal_api.h — the Google Calendar v3 REST calls the sync makes
 *
 * Every call is BLOCKING (one or more HTTPS round trips through http.h):
 * the sync thread makes them, never the main thread.  Each takes the
 * OAuth access token (oauth.h) and reports one of three outcomes:
 *
 *   GCAL_OK      the call did what was asked;
 *   GCAL_GONE    the event (or calendar) does not exist — 404 or 410, or
 *                an event whose status is "cancelled", which is how
 *                Google reports a deleted event that still has a record;
 *   GCAL_FAILED  anything else: the network, an expired grant, a quota —
 *                *err says which.  It proves nothing about the event.
 *
 * A pushed event carries the task's id in a PRIVATE extended property
 * (GCAL_TASK_PROPERTY): only this OAuth client sees it, and it is how a
 * pass tells the planner's own events apart from the rest of a calendar.
 * =========================================================================== */

#ifndef GCAL_API_H
#define GCAL_API_H

#include "gcal_store.h"
#include "json.h"

/* The private extended property naming a pushed event's task. */
#define GCAL_TASK_PROPERTY "tasks_appkit_task"

/* A call's outcome (above). */
typedef enum {
    GCAL_OK = 0,
    GCAL_GONE,
    GCAL_FAILED
} gcal_result_t;

/* One event as Google reports it.  Strings owned. */
typedef struct {
    gchar    *event_id;
    gchar    *title;                 /* "" when it has none                 */
    gint64    task_id;               /* the task it mirrors; 0 = not ours   */
    gint64    start;                 /* unix seconds                        */
    gint64    end;                   /* unix seconds, exclusive             */
    gboolean  all_day;
    gboolean  cancelled;             /* status "cancelled": deleted         */
    gboolean  declined;              /* the account's own invitation was
                                      * declined: not something to plan
                                      * around                              */
    gchar    *attendees;             /* SHARED_DB_GCAL_ATTENDEE_SEP-joined
                                      * names (shared_db.h); "" for none    */
} gcal_remote_t;

void gcal_remote_free(gcal_remote_t *r);

/*
 * gcal_api_parse_event — read one event resource.
 *
 * Inputs:
 *   item — the parsed JSON object (borrowed)
 *
 * Output:
 *   the event (gcal_remote_free), or NULL when it has no id or no
 *   readable start and end.
 */
gcal_remote_t *gcal_api_parse_event(task_json_t *item);

/*
 * gcal_api_event_body — the JSON body that writes a task's session to an
 * event: summary, start and end in UTC.  The full body (for an insert)
 * also carries the task property and turns the calendar's default
 * reminders off — the app reminds of its own sessions (task_notify.h).
 *
 * Inputs:
 *   task_id  — the task
 *   start    — the session start, unix seconds
 *   duration — its length in minutes
 *   title    — the task's title; "" or NULL is sent as "Untitled"
 *   full     — TRUE for an insert, FALSE for a patch
 *
 * Output:
 *   the body (g_free).
 */
gchar *gcal_api_event_body(gint64 task_id, gint64 start, gint duration,
                           const gchar *title, gboolean full);

/*
 * gcal_api_calendars — the account's calendar list.
 *
 * Inputs:
 *   token — the access token
 *   out   — out: GPtrArray of gcal_calendar_t* that frees its elements
 *   err   — out: the reason on failure (g_free)
 *
 * Output:
 *   GCAL_OK with *out set, or GCAL_FAILED.
 */
gcal_result_t gcal_api_calendars(const gchar *token, GPtrArray **out, gchar **err);

/*
 * gcal_api_events — every event of a calendar overlapping [lo, hi),
 * recurring ones expanded into their occurrences, every page read.
 *
 * Inputs:
 *   token       — the access token
 *   calendar_id — the calendar
 *   lo, hi      — the window, unix seconds
 *   out         — out: GPtrArray of gcal_remote_t* that frees its elements
 *   err         — out: the reason on failure (g_free)
 *
 * Output:
 *   GCAL_OK with *out set, GCAL_GONE for a calendar that does not exist,
 *   or GCAL_FAILED.
 */
gcal_result_t gcal_api_events(const gchar *token, const gchar *calendar_id,
                              gint64 lo, gint64 hi, GPtrArray **out, gchar **err);

/*
 * gcal_api_get_event — one event, by id.
 *
 * Inputs:
 *   token, calendar_id, event_id — what to read
 *   out — out: the event (gcal_remote_free) on GCAL_OK
 *   err — out: the reason on GCAL_FAILED (g_free)
 *
 * Output:
 *   the outcome; a cancelled event is GCAL_GONE.
 */
gcal_result_t gcal_api_get_event(const gchar *token, const gchar *calendar_id,
                                 const gchar *event_id, gcal_remote_t **out, gchar **err);

/*
 * gcal_api_insert — create the event mirroring a task.
 *
 * Inputs:
 *   token, calendar_id         — where
 *   task_id, start, duration, title — the session (gcal_api_event_body)
 *   event_id — out: the new event's id (g_free) on GCAL_OK
 *   err      — out: the reason on failure (g_free)
 *
 * Output:
 *   the outcome.
 */
gcal_result_t gcal_api_insert(const gchar *token, const gchar *calendar_id, gint64 task_id,
                              gint64 start, gint duration, const gchar *title,
                              gchar **event_id, gchar **err);

/*
 * gcal_api_patch — write a task's session to its event.
 *
 * Inputs:
 *   token, calendar_id, event_id    — the event
 *   task_id, start, duration, title — the session
 *   err — out: the reason on GCAL_FAILED (g_free)
 *
 * Output:
 *   the outcome; GCAL_GONE when the event no longer exists.
 */
gcal_result_t gcal_api_patch(const gchar *token, const gchar *calendar_id,
                             const gchar *event_id, gint64 task_id, gint64 start,
                             gint duration, const gchar *title, gchar **err);

/*
 * gcal_api_delete — delete an event.
 *
 * Inputs:
 *   token, calendar_id, event_id — the event
 *   err — out: the reason on GCAL_FAILED (g_free)
 *
 * Output:
 *   the outcome; GCAL_GONE when it was already gone (as good as done).
 */
gcal_result_t gcal_api_delete(const gchar *token, const gchar *calendar_id,
                              const gchar *event_id, gchar **err);

#endif /* GCAL_API_H */
