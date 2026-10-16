/*
 * edit_test.c — what the editor's fields mean (src/task_edit.h): typed
 * dates, times and counts read without losing a stored value to half-typed
 * text, the recurrence lead as a count and a unit, the lead following its
 * default, and the schedule controls turned into a task's recurrence.
 *
 * Dates are compared against task_due_from_ymd, so the tests hold in any
 * timezone.
 */

#include "check.h"
#include "recur.h"
#include "task_dates.h"
#include "task_edit.h"

#include <string.h>

/* A stored value no test text parses to, to show "kept what was stored". */
#define STORED_DATE ((int64_t)86400 * 365 * 40)
#define STORED_TIME 615             /* 10:15 */

/*
 * iso_is — whether a timestamp is the given calendar day.
 *
 * Inputs:
 *   ts   — the timestamp
 *   want — "YYYY-MM-DD"
 *
 * Output:
 *   non-zero when task_due_format_iso spells it that way.
 */
static int
iso_is(int64_t ts, const char *want)
{
    gchar *iso = task_due_format_iso(ts);
    int    ok  = strcmp(iso, want) == 0;

    g_free(iso);
    return ok;
}

/*
 * time_text_is — whether minutes format as the given "HH:MM".
 *
 * Inputs:
 *   minutes  — the stored value
 *   fallback — used when it is out of range
 *   want     — the expected text
 *
 * Output:
 *   non-zero when task_edit_time_format writes it.
 */
static int
time_text_is(int minutes, int fallback, const char *want)
{
    char text[TASK_EDIT_TIME_LEN];

    task_edit_time_format(minutes, fallback, text);
    return strcmp(text, want) == 0;
}

/*
 * test_dates — the date field: empty clears, a date parses, anything else
 * keeps the stored value.
 */
static void
test_dates(void)
{
    int64_t oct4 = task_due_from_ymd(2026, 10, 4);

    check(oct4 != 0 && iso_is(oct4, "2026-10-04"), "date: the fixture day is a real local midnight");

    check(task_edit_date_parse("", STORED_DATE) == 0, "date: empty text clears the date");
    check(task_edit_date_parse("   ", STORED_DATE) == 0, "date: only spaces clears it too");
    check(task_edit_date_parse(" \t ", STORED_DATE) == 0, "date: ... and tabs");
    check(task_edit_date_parse(NULL, STORED_DATE) == 0, "date: NULL reads as empty");

    check(task_edit_date_parse("2026-10-04", STORED_DATE) == oct4, "date: YYYY-MM-DD");
    check(task_edit_date_parse("  2026-10-04  ", STORED_DATE) == oct4, "date: spaces around it");
    check(task_edit_date_parse("2026-10-4", STORED_DATE) == oct4, "date: no leading zero needed");
    check(task_edit_date_parse("10/4/26", STORED_DATE) == oct4, "date: M/D/YY");
    check(task_edit_date_parse("10/4/2026", STORED_DATE) == oct4, "date: M/D/YYYY");
    check(task_edit_date_parse("10/04/2026", STORED_DATE) == oct4, "date: MM/DD/YYYY");
    check(task_edit_date_parse("2026-10-04", 0) == oct4, "date: a first date, nothing stored");

    /* Every prefix a user passes through while typing a date. */
    check(task_edit_date_parse("2", STORED_DATE) == STORED_DATE, "date: partial '2' keeps the stored date");
    check(task_edit_date_parse("2026", STORED_DATE) == STORED_DATE, "date: partial year keeps it");
    check(task_edit_date_parse("2026-", STORED_DATE) == STORED_DATE, "date: partial 'YYYY-' keeps it");
    check(task_edit_date_parse("2026-10", STORED_DATE) == STORED_DATE, "date: partial 'YYYY-MM' keeps it");
    check(task_edit_date_parse("2026-10-", STORED_DATE) == STORED_DATE, "date: partial 'YYYY-MM-' keeps it");
    check(task_edit_date_parse("10/4", STORED_DATE) == STORED_DATE, "date: partial 'M/D' keeps it");
    check(task_edit_date_parse("10/4/", STORED_DATE) == STORED_DATE, "date: partial 'M/D/' keeps it");
    check(iso_is(task_edit_date_parse("2026-10-1", STORED_DATE), "2026-10-01"),
          "date: a complete shorter date on the way to a longer one is read as written");

    check(task_edit_date_parse("tomorrow", STORED_DATE) == STORED_DATE, "date: words keep the stored date");
    check(task_edit_date_parse("2026-13-01", STORED_DATE) == STORED_DATE, "date: month 13 keeps it");
    check(task_edit_date_parse("2026-00-10", STORED_DATE) == STORED_DATE, "date: month 0 keeps it");
    check(task_edit_date_parse("2026-10-00", STORED_DATE) == STORED_DATE, "date: day 0 keeps it");
    check(task_edit_date_parse("2026-10-32", STORED_DATE) == STORED_DATE, "date: day 32 keeps it");
    check(task_edit_date_parse("2026-02-30", STORED_DATE) == STORED_DATE, "date: February 30 keeps it");
    check(task_edit_date_parse("2025-02-29", STORED_DATE) == STORED_DATE,
          "date: February 29 in a common year keeps it");
    check(iso_is(task_edit_date_parse("2028-02-29", STORED_DATE), "2028-02-29"),
          "date: February 29 in a leap year is a date");
    check(task_edit_date_parse("1969-12-31", STORED_DATE) == STORED_DATE, "date: before 1970 keeps it");
    check(task_edit_date_parse("13/1/26", STORED_DATE) == STORED_DATE, "date: M/D with month 13 keeps it");
    check(task_edit_date_parse("abc", 0) == 0, "date: invalid text with nothing stored stays no date");

    /* What the fields show is what they read back. */
    const char *days[] = { "2026-01-01", "2026-03-08", "2026-11-01", "2026-12-31", "2030-06-15" };
    for (size_t i = 0; i < sizeof days / sizeof days[0]; i++) {
        check(iso_is(task_edit_date_parse(days[i], 0), days[i]), "date: ISO text round-trips");
    }
}

/*
 * test_times — the time field: H:MM in 24 hours, or the stored value.
 */
static void
test_times(void)
{
    check(task_edit_time_parse("08:00", STORED_TIME) == 480, "time: 08:00");
    check(task_edit_time_parse("8:00", STORED_TIME) == 480, "time: one hour digit");
    check(task_edit_time_parse("00:00", STORED_TIME) == 0, "time: midnight");
    check(task_edit_time_parse("23:59", STORED_TIME) == 1439, "time: the last minute");
    check(task_edit_time_parse("14:30", STORED_TIME) == 870, "time: an afternoon time");
    check(task_edit_time_parse("9:5", STORED_TIME) == 545, "time: one minute digit");
    check(task_edit_time_parse(" 9:30 ", STORED_TIME) == 570, "time: spaces around it");
    check(task_edit_time_parse("009:030", STORED_TIME) == 570, "time: leading zeros");

    check(task_edit_time_parse("", STORED_TIME) == STORED_TIME, "time: empty keeps the stored time");
    check(task_edit_time_parse(NULL, STORED_TIME) == STORED_TIME, "time: NULL keeps it");
    check(task_edit_time_parse("9", STORED_TIME) == STORED_TIME, "time: partial '9' keeps it");
    check(task_edit_time_parse("09", STORED_TIME) == STORED_TIME, "time: partial '09' keeps it");
    check(task_edit_time_parse("9:", STORED_TIME) == STORED_TIME, "time: partial '9:' keeps it");
    check(task_edit_time_parse(":30", STORED_TIME) == STORED_TIME, "time: no hour keeps it");
    check(task_edit_time_parse(":", STORED_TIME) == STORED_TIME, "time: a bare colon keeps it");

    check(task_edit_time_parse("24:00", STORED_TIME) == STORED_TIME, "time: hour 24 keeps it");
    check(task_edit_time_parse("12:60", STORED_TIME) == STORED_TIME, "time: minute 60 keeps it");
    check(task_edit_time_parse("-1:00", STORED_TIME) == STORED_TIME, "time: a negative hour keeps it");
    check(task_edit_time_parse("+9:30", STORED_TIME) == STORED_TIME, "time: a sign keeps it");
    check(task_edit_time_parse("9:-5", STORED_TIME) == STORED_TIME, "time: a signed minute keeps it");
    check(task_edit_time_parse("9:30pm", STORED_TIME) == STORED_TIME, "time: AM/PM keeps it");
    check(task_edit_time_parse("9:30:00", STORED_TIME) == STORED_TIME, "time: seconds keep it");
    check(task_edit_time_parse("9 :30", STORED_TIME) == STORED_TIME, "time: a space inside keeps it");
    check(task_edit_time_parse("9.30", STORED_TIME) == STORED_TIME, "time: a dot keeps it");
    check(task_edit_time_parse("ab:cd", STORED_TIME) == STORED_TIME, "time: letters keep it");
    check(task_edit_time_parse("99999999999999999999:00", STORED_TIME) == STORED_TIME,
          "time: an absurd hour keeps it (and does not overflow)");
    check(task_edit_time_parse("9:99999999999999999999", STORED_TIME) == STORED_TIME,
          "time: an absurd minute keeps it");

    check(time_text_is(0, 480, "00:00"), "time format: midnight");
    check(time_text_is(545, 480, "09:05"), "time format: zero padded");
    check(time_text_is(1439, 480, "23:59"), "time format: the last minute");
    check(time_text_is(-1, 480, "08:00"), "time format: a negative value shows the fallback");
    check(time_text_is(1440, 480, "08:00"), "time format: past the day shows the fallback");

    char text[TASK_EDIT_TIME_LEN];
    int  round_trips = 1;
    for (int m = 0; m < 1440; m++) {
        task_edit_time_format(m, 0, text);
        if (task_edit_time_parse(text, -1) != m) round_trips = 0;
    }
    check(round_trips, "time: every minute of the day formats and reads back");
}

/*
 * test_counts — the whole-number fields.
 */
static void
test_counts(void)
{
    check(task_edit_count_parse("1", 1, 999, 5) == 1, "count: the low bound");
    check(task_edit_count_parse("999", 1, 999, 5) == 999, "count: the high bound");
    check(task_edit_count_parse(" 42 ", 1, 999, 5) == 42, "count: spaces around it");
    check(task_edit_count_parse("007", 1, 999, 5) == 7, "count: leading zeros");
    check(task_edit_count_parse("0", 0, 999, 5) == 0, "count: zero where zero is allowed");

    check(task_edit_count_parse("0", 1, 999, 5) == 5, "count: below the range keeps the current value");
    check(task_edit_count_parse("1000", 1, 999, 5) == 5, "count: above the range keeps it");
    check(task_edit_count_parse("", 1, 999, 5) == 5, "count: empty keeps it");
    check(task_edit_count_parse(NULL, 1, 999, 5) == 5, "count: NULL keeps it");
    check(task_edit_count_parse("12a", 1, 999, 5) == 5, "count: trailing letters keep it");
    check(task_edit_count_parse("-3", 0, 999, 5) == 5, "count: a sign keeps it");
    check(task_edit_count_parse("1.5", 1, 999, 5) == 5, "count: a fraction keeps it");
    check(task_edit_count_parse("1 2", 1, 999, 5) == 5, "count: a space inside keeps it");
    check(task_edit_count_parse("99999999999999999999", 1, 999, 5) == 5,
          "count: an absurd number keeps it (and does not overflow)");
}

/*
 * test_lead_units — the lead as a count and a unit, both ways.
 */
static void
test_lead_units(void)
{
    int count = -1, unit = -1;

    check(task_edit_lead_minutes(30, TASK_RECUR_MINUTE) == 30, "lead: minutes");
    check(task_edit_lead_minutes(2, TASK_RECUR_HOUR) == 120, "lead: hours");
    check(task_edit_lead_minutes(3, TASK_RECUR_DAY) == 4320, "lead: days");
    check(task_edit_lead_minutes(2, TASK_RECUR_WEEK) == 20160, "lead: weeks");
    check(task_edit_lead_minutes(0, TASK_RECUR_WEEK) == 0, "lead: none");
    check(task_edit_lead_minutes(2, TASK_RECUR_MONTH) == 2880, "lead: a unit past weeks reads as days");
    check(task_edit_lead_minutes(2, -1) == 2880, "lead: no unit selected reads as days");

    task_edit_lead_split(0, &count, &unit);
    check(count == 0 && unit == TASK_RECUR_MINUTE, "lead split: none is 0 minutes");
    task_edit_lead_split(-5, &count, &unit);
    check(count == 0 && unit == TASK_RECUR_MINUTE, "lead split: a negative lead is 0 minutes");
    task_edit_lead_split(30, &count, &unit);
    check(count == 30 && unit == TASK_RECUR_MINUTE, "lead split: 30 minutes");
    task_edit_lead_split(60, &count, &unit);
    check(count == 1 && unit == TASK_RECUR_HOUR, "lead split: 60 minutes is 1 hour");
    task_edit_lead_split(90, &count, &unit);
    check(count == 90 && unit == TASK_RECUR_MINUTE, "lead split: 90 minutes stays minutes");
    task_edit_lead_split(720, &count, &unit);
    check(count == 12 && unit == TASK_RECUR_HOUR, "lead split: 12 hours");
    task_edit_lead_split(1440, &count, &unit);
    check(count == 1 && unit == TASK_RECUR_DAY, "lead split: 1 day");
    task_edit_lead_split(2940, &count, &unit);
    check(count == 49 && unit == TASK_RECUR_HOUR, "lead split: 49 hours is not 2 days");
    task_edit_lead_split(4320, &count, &unit);
    check(count == 3 && unit == TASK_RECUR_DAY, "lead split: 3 days");
    task_edit_lead_split(SHARED_DB_TASK_RECUR_LEAD_DEFAULT, &count, &unit);
    check(count == 1 && unit == TASK_RECUR_WEEK, "lead split: the one-week default");
    task_edit_lead_split(20160, &count, &unit);
    check(count == 2 && unit == TASK_RECUR_WEEK, "lead split: 2 weeks");

    /* Every default the core can seed is shown exactly and reads back. */
    int exact = 1;
    for (int u = 0; u < TASK_RECUR_N_UNITS; u++) {
        for (int every = 1; every <= 60; every++) {
            int lead = task_recur_lead_default((TaskRecurUnit)u, every);
            task_edit_lead_split(lead, &count, &unit);
            if (task_edit_lead_minutes(count, unit) != lead || count > TASK_EDIT_LEAD_MAX) exact = 0;
        }
    }
    check(exact, "lead split: every default lead round-trips within the count's range");
}

/*
 * test_lead_follow — the lead keeps to its default until the user picks one.
 */
static void
test_lead_follow(void)
{
    task_edit_form_t form = { .repeat = true, .every = 1, .unit = TASK_RECUR_DAY,
                              .lead_count = 0, .lead_unit = TASK_RECUR_MINUTE };
    int seen_every = 0, seen_unit = TASK_RECUR_DAY, lead = -1;

    /* Repeat is switched on: the default for "every day" (12 hours). */
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 720,
          "follow: a schedule switched on takes the default lead");
    check(seen_every == 1 && seen_unit == TASK_RECUR_DAY, "follow: ... and remembers the period");
    task_edit_lead_split(lead, &form.lead_count, &form.lead_unit);

    lead = -1;
    check(!task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == -1,
          "follow: an unchanged period changes nothing");

    /* Day -> week with the lead untouched: 3 days. */
    form.unit = TASK_RECUR_WEEK;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 4320,
          "follow: an untouched lead moves to the new period's default");
    task_edit_lead_split(lead, &form.lead_count, &form.lead_unit);

    /* Every 1 week -> every 4 weeks: the default is capped at one week. */
    form.every = 4;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) &&
          lead == SHARED_DB_TASK_RECUR_LEAD_DEFAULT,
          "follow: ... and to the one-week ceiling for a long period");
    task_edit_lead_split(lead, &form.lead_count, &form.lead_unit);

    /* The user picks 2 days, then changes the period: theirs is kept. */
    form.lead_count = 2;
    form.lead_unit  = TASK_RECUR_DAY;
    form.unit       = TASK_RECUR_MONTH;
    lead = -1;
    check(!task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == -1,
          "follow: a lead the user chose is left alone");
    check(seen_every == 4 && seen_unit == TASK_RECUR_MONTH,
          "follow: ... but the period is still remembered");

    /* A chosen lead that happens to equal the default follows again. */
    form.lead_count = 1;
    form.lead_unit  = TASK_RECUR_WEEK;
    form.unit       = TASK_RECUR_HOUR;
    form.every      = 1;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 30,
          "follow: a lead equal to the old default counts as untouched");
    task_edit_lead_split(lead, &form.lead_count, &form.lead_unit);

    /* Every minute: half a minute rounds down to no lead at all. */
    form.unit = TASK_RECUR_MINUTE;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 0,
          "follow: a per-minute repeat has no lead");
    task_edit_lead_split(lead, &form.lead_count, &form.lead_unit);

    /* Switched off: the interval reads 0, and an untouched lead goes too. */
    form.repeat = false;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 0 &&
          seen_every == 0, "follow: switching repeat off is a period change to 'none'");

    /* A chosen lead survives switching off, and is replaced on switching
     * on again: a new schedule always starts from its default. */
    form.unit       = TASK_RECUR_DAY;
    seen_every      = 1;
    seen_unit       = TASK_RECUR_DAY;
    form.lead_count = 5;
    form.lead_unit  = TASK_RECUR_HOUR;
    lead            = -1;
    check(!task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == -1,
          "follow: a chosen lead is kept while repeat is switched off");
    form.repeat = true;
    check(task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead) && lead == 720,
          "follow: ... and switching on again starts from the default");

    /* A unit control with nothing selected reads as days. */
    form.unit  = -1;
    seen_every = 1;
    seen_unit  = TASK_RECUR_DAY;
    check(!task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead),
          "follow: an out-of-range unit reads as days");
}

/*
 * test_apply — the form into a task to be saved.
 */
static void
test_apply(void)
{
    int64_t oct4 = task_due_from_ymd(2026, 10, 4);
    int64_t nov2 = task_due_from_ymd(2026, 11, 2);
    Task    t    = { 0 };
    task_edit_form_t form = {
        .due_text = "2026-10-04", .due_time_text = "14:30",
        .repeat = true, .every = 2, .unit = TASK_RECUR_WEEK,
        .recur_start_text = "2026-11-02", .recur_time_text = "09:00",
        .lead_count = 3, .lead_unit = TASK_RECUR_DAY,
    };

    t.recur_next = 777;
    task_edit_apply(&form, &t);
    check(t.due == oct4 && t.due_time == 870, "apply: the due date and time");
    check(t.recur_interval == 2 && t.recur_unit == TASK_RECUR_WEEK, "apply: every 2 weeks");
    check(t.recur_start == nov2 && t.recur_time == 540, "apply: starting Nov 2 at 09:00");
    check(t.recur_lead == 4320, "apply: a 3-day lead in minutes");
    check(t.recur_next == 777, "apply: recur_next is not touched");

    /* Half-typed text everywhere: the stored values stand. */
    form.due_text         = "2026-1";
    form.due_time_text    = "14:";
    form.recur_start_text = "11/";
    task_edit_apply(&form, &t);
    check(t.due == oct4 && t.due_time == 870, "apply: partial due text keeps the stored due");
    check(t.recur_start == nov2, "apply: partial start text keeps the stored start");

    /* The one exception: an unreadable schedule time is the default. */
    form.recur_time_text = "9:";
    task_edit_apply(&form, &t);
    check(t.recur_time == SHARED_DB_TASK_RECUR_TIME_DEFAULT,
          "apply: an unreadable schedule time becomes the default time");

    /* Empty date fields clear; an empty time does not. */
    form.due_text         = "";
    form.due_time_text    = "";
    form.recur_start_text = "  ";
    task_edit_apply(&form, &t);
    check(t.due == 0, "apply: an empty due field clears the date");
    check(t.due_time == 870, "apply: an empty due time keeps the stored time");
    check(t.recur_start == 0, "apply: an empty start field clears the anchor");

    /* Repeat off: interval 0 whatever the count says; the rest is kept so
     * that switching back on finds it. */
    form.repeat = false;
    task_edit_apply(&form, &t);
    check(t.recur_interval == 0, "apply: repeat off stores interval 0");
    check(t.recur_unit == TASK_RECUR_WEEK && t.recur_lead == 4320,
          "apply: ... and keeps the unit and lead");

    form.repeat    = true;
    form.unit      = TASK_RECUR_N_UNITS;
    form.lead_unit = TASK_EDIT_LEAD_N_UNITS;
    task_edit_apply(&form, &t);
    check(t.recur_unit == TASK_RECUR_DAY, "apply: an out-of-range unit reads as days");
    check(t.recur_lead == 3 * 1440, "apply: an out-of-range lead unit reads as days");
}

/*
 * test_schedule — the bare schedule and its next occurrence, against fixed
 * moments so the answers do not depend on today.
 */
static void
test_schedule(void)
{
    int64_t jan1  = task_due_from_ymd(2026, 1, 1);
    int64_t jan5  = task_due_from_ymd(2026, 1, 5);
    int64_t jan10 = task_due_from_ymd(2026, 1, 10);
    int64_t jan11 = task_due_from_ymd(2026, 1, 11);
    int64_t noon  = 12 * 3600;
    task_edit_form_t form = {
        .due_text = "bogus", .due_time_text = "08:00",
        .repeat = true, .every = 1, .unit = TASK_RECUR_DAY,
        .recur_start_text = "2026-01-01", .recur_time_text = "09:00",
        .lead_count = 12, .lead_unit = TASK_RECUR_HOUR,
    };

    Task t = task_edit_schedule(&form, 4242);
    check(t.due == 0, "schedule: an unreadable due date is no date");
    check(t.recur_interval == 1 && t.recur_unit == TASK_RECUR_DAY && t.recur_start == jan1 &&
          t.recur_time == 540 && t.recur_lead == 720, "schedule: the controls as recur fields");
    check(t.recur_next == 4242, "schedule: carries the next occurrence it was given");
    check(t.title == NULL && t.notes == NULL && t.id == 0, "schedule: nothing else is set");

    /* A past start steps forward to the first occurrence after now. */
    check(task_edit_reseed(&form, jan10 + noon) == jan11 + 9 * 3600,
          "reseed: daily from a past start lands on tomorrow's time");
    check(task_edit_reseed(&form, jan10 + 3600) == jan10 + 9 * 3600,
          "reseed: ... or today's, when that is still ahead");

    /* A start still ahead IS the first occurrence. */
    form.recur_start_text = "2026-01-05";
    form.unit             = TASK_RECUR_WEEK;
    check(task_edit_reseed(&form, jan1 + noon) == jan5 + 9 * 3600,
          "reseed: a future start is the first occurrence");

    /* No start: the due date is the anchor. */
    form.recur_start_text = "";
    form.due_text         = "2026-01-05";
    check(task_edit_reseed(&form, jan1 + noon) == jan5 + 9 * 3600,
          "reseed: with no start the due date anchors the schedule");

    /* Neither: today at the schedule's time, or the next one. */
    form.due_text = "";
    form.unit     = TASK_RECUR_DAY;
    check(task_edit_reseed(&form, jan10 + noon) == jan11 + 9 * 3600,
          "reseed: with no anchor at all it starts from now");

    /* Hours with no start simply step from now. */
    form.unit  = TASK_RECUR_HOUR;
    form.every = 3;
    check(task_edit_reseed(&form, jan10 + noon) == jan10 + noon + 3 * 3600,
          "reseed: every 3 hours with no start is 3 hours from now");

    /* Hours WITH a start lock to its time of day: 9, 12, 15, ... */
    form.recur_start_text = "2026-01-01";
    check(task_edit_reseed(&form, jan10 + noon + 60) == jan10 + 15 * 3600,
          "reseed: every 3 hours from 09:00 keeps to 9, 12, 3");

    /* A half-typed schedule time reads as the 08:00 default. */
    form.unit            = TASK_RECUR_DAY;
    form.every           = 1;
    form.recur_time_text = "1";
    check(task_edit_reseed(&form, jan10 + noon) == jan11 + 8 * 3600,
          "reseed: an unreadable time anchors on the default 08:00");

    form.repeat = false;
    check(task_edit_reseed(&form, jan10 + noon) == 0, "reseed: repeat off has no next occurrence");

    /* The summary the editor shows is built from the same schedule. */
    form.repeat          = true;
    form.recur_time_text = "09:00";
    t = task_edit_schedule(&form, task_edit_reseed(&form, jan10 + noon));
    gchar *summary = task_recur_describe(&t, jan10 + noon);
    check(strstr(summary, "09:00") != NULL && strstr(summary, "Jan 11, 2026") != NULL,
          "schedule: the summary names the time and the next occurrence");
    g_free(summary);
}

/*
 * main — run the editor-field tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    test_dates();
    test_times();
    test_counts();
    test_lead_units();
    test_lead_follow();
    test_apply();
    test_schedule();
    return check_result();
}
