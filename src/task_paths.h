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
 * $TASKS_DATA_DIR is made absolute against the current directory and has
 * its "." and ".." collapsed, so the answer does not change if the process
 * later changes directory and every user of the path reads it the same way.
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
 * live data directory: <home>/.local/share/tasks — for the home directory
 * of the password database and for $HOME — and $XDG_DATA_HOME/tasks when
 * that variable is set.
 *
 * The path is judged both AS WRITTEN (".." collapsed by the text alone,
 * which is how SQLite reads it, even past a component that does not exist)
 * and AS RESOLVED (symbolic links followed for the part that exists), and
 * so is the live directory: a link into it, a ".." detour and a live file
 * that is itself a link elsewhere are all refused.  Only names are
 * examined; nothing is opened.
 *
 * The fence knows the live directory by its LOCATION.  It cannot recognise
 * a GTK database kept somewhere else (a db_dir the GTK app was pointed at).
 *
 * Inputs:
 *   path — a file or directory path, absolute or relative; NULL is "no"
 *
 * Output:
 *   true when the path must not be touched.  Errs on the side of true: a
 *   path that cannot be resolved at all is refused.
 */
bool task_path_is_live_data(const char *path);

/*
 * task_path_shares_fate — whether `dir` is `other`, or sits inside it: the
 * test behind the backup destination's warning in Settings.
 *
 * It is a containment test, not a comparison, because the DEFAULT backup
 * folder is a `backups/` directory inside the database's own: a child goes
 * to the trash at the same moment as its parent, so it is the one
 * arrangement the warning most needs to describe.
 *
 * Both paths are made absolute and have "//", "." and ".." collapsed by the
 * text alone, so "/a/b" and "/a/./b" are one answer, and "/a/backups-old"
 * is not read as living inside "/a/backups".  Compared without regard to
 * case, as the fence is.  Symbolic links are NOT resolved: that needs the
 * paths to exist, and would make the answer depend on whether a removable
 * disk happens to be plugged in.  Nothing is opened.
 *
 * Inputs:
 *   dir   — the directory asked about; NULL is "no"
 *   other — the directory it might share the fate of; NULL is "no"
 *
 * Output:
 *   true when losing `other` would lose `dir` too.  false also when a path
 *   cannot be made absolute or memory runs out.
 */
bool task_path_shares_fate(const char *dir, const char *other);

#endif /* TASK_PATHS_H */
