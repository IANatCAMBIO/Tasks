/* ===========================================================================
 * gcal_sync.c — the day planner's sync with Google Calendar (gcal_sync.h)
 * =========================================================================== */

#include "gcal_sync.h"
#include "gcal_api.h"
#include "gcal_plan.h"
#include "gcal_store.h"
#include "oauth.h"
#include "task_config.h"
#include "task_dates.h"
#include "task_loop.h"
#include "task_worker.h"

#include <string.h>
#include <time.h>

/* Seconds a kick waits, so a burst of edits makes one pass. */
static const double kick_delay_s = 2.0;

/* Bounds of an adopted session, in minutes (a remote move cannot make a
 * session the planner could not draw). */
static const gint adopt_min_minutes = 15;
static const gint adopt_max_minutes = 24 * 60;

/* ── State (main thread) ──────────────────────────────────────────────── */

static TaskApp  *live_app;           /* NULL once shut down                 */
static gboolean  pass_running;
static gboolean  pass_again;         /* a request arrived during the pass   */
static gboolean  again_manual;       /* ... and it was the user's           */
static guint     worker_timer;
static unsigned  kick_timer;
static gboolean  notifying;          /* our own task event is firing        */
static gchar    *last_status;        /* gcal_sync_status's line             */

/* One pass: what the main thread hands the background thread, and what
 * comes back. */
typedef struct {
    gchar      *db_path;
    gchar      *push_pref;           /* the setting, or NULL                */
    gchar     **show_pref;           /* the setting split, or NULL          */
    gint64      lo, hi;              /* the pull window                     */
    gboolean    manual;
    /* results */
    gchar      *error;               /* the first failure, or NULL          */
    guint       pushed;              /* events created, moved or written    */
    guint       removed;             /* events deleted                      */
    guint       adopted;             /* sessions changed from Google's side */
} pass_job_t;

/*
 * pass_job_free — free a job.
 *
 * Inputs:
 *   job — the job
 */
static void
pass_job_free(pass_job_t *job)
{
    g_free(job->db_path);
    g_free(job->push_pref);
    g_strfreev(job->show_pref);
    g_free(job->error);
    g_free(job);
}

/*
 * note_error — keep a pass's first failure (later ones are usually the
 * same cause again).
 *
 * Inputs:
 *   job — the pass
 *   err — the reason, owned (freed here when not kept)
 */
static void
note_error(pass_job_t *job, gchar *err)
{
    if (err == NULL) return;
    if (job->error == NULL) {
        job->error = err;
        return;
    }
    g_free(err);
}

/* ── The pass (background thread) ─────────────────────────────────────── */

/* What one pass works with, beyond the job. */
typedef struct {
    pass_job_t   *job;
    TaskDatabase *db;
    const gchar  *token;
    const gchar  *push_id;           /* the calendar sessions go to         */
    GHashTable   *fetched;           /* calendar id → itself: read OK       */
    GHashTable   *ours;              /* event id → gcal_remote_t* (borrowed)*/
    GHashTable   *known;             /* calendar id → itself: still exists  */
} pass_ctx_t;

/*
 * pick_calendars — resolve the settings against the calendar list: the
 * push calendar (the setting when it names a writable calendar, else the
 * primary) and the shown calendars (the setting's that still exist, else
 * the primary).
 *
 * Inputs:
 *   job     — the pass (its settings)
 *   cals    — gcal_calendar_t* (borrowed)
 *   primary — the primary calendar's id
 *   show    — out: gchar* ids, borrowed from cals or primary
 *
 * Output:
 *   the push calendar's id, borrowed likewise.
 */
static const gchar *
pick_calendars(pass_job_t *job, GPtrArray *cals, const gchar *primary, GPtrArray *show)
{
    const gchar *push = primary;
    for (guint i = 0; i < cals->len; i++) {
        const gcal_calendar_t *c = g_ptr_array_index(cals, i);
        if (job->push_pref != NULL && c->writable && strcmp(c->id, job->push_pref) == 0)
            push = c->id;
        if (job->show_pref != NULL && g_strv_contains((const gchar *const *)job->show_pref, c->id))
            g_ptr_array_add(show, c->id);
    }
    if (job->show_pref == NULL) g_ptr_array_add(show, (gpointer)primary);
    return push;
}

/*
 * pull — read every calendar the pass needs: the shown ones into the
 * cache, and the push calendar for the planner's own events.
 *
 * Inputs:
 *   ctx   — the pass
 *   show  — the shown calendars' ids
 *   keep  — out: gcal_remote_t* arrays to free after the push (they own
 *           what ctx->ours points at)
 */
static void
pull(pass_ctx_t *ctx, GPtrArray *show, GPtrArray *keep)
{
    GPtrArray *wanted = g_ptr_array_new();
    for (guint i = 0; i < show->len; i++) g_ptr_array_add(wanted, g_ptr_array_index(show, i));
    if (!g_ptr_array_find_with_equal_func(wanted, ctx->push_id, g_str_equal, NULL))
        g_ptr_array_add(wanted, (gpointer)ctx->push_id);

    for (guint i = 0; i < wanted->len; i++) {
        const gchar *cal    = g_ptr_array_index(wanted, i);
        GPtrArray   *events = NULL;
        gchar       *err    = NULL;
        if (gcal_api_events(ctx->token, cal, ctx->job->lo, ctx->job->hi, &events, &err)
            != GCAL_OK) {
            note_error(ctx->job, err);
            continue;
        }
        g_hash_table_add(ctx->fetched, (gpointer)cal);
        g_ptr_array_add(keep, events);

        gboolean   shown = g_ptr_array_find_with_equal_func(show, cal, g_str_equal, NULL);
        GPtrArray *cache = g_ptr_array_new_with_free_func((GDestroyNotify)gcal_event_free);
        for (guint j = 0; j < events->len; j++) {
            gcal_remote_t *r = g_ptr_array_index(events, j);
            if (r->task_id != 0) {
                g_hash_table_insert(ctx->ours, r->event_id, r);
            } else if (shown && !r->cancelled && !r->declined) {
                gcal_event_t *e = g_new0(gcal_event_t, 1);
                e->event_id = g_strdup(r->event_id);
                e->title    = g_strdup(r->title);
                e->start    = r->start;
                e->end      = r->end;
                e->all_day  = r->all_day;
                g_ptr_array_add(cache, e);
            }
        }
        if (shown) gcal_store_set_events(ctx->db, cal, cache);
        g_ptr_array_free(cache, TRUE);
    }
    gcal_store_keep_events(ctx->db, show);
    g_ptr_array_free(wanted, TRUE);
}

/*
 * look_up — what is known of a binding's event.  The pull's listing is
 * the first answer; an event missing from a listing that should hold it
 * (its calendar was read and its session lies inside the window) is
 * fetched by id, because only Google's own answer proves a deletion.
 * Anything else stays UNKNOWN.
 *
 * Inputs:
 *   ctx  — the pass
 *   b    — the binding
 *   slot — out: the event's slot when PRESENT
 *
 * Output:
 *   the state.
 */
static gcal_event_state_t
look_up(pass_ctx_t *ctx, const gcal_binding_t *b, gcal_slot_t *slot)
{
    const gcal_remote_t *r       = g_hash_table_lookup(ctx->ours, b->event_id);
    gcal_remote_t       *fetched = NULL;

    if (r == NULL) {
        gint64 end = b->pushed_start + (gint64)b->pushed_duration * 60;
        if (!g_hash_table_contains(ctx->fetched, b->calendar_id) ||
            b->pushed_start >= ctx->job->hi || end <= ctx->job->lo)
            return GCAL_EVENT_UNKNOWN;

        gchar        *err = NULL;
        gcal_result_t res = gcal_api_get_event(ctx->token, b->calendar_id, b->event_id,
                                               &fetched, &err);
        if (res == GCAL_GONE) return GCAL_EVENT_MISSING;
        if (res != GCAL_OK) {
            note_error(ctx->job, err);
            return GCAL_EVENT_UNKNOWN;
        }
        r = fetched;
    }

    gcal_event_state_t state;
    if (r->cancelled) {
        state = GCAL_EVENT_MISSING;
    } else if (r->all_day) {
        state = GCAL_EVENT_UNKNOWN;      /* made all-day there: nothing to adopt */
    } else {
        state          = GCAL_EVENT_PRESENT;
        slot->start    = r->start;
        slot->duration = (gint)((r->end - r->start) / 60);
        slot->title    = NULL;
    }
    gcal_remote_free(fetched);
    return state;
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
    if (gcal_api_insert(ctx->token, ctx->push_id, t->id, t->scheduled_start,
                        t->scheduled_duration, t->title, &event_id, &err) != GCAL_OK) {
        note_error(ctx->job, err);
        return;
    }
    gcal_binding_t b = { t->id, (gchar *)ctx->push_id, event_id, t->scheduled_start,
                         t->scheduled_duration, t->title };
    gcal_store_bind(ctx->db, &b);
    g_free(event_id);
    ctx->job->pushed++;
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
apply(pass_ctx_t *ctx, gcal_action_t act, const gcal_binding_t *b, const Task *t,
      const gcal_slot_t *ev)
{
    gchar        *err = NULL;
    gcal_result_t res;

    switch (act) {
    case GCAL_ACT_NONE:
        break;
    case GCAL_ACT_CREATE:
        push_new(ctx, t);
        break;
    case GCAL_ACT_MOVE:
        /* The old calendar may be gone with the event; only a failure
         * that leaves the event standing stops the move. */
        res = gcal_api_delete(ctx->token, b->calendar_id, b->event_id, &err);
        if (res == GCAL_FAILED && g_hash_table_contains(ctx->known, b->calendar_id)) {
            note_error(ctx->job, err);
            break;
        }
        g_free(err);
        push_new(ctx, t);
        break;
    case GCAL_ACT_UPDATE:
        res = gcal_api_patch(ctx->token, b->calendar_id, b->event_id, t->id,
                             t->scheduled_start, t->scheduled_duration, t->title, &err);
        if (res == GCAL_GONE) {
            push_new(ctx, t);
        } else if (res == GCAL_OK) {
            gcal_binding_t nb = *b;
            nb.pushed_start    = t->scheduled_start;
            nb.pushed_duration = t->scheduled_duration;
            nb.pushed_title    = t->title;
            gcal_store_bind(ctx->db, &nb);
            ctx->job->pushed++;
        } else {
            note_error(ctx->job, err);
        }
        break;
    case GCAL_ACT_REMOVE:
        res = gcal_api_delete(ctx->token, b->calendar_id, b->event_id, &err);
        if (res == GCAL_FAILED) {
            note_error(ctx->job, err);
            break;
        }
        gcal_store_unbind(ctx->db, b->task_id);
        ctx->job->removed++;
        break;
    case GCAL_ACT_FORGET:
        gcal_store_unbind(ctx->db, b->task_id);
        break;
    case GCAL_ACT_ADOPT: {
        gint dur = CLAMP(ev->duration, adopt_min_minutes, adopt_max_minutes);
        if (gcal_store_reschedule(ctx->db, t->id, t->scheduled_start, t->scheduled_duration,
                                  ev->start, dur)) {
            gcal_binding_t nb = *b;
            nb.pushed_start    = ev->start;
            nb.pushed_duration = dur;
            nb.pushed_title    = t->title;
            gcal_store_bind(ctx->db, &nb);
            ctx->job->adopted++;
        }
        break;
    }
    case GCAL_ACT_UNSCHEDULE:
        if (gcal_store_reschedule(ctx->db, t->id, t->scheduled_start, t->scheduled_duration,
                                  0, t->scheduled_duration)) {
            gcal_store_unbind(ctx->db, t->id);
            ctx->job->adopted++;
        }
        break;
    }
}

/*
 * push — every scheduled task and every binding through gcal_plan.
 *
 * Inputs:
 *   ctx — the pass
 */
static void
push(pass_ctx_t *ctx)
{
    GPtrArray  *tasks    = task_db_tasks_scheduled_for_day(ctx->db, 1, G_MAXINT64);
    GPtrArray  *bindings = gcal_store_bindings(ctx->db);
    GHashTable *by_id    = g_hash_table_new(g_int64_hash, g_int64_equal);
    GHashTable *bound    = g_hash_table_new(g_int64_hash, g_int64_equal);

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        g_hash_table_insert(by_id, &t->id, t);
    }

    for (guint i = 0; i < bindings->len; i++) {
        gcal_binding_t *b = g_ptr_array_index(bindings, i);
        const Task     *t = g_hash_table_lookup(by_id, &b->task_id);
        g_hash_table_add(bound, &b->task_id);

        gcal_slot_t pushed = { b->pushed_start, b->pushed_duration, b->pushed_title };
        gcal_slot_t now    = { 0 };
        if (t != NULL) now = (gcal_slot_t){ t->scheduled_start, t->scheduled_duration, t->title };
        gcal_slot_t        ev    = { 0 };
        gcal_event_state_t state = look_up(ctx, b, &ev);
        gboolean misplaced = strcmp(b->calendar_id, ctx->push_id) != 0;

        apply(ctx, gcal_plan(&pushed, t != NULL ? &now : NULL, state, &ev, misplaced),
              b, t, &ev);
    }

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        if (!g_hash_table_contains(bound, &t->id)) apply(ctx, GCAL_ACT_CREATE, NULL, t, NULL);
    }

    g_hash_table_destroy(bound);
    g_hash_table_destroy(by_id);
    g_ptr_array_free(bindings, TRUE);
    task_ptr_array_free_tasks(tasks);
}

static void pass_done(void *data);

/*
 * pass_run — one pass, on the background thread (gcal_sync.h).
 *
 * Inputs:
 *   data — the pass_job_t, handed back to the main thread
 */
static void
pass_run(void *data)
{
    pass_job_t *job = data;
    gchar      *err = NULL;

    gchar *token = task_oauth_access_token(&err);
    if (token == NULL) {
        note_error(job, err);
        task_loop_main_async(pass_done, job);
        return;
    }

    GError       *gerr = NULL;
    TaskDatabase *db   = task_db_open(job->db_path, &gerr);
    if (db == NULL) {
        note_error(job, g_strdup(gerr != NULL ? gerr->message : "cannot open the database"));
        g_clear_error(&gerr);
        g_free(token);
        task_loop_main_async(pass_done, job);
        return;
    }

    GPtrArray *cals = NULL;
    if (gcal_api_calendars(token, &cals, &err) != GCAL_OK) {
        note_error(job, err);
    } else {
        gcal_store_set_calendars(db, cals);

        const gchar *primary = "primary";
        pass_ctx_t   ctx     = { job, db, token, NULL,
                                 g_hash_table_new(g_str_hash, g_str_equal),
                                 g_hash_table_new(g_str_hash, g_str_equal),
                                 g_hash_table_new(g_str_hash, g_str_equal) };
        for (guint i = 0; i < cals->len; i++) {
            const gcal_calendar_t *c = g_ptr_array_index(cals, i);
            if (c->is_primary) primary = c->id;
            g_hash_table_add(ctx.known, c->id);
        }

        /* Bindings made in another account mean nothing in this one. */
        gchar *account = gcal_store_account(db);
        if (account != NULL && strcmp(account, primary) != 0) gcal_store_unbind(db, 0);
        g_free(account);
        gcal_store_set_account(db, primary);

        GPtrArray *show = g_ptr_array_new();
        GPtrArray *keep = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
        ctx.push_id = pick_calendars(job, cals, primary, show);
        pull(&ctx, show, keep);
        push(&ctx);

        g_ptr_array_free(keep, TRUE);
        g_ptr_array_free(show, TRUE);
        g_hash_table_destroy(ctx.fetched);
        g_hash_table_destroy(ctx.ours);
        g_hash_table_destroy(ctx.known);
        g_ptr_array_free(cals, TRUE);
    }

    task_db_close(db);
    g_free(token);
    task_loop_main_async(pass_done, job);
}

/* ── Main thread ──────────────────────────────────────────────────────── */

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
 * pass_start — hand a pass to the background thread, or note that one
 * is wanted when one is already running.
 *
 * Inputs:
 *   manual — whether the user asked for it
 */
static void
pass_start(gboolean manual)
{
    if (live_app == NULL || live_app->db == NULL || !gcal_sync_active()) return;
    if (pass_running) {
        pass_again    = TRUE;
        again_manual |= manual;
        return;
    }

    gint64 lo, hi, ignore;
    task_day_bounds(-GCAL_SYNC_PAST_DAYS, &lo, &ignore);
    task_day_bounds(GCAL_SYNC_AHEAD_DAYS, &ignore, &hi);

    pass_job_t *job = g_new0(pass_job_t, 1);
    job->db_path    = g_strdup(shared_db_file(live_app->db->shared));
    job->push_pref  = task_config_get(TASK_CFG_GCAL_PUSH_CALENDAR);
    gchar *show     = task_config_get(TASK_CFG_GCAL_SHOW_CALENDARS);
    job->show_pref  = show != NULL ? g_strsplit(show, ",", -1) : NULL;
    g_free(show);
    job->lo         = lo;
    job->hi         = hi;
    job->manual     = manual;

    pass_running = TRUE;
    task_loop_background(pass_run, job);
}

/*
 * pass_done — main thread: report the pass, redraw, and run the pass
 * that was asked for while this one ran.
 *
 * Inputs:
 *   data — the pass_job_t, freed here
 */
static void
pass_done(void *data)
{
    pass_job_t *job = data;
    pass_running = FALSE;

    if (live_app == NULL) {
        pass_job_free(job);
        return;
    }

    g_free(last_status);
    if (job->error != NULL) {
        last_status = g_strdup_printf("Last sync failed: %s", job->error);
        task_app_status(live_app, "Google Calendar: %s", job->error);
    } else {
        time_t    now = time(NULL);
        struct tm local;
        char      clock[16];
        localtime_r(&now, &local);
        strftime(clock, sizeof clock, "%-I:%M %p", &local);
        last_status = g_strdup_printf("Last synced at %s", clock);
        if (job->manual)
            task_app_status(live_app, "Google Calendar synced: %u sent, %u removed, "
                            "%u changed from Google", job->pushed, job->removed, job->adopted);
    }
    pass_job_free(job);
    notify_tasks();

    if (pass_again) {
        gboolean manual = again_manual;
        pass_again   = FALSE;
        again_manual = FALSE;
        pass_start(manual);
    }
}

/*
 * kick_fire — the kick's delay ran out: start the pass.
 *
 * Inputs:
 *   data — unused
 */
static void
kick_fire(void *data)
{
    (void)data;
    kick_timer = 0;
    pass_start(FALSE);
}

/*
 * gcal_sync_kick — start a pass shortly when there is anything to push.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_kick(void)
{
    if (live_app == NULL || live_app->db == NULL || !gcal_sync_active()) return;
    if (!gcal_store_dirty(live_app->db)) return;
    task_loop_timer_cancel(kick_timer);
    kick_timer = task_loop_timer_once(kick_delay_s, kick_fire, NULL, NULL);
}

/*
 * gcal_sync_now — start a pass now.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_now(gboolean manual)
{
    task_loop_timer_cancel(kick_timer);
    kick_timer = 0;
    pass_start(manual);
}

/*
 * gcal_sync_active — the setting is on and the user is signed in.
 * Documented in gcal_sync.h.
 */
gboolean
gcal_sync_active(void)
{
    return task_config_get_bool(TASK_CFG_GCAL_ENABLED) && task_oauth_authenticated();
}

/*
 * gcal_sync_signed_out — forget what was pulled.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_signed_out(void)
{
    if (live_app == NULL || live_app->db == NULL) return;

    GPtrArray *none = g_ptr_array_new();
    gcal_store_keep_events(live_app->db, none);
    gcal_store_set_calendars(live_app->db, none);
    g_ptr_array_free(none, TRUE);
    g_clear_pointer(&last_status, g_free);
    notify_tasks();
}

/*
 * gcal_sync_status — the line on the last pass.
 * Documented in gcal_sync.h.
 */
gchar *
gcal_sync_status(void)
{
    if (pass_running) return g_strdup("Syncing…");
    return g_strdup(last_status != NULL ? last_status : "Not synced yet");
}

/*
 * on_task_event — a task changed somewhere (an edit, a delete, a move):
 * kick, unless the event is the one a finished pass fires itself.
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
    if (!notifying) gcal_sync_kick();
}

/*
 * worker_run — the worker timer's pass.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   db_path — the database path (unused: the pass takes the live one)
 */
static void
worker_run(TaskApp *app, const gchar *db_path)
{
    (void)app;
    (void)db_path;
    pass_start(FALSE);
}

/*
 * worker_ready — may the timer's pass run: signed in, and a database
 * open.
 *
 * Inputs:
 *   app — the application context
 *
 * Output:
 *   TRUE when it may.
 */
static gboolean
worker_ready(TaskApp *app)
{
    return app->db != NULL && task_oauth_authenticated();
}

static const TaskWorkerDef gcal_worker = {
    .id           = "gcal",
    .sort         = 10,                 /* after the recurrence pass (sort 0),
                                         * whose results it pushes          */
    .enabled_key  = TASK_CFG_GCAL_ENABLED,
    .interval_key = TASK_CFG_GCAL_INTERVAL_MIN,
    .initial      = TASK_WORKER_INITIAL_ARMED,
    .running      = &pass_running,
    .timer        = &worker_timer,
    .run          = worker_run,
    .ready        = worker_ready,
};

/*
 * gcal_sync_settings_changed — re-arm and redraw.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_settings_changed(void)
{
    if (live_app == NULL || live_app->db == NULL) return;
    task_worker_arm(live_app, &gcal_worker, shared_db_file(live_app->db->shared));
    notify_tasks();
}

/*
 * gcal_sync_init — register the worker and the task listeners.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_init(TaskApp *app)
{
    live_app = app;
    task_oauth_init();
    task_worker_register(&gcal_worker);
    task_app_listen_tasks(app, on_task_event, NULL);
    task_app_listen_changed(app, on_task_event, NULL);
}

/*
 * gcal_sync_shut_down — stop reporting.
 * Documented in gcal_sync.h.
 */
void
gcal_sync_shut_down(void)
{
    task_loop_timer_cancel(kick_timer);
    kick_timer = 0;
    live_app   = NULL;
}
