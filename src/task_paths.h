/*
 * task_paths — where this program keeps its data, and the one place it must
 * never go.
 *
 * Everything the app stores per user (the database, the ini, the default
 * backup folder) lives in ONE directory, answered by task_data_dir():
 *
 *   1. $TASKS_DATA_DIR when it is set and not empty (the development and
 *      test sandbox: `make run`, `make seed` and `make test` always set it
 *      to a directory under dev/ in the source tree);
 *   2. otherwise ~/Library/Application Support/<TASKS_DATA_DIR_NAME>.
 *
 * The GTK app this was forked from keeps its LIVE data in
 * <XDG data dir>/tasks (~/.local/share/tasks).  This fork is experimental
 * and must not be able to reach that directory by any route — a default,
 * a config value, a file chooser — so task_path_is_live_data() names it and
 * the database and backup code refuse any path it answers YES for.  The
 * fence comes down when the fork is integrated and the owner decides how
 * the data moves across.
 *
 * Plain C, no GLib: strings returned here are malloc'd and freed with
 * free().  Safe from any thread (nothing is cached).
 */

#ifndef TASK_PATHS_H
#define TASK_PATHS_H

#include <stdbool.h>

/* Environment variable that redirects the data directory (see above). */
#define TASK_DATA_DIR_ENV "TASKS_DATA_DIR"

/*
 * task_data_dir — the directory holding the database and the ini.
 *
 * The directory is NOT created here; task_data_dir_ensure does that.  A
 * relative $TASKS_DATA_DIR is made absolute against the current directory,
 * so the answer does not change if the process later changes directory.
 *
 * Output:
 *   new absolute path (free it), or NULL when the home directory cannot be
 *   determined or memory runs out.
 */
char *task_data_dir(void);

/*
 * task_data_dir_ensure — task_data_dir(), created (with parents) if missing.
 *
 * Output:
 *   new absolute path (free it), or NULL when the directory cannot be
 *   determined or created, or lies inside the live data directory.
 */
char *task_data_dir_ensure(void);

/*
 * task_path_is_live_data — whether `path` is, or lies inside, the GTK app's
 * live data directory: <home>/.local/share/tasks, and $XDG_DATA_HOME/tasks
 * when that variable is set.
 *
 * Compared after resolving symbolic links on both sides (the deepest
 * existing ancestor of `path` is resolved, so a file that does not exist
 * yet is still caught), so a link into the live directory does not get
 * round the fence.  Only names are examined; nothing is opened.
 *
 * Inputs:
 *   path — a file or directory path, absolute or relative; NULL is "no"
 *
 * Output:
 *   true when the path must not be touched.  Errs on the side of true: a
 *   path that cannot be resolved at all is refused.
 */
bool task_path_is_live_data(const char *path);

#endif /* TASK_PATHS_H */
