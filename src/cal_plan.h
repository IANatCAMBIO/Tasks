/* ===========================================================================
 * cal_plan.h — what the calendar sync does with one scheduled task
 *
 * The day planner's sessions (tasks.scheduled_start / scheduled_duration)
 * are mirrored as events in one calendar of the macOS Calendar
 * (cal_sync.h).  Each mirrored task has a
 * BINDING (cal_store.h) that remembers the event and the slot last
 * pushed to it — the baseline both sides are compared against:
 *
 *   - the task's session differs from the baseline: it was moved,
 *     stretched or renamed HERE;
 *   - the event's slot differs from the baseline: it was moved THERE.
 *
 * The rules, in order:
 *
 *   no binding        a scheduled task is pushed (CREATE).
 *   task gone         the task left the planner (Remove from Schedule, or
 *                     the task was deleted): its event is deleted
 *                     (REMOVE), or merely forgotten when it is already
 *                     gone (FORGET).
 *   wrong calendar    the push calendar changed in Settings, or the
 *                     event's calendar no longer exists: the event moves
 *                     to the push calendar (MOVE).
 *   event deleted     deleted in the calendar: the task leaves the
 *                     planner (UNSCHEDULE) — unless it was ALSO changed
 *                     here, which is a decision to keep it: pushed again
 *                     (CREATE).
 *   task changed      the local edit wins, a concurrent remote move
 *                     included (UPDATE).
 *   event moved       the task follows it (ADOPT).
 *
 * Only a CONFIRMED deletion unschedules: an event the pass could not look
 * at (outside the fetched window, a network error) is UNKNOWN, and
 * absence of evidence changes nothing.  The event's title is never
 * adopted: the task's title is the task's.
 *
 * Plain C, no I/O: the sync engine (cal_sync.h) carries the actions out.
 * =========================================================================== */

#ifndef CAL_PLAN_H
#define CAL_PLAN_H

#include <glib.h>

/* One session: when it starts, how long it lasts, what it is called. */
typedef struct {
    gint64       start;              /* unix seconds                        */
    gint         duration;           /* minutes                             */
    const gchar *title;              /* borrowed; may be NULL for an event  */
} cal_slot_t;

/* What is known of a binding's event. */
typedef enum {
    CAL_EVENT_UNKNOWN = 0,          /* not looked at, or the look failed   */
    CAL_EVENT_PRESENT,              /* found; its slot is given            */
    CAL_EVENT_MISSING               /* confirmed deleted                   */
} cal_event_state_t;

/* The action for one task. */
typedef enum {
    CAL_ACT_NONE = 0,
    CAL_ACT_CREATE,                 /* insert an event, bind it            */
    CAL_ACT_UPDATE,                 /* write the task's slot to the event  */
    CAL_ACT_MOVE,                   /* delete the event, insert it in the
                                      * push calendar                       */
    CAL_ACT_REMOVE,                 /* delete the event, drop the binding  */
    CAL_ACT_FORGET,                 /* drop the binding                    */
    CAL_ACT_ADOPT,                  /* write the event's slot to the task  */
    CAL_ACT_UNSCHEDULE              /* clear the task's session, drop the
                                      * binding                             */
} cal_action_t;

/*
 * cal_plan — decide what to do with one task (the rules above).
 *
 * Inputs:
 *   pushed    — the binding's baseline, or NULL when the task is unbound
 *               (its title may be NULL, meaning "unknown": changed)
 *   task      — the task's session now, or NULL when it has none (not
 *               scheduled, deleted, or gone from the database)
 *   state     — what is known of the bound event (ignored when unbound)
 *   event     — the event's slot when state is PRESENT, else ignored
 *   misplaced — the bound event is not in the push calendar
 *
 * Output:
 *   the action.
 */
cal_action_t cal_plan(const cal_slot_t *pushed, const cal_slot_t *task,
                        cal_event_state_t state, const cal_slot_t *event,
                        gboolean misplaced);

#endif /* CAL_PLAN_H */
