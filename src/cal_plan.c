/* ===========================================================================
 * cal_plan.c — the calendar sync's per-task decision (cal_plan.h)
 * =========================================================================== */

#include "cal_plan.h"

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
same_time(const cal_slot_t *a, const cal_slot_t *b)
{
    return a->start == b->start && a->duration == b->duration;
}

/*
 * cal_plan — decide what to do with one task.
 * Documented in cal_plan.h.
 */
cal_action_t
cal_plan(const cal_slot_t *pushed, const cal_slot_t *task,
          cal_event_state_t state, const cal_slot_t *event, gboolean misplaced)
{
    if (pushed == NULL) return task != NULL ? CAL_ACT_CREATE : CAL_ACT_NONE;
    if (task == NULL)   return state == CAL_EVENT_MISSING ? CAL_ACT_FORGET : CAL_ACT_REMOVE;
    if (misplaced)      return CAL_ACT_MOVE;

    gboolean task_changed = !same_time(task, pushed) || pushed->title == NULL ||
                            strcmp(task->title != NULL ? task->title : "", pushed->title) != 0;

    if (state == CAL_EVENT_MISSING) return task_changed ? CAL_ACT_CREATE : CAL_ACT_UNSCHEDULE;
    if (task_changed)                return CAL_ACT_UPDATE;
    if (state == CAL_EVENT_PRESENT && !same_time(event, pushed)) return CAL_ACT_ADOPT;
    return CAL_ACT_NONE;
}
