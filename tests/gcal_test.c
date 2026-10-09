/*
 * gcal_test.c — the Google Calendar sync's core, without the network: the
 * per-task decision (gcal_plan.h), reading Google's event resources and
 * writing request bodies (gcal_api.h), and the sync's own tables
 * (gcal_store.h) — the dirty check, the compare-and-set reschedule, the
 * event cache and the account record — and the sign-in's loopback
 * listener (oauth.h), driven like a browser but stopped short of the
 * token exchange, the one step that needs Google.
 */

#include "fixture.h"
#include "gcal_api.h"
#include "gcal_plan.h"
#include "gcal_store.h"
#include "oauth.h"
#include "task_dates.h"

#include <arpa/inet.h>
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * plan_checks — every rule of gcal_plan, in the order gcal_plan.h lists
 * them.
 */
static void
plan_checks(void)
{
    gcal_slot_t base    = { 1000, 60, "Write report" };
    gcal_slot_t moved   = { 4600, 60, "Write report" };
    gcal_slot_t renamed = { 1000, 60, "Write the report" };
    gcal_slot_t longer  = { 1000, 90, "Write report" };
    gcal_slot_t ev_same = { 1000, 60, NULL };
    gcal_slot_t ev_move = { 8200, 30, NULL };

    check(gcal_plan(NULL, &base, GCAL_EVENT_UNKNOWN, NULL, FALSE) == GCAL_ACT_CREATE,
          "plan: an unbound scheduled task is pushed");
    check(gcal_plan(NULL, NULL, GCAL_EVENT_UNKNOWN, NULL, FALSE) == GCAL_ACT_NONE,
          "plan: nothing bound, nothing scheduled, nothing to do");
    check(gcal_plan(&base, NULL, GCAL_EVENT_PRESENT, &ev_same, FALSE) == GCAL_ACT_REMOVE,
          "plan: a task off the planner deletes its event");
    check(gcal_plan(&base, NULL, GCAL_EVENT_UNKNOWN, NULL, FALSE) == GCAL_ACT_REMOVE,
          "plan: ... even when the event was not looked at");
    check(gcal_plan(&base, NULL, GCAL_EVENT_MISSING, NULL, FALSE) == GCAL_ACT_FORGET,
          "plan: both gone: the binding is dropped");
    check(gcal_plan(&base, &base, GCAL_EVENT_PRESENT, &ev_same, TRUE) == GCAL_ACT_MOVE,
          "plan: an event in the wrong calendar moves");
    check(gcal_plan(&base, &base, GCAL_EVENT_MISSING, NULL, FALSE) == GCAL_ACT_UNSCHEDULE,
          "plan: deleted in Google, unchanged here: the task leaves the planner");
    check(gcal_plan(&base, &moved, GCAL_EVENT_MISSING, NULL, FALSE) == GCAL_ACT_CREATE,
          "plan: deleted in Google but moved here: pushed again");
    check(gcal_plan(&base, &moved, GCAL_EVENT_PRESENT, &ev_same, FALSE) == GCAL_ACT_UPDATE,
          "plan: a moved task updates its event");
    check(gcal_plan(&base, &renamed, GCAL_EVENT_UNKNOWN, NULL, FALSE) == GCAL_ACT_UPDATE,
          "plan: a renamed task updates its event");
    check(gcal_plan(&base, &longer, GCAL_EVENT_PRESENT, &ev_move, FALSE) == GCAL_ACT_UPDATE,
          "plan: both changed: the local edit wins");
    check(gcal_plan(&base, &base, GCAL_EVENT_PRESENT, &ev_move, FALSE) == GCAL_ACT_ADOPT,
          "plan: an event moved in Google moves the session");
    check(gcal_plan(&base, &base, GCAL_EVENT_PRESENT, &ev_same, FALSE) == GCAL_ACT_NONE,
          "plan: in step: nothing to do");
    check(gcal_plan(&base, &base, GCAL_EVENT_UNKNOWN, NULL, FALSE) == GCAL_ACT_NONE,
          "plan: an event not looked at changes nothing");

    gcal_slot_t untitled_task   = { 1000, 60, NULL };
    gcal_slot_t untitled_pushed = { 1000, 60, "" };
    check(gcal_plan(&untitled_pushed, &untitled_task, GCAL_EVENT_UNKNOWN, NULL, FALSE)
          == GCAL_ACT_NONE, "plan: a NULL title and an empty one are the same");
}

/*
 * parse — gcal_api_parse_event over a JSON text.
 *
 * Inputs:
 *   json — one event resource
 *
 * Output:
 *   the event, or NULL.
 */
static gcal_remote_t *
parse(const gchar *json)
{
    task_json_t   *root = task_json_parse(json, -1);
    gcal_remote_t *r    = gcal_api_parse_event(root);
    task_json_free(root);
    return r;
}

/*
 * api_checks — reading event resources and writing request bodies.
 */
static void
api_checks(void)
{
    gcal_remote_t *r = parse(
        "{\"id\":\"ev1\",\"status\":\"confirmed\",\"summary\":\"Stand-up \\u2615\","
        " \"start\":{\"dateTime\":\"2026-10-09T09:00:00-04:00\"},"
        " \"end\":{\"dateTime\":\"2026-10-09T09:15:00-04:00\"}}");
    check(r != NULL && strcmp(r->event_id, "ev1") == 0, "parse: a timed event");
    check(r != NULL && r->start == 1791550800 && r->end - r->start == 15 * 60,
          "parse: dateTime with an offset is read as the instant it names");
    check(r != NULL && strcmp(r->title, "Stand-up \xe2\x98\x95") == 0 && !r->all_day &&
          r->task_id == 0 && !r->cancelled && !r->declined, "parse: title, and not ours");
    gcal_remote_free(r);

    r = parse("{\"id\":\"ev2\",\"start\":{\"date\":\"2026-10-12\"},"
              " \"end\":{\"date\":\"2026-10-13\"}}");
    check(r != NULL && r->all_day && r->start == task_due_from_ymd(2026, 10, 12) &&
          r->end == task_due_from_ymd(2026, 10, 13), "parse: an all-day event spans local days");
    check(r != NULL && strcmp(r->title, "") == 0, "parse: no summary reads as empty");
    gcal_remote_free(r);

    r = parse("{\"id\":\"ev3\",\"status\":\"cancelled\"}");
    check(r != NULL && r->cancelled, "parse: a cancelled event needs no times");
    gcal_remote_free(r);

    r = parse("{\"id\":\"ev4\",\"start\":{\"dateTime\":\"2026-10-09T13:00:00Z\"},"
              " \"end\":{\"dateTime\":\"2026-10-09T14:00:00Z\"},"
              " \"extendedProperties\":{\"private\":{\"" GCAL_TASK_PROPERTY "\":\"42\"}},"
              " \"attendees\":[{\"email\":\"a@b\",\"responseStatus\":\"declined\"},"
              "                {\"self\":true,\"responseStatus\":\"accepted\"}]}");
    check(r != NULL && r->task_id == 42, "parse: the task property marks the planner's own");
    check(r != NULL && !r->declined, "parse: someone else declining is not declining");
    gcal_remote_free(r);

    r = parse("{\"id\":\"ev5\",\"start\":{\"dateTime\":\"2026-10-09T13:00:00Z\"},"
              " \"end\":{\"dateTime\":\"2026-10-09T14:00:00Z\"},"
              " \"attendees\":[{\"self\":true,\"responseStatus\":\"declined\"}]}");
    check(r != NULL && r->declined, "parse: the account's own decline is seen");
    gcal_remote_free(r);

    check(parse("{\"start\":{\"dateTime\":\"2026-10-09T13:00:00Z\"}}") == NULL,
          "parse: no id, no event");
    check(parse("{\"id\":\"x\",\"start\":{\"dateTime\":\"nonsense\"},\"end\":{}}") == NULL,
          "parse: a live event without readable times is refused");

    gchar *body = gcal_api_event_body(7, 1791550800, 90, "Plan \"Q4\"", TRUE);
    check(strstr(body, "\"summary\":\"Plan \\\"Q4\\\"\"") != NULL, "body: the title is escaped");
    check(strstr(body, "\"start\":{\"dateTime\":\"2026-10-09T13:00:00Z\"}") != NULL &&
          strstr(body, "\"end\":{\"dateTime\":\"2026-10-09T14:30:00Z\"}") != NULL,
          "body: start and end in UTC");
    check(strstr(body, "\"" GCAL_TASK_PROPERTY "\":\"7\"") != NULL &&
          strstr(body, "\"useDefault\":false") != NULL,
          "body: an insert carries the task and turns default reminders off");
    task_json_t *round = task_json_parse(body, -1);
    check(round != NULL, "body: is valid JSON");
    task_json_free(round);
    g_free(body);

    body = gcal_api_event_body(7, 1791550800, 60, "", FALSE);
    check(strstr(body, "\"summary\":\"Untitled\"") != NULL, "body: no title is sent as Untitled");
    check(strstr(body, GCAL_TASK_PROPERTY) == NULL && strstr(body, "reminders") == NULL,
          "body: a patch writes the session only");
    g_free(body);
}

/*
 * store_checks — the sync's tables against a real database.
 *
 * Inputs:
 *   db — an empty database
 */
static void
store_checks(TaskDatabase *db)
{
    check(!gcal_store_dirty(db), "store: before init nothing is dirty and nothing fails");
    GPtrArray *none = gcal_store_bindings(db);
    check(none->len == 0, "store: before init there are no bindings");
    g_ptr_array_free(none, TRUE);

    check(gcal_store_init(db) && gcal_store_init(db), "store: init is repeatable");
    check(!gcal_store_dirty(db), "store: an empty planner is clean");

    gint64 list = task_db_list_create(db, "Work", NULL);
    gint64 a    = task_db_task_create(db, list, 0, "Write report");
    gint64 b    = task_db_task_create(db, list, 0, "Review");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    check(gcal_store_dirty(db), "dirty: a scheduled, unbound task");

    gcal_binding_t bind = { a, "primary@x", "ev-a", 1791550800, 60, "Write report" };
    gcal_store_bind(db, &bind);
    check(!gcal_store_dirty(db), "dirty: bound and in step is clean");

    task_db_task_set_scheduled(db, a, 1791554400, 60);
    check(gcal_store_dirty(db), "dirty: a moved session");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    check(!gcal_store_dirty(db), "dirty: moved back is clean again");
    task_db_task_set_scheduled(db, a, 1791550800, 45);
    check(gcal_store_dirty(db), "dirty: a stretched session");
    task_db_task_set_scheduled(db, a, 1791550800, 60);

    task_db_task_set_scheduled(db, a, 0, 0);
    check(gcal_store_dirty(db), "dirty: a session removed from the planner");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    task_db_task_delete(db, a);
    check(gcal_store_dirty(db), "dirty: a deleted task");
    task_db_task_purge(db, a);
    check(gcal_store_dirty(db), "dirty: a purged task (the binding outlives the row)");
    GPtrArray *bs = gcal_store_bindings(db);
    check(bs->len == 1 && ((gcal_binding_t *)g_ptr_array_index(bs, 0))->task_id == a &&
          strcmp(((gcal_binding_t *)g_ptr_array_index(bs, 0))->event_id, "ev-a") == 0,
          "bindings: read back whole");
    g_ptr_array_free(bs, TRUE);
    gcal_store_unbind(db, a);
    check(!gcal_store_dirty(db), "unbind: the purged task is forgotten");

    /* Compare-and-set: only from the session that was read. */
    task_db_task_set_scheduled(db, b, 1791550800, 60);
    check(!gcal_store_reschedule(db, b, 1791554400, 60, 1791558000, 30),
          "reschedule: a session changed meanwhile is not overwritten");
    check(gcal_store_reschedule(db, b, 1791550800, 60, 1791558000, 30),
          "reschedule: the expected session is replaced");
    Task *t = task_db_task_get(db, b);
    check(t != NULL && t->scheduled_start == 1791558000 && t->scheduled_duration == 30,
          "reschedule: the new session is stored");
    task_free(t);
    check(gcal_store_reschedule(db, b, 1791558000, 30, 0, 30),
          "reschedule: 0 takes the task off the planner");
    t = task_db_task_get(db, b);
    check(t != NULL && t->scheduled_start == 0, "reschedule: unscheduled");
    task_free(t);

    /* The calendar list and the event cache. */
    GPtrArray      *cals = g_ptr_array_new();
    gcal_calendar_t work = { "work@x", "Work", "#336699", TRUE, TRUE };
    gcal_calendar_t hol  = { "hol@x", "Holidays", "", FALSE, FALSE };
    g_ptr_array_add(cals, &hol);
    g_ptr_array_add(cals, &work);
    gcal_store_set_calendars(db, cals);
    g_ptr_array_free(cals, TRUE);
    GPtrArray *got = gcal_store_calendars(db);
    check(got->len == 2 && ((gcal_calendar_t *)g_ptr_array_index(got, 0))->is_primary &&
          ((gcal_calendar_t *)g_ptr_array_index(got, 0))->writable,
          "calendars: the primary comes first, flags kept");
    g_ptr_array_free(got, TRUE);

    gint64 lo = task_due_from_ymd(2026, 10, 9), hi = task_due_from_ymd(2026, 10, 10);
    GPtrArray   *evs   = g_ptr_array_new();
    gcal_event_t day   = { NULL, "e1", "Offsite", NULL, lo, hi, TRUE };
    gcal_event_t meet  = { NULL, "e2", "1:1", NULL, lo + 10 * 3600, lo + 11 * 3600, FALSE };
    gcal_event_t late  = { NULL, "e3", "Late", NULL, hi - 1800, hi + 1800, FALSE };
    gcal_event_t later = { NULL, "e4", "Tomorrow", NULL, hi + 3600, hi + 7200, FALSE };
    g_ptr_array_add(evs, &meet);
    g_ptr_array_add(evs, &later);
    g_ptr_array_add(evs, &late);
    g_ptr_array_add(evs, &day);
    gcal_store_set_events(db, "work@x", evs);
    g_ptr_array_free(evs, TRUE);

    GPtrArray *today = gcal_store_events_for_day(db, lo, hi);
    check(today->len == 3, "events: the day's overlapping events, not the next day's");
    check(today->len == 3 &&
          strcmp(((gcal_event_t *)g_ptr_array_index(today, 0))->event_id, "e1") == 0 &&
          strcmp(((gcal_event_t *)g_ptr_array_index(today, 1))->event_id, "e2") == 0 &&
          strcmp(((gcal_event_t *)g_ptr_array_index(today, 2))->event_id, "e3") == 0,
          "events: all-day first, then by start");
    check(today->len > 0 &&
          strcmp(((gcal_event_t *)g_ptr_array_index(today, 0))->color, "#336699") == 0,
          "events: carry their calendar's colour");
    g_ptr_array_free(today, TRUE);

    GPtrArray *empty = g_ptr_array_new();
    gcal_store_set_events(db, "work@x", empty);
    today = gcal_store_events_for_day(db, lo, hi);
    check(today->len == 0, "events: a calendar's events are replaced whole");
    g_ptr_array_free(today, TRUE);

    evs = g_ptr_array_new();
    g_ptr_array_add(evs, &meet);
    gcal_store_set_events(db, "hol@x", evs);
    g_ptr_array_free(evs, TRUE);
    GPtrArray *keep = g_ptr_array_new();
    g_ptr_array_add(keep, "hol@x");
    gcal_store_keep_events(db, keep);
    today = gcal_store_events_for_day(db, lo, hi);
    check(today->len == 1, "keep: a kept calendar's events stay");
    g_ptr_array_free(today, TRUE);
    gcal_store_keep_events(db, empty);
    today = gcal_store_events_for_day(db, lo, hi);
    check(today->len == 0, "keep: none kept drops every event");
    g_ptr_array_free(today, TRUE);
    g_ptr_array_free(keep, TRUE);
    g_ptr_array_free(empty, TRUE);

    check(gcal_store_account(db) == NULL, "account: none recorded yet");
    gcal_store_set_account(db, "me@x");
    gchar *acct = gcal_store_account(db);
    check(acct != NULL && strcmp(acct, "me@x") == 0, "account: read back");
    g_free(acct);
}

/* What one sign-in's completion reported. */
static struct {
    int    calls;
    int    ok;
    gchar *error;
} signin;

/*
 * on_signin — the sign-in's completion: record it.
 *
 * Inputs:
 *   ok    — the outcome
 *   error — the reason (borrowed)
 *   data  — unused
 */
static void
on_signin(gboolean ok, const gchar *error, gpointer data)
{
    (void)data;
    signin.calls++;
    signin.ok = ok;
    g_free(signin.error);
    signin.error = g_strdup(error);
}

/*
 * browse — be the browser: connect to the loopback port the URL names,
 * send one request line, and read the reply.
 *
 * Inputs:
 *   url  — the sign-in URL from task_oauth_begin
 *   path — the request target ("/?code=…")
 *
 * Output:
 *   the reply (g_free), "" when the listener closed without one.
 */
static gchar *
browse(const gchar *url, const gchar *path)
{
    const gchar *at   = strstr(url, "127.0.0.1%3A");
    int          port = at != NULL ? atoi(at + strlen("127.0.0.1%3A")) : 0;
    int          fd   = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { 0 };
    addr.sin_len         = sizeof addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (port == 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return g_strdup("");
    }
    gchar *req = g_strdup_printf("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
    ssize_t sent = write(fd, req, strlen(req));
    (void)sent;
    g_free(req);

    struct timeval tv = { 0, 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    GString *reply = g_string_new(NULL);
    for (int i = 0; i < 30; i++) {
        check_spin(0.05);
        char    buf[512];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n > 0) g_string_append_len(reply, buf, n);
    }
    close(fd);
    return g_string_free(reply, FALSE);
}

/*
 * oauth_checks — the sign-in's loopback listener.
 *
 * Inputs:
 *   data_dir — the fixture's data directory, where the client file goes
 */
static void
oauth_checks(const gchar *data_dir)
{
    gchar *err = NULL;
    gchar *client = g_build_filename(data_dir, TASK_OAUTH_CLIENT_FILE, NULL);
    g_file_set_contents(client, "{\"installed\":{\"client_id\":\"test-client.apps\","
                                "\"client_secret\":\"not-a-secret\"}}", -1, NULL);
    gchar *url = task_oauth_begin(on_signin, NULL, &err);
    check(url != NULL && task_oauth_have_client(), "oauth: the data folder's client file is used");
    check(url != NULL && strstr(url, "client_id=test-client.apps") != NULL &&
          strstr(url, "code_challenge_method=S256") != NULL &&
          strstr(url, "access_type=offline") != NULL &&
          strstr(url, "calendar.events") != NULL, "oauth: the URL asks for an offline PKCE grant");
    check(task_oauth_begin(on_signin, NULL, &err) == NULL && err != NULL,
          "oauth: one sign-in at a time");
    g_clear_pointer(&err, g_free);

    gchar *reply = browse(url, "/favicon.ico");
    check(signin.calls == 0, "oauth: a request that is not the redirect is ignored");
    g_free(reply);

    reply = browse(url, "/?code=abc&state=forged");
    check(strstr(reply, "State mismatch") != NULL, "oauth: a forged state is answered");
    check(signin.calls == 1 && !signin.ok && strstr(signin.error, "state") != NULL,
          "oauth: ... and fails the sign-in without exchanging the code");
    g_free(reply);
    g_free(url);

    url   = task_oauth_begin(on_signin, NULL, &err);
    reply = url != NULL ? browse(url, "/?error=access_denied") : g_strdup("");
    check(strstr(reply, "denied") != NULL && signin.calls == 2 && !signin.ok &&
          strstr(signin.error, "access_denied") != NULL, "oauth: a refusal ends the sign-in");
    check(!task_oauth_authenticated(), "oauth: no grant is stored by a failed sign-in");
    g_free(reply);
    g_free(url);
    g_free(signin.error);
    g_unlink(client);
    g_free(client);
}

/*
 * main — run the calendar sync tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    plan_checks();
    api_checks();
    store_checks(app->db);
    char *dir = task_data_dir();
    oauth_checks(dir);
    free(dir);

    fixture_close(app);
    return check_result();
}
