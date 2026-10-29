/* ===========================================================================
 * cal_backend.h — the calendar the sync writes to, as the core sees it
 *
 * The sync (cal_sync.h) is toolkit-free C; the calendar is the macOS
 * Calendar, reached through EventKit from Objective-C (cal_eventkit.h).
 * This table is the seam between them: the app installs the EventKit one,
 * the tests a fake.  Every call is SYNCHRONOUS and made on the main
 * thread — EventKit is a local store, with nothing to wait for.
 *
 * A call reports one of three outcomes:
 *
 *   CAL_OK      it did what was asked;
 *   CAL_GONE    the event does not exist (deleted here or elsewhere);
 *   CAL_FAILED  anything else — *err says what.  It proves nothing about
 *               the event.
 * =========================================================================== */

#ifndef CAL_BACKEND_H
#define CAL_BACKEND_H

#include <glib.h>

/* A call's outcome (above). */
typedef enum {
    CAL_OK = 0,
    CAL_GONE,
    CAL_FAILED
} cal_result_t;

/* What the calendar says of one event. */
typedef struct {
    gint64   start;                  /* unix seconds                        */
    gint64   end;                    /* unix seconds, exclusive             */
    gboolean all_day;
} cal_event_t;

typedef struct {
    /* Whether the sync may run: the calendar is switched on and the app
     * may use it.                                                           */
    gboolean     (*ready)(void);

    /* The calendar sessions go to (g_free), or NULL when there is none to
     * write to.                                                             */
    gchar       *(*tasks_calendar)(void);

    /* Whether a calendar of that id exists.                                 */
    gboolean     (*calendar_exists)(const gchar *calendar_id);

    /* An event, by id: CAL_OK with *out, CAL_GONE when there is none.       */
    cal_result_t (*find)(const gchar *event_id, cal_event_t *out, gchar **err);

    /* The event a Google-sync binding named, found by its Google id (the
     * calendar's external identifier) within a month of `near`: TRUE with
     * its *event_id and *calendar_id (g_free) when found.                   */
    gboolean     (*find_google)(const gchar *google_id, gint64 near,
                                gchar **event_id, gchar **calendar_id);

    /* Create an event: *event_id (g_free) on CAL_OK.                        */
    cal_result_t (*insert)(const gchar *calendar_id, gint64 start, gint duration,
                           const gchar *title, gchar **event_id, gchar **err);

    /* Write a session to an event; CAL_GONE when it no longer exists.      */
    cal_result_t (*update)(const gchar *event_id, gint64 start, gint duration,
                           const gchar *title, gchar **err);

    /* Delete an event; CAL_GONE when it was already gone (as good as done). */
    cal_result_t (*remove)(const gchar *event_id, gchar **err);
} cal_backend_t;

#endif /* CAL_BACKEND_H */
