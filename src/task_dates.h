/*
 * task_dates — the date helpers shared by the windows and the recurrence
 * pass: local-day arithmetic, the due-date formats and parsers.
 *
 * A due date is stored as TWO values (db.h): `due`, a unix timestamp that
 * is always a LOCAL MIDNIGHT (0 = no date), and `due_time`, minutes past
 * that midnight (SHARED_DB_TASK_DUE_TIME_DEFAULT = 17:00).  Bucket by day with `due`;
 * sort and compare moments with task_due_instant.
 *
 * USE task_local_tz / task_local_dt RATHER THAN GLib's "_local"
 * CONSTRUCTORS.  Every g_date_time_new_now_local() / _new_from_unix_local()
 * / _new_local() resolves the local timezone from scratch, and that
 * resolution is the whole cost: 7318 ns against 175 ns to build a GDateTime
 * once a GTimeZone is in hand (measured, GLib 2.88.2).  These helpers run
 * per row.
 *
 * MAIN THREAD ONLY: the timezone and "today" are cached in statics.
 *
 * Carried from the GTK build's app.c; GDateTime stays for this phase.  Only
 * the core (recur.c) and the tests should need the two GDateTime-returning
 * functions — UI code works in timestamps and formatted strings.
 */

#ifndef TASK_DATES_H
#define TASK_DATES_H

#include <glib.h>

/* How a due date stands against today; what the Due column's tint shows. */
typedef enum {
    task_due_none = 0,              /* no due date                        */
    task_due_overdue,               /* before today                       */
    task_due_today,
    task_due_ahead                  /* after today                        */
} task_due_urgency_t;

/*
 * task_local_tz — the local timezone, cached and re-resolved when the local
 * day rolls over.  DST is unaffected (a GTimeZone carries the zone's whole
 * transition table); what the cache defers is a change of the SYSTEM's
 * zone, which is noticed at the next local midnight.
 *
 * Output:
 *   borrowed GTimeZone; do NOT unref it.
 */
GTimeZone *task_local_tz(void);

/*
 * task_local_dt — a unix timestamp as a local-time GDateTime.
 *
 * Inputs:
 *   unix_ts — seconds since the epoch
 *
 * Output:
 *   new GDateTime (g_date_time_unref), or NULL when out of range.
 */
GDateTime *task_local_dt(gint64 unix_ts);

/*
 * task_day_bounds — the local-midnight bounds of "today + offset_days".
 *
 * Inputs:
 *   offset_days — 0 for today, 1 for tomorrow, -1 for yesterday
 *   lo          — receives that day's local midnight
 *   hi          — receives the next day's local midnight
 */
void task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi);

/*
 * task_forecast_span — how many calendar days, today first, a forecast of
 * `days` days covers.  Counting business days, only Monday to Friday
 * count, but the weekend days between them stay inside the span: on a
 * Friday three business days are Friday, Monday and Tuesday, a span of 5.
 *
 * Inputs:
 *   weekday       — today, 1 = Monday ... 7 = Sunday (GLib's numbering)
 *   days          — the days to count, at least 1
 *   business_days — TRUE to count Monday to Friday only
 *
 * Output:
 *   the span in days, >= days; days itself when not counting business
 *   days (or when days < 1, 1).
 */
gint task_forecast_span(gint weekday, gint days, gboolean business_days);

/*
 * task_forecast_end — the local midnight that ends a forecast of `days`
 * days from today (task_forecast_span on today's weekday).
 *
 * Inputs:
 *   days          — the days to count, at least 1
 *   business_days — TRUE to count Monday to Friday only
 *
 * Output:
 *   the exclusive upper bound, unix seconds.
 */
gint64 task_forecast_end(gint days, gboolean business_days);

/*
 * task_due_format — a date for display, e.g. "Jul 13, 2026".
 *
 * Inputs:
 *   due — unix timestamp; 0 means no date
 *
 * Output:
 *   new string (g_free); "" for no date or an unrepresentable one.
 */
gchar *task_due_format(gint64 due);

/*
 * task_clock_format — "8:00 AM" for minutes past local midnight.
 *
 * Inputs:
 *   minutes — 0..1439; out-of-range values clamp into the day
 *
 * Output:
 *   new string (g_free); "" if the clock cannot be built.
 */
gchar *task_clock_format(gint minutes);

/*
 * task_day_at_minute — the same LOCAL DAY as `ts`, at `minutes` past
 * midnight.  Computed on the calendar, not by adding seconds: on the day
 * the clocks change, "9 AM" is not midnight plus nine hours.
 *
 * Inputs:
 *   ts      — a unix time; its local calendar date is what is kept
 *   minutes — minutes past local midnight, clamped to 00:00–23:59
 *
 * Output:
 *   the unix time, or `ts` unchanged if GDateTime cannot represent it.
 *   Seconds are zeroed — a time of day is a minute, not an instant.
 */
gint64 task_day_at_minute(gint64 ts, gint minutes);

/*
 * task_due_instant — the MOMENT a task is due: its date plus its time of
 * day.  Use it for sorting and for "when is this actually due"; use `due`
 * alone for anything that buckets by calendar day.
 *
 * Inputs:
 *   due      — local-midnight timestamp, 0 for none
 *   due_time — minutes past midnight; out of range reads as the default
 *
 * Output:
 *   unix timestamp, or 0 when due is 0 (no date means no moment).
 */
gint64 task_due_instant(gint64 due, gint due_time);

/*
 * task_due_format_at — "Jul 13, 2026", plus " 2:30 PM" when due_time is not
 * the 17:00 default.  The default is left unsaid on purpose: every task has
 * a due time, so printing it always would put a clock on every row and
 * distinguish nothing.
 *
 * Inputs:
 *   due      — local-midnight timestamp, 0 for none
 *   due_time — minutes past midnight
 *
 * Output:
 *   new string (g_free); "" for no date.
 */
gchar *task_due_format_at(gint64 due, gint due_time);

/*
 * task_due_format_iso — the canonical "YYYY-MM-DD" spelling of a date (the
 * local calendar day), which is what the editor's date fields show.
 *
 * Inputs:
 *   due — unix timestamp; 0 means no date
 *
 * Output:
 *   new string (g_free); "" for no date.
 */
gchar *task_due_format_iso(gint64 due);

/*
 * task_due_urgency — where a due moment stands against today.  Cheap (one
 * time() and two comparisons on the common path), so it may be called per
 * row per draw.
 *
 * Inputs:
 *   due — the due moment (task_due_instant), 0 for none
 *
 * Output:
 *   task_due_none for 0, else overdue / today / ahead.
 */
task_due_urgency_t task_due_urgency(gint64 due);

/*
 * task_due_parse — parse "YYYY-MM-DD" (also "M/D/YY" and "M/D/YYYY") into a
 * local-midnight timestamp.
 *
 * Inputs:
 *   text — the typed text; NULL is allowed; surrounding spaces are ignored
 *
 * Output:
 *   the timestamp, or 0 when the text is empty or not a valid date.
 */
gint64 task_due_parse(const gchar *text);

/*
 * task_due_from_ymd — a calendar day as a local-midnight timestamp.
 *
 * Inputs:
 *   y — year, 1970..9999
 *   m — month, 1..12
 *   d — day, 1..31 (and valid for that month)
 *
 * Output:
 *   the timestamp, or 0 when the fields are out of range.
 */
gint64 task_due_from_ymd(gint y, gint m, gint d);

#endif /* TASK_DATES_H */
