/* ===========================================================================
 * backup.c — optional rotating database backups (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "task_config.h"
#include "task_loop.h"
#include "task_paths.h"
#include "task_worker.h"             /* the shared periodic-pass scheduler  */
#include "db.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Backup filenames: the local date and time of the copy, for people
 * reading the folder.  The prune orders the files by MODIFICATION TIME
 * (backup_list), not by name: two passes in one second share a name and
 * take a "-N" suffix, and the hour the clocks fall back repeats its names,
 * so a name alone cannot say which copy is the newer.                    */
#define BACKUP_PREFIX "tasks-"
#define BACKUP_SUFFIX ".db"

/* The file in the destination folder holding the source's identity at the
 * last SUCCESSFUL backup, so an unchanged database is not copied again.
 * It lives beside the backups and NOT in the database: recording it there
 * would itself change the database, so no two passes could ever agree.
 * A hidden name, and not one the prune matches.                           */
#define BACKUP_STAMP_FILE ".last-source"

/*
 * stamp_part — one file's share of a source stamp: its size and
 * modification time, to the nanosecond (APFS keeps them): a write in the
 * same second as the last stamp must not read as "unchanged".
 *
 * Inputs:
 *   out  — the stamp being built
 *   path — the file; one that is not there contributes "-"
 */
static void
stamp_part(GString *out, const gchar *path)
{
    GStatBuf sb;
    if (g_stat(path, &sb) != 0) {
        g_string_append(out, "|-");
        return;
    }
    g_string_append_printf(out, "|%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ".%09ld",
                           (gint64)sb.st_size, (gint64)sb.st_mtimespec.tv_sec,
                           (long)sb.st_mtimespec.tv_nsec);
}

/* ---------------------------------------------------------------------------
 * source_stamp() — a cheap identity for "the state of the last backup":
 * the DESTINATION, plus the size and modification time of the source file
 * AND of its write-ahead log.  Under WAL (db.c) a write lands in the
 * "-wal" file beside the database, and the database file itself changes
 * only at a checkpoint, so the log is as much the source as the file is.
 * Not a hash: this runs on every timer tick, and the question is only
 * "would another backup right now be a duplicate", which the times answer
 * for files only SQLite writes: task_db_open leaves an up-to-date file
 * untouched (db.c, the user_version stamp).
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

    GString *stamp = g_string_new(dest_dir != NULL ? dest_dir : "");
    gchar   *wal   = g_strconcat(path, "-wal", NULL);
    stamp_part(stamp, path);
    stamp_part(stamp, wal);
    g_free(wal);
    return g_string_free(stamp, FALSE);
}

/*
 * stamp_read — the stamp recorded in a destination folder.
 *
 * Inputs:
 *   dir — the destination
 *
 * Output:
 *   new string (g_free), or NULL when none was recorded.
 */
static gchar *
stamp_read(const gchar *dir)
{
    gchar *path  = g_build_filename(dir, BACKUP_STAMP_FILE, NULL);
    gchar *stamp = NULL;
    g_file_get_contents(path, &stamp, NULL, NULL);
    g_free(path);
    return stamp;
}

/*
 * stamp_write — record a stamp in a destination folder.  A failure is
 * only logged: the backup itself is already on disk, and the cost is one
 * more copy at the next pass.
 *
 * Inputs:
 *   dir   — the destination
 *   stamp — the stamp (borrowed)
 */
static void
stamp_write(const gchar *dir, const gchar *stamp)
{
    gchar  *path = g_build_filename(dir, BACKUP_STAMP_FILE, NULL);
    GError *err  = NULL;
    if (!g_file_set_contents(path, stamp, -1, &err)) {
        g_warning("backup: could not record the source stamp in %s: %s", dir,
                  err != NULL ? err->message : "?");
        g_clear_error(&err);
    }
    g_free(path);
}

/* backup_keep() — the retention bound, read from config with the shared
 * default and clamped to something sane: a hand-edited ini must not be
 * able to ask for zero retention, which would delete a backup the moment
 * it was made.  The interval is the scheduler's business now (see
 * task_worker.h), and it applies the same guard against a negative.       */
static gint
backup_keep(void)
{
    return CLAMP(task_config_get_int(TASK_CFG_BACKUP_KEEP),
                 TASK_BACKUP_KEEP_MIN, TASK_BACKUP_KEEP_MAX);
}

/* The default destination: a folder of its own inside the database's
 * directory, so the live tasks.db does not sit among the copies.      */
#define BACKUP_SUBDIR "backups"

/* ---------------------------------------------------------------------------
 * task_backup_dir() — the resolved destination (see backup.h).
 *
 * The fallback is BACKUP_SUBDIR inside the data directory, built off
 * task_data_dir_ensure so the two can never drift apart, and created here
 * so the fallback is usable with no setup at all.
 *
 * It is a SUBDIRECTORY rather than the database's own folder (changed
 * 2026-09-09) because backups and the live file mixed together is a
 * folder nobody can read at a glance: the pruning already had to match
 * `tasks-*.db` so as not to touch `tasks.db`, and a listing that needs a
 * filename rule to be understood is one a human will eventually get
 * wrong.  It buys TIDINESS and nothing else — a subfolder shares the
 * fate of its parent exactly, so this is no more independent of the
 * database than the old default was, and Settings still says so.
 * ------------------------------------------------------------------------- */
gchar *
task_backup_dir(void)
{
    gchar *dir = task_config_get(TASK_CFG_BACKUP_DIR);
    if (dir != NULL)
        return dir;
    char *base = task_data_dir_ensure();   /* creates the data directory    */
    if (base == NULL)
        return NULL;
    gchar *sub = g_build_filename(base, BACKUP_SUBDIR, NULL);
    free(base);
    g_mkdir_with_parents(sub, 0755);
    return sub;
}

/* task_backup_ready() — see backup.h.                                      */
gboolean
task_backup_ready(gchar **reason)
{
    if (reason != NULL)
        *reason = NULL;
    if (!task_config_get_bool(TASK_CFG_BACKUP_ENABLED)) {
        if (reason != NULL)
            *reason = g_strdup("Backups are switched off "
                               "(Settings \xe2\x86\x92 Database)");
        return FALSE;
    }
    /* No "no folder chosen" case any more: task_backup_dir answers with
     * the fallback and creates it (it is NULL only when there is no data
     * directory at all).  What is left is a CHOSEN folder that has
     * since gone away or turned read-only — a removed external disk, most
     * likely — which is worth saying plainly rather than silently
     * redirecting somewhere the user is not looking.                       */
    gchar *dir = task_backup_dir();
    gboolean ok = FALSE;
    if (dir == NULL) {
        if (reason != NULL)
            *reason = g_strdup("No backup folder: the data directory "
                               "cannot be determined");
    } else if (task_path_is_live_data(dir)) {
        if (reason != NULL)
            *reason = g_strdup_printf("Backup folder is inside the live "
                                      "Tasks data directory, which this "
                                      "experimental build never touches: "
                                      "%s", dir);
    } else if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
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

/* One backup file, for the prune's ordering. */
typedef struct {
    gchar  *path;
    gint64  mtime;
} backup_entry_t;

static void
backup_entry_free(gpointer data)
{
    backup_entry_t *entry = data;
    g_free(entry->path);
    g_free(entry);
}

/* ---------------------------------------------------------------------------
 * backup_entry_cmp() — oldest first: by modification time, then, for two
 * files of one second, by name LENGTH before name — "tasks-T-1.db" (the
 * second copy of second T) comes after "tasks-T.db", which plain strcmp
 * would put it before ('-' sorts before '.').
 *
 * g_ptr_array_sort hands the comparator POINTERS TO THE ELEMENTS, so the
 * arguments are backup_entry_t **.  This was once `(GCompareFunc)g_strcmp0`
 * on an array of strings, which compared the pointer values and produced
 * an order that looked plausible and was arbitrary — and this order is
 * what the prune DELETES by.
 * ------------------------------------------------------------------------- */
static gint
backup_entry_cmp(gconstpointer a, gconstpointer b)
{
    const backup_entry_t *ea = *(const backup_entry_t * const *)a;
    const backup_entry_t *eb = *(const backup_entry_t * const *)b;

    if (ea->mtime != eb->mtime)
        return ea->mtime < eb->mtime ? -1 : 1;
    size_t la = strlen(ea->path), lb = strlen(eb->path);
    if (la != lb)
        return la < lb ? -1 : 1;
    return strcmp(ea->path, eb->path);
}

/* ---------------------------------------------------------------------------
 * backup_list() — every backup file in `dir`, sorted oldest first.
 *
 * Matched by our own PREFIX and SUFFIX only, so nothing else the user
 * keeps in that folder is ever a prune candidate — the destination may
 * well be a folder they also use for other things.
 *
 * Returns a GPtrArray of backup_entry_t (full paths); free with
 * g_ptr_array_free(a, TRUE) after.
 * ------------------------------------------------------------------------- */
static GPtrArray *
backup_list(const gchar *dir)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(backup_entry_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return out;
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_prefix(name, BACKUP_PREFIX) ||
            !g_str_has_suffix(name, BACKUP_SUFFIX))
            continue;
        backup_entry_t *entry = g_new0(backup_entry_t, 1);
        GStatBuf        sb;
        entry->path  = g_build_filename(dir, name, NULL);
        entry->mtime = g_stat(entry->path, &sb) == 0 ? (gint64)sb.st_mtime : 0;
        g_ptr_array_add(out, entry);
    }
    g_dir_close(d);
    g_ptr_array_sort(out, backup_entry_cmp);
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

/*
 * backup_finish — the pass's main-thread tail (queued by the worker with
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
    backup_job_free(job);
}

/* ---------------------------------------------------------------------------
 * backup_thread() — the pass itself, on its own SQLite connection.
 *
 * Order is deliberate and is the whole safety argument:
 *   1. stamp the source and compare — unchanged means do nothing;
 *   2. VACUUM INTO a fresh, uniquely named file;
 *   3. VERIFY it, and delete it again if it does not pass;
 *   4. record the stamp in the destination folder;
 *   5. ONLY NOW prune the oldest beyond `keep`.
 *
 * Step 5 last is what keeps a run of failing backups from eating the
 * good history that is already on disk.
 * ------------------------------------------------------------------------- */
static void
backup_thread(void *data)
{
    BackupJob *job = data;
    GError    *gerr = NULL;

    TaskDatabase *db = task_db_open(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Backup failed: cannot open the "
            "database (%s)", gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        task_loop_main_async(backup_finish, job);
        return;
    }

    gchar *stamp = source_stamp(job->db_path, job->dir);
    gchar *last  = stamp_read(job->dir);
    if (stamp != NULL && last != NULL && g_strcmp0(stamp, last) == 0) {
        job->ok      = TRUE;
        job->skipped = TRUE;
        job->message = NULL;         /* silent: nothing happened            */
        g_free(stamp);
        g_free(last);
        task_db_close(db);
        task_loop_main_async(backup_finish, job);
        return;
    }
    g_free(last);

    /* A unique, sortable name.  The seconds-resolution stamp could collide
     * if two passes landed in the same second (a timer tick racing a
     * manual press), so a suffix is appended until the name is free —
     * VACUUM INTO refuses an existing file, and silently overwriting one
     * is exactly what this module must never do.                          */
    /* localtime_r, not the date helpers: their timezone cache belongs to
     * the main thread and this is the worker.                              */
    time_t    now = time(NULL);
    struct tm local;
    char      when[32];
    localtime_r(&now, &local);
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", &local);
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

    gchar *cerr = NULL;
    if (!task_db_copy_file(db, dest, &cerr)) {
        /* Whatever the failed copy left (SQLite creates the target before
         * it fills it) must not stay to count as a backup.              */
        g_unlink(dest);
        job->message = g_strdup_printf("Backup failed: %s",
                                       cerr != NULL ? cerr : "copy error");
        g_free(cerr);
        g_free(dest);
        g_free(stamp);
        task_db_close(db);
        task_loop_main_async(backup_finish, job);
        return;
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
        task_loop_main_async(backup_finish, job);
        return;
    }
    g_free(detail);

    task_db_close(db);
    if (stamp != NULL)
        stamp_write(job->dir, stamp);
    g_free(stamp);

    /* Prune — only now, and only our own files.                            */
    GPtrArray *have = backup_list(job->dir);
    for (guint i = 0; have->len - i > (guint)job->keep; i++) {
        const gchar *victim = ((backup_entry_t *)g_ptr_array_index(have, i))->path;
        if (g_unlink(victim) == 0)
            job->pruned++;
        else
            g_warning("backup: could not remove %s", victim);
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
    task_loop_main_async(backup_finish, job);
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
    job->dir       = task_backup_dir();   /* not NULL: task_backup_ready passed */
    job->keep      = backup_keep();
    job->done      = done;
    job->user_data = user_data;
    task_loop_background(backup_thread, job);
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
    .enabled_key      = TASK_CFG_BACKUP_ENABLED,
    .interval_key     = TASK_CFG_BACKUP_INTERVAL_MIN,
    .initial          = TASK_WORKER_INITIAL_NEVER,
    .running          = NULL,        /* completed by task_backup_init       */
    .timer            = NULL,
    .run              = backup_run,
    .ready            = NULL,
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
