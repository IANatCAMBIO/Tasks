/* ===========================================================================
 * settings_window.c — the Tasks settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "db.h"
#include "backup.h"
#include "library_window.h"
#include <glib/gstdio.h>          /* g_stat, GStatBuf                    */
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

/* on_kanban_shadow_toggled() — Appearance: the Kanban cards' drop shadow
 * on/off, applied live.  It calls the APPLIER rather than notifying a
 * refresh: the board skips its rebuild while the same cards are showing,
 * so a notify would leave the setting looking inert.                      */
static void
on_kanban_shadow_toggled(GtkWidget *w, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    task_app_config_set("kanban_shadow", on ? "1" : "0");
    task_library_apply_kanban_shadow(sw->app, on);
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

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the Database settings block, kept alive so
 * handlers can update them after a location switch.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp     *app;
    /* The health block: five facts about the file, all refreshed together
     * because they are all answers about the SAME database.               */
    GtkWidget *path_label;           /* shows the active db file path       */
    GtkWidget *data_label;           /* "1735 tasks in 32 lists"            */
    GtkWidget *size_label;           /* on-disk size                        */
    GtkWidget *led_label;            /* the round health indicator          */
    GtkWidget *health_label;         /* the verdict and when it was reached */
    GtkWidget *sha_btn;              /* the digest; click copies it         */
    GtkWidget *update_btn;           /* renew every line on the plate       */
    /* Rotating backups (backup.h) — off by default.                        */
    GtkWidget *bk_check;             /* master switch                       */
    GtkWidget *bk_choose_btn;        /* destination folder chooser          */
    GtkWidget *bk_path_label;        /* the chosen folder, or a prompt      */
    GtkWidget *bk_interval_spin;     /* minutes; 0 = manual only            */
    GtkWidget *bk_keep_spin;         /* how many to retain                  */
    GtkWidget *bk_now_btn;           /* "Back Up Now"                       */
} DbSection;

/* ---------------------------------------------------------------------------
 * dir_shares_fate() — TRUE when `dir` is `other`, or sits INSIDE it.
 *
 * The test behind the backup destination's warning, and it is a
 * containment test rather than a comparison for a reason the default
 * makes plain: backups now land in a `backups/` folder INSIDE the
 * database's own directory, so string equality — which is what this was
 * until 2026-09-09 — would have answered "different folder" for the one
 * arrangement the warning most needs to describe.  A subfolder shares its
 * parent's fate exactly: the incident behind the Data safety rules was a
 * whole directory going to the trash with the app still running, and a
 * child of it would have gone at the same moment.
 *
 * Canonicalised first so "/a/b" and "/a/./b" are one answer, and the
 * separator is required after the prefix so "/a/backups-old" is not read
 * as living inside "/a/backups".  Symlinks are NOT resolved: that needs
 * the paths to exist and would make the label's answer depend on whether
 * a removable disk happens to be plugged in.
 * ------------------------------------------------------------------------- */
static gboolean
dir_shares_fate(const gchar *dir, const gchar *other)
{
    if (dir == NULL || other == NULL)
        return FALSE;
    gchar *a = g_canonicalize_filename(dir, NULL);
    gchar *b = g_canonicalize_filename(other, NULL);
    gboolean same = (g_strcmp0(a, b) == 0);
    if (!same) {
        gchar *pre = g_strconcat(b, G_DIR_SEPARATOR_S, NULL);
        same = g_str_has_prefix(a, pre);
        g_free(pre);
    }
    g_free(a);
    g_free(b);
    return same;
}

/* ---------------------------------------------------------------------------
 * bk_section_refresh() — mirror the backup settings into the widgets and
 * grey out everything the master switch does not apply to.
 *
 * The destination label is ONE SENTENCE, "Backing up to <folder>", and it
 * names the RESOLVED folder in every case — there is no "(default)" or
 * "not chosen yet" variant, because there is no state in which backups go
 * nowhere: task_backup_dir falls back to the database's own directory, so
 * the honest summary is simply where they land.
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
     * so the label cannot promise a folder the backups do not go to.      */
    gchar *dir    = task_backup_dir();
    gchar *db_dir = g_path_get_dirname(s->app->db->path);
    gchar *markup;
    if (dir_shares_fate(dir, db_dir))
        /* The one case worth a second line.  Backups here are still real
         * backups — a separate file, VACUUM INTO'd and verified — but they
         * cannot survive losing the folder, and this is the DEFAULT, so
         * saying nothing would let the arrangement most in need of
         * changing look like the one nobody need think about.             */
        markup = g_markup_printf_escaped(
            "<small>Backing up to %s\n<i>\xe2\x9a\xa0 alongside the "
            "database \xe2\x80\x94 change it to survive losing that "
            "folder</i></small>", dir);
    else
        markup = g_markup_printf_escaped(
            "<small>Backing up to %s</small>", dir);
    gtk_label_set_markup(GTK_LABEL(s->bk_path_label), markup);
    g_free(markup);
    g_free(db_dir);
    g_free(dir);
}

/* ---------------------------------------------------------------------------
 * The health block's three states, said in one place.
 *
 * The LED is GREEN only for a check that ran and passed.  There is no
 * green for "not checked yet": an indicator that goes green before
 * anything has looked is exactly the "checked, all good, when nothing was
 * checked" answer the error discipline exists to prevent.
 * ------------------------------------------------------------------------- */
#define LED_OK      "\xf0\x9f\x9f\xa2"   /* green circle                    */
#define LED_BAD     "\xf0\x9f\x94\xb4"   /* red circle                      */
#define LED_UNKNOWN "\xe2\x9a\xaa"       /* white circle: nothing has run   */

/* SHA_HEAD/SHA_TAIL — how much of the 64-char digest is shown.  The whole
 * thing is on the tooltip and on the clipboard; this is the part that
 * fits beside its label without wrapping the row.                         */
#define SHA_HEAD 8
#define SHA_TAIL 4

/* ---------------------------------------------------------------------------
 * health_stamp() — "13:24 today", "Sep 8 at 09:02" — the moment a check
 * was made, in as few words as carry it.
 *
 * task_local_dt rather than a _local constructor: those re-resolve the
 * timezone on every call (gotcha 27).  Returns a new string.
 * ------------------------------------------------------------------------- */
static gchar *
health_stamp(gint64 when)
{
    GDateTime *dt = task_local_dt(when);
    if (dt == NULL)
        return g_strdup("an unknown time");

    GDateTime *now = g_date_time_new_now(task_local_tz());
    gboolean today = now != NULL &&
        g_date_time_get_year(dt)  == g_date_time_get_year(now) &&
        g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(now);
    /* %H:%M, not %l — GLib pads the 12-hour form with a FIGURE SPACE that
     * g_strstrip does not remove (gotcha 23).                            */
    gchar *out = today ? g_date_time_format(dt, "%H:%M today")
                       : g_date_time_format(dt, "%b %-d at %H:%M");
    if (now != NULL)
        g_date_time_unref(now);
    g_date_time_unref(dt);
    return out != NULL ? out : g_strdup("an unknown time");
}

/* ---------------------------------------------------------------------------
 * db_health_refresh() — paint the LED, the verdict and the digest from
 * whatever the last recorded check found.
 *
 * Reads the STAMP rather than checking anything: a health block that ran a
 * PRAGMA pass and hashed the whole file every time the window was drawn
 * would be a health check nobody asked for.  The startup pass and the
 * Check button are what write it.
 * ------------------------------------------------------------------------- */
static void
db_health_refresh(DbSection *s)
{
    const TaskDbHealth *h = task_db_health(s->app->db);

    const gchar *led;                /* which circle                        */
    gchar *text;                     /* the verdict beside it               */
    if (h == NULL) {
        led  = LED_UNKNOWN;
        text = g_strdup("Not checked");
    } else {
        gchar *when = health_stamp(h->when);
        if (h->ok) {
            led  = LED_OK;
            text = g_strdup_printf("Healthy \xe2\x80\x94 checked %s", when);
        } else if (h->ran) {
            led  = LED_BAD;
            text = g_strdup_printf("Problems found \xe2\x80\x94 %s", when);
        } else {
            /* The checks could not be RUN.  A different sentence from
             * "problems found", and the one worth exposing: a locked or
             * unreadable file reading as merely unhealthy sends someone
             * looking for corruption that may not be there.              */
            led  = LED_BAD;
            text = g_strdup_printf("Check did not complete \xe2\x80\x94 %s",
                                   when);
        }
        g_free(when);
    }
    gtk_label_set_text(GTK_LABEL(s->led_label), led);

    gchar *markup = g_markup_printf_escaped("<small>%s</small>", text);
    gtk_label_set_markup(GTK_LABEL(s->health_label), markup);
    g_free(markup);
    /* The detail is sqlite's own words and can run to many lines, so it
     * lives on the tooltip: the row says WHAT, hovering says which.      */
    /* Set BEFORE the detail is read out of `h`, and the ternaries below
     * test `h` first: with no pass made there is no struct to read.     */
    gtk_widget_set_tooltip_text(s->health_label,
        h != NULL && h->detail != NULL ? h->detail
      : h != NULL ? "PRAGMA integrity_check and PRAGMA foreign_key_check "
                    "both passed against this file."
                  : "Nothing has verified this database yet.  "
                    "Press Update.");
    g_free(text);
}

/* ---------------------------------------------------------------------------
 * db_sha_refresh() — hash the database file and show the digest.
 *
 * Hashed HERE rather than read off the health stamp, because a digest
 * stored inside the file it describes is wrong the moment it is stored —
 * writing it changes the file.  So this is the fingerprint of the file as
 * it stands, which is the only form of it a user can check against
 * `shasum -a 256` or against a backup.
 * ------------------------------------------------------------------------- */
static void
db_sha_refresh(DbSection *s)
{
    GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(s->sha_btn));
    gchar     *sha = task_db_file_sha256(s->app->db->path);

    if (sha != NULL && strlen(sha) > SHA_HEAD + SHA_TAIL) {
        gchar *shown = g_strdup_printf(
            "%.*s\xe2\x80\xa6%s", SHA_HEAD, sha,
            sha + strlen(sha) - SHA_TAIL);
        gchar *m = g_markup_printf_escaped("<small>%s</small>", shown);
        gtk_label_set_markup(GTK_LABEL(lbl), m);
        g_free(m);
        g_free(shown);
        gchar *tip = g_strdup_printf(
            "%s\n\nThe file as it stands.  A database in use changes with "
            "the next edit, so this moves.\n\nClick to copy.", sha);
        gtk_widget_set_tooltip_text(s->sha_btn, tip);
        g_free(tip);
        gtk_widget_set_sensitive(s->sha_btn, TRUE);
        /* The full digest rides the button, so the click that copies it
         * does not go back to the disk for a file that has moved on.   */
        g_object_set_data_full(G_OBJECT(s->sha_btn), "task-sha",
                               sha, g_free);
    } else {
        gtk_label_set_markup(GTK_LABEL(lbl),
                             "<small>\xe2\x80\x94</small>");
        gtk_widget_set_tooltip_text(s->sha_btn,
                                    "The database file could not be read.");
        gtk_widget_set_sensitive(s->sha_btn, FALSE);
        g_object_set_data(G_OBJECT(s->sha_btn), "task-sha", NULL);
        g_free(sha);
    }
}

/* db_section_refresh() — sync every widget with the current app state.     */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped(
        "<small>%s</small>", s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);

    gint n_tasks, n_lists;           /* totals across the database          */
    task_db_totals(s->app->db, &n_tasks, &n_lists);
    gchar *data = g_strdup_printf(
        "<small>%d task%s in %d list%s</small>",
        n_tasks, n_tasks == 1 ? "" : "s",
        n_lists, n_lists == 1 ? "" : "s");
    gtk_label_set_markup(GTK_LABEL(s->data_label), data);
    g_free(data);

    /* g_stat, not the page count: what the user is being told is how much
     * room the file takes, which includes free pages a VACUUM would give
     * back.                                                              */
    GStatBuf st;                     /* for the database file size          */
    gchar *size = (g_stat(s->app->db->path, &st) == 0)
                  ? g_format_size((guint64)st.st_size)
                  : g_strdup("unknown");
    gchar *size_m = g_markup_printf_escaped("<small>%s</small>", size);
    gtk_label_set_markup(GTK_LABEL(s->size_label), size_m);
    g_free(size_m);
    g_free(size);

    db_health_refresh(s);
    db_sha_refresh(s);
}

/* ---------------------------------------------------------------------------
 * on_db_update_clicked() — "Update": renew every line on the plate.
 *
 * Runs a health pass and then re-reads the WHOLE block, not just the
 * verdict: the plate is one statement about one file, and a button under
 * it that refreshed two of its five lines would leave the other three
 * saying whatever they said when the window opened.  The counts and the
 * size go stale on their own — anything the user has done in the library
 * since moves both — so they are exactly what someone presses this for.
 *
 * Reports in the status bar as well, because a check that comes back
 * clean changes nothing visible and would otherwise look like a button
 * that does nothing.
 * ------------------------------------------------------------------------- */
static void
on_db_update_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    gboolean ok = task_db_health_check(s->app->db);
    /* The check writes nothing, so this re-reads five lines that are
     * unchanged unless the LIBRARY changed them — which is the whole
     * reason the digest now holds still from one press to the next. */
    db_section_refresh(s);
    const TaskDbHealth *h = task_db_health(s->app->db);
    task_app_status(s->app, "%s", ok
        ? "Database check passed"
        : h->ran ? "Database check found problems"
                 : "Database check did not complete");
}

/* on_db_sha_clicked() — put the full digest on the clipboard.              */
static void
on_db_sha_clicked(GtkButton *btn, gpointer user_data)
{
    DbSection *s = user_data;
    const gchar *sha = g_object_get_data(G_OBJECT(btn), "task-sha");
    if (sha == NULL)
        return;
    gtk_clipboard_set_text(
        gtk_clipboard_get_for_display(gtk_widget_get_display(GTK_WIDGET(btn)),
                                      GDK_SELECTION_CLIPBOARD), sha, -1);
    task_app_status(s->app, "SHA-256 copied to the clipboard");
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

/* ---------------------------------------------------------------------------
 * small_button() — a text button at about half the theme's default bulk,
 * and the one spelling of a PUSH BUTTON in this window, so no two of them
 * can come to be two sizes.  (The SHA-256 row's is not one of them and
 * should not become one — it is a relief-less label that happens to be
 * clickable, and it strips its box entirely to keep the digest on the
 * grid's value column.)
 *
 * These buttons sit UNDER the lines they act on rather than beside them —
 * the Database section's under its plate of quiet facts — so a full-size
 * button reads as the loudest thing in a block whose point is the text
 * above it.
 * Shrunk by padding and font size rather than by a shorter label: the
 * words are what say what the button does.
 *
 * min-height/min-width are named because Adwaita floors both, so trimming
 * the padding alone moves nothing (the same floor small_spin has to name,
 * where it is the whole of the difference).
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_button(const gchar *label)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    task_app_widget_add_css(btn,
        "button { padding: 1px 8px; min-height: 0; min-width: 0; }"
        "button label { font-size: 85%; }");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    return btn;
}

/* ---------------------------------------------------------------------------
 * small_spin() — a spin button no wider than the digits it can hold.
 *   lo, hi — the range; `chars` — digits to size the entry for.
 *
 * A default GtkSpinButton is enormous for a three-digit number, and the
 * lever is NOT the obvious one: `gtk_entry_set_width_chars` alone moves
 * almost nothing, because Adwaita floors `min-width` on the entry and on
 * both stepper buttons, and a floor beats a request.  MEASURED over the
 * "Every N minutes, keeping M files" row: width_chars alone took it from
 * 432 px to 430 — two pixels, which reads exactly like "the setting is
 * ignored" and is very nearly is.  Naming the floors as well takes the
 * same row to 346.  (Same trap as small_button's min-height, and the same
 * lesson as gotcha 18: a property being DISCARDED looks identical to one
 * that is too subtle, so measure rather than nudging the number.)
 *
 * Both levers are kept, because they do different jobs: the CSS removes
 * the floor, and `chars` is what then decides the width — sized to the
 * RANGE, so the widest value a user can reach still fits without the
 * entry scrolling under them.
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_spin(gdouble lo, gdouble hi, gdouble step, gint chars)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), chars);
    gtk_entry_set_max_width_chars(GTK_ENTRY(spin), chars);
    task_app_widget_add_css(spin,
        "spinbutton { min-width: 0; min-height: 0; }"
        "spinbutton entry { min-width: 0; min-height: 0; padding: 1px 2px; }"
        "spinbutton button { min-width: 0; min-height: 0; padding: 0 2px; }");
    return spin;
}

/* ---------------------------------------------------------------------------
 * info_row_attach() / info_row() — one "Label:  value" line of the
 * Database section's health block.
 *
 * The label column is what makes the block read as one statement about
 * one file: attaching to a grid puts every value at the same x for free,
 * where five separately packed lines would each start wherever their own
 * words ended.
 *
 * info_row_attach takes a WIDGET, for the two rows whose value is more
 * than text; info_row is the plain case and hands back the label to fill
 * in later.  Both are <small> — the size the path line has always been.
 * ------------------------------------------------------------------------- */
static void
info_row_attach(GtkWidget *grid, gint row, const gchar *name,
                GtkWidget *value)
{
    GtkWidget *key = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<small>%s</small>", name);
    gtk_label_set_markup(GTK_LABEL(key), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(key), 0.0);
    gtk_widget_set_valign(key, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
    gtk_widget_set_halign(value, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static GtkWidget *
info_row(GtkWidget *grid, gint row, const gchar *name)
{
    GtkWidget *value = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(value), 0.0);
    /* The path is the long one and the reason for both calls; on a short
     * value they cost nothing.                                          */
    gtk_label_set_line_wrap(GTK_LABEL(value), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(value), 44);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    /* Selectable labels come up with the whole text selected, which reads
     * as five highlighted rows the moment the window opens.             */
    gtk_label_select_region(GTK_LABEL(value), 0, 0);
    info_row_attach(grid, row, name, value);
    return value;
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

    GtkWidget *shadow_check = gtk_check_button_new_with_label(
        "Show drop shadows on Kanban cards");
    gtk_widget_set_tooltip_text(shadow_check,
        "Lifts each card off its lane.  The shadow is blurred, and GTK "
        "redraws that blur\nevery time the board paints \xe2\x80\x94 turning it "
        "off costs the board nothing\nand gives back about 1 ms per lane "
        "repaint.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shadow_check),
        task_app_config_get_bool("kanban_shadow", TRUE));
    g_signal_connect(shadow_check, "toggled",
                     G_CALLBACK(on_kanban_shadow_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), shadow_check, FALSE, FALSE, 0);

    GtkWidget *overdue_check = gtk_check_button_new_with_label(
        "Include all past-due tasks in the Due Today view");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(overdue_check),
        task_app_config_get_bool("due_today_show_overdue", FALSE));
    g_signal_connect(overdue_check, "toggled",
                     G_CALLBACK(on_due_today_overdue_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), overdue_check, FALSE, FALSE, 0);

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

    /* --- What this database IS, before anything that changes it ----------
     * A GtkGrid rather than a stack of lines with a size group: the values
     * have to start at one x or the five rows read as five unrelated
     * sentences, and a grid is where GTK already keeps that rule.  Every
     * value is <small>, the size the path line has always been.          */
    GtkWidget *info = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(info), 8);
    gtk_grid_set_row_spacing(GTK_GRID(info), 2);

    /* The five facts sit on a plate of their own, so what the database IS
     * is visibly a different kind of thing from the controls below that
     * CHANGE it.
     *
     * A GtkFrame, and it matters that it is one: a frame is a NO-WINDOW
     * widget, so CSS padding and border sit on it properly — where a
     * visible-window GtkEventBox would ignore both for its own size and
     * come out exactly as big as the grid, with the text hard against the
     * border it had just drawn (gotcha 18).  Its own shadow is turned OFF
     * so the theme's frame edge does not double up with the CSS one.
     *
     * The colours are NAMED theme colours, never literals.  @theme_base_color
     * is the white a light theme paints its entries and lists with — so
     * this is white on Adwaita, as asked — but it follows the theme into
     * dark instead of leaving a white slab there, which is exactly what
     * the compact float bar's hardcoded greys once did.  Named colours
     * also mean GTK re-resolves them itself on a light/dark switch, so
     * this needs none of themed_bg_css_apply's reload dance: that helper
     * exists because it bakes a RESOLVED literal into its CSS from C.
     * A theme naming neither colour drops the declarations and leaves the
     * plate flat, which is a plain look rather than an unreadable one. */
    GtkWidget *plate = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(plate), GTK_SHADOW_NONE);
    task_app_widget_add_css(plate,
        "frame { background-color: @theme_base_color;"
        "        border: 1px solid @borders;"
        "        border-radius: 6px;"
        "        padding: 8px 10px; }");
    gtk_container_add(GTK_CONTAINER(plate), info);

    /* Plate and button in a box of their own, and the SECTION MARGINS GO
     * ON THE BOX rather than on the plate: that is what makes the two
     * share one right edge.  With the margins on the plate, aligning the
     * button to the box would land it 12 px outside the plate's border —
     * lined up with nothing a user can see.  The box's 5 px spacing is
     * the gap between them.                                            */
    GtkWidget *plate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(plate_box, 12);
    gtk_widget_set_margin_end(plate_box, 12);
    gtk_widget_set_margin_bottom(plate_box, 8);
    gtk_box_pack_start(GTK_BOX(plate_box), plate, FALSE, FALSE, 0);

    dbs->update_btn = small_button("Update");
    gtk_widget_set_tooltip_text(dbs->update_btn,
        "Re-read every line above: run PRAGMA integrity_check and PRAGMA "
        "foreign_key_check against this database, then re-count its tasks "
        "and lists and re-read its size and SHA-256.");
    /* Right edge against the plate's, which is what the shared margins
     * above are for.                                                   */
    gtk_widget_set_halign(dbs->update_btn, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(plate_box), dbs->update_btn, FALSE, FALSE, 0);

    /* Ordered by what someone is actually asking.  Health leads: it is the
     * one line that can be BAD NEWS, and a block whose verdict is fourth
     * makes someone read three facts before finding out whether any of
     * them are worth having.  The path, the two quantities and the digest
     * follow as what the verdict is ABOUT — the digest last, because it
     * is the one line nobody reads unless they came looking for it.    */

    /* Health: the LED, its verdict, and the button that renews both.
     * The three are one value in three widgets, so they share the value
     * column rather than taking a column each — a column apiece would
     * push the block past the width the window asks for.
     *
     * The Update button is NOT on this row: it renews every line of the
     * plate, so it belongs under the whole plate rather than beside the
     * one line it used to refresh.                                     */
    GtkWidget *health_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    dbs->led_label = gtk_label_new(LED_UNKNOWN);
    /* Sized like Notes' save-state dot, which is the indicator this is
     * meant to read as.                                                 */
    task_app_widget_add_css(dbs->led_label, "label { font-size: 70%; }");
    gtk_box_pack_start(GTK_BOX(health_row), dbs->led_label, FALSE, FALSE, 0);
    dbs->health_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->health_label), 0.0);
    gtk_box_pack_start(GTK_BOX(health_row), dbs->health_label,
                       FALSE, FALSE, 0);
    info_row_attach(info, 0, "Health:", health_row);

    dbs->path_label = info_row(info, 1, "Current database:");
    dbs->data_label = info_row(info, 2, "Data:");
    dbs->size_label = info_row(info, 3, "Size on disk:");

    dbs->sha_btn = gtk_button_new_with_label("\xe2\x80\x94");
    gtk_button_set_relief(GTK_BUTTON(dbs->sha_btn), GTK_RELIEF_NONE);
    /* Every trace of the button's own box: padding, border and margin all
     * offset the label, and the digest has to start at the same x as the
     * four values above it or the column the grid exists to make is
     * broken by the one row that is not a plain label.                  */
    task_app_widget_add_css(dbs->sha_btn,
        "button { padding: 0; margin: 0; border: none; min-height: 0;"
        "         min-width: 0; }"
        "button label { font-family: monospace; }");
    /* Attached straight to the grid: with the button gone this row is one
     * widget, and a box holding a single child is a box that says nothing.
     */
    info_row_attach(info, 4, "SHA-256:", dbs->sha_btn);

    gtk_box_pack_start(GTK_BOX(vbox), plate_box, FALSE, FALSE, 0);

    g_signal_connect(dbs->sha_btn, "clicked",
                     G_CALLBACK(on_db_sha_clicked), dbs);
    g_signal_connect(dbs->update_btn, "clicked",
                     G_CALLBACK(on_db_update_clicked), dbs);

    /* There is NO control here for WHERE the database is kept, and there
     * must not be one again: the file lives at the default location, and
     * a database somewhere else is opened through File → Open Database
     * File… — which is also what a launch with nothing at the
     * default location offers.  A second way in was a second flow that
     * MOVED the file rather than opening one, so the same question was
     * answered in two places by two different mechanisms.               */
    db_section_refresh(dbs);

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

    /* Interval and retention, DIRECTLY under the switch: they are the
     * schedule that switch turns on, where the destination and the two
     * buttons below are about WHERE it lands.  The retention cap is the
     * "don't fill the disk" guarantee, so it is a spin button with a hard
     * floor of 1 — a rotation that keeps nothing is not a rotation.       */
    GtkWidget *bk_opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_opts, 12);
    gtk_box_pack_start(GTK_BOX(bk_opts), gtk_label_new("Every"),
                       FALSE, FALSE, 0);
    dbs->bk_interval_spin = small_spin(0, 10080, 15, 5);
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
    dbs->bk_keep_spin = small_spin(1, 500, 1, 3);
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

    /* Where they land, said once at the foot of the block — and the two
     * buttons that change it directly under, so the line and the control
     * that answers it read together.                                    */
    dbs->bk_path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->bk_path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->bk_path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->bk_path_label), 40);
    gtk_widget_set_margin_start(dbs->bk_path_label, 12);
    gtk_widget_set_margin_end(dbs->bk_path_label, 12);
    gtk_widget_set_margin_top(dbs->bk_path_label, 6);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->bk_path_label, FALSE, FALSE, 0);

    /* RIGHT-ALIGNED, and the right edge is the UPDATE button's: both rows
     * carry margin_end 12 and hug the right, so the section has one right
     * edge running down it rather than two that are nearly the same.
     * halign END shrinks the row to its natural width and parks it there,
     * which is what puts "Back Up Now" — packed last, so rightmost — flush
     * with Update above.  Do not swap this for pack_end on a full-width
     * row: that reverses the pair, and the folder is chosen before the
     * backup is taken.                                                  */
    GtkWidget *bk_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_btns, 12);
    gtk_widget_set_margin_end(bk_btns, 12);
    gtk_widget_set_halign(bk_btns, GTK_ALIGN_END);
    dbs->bk_choose_btn = small_button("Change Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(bk_btns), dbs->bk_choose_btn,
                       FALSE, FALSE, 0);
    dbs->bk_now_btn = small_button("Back Up Now");
    gtk_box_pack_start(GTK_BOX(bk_btns), dbs->bk_now_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bk_btns, FALSE, FALSE, 0);

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

    /* No "check integrity on startup" switch: the check runs every launch
     * (see main.c).  A health check with an off switch can only ever
     * report silence that means "not looked", which is the one answer it
     * must never give.                                                  */

    sw->loading = FALSE;

    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_settings_destroy), sw);
    gtk_widget_show_all(sw->window);
}
