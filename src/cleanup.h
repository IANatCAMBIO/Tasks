/* ===========================================================================
 * cleanup.h — deleting completed tasks once they are old, so the database
 * does not grow for ever
 *
 * A completed task is HIDDEN (the list's Show Completed switch), which is
 * the right answer for the week after it was ticked and the wrong one for
 * ever: nothing ever took those rows out of the file, so a database only
 * grew.  This is the one thing that takes them out, and it is OFF until
 * the user turns it on (TASK_CFG_CLEANUP_ENABLED): deleting a user's work
 * on their behalf is not a default anyone should wake up to after an
 * update.
 *
 * THE POLICY, in one sentence: a task completed more than N days, months
 * or years ago is deleted, with its subtasks.
 *
 *   - The retention is a number and a unit (TASK_CFG_CLEANUP_AFTER /
 *     _UNIT), the same (N, unit) pair the recurrence editor is written in,
 *     and the cutoff is real calendar arithmetic (task_recur_advance with
 *     a negative interval) — "6 months" is six months back from today, not
 *     180 days.
 *   - WHOLE FAMILIES only, and never one holding a recurring task.  Both
 *     rules belong to the write (db.h, task_db_tasks_purge_done), which is
 *     where they are spelled out.
 *   - AN ACTION ITEM'S NOTE KEEPS ITS TEXT.  A task a Notes '!' line
 *     created cannot simply be removed — the line names it, and Notes
 *     would write it back as a new task.  The pass TOMBSTONES those
 *     instead and leaves the link; Notes answers a tombstone by unmarking
 *     the line, which leaves the text exactly where the user typed it and
 *     puts a mark where the '!' was for how the work ended (Notes'
 *     serialize.h: ON_ACTION_DONE_MARK for a task deleted after it was
 *     completed — always this one, here — ON_ACTION_DROPPED_MARK for one
 *     deleted while still open).  The row itself goes on the next
 *     pass, once the link has gone with the mark.  So the task the line
 *     created is deleted and the sentence in the note is not — which is
 *     the whole point of treating them separately.
 *
 * WHEN: a pass at launch and one a day after that, through the shared
 * scheduler (task_worker.h), plus whatever Settings asks for by hand.  The
 * switch is read at every pass rather than at arming time, so turning it
 * on or off in Settings needs nothing re-armed.
 *
 * Main thread only, against the app's own connection: one transaction over
 * two statements, as the recurrence pass is, so a thread and a second
 * connection would buy nothing.
 * =========================================================================== */

#ifndef TASK_CLEANUP_H
#define TASK_CLEANUP_H

#include "task_app.h"

/* What the retention number may be, whatever unit it is in.  The floor is
 * 1 because 0 would mean "delete a task the moment it is ticked", which is
 * not a retention policy but a way to lose work. */
#define TASK_CLEANUP_AFTER_MIN 1
#define TASK_CLEANUP_AFTER_MAX 999

/* How many units the Settings popup offers (task_cleanup_units).          */
#define TASK_CLEANUP_N_UNITS 3

/* ---------------------------------------------------------------------------
 * task_cleanup_units() — the units a retention may be given in, in the
 * order Settings shows them: days, months, years.  Minutes, hours and
 * weeks are left out — a database is kept in the units a person says out
 * loud, and the three here are those.
 *
 * Output:
 *   a static array of TASK_CLEANUP_N_UNITS TaskRecurUnit values; the
 *   labels are task_recur_unit_label's.
 * ------------------------------------------------------------------------- */
const TaskRecurUnit *task_cleanup_units(void);

/* ---------------------------------------------------------------------------
 * task_cleanup_policy() — the retention as stored, never out of range: a
 * number clamped to TASK_CLEANUP_AFTER_MIN..MAX and a unit that is one of
 * task_cleanup_units' (anything else, from a hand-edited ini, reads as
 * months).  Every reader goes through here, so the clamp exists once.
 *
 * Inputs:
 *   after — out: the number; may be NULL
 *   unit  — out: its unit; may be NULL
 * ------------------------------------------------------------------------- */
void task_cleanup_policy(gint *after, TaskRecurUnit *unit);

/* ---------------------------------------------------------------------------
 * task_cleanup_policy_set() — store the retention, clamped as above.  Both
 * halves are written together because a half-applied change ("every 6
 * years" on the way to "every 6 months") would be a different policy.
 *
 * Inputs:
 *   after — the number
 *   unit  — one of task_cleanup_units' values
 * ------------------------------------------------------------------------- */
void task_cleanup_policy_set(gint after, TaskRecurUnit unit);

/* ---------------------------------------------------------------------------
 * task_cleanup_cutoff() — the moment a task has to have been completed
 * BEFORE to be old enough to go: now less the stored retention, by the
 * calendar.  Read whether or not cleanup is switched on, so Settings can
 * count what a pass would take before the user agrees to switch it on.
 *
 * Output:
 *   unix seconds.
 * ------------------------------------------------------------------------- */
gint64 task_cleanup_cutoff(void);

/* ---------------------------------------------------------------------------
 * task_cleanup_pending() — how many completed tasks a pass would take
 * right now, the Notes-born ones included (they disappear from Tasks
 * either way).  What a Settings confirmation says out loud.
 *
 * Inputs:
 *   app — the context; NULL or no database open reads as 0
 *
 * Output:
 *   how many top-level tasks.
 * ------------------------------------------------------------------------- */
guint task_cleanup_pending(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_cleanup_run() — ONE PASS, now, whatever the switch says (the switch
 * governs the scheduled passes; a pass asked for by hand was asked for).
 * Refreshes every pane and says what went in the status bar, both only
 * when something actually went.
 *
 * Inputs:
 *   app        — the context; NULL or no database open is a no-op
 *   tombstoned — out: how many were left for Notes to unmark instead of
 *                being removed outright; may be NULL
 *
 * Output:
 *   how many top-level tasks were removed outright.
 * ------------------------------------------------------------------------- */
guint task_cleanup_run(TaskApp *app, guint *tombstoned);

/* ---------------------------------------------------------------------------
 * task_cleanup_init() — register the pass with the shared scheduler
 * (task_worker.h).  Call once at startup; task_worker_arm_all then runs a
 * pass and puts it on its daily timer.
 *
 * Inputs:
 *   app — the context (nothing of it is borrowed)
 * ------------------------------------------------------------------------- */
void task_cleanup_init(TaskApp *app);

#endif /* TASK_CLEANUP_H */
