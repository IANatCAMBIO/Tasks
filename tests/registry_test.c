/*
 * registry_test.c — the settings registry (src/task_config.h), key by key:
 * every fixed key's default, a round trip of every key through the file
 * and back, the typed readers against values a hand could have written,
 * awkward string values, what is and is not written to disk, and a file
 * edited by hand.  config_test.c covers where the file lives; this covers
 * what is in it.
 */

#include "check.h"
#include "task_config.h"

#include <limits.h>
#include <string.h>
#include <sys/stat.h>

/* How a key is read. */
typedef enum { kind_bool, kind_int, kind_text } kind_t;

/* Every fixed key in task_config.h, with the default its comment there
 * documents (NULL = none).  A key added to the header belongs here too. */
static const struct {
    const char *key;
    kind_t      kind;
    const char *def;
} registry[] = {
    { TASK_CFG_BOLD_TITLES,         kind_bool, "0"   },
    { TASK_CFG_KANBAN_SHADOW,       kind_bool, "1"   },
    { TASK_CFG_FORECAST_BUSINESS_DAYS, kind_bool, "1" },
    { TASK_CFG_SHOW_COMPLETED,      kind_bool, "1"   },
    { TASK_CFG_KANBAN_VIEW,         kind_bool, "0"   },
    { TASK_CFG_COMPACT,             kind_bool, "0"   },
    { TASK_CFG_SIDEBAR_VISIBLE,     kind_bool, "1"   },
    { TASK_CFG_SIDEBAR_WIDTH,       kind_int,  NULL  },
    { TASK_CFG_WIN_X,               kind_int,  NULL  },
    { TASK_CFG_WIN_Y,               kind_int,  NULL  },
    { TASK_CFG_WIN_W,               kind_int,  "980" },
    { TASK_CFG_WIN_H,               kind_int,  "640" },
    { TASK_CFG_COMPACT_W,           kind_int,  "600" },
    { TASK_CFG_COMPACT_H,           kind_int,  "500" },
    { TASK_CFG_COMPACT_PRE_W,       kind_int,  "980" },
    { TASK_CFG_COMPACT_PRE_H,       kind_int,  "640" },
    { TASK_CFG_COL_DONE,            kind_bool, "1"   },
    { TASK_CFG_COL_TAGS,            kind_bool, "1"   },
    { TASK_CFG_COL_STATUS,          kind_bool, "0"   },
    { TASK_CFG_COL_DUE,             kind_bool, "1"   },
    { TASK_CFG_COL_SCHEDULE,        kind_bool, "1"   },
    { TASK_CFG_COL_COMPLETED,       kind_bool, "1"   },
    { TASK_CFG_COL_CREATED,         kind_bool, "1"   },
};
#define REGISTRY_COUNT (sizeof(registry) / sizeof(registry[0]))

/*
 * open_config — load the config from a fresh data directory of its own.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 *   name    — the data directory's name under it
 *   ini     — receives the ini file's path
 *   size    — size of ini
 */
static void
open_config(const char *scratch, const char *name, char *ini, size_t size)
{
    char dir[1100];

    snprintf(dir, sizeof dir, "%s/%s", scratch, name);
    snprintf(ini, size, "%s/tasks.ini", dir);
    setenv(TASK_DATA_DIR_ENV, dir, 1);
    if (!task_config_init()) {
        fprintf(stderr, "registry_test: cannot initialise the config in %s\n", dir);
        exit(2);
    }
}

/*
 * reload — close the config and load it again from the same file.
 */
static void
reload(void)
{
    task_config_close();
    if (!task_config_init()) {
        fprintf(stderr, "registry_test: cannot reload the config\n");
        exit(2);
    }
}

/*
 * reads_as — whether a key reads as the given text (NULL = unset).
 *
 * Inputs:
 *   key  — the key
 *   want — the expected value, or NULL
 *
 * Output:
 *   non-zero when it does.
 */
static int
reads_as(const char *key, const char *want)
{
    gchar *got = task_config_get(key);
    int    ok  = (got == NULL && want == NULL) ||
                 (got != NULL && want != NULL && strcmp(got, want) == 0);

    if (!ok) {
        printf("     %s: got \"%s\", wanted \"%s\"\n", key, got != NULL ? got : "(null)",
               want != NULL ? want : "(null)");
    }
    g_free(got);
    return ok;
}

/*
 * file_holds — whether a file contains a piece of text.
 *
 * Inputs:
 *   path — the file
 *   text — the text looked for
 *
 * Output:
 *   non-zero when it does.
 */
static int
file_holds(const char *path, const char *text)
{
    char   buf[8192];
    FILE  *f = fopen(path, "r");
    size_t n;

    if (f == NULL) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

/*
 * test_defaults — with no file, every key reads as its documented default
 * through every reader.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_defaults(const char *scratch)
{
    char ini[1200];
    int  text_ok = 1, bool_ok = 1, int_ok = 1;

    open_config(scratch, "defaults", ini, sizeof ini);
    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        const char *def = registry[i].def;

        text_ok = text_ok && reads_as(registry[i].key, def);
        bool_ok = bool_ok && (task_config_get_bool(registry[i].key) != FALSE) ==
                             (def != NULL && strcmp(def, "0") != 0);
        int_ok  = int_ok && task_config_get_int(registry[i].key) == (def != NULL ? atoi(def) : 0);
    }
    check(text_ok, "defaults: every key reads as its documented default");
    check(bool_ok, "defaults: ... as a bool (no default is FALSE)");
    check(int_ok, "defaults: ... as an int (no default is 0)");

    check(reads_as("no_such_key", NULL) && task_config_get_int("no_such_key") == 0,
          "defaults: an unknown key has none");
    check(reads_as(TASK_CFG_FAMILY_MANUAL "_list_12", NULL) &&
          reads_as(TASK_CFG_FAMILY_KANBAN "_group_3", NULL) &&
          reads_as(TASK_CFG_FAMILY_MANUAL "_today", NULL),
          "defaults: per-view order keys have none");
    task_config_close();
}

/*
 * test_round_trip — every key: a value that is not its default is read
 * back at once and after a reload, and removing the key brings the default
 * back, also across a reload.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_round_trip(const char *scratch)
{
    char ini[1200];
    char value[REGISTRY_COUNT][64];
    int  live_ok = 1, reload_ok = 1, removed_ok = 1, gone_ok = 1;

    open_config(scratch, "round-trip", ini, sizeof ini);
    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        const char *def = registry[i].def;

        /* Written through the setter of the key's own type. */
        switch (registry[i].kind) {
        case kind_bool: {
            gboolean flipped = def == NULL || strcmp(def, "0") == 0;
            snprintf(value[i], sizeof value[i], "%d", flipped ? 1 : 0);
            task_config_set_bool(registry[i].key, flipped);
            live_ok = live_ok && (task_config_get_bool(registry[i].key) != FALSE) == (flipped != FALSE);
            break;
        }
        case kind_int: {
            gint moved = (def != NULL ? atoi(def) : 0) + 7 + (gint)i;
            snprintf(value[i], sizeof value[i], "%d", moved);
            task_config_set_int(registry[i].key, moved);
            live_ok = live_ok && task_config_get_int(registry[i].key) == moved;
            break;
        }
        case kind_text:
            snprintf(value[i], sizeof value[i], "/some/folder %d/with spaces", (int)i);
            task_config_set(registry[i].key, value[i]);
            break;
        }
        live_ok = live_ok && reads_as(registry[i].key, value[i]);
    }
    check(live_ok, "round trip: every key reads back what its setter wrote");

    reload();
    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        reload_ok = reload_ok && reads_as(registry[i].key, value[i]);
    }
    check(reload_ok, "round trip: ... and again after a reload from disk");

    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        task_config_set(registry[i].key, NULL);
        removed_ok = removed_ok && reads_as(registry[i].key, registry[i].def);
    }
    check(removed_ok, "round trip: removing a key brings its default back");

    reload();
    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        gone_ok = gone_ok && reads_as(registry[i].key, registry[i].def) &&
                  !file_holds(ini, registry[i].key);
    }
    check(gone_ok, "round trip: ... and the key is gone from the file");

    /* Per-view order keys are ordinary keys with no default. */
    task_config_set(TASK_CFG_FAMILY_MANUAL "_list_12", "9,4,7");
    task_config_set(TASK_CFG_FAMILY_KANBAN "_group_3", "1");
    task_config_set(TASK_CFG_FAMILY_KANBAN "_all", "5,6");
    reload();
    check(reads_as(TASK_CFG_FAMILY_MANUAL "_list_12", "9,4,7") &&
          reads_as(TASK_CFG_FAMILY_KANBAN "_group_3", "1") &&
          reads_as(TASK_CFG_FAMILY_KANBAN "_all", "5,6") &&
          reads_as(TASK_CFG_FAMILY_KANBAN "_list_12", NULL),
          "round trip: order keys of both families, each its own");
    task_config_set(TASK_CFG_FAMILY_MANUAL "_list_12", NULL);
    reload();
    check(reads_as(TASK_CFG_FAMILY_MANUAL "_list_12", NULL) &&
          reads_as(TASK_CFG_FAMILY_KANBAN "_all", "5,6"),
          "round trip: dropping one order leaves the others");
    task_config_close();
}

/*
 * test_typed_readers — the bool and int readers against stored text of
 * every shape, including what only a hand edit could put there.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_typed_readers(const char *scratch)
{
    char ini[1200];

    open_config(scratch, "typed", ini, sizeof ini);

    /* A bool is FALSE only for "0"; the default applies only when unset. */
    task_config_set(TASK_CFG_KANBAN_SHADOW, "0");
    check(!task_config_get_bool(TASK_CFG_KANBAN_SHADOW), "bool: \"0\" is off, over a default of on");
    task_config_set(TASK_CFG_BOLD_TITLES, "1");
    check(task_config_get_bool(TASK_CFG_BOLD_TITLES), "bool: \"1\" is on, over a default of off");
    task_config_set(TASK_CFG_BOLD_TITLES, "yes");
    check(task_config_get_bool(TASK_CFG_BOLD_TITLES), "bool: any other text is on");
    task_config_set(TASK_CFG_BOLD_TITLES, "false");
    check(task_config_get_bool(TASK_CFG_BOLD_TITLES), "bool: ... even \"false\" (only \"0\" is off)");
    task_config_set(TASK_CFG_KANBAN_SHADOW, "");
    check(task_config_get_bool(TASK_CFG_KANBAN_SHADOW), "bool: an empty value is unset: the default");
    task_config_set(TASK_CFG_BOLD_TITLES, "");
    check(!task_config_get_bool(TASK_CFG_BOLD_TITLES), "bool: ... whichever way the default goes");

    /* An int is atoi of the text. */
    task_config_set(TASK_CFG_WIN_W, "1200");
    check(task_config_get_int(TASK_CFG_WIN_W) == 1200, "int: digits");
    task_config_set(TASK_CFG_WIN_X, "-250");
    check(task_config_get_int(TASK_CFG_WIN_X) == -250, "int: a negative number");
    task_config_set(TASK_CFG_WIN_W, "640px");
    check(task_config_get_int(TASK_CFG_WIN_W) == 640, "int: digits then text reads the digits");
    task_config_set(TASK_CFG_WIN_W, "wide");
    check(task_config_get_int(TASK_CFG_WIN_W) == 0, "int: text reads 0, not the default");
    task_config_set(TASK_CFG_WIN_W, "");
    check(task_config_get_int(TASK_CFG_WIN_W) == 980, "int: an empty value is unset: the default");

    task_config_set_int(TASK_CFG_WIN_X, INT_MAX);
    reload();
    check(task_config_get_int(TASK_CFG_WIN_X) == INT_MAX, "int: INT_MAX survives the file");
    task_config_set_int(TASK_CFG_WIN_X, INT_MIN);
    reload();
    check(task_config_get_int(TASK_CFG_WIN_X) == INT_MIN, "int: INT_MIN survives the file");
    task_config_set_int(TASK_CFG_WIN_X, 0);
    reload();
    check(reads_as(TASK_CFG_WIN_X, "0"), "int: a stored 0 is a value, not \"unset\"");
    task_config_close();
}

/*
 * test_awkward_text — string values that an ini file could mangle come
 * back from disk exactly as they were set.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_awkward_text(const char *scratch)
{
    static const struct {
        const char *value;
        const char *what;
    } cases[] = {
        { "/Users/someone/My Backups/tasks", "text: spaces inside" },
        { "  padded on both sides  ",         "text: leading and trailing spaces" },
        { "3,1,2,40,7",                       "text: commas" },
        { "a=b;c#d[e]",                       "text: ini punctuation" },
        { "C:\\not\\a\\mac\\path",            "text: backslashes" },
        { "tab\there",                        "text: a tab" },
        { "line one\nline two",               "text: a line break" },
        { "\xc3\x89" "criture \xe2\x9c\x93 \xf0\x9f\x93\x8e", "text: UTF-8, emoji included" },
        { "0",                                "text: a lone zero" },
    };
    char ini[1200];
    char long_value[4001];

    open_config(scratch, "awkward", ini, sizeof ini);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        task_config_set(TASK_CFG_GCAL_PUSH_CALENDAR, cases[i].value);
        reload();
        check(reads_as(TASK_CFG_GCAL_PUSH_CALENDAR, cases[i].value), cases[i].what);
    }

    memset(long_value, 'x', sizeof long_value - 1);
    long_value[sizeof long_value - 1] = '\0';
    task_config_set(TASK_CFG_GCAL_PUSH_CALENDAR, long_value);
    reload();
    check(reads_as(TASK_CFG_GCAL_PUSH_CALENDAR, long_value), "text: 4000 characters");
    task_config_close();
}

/*
 * test_write_through — when the file is written, and when it is not.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_write_through(const char *scratch)
{
    char        ini[1200];
    struct stat before, after;

    /* Nothing is loaded: readers answer "unset", writers do nothing. */
    task_config_set(TASK_CFG_WIN_W, "1");
    task_config_set_bool(TASK_CFG_COMPACT, TRUE);
    check(task_config_get(TASK_CFG_WIN_W) == NULL && !task_config_get_bool(TASK_CFG_COMPACT) &&
          task_config_get_int(TASK_CFG_WIN_W) == 0 && task_config_path() == NULL,
          "write: before init, nothing is read or written");

    open_config(scratch, "write", ini, sizeof ini);
    check(reads_as(TASK_CFG_WIN_W, "980"), "write: ... and the early writes left no trace");

    /* Reading, and removing what is not there, write nothing. */
    task_config_set(TASK_CFG_WIN_W, NULL);
    task_config_set(TASK_CFG_GCAL_PUSH_CALENDAR, NULL);
    check(stat(ini, &before) != 0, "write: removing an absent key does not create the file");

    task_config_set_int(TASK_CFG_WIN_W, 1000);
    check(stat(ini, &before) == 0 && file_holds(ini, "[tasks]") && file_holds(ini, "win_w=1000"),
          "write: the first change creates the file, group and key");

    /* The file is replaced on every real write, so an unchanged inode
     * means no write happened. */
    task_config_set_int(TASK_CFG_WIN_W, 1000);
    task_config_set(TASK_CFG_WIN_W, "1000");
    check(stat(ini, &after) == 0 && after.st_ino == before.st_ino,
          "write: setting the value already stored writes nothing");

    task_config_set_int(TASK_CFG_WIN_W, 1001);
    check(file_holds(ini, "win_w=1001") && !file_holds(ini, "win_w=1000"),
          "write: a changed value is on disk at once");

    /* A default written explicitly is stored: unset and "default" differ
     * on disk even though they read the same. */
    task_config_set_int(TASK_CFG_WIN_H, 640);
    check(file_holds(ini, "win_h=640"), "write: a value equal to the default is still stored");

    /* A second init keeps what is loaded; close then forgets it. */
    check(task_config_init() && task_config_get_int(TASK_CFG_WIN_W) == 1001,
          "write: a second init is a no-op");
    task_config_close();
    task_config_set_int(TASK_CFG_WIN_W, 5);
    check(task_config_get(TASK_CFG_WIN_W) == NULL && file_holds(ini, "win_w=1001"),
          "write: after close, nothing is read or written");
}

/*
 * write_file — put text into a file.
 *
 * Inputs:
 *   path — the file
 *   text — its whole content
 */
static void
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/*
 * test_hand_edited — a file someone edited: spacing, unknown keys and
 * other groups are tolerated and survive a rewrite; a file that is not an
 * ini at all reads as the defaults.
 *
 * Inputs:
 *   scratch — a directory inside the sandbox
 */
static void
test_hand_edited(const char *scratch)
{
    char ini[1200];

    /* The data directory has to exist before the file can be put in it. */
    open_config(scratch, "hand", ini, sizeof ini);
    task_config_close();
    write_file(ini,
               "# edited by hand\n"
               "[tasks]\n"
               "win_w = 1111\n"
               "bold_task_titles=1\n"
               "show_completed=\n"
               "future_setting=keep me\n"
               "\n"
               "[other]\n"
               "win_h=5\n");
    reload();
    check(task_config_get_int(TASK_CFG_WIN_W) == 1111, "hand: spaces round \"=\" are tolerated");
    check(task_config_get_bool(TASK_CFG_BOLD_TITLES), "hand: a key set by hand is read");
    check(task_config_get_bool(TASK_CFG_SHOW_COMPLETED), "hand: a key left empty reads as its default");
    check(task_config_get_int(TASK_CFG_WIN_H) == 640, "hand: a key in another group is not ours");
    check(reads_as("future_setting", "keep me"), "hand: a key this build does not know is readable");

    task_config_set_int(TASK_CFG_WIN_W, 1112);
    check(file_holds(ini, "future_setting=keep me") && file_holds(ini, "[other]") &&
          file_holds(ini, "win_h=5"),
          "hand: unknown keys and other groups survive a rewrite");
    task_config_close();

    /* Not an ini at all. */
    write_file(ini, "this is not\n\tan ini file = [[[\n");
    check(task_config_init(), "hand: a file that cannot be parsed does not stop the app");
    check(reads_as(TASK_CFG_WIN_W, "980") && task_config_get_bool(TASK_CFG_KANBAN_SHADOW),
          "hand: ... every key reads as its default");
    task_config_set_bool(TASK_CFG_COMPACT, TRUE);
    reload();
    check(task_config_get_bool(TASK_CFG_COMPACT), "hand: ... and settings can be saved again");
    task_config_close();
}

/*
 * main — run the registry tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    char scratch[1024];

    check_scratch_dir(scratch, sizeof scratch);
    test_write_through(scratch);
    test_defaults(scratch);
    test_round_trip(scratch);
    test_typed_readers(scratch);
    test_awkward_text(scratch);
    test_hand_edited(scratch);
    return check_result();
}
