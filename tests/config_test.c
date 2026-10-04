/*
 * config_test.c — where the data lives (src/task_paths.h) and the settings
 * file (src/task_config.h): the default directory, the TASKS_DATA_DIR
 * override, the fence round the GTK app's live data, the registry defaults
 * and write-through.
 *
 * The live data directory is only ever NAMED here (paths are compared and
 * refused); nothing in it is opened, and the refusals are checked to
 * happen before any open.
 */

#include "check.h"
#include "db.h"
#include "task_config.h"
#include "task_paths.h"

#include <pwd.h>
#include <string.h>
#include <sys/stat.h>

/*
 * home — the user's home directory, as the password database gives it.
 *
 * Output:
 *   borrowed static string.
 */
static const char *
home(void)
{
    return getpwuid(getuid())->pw_dir;
}

/*
 * ends_with — whether a string ends with a suffix.
 *
 * Inputs:
 *   s      — the string
 *   suffix — the wanted ending
 *
 * Output:
 *   non-zero when it does.
 */
static int
ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);

    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/*
 * test_data_dir — the default location and the override.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_data_dir(const char *scratch)
{
    char *sandbox = strdup(getenv(TASK_DATA_DIR_ENV));

    /* No override: Application Support, never the XDG directory.  Only the
     * NAME is computed; task_data_dir does not create anything. */
    unsetenv(TASK_DATA_DIR_ENV);
    char *def = task_data_dir();
    check(def != NULL && strncmp(def, home(), strlen(home())) == 0,
          "data dir: default is under the home directory");
    check(def != NULL &&
          ends_with(def, "/Library/Application Support/" TASKS_DATA_DIR_NAME),
          "data dir: default is in ~/Library/Application Support");
    check(def != NULL && strstr(def, ".local/share") == NULL,
          "data dir: default is not the XDG data directory");
    check(def != NULL && !task_path_is_live_data(def),
          "data dir: default is outside the live data directory");
    free(def);

    /* An empty override counts as unset. */
    setenv(TASK_DATA_DIR_ENV, "", 1);
    char *empty = task_data_dir();
    check(empty != NULL && ends_with(empty, "/" TASKS_DATA_DIR_NAME),
          "data dir: an empty override is ignored");
    free(empty);

    /* The override wins, and a relative one is made absolute. */
    setenv(TASK_DATA_DIR_ENV, scratch, 1);
    char *over = task_data_dir();
    check(over != NULL && strcmp(over, scratch) == 0, "data dir: override wins");
    free(over);

    setenv(TASK_DATA_DIR_ENV, "relative/dir", 1);
    char *rel = task_data_dir();
    check(rel != NULL && rel[0] == '/' && ends_with(rel, "/relative/dir"),
          "data dir: a relative override becomes absolute");
    free(rel);

    /* An override pointing INTO the live directory is refused outright. */
    char live[1024];
    snprintf(live, sizeof live, "%s/.local/share/tasks", home());
    setenv(TASK_DATA_DIR_ENV, live, 1);
    check(task_data_dir_ensure() == NULL,
          "data dir: an override into the live directory is refused");
    check(!task_config_init(), "config: refuses to load from the live directory");
    check(task_db_default_path() == NULL,
          "db: no default path inside the live directory");

    setenv(TASK_DATA_DIR_ENV, sandbox, 1);
    free(sandbox);
}

/*
 * test_fence — task_path_is_live_data and the entry points that enforce it.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_fence(const char *scratch)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/.local/share/tasks", home());
    check(task_path_is_live_data(path), "fence: the live directory itself");

    snprintf(path, sizeof path, "%s/.local/share/tasks/tasks.db", home());
    check(task_path_is_live_data(path), "fence: the live database");

    snprintf(path, sizeof path, "%s/.local/share/tasks/backups/x/y.db", home());
    check(task_path_is_live_data(path), "fence: anything below it, existing or not");

    snprintf(path, sizeof path, "%s/.LOCAL/Share/Tasks/tasks.db", home());
    check(task_path_is_live_data(path), "fence: case differences do not get round it");

    snprintf(path, sizeof path, "%s/Library/../.local/share/tasks/tasks.ini", home());
    check(task_path_is_live_data(path), "fence: \"..\" does not get round it");

    snprintf(path, sizeof path, "%s/.local/share/tasks-other/tasks.db", home());
    check(!task_path_is_live_data(path), "fence: a sibling directory is not live data");

    check(!task_path_is_live_data(scratch), "fence: the sandbox is not live data");
    check(!task_path_is_live_data(NULL), "fence: NULL is not a path");

    /* A symbolic link into the live directory is followed. */
    char link_path[1024];
    char target[1024];
    snprintf(link_path, sizeof link_path, "%s/link-to-live", scratch);
    snprintf(target, sizeof target, "%s/.local/share/tasks", home());
    check(symlink(target, link_path) == 0, "fence: (made a symlink into the live dir)");
    snprintf(path, sizeof path, "%s/tasks.db", link_path);
    check(task_path_is_live_data(path), "fence: a symlink does not get round it");
    /* Removed at once: nothing that later walks the sandbox may follow it. */
    check(unlink(link_path) == 0, "fence: (removed the symlink)");

    /* $XDG_DATA_HOME moves the GTK app's data; the fence follows. */
    char xdg[1024];
    snprintf(xdg, sizeof xdg, "%s/xdg", scratch);
    setenv("XDG_DATA_HOME", xdg, 1);
    snprintf(path, sizeof path, "%s/tasks/tasks.db", xdg);
    check(task_path_is_live_data(path), "fence: follows XDG_DATA_HOME");
    unsetenv("XDG_DATA_HOME");
    check(!task_path_is_live_data(path), "fence: ... and only while it is set");

    /* The database entry points refuse before opening anything. */
    snprintf(path, sizeof path, "%s/.local/share/tasks/tasks.db", home());
    GError *err = NULL;
    check(task_db_open(path, &err) == NULL && err != NULL,
          "fence: task_db_open refuses the live database");
    g_clear_error(&err);

    gchar *detail = NULL;
    check(!task_db_verify_file(path, &detail) && detail != NULL &&
          strstr(detail, "refused") != NULL,
          "fence: task_db_verify_file refuses it");
    g_free(detail);
    check(task_db_file_sha256(path) == NULL, "fence: task_db_file_sha256 refuses it");

    char db_path[1024];
    snprintf(db_path, sizeof db_path, "%s/fence.db", scratch);
    TaskDatabase *db = task_db_open(db_path, NULL);
    check(db != NULL, "fence: (opened a sandbox database)");
    snprintf(path, sizeof path, "%s/.local/share/tasks/copy-from-fork.db", home());
    gchar *cerr = NULL;
    check(!task_db_copy_file(db, path, &cerr) && cerr != NULL,
          "fence: task_db_copy_file refuses a destination in the live directory");
    g_free(cerr);
    task_db_close(db);
}

/*
 * test_config — defaults, write-through and reload.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_config(const char *scratch)
{
    char dir[1024];
    char ini[1100];
    char db[1100];
    struct stat sb;

    check(task_config_get(TASK_CFG_SHOW_COMPLETED) == NULL,
          "config: reads NULL before init");

    snprintf(dir, sizeof dir, "%s/cfg/nested", scratch);
    snprintf(ini, sizeof ini, "%s/tasks.ini", dir);
    snprintf(db, sizeof db, "%s/tasks.db", dir);
    setenv(TASK_DATA_DIR_ENV, dir, 1);

    check(task_config_init(), "config: init succeeds in the override directory");
    check(stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode),
          "config: init creates the data directory (with parents)");
    check(strcmp(task_config_path(), ini) == 0,
          "config: the ini lives in the override directory");

    gchar *dbp = task_db_default_path();
    check(dbp != NULL && strcmp(dbp, db) == 0,
          "config: the override redirects the database too");
    g_free(dbp);
    dbp = task_db_resolve_path(NULL);
    check(dbp != NULL && strcmp(dbp, db) == 0,
          "config: an unset db_dir resolves to the data directory");
    g_free(dbp);

    /* Registry defaults, with no file on disk. */
    check(stat(ini, &sb) != 0, "config: nothing is written until a value changes");
    check(task_config_get_bool(TASK_CFG_SHOW_COMPLETED), "config: show_completed defaults on");
    check(!task_config_get_bool(TASK_CFG_COMPACT), "config: compact_layout defaults off");
    check(task_config_get_bool(TASK_CFG_KANBAN_SHADOW), "config: kanban_shadow defaults on");
    check(!task_config_get_bool(TASK_CFG_COL_STATUS), "config: the Status column defaults hidden");
    check(task_config_get_int(TASK_CFG_BACKUP_INTERVAL_MIN) == 60,
          "config: backup interval defaults to 60");
    check(task_config_get_int(TASK_CFG_WIN_W) == 980 &&
          task_config_get_int(TASK_CFG_WIN_H) == 640,
          "config: window size defaults to 980 x 640");
    check(task_config_get(TASK_CFG_DB_DIR) == NULL, "config: a key with no default reads NULL");
    check(task_config_get_int(TASK_CFG_SIDEBAR_WIDTH) == 0,
          "config: an int with no default reads 0");
    check(!task_config_get_bool("no_such_key"), "config: an unknown bool reads FALSE");

    /* Write-through. */
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    task_config_set_int(TASK_CFG_WIN_W, 1234);
    task_config_set("manual_order_list_7", "3,1,2");
    check(stat(ini, &sb) == 0, "config: a change writes the file");
    check(!task_config_get_bool(TASK_CFG_SHOW_COMPLETED), "config: a stored bool overrides the default");
    check(task_config_get_int(TASK_CFG_WIN_W) == 1234, "config: a stored int reads back");

    /* Reload from disk. */
    task_config_close();
    check(task_config_path() == NULL, "config: closed");
    check(task_config_init(), "config: init again");
    check(!task_config_get_bool(TASK_CFG_SHOW_COMPLETED), "config: bool survives a reload");
    check(task_config_get_int(TASK_CFG_WIN_W) == 1234, "config: int survives a reload");
    gchar *order = task_config_get("manual_order_list_7");
    check(order != NULL && strcmp(order, "3,1,2") == 0, "config: string survives a reload");
    g_free(order);

    /* Removing a key brings its default back; an empty value is unset. */
    task_config_set(TASK_CFG_SHOW_COMPLETED, NULL);
    check(task_config_get_bool(TASK_CFG_SHOW_COMPLETED), "config: a removed key reads as its default");
    task_config_set(TASK_CFG_WIN_W, "");
    check(task_config_get_int(TASK_CFG_WIN_W) == 980, "config: an empty value reads as the default");

    /* A configured db_dir names the directory; the file name is fixed. */
    dbp = task_db_resolve_path("/some/where");
    check(strcmp(dbp, "/some/where/tasks.db") == 0, "config: db_dir resolves to <dir>/tasks.db");
    g_free(dbp);

    task_config_close();
}

/*
 * main — run the path and config tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    char scratch[1024];

    check_scratch_dir(scratch, sizeof scratch);
    test_data_dir(scratch);
    test_fence(scratch);
    test_config(scratch);
    return check_result();
}
