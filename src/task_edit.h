/*
 * task_edit — what the task editor's fields MEAN: reading typed dates,
 * times and counts without ever losing a stored value to half-typed text,
 * the recurrence lead as a count and a unit, and turning the editor's
 * schedule controls into the recurrence fields of a Task.
 *
 * The editor saves while the user is still typing (a debounce, not a
 * commit), so every reader here takes the value currently stored and keeps
 * it when the text is partial or invalid: "2026-1" on its way to
 * "2026-10-04" must not clear a due date.  Only an EMPTY date field means
 * "no date".
 *
 * This is the logic of the GTK build's editor_window.c, lifted out of the
 * window so it can be tested; the window itself only moves strings between
 * its controls and these functions.
 *
 * Plain C.  MAIN THREAD ONLY: dates go through task_dates.h, whose
 * timezone cache is main-thread state.
 */

#ifndef TASK_EDIT_H
#define TASK_EDIT_H

#include <stdbool.h>
#include <stdint.h>

#include "db.h"

/* The lead is shown as a count of minutes, hours, days or weeks: the first
 * four TaskRecurUnit values, so a lead unit IS a TaskRecurUnit. */
#define TASK_EDIT_LEAD_N_UNITS 4

/* Bytes task_edit_time_format writes: "HH:MM" and its terminator. */
#define TASK_EDIT_TIME_LEN 6

/* The ranges of the editor's two counts. */
#define TASK_EDIT_EVERY_MIN 1       /* "Repeat every N ..."               */
#define TASK_EDIT_EVERY_MAX 999
#define TASK_EDIT_LEAD_MIN  0       /* "Reset to New N ... beforehand"    */
#define TASK_EDIT_LEAD_MAX  999

/*
 * task_edit_form_t — the editor's date and schedule controls as they stand,
 * unparsed.  The strings are borrowed for the length of one call.
 */
typedef struct {
    const char *due_text;           /* the Due date field                 */
    const char *due_time_text;      /* the Due time field, "HH:MM"        */
    bool        repeat;             /* the "Repeat this task" switch      */
    int         every;              /* TASK_EDIT_EVERY_MIN..MAX           */
    int         unit;               /* a TaskRecurUnit; out of range
                                     * reads as days                      */
    const char *recur_start_text;   /* the Starting date field            */
    const char *recur_time_text;    /* the Starting time field, "HH:MM"   */
    int         lead_count;         /* TASK_EDIT_LEAD_MIN..MAX            */
    int         lead_unit;          /* 0..TASK_EDIT_LEAD_N_UNITS - 1; out
                                     * of range reads as days             */
} task_edit_form_t;

/*
 * task_edit_date_parse — a date field's text as a local-midnight timestamp.
 * Accepts what task_due_parse accepts: "YYYY-MM-DD", "M/D/YY", "M/D/YYYY".
 *
 * Inputs:
 *   text    — the field's text; NULL reads as empty
 *   current — the value now stored
 *
 * Output:
 *   0 when the text is empty or only spaces (the date is cleared); the
 *   parsed date when it is one; otherwise `current`, so partial or invalid
 *   text changes nothing.
 */
int64_t task_edit_date_parse(const char *text, int64_t current);

/*
 * task_edit_time_parse — a 24-hour "H:MM" / "HH:MM" field as minutes past
 * midnight.  Digits, one colon, digits and nothing else but surrounding
 * spaces: no sign, no seconds, no AM/PM, and an hour must be given.
 *
 * Inputs:
 *   text    — the field's text; NULL reads as empty
 *   current — the value now stored
 *
 * Output:
 *   0..1439 when the text is a time of day; otherwise `current` (empty
 *   text included: a time has no "none").
 */
int task_edit_time_parse(const char *text, int current);

/*
 * task_edit_time_format — minutes past midnight as "HH:MM", the spelling
 * task_edit_time_parse reads back.
 *
 * Inputs:
 *   minutes  — the stored value
 *   fallback — used instead when `minutes` is outside 0..1439 (a
 *              hand-edited database); itself assumed to be in range
 *   out      — receives the text, TASK_EDIT_TIME_LEN bytes
 */
void task_edit_time_format(int minutes, int fallback, char out[TASK_EDIT_TIME_LEN]);

/*
 * task_edit_count_parse — a whole-number field ("every N", the lead count).
 *
 * Inputs:
 *   text    — the field's text; NULL reads as empty
 *   lo, hi  — the inclusive range allowed
 *   current — the value now in effect
 *
 * Output:
 *   the number when the text is digits only (spaces around them allowed)
 *   and within the range; otherwise `current`.
 */
int task_edit_count_parse(const char *text, int lo, int hi, int current);

/*
 * task_edit_lead_minutes — a lead given as a count and a unit, in the
 * minutes the database stores.
 *
 * Inputs:
 *   count — how many
 *   unit  — 0..TASK_EDIT_LEAD_N_UNITS - 1; anything else reads as days
 *
 * Output:
 *   the lead in minutes.
 */
int task_edit_lead_minutes(int count, int unit);

/*
 * task_edit_lead_split — a stored lead as the count and unit to show: the
 * LARGEST unit that divides it exactly, so 4320 minutes reads "3 days" and
 * 90 minutes stays "90 minutes".
 *
 * Inputs:
 *   minutes — the stored lead; zero or less reads as 0 minutes
 *   count   — receives the count
 *   unit    — receives the unit, 0..TASK_EDIT_LEAD_N_UNITS - 1
 */
void task_edit_lead_split(int minutes, int *count, int *unit);

/*
 * task_edit_lead_follow — keep the lead on its DEFAULT while the user has
 * not chosen one, as the repeat period changes.
 *
 * The default lead depends on the period (task_recur_lead_default: a week,
 * or half the period when that is shorter).  When "every N units" moves,
 * a lead that still equals the default for the period it was shown with
 * is replaced by the default for the new period; a lead the user typed is
 * left alone.  A schedule being switched on (nothing seen before) always
 * takes the default.
 *
 * Inputs:
 *   seen_every — in: the interval the lead was last matched against, 0 for
 *                "repeat was off"; out: the form's interval
 *   seen_unit  — in/out: likewise the unit
 *   form       — the controls as they stand (repeat, every, unit and the
 *                lead count and unit are read)
 *   lead       — receives the new lead in minutes, only when the function
 *                returns true
 *
 * Output:
 *   true when the lead must be replaced by *lead (the caller shows it with
 *   task_edit_lead_split); false when it stays as it is.
 */
bool task_edit_lead_follow(int *seen_every, int *seen_unit, const task_edit_form_t *form,
                           int *lead);

/*
 * task_edit_apply — write the form's due date, due time and schedule into
 * a task about to be saved.  Each field falls back to the value the task
 * already holds when its text is partial or invalid, with ONE exception
 * carried from the GTK build: a schedule time that cannot be read becomes
 * SHARED_DB_TASK_RECUR_TIME_DEFAULT, not the stored time.  recur_next is NOT
 * touched: it is the editor's bookkeeping (task_edit_reseed).
 *
 * Inputs:
 *   form — the controls as they stand
 *   t    — the task as stored; its due, due_time and recur_* fields (other
 *          than recur_next) are overwritten
 */
void task_edit_apply(const task_edit_form_t *form, Task *t);

/*
 * task_edit_schedule — the schedule the form describes as a bare Task, for
 * the recurrence helpers of recur.h (task_recur_describe, task_recur_seed).
 * Unlike task_edit_apply there is no stored task behind it: a date that
 * cannot be read is "none".
 *
 * Inputs:
 *   form       — the controls as they stand
 *   recur_next — the next occurrence to carry, 0 for "not computed"
 *
 * Output:
 *   a Task with only due, the recur_* fields and nothing else set (its
 *   strings are NULL: do not task_free it).
 */
Task task_edit_schedule(const task_edit_form_t *form, int64_t recur_next);

/*
 * task_edit_reseed — the schedule was edited: its next occurrence, computed
 * afresh by the same function the recurrence pass uses (task_recur_seed).
 *
 * Inputs:
 *   form — the controls as they stand
 *   now  — the present moment, unix seconds
 *
 * Output:
 *   the first occurrence at or after `now`, or 0 when repeat is off.
 */
int64_t task_edit_reseed(const task_edit_form_t *form, int64_t now);

#endif /* TASK_EDIT_H */
