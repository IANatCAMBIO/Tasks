/* ===========================================================================
 * overdue.c — "Overdue" sidebar view, as a loadable plugin.
 *
 * The first plugin, and deliberately a small one: its whole job is to
 * prove the chain end to end — build rules, dlopen, the ABI check, the
 * host API table, and a sidebar view contributed from outside the
 * binary.  It shows every task whose due date has passed, newest
 * deadline last.
 *
 * It also demonstrates the shape the performance rules ask for.  The
 * view is a QUERY view, so the host renders it with the same row code as
 * every other list — one database call per refresh, nothing per row and
 * nothing per draw.  init() only registers; it opens no file, spawns no
 * process and touches no network, so it adds nothing measurable to
 * startup.
 * =========================================================================== */

#include "plugin.h"

/* The host's API table, stored once at entry.  A plugin imports no host
 * symbols; every call goes through here.                                  */
static const TaskHostApi *host;

/* ---------------------------------------------------------------------------
 * overdue_visible() — the row exists only while the plugin is switched
 * on in its own settings.  Config keys are namespaced by the plugin id,
 * so this reads "overdue_show_row" without the plugin having to know
 * what other keys exist.
 * ------------------------------------------------------------------------- */
static const TaskPlugin *self;       /* set at entry; needed for config     */

static gboolean
overdue_visible(TaskApp *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    return host->config->get_bool(self, "show_row", TRUE);
}

/* ---------------------------------------------------------------------------
 * overdue_query() — every task due before today's local midnight.
 *
 * The low bound is 1, not 0: due == 0 means "no date at all", so 0 would
 * sweep in every undated task rather than every overdue one.  That is
 * the same trap the app's own Due Today view documents.
 * ------------------------------------------------------------------------- */
static GPtrArray *
overdue_query(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    gint64 lo, hi;
    task_day_bounds(0, &lo, &hi);    /* hi is tomorrow's midnight           */
    return host->db->tasks_due_between(host->db->main_db(app), 1, lo);
}

static const TaskView overdue_view = {
    .id           = "overdue",
    .label        = "\xe2\x8f\xb0  Overdue",
    .name         = "Overdue",
    /* After the app's own four (10..40) and before the forecast (50).     */
    .sort         = 45,
    .visible      = overdue_visible,
    .query        = overdue_query,
    /* Tasks come from every list, so each row keeps its "in <list>" line
     * — without it there is nothing saying where the task actually is.    */
    .virtual_rows = TRUE,
    .not_a_list   = "Overdue is a view, not a list \xe2\x80\x94 "
                    "edit the list each task lives in",
};

/* ---------------------------------------------------------------------------
 * overdue_init() — registration only.  Anything slower than this belongs
 * on a worker: init() runs before the window is shown.
 * ------------------------------------------------------------------------- */
static gboolean
overdue_init(TaskApp *app, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    host->views->register_view(&overdue_view);
    return TRUE;
}

static const TaskPlugin overdue_plugin = {
    .abi_version     = TASK_PLUGIN_ABI_VERSION,
    .id              = "overdue",
    .name            = "Overdue",
    .description     = "A sidebar view of every task past its due date.",
    .version         = "1.0.0",
    .enabled_default = TRUE,
    .init            = overdue_init,
};

TASK_PLUGIN_EXPORT const TaskPlugin *
task_plugin_entry(const TaskHostApi *api)
{
    host = api;
    self = &overdue_plugin;
    return &overdue_plugin;
}
