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
 *   - `recur_start` (v11) is the DAY the schedule is anchored on, as a
 *     unix local midnight — the "Monday" half of "every Monday at
 *     9:00 AM".  It is a DATE and never a time: the time of day is
 *     `recur_time`, and the editor shows the two side by side as one
 *     phrase ("Starting <date> at <time>").  0 means
 *     unset, and the anchor then falls back to the task's due date and
 *     finally to today, which is what every schedule set before v11 was
 *     built on.  A start in the FUTURE is honored as the first
 *     occurrence, so "every Monday starting the 7th" first fires on the
 *     7th rather than a week later.
 *   - `recur_time` is the ANCHOR'S TIME OF DAY, in minutes past local
 *     midnight, default 08:00 — the "9:00 AM" half of the sentence whose
 *     other half is `recur_start`.  It reaches every unit, by two routes
 *     that are the same rule seen twice: for the DATED units it is the
 *     time each occurrence lands on (recur_step re-applies it), and for
 *     the MINUTE and HOUR units it phase-locks the stride, so "starting
 *     at 09:00, every 3 hours" means 9, 12, 3 rather than three hours
 *     from whenever the task happened to be saved.  It used to be read
 *     for the dated units alone; the editor greyed it out for the other
 *     two, which is what left "every 3 hours" unable to say when it
 *     started.
 *   - `recur_lead` is how long before the occurrence the reset happens,
 *     in minutes.  A new schedule is seeded with
 *     task_recur_lead_default — one week, or half the period when that
 *     is shorter — and whatever is stored is then CLAMPED to shorter than
 *     the period (task_recur_lead_seconds), because a lead as long as the
 *     period would put the task permanently inside its own lead window
 *     and the schedule would run away from the calendar.
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
 * ONE PASS OVER ALL OF THEM, ARMED FOR THE NEXT DEADLINE.  Two decisions,
 * and they answer different objections:
 *
 *   - A PASS rather than a GSource per recurring task, because a pass is
 *     O(recurring tasks) with no state to leak, catches up correctly after
 *     the app has been closed for a week (occurrences are SKIPPED forward,
 *     not replayed one per period), and needs nothing cancelled when a
 *     task is deleted or its schedule edited.  N timers would be none of
 *     those things.
 *   - A DEADLINE rather than a fixed cadence, because a cadence has to
 *     pick one number for every schedule and gets each of them wrong at
 *     the ends.  It was 5 minutes: a per-minute repeat (reachable straight
 *     from the editor's custom row, and clamped to a zero lead) was seen
 *     five minutes late, and recur_catch_up lands on the LAST occurrence
 *     already due — so four in five simply never happened, while the
 *     editor's summary went on promising a minute nothing could keep.  At
 *     the other end an idle database woke 288 times a day to find nothing.
 *     The pass therefore ends by arming ITSELF for the earliest fire time
 *     it just computed: exact for the fast schedules, and no wakeups at
 *     all for a database with nothing recurring in it.
 *
 * Arming too EARLY is free — the pass finds nothing due and re-arms — so
 * only a deadline that moved NEARER has to be wired up anywhere.  That is
 * one call, task_recur_wake_by, from the editor's save.
 *
 * The pass is registered with the shared scheduler (task_worker.h), which
 * is what makes "re-arm everything after the database moves" cover it too;
 * it declares no interval, so the scheduler installs no timer and the
 * deadline is the only clock.  Unlike the sync engines it runs ON THE MAIN
 * THREAD against the app's own connection: it is a handful of statements
 * over one small query with no network and no process spawn, so a thread
 * plus a second connection would buy latency nobody can perceive and cost
 * the marshalling every one of those brings.  It is INITIAL_ALWAYS,
 * because occurrences that came due while the app was closed have to be
 * applied at launch — which is also what puts the first deadline up.
 *
 * THERE ARE NO CONFIG KEYS, and no Settings section (2026-09-08).  A
 * schedule set on a task IS the request to act on it, so a switch beside
 * that could only ever leave a repeat set in the editor doing nothing,
 * with nothing on the task to say why.  What made a master switch look
 * necessary was the cost of a background timer running for nothing, and
 * the deadline arming removed it: a database with nothing recurring arms
 * NO timer at all and the whole feature is one query at launch.  Both
 * keys are gone — recur_enabled and recur_check_min — and a stale one
 * left in an ini is inert.
 * =========================================================================== */

#ifndef TASK_RECUR_H
#define TASK_RECUR_H

#include "app.h"

/* task_recur_unit_label() — the user-facing plural of a unit ("minutes",
 * "hours", …), for the editor's unit combos.  Static string; out-of-range
 * reads "minutes".
 *
 * There are no PRESETS.  "Hourly / Daily / Weekly / Every 2 weeks /
 * Monthly / Custom…" was a combo in front of this until 2026-09-08, and
 * every one of those rows was nothing but a named (interval, unit) pair
 * that the "Every N units" row underneath could say directly — so the
 * combo's only real job was deciding whether that row was allowed to
 * appear at all.  Every schedule is now written the one way, and the
 * enum, the table and task_recur_preset_{label,spec,of} are gone with it.
 * ------------------------------------------------------------------------- */
const gchar *task_recur_unit_label(TaskRecurUnit unit);

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
 * task_recur_lead_default() — the lead to seed a NEW schedule with, in
 * minutes: TASK_RECUR_LEAD_DEFAULT (one week), or HALF the repeat period
 * when that is shorter.
 *
 * A flat week is meaningless at the fast end — it is longer than an hourly
 * schedule's whole period, and the clamp below would cut it to one minute
 * short of the period, which is the runaway case that clamp exists to
 * stop.  Half a period instead, ROUNDED DOWN to a whole number of weeks,
 * days, hours or minutes so the editor can say it in one of the four units
 * its combo offers: 3 days for a weekly repeat (half is 3.5, which reads
 * as "84 hours"), 12 hours for a daily one, 30 minutes for an hourly one,
 * 0 for a per-minute one (the column stores minutes and half of one is
 * less than that).  Rounding only ever shortens, so it can never reach the
 * clamp and the two rules cannot disagree.
 *
 * Returns 0 for a spec that does not recur (interval <= 0).  This is a
 * DEFAULT and nothing enforces it: a lead the user typed is theirs, and
 * the editor only re-seeds one it put there itself.
 * ------------------------------------------------------------------------- */
gint task_recur_lead_default(TaskRecurUnit unit, gint interval);

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
 * task_recur_seed() — the first occurrence AT OR AFTER `now_ts`.
 *
 * The anchor is `recur_start` when the user set one, else the task's
 * existing due date (so "make this weekly" keeps the weekday already
 * chosen), else `now_ts`.  Dated units land the anchor on recur_time
 * first; the minute and hour units phase-lock their stride to an explicit
 * start and otherwise simply step from now.
 *
 * An anchor still AHEAD of `now_ts` is itself the answer, which is what
 * makes "starting next Monday" start next Monday; a past anchor is
 * stepped forward until it passes now.
 *
 * Returns 0 when `t` does not recur.  Called by the pass whenever
 * recur_next is 0, and by the editor whenever the schedule is edited —
 * one function, so the two can never seed differently.
 * ------------------------------------------------------------------------- */
gint64 task_recur_seed(const Task *t, gint64 now_ts);

/* ---------------------------------------------------------------------------
 * task_recur_phrase() — the schedule in words: "Every Monday at 9:00 AM",
 * "Every 2 weeks on Thursday", "Every 3 hours".
 *
 * `next_ts` is an occurrence of the schedule, and is what names the
 * weekday — the weekday is a property of where the schedule LANDS, not of
 * the (interval, unit) pair, so it cannot be derived from `t` alone.  Pass
 * the same occurrence the summary is about.
 *
 * Returns a new string (g_free it); "" when `t` does not recur.  Plain
 * text, NOT markup.
 * ------------------------------------------------------------------------- */
gchar *task_recur_phrase(const Task *t, gint64 next_ts);

/* ---------------------------------------------------------------------------
 * task_recur_describe() — the editor's summary of `t`'s schedule: the
 * phrase above, a newline, then where the schedule lands next.  E.g.
 *
 *   Every Monday at 9:00 AM
 *   Next Sep 7, 2026 \xe2\x80\x94 resets to New Sep 2, 2026 at 9:00 AM
 *
 * The "Next" stamp drops its clock time for the DATED units, because the
 * phrase on the line above has just given it; the RESET stamp always
 * carries one, since an occurrence minus the lead can land at any time of
 * day and nothing else states it.
 *
 * Built from the SAME functions the pass uses, which is the point: the
 * sentence cannot promise a date the pass would not produce.  `now_ts` is
 * passed in rather than read here so the caller can describe a schedule it
 * has not saved yet.
 *
 * A task that does NOT recur reads "Does not repeat." rather than coming
 * back empty — the honest answer to the question this function is asked,
 * and a blank string answers nothing.  The editor does not show it (its
 * master switch says so, and the summary sits inside the body that switch
 * hides), but nothing about that is this function's business.  "" only
 * for a NULL task, or a schedule that yields no occurrence at all.
 *
 * Returns a new string (g_free it).  Plain text, NOT markup — the caller
 * escapes if it needs to.
 * ------------------------------------------------------------------------- */
gchar *task_recur_describe(const Task *t, gint64 now_ts);

/* ---------------------------------------------------------------------------
 * task_recur_pass() — roll every due recurring task forward, once.
 *
 * Main thread.  Returns how many tasks were changed, and fires
 * task_app_notify_changed() plus a status message when that is nonzero (a
 * roll-forward changes both a due date and possibly a status, so the task
 * pane and every open editor need to see it).  Zero changes are silent —
 * a capped deadline can bring the pass round with nothing to do, and it
 * has nothing to say then.
 *
 * It also ARMS THE NEXT DEADLINE, which is why every caller wants this
 * one function: the earliest fire time is a by-product of the walk it
 * already does, so "roll everything due forward" and "come back when the
 * next one is" are one answer computed once.  A database with nothing
 * recurring leaves no timer armed at all.
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

/* ---------------------------------------------------------------------------
 * task_recur_wake_by() — make sure the pass runs no later than `fire_ts`
 * (unix seconds), for a schedule that has just been written.
 *
 * O(1) and side-effect free: it moves the timer, it does not run a pass.
 * That matters on the editor's debounced save, where running one would let
 * a just-typed schedule roll its own task forward under the user's cursor.
 *
 * Only a NEARER deadline does anything — a schedule that slowed down or a
 * recurring task deleted needs no call at all, because the timer simply
 * fires early, finds nothing due and re-arms.  A no-op when `fire_ts` is
 * 0, which is what a task that does not recur passes.
 * ------------------------------------------------------------------------- */
void task_recur_wake_by(TaskApp *app, gint64 fire_ts);

#endif /* TASK_RECUR_H */
