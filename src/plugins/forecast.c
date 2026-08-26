/* ===========================================================================
 * forecast.c — the Weekly Forecast, as a loadable plugin.
 *
 * Seven full-width day sections, Sunday through Saturday, stacked
 * vertically 6 px apart and scrolling together as one page.  Each is a
 * heading over a framed list of that day's tasks at natural height.
 *
 * This is a PANEL view (see task_view.h), not a query view: it is not a
 * task list with a layout, so there is nothing for the host's list or
 * Kanban board to render, and nothing for a manual or card order to
 * order.  That is why it supplies `panel_new` instead of `query`, and
 * why a panel view outranks Kanban.
 *
 * It was part of library_window.c until the plugin API could carry it,
 * and moving it was the API's real test: a panel that builds seven task
 * lists needs the host's ROW RENDERER (rows->*), or it reimplements the
 * task cell and drifts from the rest of the app the first time either
 * changes.  Nothing here draws a task row by hand.
 *
 * DESIGN NOTES worth not relearning:
 *   - In-list day headers and side-by-side day columns were both tried
 *     and rejected for this view (2026-07-16).  The board below the
 *     task pane is deliberately columnar; seven dated days are not.
 *   - The day views are SELECTION_MODE_NONE.  Seven views would each
 *     keep their own selection, leaving up to seven "selected" rows on
 *     screen at once.  Double-click activation and the checkbox both
 *     work without one, and the view reports no selection to the host,
 *     which is the honest answer for Delete Task.
 *   - An empty day still shows one inert dimmed row rather than an empty
 *     frame, so the week reads as seven days rather than four.
 * =========================================================================== */

#include "plugin.h"

static const TaskHostApi *host;
static const TaskPlugin  *self;

#define FORECAST_DAYS 7

/* ---------------------------------------------------------------------------
 * One panel's state.  Attached to the panel widget rather than kept in a
 * file static: the state belongs to the widget's lifetime, and a static
 * would quietly assume there is only ever one panel.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp      *app;
    GtkWidget    *scroller;                    /* the one outer scroller  */
    GtkWidget    *labels[FORECAST_DAYS];       /* day headings            */
    GtkListStore *stores[FORECAST_DAYS];
} Forecast;

#define FORECAST_DATA "forecast-state"

/* ---------------------------------------------------------------------------
 * on_day_toggled() — the ✓ column of a day view.
 *
 * Seven views share this handler, so the model cannot be inferred: each
 * renderer carries its own store as object data.  The status write goes
 * through the host, which owns what a tick MEANS (the status rule, the
 * fade-out when completed tasks are hidden, the refresh) — writing a
 * status directly here would make this checkbox quietly disagree with
 * every other one in the app.
 * ------------------------------------------------------------------------- */
static void
on_day_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    Forecast *f = data;
    GtkListStore *store = g_object_get_data(G_OBJECT(cell), "forecast-model");
    GtkTreeIter iter;
    if (store != NULL &&
        gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter,
                                            path_str))
        host->rows->toggle_done(f->app, store, &iter);
}

/* on_day_activated() — double-click opens the task's editor.  A row with
 * id 0 is the "No tasks due" placeholder and is not a task.              */
static void
on_day_activated(GtkTreeView *view, GtkTreePath *path,
                 GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    Forecast *f = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(gtk_tree_view_get_model(view), &iter, path))
        return;
    gint64 id;
    gtk_tree_model_get(gtk_tree_view_get_model(view), &iter, TL_ID, &id, -1);
    if (id != 0)
        host->ui->editor_open(f->app, id);
}

/* day_toggle_bg_func() — the checkbox column's data func: the row stripe,
 * plus hiding the checkbox on a placeholder row.  A renderer gets ONE
 * data func, so this one does both jobs.                                  */
static void
day_toggle_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                   GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    host->rows->bg_func(col, cell, model, iter, data);
    gint64 id;
    gtk_tree_model_get(model, iter, TL_ID, &id, -1);
    g_object_set(cell, "visible", id != 0, NULL);
}

/* ---------------------------------------------------------------------------
 * day_section() — build one day: a heading label over a framed list.
 *
 * The frame is what makes each day read as its own list where the white
 * rows meet the 6 px gaps.  Natural height, never individually scrolled:
 * the whole week scrolls together in the outer scroller.
 * ------------------------------------------------------------------------- */
static GtkWidget *
day_section(Forecast *f, gint d)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    f->labels[d] = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(f->labels[d]), GTK_JUSTIFY_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(f->labels[d]), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), f->labels[d], FALSE, FALSE, 2);

    f->stores[d] = host->rows->store_new();
    GtkWidget *view =
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(f->stores[d]));
    g_object_unref(f->stores[d]);    /* the view owns it now               */
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    /* The auto search column would be the int64 id and matches nothing.  */
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
        GTK_SELECTION_NONE);
    g_signal_connect(view, "row-activated",
                     G_CALLBACK(on_day_activated), f);

    GtkCellRenderer *done_cell = gtk_cell_renderer_toggle_new();
    g_object_set_data(G_OBJECT(done_cell), "forecast-model", f->stores[d]);
    g_signal_connect(done_cell, "toggled", G_CALLBACK(on_day_toggled), f);
    GtkTreeViewColumn *cdone =
        gtk_tree_view_column_new_with_attributes("\xe2\x9c\x93", done_cell,
                                                 "active", TL_DONE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdone, done_cell,
                                            day_toggle_bg_func, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), cdone);

    GtkCellRenderer *desc_cell = gtk_cell_renderer_text_new();
    g_object_set(desc_cell, "ypad", 6, "ellipsize", PANGO_ELLIPSIZE_END,
                 NULL);
    GtkTreeViewColumn *cdesc =
        gtk_tree_view_column_new_with_attributes("Task", desc_cell,
                                                 "markup", TL_DESC, NULL);
    gtk_tree_view_column_set_cell_data_func(cdesc, desc_cell,
                                            host->rows->bg_func, NULL, NULL);
    gtk_tree_view_column_set_expand(cdesc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), cdesc);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), view);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

/* ---------------------------------------------------------------------------
 * forecast_refresh() — rebuild every day's heading and rows.
 *
 * ONE row context for the whole week: the attachment counts, subtasks and
 * list names are gathered once and shared across all seven stores.  Seven
 * contexts would be seven times the queries for the same answers.
 * ------------------------------------------------------------------------- */
static void
forecast_refresh(TaskApp *app, GtkWidget *panel, gpointer user_data)
{
    (void)user_data;
    Forecast *f = g_object_get_data(G_OBJECT(panel), FORECAST_DATA);
    if (f == NULL)
        return;

    /* Days elapsed since this week's Sunday: GDateTime weekdays run
     * 1 (Monday) through 7 (Sunday), so it is the weekday mod 7.         */
    GDateTime *now = g_date_time_new_now_local();
    gint since_sunday = g_date_time_get_day_of_week(now) % 7;

    /* Before the stores are cleared — clearing zeroes the scrollbar.     */
    host->ui->scroll_keep(f->scroller);

    TaskRowCtx ctx;
    host->rows->ctx_init(app, &ctx, TRUE);
    guint shown = 0;
    for (gint d = 0; d < FORECAST_DAYS; d++) {
        gint offset = d - since_sunday;          /* this day vs. today    */
        GDateTime *day = g_date_time_add_days(now, offset);
        gchar *name = g_date_time_format(day, "%A");
        gchar *date = g_date_time_format(day, "%b %-e");
        /* Today wears a small blue dot beside its name (the sidebar
         * selection blue) and a "— Today" tag on the date line.          */
        gchar *hdr = g_strdup_printf(
            "%s<b>%s</b>\n<small><span alpha=\"60%%\">%s%s</span></small>",
            offset == 0 ? "<small><span foreground=\"#5683e0\">"
                          "\xe2\x97\x8f</span></small> " : "",
            name, date,
            offset == 0 ? " \xe2\x80\x94 Today" : "");
        gtk_label_set_markup(GTK_LABEL(f->labels[d]), hdr);
        g_free(hdr);
        g_free(name);
        g_free(date);
        g_date_time_unref(day);

        gtk_list_store_clear(f->stores[d]);
        gint64 lo, hi;                           /* the day's midnights   */
        task_day_bounds(offset, &lo, &hi);
        GPtrArray *tasks =
            host->db->tasks_due_between(host->db->main_db(app), lo, hi);
        guint n = host->rows->append(f->stores[d], tasks, &ctx);
        host->db->tasks_free(tasks);
        if (n == 0) {
            /* An empty day still shows a one-row list: an inert dimmed
             * placeholder, id 0, whose checkbox is hidden and whose
             * activation is ignored.                                     */
            GtkTreeIter iter;
            gtk_list_store_append(f->stores[d], &iter);
            gtk_list_store_set(f->stores[d], &iter,
                               TL_ID, (gint64)0,
                               TL_DESC, "<i><span alpha=\"55%\">"
                                        "No tasks due</span></i>",
                               TL_DUE, "",
                               -1);
        }
        shown += n;
    }
    g_date_time_unref(now);
    host->rows->ctx_clear(&ctx);

    gchar *loc = g_strdup_printf("Weekly Forecast - %u task%s this week",
                                 shown, shown == 1 ? "" : "s");
    host->ui->set_location(app, loc);
    g_free(loc);
}

/* ---------------------------------------------------------------------------
 * forecast_panel_new() — the seven sections in one outer scroller.
 * ------------------------------------------------------------------------- */
static GtkWidget *
forecast_panel_new(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    Forecast *f = g_new0(Forecast, 1);
    f->app = app;

    GtkWidget *week = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(week), 6);
    for (gint d = 0; d < FORECAST_DAYS; d++)
        gtk_box_pack_start(GTK_BOX(week), day_section(f, d), FALSE, FALSE, 0);

    f->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(f->scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(f->scroller), week);
    /* The state dies with the widget it belongs to.                      */
    g_object_set_data_full(G_OBJECT(f->scroller), FORECAST_DATA, f, g_free);
    return f->scroller;
}

static gboolean
forecast_visible(TaskApp *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    return host->config->get_bool(self, "show_row", TRUE);
}

static const TaskView forecast_view = {
    .id            = "forecast",
    .label         = "\xf0\x9f\x8c\xa4\xef\xb8\x8f  Weekly Forecast",
    .name          = "Weekly Forecast",
    .sort          = 50,
    .visible       = forecast_visible,
    .panel_new     = forecast_panel_new,
    .panel_refresh = forecast_refresh,
    /* No panel_selection: the day views keep no selection, so "nothing"
     * is the honest answer and Delete Task correctly has nothing to act
     * on while the forecast is up.                                       */
    .not_a_list    = "Weekly Forecast is a view, not a list \xe2\x80\x94 "
                     "edit the list each task lives in",
};

/* --------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */
static void
on_show_row_toggled(GtkWidget *check, gpointer data)
{
    TaskApp *app = data;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));
    host->config->set(self, "show_row", on ? "1" : "0");
    host->notify->notify_changed(app);
}

static void
forecast_settings(TaskApp *app, GtkWidget *column, GtkWindow *window,
                  gpointer user_data)
{
    (void)window;
    (void)user_data;
    gtk_box_pack_start(GTK_BOX(column),
                       host->settings->heading("Weekly Forecast"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), host->settings->note(
        "This week at a glance, day by day, with everything due on each."),
        FALSE, FALSE, 0);

    GtkWidget *check = gtk_check_button_new_with_label(
        "Show the Weekly Forecast view in the sidebar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
                                 host->config->get_bool(self, "show_row",
                                                        TRUE));
    g_signal_connect(check, "toggled",
                     G_CALLBACK(on_show_row_toggled), app);
    gtk_box_pack_start(GTK_BOX(column), check, FALSE, FALSE, 0);
}

static gboolean
forecast_init(TaskApp *app, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    host->views->register_view(&forecast_view);
    host->settings->add_section(forecast_settings, NULL);
    return TRUE;
}

static const TaskPlugin forecast_plugin = {
    .abi_version     = TASK_PLUGIN_ABI_VERSION,
    .abi_revision    = TASK_PLUGIN_ABI_REVISION,
    .id              = "forecast",
    .name            = "Weekly Forecast",
    .description     = "This week at a glance, day by day.",
    .version         = "1.0.0",
    .enabled_default = TRUE,
    .init            = forecast_init,
};

TASK_PLUGIN_EXPORT const TaskPlugin *
task_plugin_entry(const TaskHostApi *api)
{
    host = api;
    self = &forecast_plugin;
    return &forecast_plugin;
}
