/* ===========================================================================
 * backup.c — backups of the shared database (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "task_loop.h"
#include "task_paths.h"
#include "task_worker.h"             /* the shared periodic-pass scheduler  */
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * task_backup_setting() — see backup.h.
 * ------------------------------------------------------------------------- */
gint64
task_backup_setting(TaskApp *app, const gchar *key)
{
    gint64 def = 0;
    if (strcmp(key, SHARED_SETTING_BACKUP_ENABLED) == 0)
        def = SHARED_BACKUP_ENABLED_DEFAULT;
    else if (strcmp(key, SHARED_SETTING_BACKUP_INTERVAL_MIN) == 0)
        def = SHARED_BACKUP_INTERVAL_MIN_DEFAULT;
    else if (strcmp(key, SHARED_SETTING_BACKUP_KEEP) == 0)
        def = SHARED_BACKUP_KEEP_DEFAULT;

    int64_t value = def;
    if (shared_settings_get_int(app->db->shared, key, def, &value, NULL) != 0)
        value = def;
    if (strcmp(key, SHARED_SETTING_BACKUP_KEEP) == 0)
        value = CLAMP(value, SHARED_BACKUP_KEEP_MIN, SHARED_BACKUP_KEEP_MAX);
    return value;
}

/* ---------------------------------------------------------------------------
 * task_backup_set_setting() — see backup.h.
 * ------------------------------------------------------------------------- */
void
task_backup_set_setting(TaskApp *app, const gchar *key, gint64 value)
{
    char  text[32];
    char *why = NULL;

    snprintf(text, sizeof text, "%lld", (long long)value);
    if (shared_settings_set(app->db->shared, key, text, &why) != 0) {
        g_warning("backup: cannot store %s: %s", key, why != NULL ? why : "?");
        free(why);
    }
}

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
    if (dir == NULL || dir[0] != '/') {
        *why = g_strdup("The backup folder must be an absolute path");
    } else if (task_path_is_live_data(dir)) {
        *why = g_strdup_printf("%s is inside the live Tasks data directory, "
                               "which this app never touches", dir);
    } else if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        *why = g_strdup_printf("%s is not a folder", dir);
    } else if (access(dir, W_OK) != 0) {
        *why = g_strdup_printf("%s is not writable", dir);
    } else {
        return shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_DIR,
                                   dir, why) == 0;
    }
    return FALSE;
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
 * then prunes; it records the outcome in shared_settings.
 *
 * Inputs:
 *   data — the BackupJob
 */
static void
backup_thread(void *data)
{
    BackupJob *job  = data;
    GError    *gerr = NULL;

    TaskDatabase *db = task_db_open(job->db_path, &gerr);
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

    job->ok = rc >= 0;
    if (rc > 0) {
        const char *slash = made != NULL ? strrchr(made, '/') : NULL;
        job->message = g_strdup_printf("Backed up to %s",
                                       slash != NULL ? slash + 1 : "?");
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
    .period_min       = TASK_BACKUP_POLL_MIN,
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
