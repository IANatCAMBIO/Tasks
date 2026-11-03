/* ===========================================================================
 * cleanup.c — the completed-task cleanup pass (see cleanup.h)
 * =========================================================================== */

#include "cleanup.h"
#include "recur.h"
#include "task_config.h"
#include "task_worker.h"

#include <time.h>

/* The scheduler's timer for the pass; owned by the worker below.           */
static guint cleanup_timer;

/* The units offered, in the order Settings shows them.  Days, months and
 * years are the ones a retention is said in; the enum they come from is
 * the recurrence editor's, so there is one vocabulary of intervals in the
 * app rather than two spellings of the same three words. */
static const TaskRecurUnit cleanup_units[TASK_CLEANUP_N_UNITS] = {
    TASK_RECUR_DAY, TASK_RECUR_MONTH, TASK_RECUR_YEAR
};

/* task_cleanup_units() — see cleanup.h.                                    */
const TaskRecurUnit *
task_cleanup_units(void)
{
    return cleanup_units;
}

/* task_cleanup_policy() — the stored retention, clamped (see cleanup.h).   */
void
task_cleanup_policy(gint *after, TaskRecurUnit *unit)
{
    if (after != NULL) {
        gint n = task_config_get_int(TASK_CFG_CLEANUP_AFTER);
        *after = CLAMP(n, TASK_CLEANUP_AFTER_MIN, TASK_CLEANUP_AFTER_MAX);
    }
    if (unit != NULL) {
        gint stored = task_config_get_int(TASK_CFG_CLEANUP_UNIT);
        *unit = TASK_RECUR_MONTH;
        for (guint i = 0; i < TASK_CLEANUP_N_UNITS; i++) {
            if (stored == (gint)cleanup_units[i]) {
                *unit = cleanup_units[i];
                break;
            }
        }
    }
}

/* task_cleanup_policy_set() — store the retention (see cleanup.h).         */
void
task_cleanup_policy_set(gint after, TaskRecurUnit unit)
{
    gboolean known = FALSE;
    for (guint i = 0; i < TASK_CLEANUP_N_UNITS; i++)
        known = known || unit == cleanup_units[i];

    task_config_set_int(TASK_CFG_CLEANUP_AFTER,
                        CLAMP(after, TASK_CLEANUP_AFTER_MIN, TASK_CLEANUP_AFTER_MAX));
    task_config_set_int(TASK_CFG_CLEANUP_UNIT, known ? (gint)unit : TASK_RECUR_MONTH);
}

/* task_cleanup_cutoff() — now less the retention, by the calendar (see
 * cleanup.h).                                                              */
gint64
task_cleanup_cutoff(void)
{
    gint          after;
    TaskRecurUnit unit;

    task_cleanup_policy(&after, &unit);
    /* The negative interval is the step BACK; task_recur_advance owns the
     * calendar arithmetic for both directions (recur.h).  -1: the cutoff
     * is an instant, not an occurrence, so no time of day is re-applied. */
    return task_recur_advance((gint64)time(NULL), unit, -after, -1);
}

/* task_cleanup_pending() — what a pass would take now (see cleanup.h).     */
guint
task_cleanup_pending(TaskApp *app)
{
    if (app == NULL || app->db == NULL)
        return 0;
    return task_db_tasks_done_before(app->db, task_cleanup_cutoff());
}

/* task_cleanup_run() — one pass (see cleanup.h).                           */
guint
task_cleanup_run(TaskApp *app, guint *tombstoned)
{
    if (tombstoned != NULL) *tombstoned = 0;
    if (app == NULL || app->db == NULL)
        return 0;

    guint linked = 0;
    guint purged = task_db_tasks_purge_done(app->db, task_cleanup_cutoff(), &linked);
    guint total  = purged + linked;

    if (tombstoned != NULL) *tombstoned = linked;
    if (total == 0)
        return purged;               /* nothing went: nothing to say        */

    /* The FULL refresh: a tag whose last task went has left the sidebar
     * with it. */
    task_app_notify_changed(app);
    if (linked == 0)
        task_app_status(app, "Deleted %u completed task%s", total,
                        total == 1 ? "" : "s");
    else
        task_app_status(app,
                        "Deleted %u completed task%s; %u came from a note, "
                        "whose text is untouched",
                        total, total == 1 ? "" : "s", linked);
    return purged;
}

/*
 * cleanup_enabled — the worker's `ready`: may a scheduled pass run?  The
 * switch is read HERE, per pass, rather than through the worker's
 * enabled_key, which is read when the worker is armed: a pass asked for
 * once a day costs one query, and reading the switch this way is what lets
 * Settings turn cleanup on or off with nothing to re-arm.
 *
 * Inputs:
 *   app — the context (unused: the switch is config, not state)
 *
 * Output:
 *   TRUE when automatic cleanup is switched on.
 */
static gboolean
cleanup_enabled(TaskApp *app)
{
    (void)app;
    return task_config_get_bool(TASK_CFG_CLEANUP_ENABLED);
}

/*
 * cleanup_tick — the scheduler's `run`: one pass against the app's own
 * connection.
 *
 * Inputs:
 *   app     — the context
 *   db_path — the database the worker was armed with (unused: the pass
 *             runs on the app's open connection, as the recurrence pass
 *             does, not on one of its own)
 */
static void
cleanup_tick(TaskApp *app, const gchar *db_path)
{
    (void)db_path;
    task_cleanup_run(app, NULL);
}

/* ---------------------------------------------------------------------------
 * The worker.  LAST of them (`sort`): the recurrence pass (-20) reopens a
 * completed recurring task before its next occurrence, and a pass that ran
 * first would be judging a task that is about to stop being completed.
 * The integrations in between get to push what they have before the rows
 * go.
 *
 * No enabled_key: see cleanup_enabled.  A FIXED period rather than a
 * config one — the cutoff moves by days at the fastest, so there is
 * nothing a shorter cadence could find, and ARMED gives the one pass at
 * launch that catches up after the app has been closed.
 * ------------------------------------------------------------------------- */
#define CLEANUP_POLL_MIN (24 * 60)

static const TaskWorkerDef cleanup_worker = {
    .id               = "cleanup",
    .sort             = 20,
    .enabled_key      = NULL,
    .interval_key     = NULL,
    .period_min       = CLEANUP_POLL_MIN,
    .initial          = TASK_WORKER_INITIAL_ARMED,
    .running          = NULL,        /* synchronous on the main thread: a
                                      * tick cannot land on one in flight  */
    .timer            = &cleanup_timer,
    .run              = cleanup_tick,
    .ready            = cleanup_enabled,
};

/* task_cleanup_init() — see cleanup.h.                                     */
void
task_cleanup_init(TaskApp *app)
{
    (void)app;                       /* nothing of the app's is borrowed    */
    task_worker_register(&cleanup_worker);
}
