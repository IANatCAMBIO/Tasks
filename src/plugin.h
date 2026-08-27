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
 * Two numbers: see TASK_PLUGIN_ABI_VERSION / _REVISION below.  The major
 * must match exactly; the host's revision must be at or above the
 * plugin's.  Both are checked at load and a mismatch is refused loudly,
 * naming which way round it is, so "this plugin is too new for this
 * Tasks" and "this plugin is too old" are different messages.
 *
 * Growth is APPEND-ONLY within a major: fields are added to the end of
 * Task, groups to the end of TaskHostApi.  task_struct_size and
 * host_api_size travel with the table so a plugin can assert the build
 * it is running against actually matches the header it compiled with —
 * a belt-and-braces check for headers that drifted without a revision
 * bump, which is a mistake no version number can catch by itself.
 * =========================================================================== */

#ifndef TASK_PLUGIN_H
#define TASK_PLUGIN_H

#include "db.h"
#include "task_view.h"
#include "task_ops.h"
#include "task_worker.h"
#include "settings_window.h"
#include "task_rows.h"
#include "task_ui.h"

/* ---------------------------------------------------------------------------
 * TWO numbers, because there are two kinds of change.
 *
 * VERSION is the breaking one: a field reordered or removed, a signature
 * changed, a meaning changed.  Host and plugin must match EXACTLY — an
 * older plugin cannot be reasoned about across such a change, so it is
 * refused rather than guessed at.
 *
 * REVISION is the additive one: a group appended to TaskHostApi, a field
 * appended to Task.  The host must be at or above the plugin's, never
 * below.  A plugin built at revision 3 reads a group the host added at
 * revision 3; a revision-2 host never filled that pointer, so letting it
 * load would hand the plugin garbage at a fixed offset.  The reverse is
 * safe by construction: appending moves nothing the older plugin knows
 * about, so a revision-2 plugin on a revision-5 host reads exactly the
 * fields it was compiled against and ignores the rest.
 *
 * This is what makes the size fields below meaningful.  With a single
 * exact-match number they were decoration — every change refused every
 * older plugin, so nothing could ever be tolerated and nothing needed
 * measuring.
 * ------------------------------------------------------------------------- */
#define TASK_PLUGIN_ABI_VERSION  1u
#define TASK_PLUGIN_ABI_REVISION 5u

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

    /* --- since ABI 1.4: what a two-way sync needs ---------------------
     * These write a row from REMOTE data without the usual now() stamp,
     * or physically remove one that both sides agree has gone.  They are
     * not general-purpose: apply_remote leaves pinned/priority alone
     * because those are local-only, and the purges are for rows whose
     * removal has already propagated.                                   */
    void        (*list_apply_remote)(TaskDatabase *db, gint64 id,
                                     const gchar *name, gint64 updated_at);
    void        (*task_apply_remote)(TaskDatabase *db, const Task *t);
    void        (*list_restore)(TaskDatabase *db, gint64 id);
    void        (*list_purge)(TaskDatabase *db, gint64 id);
    void        (*list_emoji_if_empty)(TaskDatabase *db, gint64 list_id,
                                       const gchar *emoji);
    /* Every row of one list including subtasks and tombstones, parents
     * before subtasks — a new parent must own a remote id before its
     * children push.                                                    */
    GPtrArray  *(*tasks_in_list_all)(TaskDatabase *db, gint64 list_id);
    /* A bare tombstone; the caller attaches its own identity to the id
     * this returns.                                                     */
    gint64      (*insert_remote_tombstone)(TaskDatabase *db, gint64 list_id);
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

/* ---------------------------------------------------------------------------
 * Settings.
 *
 * A plugin configures itself by contributing a SECTION to the one
 * scrolling column the Settings window is — the same mechanism the app
 * uses for Google Tasks, Notes and the rest, so a feature does not
 * change shape when it becomes a plugin.
 *
 * Contributed sections are built AFTER the Plugins list, which makes
 * that list read as a table of contents: the plugin's name, README and
 * enable checkbox, then its controls directly below.
 *
 * Register from init().  The BUILDER then runs every time Settings is
 * opened, against a window that is destroyed and rebuilt each time — so
 * keep no widget pointers between calls, and keep the builder fast, for
 * the same reason init() must be fast: a slow builder is a Settings
 * window that takes a visible moment to appear.
 *
 * Use `heading` and `note` so a contributed section is indistinguishable
 * from a built-in one.
 *
 * NOTE a plugin the user has switched OFF can contribute nothing: its
 * code was never loaded.  Its enable checkbox in the Plugins list is the
 * only control it has in that state, which is exactly why enable/disable
 * lives there and not in the plugin's own section.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * Task rows — the app's own row renderer (see task_rows.h).  ABI 1.2.
 *
 * A plugin that displays tasks uses THIS rather than building its own
 * tree view, so its rows look like every other task in the app and stay
 * looking like them.  The Weekly Forecast needs seven of these; a
 * reimplementation would drift from the task pane the first time either
 * changed.
 *
 * PERFORMANCE: build one ctx per refresh and fill every store from it.
 * The context exists to make the attachment counts, subtasks and list
 * names ONE query each instead of one per row, which is the difference
 * between a refresh and a stall.  Never call any of this from a cell
 * data function — those run per draw.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkListStore *(*store_new)(void);
    void   (*ctx_init)(TaskApp *app, TaskRowCtx *ctx, gboolean virtual_view);
    void   (*ctx_clear)(TaskRowCtx *ctx);
    guint  (*append)(GtkListStore *store, GPtrArray *tasks,
                     const TaskRowCtx *ctx);
    gchar *(*desc_markup)(const Task *t, const gchar *list_name,
                          gint att_count, GPtrArray *subs,
                          const TaskRowCtx *ctx);
    /* Add a glyph to the task cell — since ABI 1.5.  BATCH-shaped on
     * purpose: the host asks once per refresh for the whole set of ids
     * to decorate, never per row (see task_rows.h).                    */
    void   (*add_decoration)(const TaskRowDecorDef *def);
    const gchar *(*stripe_color)(GtkTreeModel *model, GtkTreeIter *iter);
    void   (*bg_func)(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
    /* What a click on the ✓ column means — the status rule, the
     * fade-out when completed tasks are hidden, and the refresh.  Use
     * this rather than writing a status directly, or a plugin's
     * checkbox will quietly mean something different from the app's.   */
    void   (*toggle_done)(TaskApp *app, GtkListStore *store,
                          GtkTreeIter *iter);
} TaskHostRows;

/* ---------------------------------------------------------------------------
 * Window services a panel needs.  ABI 1.3.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Open the editor for a task — what a double-click on a row means.
     * Editors are singletons per task, so calling this for one already
     * open presents it rather than making a second.                      */
    void (*editor_open)(TaskApp *app, gint64 task_id);

    /* Preserve a scrolled window's position across a model rebuild.
     * Call BEFORE clearing the stores: clearing a store zeroes the
     * scrollbar, so the position has to be captured first and restored
     * once the rebuild settles.  Getting this wrong is invisible until
     * someone scrolls down and a refresh throws them back to the top. */
    void (*scroll_keep)(GtkWidget *scrolled_window);

    /* The status bar's LEFT label — where you are and how much is here.
     * A panel owns its pane, so it owns this line; the transient event
     * message on the right is notify->status.  Plain text.             */
    void (*set_location)(TaskApp *app, const gchar *text);

    /* --- since ABI 1.4 ------------------------------------------------ */
    /* A modal message dialog.  For the rare thing a status-bar line
     * cannot carry — a sign-in that failed and needs explaining.       */
    void (*notice)(GtkWindow *parent, GtkMessageType type,
                   const gchar *title, const gchar *message);
    /* One-off CSS on a single widget, so a contributed section can match
     * the app's own type scale rather than guessing at it.             */
    void (*widget_add_css)(GtkWidget *widget, const gchar *css);

    /* The directory holding the executable — where a plugin looks for a
     * credentials file shipped beside the app.                         */
    const gchar *(*exe_dir)(void);

    /* The registries the window builds its chrome from.                */
    void (*add_tool)(const TaskUiToolDef *def);
    void (*tool_set_sensitive)(const gchar *id, gboolean sensitive);
    void (*add_task_menu_item)(const TaskUiTaskMenuDef *def);
    void (*add_editor_section)(const TaskUiEditorDef *def);
    void (*add_menu_item)(const TaskUiMenuDef *def);
} TaskHostUi;

typedef struct {
    void       (*add_section)(TaskSettingsSectionFn fn, gpointer user_data);
    GtkWidget *(*heading)(const gchar *text);
    GtkWidget *(*note)(const gchar *text);
} TaskHostSettings;

/* ---------------------------------------------------------------------------
 * Pure helpers — no state, just the app's own rules, so a plugin cannot
 * disagree with them.  ABI 1.4.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* The single rule mapping a binary done flag onto the tri-state
     * status.  Every done-only source folds through this, which is what
     * stops a round trip through such a system promoting a New task. */
    TaskStatus (*status_apply_done)(TaskStatus cur, gboolean done);

    /* Dates, in the app's spellings.                                  */
    gint64  (*due_from_ymd)(gint y, gint m, gint d);
    gchar  *(*due_format_iso)(gint64 due);
    void    (*day_bounds)(gint offset_days, gint64 *lo, gint64 *hi);
} TaskHostUtil;

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
    guint32 abi_revision;            /* TASK_PLUGIN_ABI_REVISION           */
    gsize   task_struct_size;        /* sizeof(Task) as the HOST sees it   */
    gsize   host_api_size;           /* sizeof(TaskHostApi), ditto         */
    const gchar *host_version;       /* TASK_VERSION, for diagnostics      */

    const TaskHostNotify *notify;
    const TaskHostConfig *config;
    const TaskHostDb     *db;
    const TaskHostWorker *worker;
    const TaskHostViews  *views;
    const TaskHostOps    *ops;
    const TaskHostSettings *settings;
    const TaskHostRows     *rows;    /* since ABI 1.2                      */
    const TaskHostUi       *ui;      /* since ABI 1.3                      */
    const TaskHostUtil     *util;    /* since ABI 1.4                      */
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
    guint32      abi_revision;       /* TASK_PLUGIN_ABI_REVISION           */
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
