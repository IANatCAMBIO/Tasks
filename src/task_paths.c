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
 * normalized — an absolute path with "//", "." and ".." collapsed by the
 * rules of the text alone, no file system consulted.  This is how SQLite
 * reads a path (it tolerates ".." after a component that does not exist),
 * and it is what creating missing directories turns a path into.
 *
 * Inputs:
 *   abs — an absolute path
 *
 * Output:
 *   new string (free it), "/" at least; NULL when memory runs out.
 */
static char *
normalized(const char *abs)
{
    char *out = malloc(strlen(abs) + 2);
    if (out == NULL) return NULL;

    size_t len = 0;                      /* out[0..len) is the result so far */
    for (const char *p = abs; *p != '\0'; ) {
        while (*p == '/') p++;
        const char *name = p;
        while (*p != '\0' && *p != '/') p++;
        size_t n = (size_t)(p - name);

        if (n == 0 || (n == 1 && name[0] == '.')) continue;
        if (n == 2 && name[0] == '.' && name[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;   /* drop a name */
            if (len > 0) len--;                             /* and its '/' */
            continue;
        }
        out[len++] = '/';
        memcpy(out + len, name, n);
        len += n;
    }
    if (len == 0) out[len++] = '/';
    out[len] = '\0';
    return out;
}

/*
 * absolute — `path` made absolute against the current directory, otherwise
 * exactly as written.
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
 * physical — an absolute path as the kernel would resolve it: symbolic
 * links followed for the deepest part that exists (realpath), the rest —
 * which, not existing, cannot contain links — put back and normalized.
 *
 * Inputs:
 *   abs — an absolute path, not NULL
 *
 * Output:
 *   new string (free it); NULL when the existing part cannot be resolved
 *   for any reason other than "does not exist" (the caller then refuses).
 */
static char *
physical(const char *abs)
{
    char *work = strdup(abs);
    if (work == NULL) return NULL;

    char   real[PATH_MAX];
    size_t cut = strlen(work);           /* work[cut..] is the missing tail */

    for (;;) {
        char saved = work[cut];
        work[cut]  = '\0';
        char *ok   = realpath(cut == 0 ? "/" : work, real);
        int   why  = errno;
        work[cut]  = saved;
        if (ok != NULL) break;
        if (cut == 0 || (why != ENOENT && why != ENOTDIR)) {
            free(work);
            return NULL;
        }
        while (cut > 0 && work[cut - 1] != '/') cut--;   /* drop a name ...  */
        while (cut > 0 && work[cut - 1] == '/') cut--;   /* ... and its '/'  */
    }

    char *joined = join(real, work + cut);
    char *out    = joined != NULL ? normalized(joined) : NULL;

    free(joined);
    free(work);
    return out;
}

/*
 * is_inside — whether `path` is `root` or lies below it.  Compared without
 * regard to case, because the default macOS volume is case-insensitive and
 * a wrong refusal is harmless where a wrong pass is not.
 *
 * Inputs:
 *   path — normalized absolute path
 *   root — normalized absolute directory
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
 * is_under_base — whether any reading of a path lies inside "<base>/tasks".
 * Both the path and the live directory are compared as written AND as
 * resolved, so neither a link into the live directory nor a live file that
 * is itself a link to somewhere else gets past.
 *
 * Inputs:
 *   forms — the candidate path's readings (lexical and physical), each not
 *           NULL
 *   count — how many
 *   base  — an XDG data directory (e.g. "<home>/.local/share"), not NULL
 *
 * Output:
 *   true when one does, or when the live directory cannot be resolved.
 */
static bool
is_under_base(char * const *forms, int count, const char *base)
{
    char *named   = join(base, LIVE_DIR_NAME);
    char *abs     = named != NULL ? absolute(named) : NULL;
    char *written = abs != NULL ? normalized(abs) : NULL;
    char *real    = written != NULL ? physical(written) : NULL;
    bool  inside  = written == NULL || real == NULL;

    for (int i = 0; !inside && i < count; i++) {
        inside = is_inside(forms[i], written) || is_inside(forms[i], real);
    }
    free(real);
    free(written);
    free(abs);
    free(named);
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

    /* Three readings of the path: as written, ".." collapsed by the text
     * (how SQLite and mkdir -p read it); that, with links followed; and as
     * the kernel resolves the original (".." applied AFTER links). */
    char *abs = absolute(path);
    char *forms[3] = { NULL, NULL, NULL };
    if (abs != NULL) {
        forms[0] = normalized(abs);
        forms[1] = forms[0] != NULL ? physical(forms[0]) : NULL;
        forms[2] = physical(abs);
        free(abs);
    }

    char *home = home_dir();
    bool  live = forms[0] == NULL || forms[1] == NULL || forms[2] == NULL || home == NULL;

    /* The live directory hangs off the home directory.  GLib takes that
     * from $HOME, the password database may say otherwise, and
     * $XDG_DATA_HOME moves it: all three are fenced. */
    const char *homes[2] = { home, getenv("HOME") };
    for (int i = 0; !live && i < 2; i++) {
        if (homes[i] == NULL || homes[i][0] == '\0') continue;
        char *base = join(homes[i], ".local/share");
        live = base == NULL || is_under_base(forms, 3, base);
        free(base);
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (!live && xdg != NULL && xdg[0] != '\0') live = is_under_base(forms, 3, xdg);

    free(home);
    for (int i = 0; i < 3; i++) free(forms[i]);
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
    if (env != NULL && env[0] != '\0') {
        char *abs = absolute(env);
        char *dir = abs != NULL ? normalized(abs) : NULL;
        free(abs);
        return dir;
    }

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

    /* Checked again once the directories exist: what a path resolves to is
     * only certain when every component of it is there. */
    if (task_path_is_live_data(dir) || make_dirs(dir) != 0 ||
        task_path_is_live_data(dir)) {
        free(dir);
        return NULL;
    }
    return dir;
}
