/* ===========================================================================
 * recur.h — recurring tasks for Tasks
 *
 * A recurring task is an ORDINARY task that carries a schedule (the five
 * recur_* columns on the row, schema v10).  There is no second row type,
 * no template, and no generated series: the same task comes back round.
 * That is the whole design, and it is what makes recurrence compose with
 * everything else for free — a recurring task has notes, subtasks,
 * attachments, a pin, a priority and a place in a list like any other, and
 * both integrations see nothing but the due date and status it ends up
 * with.
 *
 * THE RULE, in one sentence: a set interval BEFORE each occurrence, a
 * task that has been COMPLETED is reset to New, and its due date is moved
 * to that occurrence.
 *
 * Spelled out:
 *
 *   - `recur_interval` + `recur_unit` say how often ("every 2 weeks").
 *     0 interval means the task does not recur at all, which is every
 *     task until someone says otherwise.
 *   - `recur_time` is the time of day a DATED occurrence lands on, in
 *     minutes past local midnight, default 08:00 — so Daily, Weekly,
 *     Biweekly and Monthly all mean "at 8am".  The minute and hour units
 *     ignore it: "every 3 hours" has no time of day.
 *   - `recur_lead` is how long before the occurrence the reset happens,
 *     in minutes, default 5 days.  It is CLAMPED to shorter than the
 *     repeat period (task_recur_lead_seconds), because a lead as long as
 *     the period would put the task permanently inside its own lead
 *     window and the schedule would run away from the calendar.
 *   - `recur_next` is the unix time of the next occurrence.  It is
 *     bookkeeping, not a setting: 0 means "not computed yet" and the pass
 *     seeds it.  The user never sees the field, only the sentence the
 *     editor builds from it (task_recur_describe).
 *
 * A task that is NOT complete when its occurrence comes round keeps its
 * status — the due date still rolls forward, but New stays New and In
 * Progress stays In Progress.  Only Done is reset, because only Done is
 * the state that would otherwise hide the task from the user for good.
 *
 * WHY A PASS AND NOT A PER-TASK TIMER: one scan every few minutes is
 * O(recurring tasks) with no state to leak, it catches up correctly after
 * the app has been closed for a week (occurrences are SKIPPED forward, not
 * replayed one per period), and it needs no timer to be cancelled when a
 * task is deleted or its schedule edited.  A GSource per recurring task
 * would be none of those things.
 *
 * The pass is registered with the shared scheduler (task_worker.h), which
 * is what makes "re-arm everything after the database moves" cover it too.
 * Unlike the sync engines it runs ON THE MAIN THREAD against the app's own
 * connection: it is a handful of statements over one small query with no
 * network and no process spawn, so a thread plus a second connection would
 * buy latency nobody can perceive and cost the marshalling every one of
 * those brings.  It is INITIAL_ALWAYS, because occurrences that came due
 * while the app was closed have to be applied at launch even when the
 * periodic timer is switched off.
 *
 * Config keys (in the [tasks] group; Settings → Recurring Tasks):
 *   recur_enabled    1|0, default 1 — the master switch for the pass.
 *   recur_check_min  minutes between passes, default 5; 0 = launch only.
 * =========================================================================== */

#ifndef TASK_RECUR_H
#define TASK_RECUR_H

#include "app.h"

/* The default check cadence, shared with the Settings spin button so the
 * UI and the timer cannot disagree about what an unset key means (the
 * same arrangement backup.h makes).                                       */
#define TASK_RECUR_CHECK_DEFAULT 5

/* ---------------------------------------------------------------------------
 * The presets the editor offers, in menu order.  A preset is nothing but a
 * named (interval, unit) pair — CUSTOM is the absence of one, so the user's
 * own numbers show through.  NEVER is index 0 so an untouched combo reads
 * as "does not recur", matching recur_interval = 0 on disk.
 *
 * TASK_RECUR_PRESET_CUSTOM is deliberately LAST: the combo's index is the
 * enum value, and "Custom…" belongs at the bottom of a list of shortcuts.
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_RECUR_PRESET_NEVER = 0,
    TASK_RECUR_PRESET_HOURLY,
    TASK_RECUR_PRESET_DAILY,
    TASK_RECUR_PRESET_WEEKLY,
    TASK_RECUR_PRESET_BIWEEKLY,
    TASK_RECUR_PRESET_MONTHLY,
    TASK_RECUR_PRESET_CUSTOM
} TaskRecurPreset;

#define TASK_RECUR_N_PRESETS 7

/* task_recur_preset_label() — the user-facing name ("Never" / "Hourly" /
 * … / "Custom…").  Returns a static string; an out-of-range value reads
 * "Never" rather than NULL, so it can never blank a combo row.             */
const gchar *task_recur_preset_label(TaskRecurPreset preset);

/* task_recur_unit_label() — the user-facing plural of a unit ("minutes",
 * "hours", …), for the custom row's combo.  Static string; out-of-range
 * reads "minutes".                                                         */
const gchar *task_recur_unit_label(TaskRecurUnit unit);

/* ---------------------------------------------------------------------------
 * task_recur_preset_spec() — the (interval, unit) a preset stands for.
 *
 *   preset   — the preset to expand.
 *   interval — out: repeats per unit, or 0 for NEVER.
 *   unit     — out: the unit.
 *
 * Returns FALSE for CUSTOM (and for an out-of-range value) WITHOUT
 * touching the outputs: custom means "whatever the user typed", so there
 * is nothing to expand and the caller must leave its own numbers alone.
 * ------------------------------------------------------------------------- */
gboolean task_recur_preset_spec(TaskRecurPreset preset, gint *interval,
                                TaskRecurUnit *unit);

/* ---------------------------------------------------------------------------
 * task_recur_preset_of() — which preset a task's schedule matches.
 *
 * The inverse of task_recur_preset_spec, so the editor can open a combo on
 * the right row: interval 0 is NEVER, an exact preset match is that
 * preset, and anything else is CUSTOM.  It looks ONLY at interval and
 * unit — the time of day and the lead are settings of their own and a
 * changed 8am does not stop a schedule being "Weekly".
 * ------------------------------------------------------------------------- */
TaskRecurPreset task_recur_preset_of(const Task *t);

/* ---------------------------------------------------------------------------
 * task_recur_period_seconds() — how long one repeat lasts, approximately.
 *
 * Months and years are taken as 30 and 365 days.  That is honest for the
 * ONE thing this is for — clamping the lead so it cannot swallow a whole
 * period (task_recur_lead_seconds) — and is never used to compute a date:
 * every actual occurrence goes through task_recur_advance, which does real
 * calendar arithmetic with GDateTime.
 *
 * Returns 0 for a non-recurring spec (interval <= 0).
 * ------------------------------------------------------------------------- */
gint64 task_recur_period_seconds(TaskRecurUnit unit, gint interval);

/* ---------------------------------------------------------------------------
 * task_recur_lead_seconds() — `t`'s reset lead in seconds, CLAMPED.
 *
 * The clamp is the load-bearing part: an hourly task with the default
 * five-day lead would otherwise sit permanently inside its own lead
 * window, and every pass would roll it forward again.  The result is
 * therefore never more than one minute short of a full period, and never
 * negative.  Returns 0 when `t` does not recur.
 * ------------------------------------------------------------------------- */
gint64 task_recur_lead_seconds(const Task *t);

/* ---------------------------------------------------------------------------
 * task_recur_advance() — `from` plus one repeat, by the CALENDAR.
 *
 * GDateTime does the arithmetic, so months keep their day-of-month where
 * the target month is long enough (and clamp to its last day where it is
 * not, which is what "monthly" has to mean on the 31st), weeks keep their
 * weekday, and a day step across a DST boundary keeps its wall-clock time
 * rather than sliding an hour.
 *
 * The month clamp is STICKY, and that is accepted rather than overlooked:
 * each step starts from the PREVIOUS occurrence, so a monthly task
 * anchored on the 31st runs Jan 31, Feb 28, Mar 28.  Un-sticking it needs
 * the original day-of-month kept as a separate anchor — a column and a
 * rule for a case nobody has asked for.
 *
 * `at_minute` >= 0 re-applies that time of day after the step (for the
 * day/week/month/year units, whose occurrences are "at 8am on such a
 * day"); pass -1 to keep the clock time the arithmetic produced, which is
 * what the minute and hour units want.
 *
 * Returns the new unix time, or `from` unchanged when interval <= 0.
 * ------------------------------------------------------------------------- */
gint64 task_recur_advance(gint64 from, TaskRecurUnit unit, gint interval,
                          gint at_minute);

/* ---------------------------------------------------------------------------
 * task_recur_seed() — the first occurrence STRICTLY AFTER `now_ts`.
 *
 * Anchored on the task's existing due date when it has one, so "make this
 * weekly" keeps the weekday the user already chose, and on `now_ts` when
 * it does not.  Dated units land the anchor on recur_time first; the
 * minute and hour units simply step from now.
 *
 * Returns 0 when `t` does not recur.  Called by the pass whenever
 * recur_next is 0, and by the editor whenever the schedule is edited —
 * one function, so the two can never seed differently.
 * ------------------------------------------------------------------------- */
gint64 task_recur_seed(const Task *t, gint64 now_ts);

/* ---------------------------------------------------------------------------
 * task_recur_describe() — the editor's one-line summary of `t`'s schedule,
 * e.g. "Next Sep 3, 2026 at 8:00 AM \xe2\x80\x94 resets to New on Aug 29".
 *
 * Built from the SAME functions the pass uses, which is the point: the
 * sentence cannot promise a date the pass would not produce.  `now_ts` is
 * passed in rather than read here so the caller can describe a schedule it
 * has not saved yet.
 *
 * Returns a new string (g_free it); "" when `t` does not recur.  Plain
 * text, NOT markup — the caller escapes if it needs to.
 * ------------------------------------------------------------------------- */
gchar *task_recur_describe(const Task *t, gint64 now_ts);

/* ---------------------------------------------------------------------------
 * task_recur_pass() — roll every due recurring task forward, once.
 *
 * Main thread.  Returns how many tasks were changed, and fires
 * task_app_notify_changed() plus a status message when that is nonzero (a
 * roll-forward changes both a due date and possibly a status, so the task
 * pane and every open editor need to see it).  Zero changes are silent —
 * this runs every few minutes and has nothing to say most times.
 *
 * Two callers: recur_run, the scheduler's tick, and the scratchpad test
 * harness — which is also why the pure helpers above (advance, seed,
 * lead_seconds, period_seconds) are declared here rather than left static.
 * They have no in-tree caller outside recur.c and are NOT dead: the
 * harness links against build/recur.o and drives them directly, the same
 * arrangement CLAUDE.md describes for the older test_bt.c.  Deleting one
 * because "nothing calls it" breaks the tests, not the app.
 * ------------------------------------------------------------------------- */
gint task_recur_pass(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_recur_init() — register the periodic pass with the shared
 * scheduler (task_worker.h).  Call once at startup, before the first
 * window or thread exists, like the other registrants in main().
 * ------------------------------------------------------------------------- */
void task_recur_init(TaskApp *app);

/* task_recur_auto_start() — (re)arm just this worker, for a Settings
 * change to its own interval.  Everything else re-arms all of them.        */
void task_recur_auto_start(TaskApp *app, const gchar *db_path);

#endif /* TASK_RECUR_H */
