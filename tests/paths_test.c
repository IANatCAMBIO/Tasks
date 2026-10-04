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
#include <pwd.h>
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
 * test_fence_names — spellings of the REAL live directory's name, and of
 * names that only look like it.  The predicate examines names; nothing in
 * that directory is opened, listed or created.
 */
static void
test_fence_names(void)
{
    char path[PATH_MAX];
    char cwd[PATH_MAX];

    snprintf(path, sizeof path, "%s/.local/share/tasks/", home());
    check(task_path_is_live_data(path), "fence: a trailing slash");
    snprintf(path, sizeof path, "%s//.local///share//tasks//tasks.db", home());
    check(task_path_is_live_data(path), "fence: doubled slashes");
    snprintf(path, sizeof path, "%s/./.local/./share/tasks/./tasks.db", home());
    check(task_path_is_live_data(path), "fence: \".\" components");
    snprintf(path, sizeof path, "%s/.local/share/tasks/a/../../tasks/tasks.db", home());
    check(task_path_is_live_data(path), "fence: \"..\" that leaves and comes back");
    snprintf(path, sizeof path, "%s/.local/share/tasks/..", home());
    check(!task_path_is_live_data(path), "fence: \"..\" that leaves for good is outside");

    snprintf(path, sizeof path, "%s/.local/share", home());
    check(!task_path_is_live_data(path), "fence: the directory above it is not live data");
    snprintf(path, sizeof path, "%s/.local/share/tasksX/tasks.db", home());
    check(!task_path_is_live_data(path), "fence: a longer name is another directory");
    snprintf(path, sizeof path, "%s/.local/share/task/tasks.db", home());
    check(!task_path_is_live_data(path), "fence: a shorter name is another directory");
    snprintf(path, sizeof path, "%s/.local/share/tasks.db", home());
    check(!task_path_is_live_data(path), "fence: a file beside it, named like it, is outside");
    snprintf(path, sizeof path, "%s/Library/Application Support/tasks/tasks.db", home());
    check(!task_path_is_live_data(path), "fence: the same name elsewhere is not live data");

    /* A relative path is read against the current directory. */
    check(getcwd(cwd, sizeof cwd) != NULL && chdir(home()) == 0, "fence: (moved to the home directory)");
    check(task_path_is_live_data(".local/share/tasks/tasks.db"), "fence: a relative path into it");
    check(task_path_is_live_data("./.local/share/tasks"), "fence: ... spelled with \"./\"");
    check(!task_path_is_live_data(".local/share/elsewhere"), "fence: a relative path beside it");
    check(!task_path_is_live_data(""), "fence: an empty path is the current directory");
    check(chdir(cwd) == 0, "fence: (moved back)");
}

/*
 * test_fence_links — symbolic links round a FAKE live directory in the
 * sandbox ($XDG_DATA_HOME moves what the fence calls live).  Each link is
 * removed again at once.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_fence_links(const char *scratch)
{
    char store[PATH_MAX], live[PATH_MAX], path[PATH_MAX + 64];
    char link_a[PATH_MAX], link_b[PATH_MAX], outside[PATH_MAX];

    snprintf(store, sizeof store, "%s/store", scratch);
    snprintf(live, sizeof live, "%s/tasks", store);
    snprintf(outside, sizeof outside, "%s/outside", scratch);
    mkdir(store, 0755);
    mkdir(live, 0755);
    mkdir(outside, 0755);
    setenv("XDG_DATA_HOME", store, 1);
    check(task_path_is_live_data(live) && !task_path_is_live_data(outside),
          "links: (the fake live directory is fenced, its neighbour is not)");

    /* A chain of links ending in the live directory. */
    snprintf(link_a, sizeof link_a, "%s/hop-one", scratch);
    snprintf(link_b, sizeof link_b, "%s/hop-two", scratch);
    check(symlink(live, link_b) == 0 && symlink(link_b, link_a) == 0, "links: (made a chain)");
    snprintf(path, sizeof path, "%s/tasks.db", link_a);
    check(task_path_is_live_data(path), "links: a chain of links into it is followed");
    unlink(link_a);
    unlink(link_b);

    /* A link to the directory ABOVE the live one. */
    check(symlink(store, link_a) == 0, "links: (linked the parent)");
    snprintf(path, sizeof path, "%s/tasks/sub/new.db", link_a);
    check(task_path_is_live_data(path), "links: a link to its parent, then its name");
    snprintf(path, sizeof path, "%s/other/new.db", link_a);
    check(!task_path_is_live_data(path), "links: ... but not that link's other children");
    unlink(link_a);

    /* A link INSIDE the live directory that points out of it: the name is
     * inside, so it is refused whatever it resolves to. */
    snprintf(link_a, sizeof link_a, "%s/way-out", live);
    check(symlink(outside, link_a) == 0, "links: (linked out of the live directory)");
    snprintf(path, sizeof path, "%s/file.db", link_a);
    check(task_path_is_live_data(path), "links: a name inside it is live even if it resolves outside");
    unlink(link_a);

    /* $XDG_DATA_HOME itself a link: both of its names are fenced. */
    snprintf(link_a, sizeof link_a, "%s/xdg-link", scratch);
    check(symlink(store, link_a) == 0, "links: (linked the data home)");
    setenv("XDG_DATA_HOME", link_a, 1);
    snprintf(path, sizeof path, "%s/tasks/tasks.db", link_a);
    check(task_path_is_live_data(path), "links: a linked data home, by the link's name");
    snprintf(path, sizeof path, "%s/tasks.db", live);
    check(task_path_is_live_data(path), "links: ... and by the real name");
    unlink(link_a);

    /* An empty $XDG_DATA_HOME is no data home at all. */
    setenv("XDG_DATA_HOME", "", 1);
    check(!task_path_is_live_data(live), "links: an empty XDG_DATA_HOME fences nothing");

    /* The data directory cannot be made inside the live one, however deep,
     * and the attempt creates nothing there. */
    setenv("XDG_DATA_HOME", store, 1);
    char *saved = strdup(getenv(TASK_DATA_DIR_ENV));
    snprintf(path, sizeof path, "%s/deep/er/data", live);
    setenv(TASK_DATA_DIR_ENV, path, 1);
    check(task_data_dir_ensure() == NULL, "links: a data directory below the live one is refused");
    snprintf(path, sizeof path, "%s/deep", live);
    check(!exists(path), "links: ... and none of it is created");
    setenv(TASK_DATA_DIR_ENV, saved, 1);
    free(saved);

    unsetenv("XDG_DATA_HOME");
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
    test_fence_names();
    test_fence_links(scratch);
    return check_result();
}
