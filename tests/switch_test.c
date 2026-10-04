/*
 * switch_test.c — changing the open database (src/task_switch.h): what is
 * refused and leaves the app untouched, what a switch does, and which
 * files can be made the default.
 *
 * The live data directory is a FAKE one — $XDG_DATA_HOME pointed into the
 * sandbox — as in config_test.c.
 */

#include "fixture.h"
#include "task_switch.h"

#include <string.h>
#include <sys/stat.h>

/*
 * write_text — create a small file that is not a database.
 *
 * Inputs:
 *   path — where
 *   text — its content
 */
static void
write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        perror(path);
        exit(2);
    }
    fputs(text, file);
    fclose(file);
}

/*
 * make_database — create a database holding one list, then close it.
 *
 * Inputs:
 *   path — where
 */
static void
make_database(const char *path)
{
    TaskDatabase *db = task_db_open(path, NULL);

    if (db == NULL) {
        fprintf(stderr, "cannot create %s\n", path);
        exit(2);
    }
    task_db_list_create(db, "Elsewhere", "");
    task_db_close(db);
}

/*
 * list_count — how many lists the app's open database has.
 *
 * Inputs:
 *   app — the context
 *
 * Output:
 *   the count.
 */
static int
list_count(TaskApp *app)
{
    gint tasks = 0, lists = 0;

    task_db_totals(app->db, &tasks, &lists);
    return lists;
}

/*
 * test_refusals — everything that must leave the open database alone.
 *
 * Inputs:
 *   app     — the context, its database the fixture's empty one
 *   scratch — the fixture's directory
 */
static void
test_refusals(TaskApp *app, const char *scratch)
{
    TaskDatabase *before = app->db;
    char          path[1200];
    char         *why = NULL;

    check(task_switch_check(app, app->db->path, &why) == task_switch_already_open && why == NULL,
          "check: the open database is already open");
    check(!task_switch_database(app, app->db->path, &why) && why == NULL && app->db == before,
          "switch: to the open database is a no-op");

    snprintf(path, sizeof path, "%s/notes.db", scratch);
    write_text(path, "this is not a database\n");
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL,
          "check: a file that is not a database is refused, with a reason");
    free(why);
    check(!task_switch_database(app, path, &why) && why != NULL && app->db == before,
          "switch: ... and the open database is untouched");
    free(why);

    snprintf(path, sizeof path, "%s/missing.db", scratch);
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL,
          "check: a missing file is refused");
    free(why);

    /* A sound database inside the (fake) live directory: only the fence
     * stands between it and being opened. */
    char xdg[1100];
    snprintf(xdg, sizeof xdg, "%s/xdg", scratch);
    mkdir(xdg, 0755);
    snprintf(path, sizeof path, "%s/tasks", xdg);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/tasks/tasks.db", xdg);
    make_database(path);
    setenv("XDG_DATA_HOME", xdg, 1);
    check(task_switch_check(app, path, &why) == task_switch_refused &&
          why != NULL && strstr(why, "refused") != NULL,
          "check: a database in the live data directory is refused in the core's words");
    free(why);
    check(!task_switch_database(app, path, &why) && app->db == before,
          "switch: ... and is never opened");
    free(why);
    unsetenv("XDG_DATA_HOME");

    check(list_count(app) == 0, "refusals: the open database still answers");
}

/*
 * test_switch — a real switch, and the default-folder rule.
 *
 * Inputs:
 *   app     — the context, its database the fixture's empty one
 *   scratch — the fixture's directory
 */
static void
test_switch(TaskApp *app, const char *scratch)
{
    char  first[1200], dir[1100], path[1200];
    char *why = NULL;

    snprintf(first, sizeof first, "%s", app->db->path);
    snprintf(dir, sizeof dir, "%s/elsewhere", scratch);
    mkdir(dir, 0755);

    /* Under another name: it can be opened, but cannot be the default. */
    snprintf(path, sizeof path, "%s/work.db", dir);
    make_database(path);
    check(task_switch_check(app, path, &why) == task_switch_can_open && why == NULL,
          "check: a sound database elsewhere can be opened");
    check(task_switch_database(app, path, &why) && why == NULL &&
          strcmp(app->db->path, path) == 0 && list_count(app) == 1,
          "switch: the app's database is now the one picked");
    check(task_switch_default_dir(path) == NULL,
          "default: a file not named tasks.db cannot be the default");

    /* Named tasks.db: its folder is what the setting would hold. */
    snprintf(path, sizeof path, "%s/%s", dir, TASK_DB_FILENAME);
    make_database(path);
    char *folder = task_switch_default_dir(path);
    check(folder != NULL && strcmp(folder, dir) == 0,
          "default: a tasks.db elsewhere offers its folder");
    free(folder);

    task_config_set(TASK_CFG_DB_DIR, dir);
    check(task_switch_default_dir(path) == NULL,
          "default: the database the config already opens offers nothing");
    task_config_set(TASK_CFG_DB_DIR, NULL);
    check(task_switch_default_dir(first) == NULL,
          "default: nor does the one in the data directory when no folder is set");

    check(task_switch_database(app, first, &why) && list_count(app) == 0,
          "switch: and back again");
}

/*
 * main — run the switch tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();
    char    *scratch = task_data_dir();

    test_refusals(app, scratch);
    test_switch(app, scratch);

    free(scratch);
    fixture_close(app);
    return check_result();
}
