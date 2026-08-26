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

#endif /* TASK_PLUGIN_LOADER_H */
