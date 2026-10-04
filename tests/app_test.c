/*
 * app_test.c — the event bus (src/task_app.h), the due-date tint
 * (src/task_dates.h) and the worker scheduler running under the main-loop
 * service (src/task_worker.h, src/recur.h).
 */

#include "fixture.h"
#include "recur.h"
#include "task_dates.h"
#include "task_worker.h"

#include <string.h>

/* What the listeners record. */
static int   calls[4];
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

    Task *t = task_db_task_get(app->db, id);
    t->status         = TASK_STATUS_DONE;
    t->recur_interval = 1;
    t->recur_unit     = TASK_RECUR_MINUTE;
    t->recur_lead     = 0;
    task_db_task_update(app->db, t);
    task_free(t);
    task_db_task_recur_set_next(app->db, id, now + 1);

    task_recur_init(app);
    task_worker_arm_all(app, app->db->path);     /* runs one pass, arms the timer */

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
    test_urgency();
    test_recurrence_timer(app);
    fixture_close(app);
    return check_result();
}
