/* ===========================================================================
 * plugin_loader.c — the plugin loader and the host API table
 * (see plugin.h, plugin_loader.h)
 * =========================================================================== */

#include "plugin_loader.h"
#include "app.h"
#include "editor_window.h"
#include "library_window.h"
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <string.h>

/* A plugin whose load+init takes longer than this is named in a warning.
 * It runs before the window is shown, so this is time the user spends
 * looking at nothing.  Generous on purpose — the point is to catch a
 * plugin doing network or subprocess work in init(), not to police a
 * few hundred microseconds of registration.                              */
#define PLUGIN_SLOW_MS 50.0

/* ---------------------------------------------------------------------------
 * One DISCOVERED plugin — every module file found, whether or not it
 * loaded.  Settings needs the ones that did not (see TaskPluginInfo in
 * plugin_loader.h), and `handle` is kept so a loaded module stays
 * resident; it is never dlclose()d (see task_plugins_shutdown).
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskPluginInfo    info;          /* what Settings reads                 */
    const TaskPlugin *plugin;        /* NULL unless it loaded               */
    void             *handle;
    gchar            *id;            /* owns info.id                        */
    gchar            *path;
    gchar            *readme;        /* owns info.readme, or NULL           */
} Found;

static GPtrArray *found = NULL;      /* Found*, in discovery order          */
static gchar     *plugin_dir = NULL; /* resolved once by task_plugins_dir() */

/* ---------------------------------------------------------------------------
 * task_plugins_dir() — where plugins come from (see plugin_loader.h).
 *
 * Resolved ONCE.  Re-resolving would let the answer change under a scan
 * already in progress, and the loaded set cannot change after startup
 * anyway.
 *
 * The fallback is the DEFAULT data directory, deliberately NOT the
 * configured `db_dir` — the same choice task_backup_dir makes, and for a
 * stronger reason here.  This database routinely lives in a sync folder
 * (see db.h), and plugins are COMPILED CODE: following a relocated
 * database would push architecture-specific shared objects between
 * machines, where the best case is a plugin that refuses to load.
 *
 * The middle case keeps a development or portable tree working: a
 * plugins/ folder beside the binary wins when it EXISTS, so `make` in the
 * source tree produces something the app actually loads.  It has to be an
 * existence test rather than a writability test — an unwritable
 * plugins/ folder full of modules is still exactly where they are.
 * ------------------------------------------------------------------------- */
const gchar *
task_plugins_dir(void)
{
    if (plugin_dir != NULL)
        return plugin_dir;

    gchar *configured = task_app_config_get("plugin_dir");
    if (configured != NULL && *configured != '\0') {
        plugin_dir = configured;
        return plugin_dir;
    }
    g_free(configured);

    gchar *beside = g_build_filename(task_app_exe_dir(),
                                     TASK_PLUGIN_DIR, NULL);
    if (g_file_test(beside, G_FILE_TEST_IS_DIR)) {
        plugin_dir = beside;
        return plugin_dir;
    }
    g_free(beside);

    /* <data dir>/tasks/plugins — beside the default database.  Created on
     * demand so Settings can offer to open a folder that exists, and so
     * "copy a plugin in here" is advice the user can actually follow.    */
    gchar *db  = task_db_default_path();      /* creates <data>/tasks/     */
    gchar *dir = g_path_get_dirname(db);
    g_free(db);
    plugin_dir = g_build_filename(dir, TASK_PLUGIN_DIR, NULL);
    g_free(dir);
    g_mkdir_with_parents(plugin_dir, 0755);
    return plugin_dir;
}

/* ---------------------------------------------------------------------------
 * task_plugins_set_dir() — see plugin_loader.h.
 * ------------------------------------------------------------------------- */
void
task_plugins_set_dir(const gchar *dir)
{
    task_app_config_set("plugin_dir",
                        dir != NULL && *dir != '\0' ? dir : NULL);
    /* The cached value is NOT updated: the scan has already happened, and
     * reporting a folder nothing was loaded from would be a lie until the
     * next start.  task_plugins_dir keeps answering where this run
     * actually looked.                                                    */
}

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

/* The exec/query pair is the app's own (db.h) — one implementation for
 * in-tree callers and plugins alike, so a fix like the SQLITE_ABORT
 * handling below cannot land on only one of them.                       */

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

    .exec              = task_db_exec_sql,
    .exec_query        = task_db_exec_query,
    .quote             = host_quote,

    .list_apply_remote       = task_db_list_apply_remote,
    .task_apply_remote       = task_db_task_apply_remote,
    .list_restore            = task_db_list_restore,
    .list_purge              = task_db_list_purge,
    .list_emoji_if_empty     = task_db_list_emoji_if_empty,
    .tasks_in_list_all       = task_db_tasks_in_list_all,
    .insert_remote_tombstone = task_db_insert_remote_tombstone,
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

static const TaskHostSettings host_settings = {
    .add_section = task_settings_add_section,
    .heading     = task_settings_section_heading,
    .note        = task_settings_section_note,
};

static const TaskHostRows host_rows = {
    .store_new    = task_rows_store_new,
    .ctx_init     = task_row_ctx_init,
    .ctx_clear    = task_row_ctx_clear,
    .append       = task_rows_append,
    .desc_markup  = task_rows_desc_markup,
    .stripe_color = task_rows_stripe_color,
    .bg_func      = task_rows_bg_func,
    .toggle_done  = task_rows_toggle_done,
    .add_decoration = task_rows_add_decoration,
};

/* host_notice() — plugins pass a finished string, for the same reason
 * host_status does: no varargs across the ABI boundary.               */
static void
host_notice(GtkWindow *parent, GtkMessageType type, const gchar *title,
            const gchar *message)
{
    task_app_notice(parent, type, title, "%s", message);
}

static const TaskHostUi host_ui = {
    .editor_open        = task_editor_open,
    .scroll_keep        = task_library_scroll_keep,
    .set_location       = task_library_set_location,
    .notice             = host_notice,
    .widget_add_css     = task_app_widget_add_css,
    .exe_dir            = task_app_exe_dir,
    .add_tool           = task_ui_add_tool,
    .tool_set_sensitive = task_ui_tool_set_sensitive,
    .add_task_menu_item = task_ui_add_task_menu_item,
    .add_editor_section = task_ui_add_editor_section,
    .add_menu_item      = task_ui_add_menu_item,
};

static const TaskHostUtil host_util = {
    .status_apply_done = task_status_apply_done,
    .due_from_ymd      = task_due_from_ymd,
    .due_format_iso    = task_due_format_iso,
    .day_bounds        = task_day_bounds,
};

static const TaskHostApi host_api = {
    .abi_version      = TASK_PLUGIN_ABI_VERSION,
    .abi_revision     = TASK_PLUGIN_ABI_REVISION,
    .task_struct_size = sizeof(Task),
    .host_api_size    = sizeof(TaskHostApi),
    .host_version     = TASK_VERSION,
    .notify           = &host_notify,
    .config           = &host_config,
    .db               = &host_db,
    .worker           = &host_worker,
    .views            = &host_views,
    .ops              = &host_ops,
    .settings         = &host_settings,
    .rows             = &host_rows,
    .ui               = &host_ui,
    .util             = &host_util,
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

/* enabled_for() — the user's setting for a plugin id.  Read from the
 * FILENAME's id before the module is opened, which is what makes
 * "disabled" mean "never loaded" rather than "loaded and ignored".      */
static gboolean
enabled_for(const gchar *id)
{
    gchar *key = g_strdup_printf("%s_plugin_enabled", id);
    gboolean on = task_app_config_get_bool(key, TRUE);
    g_free(key);
    return on;
}

/* load_one() — dlopen, verify, init.  Fills in `f` either way: a plugin
 * that fails still has to appear in Settings with the reason.  Every
 * failure path reports and returns; none is fatal.                       */
static void
load_one(TaskApp *app, Found *f)
{
    const gchar *path = f->path;
    const gchar *id   = f->info.id;
    gint64 t0 = g_get_monotonic_time();

    /* RTLD_NOW: resolve every symbol at load, so a missing one is an
     * error here rather than a crash later, and there is no lazy-binding
     * stall in the middle of an interaction.
     * RTLD_LOCAL: keep the plugin's symbols out of the global namespace,
     * so it cannot interpose on the host or on another plugin.           */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        g_warning("plugin \"%s\": %s", id, dlerror());
        f->info.problem = "could not be opened";
        return;
    }

    dlerror();                       /* clear any stale error              */
    TaskPluginEntryFn entry =
        (TaskPluginEntryFn)dlsym(handle, TASK_PLUGIN_ENTRY_SYMBOL);
    if (entry == NULL) {
        g_warning("plugin \"%s\": no %s symbol", id,
                  TASK_PLUGIN_ENTRY_SYMBOL);
        f->info.problem = "not a Tasks plugin";
        return;
    }

    const TaskPlugin *p = entry(&host_api);
    if (p == NULL) {
        g_warning("plugin \"%s\": declined to load", id);
        f->info.problem = "declined to load";
        return;
    }
    /* The version check is the whole reason the entry point is handed
     * the table rather than linking against it: a plugin built against
     * a different ABI must fail HERE, loudly, not by reading a struct
     * whose layout it disagrees about.
     *
     * MAJOR must match exactly — across a breaking change there is
     * nothing to reason about.                                            */
    if (p->abi_version != TASK_PLUGIN_ABI_VERSION) {
        g_warning("plugin \"%s\": built for Tasks plugin ABI %u, this "
                  "build is %u \xe2\x80\x94 not loaded", id,
                  (unsigned)p->abi_version,
                  (unsigned)TASK_PLUGIN_ABI_VERSION);
        f->info.problem = "built for a different version of Tasks";
        return;
    }
    /* REVISION only has to be at or below ours.  A plugin built against a
     * NEWER revision expects groups this host never filled, and would
     * read an uninitialised pointer at a fixed offset — so that one is
     * refused, while an older plugin is fine because growth is
     * append-only.  The two directions get different messages: "update
     * Tasks" and "this plugin is old" are different problems.            */
    if (p->abi_revision > TASK_PLUGIN_ABI_REVISION) {
        g_warning("plugin \"%s\": needs plugin ABI %u.%u, this build "
                  "provides %u.%u \xe2\x80\x94 not loaded", id,
                  (unsigned)p->abi_version, (unsigned)p->abi_revision,
                  (unsigned)TASK_PLUGIN_ABI_VERSION,
                  (unsigned)TASK_PLUGIN_ABI_REVISION);
        f->info.problem = "needs a newer version of Tasks";
        return;
    }
    if (p->id == NULL) {
        g_warning("plugin \"%s\": no id \xe2\x80\x94 not loaded", id);
        f->info.problem = "not a Tasks plugin";
        return;
    }
    /* The id in the file and the id in the struct must agree, because
     * the ENABLED setting is keyed on the filename (it has to be — it is
     * consulted before the module is opened) while the CONFIG namespace
     * is keyed on the struct.  Letting them differ would give a plugin
     * one name for being switched off and another for its settings.      */
    if (g_strcmp0(p->id, id) != 0) {
        g_warning("plugin \"%s\": declares id \"%s\" \xe2\x80\x94 the file "
                  "must be named after the id; not loaded", id, p->id);
        f->info.problem = "its file name does not match its id";
        return;
    }

    /* Everything Settings shows comes from the plugin itself, and is only
     * available once it has loaded — which is why a disabled one falls
     * back to its id.                                                     */
    f->info.name        = p->name != NULL ? p->name : f->info.id;
    f->info.description = p->description;
    f->info.version     = p->version;

    if (p->init != NULL && !p->init(app, p)) {
        g_message("plugin \"%s\" declined to start", p->id);
        f->info.problem = "declined to start";
        return;
    }

    f->plugin      = p;
    f->handle      = handle;
    f->info.loaded = TRUE;

    gdouble ms = (gdouble)(g_get_monotonic_time() - t0) / 1000.0;
    if (ms > PLUGIN_SLOW_MS)
        g_warning("plugin \"%s\" took %.1f ms to load and initialise \xe2\x80\x94 "
                  "init() runs before the window is shown, so this is "
                  "startup the user waits through", p->id, ms);
    else
        g_debug("plugin \"%s\" v%s loaded in %.1f ms", p->id,
                p->version != NULL ? p->version : "?", ms);
}

void
task_plugins_load(TaskApp *app)
{
    const gchar *dir_path = task_plugins_dir();
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir == NULL)                 /* no plugins/ directory is normal    */
        return;

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

    if (found == NULL)
        found = g_ptr_array_new();

    for (guint i = 0; i < names->len; i++) {
        const gchar *fname = g_ptr_array_index(names, i);

        Found *f = g_new0(Found, 1);
        f->id   = plugin_id_from_file(fname);
        f->path = g_build_filename(dir_path, fname, NULL);
        /* Name and description are the plugin's to supply and are not
         * knowable until it loads; the id stands in until then.          */
        f->info.id      = f->id;
        f->info.name    = f->id;
        f->info.enabled = enabled_for(f->id);

        /* Resolved here rather than at load, so a plugin the user has
         * switched off still offers its documentation — which is often
         * exactly what someone wants to read before switching it on.     */
        gchar *rd = g_strconcat(f->id, TASK_PLUGIN_README_SUFFIX, NULL);
        gchar *rpath = g_build_filename(dir_path, rd, NULL);
        if (g_file_test(rpath, G_FILE_TEST_IS_REGULAR)) {
            f->readme      = rpath;
            f->info.readme = rpath;
        } else {
            g_free(rpath);
        }
        g_free(rd);
        g_ptr_array_add(found, f);

        /* Checked BEFORE dlopen on purpose: a disabled plugin is not
         * mapped, not initialised and not resolved, so switching one off
         * costs the app nothing at all rather than merely hiding it.     */
        if (!f->info.enabled) {
            g_debug("plugin \"%s\" disabled \xe2\x80\x94 not loaded", f->id);
            continue;
        }
        load_one(app, f);
    }
    g_ptr_array_free(names, TRUE);
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

/* ---------------------------------------------------------------------------
 * task_plugins_count() / _nth() walk only what is RUNNING; _available()
 * and _info() walk everything that was found (see plugin_loader.h).
 * ------------------------------------------------------------------------- */
guint
task_plugins_count(void)
{
    guint n = 0;
    for (guint i = 0; i < task_plugins_available(); i++)
        if (((const Found *)g_ptr_array_index(found, i))->plugin != NULL)
            n++;
    return n;
}

const TaskPlugin *
task_plugins_nth(guint index)
{
    guint n = 0;
    for (guint i = 0; i < task_plugins_available(); i++) {
        const Found *f = g_ptr_array_index(found, i);
        if (f->plugin != NULL && n++ == index)
            return f->plugin;
    }
    return NULL;
}

guint
task_plugins_available(void)
{
    return found != NULL ? found->len : 0;
}

const TaskPluginInfo *
task_plugins_info(guint index)
{
    if (found == NULL || index >= found->len)
        return NULL;
    return &((const Found *)g_ptr_array_index(found, index))->info;
}

void
task_plugins_set_enabled(const gchar *id, gboolean enabled)
{
    gchar *key = g_strdup_printf("%s_plugin_enabled", id);
    task_app_config_set(key, enabled ? "1" : "0");
    g_free(key);
    /* Keep the in-memory record in step so the Settings list reflects the
     * click immediately, even though nothing loads until a restart.      */
    for (guint i = 0; i < task_plugins_available(); i++) {
        Found *f = g_ptr_array_index(found, i);
        if (g_strcmp0(f->id, id) == 0)
            f->info.enabled = enabled;
    }
}
