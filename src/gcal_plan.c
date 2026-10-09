/* ===========================================================================
 * gcal_plan.c — the Google Calendar sync's per-task decision (gcal_plan.h)
 * =========================================================================== */

#include "gcal_plan.h"

#include <string.h>

/*
 * same_time — whether two slots start and last the same.
 *
 * Inputs:
 *   a, b — the slots
 *
 * Output:
 *   TRUE when start and duration are equal.
 */
static gboolean
same_time(const gcal_slot_t *a, const gcal_slot_t *b)
{
    return a->start == b->start && a->duration == b->duration;
}

/*
 * gcal_plan — decide what to do with one task.
 * Documented in gcal_plan.h.
 */
gcal_action_t
gcal_plan(const gcal_slot_t *pushed, const gcal_slot_t *task,
          gcal_event_state_t state, const gcal_slot_t *event, gboolean misplaced)
{
    if (pushed == NULL) return task != NULL ? GCAL_ACT_CREATE : GCAL_ACT_NONE;
    if (task == NULL)   return state == GCAL_EVENT_MISSING ? GCAL_ACT_FORGET : GCAL_ACT_REMOVE;
    if (misplaced)      return GCAL_ACT_MOVE;

    gboolean task_changed = !same_time(task, pushed) || pushed->title == NULL ||
                            strcmp(task->title != NULL ? task->title : "", pushed->title) != 0;

    if (state == GCAL_EVENT_MISSING) return task_changed ? GCAL_ACT_CREATE : GCAL_ACT_UNSCHEDULE;
    if (task_changed)                return GCAL_ACT_UPDATE;
    if (state == GCAL_EVENT_PRESENT && !same_time(event, pushed)) return GCAL_ACT_ADOPT;
    return GCAL_ACT_NONE;
}
