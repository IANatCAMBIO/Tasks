/*
 * backup_test.c — the rotating backup (src/backup.h): where backups go,
 * when the feature says it is ready, what one pass writes and reports, an
 * unchanged source left alone, the rotation (which files are pruned, which
 * are never touched, two copies in one second, the limits of the keep
 * count), and the timer the settings arm.
 *
 * Every pass here runs on the real background job and is waited for with
 * check_spin.  Old backups are stood in for by empty files with old names,
 * created in the order of their names: the rotation goes by modification
 * time, then by name, so that pins down exactly which files a pass
 * removes without waiting for the clock.
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
 * run_backup — start a pass and wait for it to report.
 *
 * Inputs:
 *   app — the context
 *
 * Output:
 *   what its completion saw (calls is 0 if it never reported).
 */
static seen_t
run_backup(TaskApp *app)
{
    seen_t seen = { 0 };

    task_backup_start(app, shared_db_file(app->db->shared), on_done, &seen);
    for (int i = 0; i < 200 && seen.calls == 0; i++) check_spin(0.05);
    return seen;
}

/*
 * is_backup_name — whether a file name is one the rotation owns:
 * "tasks-….db".
 *
 * Inputs:
 *   name — a file name
 *
 * Output:
 *   non-zero when it is.
 */
static int
is_backup_name(const char *name)
{
    size_t len = strlen(name);

    return strncmp(name, "tasks-", 6) == 0 && len > 9 && strcmp(name + len - 3, ".db") == 0;
}

/*
 * count_backups — how many backup files a folder holds.
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
    DIR *d     = opendir(dir);
    int  count = 0;

    if (d == NULL) return 0;
    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (is_backup_name(e->d_name)) count++;
    }
    closedir(d);
    return count;
}

/*
 * has_file — whether a folder holds a file of this name.
 *
 * Inputs:
 *   dir  — the folder
 *   name — the file name
 *
 * Output:
 *   non-zero when it does.
 */
static int
has_file(const char *dir, const char *name)
{
    char        path[1400];
    struct stat sb;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &sb) == 0;
}

/*
 * put_file — create an empty file: a stand-in for an old backup, or for
 * something else the user keeps in the backup folder.
 *
 * Inputs:
 *   dir  — the folder
 *   name — the file name
 */
static void
put_file(const char *dir, const char *name)
{
    char path[1400];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f != NULL) fclose(f);
}

/*
 * use_folder — make a fresh folder under the fixture's data directory the
 * backup destination.  A new destination also makes the next pass a real
 * one: a pass skips only a source it has already copied to THAT folder.
 *
 * Inputs:
 *   name — the folder's name
 *   buf  — receives its path
 *   size — size of buf
 */
static void
use_folder(const char *name, char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s", getenv(TASK_DATA_DIR_ENV), name);
    mkdir(buf, 0755);
    task_config_set(TASK_CFG_BACKUP_DIR, buf);
}

/*
 * test_destination — where backups go, and when the feature is usable.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_destination(TaskApp *app)
{
    char         want[1200];
    char         path[1200];
    struct stat  sb;
    const char  *data = getenv(TASK_DATA_DIR_ENV);

    /* The default: a backups/ folder in the data directory, made on demand. */
    snprintf(want, sizeof want, "%s/backups", data);
    check(stat(want, &sb) != 0, "destination: (no backups folder yet)");
    gchar *dir = task_backup_dir();
    check(dir != NULL && strcmp(dir, want) == 0,
          "destination: the default is backups/ in the data directory");
    check(stat(want, &sb) == 0 && S_ISDIR(sb.st_mode), "destination: ... created when asked for");
    check(task_path_shares_fate(dir, data),
          "destination: ... and it shares the fate of the database's folder");
    g_free(dir);

    /* A configured folder is the answer as given, and is not created. */
    snprintf(path, sizeof path, "%s/elsewhere", data);
    task_config_set(TASK_CFG_BACKUP_DIR, path);
    dir = task_backup_dir();
    check(dir != NULL && strcmp(dir, path) == 0, "destination: a configured folder wins");
    check(stat(path, &sb) != 0, "destination: ... and is not created behind the user's back");
    g_free(dir);

    /* Readiness, and the reason when not. */
    gchar *reason = NULL;
    check(!task_backup_ready(&reason) && reason != NULL && strstr(reason, "switched off") != NULL,
          "ready: off by default, and says so");
    g_free(reason);
    check(!task_backup_ready(NULL), "ready: the reason is optional");

    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, TRUE);
    check(!task_backup_ready(&reason) && reason != NULL && strstr(reason, "does not exist") != NULL,
          "ready: a folder that is not there is named as missing");
    g_free(reason);

    mkdir(path, 0555);
    check(!task_backup_ready(&reason) && reason != NULL && strstr(reason, "not writable") != NULL,
          "ready: a read-only folder is named as not writable");
    g_free(reason);
    chmod(path, 0755);
    check(task_backup_ready(&reason) && reason == NULL, "ready: a writable folder is ready");

    /* A pass that cannot start reports at once and writes nothing. */
    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, FALSE);
    seen_t seen = { 0 };
    last_status[0] = '\0';
    task_backup_start(app, shared_db_file(app->db->shared), on_done, &seen);
    check(seen.calls == 1 && !seen.ok && strstr(seen.message, "switched off") != NULL,
          "start: a pass with backups off reports why, before returning");
    check(strstr(last_status, "switched off") != NULL, "start: ... in the status bar too");
    check(!app->backup_running && count_backups(path) == 0, "start: ... and nothing is written");
    task_backup_start(app, shared_db_file(app->db->shared), NULL, NULL);
    check(1, "start: the completion callback is optional");

    task_config_set(TASK_CFG_BACKUP_DIR, NULL);
}

/*
 * test_live_folder — a destination inside the live data directory is
 * refused by name, and nothing is created in it.  Run against a FAKE live
 * directory in the sandbox ($XDG_DATA_HOME moves what the fence calls
 * live).
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_live_folder(TaskApp *app)
{
    char xdg[1200];
    char live[1300];
    char dest[1400];

    snprintf(xdg, sizeof xdg, "%s/xdg", getenv(TASK_DATA_DIR_ENV));
    snprintf(live, sizeof live, "%s/tasks", xdg);
    snprintf(dest, sizeof dest, "%s/backups", live);
    mkdir(xdg, 0755);
    mkdir(live, 0755);
    mkdir(dest, 0755);
    setenv("XDG_DATA_HOME", xdg, 1);

    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, TRUE);
    task_config_set(TASK_CFG_BACKUP_DIR, dest);
    gchar *reason = NULL;
    check(!task_backup_ready(&reason) && reason != NULL && strstr(reason, "live") != NULL,
          "live: a folder inside the live data directory is refused, and says why");
    g_free(reason);

    seen_t seen = run_backup(app);
    check(seen.calls == 1 && !seen.ok && strstr(seen.message, "live") != NULL,
          "live: a pass aimed there fails with the same reason");
    check(count_backups(dest) == 0, "live: ... and writes nothing there");

    task_config_set(TASK_CFG_BACKUP_DIR, live);
    check(!task_backup_ready(NULL), "live: the live directory itself is refused too");

    unsetenv("XDG_DATA_HOME");
    check(task_backup_ready(NULL), "live: (the same folder is fine once it is not live)");
    task_config_set(TASK_CFG_BACKUP_DIR, NULL);
}

/*
 * test_pass — one real pass: what it writes and what it says.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_pass(TaskApp *app)
{
    char dir[1200];
    char path[1400];

    use_folder("pass", dir, sizeof dir);
    task_db_list_create(app->db, "Backed up", "");

    last_status[0] = '\0';
    seen_t seen = { 0 };
    task_backup_start(app, shared_db_file(app->db->shared), on_done, &seen);
    check(app->backup_running && seen.calls == 0,
          "pass: it runs in the background (not finished when start returns)");

    /* A second start while one is in flight is dropped without a callback. */
    seen_t second = { 0 };
    task_backup_start(app, shared_db_file(app->db->shared), on_done, &second);
    for (int i = 0; i < 200 && seen.calls == 0; i++) check_spin(0.05);
    check_spin(0.1);
    check(seen.calls == 1 && seen.ok, "pass: it completes and reports success once");
    check(second.calls == 0, "pass: a start during a pass is dropped, and calls nothing back");
    check(!app->backup_running, "pass: the in-flight flag is released");
    check(count_backups(dir) == 1, "pass: exactly one backup file is written");

    /* "Backed up to tasks-YYYYMMDD-HHMMSS.db (1 kept)" */
    char name[64] = "";
    check(sscanf(seen.message, "Backed up to %63s", name) == 1 && is_backup_name(name) &&
          strlen(name) == strlen("tasks-20260101-000000.db") &&
          strstr(seen.message, " (1 kept)") != NULL,
          "pass: the message names the file, by date and time, and the count kept");
    check(has_file(dir, name), "pass: ... and that file is the one written");
    check(strcmp(last_status, seen.message) == 0, "pass: the status bar gets the same message");

    snprintf(path, sizeof path, "%s/%s", dir, name);
    gchar *detail = NULL;
    check(task_db_verify_file(path, &detail), "pass: the backup is a sound database");
    g_free(detail);
    check(has_file(dir, name) && !has_file(getenv(TASK_DATA_DIR_ENV), name),
          "pass: nothing is written beside the live database");
}

/*
 * test_unchanged — a source already copied to this folder is not copied
 * again, however long ago: the pass succeeds silently and writes nothing.
 * A change to the database makes the next pass a real one again.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_unchanged(TaskApp *app)
{
    char   dir[1200];

    use_folder("unchanged", dir, sizeof dir);
    check(run_backup(app).ok && count_backups(dir) == 1, "unchanged: (a first backup is made)");

    /* Into the next second, so that a stamp taken by the clock would differ. */
    check_spin(1.1);
    last_status[0] = '\0';
    seen_t seen = run_backup(app);
    check(seen.calls == 1 && seen.ok && seen.message[0] == '\0',
          "unchanged: a pass over an unchanged source succeeds with no message");
    check(count_backups(dir) == 1, "unchanged: ... and writes no file");
    check(last_status[0] == '\0', "unchanged: ... and says nothing in the status bar");
    check(has_file(dir, ".last-source"), "unchanged: the stamp is kept beside the backups");

    task_db_list_create(app->db, "Changed since", "");
    seen = run_backup(app);
    check(seen.ok && seen.message[0] != '\0' && count_backups(dir) == 2,
          "unchanged: a change to the database is backed up again");

    /* The destination is part of what "already copied" means: a new folder
     * gets a backup even though the database has not changed. */
    char other[1200];
    use_folder("unchanged-moved", other, sizeof other);
    seen = run_backup(app);
    check(seen.ok && count_backups(other) == 1,
          "unchanged: a newly chosen folder is backed up to at once");
}

/*
 * test_rotation — which files a pass prunes.  Four old backups stand in
 * the folder beside files the rotation must never touch; one real pass
 * with keep = 3 must leave the new backup and the two newest old ones.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_rotation(TaskApp *app)
{
    char dir[1200];

    use_folder("rotation", dir, sizeof dir);
    put_file(dir, "tasks-20200101-000000.db");
    put_file(dir, "tasks-20200102-000000.db");
    put_file(dir, "tasks-20200103-000000.db");
    put_file(dir, "tasks-20200104-000000.db");
    put_file(dir, "tasks.db");                       /* not "tasks-…"           */
    put_file(dir, "notes.txt");
    put_file(dir, "tasks-20200101-000000.db.bak");   /* wrong ending            */
    put_file(dir, "my-tasks-20200101-000000.db");    /* wrong beginning         */

    task_config_set_int(TASK_CFG_BACKUP_KEEP, 3);
    seen_t seen = run_backup(app);
    check(seen.ok && strstr(seen.message, "(3 kept, 2 pruned)") != NULL,
          "rotation: keep 3 of 5 reports 3 kept, 2 pruned");
    check(count_backups(dir) == 3, "rotation: three backups remain");
    check(!has_file(dir, "tasks-20200101-000000.db") && !has_file(dir, "tasks-20200102-000000.db"),
          "rotation: the two OLDEST are the ones removed");
    check(has_file(dir, "tasks-20200103-000000.db") && has_file(dir, "tasks-20200104-000000.db"),
          "rotation: the two newest old backups stay");
    check(has_file(dir, "tasks.db") && has_file(dir, "notes.txt") &&
          has_file(dir, "tasks-20200101-000000.db.bak") &&
          has_file(dir, "my-tasks-20200101-000000.db"),
          "rotation: files that are not backups are never touched");

    /* Fewer files than the bound: nothing is pruned. */
    char roomy[1200];
    use_folder("rotation-roomy", roomy, sizeof roomy);
    put_file(roomy, "tasks-20200101-000000.db");
    task_config_set_int(TASK_CFG_BACKUP_KEEP, 10);
    seen = run_backup(app);
    check(seen.ok && strstr(seen.message, "(2 kept)") != NULL && count_backups(roomy) == 2,
          "rotation: under the bound nothing is pruned, and the message has no prune count");

    /* Two copies in one second: the second takes a "-1" suffix, and with
     * keep = 1 it is the one that survives — the newer, not the one whose
     * name sorts last. */
    char twice[1200];
    char first[64], second[64];
    use_folder("rotation-twice", twice, sizeof twice);
    task_config_set_int(TASK_CFG_BACKUP_KEEP, 1);
    seen = run_backup(app);
    check(seen.ok && sscanf(seen.message, "Backed up to %63s", first) == 1,
          "rotation: (a first copy)");
    task_db_list_create(app->db, "Twice", "");
    seen = run_backup(app);
    check(seen.ok && sscanf(seen.message, "Backed up to %63s", second) == 1 &&
          strcmp(first, second) != 0 && count_backups(twice) == 1 && has_file(twice, second),
          "rotation: of two copies made in one second, the newer is the one kept");
}

/*
 * test_keep_limits — the keep count is held to TASK_BACKUP_KEEP_MIN and
 * TASK_BACKUP_KEEP_MAX whatever a hand-edited ini asks for: zero retention
 * would delete a backup the moment it was made, and an unbounded count
 * would let a timer fill the disk.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_keep_limits(TaskApp *app)
{
    char dir[1200];
    char name[64];

    /* Zero and below fold to one: the new backup survives its own pass. */
    use_folder("keep-zero", dir, sizeof dir);
    put_file(dir, "tasks-20200101-000000.db");
    put_file(dir, "tasks-20200102-000000.db");
    task_config_set_int(TASK_CFG_BACKUP_KEEP, 0);
    seen_t seen = run_backup(app);
    check(seen.ok && sscanf(seen.message, "Backed up to %63s", name) == 1 &&
          count_backups(dir) == 1 && has_file(dir, name),
          "keep: 0 is held to 1, and the one kept is the backup just made");

    use_folder("keep-negative", dir, sizeof dir);
    put_file(dir, "tasks-20200101-000000.db");
    task_config_set_int(TASK_CFG_BACKUP_KEEP, -7);
    seen = run_backup(app);
    check(seen.ok && count_backups(dir) == 1, "keep: a negative count is held to 1 too");

    /* Above the ceiling: 502 old files and one new, 500 remain. */
    use_folder("keep-huge", dir, sizeof dir);
    for (int i = 0; i < TASK_BACKUP_KEEP_MAX + 2; i++) {
        snprintf(name, sizeof name, "tasks-2020%04d-000000.db", i);
        put_file(dir, name);
    }
    task_config_set_int(TASK_CFG_BACKUP_KEEP, 100000);
    seen = run_backup(app);
    check(seen.ok && count_backups(dir) == TASK_BACKUP_KEEP_MAX,
          "keep: a huge count is held to the ceiling");
    check(!has_file(dir, "tasks-20200000-000000.db") && !has_file(dir, "tasks-20200002-000000.db") &&
          has_file(dir, "tasks-20200003-000000.db"),
          "keep: ... by removing the oldest three");

    task_config_set(TASK_CFG_BACKUP_KEEP, NULL);
    check(task_config_get_int(TASK_CFG_BACKUP_KEEP) == 10, "keep: the default is 10");
}

/*
 * test_timer — the periodic timer follows the settings each time it is
 * re-armed, and arming never runs a pass of its own.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_timer(TaskApp *app)
{
    char dir[1200];

    use_folder("timer", dir, sizeof dir);
    task_backup_init(app);

    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, TRUE);
    task_config_set(TASK_CFG_BACKUP_INTERVAL_MIN, NULL);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    guint first = app->backup_timer;
    check(first != 0, "timer: enabled with the default interval arms a timer");

    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check(app->backup_timer != 0 && app->backup_timer != first,
          "timer: re-arming replaces the timer rather than adding one");

    task_config_set_int(TASK_CFG_BACKUP_INTERVAL_MIN, 0);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check(app->backup_timer == 0, "timer: interval 0 is manual only: no timer");

    task_config_set_int(TASK_CFG_BACKUP_INTERVAL_MIN, -30);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check(app->backup_timer == 0, "timer: a negative interval is manual only too");

    task_config_set_int(TASK_CFG_BACKUP_INTERVAL_MIN, 15);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check(app->backup_timer != 0, "timer: a new interval arms it again");

    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, FALSE);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check(app->backup_timer == 0, "timer: switching backups off removes the timer");

    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, TRUE);
    task_backup_auto_start(app, shared_db_file(app->db->shared));
    check_spin(0.3);
    check(count_backups(dir) == 0 && !app->backup_running,
          "timer: arming does not run a pass of its own");

    task_worker_disarm_all();
    check(app->backup_timer == 0, "timer: disarming everything clears it");
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
    test_live_folder(app);
    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, TRUE);
    test_pass(app);
    test_unchanged(app);
    test_rotation(app);
    test_keep_limits(app);
    test_timer(app);

    task_app_unlisten(app, sid);
    fixture_close(app);
    return check_result();
}
