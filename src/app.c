/* ===========================================================================
 * app.c — shared application context for Tasks (see app.h)
 * =========================================================================== */

#include "app.h"
#include "db.h"
#include "editor_window.h"
#include "backup.h"
#include "task_worker.h"
#include "plugin_loader.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Change notification (see app.h).  One entry per subscription; `fn` is
 * a TaskAppNotifyFn on the changed/tasks lists and a TaskAppStatusFn on
 * the status list.
 * ------------------------------------------------------------------------- */
typedef struct {
    guint    id;
    gpointer fn;
    gpointer user_data;
} TaskAppListener;

/* listener_add() — append a subscription, returning its id.               */
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

/* unlisten_from() — drop subscription `id` from one list; TRUE if found. */
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

/* fire() — call every listener on `list`.
 *
 * The list is COPIED first because a listener may unsubscribe itself (or
 * another) while it runs — the library window's own refresh can close an
 * editor — and walking the live list would then step through a freed
 * link.  The copy holds borrowed pointers, so an entry unsubscribed
 * earlier in the same fire would be a use-after-free; ids are checked
 * against the live list to skip exactly that.                            */
static void
fire(TaskApp *app, GSList *list, const gchar *message)
{
    GSList *snapshot = g_slist_copy(list);
    for (GSList *n = snapshot; n != NULL; n = n->next) {
        TaskAppListener *l = n->data;
        if (g_slist_find(list, l) == NULL)
            continue;                /* unsubscribed mid-fire              */
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
 * task_app_notify_tasks() — fire the task-pane event, falling back to
 * the full one when nothing listens for it (see app.h).
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

/* dialog_run() — shared core of notice/confirm: run a modal message
 * dialog and return its response.                                          */
static gint
dialog_run(GtkWindow *parent, GtkMessageType type, GtkButtonsType buttons,
           const gchar *title, const gchar *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, type,
        buttons, "%s", msg);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dlg), title);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp;
}

/* ---------------------------------------------------------------------------
 * task_app_notice() — modal OK message dialog.
 * ------------------------------------------------------------------------- */
void
task_app_notice(GtkWindow *parent, GtkMessageType type,
                const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    dialog_run(parent, type, GTK_BUTTONS_OK, title, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * task_app_confirm() — modal Yes/No question; TRUE on Yes.
 * ------------------------------------------------------------------------- */
gboolean
task_app_confirm(GtkWindow *parent, const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gint resp = dialog_run(parent, GTK_MESSAGE_QUESTION,
                           GTK_BUTTONS_YES_NO, title, msg);
    g_free(msg);
    return resp == GTK_RESPONSE_YES;
}

/* ---------------------------------------------------------------------------
 * task_app_widget_add_css() — one-off CSS on a single widget (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_widget_add_css(GtkWidget *widget, const gchar *css_text)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css_text, -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(widget),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* The old copy_file() (a plain g_file_copy) was REMOVED on 2026-08-26.
 * Database copies go through task_db_copy_file (VACUUM INTO) instead: a
 * byte copy of a live SQLite file can capture a torn page, and this
 * database routinely lives in a sync folder where the source can be
 * rewritten mid-read.  If you need to copy the database, use the db.h
 * helper and VERIFY the result with task_db_verify_file.                   */

/* ---------------------------------------------------------------------------
 * task_app_switch_database() — move tasks.db to a new directory (see app.h).
 * ------------------------------------------------------------------------- */
gboolean
task_app_switch_database(TaskApp *app, const gchar *new_dir)
{
    /* Resolve the target file path.                                        */
    gchar *target;
    if (new_dir != NULL) {
        g_mkdir_with_parents(new_dir, 0755);
        target = g_build_filename(new_dir, TASK_DB_FILENAME, NULL);
    } else {
        target = task_db_default_path();
    }
    if (g_strcmp0(target, app->db->path) == 0) {
        g_free(target);
        return TRUE;                   /* already there: nothing to do      */
    }

    /* If a database already exists at the target, ask before touching it.  */
    gboolean overwrite = FALSE;
    if (g_file_test(target, G_FILE_TEST_EXISTS)) {
        GtkWindow *parent = app->library_window != NULL
                            ? GTK_WINDOW(app->library_window) : NULL;
        GtkWidget *dialog = gtk_message_dialog_new(
            parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "That folder already contains a tasks database.\n\n"
            "Use the tasks stored there, or overwrite it with a copy "
            "of your current database?\n"
            "(Overwriting permanently replaces the file at %s.)", target);
        gtk_window_set_title(GTK_WINDOW(dialog),
                             "Tasks - Existing Database");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog),
            "_Cancel",                GTK_RESPONSE_CANCEL,
            "_Use Existing Database", 1,
            "_Overwrite It",          2,
            NULL);
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != 1 && response != 2) {
            g_free(target);
            return FALSE;              /* cancelled: nothing touched        */
        }
        overwrite = (response == 2);
    }

    task_editor_close_all(app);

    gchar *old_path = g_strdup(app->db->path);

    /* ---------------------------------------------------------------------
     * COPY, THEN VERIFY, THEN — and only then — remove the original.
     *
     * This function used to discard copy_file's return value and treat
     * "task_db_open(target) succeeded" as proof the copy was good.  It is
     * not: SQLite opens a malformed file happily and errors only when a
     * damaged page is READ.  A short copy therefore passed both checks and
     * the original was deleted — which is how a 1965-task database was
     * reduced to a 14-page fragment on 2026-08-26.  This database
     * routinely lives in a sync folder, so a partial copy is a REALISTIC
     * outcome, not a theoretical one.
     *
     * The copy itself is now VACUUM INTO on the still-open connection
     * (transactionally consistent, cannot capture a torn page) rather
     * than a byte-for-byte file copy, and the result is checked with
     * integrity_check on its own connection before anything is deleted.
     * ------------------------------------------------------------------- */
    gboolean copied = FALSE;         /* we wrote `target` ourselves         */
    gboolean copy_ok = TRUE;         /* ... and it verified                 */
    gchar   *copy_err = NULL;
    if (g_file_test(old_path, G_FILE_TEST_EXISTS) &&
        (overwrite || !g_file_test(target, G_FILE_TEST_EXISTS))) {
        /* VACUUM INTO refuses to overwrite, so clear the way first — only
         * ever when the user explicitly chose to overwrite.                */
        if (g_file_test(target, G_FILE_TEST_EXISTS))
            g_unlink(target);
        copy_ok = task_db_copy_file(app->db, target, &copy_err);
        copied  = TRUE;
        if (copy_ok) {
            gchar *detail = NULL;
            copy_ok = task_db_verify_file(target, &detail);
            if (!copy_ok) {
                g_free(copy_err);
                copy_err = detail;   /* ownership moves                     */
            } else {
                g_free(detail);
            }
        }
    }

    /* Plugins own tables in this database; tell them before it goes so
     * they can drop anything they cached from it.                         */
    task_plugins_db_closing(app, app->db);
    task_db_close(app->db);
    app->db = NULL;

    GError *gerr = NULL;
    /* A copy that did not verify means the target is NOT the user's data.
     * Leave it where it is for inspection, keep the original, and say so —
     * silently continuing is what turned this into data loss before.       */
    gboolean ok = copy_ok;
    if (ok) {
        app->db = task_db_open(target, &gerr);
        ok = (app->db != NULL);
    }

    if (!ok) {
        if (copied && !copy_ok) {
            g_warning("switch_database: the copy at %s did not verify (%s) "
                      "— keeping %s", target,
                      copy_err != NULL ? copy_err : "?", old_path);
            task_app_notice(app->library_window != NULL
                              ? GTK_WINDOW(app->library_window) : NULL,
                            GTK_MESSAGE_ERROR, NULL,
                            "The database could not be copied to that "
                          "location intact, so nothing was moved.\n\n"
                          "Your database is still where it was:\n%s\n\n"
                          "The failed copy was left at\n%s\n"
                          "for you to inspect or delete.",
                          old_path, target);
        } else {
            g_warning("switch_database: cannot open %s: %s", target,
                      gerr != NULL ? gerr->message : "?");
            task_app_notice(app->library_window != NULL
                              ? GTK_WINDOW(app->library_window) : NULL,
                            GTK_MESSAGE_ERROR, NULL,
                            "Could not open a database at that location.\n"
                          "The previous database is still in use.");
        }
        g_clear_error(&gerr);
        app->db = task_db_open(old_path, &gerr);
        if (app->db == NULL)
            g_critical("switch_database: cannot revert to %s: %s", old_path,
                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
    } else {
        /* Only now is the original expendable: the target exists, was
         * written from a consistent read, and passed integrity_check.      */
        if (g_file_test(old_path, G_FILE_TEST_EXISTS)) {
            GFile *fold = g_file_new_for_path(old_path);
            if (!g_file_delete(fold, NULL, NULL))
                g_warning("switch_database: could not remove %s", old_path);
            g_object_unref(fold);
        }
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        task_app_config_set("db_dir", new_dir);  /* NULL clears the key     */

        /* RE-ARM ALL THREE TIMERS on the new path.  Each one captured the
         * old path when it was installed, and that file has just been
         * deleted — left alone, a worker would open a path that no longer
         * exists and helpfully CREATE an empty database there, then sync
         * against it.  (CLAUDE.md long claimed this happened; it did not
         * until 2026-08-26.)
         *
         * Arm-ALL rather than naming them: naming timers here is exactly
         * how the File → Open Database path came to re-arm two of three.  */
        task_worker_arm_all(app, app->db->path);
    }
    g_free(copy_err);

    g_free(target);
    g_free(old_path);

    /* ONE call covering both outcomes: on success app->db is the new
     * database, on failure it is the reopened original, and either way a
     * plugin has to be given the chance to create its tables in it.
     * Announcing it at each open site instead is how one of them ends up
     * forgotten — the same trap the timer re-arm fell into.               */
    if (app->db != NULL)
        task_plugins_db_open(app, app->db);

    if (ok)
        task_app_notify_changed(app);
    return ok;
}

/* ===========================================================================
 * Toolbar icons + style (see app.h).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_app_init_icons_dir() — icons/ next to the executable (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_init_icons_dir(TaskApp *app)
{
    app->icons_dir = g_build_filename(task_app_exe_dir(), "icons", NULL);
}

/* ---------------------------------------------------------------------------
 * task_app_icon_image_rotated() — HiDPI-sharp GtkImage for a local icon,
 * optionally turned by whole quarter turns (see app.h).  Rasterizes at the
 * display's scale factor: `size` is the LOGICAL size, the backing pixels
 * are size × sf, and the cairo surface's device scale maps between the two
 * (raw pixbufs render 1 buffer-pixel = 1 logical px and blur on Retina —
 * Notes gotcha #5).
 * ------------------------------------------------------------------------- */
GtkWidget *
task_app_icon_image_rotated(TaskApp *app, const gchar *name, gint size,
                            GdkPixbufRotation rotation)
{
    static const gchar *EXTS[] = { "png", "svg" };

    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        GdkPixbuf *pix = NULL;       /* decoded at backing resolution       */
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            pix = gdk_pixbuf_new_from_file_at_size(path, size * sf,
                                                   size * sf, NULL);
        g_free(path);
        if (pix != NULL) {
            /* Rotate BEFORE the surface: gdk_pixbuf_rotate_simple works in
             * whole quarter turns, so a square icon comes back the same
             * size and stays pixel-exact — no resampling, no blur.  A
             * 90-degree turn of a horizontal list icon is a columnar one,
             * which is why the pane button needs only ONE image.        */
            if (rotation != GDK_PIXBUF_ROTATE_NONE) {
                GdkPixbuf *turned = gdk_pixbuf_rotate_simple(pix, rotation);
                if (turned != NULL) {
                    g_object_unref(pix);
                    pix = turned;
                }
            }
            cairo_surface_t *surface =
                gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
            g_object_unref(pix);
            GtkWidget *image = gtk_image_new_from_surface(surface);
            cairo_surface_destroy(surface);
            /* Which file this is.  A surface-backed GtkImage keeps no
             * record of where it came from (and answers NULL to
             * gtk_image_get_pixbuf), so a caller that SWAPS a button's
             * icon by state — the completed, sort and pane toggles all do
             * — has no way to ask what is on screen now.                 */
            g_object_set_data_full(G_OBJECT(image), "task-icon-name",
                                   g_strdup(name), g_free);
            g_object_set_data(G_OBJECT(image), "task-icon-rotation",
                              GINT_TO_POINTER((gint)rotation));
            return image;
        }
    }
    return NULL;
}

/* task_app_icon_image_sized() — the unrotated case (see app.h).            */
GtkWidget *
task_app_icon_image_sized(TaskApp *app, const gchar *name, gint size)
{
    return task_app_icon_image_rotated(app, name, size,
                                       GDK_PIXBUF_ROTATE_NONE);
}

/* ---------------------------------------------------------------------------
 * task_app_tool_item_new() — style-aware toolbar button (see app.h).
 * ------------------------------------------------------------------------- */
GtkToolItem *
task_app_tool_item_new(TaskApp *app, const gchar *icon_name,
                       const gchar *fallback_markup, const gchar *label,
                       const gchar *tooltip)
{
    GtkToolItem *item = gtk_tool_button_new(NULL, NULL);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);

    /* Icon: the local PNG if present, else the fallback markup rendered
     * as a label standing in for the icon.                                 */
    GtkWidget *icon = (icon_name != NULL)
                      ? task_app_icon_image_sized(app, icon_name, 24) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    gtk_widget_show(icon);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), icon);

    gtk_tool_item_set_tooltip_text(item, tooltip);
    gtk_tool_item_set_is_important(item, TRUE);
    return item;
}

/* style_name()/style_from_name() — the persisted spelling of a style.      */
static const gchar *
style_name(GtkToolbarStyle style)
{
    return style == GTK_TOOLBAR_TEXT ? "text"
         : style == GTK_TOOLBAR_BOTH ? "both" : "icons";
}

static GtkToolbarStyle
style_from_name(const gchar *name)
{
    if (g_strcmp0(name, "text") == 0) return GTK_TOOLBAR_TEXT;
    if (g_strcmp0(name, "both") == 0) return GTK_TOOLBAR_BOTH;
    return GTK_TOOLBAR_ICONS;
}

/* task_app_load_toolbar_style() — the persisted style (default icons).     */
void
task_app_load_toolbar_style(TaskApp *app)
{
    gchar *v = task_app_config_get("toolbar_style");
    app->toolbar_style = style_from_name(v);
    g_free(v);
}

/* ---------------------------------------------------------------------------
 * task_app_set_toolbar_style() — apply + persist a style change (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_set_toolbar_style(TaskApp *app, GtkToolbarStyle style)
{
    app->toolbar_style = style;
    task_app_config_set("toolbar_style", style_name(style));
    if (app->toolbars != NULL)
        for (guint i = 0; i < app->toolbars->len; i++)
            gtk_toolbar_set_style(
                GTK_TOOLBAR(g_ptr_array_index(app->toolbars, i)), style);
}

/* toolbar_destroyed() — drop a dying toolbar from the registry.            */
static void
toolbar_destroyed(GtkWidget *toolbar, gpointer data)
{
    TaskApp *app = data;
    if (app->toolbars != NULL)
        g_ptr_array_remove(app->toolbars, toolbar);
}

/* style_menu_toggled() — a radio item in the right-click menu.             */
static void
style_menu_toggled(GtkCheckMenuItem *item, gpointer data)
{
    TaskApp *app = data;
    if (!gtk_check_menu_item_get_active(item))
        return;                      /* ignore the deactivating item        */
    task_app_set_toolbar_style(app, (GtkToolbarStyle)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "task-style")));
}

/* ---------------------------------------------------------------------------
 * toolbar_context_menu() — "popup-context-menu": right-clicking a
 * toolbar offers the icons/both/text radio choices (fires on empty
 * toolbar area only, like Notes).
 * ------------------------------------------------------------------------- */
static gboolean
toolbar_context_menu(GtkToolbar *toolbar, gint x, gint y, gint button,
                     gpointer data)
{
    (void)x; (void)y; (void)button;
    TaskApp *app = data;
    static const struct {
        const gchar    *label;
        GtkToolbarStyle style;
    } CHOICES[] = {
        { "Icons",            GTK_TOOLBAR_ICONS },
        { "Text Below Icons", GTK_TOOLBAR_BOTH  },
        { "Text Only",        GTK_TOOLBAR_TEXT  },
    };
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(toolbar), NULL);
    /* One menu is built per right-click; without this it would stay
     * attached (= alive) until the toolbar dies.  selection-done fires
     * after the chosen item's activate, so destroying there is safe.       */
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    GSList *group = NULL;            /* the radio group                     */
    for (gsize i = 0; i < G_N_ELEMENTS(CHOICES); i++) {
        GtkWidget *item =
            gtk_radio_menu_item_new_with_label(group, CHOICES[i].label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "task-style",
                          GINT_TO_POINTER(CHOICES[i].style));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                       app->toolbar_style ==
                                       CHOICES[i].style);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(style_menu_toggled), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_app_register_toolbar() — track + style a toolbar (see app.h).
 * ------------------------------------------------------------------------- */
void
task_app_register_toolbar(TaskApp *app, GtkWidget *toolbar)
{
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), app->toolbar_style);
    if (app->toolbars != NULL)
        g_ptr_array_add(app->toolbars, toolbar);
    g_signal_connect(toolbar, "destroy",
                     G_CALLBACK(toolbar_destroyed), app);
    g_signal_connect(toolbar, "popup-context-menu",
                     G_CALLBACK(toolbar_context_menu), app);
}

/* ===========================================================================
 * Config — ini next to the binary, ~/.config fallback (see app.h).
 * =========================================================================== */

#define TASK_INI_GROUP "tasks"

static GKeyFile *config_kf   = NULL; /* the in-memory config                */
static gchar    *config_path = NULL; /* written through on every change     */
static gchar    *exe_dir_cached = NULL;  /* binary's directory (owned)      */

/* exe_dir_from_argv0() — the directory holding the binary (new string).    */
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

/* task_app_exe_dir() — see app.h.                                          */
const gchar *
task_app_exe_dir(void)
{
    return exe_dir_cached;
}

/* The ini's names.                                                         */
#define TASK_INI_FILE     "tasks.ini"
#define TASK_INI_DEFAULTS "tasks.ini.defaults"

/* ---------------------------------------------------------------------------
 * task_app_config_init() — resolve + load the config file once.  Portable
 * mode: tasks.ini next to the binary; when none exists there AND the
 * directory is unwritable, ~/.config/tasks/tasks.ini.  On first run it is
 * seeded from tasks.ini.defaults next to the binary.
 * ------------------------------------------------------------------------- */
void
task_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;

    gchar *exe_dir = exe_dir_from_argv0(argv0);
    exe_dir_cached = g_strdup(exe_dir);
    gchar *local = g_build_filename(exe_dir, TASK_INI_FILE, NULL);
    if (g_file_test(local, G_FILE_TEST_EXISTS) ||
        g_access(exe_dir, W_OK) == 0) {
        config_path = local;         /* portable mode                       */
    } else {
        g_free(local);
        gchar *dir = g_build_filename(g_get_user_config_dir(),
                                      TASK_APP_DIR, NULL);
        g_mkdir_with_parents(dir, 0700);
        config_path = g_build_filename(dir, TASK_INI_FILE, NULL);
        g_free(dir);
    }

    config_kf = g_key_file_new();
    if (!g_key_file_load_from_file(config_kf, config_path,
                                   G_KEY_FILE_NONE, NULL)) {
        /* First launch: seed from the committed defaults, if present.      */
        gchar *defaults = g_build_filename(exe_dir, TASK_INI_DEFAULTS, NULL);
        g_key_file_load_from_file(config_kf, defaults,
                                  G_KEY_FILE_NONE, NULL);
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
 * Namespaced config (see app.h) — "<ns>_<key>" against the same store.
 * ------------------------------------------------------------------------- */

/* ns_key() — build the prefixed key.  g_free the result.                  */
static gchar *
ns_key(const gchar *ns, const gchar *key)
{
    return g_strdup_printf("%s_%s", ns, key);
}

gchar *
task_app_config_get_ns(const gchar *ns, const gchar *key)
{
    gchar *k = ns_key(ns, key);
    gchar *v = task_app_config_get(k);
    g_free(k);
    return v;
}

void
task_app_config_set_ns(const gchar *ns, const gchar *key, const gchar *value)
{
    gchar *k = ns_key(ns, key);
    task_app_config_set(k, value);
    g_free(k);
}

gboolean
task_app_config_get_bool_ns(const gchar *ns, const gchar *key, gboolean def)
{
    gchar *k = ns_key(ns, key);
    gboolean b = task_app_config_get_bool(k, def);
    g_free(k);
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
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * task_day_bounds() — local midnight bounds of "today + offset_days".
 * ------------------------------------------------------------------------- */
void
task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi)
{
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *day = g_date_time_add_days(now, offset_days);
    GDateTime *mid = g_date_time_new_local(g_date_time_get_year(day),
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

/* ---------------------------------------------------------------------------
 * task_due_format() — human-readable due date ("" for none).
 * ------------------------------------------------------------------------- */
gchar *
task_due_format(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_date_time_format(dt, "%b %-e, %Y");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * task_due_format_iso() — canonical "YYYY-MM-DD" spelling ("" for none).
 * ------------------------------------------------------------------------- */
gchar *
task_due_format_iso(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_date_time_format(dt, "%Y-%m-%d");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * task_due_color() — urgency tint (see app.h).  Compares calendar DAYS in
 * local time so the colors roll over at midnight.
 * ------------------------------------------------------------------------- */
const gchar *
task_due_color(gint64 due)
{
    if (due == 0)
        return NULL;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(due);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);
    return day < today  ? "#c01c28"          /* overdue: red                */
         : day == today ? "#d19a00"          /* today: gold                 */
                        : "#26a269";         /* ahead: green                */
}

/* ---------------------------------------------------------------------------
 * task_due_from_ymd() — validated calendar fields → local midnight unix.
 * ------------------------------------------------------------------------- */
gint64
task_due_from_ymd(gint y, gint m, gint d)
{
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 9999)
        return 0;
    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
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
    gint y = 0, m = 0, d = 0;        /* parsed calendar fields              */
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
