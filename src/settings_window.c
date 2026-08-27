/* ===========================================================================
 * settings_window.c — the Tasks settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "db.h"
#include "plugin_loader.h"
#include "backup.h"
#include "library_window.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * TaskSettings — the singleton window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp     *app;
    gchar     *db_path;
    GtkWidget *window;
    gboolean   loading;              /* suppress write-through on load      */
} TaskSettings;

static TaskSettings *settings = NULL;  /* the singleton, or NULL            */

#define SETTINGS_WIDTH 470           /* window width AND the width the      */
                                     /* column's height is measured at      */

/* on_bold_titles_toggled() — Appearance: bold task titles on/off,
 * applied live (the task pane re-renders its markup).                      */
static void
on_due_today_overdue_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("due_today_show_overdue", on ? "1" : "0");
    task_app_notify_changed(sw->app);
}

static void
on_bold_titles_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean bold = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("bold_task_titles", bold ? "1" : "0");
    task_app_notify_changed(sw->app);
}

/* on_toolbar_style_changed() — the Appearance combo: apply the chosen
 * toolbar style live (icons / text below icons / text only).               */
static void
on_toolbar_style_changed(GtkComboBox *combo, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    static const GtkToolbarStyle STYLES[] = {
        GTK_TOOLBAR_ICONS, GTK_TOOLBAR_BOTH, GTK_TOOLBAR_TEXT
    };
    gint active = gtk_combo_box_get_active(combo);
    if (active >= 0 && active < (gint)G_N_ELEMENTS(STYLES))
        task_app_set_toolbar_style(sw->app, STYLES[active]);
}

#ifdef HAVE_GTKOSX
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live, and persist the choice.                 */
static void
on_native_menubar_toggled(GtkToggleButton *check, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean native = gtk_toggle_button_get_active(check);
    task_app_config_set("native_menubar", native ? "1" : "0");
    task_library_apply_native_menubar(sw->app, native);
}
#endif /* HAVE_GTKOSX */

/* on_integrity_check_toggled() — persist the db_integrity_check pref and
 * update the in-memory flag so the setting takes effect next launch.       */
static void
on_integrity_check_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("db_integrity_check", on ? "1" : "0");
    sw->app->db_integrity_check = on;
}

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the Database settings block, kept alive so
 * handlers can update them after a location switch.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp     *app;
    GtkWidget *check;                /* "custom folder" checkbox            */
    GtkWidget *choose_btn;           /* "Choose Folder…" (sensitive = custom)*/
    GtkWidget *path_label;           /* shows the active db file path       */
    /* Rotating backups (backup.h) — off by default.                        */
    GtkWidget *bk_check;             /* master switch                       */
    GtkWidget *bk_choose_btn;        /* destination folder chooser          */
    GtkWidget *bk_path_label;        /* the chosen folder, or a prompt      */
    GtkWidget *bk_interval_spin;     /* minutes; 0 = manual only            */
    GtkWidget *bk_keep_spin;         /* how many to retain                  */
    GtkWidget *bk_now_btn;           /* "Back Up Now"                       */
} DbSection;

/* ---------------------------------------------------------------------------
 * bk_section_refresh() — mirror the backup settings into the widgets and
 * grey out everything the master switch does not apply to.
 *
 * The destination label carries the honest state: an enabled backup with
 * no folder chosen does nothing, and saying so here is what keeps that
 * from looking like a feature that silently failed.
 * ------------------------------------------------------------------------- */
static void
bk_section_refresh(DbSection *s)
{
    gboolean on = task_app_config_get_bool("backup_enabled", FALSE);
    gtk_widget_set_sensitive(s->bk_choose_btn,    on);
    gtk_widget_set_sensitive(s->bk_interval_spin, on);
    gtk_widget_set_sensitive(s->bk_keep_spin,     on);
    gtk_widget_set_sensitive(s->bk_now_btn,       on);

    /* Always the RESOLVED destination, from the same call the worker uses,
     * so the label cannot promise a folder the backups do not go to.  With
     * no folder chosen that is the default database location under the
     * home directory, and the label says which case it is.                */
    gchar *dir      = task_backup_dir();
    gchar *chosen   = task_app_config_get("backup_dir");
    gboolean picked = (chosen != NULL && *chosen != '\0');
    g_free(chosen);
    gchar *db_dir = g_path_get_dirname(s->app->db->path);
    gchar *markup;
    if (g_strcmp0(dir, db_dir) == 0)
        /* Same folder as the live database: still a real backup (a
         * separate, verified file), but it cannot survive losing that
         * folder — say so instead of implying independence.               */
        markup = g_markup_printf_escaped(
            "<small>Backups: %s\n<i>\xe2\x9a\xa0 the same folder as the "
            "database \xe2\x80\x94 choose another to survive losing "
            "it</i></small>", dir);
    else if (picked)
        markup = g_markup_printf_escaped("<small>Backups: %s</small>", dir);
    else
        markup = g_markup_printf_escaped(
            "<small>Backups: %s <i>(default)</i></small>", dir);
    gtk_label_set_markup(GTK_LABEL(s->bk_path_label), markup);
    g_free(markup);
    g_free(db_dir);
    g_free(dir);
}

/* db_section_refresh() — sync the widgets with the current app state.      */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped(
        "<small>Current database: %s</small>", s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);
    gtk_widget_set_sensitive(s->choose_btn, s->app->db_dir != NULL);
}

static void on_db_custom_toggled(GtkToggleButton *check, gpointer user_data);

/* db_switch_report() — run the switch and re-sync widgets with whatever
 * actually happened (a cancelled or failed switch leaves the old db active).*/
static void
db_switch_report(DbSection *s, const gchar *new_dir)
{
    task_app_switch_database(s->app, new_dir);
    g_signal_handlers_block_by_func(s->check, on_db_custom_toggled, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->check),
                                 s->app->db_dir != NULL);
    g_signal_handlers_unblock_by_func(s->check, on_db_custom_toggled, s);
    db_section_refresh(s);
}

/* bk_pick_folder() — folder chooser for the BACKUP destination.  Starts at
 * the current choice when there is one.  Returns a new path, or NULL.      */
static gchar *
bk_pick_folder(DbSection *s)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Backup Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(s->bk_check)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    gchar *cur = task_app_config_get("backup_dir");
    if (cur != NULL && *cur != '\0')
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), cur);
    g_free(cur);
    gchar *dir = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return dir;
}

/* on_bk_toggled() — the backup master switch: persist, re-arm the timer,
 * and prompt for a folder the first time it is switched on with none set
 * (enabling a backup that cannot run is not a useful state to leave in).   */
static void
on_bk_toggled(GtkToggleButton *check, gpointer user_data)
{
    DbSection *s = user_data;
    gboolean on = gtk_toggle_button_get_active(check);
    task_app_config_set("backup_enabled", on ? "1" : "0");
    /* No folder prompt: task_backup_dir falls back to the default database
     * location, so switching this on always does something.  Choosing a
     * folder is an improvement, not a prerequisite.                       */
    task_backup_auto_start(s->app, s->app->db->path);
    bk_section_refresh(s);
}

/* on_bk_choose_clicked() — re-pick the destination folder.                 */
static void
on_bk_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    gchar *dir = bk_pick_folder(s);
    if (dir != NULL) {
        task_app_config_set("backup_dir", dir);
        g_free(dir);
        task_backup_auto_start(s->app, s->app->db->path);
        bk_section_refresh(s);
    }
}

/* on_bk_interval_changed() / on_bk_keep_changed() — persist and re-arm.    */
static void
on_bk_interval_changed(GtkSpinButton *spin, gpointer user_data)
{
    DbSection *s = user_data;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(spin));
    task_app_config_set("backup_interval_min", v);
    g_free(v);
    task_backup_auto_start(s->app, s->app->db->path);
}

static void
on_bk_keep_changed(GtkSpinButton *spin, gpointer user_data)
{
    (void)user_data;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(spin));
    task_app_config_set("backup_keep", v);
    g_free(v);
}

/* on_bk_now_clicked() — "Back Up Now": one pass, reported in the status
 * bar.  Also the only way to exercise the feature without waiting for a
 * timer, which is why it is worth a button.                                */
static void
on_bk_now_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    task_backup_start(s->app, s->app->db->path, NULL, NULL);
}

/* db_pick_folder() — run a folder-chooser dialog; returns new path or NULL. */
static gchar *
db_pick_folder(DbSection *s)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Database Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(s->check)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    if (s->app->db_dir != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                            s->app->db_dir);
    gchar *dir = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return dir;
}

/* on_db_custom_toggled() — checkbox toggled: switch to custom or default.  */
static void
on_db_custom_toggled(GtkToggleButton *check, gpointer user_data)
{
    DbSection *s = user_data;
    gboolean want_custom = gtk_toggle_button_get_active(check);

    if (want_custom && s->app->db_dir == NULL) {
        gchar *dir = db_pick_folder(s);
        if (dir == NULL) {
            g_signal_handlers_block_by_func(check, on_db_custom_toggled, s);
            gtk_toggle_button_set_active(check, FALSE);
            g_signal_handlers_unblock_by_func(check, on_db_custom_toggled, s);
            return;
        }
        db_switch_report(s, dir);
        g_free(dir);
    } else if (!want_custom && s->app->db_dir != NULL) {
        db_switch_report(s, NULL);   /* back to the default location        */
    }
}

/* on_db_choose_clicked() — "Choose Folder…" button: re-pick the folder.    */
static void
on_db_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    gchar *dir = db_pick_folder(s);
    if (dir != NULL) {
        db_switch_report(s, dir);
        g_free(dir);
    }
}

/* on_settings_destroy() — clear the singleton.                             */
static void
on_settings_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (settings == sw)
        settings = NULL;
    g_free(sw->db_path);
    g_free(sw);
}

/* section_label() — a bold section heading, left-aligned.                  */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

/* ---------------------------------------------------------------------------
 * Contributed sections (see settings_window.h).  Registered once at
 * startup; never removed.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskSettingsSectionFn fn;
    gpointer              user_data;
} Section;

static GSList *sections = NULL;      /* Section*, registration order        */

void
task_settings_add_section(TaskSettingsSectionFn fn, gpointer user_data)
{
    if (fn == NULL)
        return;
    Section *s = g_new0(Section, 1);
    s->fn        = fn;
    s->user_data = user_data;
    sections = g_slist_append(sections, s);
}

/* wrapped_label() — a wrapping, left-aligned explanatory label.            */
static GtkWidget *
wrapped_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

/* The same two, exported so a contributed section looks built-in.        */
GtkWidget *
task_settings_section_heading(const gchar *text)
{
    return section_label(text);
}

GtkWidget *
task_settings_section_note(const gchar *text)
{
    return wrapped_label(text);
}

/* ===========================================================================
 * The Plugins section.
 *
 * Built through task_settings_add_section() like any contributed one —
 * if the app's own section needed a shortcut, the registry would not be
 * good enough for a plugin's.
 * =========================================================================== */

/* on_plugin_toggled() — write the enabled setting and say what it means.
 *
 * The change takes effect at the NEXT START, and the message says so
 * rather than pretending otherwise: a plugin may have registered a
 * GType, a CSS provider or an icon-theme path, and none of those can be
 * undone, so unloading one in place is not something this app can
 * honestly offer.                                                        */
static void
on_plugin_toggled(GtkWidget *check, gpointer data)
{
    const gchar *id = data;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));
    task_plugins_set_enabled(id, on);

    TaskApp *app = g_object_get_data(G_OBJECT(check), "task-app");
    task_app_status(app, "%s will be %s the next time Tasks starts",
                    id, on ? "loaded" : "left unloaded");
}

/* on_readme_link() — open a plugin's README in whatever the desktop uses
 * for it.
 *
 * GTK's default handler for an <a href> in a label already calls
 * gtk_show_uri, so this exists for the FAILURE case: on a desktop with
 * no handler registered for Markdown, the default silently does nothing
 * and the click reads as a broken link.  Saying so on the status bar is
 * the difference between "no handler for .md" and "this app is buggy".
 *
 * Returning TRUE claims the signal so GTK does not then try again.       */
static gboolean
on_readme_link(GtkWidget *label, const gchar *uri, gpointer data)
{
    TaskApp *app = data;
    GError *err = NULL;
    if (!gtk_show_uri_on_window(
            GTK_WINDOW(gtk_widget_get_toplevel(label)), uri,
            GDK_CURRENT_TIME, &err)) {
        task_app_status(app, "Could not open the README: %s",
                        err != NULL ? err->message : "no application "
                        "is set up to open Markdown files");
        g_clear_error(&err);
    }
    return TRUE;
}

/* on_plugin_dir_choose() — pick the folder plugins are loaded from.
 *
 * The label is NOT updated afterwards: it reports where THIS run actually
 * looked, and that does not change until a restart.  Rewriting it to the
 * new folder would claim the running plugins came from somewhere they
 * did not.                                                               */
static void
on_plugin_dir_choose(GtkWidget *btn, gpointer data)
{
    TaskApp *app = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Plugin Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(btn)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                        task_plugins_dir());
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (dir != NULL) {
            task_plugins_set_dir(dir);
            task_app_status(app, "Plugins will load from %s the next time "
                            "Tasks starts", dir);
            g_free(dir);
        }
    }
    gtk_widget_destroy(chooser);
}

/* on_plugin_dir_default() — go back to the standard location.            */
static void
on_plugin_dir_default(GtkWidget *btn, gpointer data)
{
    (void)btn;
    TaskApp *app = data;
    task_plugins_set_dir(NULL);
    task_app_status(app, "Plugins will load from the default folder the "
                    "next time Tasks starts");
}

/* plugins_section() — the list of every plugin FOUND, running or not.    */
static void
plugins_section(TaskApp *app, GtkWidget *vbox, GtkWindow *window,
                gpointer user_data)
{
    (void)window;
    (void)user_data;

    gtk_box_pack_start(GTK_BOX(vbox), section_label("Plugins"),
                       FALSE, FALSE, 0);

    /* Where they come from and how to add one.  Shown even when none are
     * installed — that is precisely when someone needs to be told where
     * to put the first.                                                  */
    gchar *where = g_strdup_printf(
        "Plugins are loaded from\n%s\n\nTo add one, copy it into that "
        "folder and restart Tasks.", task_plugins_dir());
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(where),
                       FALSE, FALSE, 0);
    g_free(where);

    GtkWidget *dir_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *choose  = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    g_signal_connect(choose, "clicked",
                     G_CALLBACK(on_plugin_dir_choose), app);
    gtk_box_pack_start(GTK_BOX(dir_row), choose, FALSE, FALSE, 0);

    GtkWidget *reset = gtk_button_new_with_label("Use Default Folder");
    gtk_widget_set_tooltip_text(reset,
        "The plugins folder beside the database, in your home directory");
    g_signal_connect(reset, "clicked",
                     G_CALLBACK(on_plugin_dir_default), app);
    gtk_box_pack_start(GTK_BOX(dir_row), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), dir_row, FALSE, FALSE, 0);

    guint n = task_plugins_available();
    if (n == 0) {
        gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
            "No plugins are installed."), FALSE, FALSE, 0);
        return;
    }

    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Enabling or Disabling a plugin requires a restart of the "
        "application."), FALSE, FALSE, 0);

    for (guint i = 0; i < n; i++) {
        const TaskPluginInfo *pi = task_plugins_info(i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget *check = gtk_check_button_new_with_label(
            pi->name != NULL ? pi->name : pi->id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), pi->enabled);
        g_object_set_data(G_OBJECT(check), "task-app", app);
        /* The id string belongs to the loader and outlives this window,
         * so the handler can borrow it.                                  */
        g_signal_connect(check, "toggled",
                         G_CALLBACK(on_plugin_toggled), (gpointer)pi->id);
        gtk_box_pack_start(GTK_BOX(row), check, FALSE, FALSE, 0);

        /* One dimmed line under the name: what it does, its version, and
         * — the part that matters — why it is not running when it should
         * be.  A plugin that failed silently is a plugin that looks
         * broken for no reason.                                          */
        GString *sub = g_string_new(NULL);
        if (pi->description != NULL)
            g_string_append(sub, pi->description);
        if (pi->version != NULL) {
            if (sub->len > 0)
                g_string_append(sub, "  ");
            g_string_append_printf(sub, "v%s", pi->version);
        }
        if (pi->problem != NULL) {
            if (sub->len > 0)
                g_string_append(sub, "  \xe2\x80\x94  ");
            g_string_append_printf(sub, "Not loaded: %s", pi->problem);
        } else if (pi->enabled && !pi->loaded) {
            if (sub->len > 0)
                g_string_append(sub, "  \xe2\x80\x94  ");
            g_string_append(sub, "Will load on restart");
        }
        if (sub->len > 0 || pi->readme != NULL) {
            /* The description is DB- and plugin-sourced text going into
             * Pango markup, so it is escaped; the link is ours to build.
             * A bad byte or a stray "&" in a plugin's description would
             * otherwise make pango_parse_markup reject the whole label
             * and the row would draw blank.                              */
            gchar *esc = g_markup_escape_text(sub->str, -1);
            GString *m = g_string_new("<small><span alpha=\"65%\">");
            g_string_append(m, esc);
            if (pi->readme != NULL) {
                gchar *uri = g_filename_to_uri(pi->readme, NULL, NULL);
                if (uri != NULL) {
                    gchar *uesc = g_markup_escape_text(uri, -1);
                    if (sub->len > 0)
                        g_string_append(m, "  \xe2\x80\x94  ");
                    g_string_append_printf(m, "<a href=\"%s\">README</a>",
                                           uesc);
                    g_free(uesc);
                    g_free(uri);
                }
            }
            g_string_append(m, "</span></small>");

            GtkWidget *lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(lbl), m->str);
            gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_widget_set_margin_start(lbl, 24);
            g_signal_connect(lbl, "activate-link",
                             G_CALLBACK(on_readme_link), app);
            gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
            g_string_free(m, TRUE);
            g_free(esc);
        }
        g_string_free(sub, TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    }
}

/* task_settings_init() — register the app's own contributed sections.    */
void
task_settings_init(void)
{
    task_settings_add_section(plugins_section, NULL);
}

/* ---------------------------------------------------------------------------
 * settings_height_cap() — the tallest the settings column may open, in
 * pixels: the work area of the monitor the parent window is on, less room
 * for the titlebar and the dock/panel.  Falls back to a conservative
 * 900-px screen when the parent is not realized yet (no GdkWindow, so no
 * monitor to ask).
 * ------------------------------------------------------------------------- */
static gint
settings_height_cap(GtkWindow *parent)
{
    GdkRectangle area = { 0, 0, 0, 900 };     /* fallback screen height   */
    GdkWindow   *ref  = parent != NULL
                        ? gtk_widget_get_window(GTK_WIDGET(parent)) : NULL;
    if (ref != NULL) {
        GdkMonitor *mon = gdk_display_get_monitor_at_window(
                              gdk_window_get_display(ref), ref);
        if (mon != NULL)
            gdk_monitor_get_workarea(mon, &area);
    }
    return MAX(320, area.height - 140);
}

/* ---------------------------------------------------------------------------
 * settings_scroller_new() — wrap the settings column in a vertical
 * scroller.  Both of the window's size problems come from the column
 * having been the window's DIRECT child: a plain GtkBox propagates its
 * whole content height as the toplevel's MINIMUM height, and GTK refuses
 * to size a window below its minimum — so the window could only ever be
 * grown, which reads as "it can't be resized" — and a column taller than
 * the screen ran off the bottom with no way to reach the last section.
 *
 * `propagate_natural_height` keeps the "opens at exactly the height it
 * needs" behaviour the -1 default size asks for; `max_content_height`
 * caps that at the monitor's work area, so a short screen opens scrolled
 * instead of oversized; `min_content_height` is what makes shrinking
 * possible at all.  Horizontal policy is NEVER — the column wraps its own
 * explanatory labels, so it must never scroll sideways.
 *
 * The child carries a SETTINGS_WIDTH width request because of those
 * wrapping labels: with NEVER, the scroller measures its natural height
 * for the child's MINIMUM width, and a label wrapped that narrow is
 * several lines taller than the same label at 470 px — the window would
 * open with a band of empty space under the last section.  Requesting the
 * real width makes the measurement match what is drawn.
 * ------------------------------------------------------------------------- */
static GtkWidget *
settings_scroller_new(GtkWidget *child, GtkWindow *parent)
{
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(child, SETTINGS_WIDTH, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(sc), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc),
                                               240);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc),
                                               settings_height_cap(parent));
    gtk_container_add(GTK_CONTAINER(sc), child);
    return sc;
}

/* ---------------------------------------------------------------------------
 * task_settings_window_open() — show (or raise) the window (see header).
 * ------------------------------------------------------------------------- */
void
task_settings_window_open(TaskApp *app, GtkWindow *parent,
                          const gchar *db_path)
{
    if (settings != NULL) {
        gtk_window_present(GTK_WINDOW(settings->window));
        return;
    }
    TaskSettings *sw = g_new0(TaskSettings, 1);
    settings = sw;
    sw->app = app;
    sw->db_path = g_strdup(db_path);
    sw->loading = TRUE;

    sw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sw->window), "Tasks - Settings");
    gtk_window_set_transient_for(GTK_WINDOW(sw->window), parent);
    gtk_window_set_default_size(GTK_WINDOW(sw->window),
                                SETTINGS_WIDTH, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(sw->window),
                      settings_scroller_new(vbox, parent));

    /* --- Appearance --------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Appearance"),
                       FALSE, FALSE, 0);

    GtkWidget *bold_check = gtk_check_button_new_with_label(
        "Show task titles in bold");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bold_check),
        task_app_config_get_bool("bold_task_titles", FALSE));
    g_signal_connect(bold_check, "toggled",
                     G_CALLBACK(on_bold_titles_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), bold_check, FALSE, FALSE, 0);

    GtkWidget *overdue_check = gtk_check_button_new_with_label(
        "Include all past-due tasks in the Due Today view");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(overdue_check),
        task_app_config_get_bool("due_today_show_overdue", FALSE));
    g_signal_connect(overdue_check, "toggled",
                     G_CALLBACK(on_due_today_overdue_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), overdue_check, FALSE, FALSE, 0);

    /* Toolbar style: icons / text below icons / text only.  Applies live
     * to every registered toolbar; also reachable by right-clicking any
     * toolbar (like Notes).                                               */
    GtkWidget *style_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(style_row),
                       gtk_label_new("Toolbar style:"), FALSE, FALSE, 0);
    GtkWidget *style_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Text below icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Text only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(style_combo),
        app->toolbar_style == GTK_TOOLBAR_BOTH ? 1
        : app->toolbar_style == GTK_TOOLBAR_TEXT ? 2 : 0);
    g_signal_connect(style_combo, "changed",
                     G_CALLBACK(on_toolbar_style_changed), sw);
    gtk_box_pack_start(GTK_BOX(style_row), style_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), style_row, FALSE, FALSE, 0);

#ifdef __APPLE__
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
#ifdef HAVE_GTKOSX
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(mac_check),
        task_app_config_get_bool("native_menubar", FALSE));
    g_signal_connect(mac_check, "toggled",
                     G_CALLBACK(on_native_menubar_toggled), sw);
#else
    gtk_widget_set_sensitive(mac_check, FALSE);
    gtk_widget_set_tooltip_text(mac_check,
        "Requires the gtk-mac-integration library:\n"
        "sudo port install gtk-osx-application-gtk3, then rebuild "
        "(make clean && make)");
#endif
    gtk_box_pack_start(GTK_BOX(vbox), mac_check, FALSE, FALSE, 0);
#endif /* __APPLE__ */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Database ---------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Database"),
                       FALSE, FALSE, 0);

    DbSection *dbs = g_new0(DbSection, 1);
    dbs->app = app;
    g_object_set_data_full(G_OBJECT(sw->window), "task-db-section",
                           dbs, g_free);

    dbs->check = gtk_check_button_new_with_label(
        "Store the database in a custom folder (e.g. a shared drive)");
    gtk_widget_set_margin_start(dbs->check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dbs->check),
                                 app->db_dir != NULL);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->check, FALSE, FALSE, 0);

    GtkWidget *db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(db_row, 12);
    dbs->choose_btn = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(db_row), dbs->choose_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), db_row, FALSE, FALSE, 0);

    dbs->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->path_label), 40);
    gtk_widget_set_margin_start(dbs->path_label, 12);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->path_label, FALSE, FALSE, 0);

    db_section_refresh(dbs);
    g_signal_connect(dbs->check, "toggled",
                     G_CALLBACK(on_db_custom_toggled), dbs);
    g_signal_connect(dbs->choose_btn, "clicked",
                     G_CALLBACK(on_db_choose_clicked), dbs);

    /* --- Rotating backups (off by default) -------------------------------- */
    dbs->bk_check = gtk_check_button_new_with_label(
        "Back up the database automatically");
    gtk_widget_set_margin_start(dbs->bk_check, 12);
    gtk_widget_set_margin_top(dbs->bk_check, 6);
    gtk_widget_set_tooltip_text(dbs->bk_check,
        "Writes a verified copy of the database into a folder of your "
        "choice on a timer, keeping only the most recent few.  Worth "
        "pointing at a disk INDEPENDENT of wherever the database itself "
        "lives, so one mishap cannot take both.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dbs->bk_check),
        task_app_config_get_bool("backup_enabled", FALSE));
    gtk_box_pack_start(GTK_BOX(vbox), dbs->bk_check, FALSE, FALSE, 0);

    GtkWidget *bk_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_row, 12);
    dbs->bk_choose_btn = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(bk_row), dbs->bk_choose_btn,
                       FALSE, FALSE, 0);
    dbs->bk_now_btn = gtk_button_new_with_label("Back Up Now");
    gtk_box_pack_start(GTK_BOX(bk_row), dbs->bk_now_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bk_row, FALSE, FALSE, 0);

    dbs->bk_path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->bk_path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->bk_path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->bk_path_label), 40);
    gtk_widget_set_margin_start(dbs->bk_path_label, 12);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->bk_path_label, FALSE, FALSE, 0);

    /* Interval and retention.  The retention cap is the "don't fill the
     * disk" guarantee, so it is a spin button with a hard floor of 1 —
     * a rotation that keeps nothing is not a rotation.                     */
    GtkWidget *bk_opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_opts, 12);
    gtk_box_pack_start(GTK_BOX(bk_opts), gtk_label_new("Every"),
                       FALSE, FALSE, 0);
    dbs->bk_interval_spin = gtk_spin_button_new_with_range(0, 10080, 15);
    gtk_widget_set_tooltip_text(dbs->bk_interval_spin,
        "Minutes between backups.  0 backs up only when you press "
        "Back Up Now.  A pass whose database has not changed since the "
        "last backup writes nothing.");
    gchar *bkiv = task_app_config_get("backup_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_interval_spin),
        bkiv != NULL ? atoi(bkiv) : TASK_BACKUP_INTERVAL_DEFAULT);
    g_free(bkiv);
    gtk_box_pack_start(GTK_BOX(bk_opts), dbs->bk_interval_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bk_opts), gtk_label_new("minutes, keeping"),
                       FALSE, FALSE, 0);
    dbs->bk_keep_spin = gtk_spin_button_new_with_range(1, 500, 1);
    gtk_widget_set_tooltip_text(dbs->bk_keep_spin,
        "How many backup files to retain.  The oldest are removed once a "
        "NEW backup has been verified, never before.");
    gchar *bkkeep = task_app_config_get("backup_keep");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_keep_spin),
        bkkeep != NULL ? atoi(bkkeep) : TASK_BACKUP_KEEP_DEFAULT);
    g_free(bkkeep);
    gtk_box_pack_start(GTK_BOX(bk_opts), dbs->bk_keep_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bk_opts), gtk_label_new("files"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bk_opts, FALSE, FALSE, 0);

    bk_section_refresh(dbs);
    g_signal_connect(dbs->bk_check, "toggled",
                     G_CALLBACK(on_bk_toggled), dbs);
    g_signal_connect(dbs->bk_choose_btn, "clicked",
                     G_CALLBACK(on_bk_choose_clicked), dbs);
    g_signal_connect(dbs->bk_interval_spin, "value-changed",
                     G_CALLBACK(on_bk_interval_changed), dbs);
    g_signal_connect(dbs->bk_keep_spin, "value-changed",
                     G_CALLBACK(on_bk_keep_changed), dbs);
    g_signal_connect(dbs->bk_now_btn, "clicked",
                     G_CALLBACK(on_bk_now_clicked), dbs);

    GtkWidget *integrity_check = gtk_check_button_new_with_label(
        "Check database integrity on startup (PRAGMA integrity_check)");
    gtk_widget_set_margin_start(integrity_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(integrity_check),
        task_app_config_get_bool("db_integrity_check", TRUE));
    g_signal_connect(integrity_check, "toggled",
                     G_CALLBACK(on_integrity_check_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), integrity_check, FALSE, FALSE, 0);

    /* --- Contributed sections ----------------------------------------------- */
    /* After the app's own, in registration order.  The rule is packed by
     * the LOOP, not before it: each section then gets exactly one, and
     * none dangles under the last built-in section when nothing has
     * registered.                                                        */
    for (GSList *n = sections; n != NULL; n = n->next) {
        Section *sec = n->data;
        gtk_box_pack_start(GTK_BOX(vbox),
                           gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                           FALSE, FALSE, 2);
        sec->fn(app, vbox, GTK_WINDOW(sw->window), sec->user_data);
    }

    sw->loading = FALSE;

    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_settings_destroy), sw);
    gtk_widget_show_all(sw->window);
}
