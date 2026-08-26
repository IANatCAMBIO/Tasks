/* ===========================================================================
 * task_worker.c — the shared periodic-pass scheduler (see task_worker.h)
 * =========================================================================== */

#include "task_worker.h"
#include <stdlib.h>

static GSList *workers = NULL;       /* const TaskWorkerDef*, in order      */

void
task_worker_register(const TaskWorkerDef *def)
{
    if (def == NULL || def->run == NULL || def->timer == NULL)
        return;
    workers = g_slist_append(workers, (gpointer)def);
}

/* ---------------------------------------------------------------------------
 * The timer payload.  It owns its copy of the database path: the timer
 * outlives whatever string armed it, and the file can move underneath a
 * running app.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp             *app;
    const TaskWorkerDef *def;
    gchar               *db_path;
} Tick;

/* tick_free() — GDestroyNotify for the payload.  Re-arming on every
 * Settings change would otherwise leak the previous one.                   */
static void
tick_free(gpointer data)
{
    Tick *t = data;
    g_free(t->db_path);
    g_free(t);
}

/* tick_cb() — one scheduled pass.                                          */
static gboolean
tick_cb(gpointer data)
{
    Tick *t = data;
    const TaskWorkerDef *def = t->def;
    if (def->running != NULL && *def->running)
        return G_SOURCE_CONTINUE;    /* previous pass still in flight       */
    if (def->ready != NULL && !def->ready(t->app))
        return G_SOURCE_CONTINUE;
    def->run(t->app, t->db_path);
    return G_SOURCE_CONTINUE;
}

/* interval_minutes() — the worker's period from config, or its default.
 *
 * A negative value from a hand-edited ini folds to 0 (manual only)
 * rather than through to the arming arithmetic, where the cast to guint
 * would turn it into a period of some decades.                            */
static gint
interval_minutes(const TaskWorkerDef *def)
{
    if (def->interval_key == NULL)
        return def->interval_default;
    gchar *v = task_app_config_get(def->interval_key);
    gint minutes = v != NULL ? atoi(v) : def->interval_default;
    g_free(v);
    return minutes < 0 ? 0 : minutes;
}

/* ---------------------------------------------------------------------------
 * task_worker_arm() — disarm, then re-arm from config (see task_worker.h).
 * ------------------------------------------------------------------------- */
void
task_worker_arm(TaskApp *app, const TaskWorkerDef *def, const gchar *db_path)
{
    if (app == NULL || def == NULL)
        return;

    if (*def->timer != 0) {
        g_source_remove(*def->timer);   /* also frees the old payload       */
        *def->timer = 0;
    }

    if (def->enabled_key != NULL &&
        !task_app_config_get_bool(def->enabled_key, def->enabled_default))
        return;                      /* switched off: no timer, no pass     */

    if (def->on_arm != NULL)
        def->on_arm(app);

    gint minutes = interval_minutes(def);
    if (minutes > 0) {
        Tick *t = g_new0(Tick, 1);
        t->app     = app;
        t->def     = def;
        t->db_path = g_strdup(db_path);
        *def->timer = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                                 (guint)(minutes * 60),
                                                 tick_cb, t, tick_free);
    }

    /* The initial pass.  ARMED deliberately does nothing at interval 0:
     * that setting means "manual only", and running a pass anyway would
     * contradict it.  ALWAYS overrides that for a worker whose view is
     * empty until it has run once.                                        */
    gboolean initial = def->initial == TASK_WORKER_INITIAL_ALWAYS ||
                       (def->initial == TASK_WORKER_INITIAL_ARMED &&
                        minutes > 0);
    if (initial && (def->ready == NULL || def->ready(app)))
        def->run(app, db_path);
}

/* ---------------------------------------------------------------------------
 * task_worker_arm_all() — every worker, one call (see task_worker.h).
 * ------------------------------------------------------------------------- */
void
task_worker_arm_all(TaskApp *app, const gchar *db_path)
{
    for (GSList *n = workers; n != NULL; n = n->next)
        task_worker_arm(app, n->data, db_path);
}
