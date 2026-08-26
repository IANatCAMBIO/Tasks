/* ===========================================================================
 * plugin.h — the Tasks plugin ABI.
 *
 * A plugin is a shared object (.so on Linux, .dylib on macOS) in the
 * plugins/ directory next to the binary.  It exports exactly ONE symbol,
 * task_plugin_entry(), and imports NOTHING from the host: everything it
 * can do arrives in the TaskHostApi table handed to that entry point.
 *
 * WHY A TABLE AND NOT LINKED SYMBOLS
 * ----------------------------------
 * The obvious alternative — export the host's symbols and let the plugin
 * link against them — was rejected on three counts, in this order:
 *
 *   1. COST TO THE HOST.  Making the core's own code reachable from a
 *      shared object means either building it as a shared library (every
 *      task_db_* call the app makes to itself becomes an indirect call,
 *      forever, whether or not a single plugin is installed) or exporting
 *      a large dynamic symbol table.  The app must not pay for plugins it
 *      is not using.  With a table, the core calls itself directly and
 *      the cost lands entirely on the plugin.
 *   2. PORTABILITY.  Symbol resolution across a dlopen boundary differs
 *      between ELF and Mach-O in ways that need per-platform link flags.
 *      A struct of function pointers behaves identically on both.
 *   3. HONESTY.  The table IS the ABI.  What a plugin may call is a
 *      readable list rather than "whatever happened to be exported".
 *
 * THE SHARED FLOOR
 * ----------------
 * A plugin brings its own dependencies — libcurl, a JSON parser, crypto,
 * whatever it needs — with ONE exception it must not violate: GTK, GLib
 * and SQLite belong to the HOST and must never be bundled.  Plugins
 * receive live GtkWidget* and TaskDatabase* (which wraps a live sqlite3*).
 * Two GLibs in one process means two GObject type systems and an
 * immediate crash; two SQLite builds operating on one handle is
 * undefined behaviour, which — given how this database has been lost
 * before — is the one failure mode worth being paranoid about.  That is
 * also why no sqlite3 type appears anywhere in this header: a plugin
 * never links SQLite, it asks the host.
 *
 * PERFORMANCE RULES — these are requirements, not advice
 * ------------------------------------------------------
 *   * NOTHING a plugin supplies may run in a per-DRAW path.  GTK cell
 *     data functions run on every draw, not once per refresh; a call
 *     across the plugin boundary there would run thousands of times a
 *     second.  Every hook here is therefore BATCH-shaped: a plugin is
 *     handed the whole set once and answers once.
 *   * init() must not block.  It runs before the window is shown, so
 *     network access, a subprocess or a large query there is dead time
 *     the user watches.  Register, and do the work on a worker.
 *   * Never touch the database from a thread that did not open the
 *     connection.  Use worker_arm and open your own (see db.open).
 *   * The loader times every plugin's load and init and warns about a
 *     slow one, so "which plugin made this feel slow" has an answer.
 *
 * ABI COMPATIBILITY
 * -----------------
 * The host tells the plugin its abi_version, the size of Task, and the
 * size of the API table.  A plugin built against a different
 * TASK_PLUGIN_ABI_VERSION is refused at load, loudly.  Task grows only
 * by APPENDING fields, so a plugin compiled against an older header
 * simply does not see the newer ones; sizeof is checked so a mismatch
 * fails at load rather than silently reading past the end of a struct.
 * The API table grows the same way — new groups are appended, and
 * host_api_size says how much of it is real.
 * =========================================================================== */

#ifndef TASK_PLUGIN_H
#define TASK_PLUGIN_H

#include "db.h"
#include "task_view.h"
#include "task_ops.h"
#include "task_worker.h"

/* Bumped on ANY incompatible change to the structs below.                  */
#define TASK_PLUGIN_ABI_VERSION 1u

/* The directory plugins are loaded from, relative to the executable.       */
#define TASK_PLUGIN_DIR "plugins"

typedef struct TaskPlugin    TaskPlugin;
typedef struct TaskHostApi   TaskHostApi;

/* ---------------------------------------------------------------------------
 * Notification — the app's three change events (see app.h).
 *
 * A plugin subscribes to learn that something changed; it does NOT get a
 * description of what.  That is deliberate and is the crash-safety rule
 * the two built-in integrations already follow: an event is a HINT to
 * look, and the DATABASE is what says what to do.  A plugin that treated
 * events as a work queue would lose work whenever the app died between a
 * commit and its callback.
 *
 * Every function here is MAIN THREAD ONLY.  From a worker, marshal
 * first.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Post a one-line message to the status bar.  Plain text, NOT markup
     * — the status bar is a plain-text label, so "&" must stay "&".      */
    void  (*status)(TaskApp *app, const gchar *message);

    void  (*notify_changed)(TaskApp *app);   /* full refresh              */
    void  (*notify_tasks)(TaskApp *app);     /* task pane only            */

    guint (*listen_changed)(TaskApp *app, TaskAppNotifyFn fn, gpointer d);
    guint (*listen_tasks)(TaskApp *app, TaskAppNotifyFn fn, gpointer d);
    guint (*listen_status)(TaskApp *app, TaskAppStatusFn fn, gpointer d);
    void  (*unlisten)(TaskApp *app, guint id);

    /* Run `fn` on the MAIN thread soon, then free `d` with `free_fn`.
     * The one safe way out of a worker.  Wraps g_idle_add so a plugin
     * needs no GLib main-loop knowledge of its own.                      */
    void  (*invoke_main)(GSourceFunc fn, gpointer d, GDestroyNotify free_fn);
} TaskHostNotify;

/* ---------------------------------------------------------------------------
 * Config — the ini.
 *
 * A plugin's own keys are AUTOMATICALLY namespaced by its id: a plugin
 * with id "gtasks" asking for "interval_min" reads "gtasks_interval_min".
 * It therefore cannot collide with the app's keys or another plugin's,
 * and does not need to know what those are.
 *
 * `global_*` reads an app-owned key by its exact name — for the handful
 * a plugin genuinely shares, like "show_completed".  There is no global
 * SETTER on purpose: a plugin must not rewrite the app's settings.
 *
 * Main thread only.  The store is one in-memory GKeyFile with no lock,
 * and a worker touching it is a data race.  Read what you need before
 * starting one.
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar    *(*get)(const TaskPlugin *self, const gchar *key);
    void      (*set)(const TaskPlugin *self, const gchar *key,
                     const gchar *value);
    gboolean  (*get_bool)(const TaskPlugin *self, const gchar *key,
                          gboolean def);

    gchar    *(*global_get)(const gchar *key);
    gboolean  (*global_get_bool)(const gchar *key, gboolean def);
} TaskHostConfig;

/* ---------------------------------------------------------------------------
 * Database.
 *
 * `main_db` is the app's connection and is MAIN THREAD ONLY.  A worker
 * opens its OWN connection with `open` on the path from `path` — a
 * connection must never cross threads.
 *
 * The typed calls are the whole of the app's own data model.  For a
 * plugin's PRIVATE tables (a link table keyed by task id is the expected
 * shape) use `exec` and `exec_query`, which are sqlite3_exec without the
 * sqlite3 types.  Create such a table from the plugin's db_open hook.
 *
 * PERFORMANCE: `exec_query` is the batch escape hatch precisely so a
 * plugin can fetch a whole side table in ONE statement rather than
 * querying per task.  Per-task queries in a refresh are the mistake this
 * API exists to make avoidable.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskDatabase *(*main_db)(TaskApp *app);
    const gchar  *(*path)(TaskApp *app);
    TaskDatabase *(*open)(const gchar *path, GError **err);
    void          (*close)(TaskDatabase *db);

    /* Tasks. */
    Task       *(*task_get)(TaskDatabase *db, gint64 id);
    gint64      (*task_create)(TaskDatabase *db, gint64 list_id,
                               gint64 parent_id, const gchar *title);
    void        (*task_delete)(TaskDatabase *db, gint64 id);
    void        (*task_purge)(TaskDatabase *db, gint64 id);
    void        (*task_set_status)(TaskDatabase *db, gint64 id,
                                   TaskStatus status);
    GPtrArray  *(*tasks_toplevel)(TaskDatabase *db, gint64 list_id);
    GPtrArray  *(*tasks_all_visible)(TaskDatabase *db);
    GPtrArray  *(*tasks_pinned)(TaskDatabase *db);
    GPtrArray  *(*tasks_due_between)(TaskDatabase *db, gint64 lo, gint64 hi);
    GPtrArray  *(*subtasks)(TaskDatabase *db, gint64 parent_id);
    void        (*task_free)(Task *t);
    void        (*tasks_free)(GPtrArray *a);

    /* Lists. */
    GPtrArray  *(*lists)(TaskDatabase *db, gboolean include_deleted);
    TaskList   *(*list_get)(TaskDatabase *db, gint64 id);
    gint64      (*list_create)(TaskDatabase *db, const gchar *name,
                               const gchar *emoji);
    void        (*list_free)(TaskList *l);
    void        (*lists_free)(GPtrArray *a);

    /* The app's own key/value table, for a plugin's sync cursors and
     * stamps.  Keys are NOT namespaced — prefix them yourself.          */
    gchar      *(*state_get)(TaskDatabase *db, const gchar *key);
    void        (*state_set)(TaskDatabase *db, const gchar *key,
                             const gchar *value);

    /* A plugin's own tables.  `exec` runs statements with no result and
     * returns FALSE on failure (the host logs sqlite's message).
     * `exec_query` is sqlite3_exec's callback shape: return non-zero
     * from `cb` to abort.  Both take the plugin's OWN connection when
     * called from a worker.                                            */
    gboolean   (*exec)(TaskDatabase *db, const gchar *sql);
    gboolean   (*exec_query)(TaskDatabase *db, const gchar *sql,
                             gint (*cb)(gpointer d, gint n_cols,
                                        gchar **values, gchar **names),
                             gpointer user_data);
    /* Quote a string as a SQL literal — sqlite3_mprintf's %Q, so a
     * plugin never hand-rolls escaping.  g_free the result.            */
    gchar      *(*quote)(const gchar *s);
} TaskHostDb;

/* ---------------------------------------------------------------------------
 * Background work.  A plugin's periodic pass goes through the app's one
 * scheduler (see task_worker.h), which owns the timer, the db path and
 * the re-arm on a database switch — so a plugin cannot be left pointing
 * at a file that has moved.
 * ------------------------------------------------------------------------- */
typedef struct {
    void (*register_worker)(const TaskWorkerDef *def);
    void (*arm)(TaskApp *app, const TaskWorkerDef *def, const gchar *db_path);
} TaskHostWorker;

/* ---------------------------------------------------------------------------
 * Sidebar views and core operations — the same registries the app uses
 * for its own (see task_view.h, task_ops.h).
 * ------------------------------------------------------------------------- */
typedef struct {
    void (*register_view)(const TaskView *v);
} TaskHostViews;

typedef struct {
    gboolean (*move_to_list)(TaskApp *app, gint64 task_id, gint64 dest_list);
    guint    (*clear_completed)(TaskApp *app, gint64 list_id);

    void     (*add_moved_hook)(TaskOpsMovedFn fn, gpointer d);
    void     (*add_cleared_hook)(TaskOpsClearedFn fn, gpointer d);
    void     (*add_list_veto)(TaskOpsListVetoFn fn, gpointer d);
    void     (*add_delete_hook)(TaskDbDeleteSqlFn fn, gpointer d);
} TaskHostOps;

/* ---------------------------------------------------------------------------
 * The API table itself.  Groups are POINTERS so a new group is an
 * append and an old plugin keeps working; `host_api_size` says how much
 * of the table the host actually filled.
 * ------------------------------------------------------------------------- */
struct TaskHostApi {
    guint32 abi_version;             /* TASK_PLUGIN_ABI_VERSION            */
    gsize   task_struct_size;        /* sizeof(Task) as the HOST sees it   */
    gsize   host_api_size;           /* sizeof(TaskHostApi), ditto         */
    const gchar *host_version;       /* TASK_VERSION, for diagnostics      */

    const TaskHostNotify *notify;
    const TaskHostConfig *config;
    const TaskHostDb     *db;
    const TaskHostWorker *worker;
    const TaskHostViews  *views;
    const TaskHostOps    *ops;
};

/* ---------------------------------------------------------------------------
 * What a plugin IS.
 *
 * `id` is the plugin's identity: its config namespace, the name in the
 * Settings list, and how the loader reports it.  It is part of the ini
 * format once shipped — do not rename it.
 *
 * LIFECYCLE
 *   init      — the app is built, no window yet.  Register views,
 *               workers and hooks here.  MUST BE CHEAP (see the
 *               performance rules above).  Return FALSE to decline
 *               loading, e.g. a dependency is missing; the app carries
 *               on without the plugin and says so.
 *   db_open   — a database has been opened.  Create the plugin's own
 *               tables here (CREATE TABLE IF NOT EXISTS).  Called for
 *               the main connection at startup and again after a
 *               database switch.
 *   db_closing— that database is about to go.  Drop cached handles.
 *   shutdown  — the app is quitting.  Nothing is refreshed afterwards.
 *
 * `enabled_default` is consulted only the first time; after that the
 * user's choice lives in "<id>_plugin_enabled".  A DISABLED plugin is
 * never dlopen'd at all — that is the point, and it is why disabling one
 * costs the app literally nothing rather than merely hiding its UI.
 * ------------------------------------------------------------------------- */
struct TaskPlugin {
    guint32      abi_version;        /* TASK_PLUGIN_ABI_VERSION            */
    const gchar *id;                 /* "gtasks"; config namespace         */
    const gchar *name;               /* "Google Tasks"; shown to the user  */
    const gchar *description;        /* one line, shown in Settings        */
    const gchar *version;            /* the plugin's own version string    */
    gboolean     enabled_default;

    gboolean (*init)(TaskApp *app, const TaskPlugin *self);
    void     (*db_open)(TaskApp *app, TaskDatabase *db,
                        const TaskPlugin *self);
    void     (*db_closing)(TaskApp *app, TaskDatabase *db,
                           const TaskPlugin *self);
    void     (*shutdown)(TaskApp *app, const TaskPlugin *self);
};

/* ---------------------------------------------------------------------------
 * task_plugin_entry() — the ONE symbol a plugin exports.
 *
 * Called immediately after dlopen.  Store `api` (it is valid for the
 * life of the process) and return a pointer to a static TaskPlugin.
 * Return NULL to decline — the loader reports it and moves on.
 *
 * Do NOT do real work here; that is what init() is for.  This runs
 * before the host has checked anything about you.
 *
 * Declare it exactly as:
 *
 *     TASK_PLUGIN_EXPORT const TaskPlugin *
 *     task_plugin_entry(const TaskHostApi *api);
 * ------------------------------------------------------------------------- */
#define TASK_PLUGIN_EXPORT __attribute__((visibility("default")))

typedef const TaskPlugin *(*TaskPluginEntryFn)(const TaskHostApi *api);

#define TASK_PLUGIN_ENTRY_SYMBOL "task_plugin_entry"

#endif /* TASK_PLUGIN_H */
