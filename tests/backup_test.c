/*
 * backup_test.c — backups through the shared module (src/backup.h): where
 * backups go (by default `backups` beside the open database file, never
 * the per-user folder) and what is refused (the live data — Tasks' own
 * refusal — and the module's folder checks), Back Up Now on the background
 * job and the UTC name it writes, the retention the module applies, and
 * the periodic poll that makes a backup only when the shared policy says
 * one is due.  The policy's defaults and ranges are the module's
 * (shared_backup_policy_get) and are tested there.
 *
 * Every pass here runs on the real background job and is waited for with
 * check_spin.  The copy, its verification and the pruning are the shared
 * module's and have its own tests; these check Tasks' use of them.
 */

#include "fixture.h"
#include "backup.h"
#include "task_worker.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

/* What a pass's completion callback and the status bar saw. */
typedef struct {
    int  calls;
    int  ok;
    char message[512];              /* "" when the pass gave none          */
} seen_t;

static char last_status[512];

/*
 * on_done — a pass's completion: keep what it reported.
 *
 * Inputs:
 *   app     — the context (unused)
 *   ok      — whether the pass succeeded
 *   message — its summary (borrowed), or NULL
 *   data    — the seen_t
 */
static void
on_done(TaskApp *app, gboolean ok, const gchar *message, gpointer data)
{
    seen_t *seen = data;

    (void)app;
    seen->calls++;
    seen->ok = ok;
    snprintf(seen->message, sizeof seen->message, "%s", message != NULL ? message : "");
}

/*
 * on_status — the status listener: keep the latest message.
 *
 * Inputs:
 *   app     — the context (unused)
 *   message — the message (borrowed)
 *   data    — unused
 */
static void
on_status(TaskApp *app, const gchar *message, gpointer data)
{
    (void)app;
    (void)data;
    snprintf(last_status, sizeof last_status, "%s", message);
}

/*
 * wait_idle — let a pass in flight finish (its hand-back included).
 *
 * Inputs:
 *   app — the context
 */
static void
wait_idle(TaskApp *app)
{
    for (int i = 0; i < 200 && app->backup_running; i++) check_spin(0.05);
    check_spin(0.05);
}

/*
 * count_backups — how many files named like the module's backups a folder
 * holds.
 *
 * Inputs:
 *   dir — the folder
 *
 * Output:
 *   the count; 0 when the folder cannot be read.
 */
static int
count_backups(const char *dir)
{
    DIR    *d     = opendir(dir);
    int     count = 0;
    size_t  plen  = strlen(SHARED_DB_BACKUP_PREFIX);

    if (d == NULL) return 0;
    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, SHARED_DB_BACKUP_PREFIX, plen) == 0 && len > 3 &&
            strcmp(e->d_name + len - 3, ".db") == 0) {
            count++;
        }
    }
    closedir(d);
    return count;
}

/*
 * use_folder — make a fresh folder under the fixture's data directory the
 * backup destination.
 *
 * Inputs:
 *   app  — the fixture
 *   name — the folder's name
 *   buf  — receives its path
 *   size — size of buf
 */
static void
use_folder(TaskApp *app, const char *name, char *buf, size_t size)
{
    char *why = NULL;

    snprintf(buf, size, "%s/%s", getenv(TASK_DATA_DIR_ENV), name);
    mkdir(buf, 0755);
    if (!task_backup_set_dir(app, buf, &why)) {
        fprintf(stderr, "cannot use %s: %s\n", buf, why);
        exit(2);
    }
}

/*
 * test_destination — where backups go, and what is refused.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_destination(TaskApp *app)
{
    char  path[1200];
    char *why = NULL;

    /* No folder set, and CAMBIO_SHARED_DB names this sandbox's database:
     * the module's default is `backups` beside that file, never the
     * per-user folder. */
    char *env_dir = g_path_get_dirname(getenv(SHARED_DB_ENV_VAR));
    snprintf(path, sizeof path, "%s/backups", env_dir);
    g_free(env_dir);
    char *dir = task_backup_dir(app, &why);
    check(dir != NULL && strcmp(dir, path) == 0 && why == NULL,
          "destination: a sandbox's default is backups/ beside its database");
    free(dir);

    /* Refusals of a chosen folder: nothing is stored. */
    check(!task_backup_set_dir(app, "relative/backups", &why) && why != NULL,
          "destination: a relative folder is refused");
    free(why);
    snprintf(path, sizeof path, "%s/missing", getenv(TASK_DATA_DIR_ENV));
    check(!task_backup_set_dir(app, path, &why) && why != NULL && strstr(why, "not a folder") != NULL,
          "destination: a missing folder is refused");
    free(why);
    snprintf(path, sizeof path, "%s/read-only", getenv(TASK_DATA_DIR_ENV));
    mkdir(path, 0555);
    check(!task_backup_set_dir(app, path, &why) && why != NULL && strstr(why, "not writable") != NULL,
          "destination: a read-only folder is refused");
    free(why);
    chmod(path, 0755);

    /* A folder inside the (fake) live data directory. */
    char xdg[1100];
    snprintf(xdg, sizeof xdg, "%s/xdg", getenv(TASK_DATA_DIR_ENV));
    snprintf(path, sizeof path, "%s/tasks", xdg);
    mkdir(xdg, 0755);
    mkdir(path, 0755);
    setenv("XDG_DATA_HOME", xdg, 1);
    check(!task_backup_set_dir(app, path, &why) && why != NULL && strstr(why, "live") != NULL,
          "destination: a folder inside the live data is refused");
    free(why);
    char *raw = NULL;
    check(shared_settings_get(app->db->shared, SHARED_SETTING_BACKUP_DIR, &raw, NULL) == 0,
          "destination: ... and no refused folder was stored");
    free(raw);
    /* Stored behind the app's back, it is still refused at backup time. */
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_DIR, path, NULL);
    dir = task_backup_dir(app, &why);
    check(dir == NULL && why != NULL && strstr(why, "live") != NULL,
          "destination: a live folder written by hand is refused when a backup asks");
    free(why);

    seen_t seen = { 0 };
    last_status[0] = '\0';
    task_backup_start(app, on_done, &seen);
    check(seen.calls == 1 && !seen.ok && strstr(seen.message, "live") != NULL &&
          strcmp(last_status, seen.message) == 0 && !app->backup_running,
          "destination: Back Up Now then reports why at once, in the status bar too");
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_DIR, NULL, NULL);
    unsetenv("XDG_DATA_HOME");

    /* A chosen folder wins. */
    use_folder(app, "chosen", path, sizeof path);
    dir = task_backup_dir(app, &why);
    check(dir != NULL && strcmp(dir, path) == 0 && why == NULL,
          "destination: a chosen folder is where backups go");
    free(dir);
}

/*
 * test_back_up_now — one real pass: what it writes and what it says.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_back_up_now(TaskApp *app)
{
    char dir[1200];

    use_folder(app, "now", dir, sizeof dir);
    task_db_list_create(app->db, "Backed up", "");

    last_status[0] = '\0';
    seen_t seen   = { 0 };
    seen_t second = { 0 };
    task_backup_start(app, on_done, &seen);
    check(app->backup_running && seen.calls == 0,
          "now: it runs in the background (not finished when start returns)");
    task_backup_start(app, on_done, &second);
    wait_idle(app);
    check(seen.calls == 1 && seen.ok, "now: it completes and reports success once");
    check(second.calls == 0, "now: a start during a pass is dropped, and calls nothing back");
    check(count_backups(dir) == 1, "now: exactly one backup file is written");
    check(strncmp(seen.message, "Backed up to " SHARED_DB_BACKUP_PREFIX,
                  strlen("Backed up to " SHARED_DB_BACKUP_PREFIX)) == 0 &&
          strcmp(last_status, seen.message) == 0,
          "now: the message names the file, in the status bar too");
    const char *name = seen.message + strlen("Backed up to ");
    size_t      plen = strlen(SHARED_DB_BACKUP_PREFIX);
    check(strlen(name) == plen + strlen("YYYYMMDD-HHMMSSZ.db") && name[plen + 8] == '-' &&
          strcmp(name + plen + 15, "Z.db") == 0,
          "now: the name is the UTC stamp shared_data-YYYYMMDD-HHMMSSZ.db");

    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char      *why  = NULL;
    shared_db *copy = shared_db_open_readonly(path, &why);
    gint lists = 0;
    if (copy != NULL) {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(shared_db_handle(copy), "SELECT count(*) FROM lists", -1, &st, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) lists = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    check(copy != NULL && lists == 1, "now: the backup is a shared database holding the data");
    shared_db_close(copy);
    free(why);
    int64_t last_at = 0;
    shared_backup_policy_get(app->db->shared, SHARED_SETTING_BACKUP_LAST_AT, &last_at, NULL);
    check(last_at > 0,
          "now: the module records when it was made");

    /* Retention: keep 2, then two more backups leave two. */
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_KEEP, "2", NULL);
    for (int i = 0; i < 2; i++) {
        seen_t more = { 0 };
        task_backup_start(app, on_done, &more);
        wait_idle(app);
    }
    check(count_backups(dir) == 2, "now: the shared keep count prunes the oldest");
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_KEEP, NULL, NULL);
}

/*
 * test_poll — the periodic worker: armed at the fixed poll period, it makes
 * a backup at once when one is due, then none until the interval passes;
 * switched off or at interval 0 it makes none.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_poll(TaskApp *app)
{
    char dir[1200];

    use_folder(app, "poll", dir, sizeof dir);
    task_backup_init(app);
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_LAST_AT, NULL, NULL);

    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_ENABLED, "0", NULL);
    task_worker_arm_all(app, shared_db_file(app->db->shared));
    wait_idle(app);
    check(count_backups(dir) == 0 && app->backup_timer != 0,
          "poll: switched off, the poll runs but makes no backup");

    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_ENABLED, "1", NULL);
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_INTERVAL_MIN, "0", NULL);
    task_worker_arm_all(app, shared_db_file(app->db->shared));
    wait_idle(app);
    check(count_backups(dir) == 0, "poll: interval 0 means only on request");

    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_INTERVAL_MIN, "60", NULL);
    last_status[0] = '\0';
    task_worker_arm_all(app, shared_db_file(app->db->shared));
    wait_idle(app);
    check(count_backups(dir) == 1 && strncmp(last_status, "Backed up to", 12) == 0,
          "poll: a backup never made is due at once, and says so");

    last_status[0] = '\0';
    task_worker_arm_all(app, shared_db_file(app->db->shared));
    wait_idle(app);
    check(count_backups(dir) == 1 && last_status[0] == '\0',
          "poll: inside the interval nothing is made, and nothing is said");

    task_worker_disarm_all();
    check(app->backup_timer == 0, "poll: disarming everything clears the timer");
}

/*
 * test_moved_aside — the backup worker's own connection never creates the
 * database: with the file moved aside (as a restore or rollback does) a
 * pass fails, saying the file does not exist, and leaves nothing at its
 * path.  The file is then put back.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_moved_aside(TaskApp *app)
{
    char dir[1200];
    char aside[1200];

    use_folder(app, "aside", dir, sizeof dir);
    const char *path = shared_db_file(app->db->shared);
    snprintf(aside, sizeof aside, "%s.aside", path);
    /* An empty -wal is what a missing file may be created beside: without
     * the open-existing rule the worker would make a new database here. */
    check(sqlite3_exec(shared_db_handle(app->db->shared), "PRAGMA wal_checkpoint(TRUNCATE)",
                       NULL, NULL, NULL) == SQLITE_OK &&
          rename(path, aside) == 0, "aside: (the log emptied, the database moved aside)");

    seen_t seen = { 0 };
    task_backup_start(app, on_done, &seen);
    wait_idle(app);
    struct stat st;
    check(seen.calls == 1 && !seen.ok && strstr(seen.message, "does not exist") != NULL,
          "aside: the backup fails, saying the file does not exist");
    check(stat(path, &st) != 0, "aside: no new database is created at the path");
    check(count_backups(dir) == 0, "aside: and no backup is written");

    check(rename(aside, path) == 0, "aside: (the database is put back)");
}

/*
 * main — run the backup tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();
    guint    sid = task_app_listen_status(app, on_status, NULL);

    test_destination(app);
    test_back_up_now(app);
    test_poll(app);
    test_moved_aside(app);

    task_app_unlisten(app, sid);
    fixture_close(app);
    return check_result();
}
