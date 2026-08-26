/* ===========================================================================
 * plugin_loader.c — the plugin loader and the host API table
 * (see plugin.h, plugin_loader.h)
 * =========================================================================== */

#include "plugin_loader.h"
#include "app.h"
#include <dlfcn.h>
#include <string.h>

/* A plugin whose load+init takes longer than this is named in a warning.
 * It runs before the window is shown, so this is time the user spends
 * looking at nothing.  Generous on purpose — the point is to catch a
 * plugin doing network or subprocess work in init(), not to police a
 * few hundred microseconds of registration.                              */
#define PLUGIN_SLOW_MS 50.0

/* ---------------------------------------------------------------------------
 * One loaded plugin.  `handle` is kept so the module stays resident; it
 * is never dlclose()d (see task_plugins_shutdown).
 * ------------------------------------------------------------------------- */
typedef struct {
    const TaskPlugin *plugin;
    void             *handle;
    gchar            *path;
} Loaded;

static GPtrArray *loaded = NULL;     /* Loaded*, in load order              */

/* ===========================================================================
 * The host API table.
 *
 * Every function here is a thin adapter over the app's own API.  They
 * exist rather than exposing the app's symbols directly so the ABI is
 * an explicit, readable list — and so the core keeps calling itself
 * with direct calls, paying nothing for a plugin system that may not
 * even be in use.
 * =========================================================================== */

/* --- notify --------------------------------------------------------------- */

/* host_status() — plugins pass a finished string, not a format.  Varargs
 * across an ABI boundary are avoidable here, and a plugin formatting its
 * own message cannot accidentally hand user text to a printf.            */
static void
host_status(TaskApp *app, const gchar *message)
{
    task_app_status(app, "%s", message);
}

static void
host_invoke_main(GSourceFunc fn, gpointer d, GDestroyNotify free_fn)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, fn, d, free_fn);
}

static const TaskHostNotify host_notify = {
    .status         = host_status,
    .notify_changed = task_app_notify_changed,
    .notify_tasks   = task_app_notify_tasks,
    .listen_changed = task_app_listen_changed,
    .listen_tasks   = task_app_listen_tasks,
    .listen_status  = task_app_listen_status,
    .unlisten       = task_app_unlisten,
    .invoke_main    = host_invoke_main,
};

/* --- config --------------------------------------------------------------- */

static gchar *
host_config_get(const TaskPlugin *self, const gchar *key)
{
    return task_app_config_get_ns(self->id, key);
}

static void
host_config_set(const TaskPlugin *self, const gchar *key, const gchar *value)
{
    task_app_config_set_ns(self->id, key, value);
}

static gboolean
host_config_get_bool(const TaskPlugin *self, const gchar *key, gboolean def)
{
    return task_app_config_get_bool_ns(self->id, key, def);
}

static const TaskHostConfig host_config = {
    .get             = host_config_get,
    .set             = host_config_set,
    .get_bool        = host_config_get_bool,
    .global_get      = task_app_config_get,
    .global_get_bool = task_app_config_get_bool,
};

/* --- database ------------------------------------------------------------- */

static TaskDatabase *
host_main_db(TaskApp *app)
{
    return app->db;
}

static const gchar *
host_db_path(TaskApp *app)
{
    return app->db != NULL ? app->db->path : NULL;
}

/* host_exec() — statements with no result.  Returns FALSE and logs
 * sqlite's own message on failure; a plugin silently losing a write is
 * the outcome the app's error discipline exists to prevent.             */
static gboolean
host_exec(TaskDatabase *db, const gchar *sql)
{
    gchar *msg = NULL;
    if (sqlite3_exec(db->sq, sql, NULL, NULL, &msg) != SQLITE_OK) {
        g_warning("plugin sql: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* Trampoline: sqlite3_exec's callback shape without the sqlite3 types
 * reaching the plugin's header.                                          */
typedef struct {
    gint (*cb)(gpointer d, gint n_cols, gchar **values, gchar **names);
    gpointer user_data;
} QueryCtx;

static int
query_trampoline(void *data, int n_cols, char **values, char **names)
{
    QueryCtx *q = data;
    return q->cb(q->user_data, n_cols, values, names);
}

static gboolean
host_exec_query(TaskDatabase *db, const gchar *sql,
                gint (*cb)(gpointer, gint, gchar **, gchar **),
                gpointer user_data)
{
    QueryCtx q = { cb, user_data };
    gchar *msg = NULL;
    if (sqlite3_exec(db->sq, sql, query_trampoline, &q, &msg) != SQLITE_OK) {
        g_warning("plugin sql: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* host_quote() — sqlite3_mprintf's %Q, re-homed onto g_free so a plugin
 * never needs sqlite3_free (and so never needs to link SQLite).          */
static gchar *
host_quote(const gchar *s)
{
    gchar *q = sqlite3_mprintf("%Q", s);
    gchar *out = g_strdup(q);
    sqlite3_free(q);
    return out;
}

static const TaskHostDb host_db = {
    .main_db           = host_main_db,
    .path              = host_db_path,
    .open              = task_db_open,
    .close             = task_db_close,

    .task_get          = task_db_task_get,
    .task_create       = task_db_task_create,
    .task_delete       = task_db_task_delete,
    .task_purge        = task_db_task_purge,
    .task_set_status   = task_db_task_set_status,
    .tasks_toplevel    = task_db_tasks_toplevel,
    .tasks_all_visible = task_db_tasks_all_visible,
    .tasks_pinned      = task_db_tasks_pinned,
    .tasks_due_between = task_db_tasks_due_between,
    .subtasks          = task_db_subtasks,
    .task_free         = task_free,
    .tasks_free        = task_ptr_array_free_tasks,

    .lists             = task_db_lists,
    .list_get          = task_db_list_get,
    .list_create       = task_db_list_create,
    .list_free         = task_list_free,
    .lists_free        = task_ptr_array_free_lists,

    .state_get         = task_db_state_get,
    .state_set         = task_db_state_set,

    .exec              = host_exec,
    .exec_query        = host_exec_query,
    .quote             = host_quote,
};

/* --- workers, views, ops -------------------------------------------------- */

static const TaskHostWorker host_worker = {
    .register_worker = task_worker_register,
    .arm             = task_worker_arm,
};

static const TaskHostViews host_views = {
    .register_view = task_view_register,
};

static const TaskHostOps host_ops = {
    .move_to_list     = task_ops_move_to_list,
    .clear_completed  = task_ops_clear_completed,
    .add_moved_hook   = task_ops_add_moved_hook,
    .add_cleared_hook = task_ops_add_cleared_hook,
    .add_list_veto    = task_ops_add_list_veto,
    .add_delete_hook  = task_db_add_delete_hook,
};

static const TaskHostApi host_api = {
    .abi_version      = TASK_PLUGIN_ABI_VERSION,
    .task_struct_size = sizeof(Task),
    .host_api_size    = sizeof(TaskHostApi),
    .host_version     = TASK_VERSION,
    .notify           = &host_notify,
    .config           = &host_config,
    .db               = &host_db,
    .worker           = &host_worker,
    .views            = &host_views,
    .ops              = &host_ops,
};

/* ===========================================================================
 * Loading.
 * =========================================================================== */

/* plugin_id_from_file() — "gtasks.so" -> "gtasks".  Used to consult the
 * enabled setting BEFORE dlopen, so a disabled plugin is never mapped
 * into the process at all.  New string.                                  */
static gchar *
plugin_id_from_file(const gchar *filename)
{
    const gchar *dot = strrchr(filename, '.');
    return dot != NULL ? g_strndup(filename, (gsize)(dot - filename))
                       : g_strdup(filename);
}

/* is_module() — does this filename look like a loadable module?  Both
 * suffixes are accepted on both platforms: the extension is a build
 * convention, not a format, and refusing a .so on macOS would only
 * surprise someone who built one.                                        */
static gboolean
is_module(const gchar *filename)
{
    return g_str_has_suffix(filename, ".so") ||
           g_str_has_suffix(filename, ".dylib");
}

/* load_one() — dlopen, verify, init.  Returns TRUE when the plugin is
 * live.  Every failure path reports and returns FALSE; none is fatal.    */
static gboolean
load_one(TaskApp *app, const gchar *path, const gchar *id)
{
    gint64 t0 = g_get_monotonic_time();

    /* RTLD_NOW: resolve every symbol at load, so a missing one is an
     * error here rather than a crash later, and there is no lazy-binding
     * stall in the middle of an interaction.
     * RTLD_LOCAL: keep the plugin's symbols out of the global namespace,
     * so it cannot interpose on the host or on another plugin.           */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        g_warning("plugin \"%s\": %s", id, dlerror());
        return FALSE;
    }

    dlerror();                       /* clear any stale error              */
    TaskPluginEntryFn entry =
        (TaskPluginEntryFn)dlsym(handle, TASK_PLUGIN_ENTRY_SYMBOL);
    if (entry == NULL) {
        g_warning("plugin \"%s\": no %s symbol", id,
                  TASK_PLUGIN_ENTRY_SYMBOL);
        return FALSE;
    }

    const TaskPlugin *p = entry(&host_api);
    if (p == NULL) {
        g_warning("plugin \"%s\": declined to load", id);
        return FALSE;
    }
    /* The version check is the whole reason the entry point is handed
     * the table rather than linking against it: a plugin built against
     * a different ABI must fail HERE, loudly, not by reading a struct
     * whose layout it disagrees about.                                   */
    if (p->abi_version != TASK_PLUGIN_ABI_VERSION) {
        g_warning("plugin \"%s\": built for ABI %u, this build is %u "
                  "\xe2\x80\x94 not loaded", id,
                  (unsigned)p->abi_version,
                  (unsigned)TASK_PLUGIN_ABI_VERSION);
        return FALSE;
    }
    if (p->id == NULL) {
        g_warning("plugin \"%s\": no id \xe2\x80\x94 not loaded", id);
        return FALSE;
    }
    /* The id in the file and the id in the struct must agree, because
     * the ENABLED setting is keyed on the filename (it has to be — it is
     * consulted before the module is opened) while the CONFIG namespace
     * is keyed on the struct.  Letting them differ would give a plugin
     * one name for being switched off and another for its settings.      */
    if (g_strcmp0(p->id, id) != 0) {
        g_warning("plugin \"%s\": declares id \"%s\" \xe2\x80\x94 the file "
                  "must be named after the id; not loaded", id, p->id);
        return FALSE;
    }

    if (p->init != NULL && !p->init(app, p)) {
        g_message("plugin \"%s\" declined to start", p->id);
        return FALSE;
    }

    Loaded *l = g_new0(Loaded, 1);
    l->plugin = p;
    l->handle = handle;
    l->path   = g_strdup(path);
    if (loaded == NULL)
        loaded = g_ptr_array_new();
    g_ptr_array_add(loaded, l);

    gdouble ms = (gdouble)(g_get_monotonic_time() - t0) / 1000.0;
    if (ms > PLUGIN_SLOW_MS)
        g_warning("plugin \"%s\" took %.1f ms to load and initialise \xe2\x80\x94 "
                  "init() runs before the window is shown, so this is "
                  "startup the user waits through", p->id, ms);
    else
        g_debug("plugin \"%s\" v%s loaded in %.1f ms", p->id,
                p->version != NULL ? p->version : "?", ms);
    return TRUE;
}

void
task_plugins_load(TaskApp *app)
{
    gchar *dir_path = g_build_filename(task_app_exe_dir(),
                                       TASK_PLUGIN_DIR, NULL);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir == NULL) {               /* no plugins/ directory is normal    */
        g_free(dir_path);
        return;
    }

    /* Sort the filenames so load order is the same on every run and on
     * every machine: a plugin's view lands in a stable place in the
     * sidebar rather than wherever readdir happened to put it.           */
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL)
        if (is_module(name))
            g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);

    for (guint i = 0; i < names->len; i++) {
        const gchar *fname = g_ptr_array_index(names, i);
        gchar *id  = plugin_id_from_file(fname);
        gchar *key = g_strdup_printf("%s_plugin_enabled", id);
        /* Checked BEFORE dlopen on purpose: a disabled plugin is not
         * mapped, not initialised and not resolved, so switching one off
         * costs the app nothing at all rather than merely hiding it.     */
        if (!task_app_config_get_bool(key, TRUE)) {
            g_debug("plugin \"%s\" disabled \xe2\x80\x94 not loaded", id);
        } else {
            gchar *full = g_build_filename(dir_path, fname, NULL);
            load_one(app, full, id);
            g_free(full);
        }
        g_free(key);
        g_free(id);
    }
    g_ptr_array_free(names, TRUE);
    g_free(dir_path);
}

void
task_plugins_db_open(TaskApp *app, TaskDatabase *db)
{
    for (guint i = 0; i < task_plugins_count(); i++) {
        const TaskPlugin *p = task_plugins_nth(i);
        if (p->db_open != NULL)
            p->db_open(app, db, p);
    }
}

void
task_plugins_db_closing(TaskApp *app, TaskDatabase *db)
{
    for (guint i = 0; i < task_plugins_count(); i++) {
        const TaskPlugin *p = task_plugins_nth(i);
        if (p->db_closing != NULL)
            p->db_closing(app, db, p);
    }
}

void
task_plugins_shutdown(TaskApp *app)
{
    for (guint i = 0; i < task_plugins_count(); i++) {
        const TaskPlugin *p = task_plugins_nth(i);
        if (p->shutdown != NULL)
            p->shutdown(app, p);
    }
    /* The modules are NOT dlclose()d.  A plugin that registered a GType,
     * a CSS provider or an icon path cannot be unloaded safely — those
     * registrations are process-global and have no undo — and unmapping
     * at exit buys nothing.                                              */
}

guint
task_plugins_count(void)
{
    return loaded != NULL ? loaded->len : 0;
}

const TaskPlugin *
task_plugins_nth(guint index)
{
    if (loaded == NULL || index >= loaded->len)
        return NULL;
    return ((const Loaded *)g_ptr_array_index(loaded, index))->plugin;
}
