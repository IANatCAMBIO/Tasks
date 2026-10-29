/* ===========================================================================
 * cal_store.h — the calendar sync's own table
 *
 * An integration owns its side tables (db.h), so this one is read and
 * written here, not in db.c.  It is part of the shared schema (shared_db.h
 * creates it with every new file) and stays in the file when the sync is
 * switched off:
 *
 *   cal_task  task_id, calendar_id, event_id, pushed_start,
 *             pushed_duration, pushed_title — a scheduled task's BINDING
 *             to the event mirroring it (cal_plan.h), by the macOS
 *             Calendar's identifiers.  Deliberately no foreign key: a
 *             binding must outlive its task's purge so the event can
 *             still be deleted.  A binding the Google sync made (schema
 *             204 and before) holds Google's ids until the first pass
 *             finds its event (cal_sync.c).
 *
 * The day planners of both apps leave the bound events out of the
 * calendar they draw: those are drawn as the tasks they are.
 * =========================================================================== */

#ifndef CAL_STORE_H
#define CAL_STORE_H

#include "db.h"

/* One task's binding.  Strings owned. */
typedef struct {
    gint64  task_id;
    gchar  *calendar_id;
    gchar  *event_id;
    gint64  pushed_start;
    gint    pushed_duration;
    gchar  *pushed_title;
} cal_binding_t;

void cal_binding_free(cal_binding_t *b);

/*
 * cal_store_bindings — every binding.
 *
 * Inputs:
 *   db — the connection
 *
 * Output:
 *   GPtrArray of cal_binding_t* that frees its elements; never NULL.
 */
GPtrArray *cal_store_bindings(TaskDatabase *db);

/*
 * cal_store_bind — write (insert or replace) a task's binding.
 *
 * Inputs:
 *   db — the connection
 *   b  — the binding (borrowed)
 */
void cal_store_bind(TaskDatabase *db, const cal_binding_t *b);

/*
 * cal_store_unbind — drop one task's binding.
 *
 * Inputs:
 *   db      — the connection
 *   task_id — the task
 */
void cal_store_unbind(TaskDatabase *db, gint64 task_id);

/*
 * cal_store_dirty — whether a pass has anything to push: a scheduled
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
gboolean cal_store_dirty(TaskDatabase *db);

/*
 * cal_store_reschedule — compare-and-set a task's session: write the new
 * one only when the task still has the session the caller read, so that a
 * planner edit made since the pass read it wins.
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
gboolean cal_store_reschedule(TaskDatabase *db, gint64 task_id, gint64 expect_start,
                              gint expect_dur, gint64 start, gint duration);

#endif /* CAL_STORE_H */
