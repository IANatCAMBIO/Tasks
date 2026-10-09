/* ===========================================================================
 * gcal_sync.h — the day planner's two-way sync with Google Calendar
 *
 * One PASS, on a background thread with its own database connection:
 *
 *   1. PULL.  The account's calendar list, then every event in the shown
 *      calendars from a week ago to two months ahead (GCAL_SYNC_PAST_DAYS,
 *      GCAL_SYNC_AHEAD_DAYS) into the gcal_event cache (gcal_store.h),
 *      which the planner draws so that sessions can be planned around
 *      meetings.  Declined invitations are left out, and so are the
 *      planner's own events: those are drawn as the tasks they are.
 *   2. PUSH.  Every scheduled task against its binding (gcal_plan.h): a
 *      task dragged onto the planner becomes an event in the push
 *      calendar; moving or stretching it moves the event; Remove from
 *      Schedule, or deleting the task, deletes the event.  Moving the
 *      event in Google Calendar moves the session; deleting it there takes
 *      the task off the planner.
 *
 * What starts a pass:
 *   - the worker timer (task_worker.h), every TASK_CFG_GCAL_INTERVAL_MIN
 *     minutes and once when armed, while TASK_CFG_GCAL_ENABLED is on and
 *     the user is signed in (oauth.h);
 *   - gcal_sync_kick, after an edit that left something to push — the
 *     planner calls it after each of its writes, and every task event
 *     (an editor rename, a delete in the list) is checked the same way;
 *   - gcal_sync_now: Sync Now, a sign-in, a Settings change.
 *
 * At most one pass runs; a request made while one runs starts another
 * when it ends.  A finished pass fires the task-pane event, which redraws
 * the planner, and reports a failure in the status bar.
 *
 * Sessions are local to this account: the account the bindings were made
 * in is recorded, and signing in to a different one forgets them (its
 * events are left alone) and pushes the sessions afresh.
 *
 * MAIN THREAD ONLY.
 * =========================================================================== */

#ifndef GCAL_SYNC_H
#define GCAL_SYNC_H

#include "task_app.h"

/* The pull window, in days either side of today. */
#define GCAL_SYNC_PAST_DAYS  7
#define GCAL_SYNC_AHEAD_DAYS 60

/*
 * gcal_sync_init — register the worker and subscribe to task events.
 * Call once at startup, with the other workers' init (task_notify_init).
 *
 * Inputs:
 *   app — the application context; it must outlive gcal_sync_shut_down
 */
void gcal_sync_init(TaskApp *app);

/*
 * gcal_sync_shut_down — stop: no pending kick fires and a pass still
 * running reports to nobody.  Call at quit before the TaskApp is freed.
 */
void gcal_sync_shut_down(void);

/*
 * gcal_sync_active — whether syncing is on: the setting is on and the
 * user is signed in.
 *
 * Output:
 *   TRUE when the sync runs.
 */
gboolean gcal_sync_active(void);

/*
 * gcal_sync_kick — something changed that may need pushing: start a pass
 * shortly (edits in quick succession share one) when the sync is active
 * and gcal_store_dirty says there is anything to push.
 */
void gcal_sync_kick(void);

/*
 * gcal_sync_now — start a pass now (or right after the running one),
 * whatever is dirty.  A no-op while the sync is not active.
 *
 * Inputs:
 *   manual — TRUE when the user asked (Sync Now): success is then
 *            announced in the status bar too
 */
void gcal_sync_now(gboolean manual);

/*
 * gcal_sync_settings_changed — the user changed what the sync does (the
 * switch, a sign-in, the calendars): re-arm the worker, which starts a
 * pass when the sync is now active, and redraw the planner.
 */
void gcal_sync_settings_changed(void);

/*
 * gcal_sync_signed_out — the user signed out: forget the pulled
 * calendars and events, so the planner stops drawing them.  The bindings
 * stay (see the account rule above).
 */
void gcal_sync_signed_out(void);

/*
 * gcal_sync_status — one line on the last pass, for Settings: when it
 * finished, or why it failed, or that none has run.
 *
 * Output:
 *   the line (g_free).
 */
gchar *gcal_sync_status(void);

#endif /* GCAL_SYNC_H */
