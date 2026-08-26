/* ===========================================================================
 * plugin_loader.h — find, load and hold the plugins.
 * =========================================================================== */

#ifndef TASK_PLUGIN_LOADER_H
#define TASK_PLUGIN_LOADER_H

#include "plugin.h"

/* ---------------------------------------------------------------------------
 * task_plugins_load() — scan <exe_dir>/plugins, load what is enabled,
 * and run each plugin's init().
 *
 * Call ONCE from main(), after the app is built and the app's own
 * registries are populated, and BEFORE the library window: plugins
 * register sidebar views and the sidebar is built from that registry.
 *
 * A plugin is skipped without loading at all when
 * "<id>_plugin_enabled" is 0 — which is the point of the setting.  The
 * id is read from the FILENAME for that check (gtasks.so -> "gtasks"),
 * so a disabled plugin is never dlopen'd and costs nothing.
 *
 * Nothing here is fatal.  A missing directory, an unreadable file, a
 * wrong ABI, a NULL entry point or an init() that declines are all
 * reported and stepped over — the app must always start, with or
 * without any given plugin.
 *
 * Timing: the load and init of each plugin is measured, and one that
 * takes longer than the warning threshold is named in a g_warning.
 * init() runs before the window is shown, so time spent there is time
 * the user watches nothing happen.
 * ------------------------------------------------------------------------- */
void task_plugins_load(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_plugins_db_open() / task_plugins_db_closing() — tell every loaded
 * plugin that a database opened or is about to close, so it can create
 * or forget its own tables.  Call from startup and from the
 * database-switch paths.
 * ------------------------------------------------------------------------- */
void task_plugins_db_open(TaskApp *app, TaskDatabase *db);
void task_plugins_db_closing(TaskApp *app, TaskDatabase *db);

/* ---------------------------------------------------------------------------
 * task_plugins_shutdown() — run every plugin's shutdown().  The modules
 * are deliberately NOT dlclose()d: a plugin that has registered a GType,
 * a GTK CSS provider or an icon path cannot be unloaded safely — those
 * registrations are process-global and cannot be undone — and unloading
 * at exit buys nothing.
 * ------------------------------------------------------------------------- */
void task_plugins_shutdown(TaskApp *app);

/* ---------------------------------------------------------------------------
 * Introspection, for the Settings list.  Entries are in load order.
 * ------------------------------------------------------------------------- */
guint              task_plugins_count(void);
const TaskPlugin  *task_plugins_nth(guint index);

/* ---------------------------------------------------------------------------
 * What Settings needs to draw the plugin list.
 *
 * This covers every module FOUND, not every module loaded — which is the
 * whole point.  A plugin that is switched off is never opened, and one
 * that failed its ABI check is not running, yet both must still appear
 * with a way back: a list that showed only what loaded would make
 * disabling a plugin the last thing you could ever do to it.
 *
 * `name`/`description`/`version` come from the plugin itself when it
 * loaded, and fall back to the id when it did not — there is nowhere
 * else to read them from without opening the module, which is exactly
 * what a disabled plugin must not have done to it.
 *
 * `problem` is NULL when all is well, else a short reason ("built for a
 * different version of Tasks").  Borrowed; valid for the process.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar *id;
    const gchar *name;
    const gchar *description;
    const gchar *version;
    gboolean     enabled;            /* the user's setting                 */
    gboolean     loaded;             /* actually running now               */
    const gchar *problem;            /* why not, or NULL                   */
    /* Absolute path of the plugin's README, or NULL when it ships
     * without one.  Found by CONVENTION — "<id>.README.md" beside the
     * module — and not declared by the plugin, because it has to be
     * readable for a plugin that is switched OFF, and a plugin that is
     * switched off is never opened.  A struct field could not be read
     * without loading exactly what the user asked not to load.           */
    const gchar *readme;
} TaskPluginInfo;

/* The README filename a plugin is looked for under, given its id.        */
#define TASK_PLUGIN_README_SUFFIX ".README.md"

guint                  task_plugins_available(void);
const TaskPluginInfo  *task_plugins_info(guint index);

/* task_plugins_set_enabled() — write the enabled setting for `id`.
 *
 * Takes effect at the NEXT START, and deliberately so: a plugin may have
 * registered a GType, a CSS provider or an icon-theme path, none of
 * which can be undone, so unloading one in place is not something this
 * app can honestly offer.  The caller is expected to say so.
 * ------------------------------------------------------------------------- */
void task_plugins_set_enabled(const gchar *id, gboolean enabled);

/* ---------------------------------------------------------------------------
 * task_plugins_dir() — the absolute path plugins were loaded from THIS
 * RUN.  Borrowed string, valid for the process.
 *
 * Resolved once, in order:
 *   1. the "plugin_dir" setting, when set;
 *   2. a "plugins" folder beside the binary, when one exists (a portable
 *      install, or a source tree someone just ran `make` in);
 *   3. "<data dir>/tasks/plugins", beside the DEFAULT database, created
 *      on demand.
 *
 * Step 3 uses the DEFAULT database directory rather than a relocated
 * `db_dir`, matching task_backup_dir.  The reason is sharper here:
 * plugins are compiled code, this database routinely lives in a sync
 * folder, and following it would push architecture-specific shared
 * objects between machines.
 * ------------------------------------------------------------------------- */
const gchar *task_plugins_dir(void);

/* ---------------------------------------------------------------------------
 * task_plugins_set_dir() — choose the folder for NEXT start.  NULL or ""
 * restores the default.
 *
 * Deliberately does not re-scan: plugins cannot be loaded or unloaded
 * after startup (a module that has registered a GType cannot be undone),
 * so a new folder can only take effect on restart.  task_plugins_dir()
 * keeps reporting where this run actually looked, which is the honest
 * answer while the old plugins are still running.
 * ------------------------------------------------------------------------- */
void task_plugins_set_dir(const gchar *dir);

#endif /* TASK_PLUGIN_LOADER_H */
