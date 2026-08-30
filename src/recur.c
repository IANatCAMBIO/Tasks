/* ===========================================================================
 * recur.c — recurring tasks (see recur.h for the rule and the reasoning)
 *
 * Three layers, smallest first: the preset tables, the calendar arithmetic
 * (all of it through GDateTime — nothing here adds 86400 to a timestamp
 * and calls it a day), and the pass that walks the recurring tasks once.
 * =========================================================================== */

#include "recur.h"
#include "task_worker.h"
#include <string.h>
#include <time.h>

/* How many calendar steps one catch-up may take.  A daily task and 55
 * years of absence; anything past that is a nonsense anchor rather than a
 * schedule, and the seeder re-anchors on today instead of spinning.  The
 * FIXED-length units never come near it — they are fast-forwarded with one
 * division (see recur_catch_up).                                           */
#define RECUR_MAX_STEPS 20000

/* ===========================================================================
 * The presets.
 * =========================================================================== */

/* One row per TaskRecurPreset, IN ENUM ORDER: the editor's combo appends
 * these in order and reads its active index back as the enum value, so the
 * order here is the order on screen.  interval 0 marks the two rows that
 * are not a schedule at all (Never, and Custom — which means "the user's
 * own numbers, don't touch them").                                        */
static const struct {
    const gchar   *label;
    gint           interval;
    TaskRecurUnit  unit;
} recur_presets[TASK_RECUR_N_PRESETS] = {
    { "Never",             0, TASK_RECUR_MINUTE },
    { "Hourly",            1, TASK_RECUR_HOUR   },
    { "Daily",             1, TASK_RECUR_DAY    },
    { "Weekly",            1, TASK_RECUR_WEEK   },
    { "Every 2 weeks",     2, TASK_RECUR_WEEK   },
    { "Monthly",           1, TASK_RECUR_MONTH  },
    { "Custom\xe2\x80\xa6", 0, TASK_RECUR_MINUTE },
};

/* Plural unit names for the custom row's combo, indexed by TaskRecurUnit
 * — so its active index is the enum value too.                            */
static const gchar *recur_units[TASK_RECUR_N_UNITS] = {
    "minutes", "hours", "days", "weeks", "months", "years"
};

/* task_recur_preset_label() — see recur.h.                                 */
const gchar *
task_recur_preset_label(TaskRecurPreset preset)
{
    if (preset < 0 || preset >= TASK_RECUR_N_PRESETS)
        return recur_presets[TASK_RECUR_PRESET_NEVER].label;
    return recur_presets[preset].label;
}

/* task_recur_unit_label() — see recur.h.                                   */
const gchar *
task_recur_unit_label(TaskRecurUnit unit)
{
    if (unit < 0 || unit >= TASK_RECUR_N_UNITS)
        return recur_units[TASK_RECUR_MINUTE];
    return recur_units[unit];
}

/* task_recur_preset_spec() — see recur.h.                                  */
gboolean
task_recur_preset_spec(TaskRecurPreset preset, gint *interval,
                       TaskRecurUnit *unit)
{
    if (preset < 0 || preset >= TASK_RECUR_N_PRESETS ||
        preset == TASK_RECUR_PRESET_CUSTOM)
        return FALSE;                /* nothing to expand — outputs UNTOUCHED */
    if (interval != NULL)
        *interval = recur_presets[preset].interval;
    if (unit != NULL)
        *unit = recur_presets[preset].unit;
    return TRUE;
}

/* task_recur_preset_of() — see recur.h.                                    */
TaskRecurPreset
task_recur_preset_of(const Task *t)
{
    if (t == NULL || t->recur_interval <= 0)
        return TASK_RECUR_PRESET_NEVER;
    /* From HOURLY, so the interval-0 rows at either end (Never, Custom)
     * can never be matched by a real schedule.                             */
    for (gint p = TASK_RECUR_PRESET_HOURLY; p < TASK_RECUR_N_PRESETS; p++)
        if (recur_presets[p].interval == t->recur_interval &&
            recur_presets[p].unit == t->recur_unit)
            return (TaskRecurPreset)p;
    return TASK_RECUR_PRESET_CUSTOM;
}

/* ===========================================================================
 * Calendar arithmetic.
 * =========================================================================== */

/* recur_dated() — does this unit describe a TIME OF DAY?
 *
 * Days and longer land "at 8am on such a day", so recur_time applies to
 * them; minutes and hours are a stride from wherever the schedule started
 * and have no time of day to honor.  The units are in ascending duration
 * order (db.h), which is what makes this one comparison.                   */
static gboolean
recur_dated(TaskRecurUnit unit)
{
    return unit >= TASK_RECUR_DAY;
}

/*
 * recur_at_minute — the same LOCAL DAY as `ts`, at `minutes` past midnight.
 *
 * Inputs:
 *   ts      — a unix time; its local calendar date is what is kept
 *   minutes — minutes past local midnight, clamped to 00:00–23:59
 *
 * Output:
 *   the new unix time, or `ts` unchanged if GDateTime cannot represent it.
 *   Seconds are zeroed — an occurrence is a minute, not an instant.
 */
static gint64
recur_at_minute(gint64 ts, gint minutes)
{
    if (minutes < 0)
        minutes = 0;
    if (minutes > 23 * 60 + 59)
        minutes = 23 * 60 + 59;
    GDateTime *dt = g_date_time_new_from_unix_local(ts);
    if (dt == NULL)
        return ts;
    GDateTime *out = g_date_time_new_local(g_date_time_get_year(dt),
                                           g_date_time_get_month(dt),
                                           g_date_time_get_day_of_month(dt),
                                           minutes / 60, minutes % 60, 0.0);
    g_date_time_unref(dt);
    if (out == NULL)
        return ts;
    gint64 v = g_date_time_to_unix(out);
    g_date_time_unref(out);
    return v;
}

/* task_recur_advance() — see recur.h.                                      */
gint64
task_recur_advance(gint64 from, TaskRecurUnit unit, gint interval,
                   gint at_minute)
{
    if (interval <= 0)
        return from;
    GDateTime *dt = g_date_time_new_from_unix_local(from);
    if (dt == NULL)
        return from;
    GDateTime *out = NULL;
    switch (unit) {
    case TASK_RECUR_MINUTE: out = g_date_time_add_minutes(dt, interval); break;
    case TASK_RECUR_HOUR:   out = g_date_time_add_hours(dt, interval);   break;
    case TASK_RECUR_DAY:    out = g_date_time_add_days(dt, interval);    break;
    case TASK_RECUR_WEEK:   out = g_date_time_add_weeks(dt, interval);   break;
    case TASK_RECUR_MONTH:  out = g_date_time_add_months(dt, interval);  break;
    case TASK_RECUR_YEAR:   out = g_date_time_add_years(dt, interval);   break;
    }
    g_date_time_unref(dt);
    if (out == NULL)
        return from;
    gint64 ts = g_date_time_to_unix(out);
    g_date_time_unref(out);
    /* Re-apply the time of day for the dated units.  g_date_time_add_days
     * already keeps the wall clock across a DST boundary; this is what
     * makes an occurrence land at 8am even when the STARTING point did
     * not (a task whose due date was set before recurrence was, say).      */
    if (at_minute >= 0)
        ts = recur_at_minute(ts, at_minute);
    return ts;
}

/* recur_step() — one repeat of THIS task's schedule, honoring its time of
 * day where the unit has one.  Every advance in this file goes through
 * here, so "which units get recur_time" is decided in exactly one place.   */
static gint64
recur_step(const Task *t, gint64 from)
{
    return task_recur_advance(from, t->recur_unit, t->recur_interval,
                              recur_dated(t->recur_unit) ? t->recur_time
                                                         : -1);
}

/* task_recur_period_seconds() — see recur.h (approximate BY DESIGN).       */
gint64
task_recur_period_seconds(TaskRecurUnit unit, gint interval)
{
    if (interval <= 0)
        return 0;
    gint64 unit_s;
    switch (unit) {
    case TASK_RECUR_MINUTE: unit_s = 60;             break;
    case TASK_RECUR_HOUR:   unit_s = 3600;           break;
    case TASK_RECUR_DAY:    unit_s = 86400;          break;
    case TASK_RECUR_WEEK:   unit_s = 7  * 86400;     break;
    case TASK_RECUR_MONTH:  unit_s = 30 * 86400;     break;
    case TASK_RECUR_YEAR:   unit_s = 365 * (gint64)86400; break;
    default:                unit_s = 60;             break;
    }
    return unit_s * interval;
}

/* task_recur_lead_seconds() — see recur.h.  The CLAMP is the point.        */
gint64
task_recur_lead_seconds(const Task *t)
{
    if (t == NULL || t->recur_interval <= 0)
        return 0;
    gint64 lead   = (gint64)t->recur_lead * 60;
    gint64 period = task_recur_period_seconds(t->recur_unit,
                                              t->recur_interval);
    if (lead < 0)
        lead = 0;
    /* A lead >= the period means the task is ALWAYS inside its own lead
     * window: every pass fires, rolls the due date forward and advances
     * the schedule, so an hourly task with the five-day default would run
     * off into next year within a few minutes.  So: one minute short of a
     * period, and NEVER negative.
     *
     * The test is `lead >= period`, not `period > 60 && lead > period-60`.
     * That earlier form left the shortest period of all — every ONE
     * minute, period exactly 60 — unclamped, so the default five-day lead
     * survived and the very first pass fast-forwarded the schedule five
     * days ahead in one step.  One minute is reachable straight from the
     * editor's custom row, so it is an ordinary case and not a corner.    */
    if (lead >= period)
        lead = MAX(period - 60, 0);
    return lead;
}

/*
 * recur_catch_up — the last occurrence that has already come due, and the
 * one after it.
 *
 * Inputs:
 *   t     — the recurring task (interval > 0)
 *   next  — the occurrence recorded on the row (or freshly seeded)
 *   limit — the newest occurrence that counts as due: now + the lead
 *
 * Outputs:
 *   *fire  — the occurrence to apply (the LAST one due, not the first)
 *   *after — the occurrence following it, to record as recur_next
 *   returns FALSE, leaving both untouched, when nothing is due yet.
 *
 * Occurrences are SKIPPED, never replayed: an app closed for a month owes
 * one roll-forward, not thirty.  The fixed-length units skip with a single
 * division — a per-minute schedule and a month of absence is otherwise
 * 43200 GDateTime allocations — while the calendar units step, because a
 * month is not a fixed number of seconds and only GDateTime knows where
 * the 31st of the next one is.
 */
static gboolean
recur_catch_up(const Task *t, gint64 next, gint64 limit,
               gint64 *fire, gint64 *after)
{
    if (next > limit)
        return FALSE;

    gint64 occ = next;
    if (!recur_dated(t->recur_unit)) {
        gint64 period = task_recur_period_seconds(t->recur_unit,
                                                   t->recur_interval);
        if (period > 0)
            occ += ((limit - occ) / period) * period;
    }

    gint64 nxt = recur_step(t, occ);
    for (gint i = 0; nxt <= limit && i < RECUR_MAX_STEPS; i++) {
        occ = nxt;
        nxt = recur_step(t, occ);
        if (nxt <= occ)
            break;                   /* no forward progress — see below     */
    }
    /* A schedule that does not advance would be re-fired by every pass
     * for ever.  task_recur_advance only fails to move when GDateTime
     * cannot represent the result, which is not a case worth spinning on:
     * push the stamp past `limit` so the row settles until the clock
     * genuinely catches up.                                                */
    if (nxt <= occ)
        nxt = limit + 60;

    *fire  = occ;
    *after = nxt;
    return TRUE;
}

/* task_recur_seed() — see recur.h.                                         */
gint64
task_recur_seed(const Task *t, gint64 now_ts)
{
    if (t == NULL || t->recur_interval <= 0)
        return 0;

    /* Minutes and hours have no time of day.  With no explicit start they
     * have no meaningful anchor either, so the next one is simply one
     * stride from now.  A start date PHASE-LOCKS the stride to it instead
     * — "every 3 hours" then means midnight, 3am, 6am rather than
     * "three hours from whenever I happened to save this".
     *
     * The catch-up is ONE DIVISION, the same trick recur_catch_up uses and
     * for the same reason: a per-minute schedule started a month ago is
     * otherwise 43200 GDateTime allocations to walk.                       */
    if (!recur_dated(t->recur_unit)) {
        gint64 period = task_recur_period_seconds(t->recur_unit,
                                                  t->recur_interval);
        if (t->recur_start <= 0 || period <= 0)
            return task_recur_advance(now_ts, t->recur_unit,
                                      t->recur_interval, -1);
        gint64 occ = t->recur_start;
        if (occ <= now_ts)
            occ += ((now_ts - occ) / period + 1) * period;
        return occ;
    }

    /* Dated units anchor on the start date when the user set one, and
     * otherwise on the due date they already chose — so "make this
     * weekly" keeps its weekday and "monthly" its day of the month
     * without anybody having to name a start.  The anchor itself counts
     * when it is still ahead of now: a task starting next Tuesday recurs
     * NEXT TUESDAY, not the one after.                                     */
    gint64 anchor = t->recur_start > 0 ? t->recur_start
                                       : (t->due > 0 ? t->due : now_ts);
    gint64 occ    = recur_at_minute(anchor, t->recur_time);
    for (gint i = 0; occ <= now_ts && i < RECUR_MAX_STEPS; i++)
        occ = recur_step(t, occ);

    /* The anchor was absurd (a due date decades back, or a step that
     * cannot advance): start from today instead of reporting a date in
     * the past.  Re-anchored, this loop cannot run more than once for
     * any unit day-or-longer.                                              */
    if (occ <= now_ts) {
        occ = recur_at_minute(now_ts, t->recur_time);
        for (gint i = 0; occ <= now_ts && i < RECUR_MAX_STEPS; i++)
            occ = recur_step(t, occ);
    }
    return occ;
}

/* ===========================================================================
 * The editor's summary line.
 * =========================================================================== */

/* recur_minute_of() — a unix time's minutes past LOCAL midnight, or -1 if
 * it cannot be read.  The bridge between an occurrence (an instant) and
 * everything here that speaks in minutes-past-midnight.                    */
static gint
recur_minute_of(gint64 ts)
{
    GDateTime *dt = g_date_time_new_from_unix_local(ts);
    if (dt == NULL)
        return -1;
    gint m = g_date_time_get_hour(dt) * 60 + g_date_time_get_minute(dt);
    g_date_time_unref(dt);
    return m;
}

/* ---------------------------------------------------------------------------
 * recur_stamp() — "Sep 3, 2026 at 8:00 AM" for a unix time, or just
 * "Sep 3, 2026" when `with_time` is FALSE.  Returns a new string (g_free).
 *
 * The DATE goes through task_due_format, so it reads exactly like every
 * other date in the app.
 * ------------------------------------------------------------------------- */
static gchar *
recur_stamp(gint64 ts, gboolean with_time)
{
    gchar *date = task_due_format(ts);
    if (!with_time)
        return date;
    gint m = recur_minute_of(ts);
    if (m < 0)
        return date;
    gchar *clock = task_clock_format(m);
    gchar *out   = g_strdup_printf("%s at %s", date, clock);
    g_free(clock);
    g_free(date);
    return out;
}

/* task_recur_phrase() — see recur.h.
 *
 * The WEEKDAY comes off the occurrence rather than out of the schedule,
 * because the schedule does not hold one: "every week" anchored on the 7th
 * IS "every Monday", and the only place that fact lives is the date the
 * seeder produced.  Same reason nothing here names a day of the month —
 * "Every month" plus the Next line below it says it without an ordinal
 * suffix table.                                                            */
gchar *
task_recur_phrase(const Task *t, gint64 next_ts)
{
    if (t == NULL || t->recur_interval <= 0)
        return g_strdup("");

    gint   n    = t->recur_interval;
    gchar *body = NULL;

    switch (t->recur_unit) {
    case TASK_RECUR_MINUTE:
        body = n == 1 ? g_strdup("Every minute")
                      : g_strdup_printf("Every %d minutes", n);
        break;
    case TASK_RECUR_HOUR:
        body = n == 1 ? g_strdup("Every hour")
                      : g_strdup_printf("Every %d hours", n);
        break;
    case TASK_RECUR_DAY:
        body = n == 1 ? g_strdup("Every day")
                      : g_strdup_printf("Every %d days", n);
        break;
    case TASK_RECUR_WEEK: {
        GDateTime *dt  = g_date_time_new_from_unix_local(next_ts);
        gchar     *day = dt != NULL ? g_date_time_format(dt, "%A") : NULL;
        if (dt != NULL)
            g_date_time_unref(dt);
        if (day == NULL)             /* no weekday to name — say the unit  */
            body = n == 1 ? g_strdup("Every week")
                          : g_strdup_printf("Every %d weeks", n);
        else
            body = n == 1 ? g_strdup_printf("Every %s", day)
                          : g_strdup_printf("Every %d weeks on %s", n, day);
        g_free(day);
        break;
    }
    case TASK_RECUR_MONTH:
        body = n == 1 ? g_strdup("Every month")
                      : g_strdup_printf("Every %d months", n);
        break;
    case TASK_RECUR_YEAR:
        body = n == 1 ? g_strdup("Every year")
                      : g_strdup_printf("Every %d years", n);
        break;
    default:
        body = g_strdup("");
        break;
    }

    /* Only the dated units have a time of day to state — "every 3 hours at
     * 8am" is not a thing anyone can mean, which is the same rule
     * recur_step and the editor's greying follow.                          */
    if (!recur_dated(t->recur_unit))
        return body;
    gchar *clock = task_clock_format(t->recur_time);
    gchar *out   = g_strdup_printf("%s at %s", body, clock);
    g_free(clock);
    g_free(body);
    return out;
}

/* task_recur_describe() — see recur.h.                                     */
gchar *
task_recur_describe(const Task *t, gint64 now_ts)
{
    if (t == NULL || t->recur_interval <= 0)
        return g_strdup("");

    gint64 next = t->recur_next > 0 ? t->recur_next
                                    : task_recur_seed(t, now_ts);
    if (next <= 0)
        return g_strdup("");
    gint64 lead = task_recur_lead_seconds(t);

    gchar *phrase = task_recur_phrase(t, next);
    /* The phrase has already given the time of day for the dated units, so
     * the Next stamp drops it rather than saying it twice on two adjacent
     * lines.  The reset stamp keeps its own: an occurrence minus the lead
     * lands at whatever time the arithmetic leaves it, and nothing else in
     * the sentence states that.                                            */
    gchar *when  = recur_stamp(next, !recur_dated(t->recur_unit));
    gchar *reset = recur_stamp(next - lead, TRUE);
    /* Say so when the clamp shortened the lead, rather than printing a
     * reset date the user's own number does not explain.  Silence there
     * is how a control comes to look broken.                               */
    gboolean clamped = lead != (gint64)t->recur_lead * 60;
    gchar *out = g_strdup_printf(
        "%s\nNext %s \xe2\x80\x94 resets to New %s%s", phrase, when, reset,
        clamped ? " (lead shortened to fit the repeat)" : "");
    g_free(phrase);
    g_free(when);
    g_free(reset);
    return out;
}

/* ===========================================================================
 * The pass.
 * =========================================================================== */

/*
 * recur_due_of — split an occurrence into the two columns a due date is.
 *
 * Inputs:
 *   ts       — the occurrence, as a unix time
 *
 * Outputs:
 *   *due_min — the occurrence's minutes past LOCAL midnight, for
 *              tasks.due_time; left alone when `ts` cannot be read
 *   returns    tasks.due: local midnight of the day it falls on, or 0
 *
 * tasks.due is DATE-ONLY (db.h) and so is Google's, which is why the day
 * and the clock go to two different columns rather than into one
 * timestamp.  Since v12 the clock DOES reach the task, in due_time — that
 * is what makes "every Monday at 9:00 AM" produce a due date that says
 * 9:00 AM instead of leaving the time locked inside the schedule.
 *
 * The minutes come off `ts` ITSELF rather than from recur_time, and that
 * is deliberate: for the dated units recur_step has already applied
 * recur_time, so the two agree, while for the minute and hour units —
 * which have no recur_time at all — `ts` is the only thing that knows
 * what o'clock the occurrence is.  One rule, no branch.
 */
static gint64
recur_due_of(gint64 ts, gint *due_min)
{
    GDateTime *dt = g_date_time_new_from_unix_local(ts);
    if (dt == NULL)
        return 0;
    gint64 due = task_due_from_ymd(g_date_time_get_year(dt),
                                   g_date_time_get_month(dt),
                                   g_date_time_get_day_of_month(dt));
    if (due_min != NULL)
        *due_min = g_date_time_get_hour(dt) * 60 +
                   g_date_time_get_minute(dt);
    g_date_time_unref(dt);
    return due;
}

/* task_recur_pass() — see recur.h.                                         */
gint
task_recur_pass(TaskApp *app)
{
    if (app == NULL || app->db == NULL)
        return 0;

    GPtrArray *tasks = task_db_tasks_recurring(app->db);
    gint64 nowts   = (gint64)time(NULL);
    gint   changed = 0;              /* rows actually rolled forward        */
    gint   reopened = 0;             /* of those, ones that were Done       */

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        gint64 next = t->recur_next > 0 ? t->recur_next
                                        : task_recur_seed(t, nowts);
        if (next <= 0)
            continue;                /* nothing a schedule can be made of   */

        gint64 lead = task_recur_lead_seconds(t);
        gint64 fire = 0, after = 0;
        if (recur_catch_up(t, next, nowts + lead, &fire, &after)) {
            gint due_min = TASK_DUE_TIME_DEFAULT;
            gint64 due   = recur_due_of(fire, &due_min);
            /* TWO independent halves, and either may be a no-op — the same
             * division the Kanban drop makes.  A completed task must be
             * reopened; a due date that is already the occurrence's needs
             * no write.  When NEITHER applies (an hourly schedule firing
             * repeatedly inside one day, say) only the local stamp moves,
             * so an idle recurring task does not dirty itself for sync
             * every few minutes.
             *
             * due_time joins the "has it changed?" test rather than riding
             * along silently: an hourly schedule keeps the same DAY for
             * hours at a stretch, and the o'clock is then the only thing
             * about the due date that moved.                             */
            if (t->status == TASK_STATUS_DONE || due != t->due ||
                due_min != t->due_time) {
                task_db_task_recur_apply(app->db, t->id, due, due_min,
                                         after);
                changed++;
                if (t->status == TASK_STATUS_DONE)
                    reopened++;
            } else if (after != t->recur_next) {
                task_db_task_recur_set_next(app->db, t->id, after);
            }
        } else if (next != t->recur_next) {
            /* Seeded this pass (recur_next was 0, or the schedule was
             * edited): record it so the next pass has it, with NO
             * updated_at bump.                                            */
            task_db_task_recur_set_next(app->db, t->id, next);
        }
    }
    task_ptr_array_free_tasks(tasks);

    /* Silent when nothing moved: this runs every few minutes and has
     * nothing to say most times.  A roll-forward changes a due date and
     * possibly a status, so it is the FULL notify — the sidebar's Due
     * Today and Favorites rows and every open editor all read those.      */
    if (changed > 0) {
        task_app_notify_changed(app);
        if (reopened > 0)
            task_app_status(app,
                "Recurrence: %d task%s rolled forward, %d reopened",
                changed, changed == 1 ? "" : "s", reopened);
        else
            task_app_status(app, "Recurrence: %d task%s rolled forward",
                            changed, changed == 1 ? "" : "s");
    }
    return changed;
}

/* ===========================================================================
 * The periodic worker (task_worker.h).
 * =========================================================================== */

/* recur_run() — the scheduler's entry point.  ON THE MAIN THREAD, against
 * the app's own connection: unlike the sync engines this pass makes no
 * network call and spawns no process, so `db_path` is not even needed —
 * the handle it would open is already here and already correct.            */
static void
recur_run(TaskApp *app, const gchar *db_path)
{
    (void)db_path;
    task_recur_pass(app);
}

/* The GSource id lives HERE rather than on TaskApp: nothing outside this
 * file arms, disarms or reads it, and the scheduler only borrows the
 * pointer (task_worker.h).                                                 */
static guint recur_timer = 0;

static const TaskWorkerDef recur_worker = {
    .id               = "recurrence",
    /* Runs BEFORE the integrations (Notes is -10, Google 0): a task the
     * pass reopens should reach both in the same press of Sync rather
     * than waiting for the next one.                                      */
    .sort             = -20,
    .enabled_key      = "recur_enabled",
    .enabled_default  = TRUE,
    .interval_key     = "recur_check_min",
    .interval_default = TASK_RECUR_CHECK_DEFAULT,
    /* ALWAYS: occurrences that came due while the app was closed have to
     * be applied at launch, and "check only when I ask" (interval 0) does
     * not mean "ignore the week I was away".                              */
    .initial          = TASK_WORKER_INITIAL_ALWAYS,
    /* No `running` flag: the pass is synchronous on the main thread, so a
     * tick can never land on one already in flight.                       */
    .running          = NULL,
    .timer            = &recur_timer,
    .run              = recur_run,
    .ready            = NULL,
    .on_arm           = NULL,
    .on_blocked       = NULL,
};

/* task_recur_auto_start() — see recur.h.                                   */
void
task_recur_auto_start(TaskApp *app, const gchar *db_path)
{
    task_worker_arm(app, &recur_worker, db_path);
}

/* task_recur_init() — see recur.h.                                         */
void
task_recur_init(TaskApp *app)
{
    (void)app;                       /* nothing of the app's is borrowed    */
    task_worker_register(&recur_worker);
}
