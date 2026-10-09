#include "task_dates.h"
#include "db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The cached zone and today's window.  The zone is cached because resolving
 * it is 40x the cost of everything else here (see task_dates.h); there is
 * no g_date_time_new_from_unix(tz, t), which is why task_local_dt goes
 * through _from_unix_utc + g_date_time_to_timezone. */
static GTimeZone *local_tz;         /* owned                              */
static gint64     local_lo;         /* [lo, hi) is today, unix seconds    */
static gint64     local_hi;

/*
 * local_cache_ensure — resolve the zone and today's bounds if the cache is
 * empty or the day has rolled over.  One time() and two comparisons on the
 * common path.
 */
static void
local_cache_ensure(void)
{
    gint64 now = (gint64)time(NULL);
    if (local_tz != NULL && now >= local_lo && now < local_hi) return;

    g_clear_pointer(&local_tz, g_time_zone_unref);
    local_tz = g_time_zone_new_local();

    GDateTime *n   = g_date_time_new_now(local_tz);
    GDateTime *mid = n != NULL
        ? g_date_time_new(local_tz, g_date_time_get_year(n),
                          g_date_time_get_month(n),
                          g_date_time_get_day_of_month(n), 0, 0, 0)
        : NULL;
    GDateTime *nxt = mid != NULL ? g_date_time_add_days(mid, 1) : NULL;
    if (nxt != NULL) {
        local_lo = g_date_time_to_unix(mid);
        local_hi = g_date_time_to_unix(nxt);
    } else {
        /* Cannot happen from a real GDateTime, but [now, now) would
         * re-resolve the zone on every call.  A minute's grace. */
        local_lo = now;
        local_hi = now + 60;
    }
    g_clear_pointer(&n, g_date_time_unref);
    g_clear_pointer(&mid, g_date_time_unref);
    g_clear_pointer(&nxt, g_date_time_unref);
}

/*
 * task_local_tz — the cached local timezone.
 * Output is documented in task_dates.h.
 */
GTimeZone *
task_local_tz(void)
{
    local_cache_ensure();
    return local_tz;
}

/*
 * task_local_dt — a timestamp as a local GDateTime.
 * Inputs/output are documented in task_dates.h.
 */
GDateTime *
task_local_dt(gint64 unix_ts)
{
    GDateTime *utc = g_date_time_new_from_unix_utc(unix_ts);
    if (utc == NULL) return NULL;

    GDateTime *local = g_date_time_to_timezone(utc, task_local_tz());
    g_date_time_unref(utc);
    return local;
}

/*
 * task_day_bounds — local midnight bounds of today + offset_days.
 * Inputs are documented in task_dates.h.
 */
void
task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi)
{
    GTimeZone *tz  = task_local_tz();
    GDateTime *now = g_date_time_new_now(tz);
    GDateTime *day = g_date_time_add_days(now, offset_days);
    GDateTime *mid = g_date_time_new(tz, g_date_time_get_year(day),
                                     g_date_time_get_month(day),
                                     g_date_time_get_day_of_month(day),
                                     0, 0, 0);
    GDateTime *nxt = g_date_time_add_days(mid, 1);

    *lo = g_date_time_to_unix(mid);
    *hi = g_date_time_to_unix(nxt);
    g_date_time_unref(now);
    g_date_time_unref(day);
    g_date_time_unref(mid);
    g_date_time_unref(nxt);
}

/*
 * format_local — a timestamp through one g_date_time_format pattern.
 *
 * Inputs:
 *   ts      — unix timestamp; 0 means no date
 *   pattern — g_date_time_format pattern
 *
 * Output:
 *   new string (g_free); "" for 0 or an unrepresentable time.
 */
static gchar *
format_local(gint64 ts, const gchar *pattern)
{
    GDateTime *dt = ts != 0 ? task_local_dt(ts) : NULL;
    if (dt == NULL) return g_strdup("");

    gchar *s = g_date_time_format(dt, pattern);
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/*
 * task_due_format — "Jul 13, 2026".
 * Inputs/output are documented in task_dates.h.
 */
gchar *
task_due_format(gint64 due)
{
    return format_local(due, "%b %-e, %Y");
}

/*
 * task_due_format_iso — "2026-07-13".
 * Inputs/output are documented in task_dates.h.
 */
gchar *
task_due_format_iso(gint64 due)
{
    return format_local(due, "%Y-%m-%d");
}

/*
 * task_clock_format — "8:00 AM" for minutes past midnight.
 *
 * "%I:%M %p" with the leading zero dropped BY HAND: GLib's "%l" pads with
 * U+2007 FIGURE SPACE, not an ASCII space, so stripping would leave it and
 * the sentence would read "at  8:00 AM".
 * Inputs/output are documented in task_dates.h.
 */
gchar *
task_clock_format(gint minutes)
{
    minutes = CLAMP(minutes, 0, 23 * 60 + 59);

    GDateTime *dt = g_date_time_new(task_local_tz(), 2000, 1, 1,
                                    minutes / 60, minutes % 60, 0.0);
    if (dt == NULL) return g_strdup("");

    gchar *clock = g_date_time_format(dt, "%I:%M %p");
    g_date_time_unref(dt);
    if (clock == NULL) return g_strdup("");

    g_strchomp(clock);
    if (clock[0] == '0') memmove(clock, clock + 1, strlen(clock));
    return clock;
}

/*
 * task_day_at_minute — the calendar day of a time, at a minute of the day.
 * Inputs/output are documented in task_dates.h.
 */
gint64
task_day_at_minute(gint64 ts, gint minutes)
{
    minutes = CLAMP(minutes, 0, 23 * 60 + 59);

    GDateTime *dt = task_local_dt(ts);
    if (dt == NULL) return ts;

    GDateTime *out = g_date_time_new(task_local_tz(),
                                     g_date_time_get_year(dt),
                                     g_date_time_get_month(dt),
                                     g_date_time_get_day_of_month(dt),
                                     minutes / 60, minutes % 60, 0.0);
    g_date_time_unref(dt);
    if (out == NULL) return ts;

    gint64 v = g_date_time_to_unix(out);
    g_date_time_unref(out);
    return v;
}

/*
 * task_due_instant — date plus time of day.
 * Inputs/output are documented in task_dates.h.
 */
gint64
task_due_instant(gint64 due, gint due_time)
{
    if (due == 0) return 0;
    if (due_time < 0 || due_time > 23 * 60 + 59) {
        due_time = TASK_DUE_TIME_DEFAULT;
    }
    return task_day_at_minute(due, due_time);
}

/*
 * task_due_format_at — the date, plus the time when it is not the default.
 * Inputs/output are documented in task_dates.h.
 */
gchar *
task_due_format_at(gint64 due, gint due_time)
{
    gchar *date = task_due_format(due);
    if (due == 0 || due_time == TASK_DUE_TIME_DEFAULT) return date;

    gchar *clock = task_clock_format(due_time);
    gchar *out   = g_strdup_printf("%s %s", date, clock);
    g_free(clock);
    g_free(date);
    return out;
}

/*
 * task_due_urgency — overdue / today / ahead, from the cached day window.
 * Inputs/output are documented in task_dates.h.
 */
task_due_urgency_t
task_due_urgency(gint64 due)
{
    if (due == 0) return task_due_none;

    local_cache_ensure();
    if (due < local_lo) return task_due_overdue;
    return due < local_hi ? task_due_today : task_due_ahead;
}

/*
 * task_due_from_ymd — validated year/month/day to local midnight.
 * Inputs/output are documented in task_dates.h.
 */
gint64
task_due_from_ymd(gint y, gint m, gint d)
{
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 9999) return 0;

    GDateTime *dt = g_date_time_new(task_local_tz(), y, m, d, 0, 0, 0);
    if (dt == NULL) return 0;

    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/*
 * task_due_parse — "YYYY-MM-DD" or "M/D/YY[YY]" to local midnight.
 * Inputs/output are documented in task_dates.h.
 */
gint64
task_due_parse(const gchar *text)
{
    if (text == NULL) return 0;

    gchar   *t  = g_strstrip(g_strdup(text));
    gint     y  = 0, m = 0, d = 0;
    gboolean ok = FALSE;

    if (sscanf(t, "%d-%d-%d", &y, &m, &d) == 3) {
        ok = TRUE;
    } else if (sscanf(t, "%d/%d/%d", &m, &d, &y) == 3) {
        if (y < 100) y += 2000;
        ok = TRUE;
    }
    g_free(t);
    return ok ? task_due_from_ymd(y, m, d) : 0;
}
