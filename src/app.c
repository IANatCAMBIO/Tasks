/* ===========================================================================
 * app.c — shared application context for Tasks (see app.h)
 *
 * GTK4 port of the GTK3 app.c.  The listener system, date helpers and config
 * code are kept exactly; GTK3 API (GtkToolItem, GtkMenu, GdkPixbuf surfaces,
 * GtkDialog, GtkFileChooserDialog) is replaced throughout with GTK4
 * equivalents following the Notes app (~/salt_development/notes/src/app.c)
 * as the blueprint.
 *
 * Deleted from the GTK3 build:
 *   dialog_run()               — blocking GTK3 modal loop
 *   task_app_widget_add_css()  — per-widget provider (use task_app_css_install)
 *   task_app_icon_image_rotated() — GdkPixbuf/cairo surface path
 * =========================================================================== */

#include "app.h"
#include "db.h"
#include "editor_window.h"
#include "backup.h"
#include "task_worker.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===========================================================================
 * Change notification (see app.h).
 * =========================================================================== */

/* One entry per subscription.  `fn` is a TaskAppNotifyFn on the
 * changed/tasks lists and a TaskAppStatusFn on the status list.            */
typedef struct {
    guint    id;
    gpointer fn;
    gpointer user_data;
} TaskAppListener;

/*
 * listener_add — append a subscription.
 *
 * Inputs:
 *   app       — the application context.
 *   list      — the subscription list to add to.
 *   fn        — the callback (not NULL).
 *   user_data — passed through to fn.
 *
 * Output:
 *   the subscription id (0 on failure).
 */
static guint
listener_add(TaskApp *app, GSList **list, gpointer fn, gpointer user_data)
{
    if (app == NULL || fn == NULL)
        return 0;
    TaskAppListener *l = g_new0(TaskAppListener, 1);
    l->id        = ++app->listener_next;
    l->fn        = fn;
    l->user_data = user_data;
    *list = g_slist_append(*list, l);
    return l->id;
}

guint
task_app_listen_changed(TaskApp *app, TaskAppNotifyFn fn, gpointer user_data)
{
    return listener_add(app, &app->changed_l, (gpointer)fn, user_data);
}

guint
task_app_listen_tasks(TaskApp *app, TaskAppNotifyFn fn, gpointer user_data)
{
    return listener_add(app, &app->tasks_l, (gpointer)fn, user_data);
}

guint
task_app_listen_status(TaskApp *app, TaskAppStatusFn fn, gpointer user_data)
{
    return listener_add(app, &app->status_l, (gpointer)fn, user_data);
}

/*
 * unlisten_from — drop subscription `id` from one list.
 *
 * Output:
 *   TRUE when found and removed.
 */
static gboolean
unlisten_from(GSList **list, guint id)
{
    for (GSList *n = *list; n != NULL; n = n->next) {
        TaskAppListener *l = n->data;
        if (l->id == id) {
            *list = g_slist_delete_link(*list, n);
            g_free(l);
            return TRUE;
        }
    }
    return FALSE;
}

void
task_app_unlisten(TaskApp *app, guint id)
{
    if (app == NULL || id == 0)
        return;
    unlisten_from(&app->changed_l, id) ||
    unlisten_from(&app->tasks_l,   id) ||
    unlisten_from(&app->status_l,  id);
}

/*
 * fire — call every listener on `list`.
 *
 * The list is COPIED first because a listener may unsubscribe itself (or
 * another) while it runs — the library window's refresh can close an editor
 * — and walking the live list would then step through a freed link.  The
 * copy holds borrowed pointers, so an entry unsubscribed earlier in the
 * same fire would be a use-after-free; ids are checked against the live
 * list to skip exactly that.
 *
 * Inputs:
 *   app     — the application context.
 *   list    — the subscription list to fire.
 *   message — non-NULL for status listeners; NULL for notify listeners.
 */
static void
fire(TaskApp *app, GSList *list, const gchar *message)
{
    GSList *snapshot = g_slist_copy(list);
    for (GSList *n = snapshot; n != NULL; n = n->next) {
        TaskAppListener *l = n->data;
        if (g_slist_find(list, l) == NULL)
            continue;                /* unsubscribed mid-fire                */
        if (message != NULL)
            ((TaskAppStatusFn)l->fn)(app, message, l->user_data);
        else
            ((TaskAppNotifyFn)l->fn)(app, l->user_data);
    }
    g_slist_free(snapshot);
}

/* ---------------------------------------------------------------------------
 * task_app_status() — post an event message to every status listener.
 * ------------------------------------------------------------------------- */
void
task_app_status(TaskApp *app, const gchar *fmt, ...)
{
    if (app == NULL || app->status_l == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fire(app, app->status_l, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * task_app_notify_changed() — fire the full-refresh event (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_notify_changed(TaskApp *app)
{
    if (app != NULL)
        fire(app, app->changed_l, NULL);
}

/* ---------------------------------------------------------------------------
 * task_app_notify_tasks() — fire the task-pane event, falling back to the
 * full one when nothing listens for it (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_notify_tasks(TaskApp *app)
{
    if (app == NULL)
        return;
    if (app->tasks_l != NULL)
        fire(app, app->tasks_l, NULL);
    else
        fire(app, app->changed_l, NULL);
}

/* ===========================================================================
 * Dialogs (GTK4: GtkAlertDialog, GtkFileDialog, custom GtkWindow).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_app_notice() — non-blocking informational dialog (see app.h).
 * GtkAlertDialog shows the title as the heading and fmt as the detail.
 * ------------------------------------------------------------------------- */
void
task_app_notice(GtkWindow *parent, const gchar *title,
                const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *message = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    /* The heading is the title when there is one, with the message as the
     * detail; without a title the message IS the heading.
     * gtk_alert_dialog_show copies everything into the window it presents,
     * so the dialog object is not needed once it is up.                      */
    GtkAlertDialog *dialog =
        gtk_alert_dialog_new("%s", title != NULL ? title : message);
    if (title != NULL)
        gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
    g_free(message);
}

/* ConfirmJob — what task_app_confirm() carries across the async gap.        */
typedef struct {
    TaskConfirmFn done;
    gpointer      user_data;
} ConfirmJob;

/*
 * confirm_done — GAsyncReadyCallback for task_app_confirm(): maps the
 * button index to a yes/no and hands it to the caller's completion.
 *   source    — the GtkAlertDialog.
 *   result    — the async result.
 *   user_data — the ConfirmJob.
 */
static void
confirm_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ConfirmJob     *job    = user_data;
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    /* button 0 = "_No", button 1 = "_Yes", -1 = dismissed                  */
    gint button = gtk_alert_dialog_choose_finish(dialog, result, NULL);
    job->done(button == 1, job->user_data);
    g_free(job);
}

/* ---------------------------------------------------------------------------
 * task_app_confirm() — async Yes/No dialog (see app.h).
 * `message` is the already-formatted detail text.
 * ------------------------------------------------------------------------- */
void
task_app_confirm(GtkWindow *parent, const gchar *title, const gchar *message,
                 TaskConfirmFn done, gpointer user_data)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    static const char *BUTTONS[] = { "_No", "_Yes", NULL };
    gtk_alert_dialog_set_buttons(dialog, BUTTONS);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 1);

    ConfirmJob *job   = g_new0(ConfirmJob, 1);
    job->done      = done;
    job->user_data = user_data;
    gtk_alert_dialog_choose(dialog, parent, NULL, confirm_done, job);
}

/* PickJob — what task_app_pick_path() carries across the async gap.         */
typedef struct {
    TaskPickKind kind;                /* open / save / folder                 */
    TaskPickFn   done;                /* the caller's completion              */
    gpointer     user_data;
} PickJob;

/*
 * pick_path_done — GAsyncReadyCallback for task_app_pick_path().
 */
static void
pick_path_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PickJob       *job    = user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *file;

    /* A cancel comes back with a NULL file (GTK_DIALOG_ERROR_DISMISSED) —
     * that is the answer, not an error worth reporting.                      */
    switch (job->kind) {
    case TASK_PICK_OPEN:
        file = gtk_file_dialog_open_finish(dialog, result, NULL);
        break;
    case TASK_PICK_SAVE:
        file = gtk_file_dialog_save_finish(dialog, result, NULL);
        break;
    default:
        file = gtk_file_dialog_select_folder_finish(dialog, result, NULL);
        break;
    }
    gchar *path = (file != NULL) ? g_file_get_path(file) : NULL;
    g_clear_object(&file);

    job->done(path, job->user_data);  /* the completion owns path            */
    g_object_unref(dialog);           /* the ref task_app_pick_path took      */
    g_free(job);
}

/* ---------------------------------------------------------------------------
 * task_app_pick_path() — async file/folder chooser (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_pick_path(GtkWindow *parent, const gchar *title,
                   TaskPickKind kind, const gchar *accept_label,
                   const gchar *filter_name, const gchar *filter_pattern,
                   const gchar *start_dir,
                   TaskPickFn done, gpointer user_data)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_set_accept_label(dialog, accept_label);
    if (start_dir != NULL) {
        GFile *folder = g_file_new_for_path(start_dir);
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    if (filter_name != NULL) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_name);
        if (filter_pattern != NULL)
            gtk_file_filter_add_pattern(filter, filter_pattern);
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filters);
        g_object_unref(filter);
    }

    PickJob *job   = g_new0(PickJob, 1);
    job->kind      = kind;
    job->done      = done;
    job->user_data = user_data;
    switch (kind) {
    case TASK_PICK_OPEN:
        gtk_file_dialog_open(dialog, parent, NULL, pick_path_done, job);
        break;
    case TASK_PICK_SAVE:
        gtk_file_dialog_save(dialog, parent, NULL, pick_path_done, job);
        break;
    default:
        gtk_file_dialog_select_folder(dialog, parent, NULL, pick_path_done,
                                      job);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * task_app_dialog_new() — a custom modal window with Accept / Cancel (see
 * app.h).  GTK4 has no GtkDialog; we build a plain GtkWindow.
 *
 * The close-request handler (window X or Escape) calls dialog_respond FALSE
 * and returns TRUE (preventing GTK's default destroy) so every exit path
 * goes through dialog_respond and the window is always destroyed there.
 * The responded flag prevents a double-call when dialog_respond() is invoked
 * from a button AND GTK later delivers a close-request on the same frame.
 * ------------------------------------------------------------------------- */

/* DialogJob — what the two button callbacks and the close-request share.    */
typedef struct {
    TaskDialogFn done;
    gpointer     user_data;
    gboolean     responded;          /* first response wins; prevents double-call */
} DialogJob;

/* dialog_respond() — deliver the result, then destroy the dialog window.    */
static void
dialog_respond(GtkWindow *dialog, gboolean accepted, DialogJob *job)
{
    if (job->responded)
        return;
    job->responded = TRUE;
    job->done(accepted, dialog, job->user_data);
    gtk_window_destroy(dialog);
}

static void
dialog_on_accept(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkRoot   *root   = gtk_widget_get_root(GTK_WIDGET(btn));
    DialogJob *job    = data;
    dialog_respond(GTK_WINDOW(root), TRUE, job);
}

static void
dialog_on_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkRoot   *root   = gtk_widget_get_root(GTK_WIDGET(btn));
    DialogJob *job    = data;
    dialog_respond(GTK_WINDOW(root), FALSE, job);
}

/* dialog_on_key() — Escape key → respond FALSE.                             */
static gboolean
dialog_on_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
              GdkModifierType mods, gpointer data)
{
    (void)keycode; (void)mods;
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    GtkWidget *dialog = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(ctrl));
    dialog_respond(GTK_WINDOW(dialog), FALSE, data);
    return TRUE;
}

/* dialog_close_request() — window close button or destroy request.          */
static gboolean
dialog_close_request(GtkWindow *dialog, gpointer data)
{
    dialog_respond(dialog, FALSE, data);
    return TRUE;                     /* we handle it; GTK must not also close */
}

GtkWindow *
task_app_dialog_new(GtkWindow *parent, const gchar *title,
                    GtkWidget *content, const gchar *accept_label,
                    TaskDialogFn done, gpointer user_data)
{
    DialogJob *job    = g_new0(DialogJob, 1);
    job->done      = done;
    job->user_data = user_data;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    /* job's lifetime is tied to the dialog's object lifetime                 */
    g_object_set_data_full(G_OBJECT(dialog), "task-dialog-job", job, g_free);

    /* Escape key → cancel                                                    */
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(dialog_on_key), job);
    gtk_widget_add_controller(dialog, key);
    /* Close-request (window X button) → cancel                               */
    g_signal_connect(dialog, "close-request",
                     G_CALLBACK(dialog_close_request), job);

    /* Layout: content on top, button row at the bottom.                      */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_box_append(GTK_BOX(vbox), content);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget *cancel_btn = gtk_button_new_with_mnemonic("_Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(dialog_on_cancel), job);
    gtk_box_append(GTK_BOX(hbox), cancel_btn);

    GtkWidget *accept_btn = gtk_button_new_with_mnemonic(accept_label);
    gtk_widget_add_css_class(accept_btn, "suggested-action");
    g_signal_connect(accept_btn, "clicked", G_CALLBACK(dialog_on_accept), job);
    gtk_box_append(GTK_BOX(hbox), accept_btn);

    gtk_window_present(GTK_WINDOW(dialog));
    return GTK_WINDOW(dialog);
}

/* ===========================================================================
 * task_app_menu_popup() — GtkPopoverMenu at a position (see app.h).
 *
 * Follows the Notes pattern exactly (on_app_menu_popup in notes/src/app.c).
 * The popover is parented to the window's own child box, not to `attach`,
 * because a popover on a GtkColumnView or GtkListView breaks GTK's CSS-node
 * chain and comes up the wrong size (Notes D35).
 * =========================================================================== */

/* menu_popup_drop() — take the popover down for good.                        */
static void
menu_popup_drop(GtkWidget *popover)
{
    guint idle = GPOINTER_TO_UINT(
        g_object_steal_data(G_OBJECT(popover), "task-popup-idle"));
    if (idle != 0)
        g_source_remove(idle);
    gulong handler = GPOINTER_TO_SIZE(
        g_object_steal_data(G_OBJECT(popover), "task-popup-unrealize"));
    GtkWidget *parent = gtk_widget_get_parent(popover);
    if (parent != NULL) {
        if (handler != 0)
            g_signal_handler_disconnect(parent, handler);
        gtk_widget_unparent(popover);
    }
    if (g_object_steal_data(G_OBJECT(popover), "task-popup-ref") != NULL)
        g_object_unref(popover);
}

static gboolean
menu_popup_idle(gpointer data)
{
    g_object_set_data(G_OBJECT(data), "task-popup-idle", NULL);
    menu_popup_drop(data);
    return G_SOURCE_REMOVE;
}

static void
menu_popup_closed(GtkPopover *popover, gpointer user_data)
{
    (void)user_data;
    g_object_set_data(G_OBJECT(popover), "task-popup-idle",
        GUINT_TO_POINTER(g_idle_add(menu_popup_idle, popover)));
}

static void
menu_popup_parent_unrealize(GtkWidget *parent, gpointer user_data)
{
    (void)parent;
    menu_popup_drop(user_data);
}

void
task_app_menu_popup(GtkWidget *attach, GMenuModel *model, gdouble x, gdouble y)
{
    GtkRoot   *root   = gtk_widget_get_root(attach);
    GtkWidget *parent = gtk_window_get_child(GTK_WINDOW(root));
    graphene_point_t at_parent;
    if (!gtk_widget_compute_point(attach, parent,
                                  &GRAPHENE_POINT_INIT((float)x, (float)y),
                                  &at_parent))
        at_parent = GRAPHENE_POINT_INIT((float)x, (float)y);

    GtkWidget *popover = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);
    gtk_widget_set_parent(popover, parent);
    g_object_set_data(G_OBJECT(popover), "task-popup-ref",
                      g_object_ref(popover));
    g_object_set_data(G_OBJECT(popover), "task-popup-unrealize",
        GSIZE_TO_POINTER(g_signal_connect(parent, "unrealize",
            G_CALLBACK(menu_popup_parent_unrealize), popover)));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    GdkRectangle at = { (gint)at_parent.x, (gint)at_parent.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &at);
    g_signal_connect(popover, "closed", G_CALLBACK(menu_popup_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

/* ---------------------------------------------------------------------------
 * task_app_menu_section_end() — see app.h.
 * ------------------------------------------------------------------------- */
void
task_app_menu_section_end(GMenu *menu, GMenu **section)
{
    g_menu_append_section(menu, NULL, G_MENU_MODEL(*section));
    g_object_unref(*section);
    *section = g_menu_new();
}

/* ===========================================================================
 * CSS helpers.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_app_css_install() — load a CSS string once, globally (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_css_install(const gchar *css)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ===========================================================================
 * Toolbar icons (GTK4 icon theme path — see app.h).
 *
 * The executable puts its icons/ directory on the icon theme's search path
 * (main.c, on_startup).  Once that is done, every local PNG can be loaded by
 * name through the theme — the name is the extension-stripped basename, and
 * GTK probes that path for the file.  Callers then work entirely through icon
 * names, not file paths.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_app_init_icons_dir() — icons/ next to the executable (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_init_icons_dir(TaskApp *app)
{
    app->icons_dir = g_build_filename(task_app_exe_dir(), "icons", NULL);
}

/*
 * display_scale_factor — the integer scale factor of the first listed monitor
 * (2 on Retina), 1 when no display or monitor is known yet.
 * GTK4 has no "primary" monitor; the first one is the app's home for icon
 * rasterization.
 */
static gint
display_scale_factor(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL)
        return 1;
    GdkMonitor *monitor =
        g_list_model_get_item(gdk_display_get_monitors(display), 0);
    if (monitor == NULL)
        return 1;
    gint sf = gdk_monitor_get_scale_factor(monitor);
    g_object_unref(monitor);
    return sf;
}

/*
 * icon_theme_has — does the icon theme know `name`?
 * main.c adds icons/ as a search path, where GTK picks up PNGs as
 * "unthemed" icons by basename, so this IS the test for "a local file
 * exists and loads" — GTK would otherwise hand back its missing-image
 * placeholder and the caller wants the text fallback instead.
 */
static gboolean
icon_theme_has(const gchar *name)
{
    return gtk_icon_theme_has_icon(
        gtk_icon_theme_get_for_display(gdk_display_get_default()), name);
}

/* ---------------------------------------------------------------------------
 * task_app_icon_image_sized() — a GtkImage at an explicit pixel size
 * (see app.h).  Returns NULL when the icon is not in the theme (caller falls
 * back to a markup label).
 * ------------------------------------------------------------------------- */
GtkWidget *
task_app_icon_image_sized(TaskApp *app, const gchar *name, gint size)
{
    (void)app;                        /* the theme knows the directory        */
    if (!icon_theme_has(name))
        return NULL;
    GtkWidget *image = gtk_image_new_from_icon_name(name);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

/* ---------------------------------------------------------------------------
 * task_app_icon_paintable() — a GdkPaintable for a local icon, scaled for
 * the display (see app.h).  Used for drag icons and other non-widget uses.
 * ------------------------------------------------------------------------- */
GdkPaintable *
task_app_icon_paintable(TaskApp *app, const gchar *name, gint size)
{
    (void)app;
    if (!icon_theme_has(name))
        return NULL;
    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
        gtk_icon_theme_get_for_display(gdk_display_get_default()),
        name, NULL, size, display_scale_factor(), GTK_TEXT_DIR_NONE, 0);
    return GDK_PAINTABLE(icon);
}

/* ===========================================================================
 * Tooltips (see app.h).
 *
 * GTK4's tooltip-window mechanism fires "query-tooltip" frequently on a
 * Retina/quartz display; a tooltip asked for within TOOLTIP_MIN_GAP_MS of the
 * previous one hiding is refused and re-asked once the gap has passed.
 * This prevents the resize crash recorded as Notes D32.
 * =========================================================================== */

/* A tooltip within this many ms of the previous one hiding is refused and
 * re-asked.  550 ms is past GTK's browse-mode window (500 ms), so the
 * re-ask goes through the normal hover delay.                                */
#define TOOLTIP_MIN_GAP_MS 550

/* The GtkLabel shown as the tooltip widget; one per watched widget.          */
#define TOOLTIP_LABEL_KEY "task-tooltip-label"

static GtkWidget *tooltip_mapped;     /* the label showing RIGHT NOW, if any  */
static gint64     tooltip_hidden_at;  /* monotonic µs when the last one hid   */

static void
on_tooltip_label_map(GtkWidget *label, gpointer data)
{
    (void)data;
    tooltip_mapped = label;
}

static void
on_tooltip_label_unmap(GtkWidget *label, gpointer data)
{
    (void)data;
    if (tooltip_mapped == label)
        tooltip_mapped = NULL;
    tooltip_hidden_at = g_get_monotonic_time();
}

static gboolean
tooltip_ask_again(gpointer widget)
{
    gtk_widget_trigger_tooltip_query(widget);
    g_object_unref(widget);
    return G_SOURCE_REMOVE;
}

static gboolean
on_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard,
                 GtkTooltip *tooltip, gpointer data)
{
    (void)x; (void)y; (void)keyboard; (void)data;
    GtkWidget *label = g_object_get_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY);
    if (label == NULL)
        return FALSE;
    /* Refused in a non-active window (macOS: a popup on an inactive library
     * window orders it above the editor the user is typing in).              */
    GtkRoot *root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root) && !gtk_window_is_active(GTK_WINDOW(root)))
        return FALSE;
    if (tooltip_mapped == NULL) {
        gint64 gap = g_get_monotonic_time() - tooltip_hidden_at;
        if (gap < TOOLTIP_MIN_GAP_MS * 1000) {
            g_timeout_add(
                (guint)((TOOLTIP_MIN_GAP_MS * 1000 - gap) / 1000) + 1,
                tooltip_ask_again, g_object_ref(widget));
            return FALSE;
        }
    }
    gtk_tooltip_set_custom(tooltip, label);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_app_set_tooltip() — see app.h.  Installs a custom label widget so the
 * query-tooltip gap guard above applies.
 * ------------------------------------------------------------------------- */
void
task_app_set_tooltip(GtkWidget *widget, const gchar *text)
{
    if (text == NULL || *text == '\0') {
        g_object_set_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY, NULL);
        gtk_widget_set_has_tooltip(widget, FALSE);
        return;
    }
    GtkWidget *label = g_object_get_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY);
    if (label == NULL) {
        label = gtk_label_new(text);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        g_signal_connect(label, "map",   G_CALLBACK(on_tooltip_label_map),   NULL);
        g_signal_connect(label, "unmap", G_CALLBACK(on_tooltip_label_unmap), NULL);
        g_object_set_data_full(G_OBJECT(widget), TOOLTIP_LABEL_KEY,
                               g_object_ref_sink(label), g_object_unref);
        g_signal_connect(widget, "query-tooltip",
                         G_CALLBACK(on_query_tooltip), NULL);
    } else {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
    gtk_widget_set_has_tooltip(widget, TRUE);
}

/* ===========================================================================
 * Toolbar buttons (see app.h).
 *
 * GTK4 has no GtkToolbar or GtkToolItem.  A toolbar is a GtkBox with
 * "toolbar" CSS class; each button is a flat GtkButton or GtkToggleButton.
 * =========================================================================== */

/* Object-data key that keeps a toolbar button's accessible label so
 * task_app_tool_item_set_icon can rebuild the markup fallback from it.       */
#define TOOL_LABEL_KEY "task-tool-label"

/*
 * tool_icon_widget — the icon widget for a toolbar button.
 *
 * Returns the local icon image when the name is in the theme, else a markup
 * label as fallback.  THE single place that rule lives: shared by
 * task_app_tool_item_new and task_app_tool_item_set_icon so a button built
 * with an icon and one re-pointed at one cannot come to disagree about the
 * fallback.
 *
 * Inputs:
 *   app             — application context (holds icons_dir).
 *   icon_name       — icon file basename, or NULL for markup only.
 *   fallback_markup — Pango markup when the file does not load, or NULL.
 *   label           — last-resort text when both are absent.
 *
 * Output:
 *   a floating GtkWidget for the caller to parent.  Never NULL.
 */
static GtkWidget *
tool_icon_widget(TaskApp *app, const gchar *icon_name,
                 const gchar *fallback_markup, const gchar *label)
{
    GtkWidget *icon = (icon_name != NULL)
                      ? task_app_icon_image_sized(app, icon_name, 24) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup : label);
    }
    return icon;
}

/* ---------------------------------------------------------------------------
 * task_app_tool_item_new() — a toolbar button (see app.h).
 * ------------------------------------------------------------------------- */
GtkWidget *
task_app_tool_item_new(TaskApp *app, gboolean toggle,
                       const gchar *icon_name,
                       const gchar *fallback_markup,
                       const gchar *label, const gchar *tooltip)
{
    GtkWidget *button = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);  /* flat             */
    /* A toolbar press must not steal the focus from any text view: editing
     * actions are gated on focus.                                             */
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_button_set_child(GTK_BUTTON(button),
        tool_icon_widget(app, icon_name, fallback_markup, label));
    task_app_set_tooltip(button, tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    g_object_set_data_full(G_OBJECT(button), TOOL_LABEL_KEY,
                           g_strdup(label), g_free);
    return button;
}

/* ---------------------------------------------------------------------------
 * task_app_tool_item_set_icon() — swap a toolbar button's icon (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_tool_item_set_icon(TaskApp *app, GtkWidget *button,
                            const gchar *icon_name,
                            const gchar *fallback_markup)
{
    /* set_child unparents and drops the old icon widget, which held the only
     * reference to it, so the previous image is freed by this call.          */
    gtk_button_set_child(GTK_BUTTON(button),
        tool_icon_widget(app, icon_name, fallback_markup,
                         g_object_get_data(G_OBJECT(button), TOOL_LABEL_KEY)));
}

/* ===========================================================================
 * task_app_double_click_watch() / task_app_select_on_press() (see app.h).
 *
 * Both follow the Notes pattern exactly (on_app_double_click_watch /
 * on_app_select_on_press in notes/src/app.c).
 * =========================================================================== */

/* DoubleClick — the last primary press on a watched widget: time, position
 * and the callback.  Stored as the gesture's data, NOT as gesture state, so
 * a gesture reset does not lose the first press.                             */
typedef struct {
    TaskDoubleClickFn cb;
    gpointer          data;
    guint32           last_time;      /* ms, 0 = none                         */
    gdouble           last_x, last_y;
} DoubleClick;

/*
 * on_double_click_pressed — every primary press: a second one within the
 * settings' time and distance of the first is the double-click.
 * n_press is deliberately unused: it is what a gesture reset zeroes.
 */
static void
on_double_click_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                        gpointer user_data)
{
    (void)n_press;
    DoubleClick *dc = user_data;
    GtkWidget *widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(g));
    GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(g), NULL);
    guint32 now     = event != NULL ? gdk_event_get_time(event) : 0;
    gint time_ms = 400, dist = 5;    /* the settings' documented defaults    */
    g_object_get(gtk_widget_get_settings(widget),
                 "gtk-double-click-time",     &time_ms,
                 "gtk-double-click-distance", &dist,
                 NULL);
    gboolean second = dc->last_time != 0 &&
                      now - dc->last_time <= (guint32)time_ms &&
                      ABS(x - dc->last_x) <= dist && ABS(y - dc->last_y) <= dist;
    if (second) {
        dc->last_time = 0;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        dc->cb(widget, dc->data);
    } else {
        dc->last_time = now;
        dc->last_x    = x;
        dc->last_y    = y;
    }
}

/* ---------------------------------------------------------------------------
 * task_app_double_click_watch() — see app.h.
 * ------------------------------------------------------------------------- */
void
task_app_double_click_watch(GtkWidget *widget, TaskDoubleClickFn cb,
                            gpointer data)
{
    DoubleClick *dc = g_new0(DoubleClick, 1);
    dc->cb   = cb;
    dc->data = data;
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g),
                                               GTK_PHASE_CAPTURE);
    g_object_set_data_full(G_OBJECT(g), "task-double-click", dc, g_free);
    g_signal_connect(g, "pressed", G_CALLBACK(on_double_click_pressed), dc);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g));
}

/* SelectPress — a watched row's list item, and whether the press left the
 * selection alone for a drag that the release must then collapse.           */
typedef struct {
    GtkListItem *item;
    gboolean     collapse;
} SelectPress;

/*
 * select_press_modifiers — GTK's own reading of a press's modifiers:
 * Shift extends, Control toggles; on macOS Command also toggles.
 */
static void
select_press_modifiers(GtkGesture *g, gboolean *modify, gboolean *extend)
{
    GdkEvent *event = gtk_gesture_get_last_event(g, NULL);
    GdkModifierType state = event != NULL ? gdk_event_get_modifier_state(event)
                                          : 0;
    *extend = (state & GDK_SHIFT_MASK)   != 0;
    *modify = (state & GDK_CONTROL_MASK) != 0;
#ifdef __APPLE__
    *modify = *modify || (state & GDK_META_MASK) != 0;
#endif
}

/* select_item() — run the view's "list.select-item" action for the row.     */
static void
select_item(GtkWidget *widget, guint pos, gboolean modify, gboolean extend)
{
    gtk_widget_activate_action(widget, "list.select-item", "(ubb)",
                               pos, modify, extend);
}

static void
on_select_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                  gpointer user_data)
{
    (void)x; (void)y;
    SelectPress *sp = user_data;
    sp->collapse = FALSE;
    if (n_press != 1)
        return;
    guint pos = gtk_list_item_get_position(sp->item);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;
    gboolean modify, extend;
    select_press_modifiers(GTK_GESTURE(g), &modify, &extend);
    if (!modify && !extend && gtk_list_item_get_selected(sp->item)) {
        sp->collapse = TRUE;
        return;
    }
    select_item(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                pos, modify, extend);
}

static void
on_select_released(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                   gpointer user_data)
{
    (void)n_press; (void)x; (void)y;
    SelectPress *sp = user_data;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    if (!sp->collapse)
        return;
    sp->collapse = FALSE;
    guint pos = gtk_list_item_get_position(sp->item);
    if (pos != GTK_INVALID_LIST_POSITION)
        select_item(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                    pos, FALSE, FALSE);
}

/* ---------------------------------------------------------------------------
 * task_app_select_on_press() — see app.h.
 * ------------------------------------------------------------------------- */
void
task_app_select_on_press(GtkWidget *widget, GtkListItem *item)
{
    SelectPress *sp = g_new0(SelectPress, 1);
    sp->item = item;
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g),
                                               GTK_PHASE_CAPTURE);
    g_object_set_data_full(G_OBJECT(g), "task-select-press", sp, g_free);
    g_signal_connect(g, "pressed",  G_CALLBACK(on_select_pressed),  sp);
    g_signal_connect(g, "released", G_CALLBACK(on_select_released), sp);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g));
}

/* ===========================================================================
 * Config — ini next to the binary (see app.h).
 *
 * THE INI LIVES WITH THE DATABASE, in task_db_default_dir()
 * (~/.local/share/tasks on Linux).  Three resolution steps (see the full
 * comment in the GTK3 build): data-dir ini if it EXISTS, else binary-adjacent
 * if it EXISTS (portable mode), else CREATE at data-dir.
 * =========================================================================== */

#define TASK_INI_GROUP    "tasks"
#define TASK_INI_FILE     "tasks.ini"
#define TASK_INI_DEFAULTS "tasks.ini.defaults"

static GKeyFile *config_kf       = NULL;  /* the in-memory config            */
static gchar    *config_path     = NULL;  /* written through on every change  */
static gchar    *exe_dir_cached  = NULL;  /* binary's directory (owned)       */

/*
 * exe_dir_from_argv0 — the directory holding the binary (new string).
 * When launched via a bare name from PATH there is no directory part, so fall
 * back to the current working directory.
 */
static gchar *
exe_dir_from_argv0(const gchar *argv0)
{
    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        gchar *dir = g_path_get_dirname(abs);
        g_free(abs);
        return dir;
    }
    return g_get_current_dir();
}

const gchar *
task_app_exe_dir(void)
{
    return exe_dir_cached;
}

/* ---------------------------------------------------------------------------
 * task_app_config_init() — resolve + load the config file once (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;

    gchar *exe_dir = exe_dir_from_argv0(argv0);
    exe_dir_cached = g_strdup(exe_dir);

    gchar *data_dir = task_db_default_dir();
    gchar *shared   = g_build_filename(data_dir, TASK_INI_FILE, NULL);
    gchar *local    = g_build_filename(exe_dir,  TASK_INI_FILE, NULL);

    if (g_file_test(shared, G_FILE_TEST_EXISTS)) {
        config_path = shared;
        g_free(local);
    } else if (g_file_test(local, G_FILE_TEST_EXISTS)) {
        config_path = local;
        g_free(shared);
    } else {
        config_path = shared;
        g_free(local);
    }
    g_free(data_dir);

    config_kf = g_key_file_new();
    if (!g_key_file_load_from_file(config_kf, config_path,
                                   G_KEY_FILE_NONE, NULL)) {
        gchar *defaults = g_build_filename(exe_dir, TASK_INI_DEFAULTS, NULL);
        g_key_file_load_from_file(config_kf, defaults, G_KEY_FILE_NONE, NULL);
        g_free(defaults);
    }
    g_free(exe_dir);
}

/* ---------------------------------------------------------------------------
 * task_app_config_get() — read one setting; NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *
task_app_config_get(const gchar *key)
{
    if (config_kf == NULL)
        return NULL;
    gchar *v = g_key_file_get_string(config_kf, TASK_INI_GROUP, key, NULL);
    if (v != NULL && *v == '\0') {
        g_free(v);
        v = NULL;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * task_app_config_get_bool() — read a 0/1 setting (see app.h).
 * ------------------------------------------------------------------------- */
gboolean
task_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar *v = task_app_config_get(key);
    if (v == NULL)
        return def;
    gboolean b = strcmp(v, "0") != 0;
    g_free(v);
    return b;
}

/* ---------------------------------------------------------------------------
 * task_app_config_set() — change one setting and write the ini through.
 * NULL removes the key.  Unchanged values skip the rewrite.
 * ------------------------------------------------------------------------- */
void
task_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL)
        return;
    gchar *old = g_key_file_get_string(config_kf, TASK_INI_GROUP, key, NULL);
    gboolean same = (old == NULL && value == NULL) ||
                    (old != NULL && value != NULL &&
                     strcmp(old, value) == 0);
    g_free(old);
    if (same)
        return;
    if (value != NULL)
        g_key_file_set_string(config_kf, TASK_INI_GROUP, key, value);
    else
        g_key_file_remove_key(config_kf, TASK_INI_GROUP, key, NULL);
    g_key_file_save_to_file(config_kf, config_path, NULL);
}

/* ===========================================================================
 * Date helpers (see app.h).
 *
 * EVERY GLib "_local" constructor resolves the local timezone from scratch,
 * and that resolution is the whole cost of a date operation at scale.
 * Measured on GLib 2.88.2: g_time_zone_new_local() is 7318 ns against 175 ns
 * for building a GDateTime once a GTimeZone is in hand — 40x.  Per row and
 * per draw at 500 rows that was 13.5 ms of timezone lookups per refresh.
 *
 * The zone is CACHED, and every constructor here takes it explicitly.
 * There is no g_date_time_new_from_unix(tz, t), which is why task_local_dt
 * goes through _from_unix_utc + g_date_time_to_timezone (160 ns vs 8316).
 * =========================================================================== */

static GTimeZone *local_tz = NULL;   /* owned                                */
static gint64     local_lo = 0;      /* [lo, hi) today window, unix seconds  */
static gint64     local_hi = 0;

/*
 * local_cache_ensure — resolve the zone and today's bounds if the cache is
 * empty or the day has rolled over.  One time(NULL) and two comparisons on
 * the common path.
 */
static void
local_cache_ensure(void)
{
    gint64 now = (gint64)time(NULL);
    if (local_tz != NULL && now >= local_lo && now < local_hi)
        return;

    g_clear_pointer(&local_tz, g_time_zone_unref);
    local_tz = g_time_zone_new_local();

    GDateTime *n   = g_date_time_new_now(local_tz);
    GDateTime *mid = n != NULL
        ? g_date_time_new(local_tz, g_date_time_get_year(n),
                          g_date_time_get_month(n),
                          g_date_time_get_day_of_month(n), 0, 0, 0)
        : NULL;
    GDateTime *nxt = mid != NULL ? g_date_time_add_days(mid, 1) : NULL;
    if (nxt != NULL) {
        local_lo = g_date_time_to_unix(mid);
        local_hi = g_date_time_to_unix(nxt);
    } else {
        /* Cannot happen from a real GDateTime, but [now, now) would
         * re-resolve the zone on every call.  A minute's grace.             */
        local_lo = now;
        local_hi = now + 60;
    }
    g_clear_pointer(&n,   g_date_time_unref);
    g_clear_pointer(&mid, g_date_time_unref);
    g_clear_pointer(&nxt, g_date_time_unref);
}

GTimeZone *
task_local_tz(void)
{
    local_cache_ensure();
    return local_tz;
}

GDateTime *
task_local_dt(gint64 unix_ts)
{
    GDateTime *utc = g_date_time_new_from_unix_utc(unix_ts);
    if (utc == NULL)
        return NULL;
    GDateTime *local = g_date_time_to_timezone(utc, task_local_tz());
    g_date_time_unref(utc);
    return local;
}

void
task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi)
{
    GTimeZone *tz  = task_local_tz();
    GDateTime *now = g_date_time_new_now(tz);
    GDateTime *day = g_date_time_add_days(now, offset_days);
    GDateTime *mid = g_date_time_new(tz, g_date_time_get_year(day),
                                     g_date_time_get_month(day),
                                     g_date_time_get_day_of_month(day),
                                     0, 0, 0);
    GDateTime *nxt = g_date_time_add_days(mid, 1);
    *lo = g_date_time_to_unix(mid);
    *hi = g_date_time_to_unix(nxt);
    g_date_time_unref(now);
    g_date_time_unref(day);
    g_date_time_unref(mid);
    g_date_time_unref(nxt);
}

gchar *
task_due_format(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = task_local_dt(due);
    if (dt == NULL)
        return g_strdup("");
    gchar *s = g_date_time_format(dt, "%b %-e, %Y");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * task_clock_format() — "8:00 AM" for minutes past local midnight.
 *
 * "%I:%M %p" with the leading zero dropped BY HAND.  GLib's "%l" pads with
 * U+2007 FIGURE SPACE (not ASCII), so g_strstrip leaves it and the sentence
 * reads "at  8:00 AM" (gotcha 23; measured against GLib 2.84).
 * ------------------------------------------------------------------------- */
gchar *
task_clock_format(gint minutes)
{
    if (minutes < 0)
        minutes = 0;
    if (minutes > 23 * 60 + 59)
        minutes = 23 * 60 + 59;
    GDateTime *dt = g_date_time_new(task_local_tz(), 2000, 1, 1,
                                    minutes / 60, minutes % 60, 0.0);
    if (dt == NULL)
        return g_strdup("");
    gchar *clock = g_date_time_format(dt, "%I:%M %p");
    g_date_time_unref(dt);
    if (clock == NULL)
        return g_strdup("");
    g_strchomp(clock);
    if (clock[0] == '0')
        memmove(clock, clock + 1, strlen(clock));
    return clock;
}

gint64
task_due_instant(gint64 due, gint due_time)
{
    if (due == 0)
        return 0;
    if (due_time < 0 || due_time > 23 * 60 + 59)
        due_time = TASK_DUE_TIME_DEFAULT;
    return due + (gint64)due_time * 60;
}

gchar *
task_due_format_at(gint64 due, gint due_time)
{
    gchar *date = task_due_format(due);
    if (due == 0 || due_time == TASK_DUE_TIME_DEFAULT)
        return date;
    gchar *clock = task_clock_format(due_time);
    gchar *out   = g_strdup_printf("%s %s", date, clock);
    g_free(clock);
    g_free(date);
    return out;
}

gchar *
task_due_format_iso(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = task_local_dt(due);
    if (dt == NULL)
        return g_strdup("");
    gchar *s = g_date_time_format(dt, "%Y-%m-%d");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * task_due_color() — urgency tint (see app.h).
 *
 * THE HOTTEST FUNCTION IN THE APP: the Due column's cell data func runs per
 * visible row per DRAW.  With the cached day window it is 21 ns, 703x faster
 * than building two GDateTimes a call (measured).  No GDateTime here.
 * `due` is the full instant (task_due_instant, what TL_DUE_RAW holds).
 * ------------------------------------------------------------------------- */
const gchar *
task_due_color(gint64 due)
{
    if (due == 0)
        return NULL;
    local_cache_ensure();
    return due < local_lo ? "#c01c28"   /* overdue: red                       */
         : due < local_hi ? "#d19a00"   /* today: gold                        */
                          : "#26a269";  /* ahead: green                       */
}

gint64
task_due_from_ymd(gint y, gint m, gint d)
{
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 9999)
        return 0;
    GDateTime *dt = g_date_time_new(task_local_tz(), y, m, d, 0, 0, 0);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/* ---------------------------------------------------------------------------
 * task_due_parse() — "YYYY-MM-DD" or "M/D/YY[YY]" → local midnight unix.
 * ------------------------------------------------------------------------- */
gint64
task_due_parse(const gchar *text)
{
    if (text == NULL)
        return 0;
    gchar *t = g_strstrip(g_strdup(text));
    gint y = 0, m = 0, d = 0;
    gboolean ok = FALSE;
    if (sscanf(t, "%d-%d-%d", &y, &m, &d) == 3) {
        ok = TRUE;
    } else if (sscanf(t, "%d/%d/%d", &m, &d, &y) == 3) {
        if (y < 100)
            y += 2000;
        ok = TRUE;
    }
    g_free(t);
    return ok ? task_due_from_ymd(y, m, d) : 0;
}
