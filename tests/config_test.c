/*
 * config_test.c — where the data lives (src/task_paths.h) and the settings
 * file (src/task_config.h): the default directory, the TASKS_DATA_DIR
 * override, the fence round the GTK app's live data, the registry defaults
 * and write-through.
 *
 * The REAL live data directory is only ever NAMED here, and only to the
 * path predicate, which opens nothing.  Every function that could open,
 * create or write something is tested against a FAKE live directory
 * instead — $XDG_DATA_HOME pointed into the sandbox — so that if the fence
 * ever regressed, the test would damage a scratch folder, not real data.
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

    setenv(TASK_DATA_DIR_ENV, "a/./b//../c/", 1);
    char *tidy = task_data_dir();
    check(tidy != NULL && ends_with(tidy, "/a/c") && strstr(tidy, "..") == NULL,
          "data dir: \".\", \"..\" and doubled slashes are collapsed");
    free(tidy);

    setenv(TASK_DATA_DIR_ENV, sandbox, 1);
    free(sandbox);
}

/*
 * fake_live_dir — stand a FAKE live data directory up inside the sandbox:
 * $XDG_DATA_HOME is pointed at <scratch>/xdg, which makes <scratch>/xdg/tasks
 * "the GTK app's data" as far as the fence is concerned.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 *   buf     — receives the fake live directory's path
 *   size    — size of buf
 */
static void
fake_live_dir(const char *scratch, char *buf, size_t size)
{
    char xdg[1024];

    snprintf(xdg, sizeof xdg, "%s/xdg", scratch);
    mkdir(xdg, 0755);
    snprintf(buf, size, "%s/tasks", xdg);
    mkdir(buf, 0755);
    setenv("XDG_DATA_HOME", xdg, 1);
}

/*
 * exists — whether a path exists (links not followed).
 *
 * Inputs:
 *   path — the path
 *
 * Output:
 *   non-zero when it does.
 */
static int
exists(const char *path)
{
    struct stat sb;

    return lstat(path, &sb) == 0;
}

/*
 * test_fence — the path predicate, task_path_is_live_data, against the
 * real live directory's NAME (nothing is opened).
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

    /* ".." after a component that does not exist: the kernel would fail
     * on it, but SQLite collapses it by the text and opens the file. */
    snprintf(path, sizeof path, "%s/no-such-dir/../.local/share/tasks/tasks.db", home());
    check(task_path_is_live_data(path), "fence: \"..\" after a missing component");

    /* $HOME is where GLib looks; it is fenced as well as the passwd home. */
    char *real_home = strdup(getenv("HOME"));
    char  other_home[1024];
    snprintf(other_home, sizeof other_home, "%s/elsewhere", scratch);
    setenv("HOME", other_home, 1);
    snprintf(path, sizeof path, "%s/.local/share/tasks/tasks.db", other_home);
    check(task_path_is_live_data(path), "fence: follows $HOME");
    snprintf(path, sizeof path, "%s/.local/share/tasks/tasks.db", home());
    check(task_path_is_live_data(path), "fence: ... and still the password-database home");
    setenv("HOME", real_home, 1);
    free(real_home);
}

/*
 * test_fence_enforced — every entry point that could open, create or write
 * refuses a path in the live directory, and leaves it untouched.  Run
 * against the FAKE live directory.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_fence_enforced(const char *scratch)
{
    char live[1024];
    char path[1200];
    char link_path[1200];

    fake_live_dir(scratch, live, sizeof live);
    check(task_path_is_live_data(live), "fence: follows XDG_DATA_HOME");

    /* A stand-in for the live database and ini, so that links to them and
     * reads of them can be told from refusals. */
    snprintf(path, sizeof path, "%s/tasks.db", live);
    FILE *f = fopen(path, "w");
    fputs("not a database", f);
    fclose(f);
    char ini[1200];
    snprintf(ini, sizeof ini, "%s/tasks.ini", live);
    f = fopen(ini, "w");
    fputs("[tasks]\nwin_w=4321\n", f);
    fclose(f);

    /* The data directory itself. */
    setenv(TASK_DATA_DIR_ENV, live, 1);
    check(task_data_dir_ensure() == NULL, "enforced: the live directory is refused as data dir");
    check(!task_config_init(), "enforced: config refuses to load from it");

    /* ... reached by a ".." detour through a directory that does not exist
     * (and must not be created on the way). */
    snprintf(path, sizeof path, "%s/missing/../xdg/tasks", scratch);
    setenv(TASK_DATA_DIR_ENV, path, 1);
    check(!task_config_init(), "enforced: a \"..\" detour to it is refused");
    snprintf(path, sizeof path, "%s/missing", scratch);
    check(!exists(path), "enforced: ... and nothing was created on the way");

    /* ... and by a symbolic link. */
    snprintf(link_path, sizeof link_path, "%s/link-to-live", scratch);
    check(symlink(live, link_path) == 0, "enforced: (made a symlink to the live dir)");
    setenv(TASK_DATA_DIR_ENV, link_path, 1);
    check(!task_config_init(), "enforced: a symlink to it is refused");
    snprintf(path, sizeof path, "%s/tasks.db", link_path);
    check(task_path_is_live_data(path), "enforced: a path through the symlink is live data");

    /* A data directory that is fine, holding an ini that is a link to the
     * live ini: the live file must not be read. */
    char clean[1200];
    snprintf(clean, sizeof clean, "%s/clean", scratch);
    mkdir(clean, 0755);
    snprintf(path, sizeof path, "%s/tasks.ini", clean);
    check(symlink(ini, path) == 0, "enforced: (linked an ini to the live ini)");
    setenv(TASK_DATA_DIR_ENV, clean, 1);
    check(!task_config_init(), "enforced: an ini that links into the live directory is refused");
    check(task_config_get_int(TASK_CFG_WIN_W) == 0, "enforced: ... and was not read");
    unlink(path);

    /* The database entry points.  A live file that is itself a link out of
     * the live directory is refused too (the name is inside). */
    snprintf(path, sizeof path, "%s/tasks.db", live);
    GError *err = NULL;
    check(task_db_open(path, &err) == NULL && err != NULL,
          "enforced: task_db_open refuses the live database");
    g_clear_error(&err);

    snprintf(path, sizeof path, "%s/missing/../xdg/tasks/tasks.db", scratch);
    check(task_db_open(path, NULL) == NULL, "enforced: task_db_open refuses a \"..\" detour");

    snprintf(path, sizeof path, "%s/db-link.db", clean);
    char target[1200];
    snprintf(target, sizeof target, "%s/tasks.db", live);
    check(symlink(target, path) == 0, "enforced: (linked a database to the live one)");
    check(task_db_open(path, NULL) == NULL, "enforced: task_db_open refuses a link to it");
    unlink(path);

    gchar *detail = NULL;
    check(!task_db_verify_file(target, &detail) && detail != NULL &&
          strstr(detail, "refused") != NULL,
          "enforced: task_db_verify_file refuses it");
    g_free(detail);
    check(task_db_file_sha256(target) == NULL, "enforced: task_db_file_sha256 refuses it");

    snprintf(path, sizeof path, "%s/good.db", clean);
    TaskDatabase *db = task_db_open(path, NULL);
    check(db != NULL, "enforced: (opened a sandbox database)");
    snprintf(path, sizeof path, "%s/copy-from-fork.db", live);
    gchar *cerr = NULL;
    check(!task_db_copy_file(db, path, &cerr) && cerr != NULL,
          "enforced: task_db_copy_file refuses a destination in the live directory");
    g_free(cerr);
    check(!exists(path), "enforced: ... and wrote nothing there");
    task_db_close(db);

    /* The stand-in live files are exactly as they were made. */
    char  seen[32] = "";
    f = fopen(target, "r");
    fgets(seen, sizeof seen, f);
    fclose(f);
    check(strcmp(seen, "not a database") == 0, "enforced: the live database is untouched");

    /* Removed at once: nothing that later walks the sandbox may follow it. */
    check(unlink(link_path) == 0, "enforced: (removed the symlink)");
    unsetenv("XDG_DATA_HOME");
    snprintf(path, sizeof path, "%s/tasks.db", live);
    check(!task_path_is_live_data(path), "fence: XDG_DATA_HOME counts only while it is set");
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
    struct stat sb;

    check(task_config_get(TASK_CFG_SHOW_COMPLETED) == NULL,
          "config: reads NULL before init");

    snprintf(dir, sizeof dir, "%s/cfg/nested", scratch);
    snprintf(ini, sizeof ini, "%s/tasks.ini", dir);
    setenv(TASK_DATA_DIR_ENV, dir, 1);

    check(task_config_init(), "config: init succeeds in the override directory");
    check(stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode),
          "config: init creates the data directory (with parents)");
    check(strcmp(task_config_path(), ini) == 0,
          "config: the ini lives in the override directory");

    /* The database is not in the data directory: it is the shared file,
     * which CAMBIO_SHARED_DB names (shared_db_path). */
    char *dbp = shared_db_path(NULL);
    check(dbp != NULL && strcmp(dbp, getenv(SHARED_DB_ENV_VAR)) == 0,
          "config: the shared database is the one CAMBIO_SHARED_DB names");
    free(dbp);

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
    check(task_config_get(TASK_CFG_LIST_SORT) == NULL, "config: a key with no default reads NULL");
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
    test_fence_enforced(scratch);
    test_config(scratch);
    return check_result();
}
