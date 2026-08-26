/* ===========================================================================
 * backup.c — optional rotating database backups (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "task_worker.h"             /* the shared periodic-pass scheduler  */
#include "db.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

/* Backup filenames: sortable, so lexical order IS chronological order and
 * the prune can pick victims without parsing dates or trusting mtimes on
 * a destination that may be a network mount.                              */
#define BACKUP_PREFIX "tasks-"
#define BACKUP_SUFFIX ".db"

/* sync_state key holding the source's identity at the last SUCCESSFUL
 * backup, so an unchanged database is not copied again.  It lives in the
 * DATABASE rather than the ini because it describes that database — copy
 * the file elsewhere and the record travels with it.                      */
#define BACKUP_STAMP_KEY "backup_source_stamp"

/* ---------------------------------------------------------------------------
 * source_stamp() — a cheap identity for "the state of the last backup":
 * the DESTINATION, plus the source file's size and modification time.
 * Not a hash: this runs on every timer tick, and the question is only
 * "would another backup right now be a duplicate", which mtime answers
 * for a file SQLite has written.
 *
 * The destination is part of it deliberately.  Without it, choosing a new
 * backup folder would produce NO backup there until the database happened
 * to change — the new folder would sit empty and the feature would look
 * broken.  Including it means a changed destination counts as "not backed
 * up yet", which is what the user just asked for by changing it.
 *
 * Returns a new string (g_free), or NULL when the file cannot be stat'd.
 * ------------------------------------------------------------------------- */
static gchar *
source_stamp(const gchar *path, const gchar *dest_dir)
{
    GStatBuf sb;
    if (g_stat(path, &sb) != 0)
        return NULL;
    return g_strdup_printf("%s|%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                           dest_dir != NULL ? dest_dir : "",
                           (gint64)sb.st_size, (gint64)sb.st_mtime);
}

/* backup_keep() — the retention bound, read from config with the shared
 * default and clamped to something sane: a hand-edited ini must not be
 * able to ask for zero retention, which would delete a backup the moment
 * it was made.  The interval is the scheduler's business now (see
 * task_worker.h), and it applies the same guard against a negative.       */
static gint
backup_keep(void)
{
    gchar *v = task_app_config_get("backup_keep");
    gint n = v != NULL ? atoi(v) : TASK_BACKUP_KEEP_DEFAULT;
    g_free(v);
    return CLAMP(n, 1, 500);
}

/* ---------------------------------------------------------------------------
 * task_backup_dir() — the resolved destination (see backup.h).
 *
 * The fallback is the DEFAULT DATABASE directory under the home
 * directory, taken from task_db_default_path so the two can never drift
 * apart — and reached through it deliberately, because that call also
 * creates the directory, which is what makes the fallback usable with no
 * setup at all.
 * ------------------------------------------------------------------------- */
gchar *
task_backup_dir(void)
{
    gchar *dir = task_app_config_get("backup_dir");
    if (dir != NULL && *dir != '\0')
        return dir;
    g_free(dir);
    gchar *db  = task_db_default_path();   /* creates <data>/tasks/         */
    gchar *out = g_path_get_dirname(db);
    g_free(db);
    return out;
}

/* task_backup_ready() — see backup.h.                                      */
gboolean
task_backup_ready(gchar **reason)
{
    if (reason != NULL)
        *reason = NULL;
    if (!task_app_config_get_bool("backup_enabled", FALSE)) {
        if (reason != NULL)
            *reason = g_strdup("Backups are switched off "
                               "(Settings \xe2\x86\x92 Database)");
        return FALSE;
    }
    /* No "no folder chosen" case any more: task_backup_dir always answers,
     * and creates the fallback.  What is left is a CHOSEN folder that has
     * since gone away or turned read-only — a removed external disk, most
     * likely — which is worth saying plainly rather than silently
     * redirecting somewhere the user is not looking.                       */
    gchar *dir = task_backup_dir();
    gboolean ok = FALSE;
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        if (reason != NULL)
            *reason = g_strdup_printf("Backup folder does not exist: %s",
                                      dir);
    } else if (g_access(dir, W_OK) != 0) {
        if (reason != NULL)
            *reason = g_strdup_printf("Backup folder is not writable: %s",
                                      dir);
    } else {
        ok = TRUE;
    }
    g_free(dir);
    return ok;
}

/* ---------------------------------------------------------------------------
 * backup_list() — every backup file in `dir`, sorted oldest first.
 *
 * Matched by our own PREFIX and SUFFIX only, so nothing else the user
 * keeps in that folder is ever a prune candidate — the destination may
 * well be a folder they also use for other things.
 *
 * Returns a GPtrArray of full paths (g_free each); free with
 * g_ptr_array_free(a, TRUE) after.
 * ------------------------------------------------------------------------- */
static GPtrArray *
backup_list(const gchar *dir)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return out;
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (g_str_has_prefix(name, BACKUP_PREFIX) &&
            g_str_has_suffix(name, BACKUP_SUFFIX))
            g_ptr_array_add(out, g_build_filename(dir, name, NULL));
    }
    g_dir_close(d);
    /* Lexical == chronological, by construction of the filename.           */
    g_ptr_array_sort(out, (GCompareFunc)g_strcmp0);
    return out;
}

/* ---------------------------------------------------------------------------
 * The worker job.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp          *app;
    gchar          *db_path;         /* source, copied for the worker       */
    gchar          *dir;             /* destination                         */
    gint            keep;
    TaskBackupDoneFn  done;
    gpointer        user_data;
    /* filled by the worker, read on the main thread                        */
    gboolean        ok;
    gboolean        skipped;         /* nothing changed; not a failure      */
    gchar          *message;
    gint            pruned;
} BackupJob;

static void
backup_job_free(BackupJob *job)
{
    g_free(job->db_path);
    g_free(job->dir);
    g_free(job->message);
    g_free(job);
}

/* backup_finish() — main-thread tail: report and release the guard.        */
static gboolean
backup_finish(gpointer data)
{
    BackupJob *job = data;
    job->app->backup_running = FALSE;
    if (job->message != NULL)
        task_app_status(job->app, "%s", job->message);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    backup_job_free(job);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * backup_thread() — the pass itself, on its own SQLite connection.
 *
 * Order is deliberate and is the whole safety argument:
 *   1. stamp the source and compare — unchanged means do nothing;
 *   2. VACUUM INTO a fresh, uniquely named file;
 *   3. VERIFY it, and delete it again if it does not pass;
 *   4. record the stamp;
 *   5. ONLY NOW prune the oldest beyond `keep`.
 *
 * Step 5 last is what keeps a run of failing backups from eating the
 * good history that is already on disk.
 * ------------------------------------------------------------------------- */
static gpointer
backup_thread(gpointer data)
{
    BackupJob *job = data;
    GError    *gerr = NULL;

    TaskDatabase *db = task_db_open(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Backup failed: cannot open the "
            "database (%s)", gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_idle_add(backup_finish, job);
        return NULL;
    }

    gchar *stamp = source_stamp(job->db_path, job->dir);
    gchar *last  = task_db_state_get(db, BACKUP_STAMP_KEY);
    if (stamp != NULL && last != NULL && g_strcmp0(stamp, last) == 0) {
        job->ok      = TRUE;
        job->skipped = TRUE;
        job->message = NULL;         /* silent: nothing happened            */
        g_free(stamp);
        g_free(last);
        task_db_close(db);
        g_idle_add(backup_finish, job);
        return NULL;
    }
    g_free(last);

    /* A unique, sortable name.  The seconds-resolution stamp could collide
     * if two passes landed in the same second (a timer tick racing a
     * manual press), so a suffix is appended until the name is free —
     * VACUUM INTO refuses an existing file, and silently overwriting one
     * is exactly what this module must never do.                          */
    GDateTime *now = g_date_time_new_now_local();
    gchar *when = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    gchar *dest = NULL;
    for (gint n = 0; n < 100; n++) {
        g_free(dest);
        dest = n == 0
            ? g_strdup_printf("%s/%s%s%s", job->dir, BACKUP_PREFIX, when,
                              BACKUP_SUFFIX)
            : g_strdup_printf("%s/%s%s-%d%s", job->dir, BACKUP_PREFIX, when,
                              n, BACKUP_SUFFIX);
        if (!g_file_test(dest, G_FILE_TEST_EXISTS))
            break;
    }
    g_free(when);

    gchar *cerr = NULL;
    if (!task_db_copy_file(db, dest, &cerr)) {
        job->message = g_strdup_printf("Backup failed: %s",
                                       cerr != NULL ? cerr : "copy error");
        g_free(cerr);
        g_free(dest);
        g_free(stamp);
        task_db_close(db);
        g_idle_add(backup_finish, job);
        return NULL;
    }
    g_free(cerr);

    /* Verify before it counts as a backup at all.  An unverifiable copy is
     * worse than no copy: it would sit in the rotation looking like
     * history and displace a good one.                                     */
    gchar *detail = NULL;
    if (!task_db_verify_file(dest, &detail)) {
        g_unlink(dest);
        job->message = g_strdup_printf("Backup failed verification and was "
            "discarded: %s", detail != NULL ? detail : "?");
        g_free(detail);
        g_free(dest);
        g_free(stamp);
        task_db_close(db);
        g_idle_add(backup_finish, job);
        return NULL;
    }
    g_free(detail);

    if (stamp != NULL)
        task_db_state_set(db, BACKUP_STAMP_KEY, stamp);
    g_free(stamp);
    task_db_close(db);

    /* Prune — only now, and only our own files.                            */
    GPtrArray *have = backup_list(job->dir);
    for (guint i = 0; have->len - i > (guint)job->keep; i++) {
        if (g_unlink(g_ptr_array_index(have, i)) == 0)
            job->pruned++;
        else
            g_warning("backup: could not remove %s",
                      (const gchar *)g_ptr_array_index(have, i));
    }
    guint kept = have->len;
    g_ptr_array_free(have, TRUE);

    job->ok = TRUE;
    gchar *base = g_path_get_basename(dest);
    job->message = job->pruned > 0
        ? g_strdup_printf("Backed up to %s (%u kept, %d pruned)", base,
                          kept - (guint)job->pruned, job->pruned)
        : g_strdup_printf("Backed up to %s (%u kept)", base, kept);
    g_free(base);
    g_free(dest);
    g_idle_add(backup_finish, job);
    return NULL;
}

/* task_backup_start() — see backup.h.                                      */
void
task_backup_start(TaskApp *app, const gchar *db_path, TaskBackupDoneFn done,
                  gpointer user_data)
{
    gchar *why = NULL;
    if (!task_backup_ready(&why)) {
        task_app_status(app, "%s", why != NULL ? why : "Backups unavailable");
        if (done != NULL)
            done(app, FALSE, why, user_data);
        g_free(why);
        return;
    }
    g_free(why);
    if (app->backup_running)
        return;                      /* a pass is already in flight         */

    app->backup_running = TRUE;
    BackupJob *job = g_new0(BackupJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->dir       = task_backup_dir();   /* resolved, never NULL */
    job->keep      = backup_keep();
    job->done      = done;
    job->user_data = user_data;
    GThread *th = g_thread_new("task-backup", backup_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * The periodic timer — the shared scheduler drives it (task_worker.h).
 *
 * INITIAL_NEVER: an unprompted copy at every launch is not what the
 * setting promises, and a pass whose source is unchanged writes nothing
 * anyway (see the source stamp in task_backup_start).
 * ------------------------------------------------------------------------- */
static void
backup_run(TaskApp *app, const gchar *db_path)
{
    task_backup_start(app, db_path, NULL, NULL);
}

static const TaskWorkerDef backup_worker = {
    .id               = "backup",
    .enabled_key      = "backup_enabled",
    .enabled_default  = FALSE,
    .interval_key     = "backup_interval_min",
    .interval_default = TASK_BACKUP_INTERVAL_DEFAULT,
    .initial          = TASK_WORKER_INITIAL_NEVER,
    .running          = NULL,        /* completed by task_backup_init       */
    .timer            = NULL,
    .run              = backup_run,
    .ready            = NULL,
    .on_arm           = NULL,
};

static TaskWorkerDef backup_worker_live;

/* task_backup_auto_start() — see backup.h.                                 */
void
task_backup_auto_start(TaskApp *app, const gchar *db_path)
{
    task_worker_arm(app, &backup_worker_live, db_path);
}

/* task_backup_init() — see backup.h.                                       */
void
task_backup_init(TaskApp *app)
{
    backup_worker_live         = backup_worker;
    backup_worker_live.running = &app->backup_running;
    backup_worker_live.timer   = &app->backup_timer;
    task_worker_register(&backup_worker_live);
}
