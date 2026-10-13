/*
 * app_test.c — the event bus (src/task_app.h), the due-date tint and
 * the 3-Day Forecast (src/task_dates.h, src/core_views.h) and the worker scheduler running under the main-loop
 * service (src/task_worker.h, src/recur.h).
 */

#include "fixture.h"
#include "core_views.h"
#include "recur.h"
#include "task_config.h"
#include "task_dates.h"
#include "task_view.h"
#include "task_worker.h"

#include <string.h>

/* What the listeners record. */
static int   calls[5];
static guint ids[4];
static char  last_message[64];

/*
 * on_event_0 — a listener that unsubscribes ITSELF and listener 2 while the
 * event is being fired.
 *
 * Inputs:
 *   app  — the context
 *   data — unused
 */
static void
on_event_0(TaskApp *app, gpointer data)
{
    (void)data;
    calls[0]++;
    task_app_unlisten(app, ids[0]);
    task_app_unlisten(app, ids[2]);
}

/*
 * on_event — a plain counting listener.
 *
 * Inputs:
 *   app  — the context (unused)
 *   data — the index of the counter to bump, as a pointer
 */
static void
on_event(TaskApp *app, gpointer data)
{
    (void)app;
    calls[GPOINTER_TO_INT(data)]++;
}

/*
 * on_status — a status listener: keep the message.
 *
 * Inputs:
 *   app     — the context (unused)
 *   message — the message (borrowed)
 *   data    — unused
 */
static void
on_status(TaskApp *app, const gchar *message, gpointer data)
{
    (void)app;
    (void)data;
    snprintf(last_message, sizeof last_message, "%s", message);
}

/*
 * test_events — firing, the fallback, and unsubscribing mid-fire.
 *
 * Inputs:
 *   app — the context
 */
static void
test_events(TaskApp *app)
{
    task_app_notify_changed(app);
    task_app_notify_tasks(app);
    task_app_status(app, "nobody %s", "listening");
    check(1, "events: firing with no listeners is harmless");

    ids[0] = task_app_listen_changed(app, on_event_0, NULL);
    ids[1] = task_app_listen_changed(app, on_event, GINT_TO_POINTER(1));
    ids[2] = task_app_listen_changed(app, on_event, GINT_TO_POINTER(2));
    check(ids[0] != 0 && ids[1] != ids[0] && ids[2] != ids[1], "events: distinct ids");

    /* With no task-pane listener the light event falls back to the full
     * one; listener 0 removes itself and listener 2 as it runs. */
    task_app_notify_tasks(app);
    check(calls[0] == 1 && calls[1] == 1, "events: light event falls back to full");
    check(calls[2] == 0, "events: a listener removed mid-fire is not called");

    task_app_notify_changed(app);
    check(calls[0] == 1 && calls[1] == 2 && calls[2] == 0,
          "events: removed listeners stay removed");

    ids[3] = task_app_listen_tasks(app, on_event, GINT_TO_POINTER(3));
    task_app_notify_tasks(app);
    check(calls[3] == 1 && calls[1] == 2, "events: a task-pane listener takes the light event");

    guint sid = task_app_listen_status(app, on_status, NULL);
    task_app_status(app, "%d tasks", 3);
    check(strcmp(last_message, "3 tasks") == 0, "events: status message is formatted");

    task_app_unlisten(app, sid);
    task_app_unlisten(app, ids[1]);
    task_app_unlisten(app, ids[3]);
    task_app_unlisten(app, 9999);
    task_app_status(app, "unheard");
    check(strcmp(last_message, "3 tasks") == 0, "events: unsubscribed status listener is silent");
    check(task_app_listen_changed(app, NULL, NULL) == 0, "events: a NULL callback is refused");
}

/*
 * test_outside_changes — the watch (task_app_check_outside): another
 * connection's commit fires the full refresh once; the app's own writes,
 * and another connection's transaction that writes nothing (the backup
 * poll's claim check), do not; the timer finds a change by itself.
 *
 * Inputs:
 *   app — the context
 */
static void
test_outside_changes(TaskApp *app)
{
    guint id = task_app_listen_changed(app, on_event, GINT_TO_POINTER(4));
    int   before;

    task_app_check_outside(app);                 /* settle the baseline */
    before = calls[4];
    check(!task_app_check_outside(app) && calls[4] == before,
          "outside: nothing changed, nothing fired");

    task_db_list_create(app->db, "Own write", "");
    check(!task_app_check_outside(app) && calls[4] == before,
          "outside: the app's own write is not an outside change");

    TaskDatabase *other = task_db_open(shared_db_file(app->db->shared), NULL);
    task_db_list_create(other, "Written elsewhere", "");
    check(task_app_check_outside(app) && calls[4] == before + 1,
          "outside: another connection's commit fires the full refresh");
    check(!task_app_check_outside(app) && calls[4] == before + 1,
          "outside: ... once");

    shared_db_begin(other->shared, NULL);
    shared_db_commit(other->shared, NULL);
    check(!task_app_check_outside(app),
          "outside: a transaction that writes nothing is not a change");

    task_app_watch_start(app);
    check(app->watch_timer != 0, "outside: the watch is armed");
    task_db_list_create(other, "Seen by the timer", "");
    for (int i = 0; i < 40 && calls[4] == before + 1; i++) check_spin(0.05);
    check(calls[4] == before + 2, "outside: the watch finds a change by itself");
    task_app_watch_stop(app);
    check(app->watch_timer == 0, "outside: the watch stops");

    task_db_close(other);
    task_app_unlisten(app, id);
}

/*
 * test_urgency — the Due column's tint against today's bounds.
 */
static void
test_urgency(void)
{
    gint64 lo, hi;

    task_day_bounds(0, &lo, &hi);
    check(hi - lo >= 23 * 3600 && hi - lo <= 25 * 3600, "dates: today is about a day long");
    check(task_due_urgency(0) == task_due_none, "urgency: no date, no tint");
    check(task_due_urgency(lo - 1) == task_due_overdue, "urgency: before today is overdue");
    check(task_due_urgency(lo) == task_due_today, "urgency: local midnight is today");
    check(task_due_urgency(hi - 1) == task_due_today, "urgency: the last second of today");
    check(task_due_urgency(hi) == task_due_ahead, "urgency: tomorrow is ahead");
    check(task_due_urgency(task_due_instant(lo, 8 * 60)) == task_due_today,
          "urgency: a due moment today");
}

/*
 * test_forecast_span — the 3-Day Forecast's window: three calendar days,
 * or three business days with the weekend between them kept inside.
 */
static void
test_forecast_span(void)
{
    check(task_forecast_span(1, 3, FALSE) == 3, "forecast: three calendar days");
    check(task_forecast_span(5, 3, FALSE) == 3, "forecast: ... even from a Friday");
    check(task_forecast_span(1, 3, TRUE) == 3, "forecast: Monday's business days are Mon-Wed");
    check(task_forecast_span(3, 3, TRUE) == 3, "forecast: Wednesday's are Wed-Fri");
    check(task_forecast_span(4, 3, TRUE) == 5, "forecast: Thursday's run to Monday");
    check(task_forecast_span(5, 3, TRUE) == 5, "forecast: Friday's run to Tuesday");
    check(task_forecast_span(6, 3, TRUE) == 5, "forecast: Saturday's run to Wednesday");
    check(task_forecast_span(7, 3, TRUE) == 4, "forecast: Sunday's run to Wednesday");
    check(task_forecast_span(1, 0, TRUE) == 1, "forecast: fewer than one day counts one");

    gint64 lo, hi;
    task_day_bounds(2, &lo, &hi);
    check(task_forecast_end(3, FALSE) == hi, "forecast: the calendar window ends after the day after tomorrow");
    check(task_forecast_end(3, TRUE) >= hi, "forecast: the business window ends no sooner");
}

/*
 * due_task — a task in a list, due at a given local midnight.
 *
 * Inputs:
 *   app   — the context
 *   list  — the list
 *   title — the title
 *   due   — local midnight, or 0 for no date
 *
 * Output:
 *   the task's id.
 */
static gint64
due_task(TaskApp *app, gint64 list, const char *title, gint64 due)
{
    gint64 id = task_db_task_create(app->db, list, 0, title);
    Task  *t  = task_db_task_get(app->db, id);
    Task   after = *t;
    after.due = due;
    task_db_task_update(app->db, t, &after);
    task_free(t);
    return id;
}

/*
 * forecast_has — whether the 3-Day Forecast view shows a task.
 *
 * Inputs:
 *   app — the context
 *   id  — the task
 *
 * Output:
 *   TRUE when the view's query returns it.
 */
static gboolean
forecast_has(TaskApp *app, gint64 id)
{
    GPtrArray *tasks = task_view_find("forecast")->query(app, NULL);
    gboolean   found = FALSE;
    for (guint i = 0; i < tasks->len && !found; i++) {
        found = ((const Task *)g_ptr_array_index(tasks, i))->id == id;
    }
    task_ptr_array_free_tasks(tasks);
    return found;
}

/*
 * test_forecast_view — the 3-Day Forecast shows everything past due and
 * everything due inside its window, and nothing later or undated; the
 * business-days setting is on by default and widens the window.
 *
 * Inputs:
 *   app — the context
 */
static void
test_forecast_view(TaskApp *app)
{
    task_core_views_init();
    check(task_config_get_bool(TASK_CFG_FORECAST_BUSINESS_DAYS),
          "forecast: business days are counted by default");

    gint64 list = task_db_list_create(app->db, "Forecast", NULL);
    gint64 lo, hi;
    task_day_bounds(-30, &lo, &hi);
    gint64 overdue = due_task(app, list, "Long overdue", lo);
    task_day_bounds(0, &lo, &hi);
    gint64 today   = due_task(app, list, "Today", lo);
    gint64 undated = due_task(app, list, "Undated", 0);

    gint64 business_end = task_forecast_end(3, TRUE);
    gint64 last_in      = due_task(app, list, "Last business day", business_end - 24 * 3600);
    gint64 after        = due_task(app, list, "After", business_end);

    check(forecast_has(app, overdue) && forecast_has(app, today),
          "forecast: past-due and today's tasks are shown");
    check(forecast_has(app, last_in), "forecast: the window's last day is shown");
    check(!forecast_has(app, after) && !forecast_has(app, undated),
          "forecast: later and undated tasks are not");

    task_config_set_bool(TASK_CFG_FORECAST_BUSINESS_DAYS, FALSE);
    task_day_bounds(2, &lo, &hi);
    gint64 day_after = due_task(app, list, "Day after tomorrow", lo);
    gint64 day_three = due_task(app, list, "Three days out", hi);
    check(forecast_has(app, day_after) && !forecast_has(app, day_three),
          "forecast: calendar days run to the day after tomorrow");
    task_config_set_bool(TASK_CFG_FORECAST_BUSINESS_DAYS, TRUE);
}

/*
 * test_recurrence_timer — the recurrence pass, armed by the scheduler, is
 * woken by a main-loop timer: a Done task whose next occurrence is a second
 * away is reopened without anything calling the pass again.
 *
 * Inputs:
 *   app — the context
 */
static void
test_recurrence_timer(TaskApp *app)
{
    gint64 list = task_db_list_create(app->db, "Repeats", NULL);
    gint64 id   = task_db_task_create(app->db, list, 0, "Every minute");
    gint64 now  = (gint64)time(NULL);

    Task *t     = task_db_task_get(app->db, id);
    Task  after = *t;
    after.status         = TASK_STATUS_DONE;
    after.recur_interval = 1;
    after.recur_unit     = TASK_RECUR_MINUTE;
    after.recur_lead     = 0;
    task_db_task_update(app->db, t, &after);
    task_free(t);
    task_db_task_recur_set_next(app->db, id, now + 1);

    task_recur_init(app);
    task_worker_arm_all(app, shared_db_file(app->db->shared));     /* runs one pass, arms the timer */

    t = task_db_task_get(app->db, id);
    check(t->status == TASK_STATUS_DONE, "recurrence: not yet due at arming");
    task_free(t);

    for (int i = 0; i < 40; i++) {
        check_spin(0.1);
        t = task_db_task_get(app->db, id);
        gboolean reopened = t->status == TASK_STATUS_NEW;
        task_free(t);
        if (reopened) break;
    }
    t = task_db_task_get(app->db, id);
    check(t->status == TASK_STATUS_NEW, "recurrence: the deadline timer fired and reopened the task");
    check(t->recur_next > now + 1, "recurrence: the next occurrence moved on");
    task_free(t);

    task_worker_disarm_all();
    check_spin(0.05);
    check(1, "recurrence: disarmed cleanly");
}

/*
 * main — run the context, date and scheduler tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    test_events(app);
    test_outside_changes(app);
    test_urgency();
    test_forecast_span();
    test_forecast_view(app);
    test_recurrence_timer(app);
    fixture_close(app);
    return check_result();
}
