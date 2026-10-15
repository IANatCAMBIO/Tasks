/*
 * switch_test.c — changing the open database (src/task_switch.h): what is
 * refused and leaves the app untouched (an old Tasks database included),
 * what a switch does, and that the shared module refuses to change the
 * default while CAMBIO_SHARED_DB names the file.
 *
 * The live data directory is a FAKE one — $XDG_DATA_HOME pointed into the
 * sandbox — as in config_test.c.
 */

#include "fixture.h"
#include "task_switch.h"

#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    task_db_task_create(db, 0, "Elsewhere", "probe");
    task_db_close(db);
}

/*
 * tag_count — how many tags the app's open database has.
 *
 * Inputs:
 *   app — the context
 *
 * Output:
 *   the count.
 */
static int
tag_count(TaskApp *app)
{
    gint tasks = 0, tags = 0;

    task_db_totals(app->db, &tasks, &tags);
    return tags;
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

    check(task_switch_check(app, shared_db_file(app->db->shared), &why) == task_switch_already_open && why == NULL,
          "check: the open database is already open");
    check(!task_switch_database(app, shared_db_file(app->db->shared), &why) && why == NULL && app->db == before,
          "switch: to the open database is a no-op");

    snprintf(path, sizeof path, "%s/notes.db", scratch);
    write_text(path, "this is not a database\n");
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL,
          "check: a file that is not a database is refused, with a reason");
    free(why);
    check(!task_switch_database(app, path, &why) && why != NULL && app->db == before,
          "switch: ... and the open database is untouched");
    free(why);

    /* An old Tasks database (schema v14, no shared stamps): refused in the
     * shared module's words and left untouched. */
    snprintf(path, sizeof path, "%s/tasks.db", scratch);
    sqlite3 *sq = NULL;
    sqlite3_open(path, &sq);
    sqlite3_exec(sq, "CREATE TABLE tasks (id INTEGER PRIMARY KEY);"
                     "PRAGMA user_version = 14", NULL, NULL, NULL);
    sqlite3_close(sq);
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL &&
          strstr(why, "not a shared database") != NULL,
          "check: an old Tasks database is refused in the shared module's words");
    free(why);
    check(!task_switch_database(app, path, &why) && why != NULL && app->db == before,
          "switch: ... and the open database is untouched");
    free(why);

    snprintf(path, sizeof path, "%s/missing.db", scratch);
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL,
          "check: a missing file is refused");
    free(why);
    check(access(path, F_OK) != 0, "check: ... and nothing is created in its place");

    /* An empty file is never a legitimate database (the module creates a
     * missing one complete, in one step): refused, by the switch and by a
     * plain open, and left exactly as it was. */
    snprintf(path, sizeof path, "%s/empty.db", scratch);
    close(open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    check(task_switch_check(app, path, &why) == task_switch_refused && why != NULL,
          "check: an empty file is refused");
    free(why);
    GError       *err   = NULL;
    TaskDatabase *empty = task_db_open(path, &err);
    struct stat   st;
    check(empty == NULL && err != NULL && stat(path, &st) == 0 && st.st_size == 0,
          "open: an empty file is refused and left empty");
    g_clear_error(&err);

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

    check(tag_count(app) == 0, "refusals: the open database still answers");
}

/*
 * test_switch — a real switch, and the default rule under the sandbox.
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

    snprintf(first, sizeof first, "%s", shared_db_file(app->db->shared));
    snprintf(dir, sizeof dir, "%s/elsewhere", scratch);
    mkdir(dir, 0755);

    snprintf(path, sizeof path, "%s/work.db", dir);
    make_database(path);
    check(task_switch_check(app, path, &why) == task_switch_can_open && why == NULL,
          "check: a sound database elsewhere can be opened");
    check(task_switch_database(app, path, &why) && why == NULL &&
          strcmp(shared_db_file(app->db->shared), path) == 0 && tag_count(app) == 1,
          "switch: the app's database is now the one picked");

    /* Make Default is offered for a file that is not the default (here
     * the one CAMBIO_SHARED_DB names), but the shared module refuses it
     * while the variable is set (the tests always set it), saying why:
     * the per-user pointer file must not be written. */
    check(task_switch_can_be_default(path) && !task_switch_can_be_default(first),
          "default: offered for another file, not for the default itself");
    check(shared_db_set_default_path(path, &why) != 0 && why != NULL &&
          strstr(why, SHARED_DB_ENV_VAR) != NULL,
          "default: the module refuses while CAMBIO_SHARED_DB names the database, saying why");
    free(why);

    check(task_switch_database(app, first, &why) && tag_count(app) == 0,
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
