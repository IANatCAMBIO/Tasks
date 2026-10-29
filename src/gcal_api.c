/* ===========================================================================
 * gcal_api.c — the Google Calendar v3 REST calls (gcal_api.h)
 * =========================================================================== */

#include "gcal_api.h"
#include "http.h"
#include "task_dates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The API root.  A build may point it elsewhere (-DGCAL_API_BASE=...), which
 * is how the sync is exercised against a local stand-in for Google.       */
#ifndef GCAL_API_BASE
#define GCAL_API_BASE "https://www.googleapis.com/calendar/v3"
#endif
#define API_BASE GCAL_API_BASE

/* The event fields a pass reads: everything else stays on Google's side. */
#define EVENT_FIELDS "id,status,summary,start,end," \
                     "attendees(self,responseStatus,displayName,email,resource)," \
                     "extendedProperties/private"

/*
 * esc — URI-escape a path segment or a query value.
 *
 * Inputs:
 *   s — the string, not NULL
 *
 * Output:
 *   the escaped string (g_free).
 */
static gchar *
esc(const gchar *s)
{
    return g_uri_escape_string(s, NULL, FALSE);
}

/*
 * rfc3339 — a unix time as an RFC 3339 UTC timestamp.
 *
 * Inputs:
 *   t   — unix seconds
 *   buf — receives "YYYY-MM-DDTHH:MM:SSZ"
 *   len — its size, at least 21
 */
static void
rfc3339(gint64 t, gchar *buf, gsize len)
{
    time_t    tt = (time_t)t;
    struct tm utc;
    gmtime_r(&tt, &utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

/*
 * read_time — the instant an event's start or end object names: its
 * "dateTime", or for an all-day event the local midnight of its "date".
 *
 * Inputs:
 *   obj     — the start or end object
 *   all_day — out: TRUE when it was a date
 *
 * Output:
 *   unix seconds, or 0 when unreadable.
 */
static gint64
read_time(task_json_t *obj, gboolean *all_day)
{
    const gchar *dt = task_json_str(obj, "dateTime");
    if (dt != NULL) {
        *all_day = FALSE;
        GDateTime *t = g_date_time_new_from_iso8601(dt, NULL);
        if (t == NULL) return 0;
        gint64 u = g_date_time_to_unix(t);
        g_date_time_unref(t);
        return u;
    }
    const gchar *date = task_json_str(obj, "date");
    gint y, m, d;
    if (date == NULL || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    *all_day = TRUE;
    return task_due_from_ymd(y, m, d);
}

/*
 * gcal_remote_free — free an event.  NULL-safe.
 * Documented in gcal_api.h.
 */
void
gcal_remote_free(gcal_remote_t *r)
{
    if (r == NULL) return;
    g_free(r->event_id);
    g_free(r->title);
    g_free(r->attendees);
    g_free(r);
}

/*
 * gcal_api_parse_event — read one event resource.
 * Documented in gcal_api.h.
 */
gcal_remote_t *
gcal_api_parse_event(task_json_t *item)
{
    const gchar *id = task_json_str(item, "id");
    if (id == NULL) return NULL;

    gcal_remote_t *r = g_new0(gcal_remote_t, 1);
    r->event_id  = g_strdup(id);
    r->title     = g_strdup(task_json_str(item, "summary") != NULL
                            ? task_json_str(item, "summary") : "");
    r->cancelled = g_strcmp0(task_json_str(item, "status"), "cancelled") == 0;

    const gchar *tid = task_json_str(task_json_get(task_json_get(item, "extendedProperties"),
                                                   "private"), GCAL_TASK_PROPERTY);
    r->task_id = tid != NULL ? g_ascii_strtoll(tid, NULL, 10) : 0;

    /* The attendees, as shared_db.h's gcal_event.attendees lists them: a
     * room or other resource is not one, and a line break inside a name
     * would split it in two. */
    task_json_t *attendees = task_json_get(item, "attendees");
    GString     *names     = g_string_new("");
    for (guint i = 0; i < task_json_len(attendees); i++) {
        task_json_t *a = task_json_at(attendees, i);
        if (task_json_bool(a, "self", FALSE) &&
            g_strcmp0(task_json_str(a, "responseStatus"), "declined") == 0)
            r->declined = TRUE;
        const gchar *name = task_json_str(a, "displayName");
        if (name == NULL || *name == '\0') name = task_json_str(a, "email");
        if (task_json_bool(a, "resource", FALSE) || name == NULL || *name == '\0') continue;
        if (names->len > 0) g_string_append(names, SHARED_DB_GCAL_ATTENDEE_SEP);
        gsize at = names->len;
        g_string_append(names, name);
        g_strdelimit(names->str + at, "\r\n", ' ');
    }
    r->attendees = g_string_free(names, FALSE);

    /* A cancelled event's record may carry no times at all; it is still
     * an answer ("deleted"), so only a live event must have them. */
    gboolean start_day = FALSE, end_day = FALSE;
    r->start   = read_time(task_json_get(item, "start"), &start_day);
    r->end     = read_time(task_json_get(item, "end"), &end_day);
    r->all_day = start_day;
    if (!r->cancelled && (r->start == 0 || r->end < r->start)) {
        gcal_remote_free(r);
        return NULL;
    }
    return r;
}

/*
 * gcal_api_event_body — the JSON body for a task's session.
 * Documented in gcal_api.h.
 */
gchar *
gcal_api_event_body(gint64 task_id, gint64 start, gint duration, const gchar *title,
                    gboolean full)
{
    gchar from[32], to[32];
    rfc3339(start, from, sizeof from);
    rfc3339(start + (gint64)duration * 60, to, sizeof to);

    GString *b = g_string_new("{\"summary\":");
    task_json_escape(b, title != NULL && *title != '\0' ? title : "Untitled");
    g_string_append_printf(b, ",\"start\":{\"dateTime\":\"%s\"},\"end\":{\"dateTime\":\"%s\"}",
                           from, to);
    if (full) {
        g_string_append_printf(b, ",\"reminders\":{\"useDefault\":false},"
                                  "\"extendedProperties\":{\"private\":"
                                  "{\"" GCAL_TASK_PROPERTY "\":\"%lld\"}}",
                               (long long)task_id);
    }
    g_string_append_c(b, '}');
    return g_string_free(b, FALSE);
}

/*
 * call — one API request: send it, classify the status, parse the body.
 *
 * Inputs:
 *   method — "GET", "POST", "PATCH" or "DELETE"
 *   url    — the full URL
 *   token  — the access token
 *   body   — a JSON body, or NULL
 *   out    — out: the parsed reply on GCAL_OK (task_json_free); may be NULL
 *            when the caller wants none (a DELETE's reply is empty)
 *   err    — out: the reason on GCAL_FAILED (g_free)
 *
 * Output:
 *   the outcome.
 */
static gcal_result_t
call(const gchar *method, const gchar *url, const gchar *token, const gchar *body,
     task_json_t **out, gchar **err)
{
    glong  status = 0;
    gchar *terr   = NULL;
    gchar *reply  = task_http_request(method, url, token,
                                      body != NULL ? "application/json" : NULL,
                                      NULL, body, &status, &terr);
    *err = NULL;
    if (out != NULL) *out = NULL;
    if (reply == NULL) {
        *err = terr != NULL ? terr : g_strdup("network failure");
        return GCAL_FAILED;
    }
    g_free(terr);

    gcal_result_t result;
    if (status >= 200 && status < 300) {
        result = GCAL_OK;
        if (out != NULL) {
            *out = task_json_parse(reply, -1);
            if (*out == NULL) {
                *err   = g_strdup("Google sent a reply that is not JSON");
                result = GCAL_FAILED;
            }
        }
    } else if (status == 404 || status == 410) {
        result = GCAL_GONE;
    } else {
        task_json_t *root = task_json_parse(reply, -1);
        const gchar *msg  = task_json_str(task_json_get(root, "error"), "message");
        *err   = g_strdup_printf("HTTP %ld: %s", status, msg != NULL ? msg : "request failed");
        task_json_free(root);
        result = GCAL_FAILED;
    }
    g_free(reply);
    return result;
}

/*
 * gcal_api_calendars — the account's calendar list, every page.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_calendars(const gchar *token, GPtrArray **out, gchar **err)
{
    GPtrArray *cals   = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_calendar_free);
    gchar     *page   = NULL;
    gchar     *fields = esc("items(id,summary,summaryOverride,backgroundColor,accessRole,"
                            "primary),nextPageToken");
    gcal_result_t result;

    do {
        gchar *page_esc = page != NULL ? esc(page) : NULL;
        gchar *url = g_strdup_printf(API_BASE "/users/me/calendarList?maxResults=250"
                                     "&fields=%s%s%s", fields,
                                     page_esc != NULL ? "&pageToken=" : "",
                                     page_esc != NULL ? page_esc : "");
        task_json_t *root = NULL;
        result = call("GET", url, token, NULL, &root, err);
        g_free(url);
        g_free(page_esc);
        g_clear_pointer(&page, g_free);
        if (result != GCAL_OK) break;

        task_json_t *items = task_json_get(root, "items");
        for (guint i = 0; i < task_json_len(items); i++) {
            task_json_t *it = task_json_at(items, i);
            const gchar *id = task_json_str(it, "id");
            if (id == NULL) continue;
            const gchar *name = task_json_str(it, "summaryOverride");
            if (name == NULL) name = task_json_str(it, "summary");
            const gchar *role = task_json_str(it, "accessRole");

            gcal_calendar_t *c = g_new0(gcal_calendar_t, 1);
            c->id         = g_strdup(id);
            c->summary    = g_strdup(name != NULL ? name : id);
            c->color      = g_strdup(task_json_str(it, "backgroundColor") != NULL
                                     ? task_json_str(it, "backgroundColor") : "");
            c->writable   = g_strcmp0(role, "owner") == 0 || g_strcmp0(role, "writer") == 0;
            c->is_primary = task_json_bool(it, "primary", FALSE);
            g_ptr_array_add(cals, c);
        }
        page = g_strdup(task_json_str(root, "nextPageToken"));
        task_json_free(root);
    } while (page != NULL);

    g_free(fields);
    if (result != GCAL_OK) {
        g_ptr_array_free(cals, TRUE);
        if (result == GCAL_GONE) {
            *err   = g_strdup("the calendar list is not available");
            result = GCAL_FAILED;
        }
        return result;
    }
    *out = cals;
    return GCAL_OK;
}

/*
 * gcal_api_events — a calendar's events in a window, every page.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_events(const gchar *token, const gchar *calendar_id, gint64 lo, gint64 hi,
                GPtrArray **out, gchar **err)
{
    gchar from[32], to[32];
    rfc3339(lo, from, sizeof from);
    rfc3339(hi, to, sizeof to);

    GPtrArray *events = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_remote_free);
    gchar     *cal    = esc(calendar_id);
    gchar     *fields = esc("items(" EVENT_FIELDS "),nextPageToken");
    gchar     *page   = NULL;
    gcal_result_t result;

    do {
        gchar *page_esc = page != NULL ? esc(page) : NULL;
        gchar *url = g_strdup_printf(API_BASE "/calendars/%s/events?singleEvents=true"
                                     "&maxResults=250&timeMin=%s&timeMax=%s&fields=%s%s%s",
                                     cal, from, to, fields,
                                     page_esc != NULL ? "&pageToken=" : "",
                                     page_esc != NULL ? page_esc : "");
        task_json_t *root = NULL;
        result = call("GET", url, token, NULL, &root, err);
        g_free(url);
        g_free(page_esc);
        g_clear_pointer(&page, g_free);
        if (result != GCAL_OK) break;

        task_json_t *items = task_json_get(root, "items");
        for (guint i = 0; i < task_json_len(items); i++) {
            gcal_remote_t *r = gcal_api_parse_event(task_json_at(items, i));
            if (r != NULL) g_ptr_array_add(events, r);
        }
        page = g_strdup(task_json_str(root, "nextPageToken"));
        task_json_free(root);
    } while (page != NULL);

    g_free(cal);
    g_free(fields);
    if (result != GCAL_OK) {
        g_ptr_array_free(events, TRUE);
        return result;
    }
    *out = events;
    return GCAL_OK;
}

/*
 * event_url — the URL of one event, or of a calendar's event collection.
 *
 * Inputs:
 *   calendar_id — the calendar
 *   event_id    — the event, or NULL for the collection
 *
 * Output:
 *   the URL (g_free).
 */
static gchar *
event_url(const gchar *calendar_id, const gchar *event_id)
{
    gchar *cal = esc(calendar_id);
    gchar *ev  = event_id != NULL ? esc(event_id) : NULL;
    gchar *url = ev != NULL ? g_strdup_printf(API_BASE "/calendars/%s/events/%s", cal, ev)
                            : g_strdup_printf(API_BASE "/calendars/%s/events", cal);
    g_free(cal);
    g_free(ev);
    return url;
}

/*
 * gcal_api_get_event — one event, by id.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_get_event(const gchar *token, const gchar *calendar_id, const gchar *event_id,
                   gcal_remote_t **out, gchar **err)
{
    gchar *base   = event_url(calendar_id, event_id);
    gchar *fields = esc(EVENT_FIELDS);
    gchar *url    = g_strdup_printf("%s?fields=%s", base, fields);
    task_json_t  *root   = NULL;
    gcal_result_t result = call("GET", url, token, NULL, &root, err);
    g_free(base);
    g_free(fields);
    g_free(url);
    if (result != GCAL_OK) return result;

    gcal_remote_t *r = gcal_api_parse_event(root);
    task_json_free(root);
    if (r == NULL) {
        *err = g_strdup("Google sent an event without a usable time");
        return GCAL_FAILED;
    }
    if (r->cancelled) {
        gcal_remote_free(r);
        return GCAL_GONE;
    }
    *out = r;
    return GCAL_OK;
}

/*
 * gcal_api_insert — create the event mirroring a task.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_insert(const gchar *token, const gchar *calendar_id, gint64 task_id, gint64 start,
                gint duration, const gchar *title, gchar **event_id, gchar **err)
{
    gchar        *url    = event_url(calendar_id, NULL);
    gchar        *body   = gcal_api_event_body(task_id, start, duration, title, TRUE);
    task_json_t  *root   = NULL;
    gcal_result_t result = call("POST", url, token, body, &root, err);
    g_free(url);
    g_free(body);
    if (result == GCAL_GONE) {
        *err   = g_strdup("the calendar to add sessions to does not exist");
        result = GCAL_FAILED;
    }
    if (result != GCAL_OK) return result;

    const gchar *id = task_json_str(root, "id");
    if (id == NULL) {
        *err   = g_strdup("Google created an event but sent no id");
        result = GCAL_FAILED;
    } else {
        *event_id = g_strdup(id);
    }
    task_json_free(root);
    return result;
}

/*
 * gcal_api_patch — write a task's session to its event.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_patch(const gchar *token, const gchar *calendar_id, const gchar *event_id,
               gint64 task_id, gint64 start, gint duration, const gchar *title, gchar **err)
{
    gchar        *url    = event_url(calendar_id, event_id);
    gchar        *body   = gcal_api_event_body(task_id, start, duration, title, FALSE);
    task_json_t  *root   = NULL;
    gcal_result_t result = call("PATCH", url, token, body, &root, err);
    g_free(url);
    g_free(body);
    task_json_free(root);
    return result;
}

/*
 * gcal_api_delete — delete an event.
 * Documented in gcal_api.h.
 */
gcal_result_t
gcal_api_delete(const gchar *token, const gchar *calendar_id, const gchar *event_id,
                gchar **err)
{
    gchar        *url    = event_url(calendar_id, event_id);
    gcal_result_t result = call("DELETE", url, token, NULL, NULL, err);
    g_free(url);
    return result;
}
