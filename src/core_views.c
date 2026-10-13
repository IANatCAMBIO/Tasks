/* ===========================================================================
 * core_views.c — the app's own virtual views, registered through the
 * public registry (see task_view.h).
 *
 * These four are built in, but they go through task_view_register() like
 * anything else rather than being special-cased in the sidebar.  That is
 * deliberate: the registry is only trustworthy for a view added later if
 * it is already carrying the app's own.  If something here needs a back
 * door, the API is wrong and it should grow — not be bypassed.
 * =========================================================================== */

#include "core_views.h"
#include "task_config.h"
#include "task_dates.h"
#include "task_view.h"

/* --- Favorites ----------------------------------------------------------- */

/* Shown only while something is pinned — an empty Favorites row is
 * noise.  Mirrored Notes items carry the ordinary `pinned` flag like any
 * other task, so there is no second check.                                */
static gboolean
pinned_visible(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_has_pinned(app->db);
}

static GPtrArray *
pinned_query(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_tasks_pinned(app->db);
}

/* --- All Tasks ----------------------------------------------------------- */

static GPtrArray *
all_query(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_tasks_all_visible(app->db);
}

/* --- 3-Day Forecast ------------------------------------------------------ */

#define FORECAST_DAYS 3             /* today and the next two, or the next
                                     * three business days counting today */

/* Everything past due, then everything due within the forecast.  The low
 * bound is 1 rather than 0: due == 0 means "no date at all", so 0 would
 * sweep in every undated task instead of every past-due one.             */
static GPtrArray *
forecast_query(TaskApp *app, gpointer d)
{
    (void)d;
    gint64 hi = task_forecast_end(FORECAST_DAYS,
                                  task_config_get_bool(TASK_CFG_FORECAST_BUSINESS_DAYS));
    return task_db_tasks_due_between(app->db, 1, hi, FALSE);
}

/* --- Action Items ------------------------------------------------------- */

/*
 * action_items_visible — whether the Action Items row is shown: only while
 * a live Notes-born task exists, like Favorites while something is pinned.
 *
 * Inputs:
 *   app — the context
 *   d   — unused
 *
 * Output:
 *   TRUE when there is at least one action item.
 */
static gboolean
action_items_visible(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_has_action_items(app->db);
}

/*
 * action_items_query — every live task Notes wrote, wherever the user has
 * filed it since, subtasks included (as Favorites gathers them).  Done ones
 * come back too: the rows layer applies show_completed, as for every view.
 *
 * Inputs:
 *   app — the context
 *   d   — unused
 *
 * Output:
 *   newly allocated GPtrArray of Task* (task_ptr_array_free_tasks).
 */
static GPtrArray *
action_items_query(TaskApp *app, gpointer d)
{
    (void)d;
    return task_db_tasks_action_items(app->db);
}

static const TaskView core_view_pinned = {
    .id           = "pinned",
    .label        = "\xe2\xad\x90\xef\xb8\x8f  Favorites",
    .name         = "Favorites",
    .sort         = 10,
    .visible      = pinned_visible,
    .query        = pinned_query,
    .virtual_rows = TRUE,
};

static const TaskView core_view_all = {
    .id           = "all",
    .label        = "\xf0\x9f\x94\xae  All Tasks",
    .name         = "All Tasks",
    .sort         = 20,
    .query        = all_query,
    .virtual_rows = TRUE,
};

static const TaskView core_view_forecast = {
    .id           = "forecast",
    .label        = "\xe2\x98\x80\xef\xb8\x8f  3-Day Forecast",
    .name         = "3-Day Forecast",
    .sort         = 40,
    .query        = forecast_query,
    .virtual_rows = TRUE,
};

/* After the 3-Day Forecast.  It is named and badged after the managed list
 * Notes files its items in, so the two read as the same thing.            */
static const TaskView core_view_action_items = {
    .id           = "action_items",
    .label        = SHARED_DB_ACTION_LIST_EMOJI "  " SHARED_DB_ACTION_LIST_NAME,
    .name         = SHARED_DB_ACTION_LIST_NAME,
    .unit         = "action item",
    .sort         = 50,
    .visible      = action_items_visible,
    .query        = action_items_query,
    .virtual_rows = TRUE,
};

void
task_core_views_init(void)
{
    task_view_register(&core_view_pinned);
    task_view_register(&core_view_all);
    task_view_register(&core_view_forecast);
    task_view_register(&core_view_action_items);
}
