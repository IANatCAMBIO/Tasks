/* ===========================================================================
 * core_views.c — the app's own virtual views, registered through the
 * public registry (see task_view.h).
 *
 * These four are built in, but they go through task_view_register() like
 * anything else rather than being special-cased in the sidebar.  That is
 * deliberate: the registry is only trustworthy for a plugin's view if it
 * is already carrying the app's own.  If something here needs a back
 * door, the API is wrong and it should grow — not be bypassed.
 *
 * Weekly Forecast is NOT here; it is a panel view and moves out
 * separately.
 * =========================================================================== */

#include "core_views.h"
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

/* --- Due Today ----------------------------------------------------------- */

/* "due_today_show_overdue" widens the low bound to 1 rather than to 0:
 * due == 0 means "no date at all", so 0 would sweep in every undated
 * task instead of every past-due one.                                     */
static GPtrArray *
today_query(TaskApp *app, gpointer d)
{
    (void)d;
    gint64 lo, hi;
    task_day_bounds(0, &lo, &hi);
    if (task_app_config_get_bool("due_today_show_overdue", FALSE))
        lo = 1;
    return task_db_tasks_due_between(app->db, lo, hi);
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

static const TaskView core_view_today = {
    .id           = "today",
    .label        = "\xe2\x98\x80\xef\xb8\x8f  Due Today",
    .name         = "Due Today",
    .sort         = 40,
    .query        = today_query,
    .virtual_rows = TRUE,
};

void
task_core_views_init(void)
{
    task_view_register(&core_view_pinned);
    task_view_register(&core_view_all);
    task_view_register(&core_view_today);
}
