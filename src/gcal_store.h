/* ===========================================================================
 * gcal_store.h — the Google Calendar sync's own tables
 *
 * An integration owns its side tables (db.h), so these are read and
 * written here, not in db.c.  They are part of the shared schema
 * (shared_db.h creates them with every new file) and stay in the file when
 * the sync is switched off:
 *
 *   gcal_calendar  id (Google's calendar id), summary, color ("#rrggbb"),
 *                  writable, is_primary — the account's calendar list,
 *                  rewritten by every pass; Settings offers its rows
 *   gcal_event     calendar_id, event_id, title, start, end (unix), all_day
 *                  — the shown calendars' events in the pass's window,
 *                  what the day planner draws; a cache, rewritten per
 *                  calendar by every pass that could read that calendar
 *   gcal_task      task_id, calendar_id, event_id, pushed_start,
 *                  pushed_duration, pushed_title — a scheduled task's
 *                  BINDING to the event mirroring it (gcal_plan.h).
 *                  Deliberately no foreign key: a binding must outlive
 *                  its task's purge so the event can still be deleted.
 *
 * The account the bindings belong to (its primary calendar's id) is kept
 * in sync_state under "gcal_account".
 *
 * One connection per thread, as everywhere: the sync thread opens its own.
 * =========================================================================== */

#ifndef GCAL_STORE_H
#define GCAL_STORE_H

#include "db.h"

/* One calendar of the account.  Strings owned. */
typedef struct {
    gchar    *id;
    gchar    *summary;
    gchar    *color;                 /* "#rrggbb", or "" when unknown       */
    gboolean  writable;              /* owner or writer access              */
    gboolean  is_primary;
} gcal_calendar_t;

/* One event shown in the planner.  Strings owned. */
typedef struct {
    gchar    *calendar_id;
    gchar    *event_id;
    gchar    *title;
    gchar    *color;                 /* its calendar's; "" when unknown     */
    gint64    start;                 /* unix seconds                        */
    gint64    end;                   /* unix seconds, exclusive             */
    gboolean  all_day;
} gcal_event_t;

/* One task's binding.  Strings owned. */
typedef struct {
    gint64  task_id;
    gchar  *calendar_id;
    gchar  *event_id;
    gint64  pushed_start;
    gint    pushed_duration;
    gchar  *pushed_title;
} gcal_binding_t;

void gcal_calendar_free(gcal_calendar_t *c);
void gcal_event_free(gcal_event_t *e);
void gcal_binding_free(gcal_binding_t *b);

/*
 * gcal_store_calendars — the stored calendar list, primary first, then
 * by name.
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   GPtrArray of gcal_calendar_t* that frees its elements; never NULL
 *   (empty when the tables do not exist yet).
 */
GPtrArray *gcal_store_calendars(TaskDatabase *db);

/*
 * gcal_store_set_calendars — replace the stored calendar list.
 *
 * Inputs:
 *   db        — the connection
 *   calendars — gcal_calendar_t* elements (borrowed)
 */
void gcal_store_set_calendars(TaskDatabase *db, GPtrArray *calendars);

/*
 * gcal_store_set_events — replace one calendar's cached events.
 *
 * Inputs:
 *   db          — the connection
 *   calendar_id — the calendar
 *   events      — gcal_event_t* elements (borrowed); their calendar_id
 *                 is ignored in favour of `calendar_id`
 */
void gcal_store_set_events(TaskDatabase *db, const gchar *calendar_id, GPtrArray *events);

/*
 * gcal_store_keep_events — drop the cached events of every calendar not
 * named: one no longer shown, or all of them (`keep` empty) when the
 * account changes.
 *
 * Inputs:
 *   db   — the connection
 *   keep — gchar* calendar ids (borrowed)
 */
void gcal_store_keep_events(TaskDatabase *db, GPtrArray *keep);

/*
 * gcal_store_events_for_day — the cached events overlapping [lo, hi),
 * all-day ones first, then by start.
 *
 * Inputs:
 *   db — the connection
 *   lo — unix seconds of the day's local midnight
 *   hi — unix seconds of the next local midnight
 *
 * Output:
 *   GPtrArray of gcal_event_t* that frees its elements; never NULL.
 */
GPtrArray *gcal_store_events_for_day(TaskDatabase *db, gint64 lo, gint64 hi);

/*
 * gcal_store_bindings — every binding.
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   GPtrArray of gcal_binding_t* that frees its elements; never NULL.
 */
GPtrArray *gcal_store_bindings(TaskDatabase *db);

/*
 * gcal_store_bind — write (insert or replace) a task's binding.
 *
 * Inputs:
 *   db — the connection
 *   b  — the binding (borrowed)
 */
void gcal_store_bind(TaskDatabase *db, const gcal_binding_t *b);

/*
 * gcal_store_unbind — drop one task's binding, or all of them.
 *
 * Inputs:
 *   db      — the connection
 *   task_id — the task, or 0 for every binding
 */
void gcal_store_unbind(TaskDatabase *db, gint64 task_id);

/*
 * gcal_store_dirty — whether a pass has anything to push: a scheduled
 * task with no binding, or whose session or title differs from what was
 * pushed, or a binding whose task left the planner.  One query, cheap
 * enough to ask on every task event.
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   TRUE when a push is due.
 */
gboolean gcal_store_dirty(TaskDatabase *db);

/*
 * gcal_store_reschedule — compare-and-set a task's session: write the
 * new one only when the task still has the session the caller read.  The
 * sync thread reads, talks to Google, and only then writes; a planner
 * edit made in between must win, not be overwritten.
 *
 * Inputs:
 *   db           — the connection
 *   task_id      — the task
 *   expect_start — the session start that was read
 *   expect_dur   — the duration that was read
 *   start        — the new start; 0 clears the session
 *   duration     — the new duration in minutes
 *
 * Output:
 *   TRUE when the row was changed.
 */
gboolean gcal_store_reschedule(TaskDatabase *db, gint64 task_id, gint64 expect_start,
                               gint expect_dur, gint64 start, gint duration);

/*
 * gcal_store_account / gcal_store_set_account — the account the bindings
 * belong to.
 *
 * Inputs:
 *   db      — the connection
 *   account — the primary calendar's id (set only)
 *
 * Output:
 *   (get) the id (g_free), or NULL when none is recorded.
 */
gchar *gcal_store_account(TaskDatabase *db);
void   gcal_store_set_account(TaskDatabase *db, const gchar *account);

#endif /* GCAL_STORE_H */
