/* ===========================================================================
 * settings_window.c — the Tasks settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "db.h"
#include "oauth.h"
#include "gtasks.h"
#include "bnsync.h"
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
    GtkWidget *sync_check;           /* Google Tasks master switch          */
    GtkWidget *sync_toolbar_check;   /* show Sync button in toolbar         */
    GtkWidget *interval_spin;
    GtkWidget *signin_btn;
    GtkWidget *signout_btn;
    GtkWidget *state_label;          /* "Signed in" / "Not signed in"       */
    GtkWidget *bn_check;             /* Notes mirror master switch        */
    GtkWidget *bn_cli_entry;         /* Notes command path                */
    GtkWidget *bn_embed_combo;       /* list mirrored items are filed into  */
    GArray    *bn_embed_ids;         /* combo index → list id (0 = managed) */
    GtkWidget *bn_interval_spin;     /* mirror pass interval, minutes       */
    GtkWidget *bn_meta_check;        /* sidebar Action Items view toggle    */
    gboolean   loading;              /* suppress write-through on load      */
} TaskSettings;

static TaskSettings *settings = NULL;  /* the singleton, or NULL            */

#define SETTINGS_WIDTH 470           /* window width AND the width the      */
                                     /* column's height is measured at      */

/* ---------------------------------------------------------------------------
 * state_refresh() — reflect the master switch + sign-in state: with the
 * switch off, Sign In / Sign Out / the auto-sync interval all grey out.
 * ------------------------------------------------------------------------- */
static void
state_refresh(TaskSettings *sw)
{
    gboolean enabled = task_app_config_get_bool("google_sync_enabled",
                                                TRUE);
    gboolean in = task_oauth_authenticated();
    gtk_label_set_markup(GTK_LABEL(sw->state_label),
        !enabled ? "<span foreground=\"#888888\">Sync disabled</span>"
        : in     ? "<span foreground=\"#26a269\">Signed in</span>"
                 : "<span foreground=\"#888888\">Not signed in</span>");
    gtk_widget_set_sensitive(sw->signout_btn, enabled && in);
    gtk_widget_set_sensitive(sw->signin_btn,
                             enabled && task_oauth_have_client() && !in);
    gtk_widget_set_sensitive(sw->interval_spin, enabled);
    gtk_widget_set_sensitive(sw->sync_toolbar_check, enabled);
}

/* ---------------------------------------------------------------------------
 * on_sync_enabled_toggled() — the Google Tasks master switch: persist,
 * re-grey the section, and arm/disarm the auto-sync timer.
 * ------------------------------------------------------------------------- */
static void
on_sync_enabled_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("google_sync_enabled", on ? "1" : "0");
    state_refresh(sw);
    task_sync_auto_start(sw->app, sw->app->db->path);
    /* Full notify: the library hides/shows its Sync button with this.      */
    task_app_notify_changed(sw->app);
    task_app_status(sw->app, on ? "Google Tasks sync enabled"
                              : "Google Tasks sync disabled");
}

/* on_interval_changed() — write-through + restart the auto-sync timer.     */
static void
on_interval_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gint minutes = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(sw->interval_spin));
    gchar *v = g_strdup_printf("%d", minutes);
    task_app_config_set("sync_interval_min", v);
    g_free(v);
    task_sync_auto_start(sw->app, sw->app->db->path);
}

/* on_sync_toolbar_toggled() — persist the toolbar-button visibility pref
 * and tell the library window to show or hide its Sync button live.        */
static void
on_sync_toolbar_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("sync_toolbar_button", on ? "1" : "0");
    task_app_notify_changed(sw->app);
}

/* signin_done() — completion of the browser flow started here.             */
static void
signin_done(gboolean ok, const gchar *error, gpointer data)
{
    TaskSettings *sw = data;
    if (settings != sw)              /* window closed mid-flow              */
        return;
    state_refresh(sw);
    if (ok)
        task_app_status(sw->app, "Signed in to Google");
    task_sync_signin_done(sw->app, GTK_WINDOW(sw->window), sw->db_path,
                          ok, error, NULL);
}

/* on_signin() — the Sign In button.                                        */
static void
on_signin(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    task_app_status(sw->app, "Opening browser for Google sign-in\xe2\x80\xa6");
    task_oauth_begin(GTK_WINDOW(sw->window), signin_done, sw);
}

/* on_signout() — the Sign Out button.                                      */
static void
on_signout(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    task_oauth_signout();
    state_refresh(sw);
    task_app_status(sw->app, "Signed out \xe2\x80\x94 the stored sign-in "
                    "was removed and syncing stopped");
}

/* on_bn_toggled() — the Notes enable checkbox: persist, then re-arm
 * the mirror timer (switching ON runs a pass immediately, which is what
 * populates the mirror; switching OFF stops the timer).                    */
static void
on_bn_toggled(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->bn_check));
    task_app_config_set("notes_sync", on ? "1" : "0");
    task_bnsync_auto_start(sw->app, sw->db_path);
    task_app_notify_changed(sw->app);
}

/* on_bn_interval_changed() — write-through + re-arm the mirror timer.      */
static void
on_bn_interval_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(sw->bn_interval_spin)));
    task_app_config_set("notes_sync_interval_min", v);
    g_free(v);
    task_bnsync_auto_start(sw->app, sw->db_path);
}

/* on_bn_meta_toggled() — show/hide the sidebar's Action Items view.        */
static void
on_bn_meta_toggled(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    task_app_config_set("notes_meta_row",
        gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(sw->bn_meta_check)) ? "1" : "0");
    task_app_notify_changed(sw->app);
}

/* on_bn_embed_changed() — which list mirrored action items live in.
 * Persists the choice, MOVES the existing items there (the setting
 * names where they live, not merely where the next one lands), and
 * runs a pass so the change is visible immediately.  A per-task move
 * made by hand still sticks until this setting is touched again.           */
static void
on_bn_embed_changed(GtkComboBox *combo, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gint active = gtk_combo_box_get_active(combo);
    if (active < 0 || active >= (gint)sw->bn_embed_ids->len)
        return;
    gint64 id = g_array_index(sw->bn_embed_ids, gint64, active);
    if (id == 0) {
        task_app_config_set("notes_embed_list", NULL);
    } else {
        gchar *v = g_strdup_printf("%" G_GINT64_FORMAT, id);
        task_app_config_set("notes_embed_list", v);
        g_free(v);
    }
    task_bnsync_reconcile_target(sw->app);
    if (task_app_config_get_bool("notes_sync", FALSE))
        task_bnsync_start(sw->app, sw->db_path, NULL, NULL);
    task_app_notify_changed(sw->app);
}

/* on_bn_cli_changed() — the CLI path entry: persist ONLY.  The mirror
 * pass happens on commit (focus-out/Enter) — running it per keystroke
 * would spawn the half-typed command over and over.                        */
static void
on_bn_cli_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    const gchar *cli = gtk_entry_get_text(GTK_ENTRY(sw->bn_cli_entry));
    task_app_config_set("notes_cli", *cli != '\0' ? cli : NULL);
}

/* on_bn_cli_commit() — Enter in the CLI path entry: run a mirror pass
 * against the newly named binary so a wrong path reports itself now
 * rather than at the next tick.                                            */
static void
on_bn_cli_commit(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    if (task_app_config_get_bool("notes_sync", FALSE))
        task_bnsync_start(sw->app, sw->db_path, NULL, NULL);
    task_app_notify_changed(sw->app);
}

/* on_bn_cli_focus_out() — leaving the CLI path entry: commit now.          */
static gboolean
on_bn_cli_focus_out(GtkWidget *w, GdkEventFocus *event, gpointer data)
{
    (void)event;
    on_bn_cli_commit(w, data);
    return FALSE;                    /* propagate                           */
}

/* on_forecast_toggled() — Appearance: show or hide the sidebar's Weekly
 * Forecast view.  The full notify rebuilds the sidebar; a hidden view
 * that was selected falls back to the first list there.                    */
static void
on_forecast_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("weekly_forecast", on ? "1" : "0");
    task_app_notify_changed(sw->app);
}

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
    if (sw->bn_embed_ids != NULL)
        g_array_free(sw->bn_embed_ids, TRUE);
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

    GtkWidget *forecast_check = gtk_check_button_new_with_label(
        "Show the Weekly Forecast view (this week, day by day)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(forecast_check),
        task_app_config_get_bool("weekly_forecast", TRUE));
    g_signal_connect(forecast_check, "toggled",
                     G_CALLBACK(on_forecast_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), forecast_check, FALSE, FALSE, 0);

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

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Notes ------------------------------------------------------------ */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Notes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Mirror the action items from Notes as ordinary tasks, with "
        "their own notes, subtasks and attachments. Ticking one off or "
        "changing its due date is sent back to Notes on the interval "
        "below; the item's text belongs to the note it lives in, so "
        "edit that in Notes."),
        FALSE, FALSE, 0);
    sw->bn_check = gtk_check_button_new_with_label(
        "Mirror Notes action items");
    g_signal_connect(sw->bn_check, "toggled",
                     G_CALLBACK(on_bn_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->bn_check, FALSE, FALSE, 0);

    GtkWidget *bn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bn_row),
                       gtk_label_new("Notes command:"),
                       FALSE, FALSE, 0);
    sw->bn_cli_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->bn_cli_entry),
                                   "notes (searched on PATH)");
    gtk_widget_set_hexpand(sw->bn_cli_entry, TRUE);
    g_signal_connect(sw->bn_cli_entry, "changed",
                     G_CALLBACK(on_bn_cli_changed), sw);
    g_signal_connect(sw->bn_cli_entry, "activate",
                     G_CALLBACK(on_bn_cli_commit), sw);
    g_signal_connect(sw->bn_cli_entry, "focus-out-event",
                     G_CALLBACK(on_bn_cli_focus_out), sw);
    gtk_box_pack_start(GTK_BOX(bn_row), sw->bn_cli_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bn_row, FALSE, FALSE, 0);

    /* Which real list the mirrored tasks are filed into.  0 = let the
     * mirror manage its own "Action Items" list, created on first use.     */
    GtkWidget *embed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(embed_row),
                       gtk_label_new("Mirror action items into:"),
                       FALSE, FALSE, 0);
    sw->bn_embed_combo = gtk_combo_box_text_new();
    sw->bn_embed_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    gint64 own = 0;
    g_array_append_val(sw->bn_embed_ids, own);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sw->bn_embed_combo),
                                   "Action Items (managed list)");
    gchar *embed_v = task_app_config_get("notes_embed_list");
    gint64 embed_id = embed_v != NULL
                      ? g_ascii_strtoll(embed_v, NULL, 10) : 0;
    g_free(embed_v);
    gint embed_active = 0;           /* combo index to preselect            */
    GPtrArray *lists = task_db_lists(app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        gchar *label = *l->emoji != '\0'
            ? g_strdup_printf("%s  %s", l->emoji, l->name)
            : g_strdup(l->name);
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(sw->bn_embed_combo), label);
        g_free(label);
        g_array_append_val(sw->bn_embed_ids, l->id);
        if (l->id == embed_id)
            embed_active = (gint)sw->bn_embed_ids->len - 1;
    }
    task_ptr_array_free_lists(lists);
    gtk_combo_box_set_active(GTK_COMBO_BOX(sw->bn_embed_combo),
                             embed_active);
    gtk_widget_set_tooltip_text(sw->bn_embed_combo,
        "Action items live here \xe2\x80\x94 changing this moves the "
        "existing ones too");
    g_signal_connect(sw->bn_embed_combo, "changed",
                     G_CALLBACK(on_bn_embed_changed), sw);
    gtk_box_pack_start(GTK_BOX(embed_row), sw->bn_embed_combo,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), embed_row, FALSE, FALSE, 0);

    /* How often the mirror runs — the same shape as the Google interval
     * (0 = only when Sync is pressed).                                     */
    GtkWidget *bn_iv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bn_iv_row),
                       gtk_label_new("Sync action items every"),
                       FALSE, FALSE, 0);
    sw->bn_interval_spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(sw->bn_interval_spin,
        "0 = only when you press Sync");
    g_signal_connect(sw->bn_interval_spin, "value-changed",
                     G_CALLBACK(on_bn_interval_changed), sw);
    gtk_box_pack_start(GTK_BOX(bn_iv_row), sw->bn_interval_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bn_iv_row), gtk_label_new("minutes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bn_iv_row, FALSE, FALSE, 0);

    sw->bn_meta_check = gtk_check_button_new_with_label(
        "Show the Action Items view in the sidebar");
    gtk_widget_set_tooltip_text(sw->bn_meta_check,
        "Lists every mirrored action item in one place, whichever list "
        "each one lives in");
    g_signal_connect(sw->bn_meta_check, "toggled",
                     G_CALLBACK(on_bn_meta_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->bn_meta_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Google Tasks ------------------------------------------------------ */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Google Tasks"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Two-way non-destructive sync with Google Tasks.  Sign in will "
        "open a browser window for authentication; Sign out will remove "
        "the local stored token."), FALSE, FALSE, 0);

    sw->sync_check = gtk_check_button_new_with_label(
        "Enable Google Tasks sync");
    g_signal_connect(sw->sync_check, "toggled",
                     G_CALLBACK(on_sync_enabled_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->sync_check, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    sw->signin_btn = gtk_button_new_with_label(
        "Sign In to Google\xe2\x80\xa6");
    g_signal_connect(sw->signin_btn, "clicked",
                     G_CALLBACK(on_signin), sw);
    gtk_box_pack_start(GTK_BOX(btn_row), sw->signin_btn, FALSE, FALSE, 0);
    sw->signout_btn = gtk_button_new_with_label("Sign Out");
    g_signal_connect(sw->signout_btn, "clicked",
                     G_CALLBACK(on_signout), sw);
    gtk_box_pack_start(GTK_BOX(btn_row), sw->signout_btn,
                       FALSE, FALSE, 0);
    sw->state_label = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(btn_row), sw->state_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_row, FALSE, FALSE, 0);

    GtkWidget *interval_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(interval_row),
                       gtk_label_new("Auto-sync every"), FALSE, FALSE, 0);
    sw->interval_spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(sw->interval_spin,
        "Minutes between automatic syncs while signed in; 0 disables "
        "the timer (the Sync button always works)");
    g_signal_connect(sw->interval_spin, "value-changed",
                     G_CALLBACK(on_interval_changed), sw);
    gtk_box_pack_start(GTK_BOX(interval_row), sw->interval_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(interval_row),
                       gtk_label_new("minutes (0 = off)"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), interval_row, FALSE, FALSE, 0);

    sw->sync_toolbar_check = gtk_check_button_new_with_label(
        "Show Sync button in toolbar");
    g_signal_connect(sw->sync_toolbar_check, "toggled",
                     G_CALLBACK(on_sync_toolbar_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->sync_toolbar_check,
                       FALSE, FALSE, 0);

    /* --- Load current values ------------------------------------------------ */
    gchar *iv  = task_app_config_get("sync_interval_min");
    gchar *bnc = task_app_config_get("notes_cli");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->sync_check),
        task_app_config_get_bool("google_sync_enabled", TRUE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->sync_toolbar_check),
        task_app_config_get_bool("sync_toolbar_button", TRUE));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->interval_spin),
                              iv != NULL ? g_ascii_strtod(iv, NULL) : 5);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->bn_check),
        task_app_config_get_bool("notes_sync", FALSE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->bn_meta_check),
        task_app_config_get_bool("notes_meta_row", TRUE));
    gchar *bniv = task_app_config_get("notes_sync_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->bn_interval_spin),
                              bniv != NULL ? g_ascii_strtod(bniv, NULL) : 5);
    g_free(bniv);
    if (bnc != NULL)
        gtk_entry_set_text(GTK_ENTRY(sw->bn_cli_entry), bnc);
    g_free(iv);
    g_free(bnc);
    sw->loading = FALSE;

    state_refresh(sw);
    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_settings_destroy), sw);
    gtk_widget_show_all(sw->window);
}
