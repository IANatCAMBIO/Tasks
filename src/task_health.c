#include "task_health.h"
#include "task_loop.h"
#include "task_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Month names for the stamp.  A table of our own rather than strftime's
 * "%b", whose answer follows the C locale: the rest of the sentence is
 * English whatever the locale is. */
static const char * const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* The detail line of a verdict that has no words from sqlite. */
#define DETAIL_PASSED    "PRAGMA integrity_check and PRAGMA foreign_key_check " \
                         "both passed against this file."
#define DETAIL_SILENT    "The check gave no further detail."
#define DETAIL_UNCHECKED "Nothing has verified this database yet.  Press Update."

/* ── The verdict's words ────────────────────────────────────────────── */

/*
 * task_health_stamp — "13:24 today" or "Sep 8 at 09:02".
 * Inputs/output are documented in task_health.h.
 */
char *
task_health_stamp(int64_t when, int64_t now)
{
    /* localtime_r, not the task_dates helpers: their timezone cache belongs
     * to the main thread, and this has no reason to be tied to it. */
    time_t    when_t = (time_t)when;
    time_t    now_t  = (time_t)now;
    struct tm when_tm;
    struct tm now_tm;
    char     *out = NULL;

    if (localtime_r(&when_t, &when_tm) == NULL) return strdup("an unknown time");

    bool today = localtime_r(&now_t, &now_tm) != NULL &&
                 when_tm.tm_year == now_tm.tm_year &&
                 when_tm.tm_yday == now_tm.tm_yday;
    int  made  = today
        ? asprintf(&out, "%02d:%02d today", when_tm.tm_hour, when_tm.tm_min)
        : asprintf(&out, "%s %d at %02d:%02d", month_names[when_tm.tm_mon],
                   when_tm.tm_mday, when_tm.tm_hour, when_tm.tm_min);
    return made < 0 ? NULL : out;
}

/*
 * task_health_describe — the light, the line and the tooltip of a verdict.
 * Inputs/output are documented in task_health.h.
 */
task_health_light_t
task_health_describe(const TaskDbHealth *health, int64_t now, char **summary, char **detail)
{
    if (health == NULL) {
        *summary = strdup("Not checked");
        *detail  = strdup(DETAIL_UNCHECKED);
        return task_health_unknown;
    }

    const char *lead  = health->ok  ? "Healthy \xe2\x80\x94 checked"
                      : health->ran ? "Problems found \xe2\x80\x94"
                                    : "Check did not complete \xe2\x80\x94";
    char       *stamp = task_health_stamp(health->when, now);

    if (stamp == NULL || asprintf(summary, "%s %s", lead, stamp) < 0) *summary = NULL;
    free(stamp);

    /* A failed check with no words must not borrow the sentence that says
     * both checks passed. */
    *detail = strdup(health->detail != NULL ? health->detail
                   : health->ok             ? DETAIL_PASSED
                                            : DETAIL_SILENT);
    return health->ok ? task_health_good : task_health_bad;
}

/* ── The background probe ───────────────────────────────────────────── */

/*
 * One probe.  The first block is copied on the main thread before the
 * worker starts; the second is filled by the worker and read back on the
 * main thread.  The worker never touches `app`.
 */
typedef struct {
    TaskApp            *app;
    char               *path;       /* the database file, copied          */
    bool                check;      /* run the integrity check too        */
    task_health_done_fn done;
    void               *user_data;

    TaskDbHealth        verdict;    /* meaningful only with `check`       */
    int64_t             size;       /* bytes, or -1                       */
    gchar              *sha256;     /* hex digest (g_free), or NULL       */
} probe_job_t;

/*
 * file_size — how much room a file takes on disk.  stat rather than
 * SQLite's page count: the question is the file's size, which includes
 * free pages a VACUUM would give back.
 *
 * Inputs:
 *   path — the file
 *
 * Output:
 *   its size in bytes; -1 when it cannot be read, or lies inside the live
 *   data directory (which nothing in this build looks into).
 */
static int64_t
file_size(const char *path)
{
    struct stat sb;

    if (task_path_is_live_data(path) || stat(path, &sb) != 0) return -1;
    return (int64_t)sb.st_size;
}

/*
 * probe_finish — the probe's main-thread tail (queued by the worker):
 * record the verdict if it is still about the open database, report, and
 * free the job.
 *
 * Inputs:
 *   data — the probe_job_t
 */
static void
probe_finish(void *data)
{
    probe_job_t *job = data;
    TaskDatabase *db = job->app->db;

    /* The staleness guard.  The verdict and the digest are facts about the
     * file at job->path; they belong on the plate only while that is still
     * the file the app has open. */
    task_health_probe_t probe = {
        .current = db != NULL && strcmp(db->path, job->path) == 0,
        .size    = job->size,
        .sha256  = job->sha256,
    };
    if (probe.current && job->check) task_db_health_record(db, &job->verdict);

    job->done(job->app, &probe, job->user_data);

    g_free(job->verdict.detail);
    g_free(job->sha256);
    free(job->path);
    free(job);
}

/*
 * probe_thread — the probe itself, on a background thread.  The check goes
 * first so that the size and digest describe the file as the check left it
 * (unchanged: the check's connection is read-only).
 *
 * Inputs:
 *   data — the probe_job_t
 */
static void
probe_thread(void *data)
{
    probe_job_t *job = data;

    if (job->check) task_db_health_check_file(job->path, &job->verdict);
    job->size   = file_size(job->path);
    job->sha256 = task_db_file_sha256(job->path);
    task_loop_main_async(probe_finish, job);
}

/*
 * task_health_probe_start — read the database file in the background.
 * Inputs are documented in task_health.h.
 */
void
task_health_probe_start(TaskApp *app, bool check, task_health_done_fn done, void *user_data)
{
    probe_job_t *job  = calloc(1, sizeof *job);
    char        *path = strdup(app->db->path);

    if (job == NULL || path == NULL) {
        /* Out of memory: still answer, as "nothing could be read". */
        task_health_probe_t none = { .current = true, .size = -1, .sha256 = NULL };
        free(job);
        free(path);
        done(app, &none, user_data);
        return;
    }
    job->app       = app;
    job->path      = path;
    job->check     = check;
    job->done      = done;
    job->user_data = user_data;
    task_loop_background(probe_thread, job);
}
