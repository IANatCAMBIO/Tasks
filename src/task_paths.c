#include "task_paths.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* The folder name under ~/Library/Application Support.  Defined once, in
 * the Makefile, with the rest of the app's identity. */
#ifndef TASKS_DATA_DIR_NAME
#error "TASKS_DATA_DIR_NAME must be defined by the Makefile"
#endif

/* The GTK app's directory name under the XDG data directory. */
#define LIVE_DIR_NAME "tasks"

/*
 * join — "<a>/<b>" as a new string.
 *
 * Inputs:
 *   a — leading part, not NULL
 *   b — trailing part, not NULL
 *
 * Output:
 *   new string (free it), or NULL when memory runs out.
 */
static char *
join(const char *a, const char *b)
{
    char *out = NULL;

    if (asprintf(&out, "%s/%s", a, b) < 0) return NULL;
    return out;
}

/*
 * home_dir — the user's home directory from the password database, so a
 * changed $HOME cannot move either the default data directory or the fence
 * around the live one.  $HOME is only the fallback for an account with no
 * password entry.
 *
 * Output:
 *   new string (free it), or NULL when neither source has an answer.
 */
static char *
home_dir(void)
{
    struct passwd  pw;
    struct passwd *found = NULL;
    char           buf[4096];

    if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 &&
        found != NULL && found->pw_dir != NULL && found->pw_dir[0] != '\0') {
        return strdup(found->pw_dir);
    }
    const char *env = getenv("HOME");
    return env != NULL && env[0] != '\0' ? strdup(env) : NULL;
}

/*
 * absolute — `path` made absolute against the current directory.
 *
 * Inputs:
 *   path — absolute or relative path, not NULL
 *
 * Output:
 *   new string (free it), or NULL on failure.
 */
static char *
absolute(const char *path)
{
    char cwd[PATH_MAX];

    if (path[0] == '/') return strdup(path);
    if (getcwd(cwd, sizeof cwd) == NULL) return NULL;
    return join(cwd, path);
}

/*
 * resolve — `path` with symbolic links resolved, whether or not it exists:
 * the deepest ancestor that does exist is resolved with realpath() and the
 * missing tail is put back on unchanged.
 *
 * Inputs:
 *   path — absolute or relative path, not NULL
 *
 * Output:
 *   new absolute path (free it), or NULL when no ancestor can be resolved.
 */
static char *
resolve(const char *path)
{
    char *abs = absolute(path);
    if (abs == NULL) return NULL;

    char   real[PATH_MAX];
    size_t cut = strlen(abs);            /* abs[cut..] is the missing tail */

    for (;;) {
        char saved = abs[cut];
        abs[cut]   = '\0';
        char *ok   = realpath(cut == 0 ? "/" : abs, real);
        abs[cut]   = saved;
        if (ok != NULL) break;
        if (cut == 0) {
            free(abs);
            return NULL;
        }
        while (cut > 0 && abs[cut - 1] != '/') cut--;   /* drop a name ...  */
        while (cut > 0 && abs[cut - 1] == '/') cut--;   /* ... and its '/'  */
    }

    const char *tail = abs + cut;
    while (*tail == '/') tail++;
    char *out = *tail != '\0' ? join(strcmp(real, "/") == 0 ? "" : real, tail)
                              : strdup(real);
    free(abs);
    return out;
}

/*
 * is_inside — whether `path` is `root` or lies below it.  Compared without
 * regard to case, because the default macOS volume is case-insensitive and
 * a wrong refusal is harmless where a wrong pass is not.
 *
 * Inputs:
 *   path — resolved absolute path
 *   root — resolved absolute directory
 *
 * Output:
 *   true when path is inside root.
 */
static bool
is_inside(const char *path, const char *root)
{
    size_t n = strlen(root);

    if (strncasecmp(path, root, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

/*
 * is_under_base — whether `resolved` lies inside "<base>/tasks".
 *
 * Inputs:
 *   resolved — the candidate path, already resolved
 *   base     — an XDG data directory (e.g. "<home>/.local/share"), not NULL
 *
 * Output:
 *   true when it does, or when the live directory cannot be resolved.
 */
static bool
is_under_base(const char *resolved, const char *base)
{
    char *root = join(base, LIVE_DIR_NAME);
    if (root == NULL) return true;

    char *real_root = resolve(root);
    bool  inside    = real_root == NULL || is_inside(resolved, real_root);

    free(real_root);
    free(root);
    return inside;
}

/*
 * task_path_is_live_data — the fence round the GTK app's data.
 * Inputs/output are documented in task_paths.h.
 */
bool
task_path_is_live_data(const char *path)
{
    if (path == NULL) return false;

    char *resolved = resolve(path);
    char *home     = home_dir();
    bool  live     = true;               /* unresolvable: refuse */

    if (resolved != NULL && home != NULL) {
        char *xdg_default = join(home, ".local/share");
        live = xdg_default == NULL || is_under_base(resolved, xdg_default);
        free(xdg_default);

        const char *xdg = getenv("XDG_DATA_HOME");
        if (!live && xdg != NULL && xdg[0] != '\0') {
            live = is_under_base(resolved, xdg);
        }
    }
    free(home);
    free(resolved);
    return live;
}

/*
 * task_data_dir — the data directory, not created.
 * Inputs/output are documented in task_paths.h.
 */
char *
task_data_dir(void)
{
    const char *env = getenv(TASK_DATA_DIR_ENV);
    if (env != NULL && env[0] != '\0') return absolute(env);

    char *home = home_dir();
    if (home == NULL) return NULL;

    char *dir = join(home, "Library/Application Support/" TASKS_DATA_DIR_NAME);
    free(home);
    return dir;
}

/*
 * make_dirs — create `path` and any missing parents (mode 0755).
 *
 * Inputs:
 *   path — absolute directory path; modified in place and restored
 *
 * Output:
 *   0 on success (or already present), -1 with errno set on failure.
 */
static int
make_dirs(char *path)
{
    for (char *p = path + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        int rc = mkdir(path, 0755);
        *p = saved;
        if (rc != 0 && errno != EEXIST) return -1;
        if (saved == '\0') return 0;
    }
}

/*
 * task_data_dir_ensure — the data directory, created if missing.
 * Inputs/output are documented in task_paths.h.
 */
char *
task_data_dir_ensure(void)
{
    char *dir = task_data_dir();

    if (dir == NULL) return NULL;
    if (task_path_is_live_data(dir) || make_dirs(dir) != 0) {
        free(dir);
        return NULL;
    }
    return dir;
}
