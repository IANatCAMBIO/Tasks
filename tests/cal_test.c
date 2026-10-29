/*
 * cal_test.c — the calendar sync's core, without a calendar: the per-task
 * decision (cal_plan.h), the binding table (cal_store.h) — the dirty
 * check and the compare-and-set reschedule — and whole passes of the sync
 * (cal_sync.h) against a fake calendar (cal_backend.h): a session pushed,
 * moved, adopted from the calendar, unscheduled by a deletion there,
 * moved to another calendar, removed, and a Google-sync binding found
 * again by its Google id.  The macOS Calendar is never touched.
 */

#include "fixture.h"
#include "cal_plan.h"
#include "cal_store.h"
#include "cal_sync.h"

#include <string.h>

/*
 * plan_checks — every rule of cal_plan, in the order cal_plan.h lists
 * them.
 */
static void
plan_checks(void)
{
    cal_slot_t base    = { 1000, 60, "Write report" };
    cal_slot_t moved   = { 4600, 60, "Write report" };
    cal_slot_t renamed = { 1000, 60, "Write the report" };
    cal_slot_t longer  = { 1000, 90, "Write report" };
    cal_slot_t ev_same = { 1000, 60, NULL };
    cal_slot_t ev_move = { 8200, 30, NULL };

    check(cal_plan(NULL, &base, CAL_EVENT_UNKNOWN, NULL, FALSE) == CAL_ACT_CREATE,
          "plan: an unbound scheduled task is pushed");
    check(cal_plan(NULL, NULL, CAL_EVENT_UNKNOWN, NULL, FALSE) == CAL_ACT_NONE,
          "plan: nothing bound, nothing scheduled, nothing to do");
    check(cal_plan(&base, NULL, CAL_EVENT_PRESENT, &ev_same, FALSE) == CAL_ACT_REMOVE,
          "plan: a task off the planner deletes its event");
    check(cal_plan(&base, NULL, CAL_EVENT_UNKNOWN, NULL, FALSE) == CAL_ACT_REMOVE,
          "plan: ... even when the event was not looked at");
    check(cal_plan(&base, NULL, CAL_EVENT_MISSING, NULL, FALSE) == CAL_ACT_FORGET,
          "plan: both gone: the binding is dropped");
    check(cal_plan(&base, &base, CAL_EVENT_PRESENT, &ev_same, TRUE) == CAL_ACT_MOVE,
          "plan: an event in the wrong calendar moves");
    check(cal_plan(&base, &base, CAL_EVENT_MISSING, NULL, FALSE) == CAL_ACT_UNSCHEDULE,
          "plan: deleted in the calendar, unchanged here: the task leaves the planner");
    check(cal_plan(&base, &moved, CAL_EVENT_MISSING, NULL, FALSE) == CAL_ACT_CREATE,
          "plan: deleted in the calendar but moved here: pushed again");
    check(cal_plan(&base, &moved, CAL_EVENT_PRESENT, &ev_same, FALSE) == CAL_ACT_UPDATE,
          "plan: a moved task updates its event");
    check(cal_plan(&base, &renamed, CAL_EVENT_UNKNOWN, NULL, FALSE) == CAL_ACT_UPDATE,
          "plan: a renamed task updates its event");
    check(cal_plan(&base, &longer, CAL_EVENT_PRESENT, &ev_move, FALSE) == CAL_ACT_UPDATE,
          "plan: both changed: the local edit wins");
    check(cal_plan(&base, &base, CAL_EVENT_PRESENT, &ev_move, FALSE) == CAL_ACT_ADOPT,
          "plan: an event moved in the calendar moves the session");
    check(cal_plan(&base, &base, CAL_EVENT_PRESENT, &ev_same, FALSE) == CAL_ACT_NONE,
          "plan: in step: nothing to do");
    check(cal_plan(&base, &base, CAL_EVENT_UNKNOWN, NULL, FALSE) == CAL_ACT_NONE,
          "plan: an event not looked at changes nothing");

    cal_slot_t untitled_task   = { 1000, 60, NULL };
    cal_slot_t untitled_pushed = { 1000, 60, "" };
    check(cal_plan(&untitled_pushed, &untitled_task, CAL_EVENT_UNKNOWN, NULL, FALSE)
          == CAL_ACT_NONE, "plan: a NULL title and an empty one are the same");
}

/*
 * store_checks — the sync's binding table against a real database.
 *
 * Inputs:
 *   db — an empty database
 */
static void
store_checks(TaskDatabase *db)
{
    GPtrArray *none = cal_store_bindings(db);
    check(none->len == 0, "store: a new file has no bindings");
    g_ptr_array_free(none, TRUE);
    check(!cal_store_dirty(db), "store: an empty planner is clean");

    gint64 a    = task_db_task_create(db, 0, "Write report", "Work");
    gint64 b    = task_db_task_create(db, 0, "Review", "Work");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    check(cal_store_dirty(db), "dirty: a scheduled, unbound task");

    cal_binding_t bind = { a, "primary@x", "ev-a", 1791550800, 60, "Write report" };
    cal_store_bind(db, &bind);
    check(!cal_store_dirty(db), "dirty: bound and in step is clean");

    task_db_task_set_scheduled(db, a, 1791554400, 60);
    check(cal_store_dirty(db), "dirty: a moved session");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    check(!cal_store_dirty(db), "dirty: moved back is clean again");
    task_db_task_set_scheduled(db, a, 1791550800, 45);
    check(cal_store_dirty(db), "dirty: a stretched session");
    task_db_task_set_scheduled(db, a, 1791550800, 60);

    task_db_task_set_scheduled(db, a, 0, 0);
    check(cal_store_dirty(db), "dirty: a session removed from the planner");
    task_db_task_set_scheduled(db, a, 1791550800, 60);
    task_db_task_delete(db, a);
    check(cal_store_dirty(db), "dirty: a deleted task");
    task_db_task_purge(db, a);
    check(cal_store_dirty(db), "dirty: a purged task (the binding outlives the row)");
    GPtrArray *bs = cal_store_bindings(db);
    check(bs->len == 1 && ((cal_binding_t *)g_ptr_array_index(bs, 0))->task_id == a &&
          strcmp(((cal_binding_t *)g_ptr_array_index(bs, 0))->event_id, "ev-a") == 0,
          "bindings: read back whole");
    g_ptr_array_free(bs, TRUE);
    cal_store_unbind(db, a);
    check(!cal_store_dirty(db), "unbind: the purged task is forgotten");

    /* Compare-and-set: only from the session that was read. */
    task_db_task_set_scheduled(db, b, 1791550800, 60);
    check(!cal_store_reschedule(db, b, 1791554400, 60, 1791558000, 30),
          "reschedule: a session changed meanwhile is not overwritten");
    check(cal_store_reschedule(db, b, 1791550800, 60, 1791558000, 30),
          "reschedule: the expected session is replaced");
    Task *t = task_db_task_get(db, b);
    check(t != NULL && t->scheduled_start == 1791558000 && t->scheduled_duration == 30,
          "reschedule: the new session is stored");
    task_free(t);
    check(cal_store_reschedule(db, b, 1791558000, 30, 0, 30),
          "reschedule: 0 takes the task off the planner");
    t = task_db_task_get(db, b);
    check(t != NULL && t->scheduled_start == 0, "reschedule: unscheduled");
    task_free(t);
}

/* ── A fake calendar ──────────────────────────────────────────────────── */

/* One event of the fake calendar. */
typedef struct {
    gchar  *calendar_id;
    gchar  *title;
    gint64  start, end;
} fake_event_t;

/* The fake calendar: calendars "work" and "home", events by id. */
static struct {
    gboolean    ready;
    const char *tasks_calendar;
    GHashTable *events;              /* id → fake_event_t*                  */
    GHashTable *google;              /* Google id → event id                */
    int         next_id;
} fake;

/*
 * fake_event_free — free one fake event.
 *
 * Inputs:
 *   data — the fake_event_t
 */
static void
fake_event_free(gpointer data)
{
    fake_event_t *e = data;
    g_free(e->calendar_id);
    g_free(e->title);
    g_free(e);
}

static gboolean fk_ready(void) { return fake.ready; }
static gchar   *fk_tasks_calendar(void) { return g_strdup(fake.tasks_calendar); }

/*
 * fk_calendar_exists — "work" and "home" exist.
 *
 * Inputs:
 *   calendar_id — the id
 *
 * Output: TRUE for those two.
 */
static gboolean
fk_calendar_exists(const gchar *calendar_id)
{
    return strcmp(calendar_id, "work") == 0 || strcmp(calendar_id, "home") == 0;
}

/*
 * fk_find — an event, by id (cal_backend.h).
 *
 * Inputs:
 *   event_id — the id
 *   out      — out: its times
 *   err      — unused
 *
 * Output: CAL_OK or CAL_GONE.
 */
static cal_result_t
fk_find(const gchar *event_id, cal_event_t *out, gchar **err)
{
    (void)err;
    fake_event_t *e = g_hash_table_lookup(fake.events, event_id);
    if (e == NULL) return CAL_GONE;
    *out = (cal_event_t){ e->start, e->end, FALSE };
    return CAL_OK;
}

/*
 * fk_find_google — a Google id's event (cal_backend.h).
 *
 * Inputs:
 *   google_id   — the Google id
 *   near        — unused
 *   event_id    — out: the event's id
 *   calendar_id — out: its calendar's
 *
 * Output: TRUE when known.
 */
static gboolean
fk_find_google(const gchar *google_id, gint64 near, gchar **event_id, gchar **calendar_id)
{
    (void)near;
    const gchar  *id = g_hash_table_lookup(fake.google, google_id);
    fake_event_t *e  = id != NULL ? g_hash_table_lookup(fake.events, id) : NULL;
    if (e == NULL) return FALSE;
    *event_id    = g_strdup(id);
    *calendar_id = g_strdup(e->calendar_id);
    return TRUE;
}

/*
 * fk_insert — create an event (cal_backend.h).
 *
 * Inputs:
 *   calendar_id, start, duration, title — the session
 *   event_id — out: the new id
 *   err      — unused
 *
 * Output: CAL_OK.
 */
static cal_result_t
fk_insert(const gchar *calendar_id, gint64 start, gint duration, const gchar *title,
          gchar **event_id, gchar **err)
{
    (void)err;
    fake_event_t *e = g_new0(fake_event_t, 1);
    e->calendar_id = g_strdup(calendar_id);
    e->title       = g_strdup(title);
    e->start       = start;
    e->end         = start + (gint64)duration * 60;
    *event_id = g_strdup_printf("ev%d", ++fake.next_id);
    g_hash_table_insert(fake.events, g_strdup(*event_id), e);
    return CAL_OK;
}

/*
 * fk_update — write a session to an event (cal_backend.h).
 *
 * Inputs:
 *   event_id, start, duration, title — the event and the session
 *   err — unused
 *
 * Output: CAL_OK or CAL_GONE.
 */
static cal_result_t
fk_update(const gchar *event_id, gint64 start, gint duration, const gchar *title, gchar **err)
{
    (void)err;
    fake_event_t *e = g_hash_table_lookup(fake.events, event_id);
    if (e == NULL) return CAL_GONE;
    g_free(e->title);
    e->title = g_strdup(title);
    e->start = start;
    e->end   = start + (gint64)duration * 60;
    return CAL_OK;
}

/*
 * fk_remove — delete an event (cal_backend.h).
 *
 * Inputs:
 *   event_id — the event
 *   err      — unused
 *
 * Output: CAL_OK or CAL_GONE.
 */
static cal_result_t
fk_remove(const gchar *event_id, gchar **err)
{
    (void)err;
    return g_hash_table_remove(fake.events, event_id) ? CAL_OK : CAL_GONE;
}

static const cal_backend_t fake_backend = {
    .ready = fk_ready, .tasks_calendar = fk_tasks_calendar,
    .calendar_exists = fk_calendar_exists, .find = fk_find, .find_google = fk_find_google,
    .insert = fk_insert, .update = fk_update, .remove = fk_remove,
};

/*
 * bound_event — the event a task is bound to.
 *
 * Inputs:
 *   db      — the database
 *   task_id — the task
 *
 * Output: the fake event, or NULL when unbound or gone.
 */
static fake_event_t *
bound_event(TaskDatabase *db, gint64 task_id)
{
    fake_event_t *found    = NULL;
    GPtrArray    *bindings = cal_store_bindings(db);
    for (guint i = 0; i < bindings->len; i++) {
        cal_binding_t *b = g_ptr_array_index(bindings, i);
        if (b->task_id == task_id) found = g_hash_table_lookup(fake.events, b->event_id);
    }
    g_ptr_array_free(bindings, TRUE);
    return found;
}

/*
 * sync_checks — whole passes against the fake calendar.
 *
 * Inputs:
 *   app — the context (its database empty of sessions)
 */
static void
sync_checks(TaskApp *app)
{
    TaskDatabase *db = app->db;
    fake.events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_event_free);
    fake.google = g_hash_table_new(g_str_hash, g_str_equal);
    fake.tasks_calendar = "work";
    cal_sync_init(app, &fake_backend);

    gint64 t = task_db_task_create(db, 0, "Plan the week", "Work");
    task_db_task_set_scheduled(db, t, 1791550800, 60);
    cal_sync_pass();
    check(g_hash_table_size(fake.events) == 0, "sync: nothing is written while the calendar is off");

    fake.ready = TRUE;
    cal_sync_pass();
    fake_event_t *e = bound_event(db, t);
    check(e != NULL && strcmp(e->calendar_id, "work") == 0 && e->start == 1791550800 &&
          e->end - e->start == 3600 && strcmp(e->title, "Plan the week") == 0,
          "sync: a scheduled task becomes an event in the tasks calendar");
    cal_sync_pass();
    check(g_hash_table_size(fake.events) == 1, "sync: a pass in step creates nothing more");

    task_db_task_set_scheduled(db, t, 1791554400, 30);
    cal_sync_pass();
    e = bound_event(db, t);
    check(e != NULL && e->start == 1791554400 && e->end - e->start == 1800,
          "sync: moving and stretching the session moves the event");

    e->start = 1791561600;
    e->end   = 1791561600 + 45 * 60;
    cal_sync_pass();
    Task *task = task_db_task_get(db, t);
    check(task != NULL && task->scheduled_start == 1791561600 && task->scheduled_duration == 45,
          "sync: an event moved in the calendar moves the session");
    task_free(task);

    fake.tasks_calendar = "home";
    cal_sync_pass();
    e = bound_event(db, t);
    check(e != NULL && strcmp(e->calendar_id, "home") == 0 && g_hash_table_size(fake.events) == 1,
          "sync: another calendar for tasks moves the event there");

    GPtrArray *bs = cal_store_bindings(db);
    g_hash_table_remove(fake.events, ((cal_binding_t *)g_ptr_array_index(bs, 0))->event_id);
    g_ptr_array_free(bs, TRUE);
    cal_sync_pass();
    task = task_db_task_get(db, t);
    check(task != NULL && task->scheduled_start == 0 && bound_event(db, t) == NULL,
          "sync: an event deleted in the calendar takes the task off the planner");
    task_free(task);

    task_db_task_set_scheduled(db, t, 1791550800, 60);
    cal_sync_pass();
    check(bound_event(db, t) != NULL, "sync: scheduled again, pushed again");
    task_db_task_set_scheduled(db, t, 0, 0);
    cal_sync_pass();
    check(g_hash_table_size(fake.events) == 0, "sync: Remove from Schedule deletes the event");

    /* A Google-sync binding: found again by its Google id, not pushed twice. */
    gchar *made = NULL;
    fk_insert("home", 1791550800, 60, "Plan the week", &made, NULL);
    g_hash_table_insert(fake.google, "google-7", made);
    task_db_task_set_scheduled(db, t, 1791550800, 60);
    cal_binding_t old = { t, "me@gmail.com", "google-7", 1791550800, 60, "Plan the week" };
    cal_store_bind(db, &old);
    cal_sync_pass();
    e = bound_event(db, t);
    check(e != NULL && g_hash_table_size(fake.events) == 1 && strcmp(e->calendar_id, "home") == 0,
          "sync: a Google-sync binding is rebound to its event, which is not pushed twice");

    cal_sync_shut_down();
    g_free(made);
    g_hash_table_destroy(fake.google);
    g_hash_table_destroy(fake.events);
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
    store_checks(app->db);
    sync_checks(app);

    fixture_close(app);
    return check_result();
}
