#include "task_edit.h"

#include "recur.h"
#include "task_dates.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The last minute of a day, in minutes past midnight. */
#define LAST_MINUTE (23 * 60 + 59)

/* A lead unit in minutes, indexed by TaskRecurUnit: minute, hour, day, week. */
static const int lead_unit_minutes[TASK_EDIT_LEAD_N_UNITS] = { 1, 60, 1440, 10080 };

/*
 * skip_spaces — step over spaces and tabs.
 *
 * Inputs:
 *   p — a NUL-terminated string
 *
 * Output:
 *   the first character that is neither.
 */
static const char *
skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/*
 * read_digits — read a run of decimal digits as a number.  Hand-rolled
 * rather than strtol so that a sign, a leading space or an empty run is
 * not a number, and so a run of any length cannot overflow.
 *
 * Inputs:
 *   p     — where the digits should start
 *   value — receives the number, capped at `cap` + 1 once it passes `cap`
 *   cap   — the largest value the caller will accept
 *
 * Output:
 *   the first character after the digits; `p` itself when there are none
 *   (*value is then 0).
 */
static const char *
read_digits(const char *p, int *value, int cap)
{
    int v = 0;

    while (*p >= '0' && *p <= '9') {
        if (v <= cap) v = v * 10 + (*p - '0');
        if (v > cap) v = cap + 1;
        p++;
    }
    *value = v;
    return p;
}

/*
 * task_edit_date_parse — a date field's text, mid-typing safe.
 * Inputs/output are documented in task_edit.h.
 */
int64_t
task_edit_date_parse(const char *text, int64_t current)
{
    if (text == NULL || *skip_spaces(text) == '\0') return 0;

    int64_t parsed = task_due_parse(text);
    return parsed != 0 ? parsed : current;
}

/*
 * task_edit_time_parse — an "HH:MM" field's text, mid-typing safe.
 * Inputs/output are documented in task_edit.h.
 */
int
task_edit_time_parse(const char *text, int current)
{
    if (text == NULL) return current;

    int         hour   = 0, minute = 0;
    const char *start  = skip_spaces(text);
    const char *colon  = read_digits(start, &hour, 23);
    if (colon == start || *colon != ':') return current;

    const char *end = read_digits(colon + 1, &minute, 59);
    if (end == colon + 1 || *skip_spaces(end) != '\0') return current;
    if (hour > 23 || minute > 59) return current;

    return hour * 60 + minute;
}

/*
 * task_edit_time_format — minutes past midnight as "HH:MM".
 * Inputs are documented in task_edit.h.
 */
void
task_edit_time_format(int minutes, int fallback, char out[TASK_EDIT_TIME_LEN])
{
    if (minutes < 0 || minutes > LAST_MINUTE) minutes = fallback;
    snprintf(out, TASK_EDIT_TIME_LEN, "%02d:%02d", minutes / 60, minutes % 60);
}

/*
 * task_edit_count_parse — a whole-number field's text, mid-typing safe.
 * Inputs/output are documented in task_edit.h.
 */
int
task_edit_count_parse(const char *text, int lo, int hi, int current)
{
    if (text == NULL) return current;

    int         value = 0;
    const char *start = skip_spaces(text);
    const char *end   = read_digits(start, &value, hi);
    if (end == start || *skip_spaces(end) != '\0') return current;

    return value >= lo && value <= hi ? value : current;
}

/*
 * unit_is — whether a length's unit text is one of a set of spellings.
 *
 * Inputs:
 *   unit  — the text after the number, leading spaces already skipped
 *   names — the accepted spellings, NULL-terminated, lower case
 *
 * Output:
 *   true when the text, ignoring case and trailing spaces, is one of them.
 */
static bool
unit_is(const char *unit, const char *const *names)
{
    size_t len = strlen(unit);

    while (len > 0 && unit[len - 1] == ' ') len--;
    for (; *names != NULL; names++) {
        if (strlen(*names) == len && g_ascii_strncasecmp(unit, *names, len) == 0) return true;
    }
    return false;
}

/*
 * task_edit_duration_parse — a session length's text, mid-typing safe.
 * Inputs/output are documented in task_edit.h.
 */
int
task_edit_duration_parse(const char *text, int current)
{
    static const char *const hour_units[]   = { "", "h", "hr", "hrs", "hour", "hours", NULL };
    static const char *const minute_units[] = { "m", "min", "mins", "minute", "minutes", NULL };

    if (text == NULL) return current;

    const char *start = skip_spaces(text);
    if (!isdigit((unsigned char)*start) && *start != '.') return current;

    char   *end   = NULL;
    double  value = g_ascii_strtod(start, &end);
    if (end == start) return current;

    const char *unit = skip_spaces(end);
    double      minutes;
    if (unit_is(unit, hour_units)) {
        minutes = value * 60.0;
    } else if (unit_is(unit, minute_units)) {
        minutes = value;
    } else {
        return current;
    }
    if (!(minutes >= 0.5 && minutes < TASK_EDIT_DURATION_MAX + 0.5)) return current;
    return (int)(minutes + 0.5);
}

/*
 * lead_unit_or_days — a lead unit index, with anything out of range read as
 * days (what a control with no selection is taken to mean).
 *
 * Inputs:
 *   unit — the index as a control reported it
 *
 * Output:
 *   0..TASK_EDIT_LEAD_N_UNITS - 1.
 */
static int
lead_unit_or_days(int unit)
{
    return unit >= 0 && unit < TASK_EDIT_LEAD_N_UNITS ? unit : (int)TASK_RECUR_DAY;
}

/*
 * task_edit_lead_minutes — a count and a unit as stored minutes.
 * Inputs/output are documented in task_edit.h.
 */
int
task_edit_lead_minutes(int count, int unit)
{
    return count * lead_unit_minutes[lead_unit_or_days(unit)];
}

/*
 * task_edit_lead_split — stored minutes as the count and unit to show.
 * Inputs are documented in task_edit.h.
 */
void
task_edit_lead_split(int minutes, int *count, int *unit)
{
    *count = 0;
    *unit  = (int)TASK_RECUR_MINUTE;
    if (minutes <= 0) return;

    /* Largest unit first; minutes divide everything, so the loop always
     * ends by setting both. */
    for (int i = TASK_EDIT_LEAD_N_UNITS - 1; i >= 0; i--) {
        if (minutes % lead_unit_minutes[i] == 0) {
            *count = minutes / lead_unit_minutes[i];
            *unit  = i;
            return;
        }
    }
}

/*
 * form_every — the interval the form describes.
 *
 * Inputs:
 *   form — the controls as they stand
 *
 * Output:
 *   the count, or 0 when repeat is switched off (0 is how a task that does
 *   not recur is stored).
 */
static int
form_every(const task_edit_form_t *form)
{
    return form->repeat ? form->every : 0;
}

/*
 * form_unit — the repeat unit the form describes.
 *
 * Inputs:
 *   form — the controls as they stand
 *
 * Output:
 *   the unit; days when the control's index is out of range.
 */
static TaskRecurUnit
form_unit(const task_edit_form_t *form)
{
    return form->unit >= 0 && form->unit < TASK_RECUR_N_UNITS ? (TaskRecurUnit)form->unit
                                                              : TASK_RECUR_DAY;
}

/*
 * task_edit_lead_follow — move an untouched lead to the new period's
 * default.
 * Inputs/output are documented in task_edit.h.
 */
bool
task_edit_lead_follow(int *seen_every, int *seen_unit, const task_edit_form_t *form, int *lead)
{
    int every = form_every(form);
    int unit  = (int)form_unit(form);
    if (every == *seen_every && unit == *seen_unit) return false;

    /* "Untouched" means: still what the default was for the period the
     * lead was last matched against. */
    int  was     = task_recur_lead_default((TaskRecurUnit)*seen_unit, *seen_every);
    int  shown   = task_edit_lead_minutes(form->lead_count, form->lead_unit);
    bool follows = *seen_every <= 0 || shown == was;

    *seen_every = every;
    *seen_unit  = unit;
    if (follows) *lead = task_recur_lead_default((TaskRecurUnit)unit, every);
    return follows;
}

/*
 * schedule_read — the form's schedule controls into a task's recur_* fields
 * (all but recur_next).
 *
 * Inputs:
 *   form — the controls as they stand
 *   t    — the task; its recur_start is the fallback for a start date that
 *          cannot be read
 */
static void
schedule_read(const task_edit_form_t *form, Task *t)
{
    t->recur_interval = form_every(form);
    t->recur_unit     = form_unit(form);
    t->recur_time     = task_edit_time_parse(form->recur_time_text, SHARED_DB_TASK_RECUR_TIME_DEFAULT);
    t->recur_start    = task_edit_date_parse(form->recur_start_text, t->recur_start);
    t->recur_lead     = task_edit_lead_minutes(form->lead_count, form->lead_unit);
}

/*
 * session_read — the form's Scheduling controls into a task's
 * scheduled_start and scheduled_duration.
 *
 * Inputs:
 *   form — the controls as they stand
 *   t    — the task; its stored session is the fallback for any text that
 *          cannot be read
 */
static void
session_read(const task_edit_form_t *form, Task *t)
{
    bool    scheduled = t->scheduled_start != 0;
    int64_t day       = scheduled ? task_day_at_minute(t->scheduled_start, 0) : 0;
    int     minutes   = scheduled ? task_minutes_of_day(t->scheduled_start)
                                  : TASK_EDIT_SCHEDULE_TIME_DEFAULT;

    day     = task_edit_date_parse(form->sched_date_text, day);
    minutes = task_edit_time_parse(form->sched_time_text, minutes);
    t->scheduled_start    = day == 0 ? 0 : task_day_at_minute(day, minutes);
    t->scheduled_duration = task_edit_duration_parse(form->sched_duration_text,
                                                     t->scheduled_duration);
}

/*
 * task_edit_apply — the form's dates and schedule into a task to be saved.
 * Inputs are documented in task_edit.h.
 */
void
task_edit_apply(const task_edit_form_t *form, Task *t)
{
    t->due      = task_edit_date_parse(form->due_text, t->due);
    t->due_time = task_edit_time_parse(form->due_time_text, t->due_time);
    session_read(form, t);
    schedule_read(form, t);
}

/*
 * task_edit_schedule — the form's schedule as a bare Task.
 * Inputs/output are documented in task_edit.h.
 */
Task
task_edit_schedule(const task_edit_form_t *form, int64_t recur_next)
{
    Task t = { 0 };

    t.due = task_edit_date_parse(form->due_text, 0);
    schedule_read(form, &t);
    t.recur_next = recur_next;
    return t;
}

/*
 * task_edit_reseed — the next occurrence of the schedule as edited.
 * Inputs/output are documented in task_edit.h.
 */
int64_t
task_edit_reseed(const task_edit_form_t *form, int64_t now)
{
    Task t = task_edit_schedule(form, 0);

    return task_recur_seed(&t, now);
}
