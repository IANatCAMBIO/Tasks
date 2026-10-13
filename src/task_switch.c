#include "task_switch.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shared_db.h"
#include "task_paths.h"
#include "task_worker.h"

/*
 * same_file — whether two paths name one existing file.  Compared by
 * device and inode, so a symbolic link, a hard link, a ".." detour and a
 * different spelling on a case-insensitive volume are all seen through.
 *
 * Inputs:
 *   a, b — the paths
 *
 * Output:
 *   true when both exist and are the same file.
 */
static bool
same_file(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/*
 * task_switch_check — whether the app could switch to `path`.
 * Inputs/output are documented in task_switch.h.
 */
task_switch_check_t
task_switch_check(const TaskApp *app, const char *path, char **why)
{
    gchar *detail = NULL;

    *why = NULL;
    /* The fence is asked first: a path it refuses is not even stat'ed, and
     * task_db_verify_file below answers for it with the refusal text. */
    if (!task_path_is_live_data(path) && same_file(path, shared_db_file(app->db->shared))) {
        return task_switch_already_open;
    }
    if (task_db_verify_file(path, &detail)) return task_switch_can_open;

    *why = strdup(detail != NULL ? detail : "the file could not be checked");
    g_free(detail);
    return task_switch_refused;
}

/*
 * task_switch_database — open `path` and make it the app's database.
 * Inputs/output are documented in task_switch.h.
 */
bool
task_switch_database(TaskApp *app, const char *path, char **why)
{
    if (task_switch_check(app, path, why) != task_switch_can_open) return false;

    GError       *err  = NULL;
    TaskDatabase *next = task_db_open(path, &err);
    if (next == NULL) {
        *why = strdup(err != NULL ? err->message : "the database could not be opened");
        g_clear_error(&err);
        return false;
    }

    /* The new database is open: only now does the old one go. */
    task_db_close(app->db);
    app->db = next;
    task_worker_arm_all(app, shared_db_file(next->shared));
    return true;
}

/*
 * task_switch_can_be_default — whether Make Default applies to `path`.
 * Inputs/output are documented in task_switch.h.
 */
bool
task_switch_can_be_default(const char *path)
{
    char *current    = shared_db_path(NULL);
    bool  is_default = current != NULL && same_file(path, current);
    free(current);
    return !is_default;
}
