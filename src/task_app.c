#include "task_app.h"
#include "task_loop.h"

/* One entry per subscription.  `fn` is a TaskAppNotifyFn on the
 * changed/tasks lists and a TaskAppStatusFn on the status list. */
typedef struct {
    guint    id;
    gpointer fn;
    gpointer user_data;
} task_app_listener_t;

/*
 * task_app_new — an empty context.
 * Output is documented in task_app.h.
 */
TaskApp *
task_app_new(void)
{
    return g_new0(TaskApp, 1);
}

/*
 * task_app_free — close the database and free the context.
 * Inputs are documented in task_app.h.
 */
void
task_app_free(TaskApp *app)
{
    if (app == NULL) return;

    task_app_watch_stop(app);
    /* A listener still subscribed here outlived its window; not an error. */
    g_slist_free_full(app->changed_l, g_free);
    g_slist_free_full(app->tasks_l, g_free);
    g_slist_free_full(app->status_l, g_free);
    task_db_close(app->db);
    g_free(app);
}

/*
 * listener_add — append a subscription.
 *
 * Inputs:
 *   app       — the application context
 *   list      — the subscription list to add to
 *   fn        — the callback
 *   user_data — passed through to fn
 *
 * Output:
 *   the subscription id, or 0 when app or fn is NULL.
 */
static guint
listener_add(TaskApp *app, GSList **list, gpointer fn, gpointer user_data)
{
    if (app == NULL || fn == NULL) return 0;

    task_app_listener_t *l = g_new0(task_app_listener_t, 1);
    l->id        = ++app->listener_next;
    l->fn        = fn;
    l->user_data = user_data;
    *list = g_slist_append(*list, l);
    return l->id;
}

/*
 * task_app_listen_changed — subscribe to the full-refresh event.
 * Inputs/output are documented in task_app.h.
 */
guint
task_app_listen_changed(TaskApp *app, TaskAppNotifyFn fn, gpointer user_data)
{
    return listener_add(app, app != NULL ? &app->changed_l : NULL,
                        (gpointer)fn, user_data);
}

/*
 * task_app_listen_tasks — subscribe to the task-pane event.
 * Inputs/output are documented in task_app.h.
 */
guint
task_app_listen_tasks(TaskApp *app, TaskAppNotifyFn fn, gpointer user_data)
{
    return listener_add(app, app != NULL ? &app->tasks_l : NULL,
                        (gpointer)fn, user_data);
}

/*
 * task_app_listen_status — subscribe to status messages.
 * Inputs/output are documented in task_app.h.
 */
guint
task_app_listen_status(TaskApp *app, TaskAppStatusFn fn, gpointer user_data)
{
    return listener_add(app, app != NULL ? &app->status_l : NULL,
                        (gpointer)fn, user_data);
}

/*
 * unlisten_from — drop subscription `id` from one list.
 *
 * Inputs:
 *   list — the subscription list
 *   id   — the subscription to remove
 *
 * Output:
 *   TRUE when it was found and removed.
 */
static gboolean
unlisten_from(GSList **list, guint id)
{
    for (GSList *n = *list; n != NULL; n = n->next) {
        task_app_listener_t *l = n->data;
        if (l->id == id) {
            *list = g_slist_delete_link(*list, n);
            g_free(l);
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * task_app_unlisten — remove a subscription.
 * Inputs are documented in task_app.h.
 */
void
task_app_unlisten(TaskApp *app, guint id)
{
    if (app == NULL || id == 0) return;

    if (unlisten_from(&app->changed_l, id)) return;
    if (unlisten_from(&app->tasks_l, id)) return;
    unlisten_from(&app->status_l, id);
}

/*
 * listed — whether subscription `id` is still on `list`.
 *
 * Inputs:
 *   list — the live subscription list
 *   id   — the subscription to look for
 *
 * Output:
 *   TRUE when it is still subscribed.
 */
static gboolean
listed(GSList *list, guint id)
{
    for (GSList *n = list; n != NULL; n = n->next) {
        if (((task_app_listener_t *)n->data)->id == id) return TRUE;
    }
    return FALSE;
}

/*
 * fire — call every listener on one of the app's lists.
 *
 * A listener may unsubscribe itself or another while it runs (the library
 * window's refresh can close an editor), so the walk is over a SNAPSHOT of
 * the subscriptions taken by value — ids, callbacks and contexts — and each
 * is checked against the live list just before it is called.  Nothing in
 * the snapshot points at a subscription record, so one freed mid-fire is
 * skipped rather than read.
 *
 * Inputs:
 *   app     — the application context
 *   list    — address of the list to fire (re-read on every check, since a
 *             listener may change its head)
 *   message — non-NULL for status listeners; NULL for notify listeners
 */
static void
fire(TaskApp *app, GSList **list, const gchar *message)
{
    guint            count    = g_slist_length(*list);
    task_app_listener_t *snapshot = g_new(task_app_listener_t, count);
    guint            i        = 0;

    for (GSList *n = *list; n != NULL; n = n->next) {
        snapshot[i++] = *(task_app_listener_t *)n->data;
    }
    for (i = 0; i < count; i++) {
        if (!listed(*list, snapshot[i].id)) continue;   /* gone mid-fire */
        if (message != NULL) {
            ((TaskAppStatusFn)snapshot[i].fn)(app, message,
                                              snapshot[i].user_data);
        } else {
            ((TaskAppNotifyFn)snapshot[i].fn)(app, snapshot[i].user_data);
        }
    }
    g_free(snapshot);
}

/*
 * task_app_status — post an event message to every status listener.
 * Inputs are documented in task_app.h.
 */
void
task_app_status(TaskApp *app, const gchar *fmt, ...)
{
    if (app == NULL || app->status_l == NULL) return;

    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fire(app, &app->status_l, msg);
    g_free(msg);
}

/*
 * task_app_write_failed — report a write that failed, then refresh.
 * Inputs are documented in task_app.h.
 */
void
task_app_write_failed(TaskApp *app, const gchar *what)
{
    if (app == NULL) return;
    task_app_status(app, "%s was not saved: " TASK_APP_WRITE_FAILED_REASON, what);
    task_app_notify_changed(app);
}

/*
 * task_app_notify_changed — fire the full-refresh event.
 * Inputs are documented in task_app.h.
 */
void
task_app_notify_changed(TaskApp *app)
{
    if (app != NULL) fire(app, &app->changed_l, NULL);
}

/*
 * task_app_notify_tasks — fire the task-pane event, or the full one when
 * nothing listens for it.
 * Inputs are documented in task_app.h.
 */
void
task_app_notify_tasks(TaskApp *app)
{
    if (app == NULL) return;
    fire(app, app->tasks_l != NULL ? &app->tasks_l : &app->changed_l, NULL);
}

/*
 * task_app_check_outside — look for another connection's commits.
 * Inputs/output are documented in task_app.h.
 */
gboolean
task_app_check_outside(TaskApp *app)
{
    if (app == NULL || app->db == NULL) return FALSE;
    if (!task_db_outside_change(app->db)) return FALSE;

    task_app_notify_changed(app);
    return TRUE;
}

/*
 * watch_tick — the watch timer's callback.
 *
 * Inputs:
 *   data — the TaskApp
 */
static void
watch_tick(void *data)
{
    task_app_check_outside(data);
}

/*
 * task_app_watch_start — poll for outside changes on the main loop.
 * Inputs are documented in task_app.h.
 */
void
task_app_watch_start(TaskApp *app)
{
    task_app_watch_stop(app);
    app->watch_timer = task_loop_timer_every(SHARED_DB_POLL_SECONDS, watch_tick, app, NULL);
}

/*
 * task_app_watch_stop — cancel the outside-change watch.
 * Inputs are documented in task_app.h.
 */
void
task_app_watch_stop(TaskApp *app)
{
    if (app == NULL) return;
    task_loop_timer_cancel(app->watch_timer);
    app->watch_timer = 0;
}
