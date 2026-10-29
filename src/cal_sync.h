/* ===========================================================================
 * cal_sync.h — the day planner's two-way sync with the macOS Calendar
 *
 * The planner's sessions are mirrored as events in one calendar of the
 * macOS Calendar (the SHARED_CALENDAR_TASKS setting, shared_calendar.h),
 * which carries them on to whatever account that calendar belongs to
 * (Google, iCloud, Exchange, …).  A task dragged onto the planner becomes
 * an event; moving or stretching it moves the event; Remove from
 * Schedule, or deleting the task, deletes the event.  Moving the event in
 * Calendar moves the session; deleting it there takes the task off the
 * planner.  The rules are cal_plan.h's; the bindings cal_store.h's; the
 * calendar is reached through a cal_backend_t (cal_backend.h).
 *
 * One PASS walks every binding and every scheduled task through the
 * rules.  It runs on the main thread, synchronously: the calendar is a
 * local store.  What starts one:
 *   - cal_sync_kick, after an edit — the planner after each of its
 *     writes, and every task event (an editor rename, a delete in the
 *     list) — when there is anything to push, or the settings it ran
 *     under changed (Notes may have changed them: they are shared);
 *   - cal_sync_calendar_changed: the calendar changed (an event moved in
 *     Calendar, a sync with an account), which may move a session;
 *   - cal_sync_settings_changed: Settings changed what the sync does.
 * Kicks in quick succession share one pass.  A pass fires the task-pane
 * event, which redraws the planner, and reports a failure in the status
 * bar.
 *
 * Bindings the Google sync made (schema 204 and before) name Google's
 * ids.  A pass finds each one's event in the calendar by its Google id
 * and rebinds it, so no session is pushed twice.
 *
 * MAIN THREAD ONLY.
 * =========================================================================== */

#ifndef CAL_SYNC_H
#define CAL_SYNC_H

#include "cal_backend.h"
#include "task_app.h"

/*
 * cal_sync_init — subscribe to task events.  Call once at startup.
 *
 * Inputs:
 *   app     — the application context; it must outlive cal_sync_shut_down
 *   backend — the calendar (borrowed; static storage)
 */
void cal_sync_init(TaskApp *app, const cal_backend_t *backend);

/*
 * cal_sync_shut_down — stop: no pending kick fires.  Call at quit before
 * the TaskApp is freed.
 */
void cal_sync_shut_down(void);

/*
 * cal_sync_kick — something changed that may need pushing: start a pass
 * shortly (edits in quick succession share one) when the sync is ready
 * and there is anything to push or the settings changed since the last
 * pass.
 */
void cal_sync_kick(void);

/*
 * cal_sync_calendar_changed — the calendar changed outside the planner:
 * redraw the planner now and start a pass shortly.
 */
void cal_sync_calendar_changed(void);

/*
 * cal_sync_settings_changed — the user changed what the sync does (the
 * switch, the calendar sessions go to): run a pass now and redraw the
 * planner.
 */
void cal_sync_settings_changed(void);

/*
 * cal_sync_pass — run one pass now (cal_sync_kick's work, without the
 * wait), then redraw the planner.  Pushes nothing while the sync is not
 * ready.
 */
void cal_sync_pass(void);

#endif /* CAL_SYNC_H */
