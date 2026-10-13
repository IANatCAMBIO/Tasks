/* ===========================================================================
 * backup.c — backups of the shared database (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "task_loop.h"
#include "task_paths.h"
#include "task_worker.h"             /* the shared periodic-pass scheduler  */
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * task_backup_dir() — see backup.h.
 * ------------------------------------------------------------------------- */
char *
task_backup_dir(TaskApp *app, char **why)
{
    char *chosen = NULL;

    *why = NULL;
    if (shared_settings_get(app->db->shared, SHARED_SETTING_BACKUP_DIR, &chosen,
                            why) < 0)
        return NULL;
    gboolean set = chosen != NULL && chosen[0] != '\0';
    gboolean live = set && task_path_is_live_data(chosen);
    if (live)
        *why = g_strdup_printf("The backup folder %s is inside the live Tasks "
                               "data directory, which this app never touches",
                               chosen);
    free(chosen);
    if (live)
        return NULL;
    return shared_db_backup_dir(app->db->shared, why);
}

/* ---------------------------------------------------------------------------
 * task_backup_set_dir() — see backup.h.
 * ------------------------------------------------------------------------- */
gboolean
task_backup_set_dir(TaskApp *app, const gchar *dir, char **why)
{
    *why = NULL;
    if (dir != NULL && task_path_is_live_data(dir)) {
        *why = g_strdup_printf("%s is inside the live Tasks data directory, "
                               "which this app never touches", dir);
        return FALSE;
    }
    return shared_db_set_backup_dir(app->db->shared, dir, why) == 0;
}

/* ---------------------------------------------------------------------------
 * The worker job.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp          *app;
    gchar            *db_path;       /* the database, copied for the thread */
    gboolean          periodic;      /* backup_if_due rather than Back Up Now */
    TaskBackupDoneFn  done;
    gpointer          user_data;
    /* filled by the thread, read on the main thread                       */
    gboolean          ok;
    gchar            *message;       /* NULL: a poll that found nothing due */
} BackupJob;

/*
 * backup_finish — the pass's main-thread tail (queued by the thread with
 * task_loop_main_async): report, release the in-flight guard, free the job.
 *
 * Inputs:
 *   data — the BackupJob
 */
static void
backup_finish(void *data)
{
    BackupJob *job = data;
    job->app->backup_running = FALSE;
    if (job->message != NULL)
        task_app_status(job->app, "%s", job->message);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
}

/*
 * backup_thread — one pass, on its own connection: shared_db_backup for
 * Back Up Now, shared_db_backup_if_due for the periodic poll.  The module
 * reserves a new file, copies with VACUUM INTO, verifies the copy and only
 * then prunes; it records the outcome in shared_settings.  A pass that
 * made its copy but failed to prune reports failure with both facts.
 *
 * Inputs:
 *   data — the BackupJob
 */
static void
backup_thread(void *data)
{
    BackupJob *job  = data;
    GError    *gerr = NULL;

    TaskDatabase *db = task_db_open_existing(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Backup failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        task_loop_main_async(backup_finish, job);
        return;
    }
    char *made = NULL;
    char *why  = NULL;
    int   rc   = job->periodic
        ? shared_db_backup_if_due(db->shared, time(NULL), &made, &why)
        : (shared_db_backup(db->shared, &made, &why) == 0 ? 1 : -1);
    task_db_close(db);

    /* A failure with a backup made is a copy whose pruning then failed:
     * the module's words name the new file AND the failure.              */
    job->ok = rc >= 0;
    if (rc > 0) {
        const char *slash = made != NULL ? strrchr(made, '/') : NULL;
        job->message = g_strdup_printf("Backed up to %s",
                                       slash != NULL ? slash + 1 : "?");
    } else if (rc < 0 && made != NULL && why != NULL) {
        job->message = g_strdup_printf("%c%s", g_ascii_toupper(why[0]), why + 1);
    } else if (rc < 0) {
        job->message = g_strdup_printf("Backup failed: %s",
                                       why != NULL ? why : "?");
    }
    free(made);
    free(why);
    task_loop_main_async(backup_finish, job);
}

/*
 * backup_launch — start one pass on a worker thread, after the refusals
 * task_backup_dir makes.
 *
 * Inputs:
 *   app       — the application context, its database open
 *   db_path   — the database to back up
 *   periodic  — TRUE for the schedule's poll, FALSE for Back Up Now
 *   done      — completion callback, or NULL
 *   user_data — passed to done
 */
static void
backup_launch(TaskApp *app, const gchar *db_path, gboolean periodic,
              TaskBackupDoneFn done, gpointer user_data)
{
    if (app->backup_running)
        return;                      /* a pass is already in flight         */

    char *why = NULL;
    char *dir = task_backup_dir(app, &why);
    if (dir == NULL) {
        /* The poll stays quiet: Settings shows the same reason, and a
         * message every minute would bury every other one.               */
        if (!periodic) {
            task_app_status(app, "%s", why != NULL ? why : "Backups unavailable");
            if (done != NULL)
                done(app, FALSE, why, user_data);
        }
        free(why);
        return;
    }
    free(dir);

    app->backup_running = TRUE;
    BackupJob *job = g_new0(BackupJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->periodic  = periodic;
    job->done      = done;
    job->user_data = user_data;
    task_loop_background(backup_thread, job);
}

/* task_backup_start() — see backup.h.                                      */
void
task_backup_start(TaskApp *app, TaskBackupDoneFn done, gpointer user_data)
{
    backup_launch(app, shared_db_file(app->db->shared), FALSE, done, user_data);
}

/*
 * backup_poll — the scheduler's pass: ask the module whether a backup is
 * due (and make it when it is).
 *
 * Inputs:
 *   app     — the application context
 *   db_path — the database the worker was armed with
 */
static void
backup_poll(TaskApp *app, const gchar *db_path)
{
    backup_launch(app, db_path, TRUE, NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * The periodic poll — the shared scheduler drives it (task_worker.h).  No
 * master switch or interval of its own: the shared policy is the module's
 * to apply, at every poll.  ARMED: one poll at launch, so a backup that
 * fell due while no app was running is made then.
 * ------------------------------------------------------------------------- */
static const TaskWorkerDef backup_worker = {
    .id               = "backup",
    .enabled_key      = NULL,
    .interval_key     = NULL,
    .period_min       = SHARED_BACKUP_POLL_MIN,
    .initial          = TASK_WORKER_INITIAL_ARMED,
    .running          = NULL,        /* completed by task_backup_init       */
    .timer            = NULL,
    .run              = backup_poll,
    .ready            = NULL,
};

static TaskWorkerDef backup_worker_live;

/* task_backup_init() — see backup.h.                                       */
void
task_backup_init(TaskApp *app)
{
    backup_worker_live         = backup_worker;
    backup_worker_live.running = &app->backup_running;
    backup_worker_live.timer   = &app->backup_timer;
    task_worker_register(&backup_worker_live);
}
