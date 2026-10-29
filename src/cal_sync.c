/* ===========================================================================
 * cal_sync.c — the day planner's sync with the macOS Calendar (cal_sync.h)
 * =========================================================================== */

#include "cal_sync.h"
#include "cal_plan.h"
#include "cal_store.h"
#include "task_loop.h"

#include <string.h>

/* Seconds a kick waits, so a burst of edits makes one pass. */
static const double kick_delay_s = 2.0;

/* Bounds of an adopted session, in minutes (a move in the calendar cannot
 * make a session the planner could not draw). */
static const gint adopt_min_minutes = 15;
static const gint adopt_max_minutes = 24 * 60;

/* ── State ────────────────────────────────────────────────────────────── */

static TaskApp             *live_app;  /* NULL once shut down               */
static const cal_backend_t *backend;
static unsigned             kick_timer;
static gboolean             notifying;  /* our own task event is firing     */
static gboolean             last_ready; /* the last pass's settings, to see */
static gchar               *last_calendar; /* a change made from Notes      */

/* What one pass works with. */
typedef struct {
    TaskDatabase *db;
    gchar        *push_id;           /* the calendar sessions go to         */
    gchar        *error;             /* the first failure, or NULL          */
} pass_ctx_t;

/*
 * note_error — keep a pass's first failure (later ones are usually the
 * same cause again).
 *
 * Inputs:
 *   ctx — the pass
 *   err — the reason, owned (freed here when not kept)
 */
static void
note_error(pass_ctx_t *ctx, gchar *err)
{
    if (err == NULL) return;
    if (ctx->error == NULL) {
        ctx->error = err;
        return;
    }
    g_free(err);
}

/*
 * rebind_google — a binding the Google sync made names Google's ids, which
 * the calendar does not know: find its event by its Google id and bind
 * the task to it, so the pass goes on as if this sync had made it.
 *
 * Inputs:
 *   ctx — the pass
 *   b   — the binding; its ids are replaced when the event is found
 *
 * Output:
 *   TRUE when it was found and rebound.
 */
static gboolean
rebind_google(pass_ctx_t *ctx, cal_binding_t *b)
{
    gchar *event_id = NULL, *calendar_id = NULL;
    if (!backend->find_google(b->event_id, b->pushed_start, &event_id, &calendar_id))
        return FALSE;
    g_free(b->event_id);
    g_free(b->calendar_id);
    b->event_id    = event_id;
    b->calendar_id = calendar_id;
    cal_store_bind(ctx->db, b);
    return TRUE;
}

/*
 * look_up — what is known of a binding's event.  Only an event missing
 * from a calendar that exists is a deletion; a calendar that is gone (or
 * a failed look) proves nothing, and the event is UNKNOWN.
 *
 * Inputs:
 *   ctx  — the pass
 *   b    — the binding (a Google one is rebound first)
 *   slot — out: the event's slot when PRESENT
 *
 * Output:
 *   the state.
 */
static cal_event_state_t
look_up(pass_ctx_t *ctx, cal_binding_t *b, cal_slot_t *slot)
{
    if (!backend->calendar_exists(b->calendar_id) && !rebind_google(ctx, b))
        return CAL_EVENT_UNKNOWN;

    cal_event_t  ev  = { 0 };
    gchar       *err = NULL;
    cal_result_t res = backend->find(b->event_id, &ev, &err);
    if (res == CAL_GONE) return CAL_EVENT_MISSING;
    if (res != CAL_OK) {
        note_error(ctx, err);
        return CAL_EVENT_UNKNOWN;
    }
    if (ev.all_day) return CAL_EVENT_UNKNOWN;    /* made all-day there: nothing to adopt */
    slot->start    = ev.start;
    slot->duration = (gint)((ev.end - ev.start) / 60);
    slot->title    = NULL;
    return CAL_EVENT_PRESENT;
}

/*
 * push_new — insert the event for a task and bind it, replacing any
 * binding it had.
 *
 * Inputs:
 *   ctx — the pass
 *   t   — the task
 */
static void
push_new(pass_ctx_t *ctx, const Task *t)
{
    gchar *event_id = NULL, *err = NULL;
    if (backend->insert(ctx->push_id, t->scheduled_start, t->scheduled_duration, t->title,
                        &event_id, &err) != CAL_OK) {
        note_error(ctx, err);
        return;
    }
    cal_binding_t b = { t->id, ctx->push_id, event_id, t->scheduled_start,
                        t->scheduled_duration, t->title };
    cal_store_bind(ctx->db, &b);
    g_free(event_id);
}

/*
 * apply — carry out one planned action.
 *
 * Inputs:
 *   ctx — the pass
 *   act — the action
 *   b   — the task's binding, or NULL (CREATE only)
 *   t   — the task, or NULL when it left the planner
 *   ev  — the event's slot (ADOPT only)
 */
static void
apply(pass_ctx_t *ctx, cal_action_t act, const cal_binding_t *b, const Task *t,
      const cal_slot_t *ev)
{
    gchar        *err = NULL;
    cal_result_t  res;

    switch (act) {
    case CAL_ACT_NONE:
        break;
    case CAL_ACT_CREATE:
        push_new(ctx, t);
        break;
    case CAL_ACT_MOVE:
        /* The old calendar may be gone with the event; only a failure
         * that leaves the event standing stops the move. */
        res = backend->remove(b->event_id, &err);
        if (res == CAL_FAILED && backend->calendar_exists(b->calendar_id)) {
            note_error(ctx, err);
            break;
        }
        g_free(err);
        push_new(ctx, t);
        break;
    case CAL_ACT_UPDATE:
        res = backend->update(b->event_id, t->scheduled_start, t->scheduled_duration, t->title,
                              &err);
        if (res == CAL_GONE) {
            push_new(ctx, t);
        } else if (res == CAL_OK) {
            cal_binding_t nb = *b;
            nb.pushed_start    = t->scheduled_start;
            nb.pushed_duration = t->scheduled_duration;
            nb.pushed_title    = t->title;
            cal_store_bind(ctx->db, &nb);
        } else {
            note_error(ctx, err);
        }
        break;
    case CAL_ACT_REMOVE:
        res = backend->remove(b->event_id, &err);
        if (res == CAL_FAILED) {
            note_error(ctx, err);
            break;
        }
        cal_store_unbind(ctx->db, b->task_id);
        break;
    case CAL_ACT_FORGET:
        cal_store_unbind(ctx->db, b->task_id);
        break;
    case CAL_ACT_ADOPT: {
        gint dur = CLAMP(ev->duration, adopt_min_minutes, adopt_max_minutes);
        if (cal_store_reschedule(ctx->db, t->id, t->scheduled_start, t->scheduled_duration,
                                 ev->start, dur)) {
            cal_binding_t nb = *b;
            nb.pushed_start    = ev->start;
            nb.pushed_duration = dur;
            nb.pushed_title    = t->title;
            cal_store_bind(ctx->db, &nb);
        }
        break;
    }
    case CAL_ACT_UNSCHEDULE:
        if (cal_store_reschedule(ctx->db, t->id, t->scheduled_start, t->scheduled_duration,
                                 0, t->scheduled_duration))
            cal_store_unbind(ctx->db, t->id);
        break;
    }
}

/*
 * push — every binding and every scheduled task through cal_plan.
 *
 * Inputs:
 *   ctx — the pass
 */
static void
push(pass_ctx_t *ctx)
{
    GPtrArray  *tasks    = task_db_tasks_scheduled_for_day(ctx->db, 1, G_MAXINT64);
    GPtrArray  *bindings = cal_store_bindings(ctx->db);
    GHashTable *by_id    = g_hash_table_new(g_int64_hash, g_int64_equal);
    GHashTable *bound    = g_hash_table_new(g_int64_hash, g_int64_equal);

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        g_hash_table_insert(by_id, &t->id, t);
    }

    for (guint i = 0; i < bindings->len; i++) {
        cal_binding_t *b = g_ptr_array_index(bindings, i);
        const Task    *t = g_hash_table_lookup(by_id, &b->task_id);
        g_hash_table_add(bound, &b->task_id);

        cal_slot_t        ev    = { 0 };
        cal_event_state_t state = look_up(ctx, b, &ev);
        cal_slot_t        pushed = { b->pushed_start, b->pushed_duration, b->pushed_title };
        cal_slot_t        now    = { 0 };
        if (t != NULL) now = (cal_slot_t){ t->scheduled_start, t->scheduled_duration, t->title };
        gboolean misplaced = strcmp(b->calendar_id, ctx->push_id) != 0;

        apply(ctx, cal_plan(&pushed, t != NULL ? &now : NULL, state, &ev, misplaced), b, t, &ev);
    }

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        if (!g_hash_table_contains(bound, &t->id)) apply(ctx, CAL_ACT_CREATE, NULL, t, NULL);
    }

    g_hash_table_destroy(bound);
    g_hash_table_destroy(by_id);
    g_ptr_array_free(bindings, TRUE);
    task_ptr_array_free_tasks(tasks);
}

/*
 * notify_tasks — fire the task-pane event (redrawing the planner)
 * without it counting as an edit to push.
 */
static void
notify_tasks(void)
{
    notifying = TRUE;
    task_app_notify_tasks(live_app);
    notifying = FALSE;
}

/*
 * cal_sync_pass — one pass, then a redraw (also when the sync is off: it
 * may just have been switched off).
 * Documented in cal_sync.h.
 */
void
cal_sync_pass(void)
{
    if (live_app == NULL || live_app->db == NULL) return;
    gboolean ready = backend->ready();
    gchar   *push_id = ready ? backend->tasks_calendar() : NULL;
    last_ready = ready;
    g_free(last_calendar);
    last_calendar = g_strdup(push_id);
    if (push_id != NULL) {
        pass_ctx_t ctx = { live_app->db, push_id, NULL };
        push(&ctx);
        if (ctx.error != NULL) task_app_status(live_app, "Calendar: %s", ctx.error);
        g_free(ctx.error);
        g_free(push_id);
    } else if (ready) {
        task_app_status(live_app, "Calendar: no calendar can take the scheduled tasks");
    }
    notify_tasks();
}

/*
 * settings_moved — whether the sync's settings differ from the last
 * pass's: switched on or off, or another calendar for the sessions.
 *
 * Output:
 *   TRUE when they do.
 */
static gboolean
settings_moved(void)
{
    gboolean ready = backend->ready();
    if (ready != last_ready) return TRUE;
    if (!ready) return FALSE;
    gchar   *now   = backend->tasks_calendar();
    gboolean moved = g_strcmp0(now, last_calendar) != 0;
    g_free(now);
    return moved;
}

/*
 * kick_fire — the kick's delay ran out: run the pass.
 *
 * Inputs:
 *   data — unused
 */
static void
kick_fire(void *data)
{
    (void)data;
    kick_timer = 0;
    cal_sync_pass();
}

/*
 * schedule — run a pass shortly, after any already scheduled.
 */
static void
schedule(void)
{
    task_loop_timer_cancel(kick_timer);
    kick_timer = task_loop_timer_once(kick_delay_s, kick_fire, NULL, NULL);
}

/*
 * cal_sync_kick — a pass shortly when one is due.
 * Documented in cal_sync.h.
 */
void
cal_sync_kick(void)
{
    if (live_app == NULL || live_app->db == NULL) return;
    if (settings_moved() || (backend->ready() && cal_store_dirty(live_app->db))) schedule();
}

/*
 * cal_sync_calendar_changed — redraw, and a pass shortly.
 * Documented in cal_sync.h.
 */
void
cal_sync_calendar_changed(void)
{
    if (live_app == NULL || live_app->db == NULL) return;
    notify_tasks();
    if (backend->ready()) schedule();
}

/*
 * cal_sync_settings_changed — a pass now (which redraws).
 * Documented in cal_sync.h.
 */
void
cal_sync_settings_changed(void)
{
    task_loop_timer_cancel(kick_timer);
    kick_timer = 0;
    cal_sync_pass();
}

/*
 * on_task_event — a task changed somewhere (an edit, a delete, a move),
 * or another app committed: kick, unless the event is the one a pass
 * fires itself.
 *
 * Inputs:
 *   app  — the application context (unused: live_app is the same one)
 *   data — unused
 */
static void
on_task_event(TaskApp *app, gpointer data)
{
    (void)app;
    (void)data;
    if (!notifying) cal_sync_kick();
}

/*
 * cal_sync_init — subscribe, and a first pass shortly.
 * Documented in cal_sync.h.
 */
void
cal_sync_init(TaskApp *app, const cal_backend_t *calendar)
{
    live_app = app;
    backend  = calendar;
    task_app_listen_tasks(app, on_task_event, NULL);
    task_app_listen_changed(app, on_task_event, NULL);
    if (backend->ready()) schedule();
}

/*
 * cal_sync_shut_down — stop.
 * Documented in cal_sync.h.
 */
void
cal_sync_shut_down(void)
{
    task_loop_timer_cancel(kick_timer);
    kick_timer = 0;
    live_app   = NULL;
    g_clear_pointer(&last_calendar, g_free);
}
