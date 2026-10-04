/*
 * task_switch — changing which database file the running app has open
 * (File > Open Database File...).
 *
 * The order is what matters, and it is the reason this lives in the core
 * rather than in a window: the file the user picked is VERIFIED read-only
 * before anything is touched, the new database is OPENED before the old
 * one is closed, and only then are the two swapped.  So a file that cannot
 * be used — a damaged one, one that is not a database, one inside the GTK
 * app's live data directory (task_paths.h) — leaves the app exactly as it
 * was, with nothing to put back.
 *
 * The one protection against opening the wrong file is the LOCATION fence
 * in task_paths.h.  Any sound SQLite file outside it can be opened, and
 * opening it creates this app's tables in it (task_db_open).
 *
 * Plain C; strings returned here are malloc'd and freed with free().
 * MAIN THREAD ONLY (it changes the TaskApp and reads the config).
 */

#ifndef TASK_SWITCH_H
#define TASK_SWITCH_H

#include <stdbool.h>

#include "task_app.h"

/* What task_switch_check found. */
typedef enum {
    task_switch_can_open = 0,       /* a sound database, not the open one  */
    task_switch_already_open,       /* it IS the open database             */
    task_switch_refused             /* it must not, or cannot, be opened   */
} task_switch_check_t;

/*
 * task_switch_check — whether the app could switch to the database at
 * `path`.  Changes nothing: the file is examined on a separate read-only
 * connection (task_db_verify_file), and a path inside the live data
 * directory is refused by its name alone, unopened.
 *
 * Inputs:
 *   app  — the application context, its database open
 *   path — absolute path of the file the user picked
 *   why  — receives the reason when the answer is task_switch_refused (the
 *          core's own words; free it), otherwise NULL
 *
 * Output:
 *   the verdict.
 */
task_switch_check_t task_switch_check(const TaskApp *app, const char *path, char **why);

/*
 * task_switch_database — make the database at `path` the app's open
 * database: check it (task_switch_check), open it, close the old one, and
 * re-arm every background worker against the new file (each timer carries
 * the path it was armed with).
 *
 * The caller closes the task editors FIRST, so their pending saves land in
 * the old database, and afterwards fires task_app_notify_changed.
 *
 * Inputs:
 *   app  — the application context, its database open
 *   path — absolute path of the database to open
 *   why  — receives the reason when it was refused or could not be opened
 *          (free it), otherwise NULL
 *
 * Output:
 *   true when app->db is now the database at `path`.  false when it was
 *   refused, could not be opened, or was already the open one (no reason
 *   is given for that): app->db is then untouched and still open.
 */
bool task_switch_database(TaskApp *app, const char *path, char **why);

/*
 * task_switch_default_dir — the folder to store as TASK_CFG_DB_DIR so that
 * the database at `path` is the one opened at every launch.
 *
 * The setting names a FOLDER, and the file opened in it is always
 * TASK_DB_FILENAME: a database under any other name cannot be the default,
 * and recording its folder would open (or create) a different file there
 * next time.
 *
 * Inputs:
 *   path — absolute path of a database file
 *
 * Output:
 *   new string (free it): the folder holding `path`.  NULL when the file
 *   is not named TASK_DB_FILENAME, or is already the database the config
 *   opens at launch (there is nothing to change).
 */
char *task_switch_default_dir(const char *path);

#endif /* TASK_SWITCH_H */
