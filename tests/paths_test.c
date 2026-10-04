/*
 * paths_test.c — the path predicates of src/task_paths.h beyond what
 * config_test.c covers: task_path_shares_fate (the backup folder's
 * "alongside the database" warning), and more ways round the fence that
 * must stay shut.
 *
 * As in config_test.c, the REAL live data directory is only ever named to
 * predicates that open nothing; everything that touches the disk does so
 * against a FAKE live directory inside the sandbox.
 */

#include "check.h"
#include "task_paths.h"

#include <limits.h>
#include <string.h>
#include <sys/stat.h>

/*
 * test_shares_fate — containment, by the text of the paths alone.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_shares_fate(const char *scratch)
{
    char cwd[PATH_MAX];
    char inside[PATH_MAX + 16];

    check(task_path_shares_fate("/a/b", "/a/b"), "fate: a directory shares its own");
    check(task_path_shares_fate("/a/b/backups", "/a/b"),
          "fate: a subdirectory shares its parent's (the default backup folder)");
    check(task_path_shares_fate("/a/b/c/d/e", "/a/b"), "fate: at any depth");
    check(!task_path_shares_fate("/a/b", "/a/b/backups"),
          "fate: a parent does not share its child's");
    check(!task_path_shares_fate("/a/backups-old", "/a/backups"),
          "fate: a sibling whose name merely starts the same is elsewhere");
    check(!task_path_shares_fate("/a/b", "/a/c"), "fate: siblings are separate");
    check(!task_path_shares_fate("/Volumes/Backup/tasks", "/Users/someone/data"),
          "fate: another volume is separate");

    check(task_path_shares_fate("/a/./b//backups/", "/a/b"),
          "fate: \".\", doubled and trailing slashes are not differences");
    check(task_path_shares_fate("/a/x/../b/backups", "/a/b"),
          "fate: \"..\" is collapsed before comparing");
    check(!task_path_shares_fate("/a/b/../c", "/a/b"),
          "fate: \"..\" that leads out of the directory is outside it");
    check(task_path_shares_fate("/A/B/Backups", "/a/b"),
          "fate: case differences are not differences (the volume ignores them)");

    check(task_path_shares_fate("/anything/at/all", "/"), "fate: everything is inside the root");
    check(task_path_shares_fate("/", "/"), "fate: the root shares its own");
    check(!task_path_shares_fate("/", "/a"), "fate: the root is inside nothing else");

    check(!task_path_shares_fate(NULL, "/a") && !task_path_shares_fate("/a", NULL) &&
          !task_path_shares_fate(NULL, NULL),
          "fate: NULL is never shared");

    /* Relative paths are read against the current directory. */
    check(getcwd(cwd, sizeof cwd) != NULL, "fate: (read the current directory)");
    check(task_path_shares_fate("some/where", cwd),
          "fate: a relative path is inside the current directory");
    check(task_path_shares_fate("some/where", "."), "fate: ... however that is spelled");
    check(!task_path_shares_fate("../elsewhere", "."),
          "fate: ... and \"..\" from it is outside");

    /* Nothing is consulted on disk: paths that do not exist still answer,
     * and a symbolic link is judged by its own name, not its target. */
    snprintf(inside, sizeof inside, "%s/no/such/dir", scratch);
    check(task_path_shares_fate(inside, scratch), "fate: paths need not exist");

    char target[PATH_MAX + 16];
    char link_path[PATH_MAX + 16];
    snprintf(target, sizeof target, "%s/fate-target", scratch);
    snprintf(link_path, sizeof link_path, "%s/fate-link", scratch);
    mkdir(target, 0755);
    check(symlink(target, link_path) == 0, "fate: (made a symlink)");
    check(!task_path_shares_fate(link_path, target),
          "fate: a link is not resolved to the directory it points at");
    check(task_path_shares_fate(link_path, scratch), "fate: ... it is where its name is");
    unlink(link_path);
}

/*
 * main — run the path tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    char scratch[1024];

    check_scratch_dir(scratch, sizeof scratch);
    test_shares_fate(scratch);
    return check_result();
}
