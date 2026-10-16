/* ===========================================================================
 * core_views.c — the app's own virtual views, registered through the
 * public registry (see task_view.h).
 *
 * These three are built in, but they go through task_view_register() like
 * anything else rather than being special-cased in the sidebar.  That is
 * deliberate: the registry is only trustworthy for a view added later if
 * it is already carrying the app's own.  If something here needs a back
 * door, the API is wrong and it should grow — not be bypassed.
 *
 * Every other row of the sidebar is a tag (task_pane.h, task_sel_tag).
 * =========================================================================== */

#include "core_views.h"
#include "task_config.h"
#include "task_dates.h"
#include "task_view.h"

/* --- All Favorites ------------------------------------------------------- */

/*
 * favorite_query — every live task carrying a favorite tag.
 *
 * Inputs:
 *   app — the context
 *   d   — unused
 *
 * Output:
 *   newly allocated GPtrArray of Task* (task_ptr_array_free_tasks).
 */
static GPtrArray *
favorite_query(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_tasks_favorite(app->db);
}

/* --- All Tasks ----------------------------------------------------------- */

/*
 * all_query — every live top-level task.
 *
 * Inputs:
 *   app — the context
 *   d   — unused
 *
 * Output:
 *   newly allocated GPtrArray of Task* (task_ptr_array_free_tasks).
 */
static GPtrArray *
all_query(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_tasks_all_visible(app->db);
}

/* --- 3-Day Forecast ------------------------------------------------------ */

#define FORECAST_DAYS 3             /* today and the next two, or the next
                                     * three business days counting today */

/*
 * forecast_query — everything past due, then everything due within the
 * forecast.  The low bound is 1 rather than 0: due == 0 means "no date at
 * all", so 0 would sweep in every undated task instead of every past-due
 * one.
 *
 * Inputs:
 *   app — the context
 *   d   — unused
 *
 * Output:
 *   newly allocated GPtrArray of Task* (task_ptr_array_free_tasks).
 */
static GPtrArray *
forecast_query(TaskApp *app, gpointer d)
{
    (void)d;
    gint64 hi = task_forecast_end(FORECAST_DAYS,
                                  task_config_get_bool(TASK_CFG_FORECAST_BUSINESS_DAYS));
    return task_db_tasks_due_between(app->db, 1, hi, FALSE);
}

static const TaskView core_view_all = {
    .id    = "all",
    .label = "\xf0\x9f\x94\xae  All Tasks",
    .name  = "All Tasks",
    .sort  = 20,
    .query = all_query,
};

/* `id` is part of the ini format (task_view.h): "pinned" keeps the saved
 * orders of the view All Favorites was before it gathered favorite tags. */
static const TaskView core_view_favorites = {
    .id    = "pinned",
    .label = "\xe2\xad\x90\xef\xb8\x8f  All Favorites",
    .name  = "All Favorites",
    .sort  = 30,
    .query = favorite_query,
};

static const TaskView core_view_forecast = {
    .id    = "forecast",
    .label = "\xe2\x98\x80\xef\xb8\x8f  3-Day Forecast",
    .name  = "3-Day Forecast",
    .sort  = 40,
    .query = forecast_query,
};

/*
 * task_core_views_init — register the three built-in views (see
 * core_views.h).
 */
void
task_core_views_init(void)
{
    task_view_register(&core_view_all);
    task_view_register(&core_view_favorites);
    task_view_register(&core_view_forecast);
}
