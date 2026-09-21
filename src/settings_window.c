/* ===========================================================================
 * settings_window.c — the Tasks settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "db.h"
#include "backup.h"
#include "library_window.h"
#include <glib/gstdio.h>          /* g_stat, GStatBuf                    */
#include <stdlib.h>               /* atoi                                */
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

#define SETTINGS_WIDTH 470           /* the width the column is measured    */
                                     /* at, and so the width it opens at    */

/* The tallest the column opens before it scrolls.  A CONSTANT, where the
 * GTK3 build asked the parent's monitor for its work area: GTK4 has no
 * window positioning and no "which monitor is the parent on" to ask, so
 * the cap is the same one Notes settled on.  A short screen still opens
 * scrolled — the window manager clamps the window to the screen and the
 * scroller then has less than this to work with.                           */
#define SETTINGS_MAX_HEIGHT 600

/* on_due_today_overdue_toggled() — Appearance: Due Today lists every
 * past-due task, on/off, applied live.                                     */
static void
on_due_today_overdue_toggled(GtkCheckButton *check, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_check_button_get_active(check);
    task_app_config_set("due_today_show_overdue", on ? "1" : "0");
    task_app_notify_changed(sw->app);
}

/* on_kanban_shadow_toggled() — Appearance: the Kanban cards' drop shadow
 * on/off, applied live.  It calls the APPLIER rather than notifying a
 * refresh: the board skips its rebuild while the same cards are showing,
 * so a notify would leave the setting looking inert.                      */
static void
on_kanban_shadow_toggled(GtkCheckButton *check, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_check_button_get_active(check);
    task_app_config_set("kanban_shadow", on ? "1" : "0");
    task_library_apply_kanban_shadow(sw->app, on);
}

/* on_bold_titles_toggled() — Appearance: bold task titles on/off,
 * applied live (the task pane re-renders its markup).                      */
static void
on_bold_titles_toggled(GtkCheckButton *check, gpointer data)
{
    TaskSettings *sw = data;
    if (sw->loading)
        return;
    gboolean bold = gtk_check_button_get_active(check);
    task_app_config_set("bold_task_titles", bold ? "1" : "0");
    task_app_notify_changed(sw->app);
}

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the Database settings block, kept alive so
 * handlers can update them after a check, a backup or a folder change.
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
    task_app_set_tooltip(s->health_label,
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
    GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(s->sha_btn));
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
        task_app_set_tooltip(s->sha_btn, tip);
        g_free(tip);
        gtk_widget_set_sensitive(s->sha_btn, TRUE);
        /* The full digest rides the button, so the click that copies it
         * does not go back to the disk for a file that has moved on.   */
        g_object_set_data_full(G_OBJECT(s->sha_btn), "task-sha",
                               sha, g_free);
    } else {
        gtk_label_set_markup(GTK_LABEL(lbl),
                             "<small>\xe2\x80\x94</small>");
        task_app_set_tooltip(s->sha_btn,
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
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(btn)), sha);
    task_app_status(s->app, "SHA-256 copied to the clipboard");
}

/* on_bk_toggled() — the backup master switch: persist and re-arm the
 * timer.  No folder prompt: task_backup_dir falls back to the default
 * database location, so switching this on always does something.
 * Choosing a folder is an improvement, not a prerequisite.                 */
static void
on_bk_toggled(GtkCheckButton *check, gpointer user_data)
{
    DbSection *s = user_data;
    gboolean on = gtk_check_button_get_active(check);
    task_app_config_set("backup_enabled", on ? "1" : "0");
    task_backup_auto_start(s->app, s->app->db->path);
    bk_section_refresh(s);
}

/* ---------------------------------------------------------------------------
 * on_bk_folder_picked() — task_app_pick_path()'s continuation for the
 * backup destination: persist the new folder, re-arm the timer against it
 * and re-render the block.
 *
 * Inputs:
 *   dir       — the chosen folder (owned here, freed), or NULL if the
 *               chooser was cancelled — nothing changes then.
 *   user_data — the DbSection.  The chooser is modal over the settings
 *               window, which is what keeps the section alive until this
 *               runs.
 * ------------------------------------------------------------------------- */
static void
on_bk_folder_picked(gchar *dir, gpointer user_data)
{
    DbSection *s = user_data;
    if (dir == NULL)
        return;
    task_app_config_set("backup_dir", dir);
    g_free(dir);
    task_backup_auto_start(s->app, s->app->db->path);
    bk_section_refresh(s);
}

/* on_bk_choose_clicked() — re-pick the destination folder.  Asynchronous:
 * on_bk_folder_picked does the rest once the chooser closes.               */
static void
on_bk_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    /* Start where backups go NOW — the resolved folder, default included —
     * so re-picking from wherever the chooser last was is not how backups
     * end up in two places.                                             */
    gchar *current = task_backup_dir();
    task_app_pick_path(GTK_WINDOW(gtk_widget_get_root(s->bk_check)),
                       "Choose Backup Folder", TASK_PICK_FOLDER, "_Select",
                       NULL, NULL, current, on_bk_folder_picked, s);
    g_free(current);
}

/* on_bk_interval_changed() — persist the cadence and re-arm the timer.     */
static void
on_bk_interval_changed(GtkSpinButton *spin, gpointer user_data)
{
    DbSection *s = user_data;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(spin));
    task_app_config_set("backup_interval_min", v);
    g_free(v);
    task_backup_auto_start(s->app, s->app->db->path);
}

/* on_bk_keep_changed() — persist the retention bound.  No re-arm: the
 * cadence has not moved, and the bound is read at the start of each pass.  */
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

/* on_settings_destroy() — clear the singleton.  Touches only the state it
 * owns: GTK4 emits this AFTER the child tree is gone (D3).                 */
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
 * settings_css_install() — the window's stylesheet, installed ONCE for the
 * display (a static guard) through task_app_css_install.  One rule per
 * `task-*` class the window puts on its widgets; a per-widget provider is
 * deprecated in GTK4 and could not follow a theme change anyway.
 *
 * 1. small_button: a compact button.  The theme FLOORS min-height and
 *    min-width, so both are named or trimming the padding moves nothing.
 * 2. small_spin: a spin button no wider than its digits.  GTK4 keeps the
 *    floor and the 8 px side padding on the `spinbutton` node itself
 *    (Default theme: `spinbutton:not(.vertical), entry { min-height: 32px;
 *    padding-left: 8px; padding-right: 8px }`), with a `text` child for
 *    the digits and `button` children for the steppers — GTK3 had them on
 *    an `entry` child, which no longer exists, so the selector is
 *    `spinbutton > text` (Notes measured it).  The pixel numbers recorded
 *    in CLAUDE.md for this row (432 → 430 → 346) were measured on GTK3
 *    and are to be RE-MEASURED in Phase 4; the two-lever RULE is what
 *    carries over.
 * 3. The health plate: a bordered frame in the theme's base colour.  The
 *    colours are NAMED theme colours, never literals: @theme_base_color is
 *    the white a light theme paints its entries and lists with, and it
 *    follows the theme into dark instead of leaving a white slab there.
 *    GTK re-resolves a named colour itself on a light/dark switch, which
 *    is why nothing here reloads anything.  Both names are still defined
 *    by GTK 4.22's Default theme (Notes verified it); a theme naming
 *    neither renders the declarations transparent, which leaves the plate
 *    flat — a plain look rather than an unreadable one.
 * 4. The SHA-256 button: a relief-less label that happens to be clickable,
 *    stripped of every trace of its own box — padding, border and margin
 *    all offset the label, and the digest has to start at the same x as
 *    the four values above it or the column the grid exists to make is
 *    broken by the one row that is not a plain label.
 * 5. The LED: sized like Notes' save-state dot, which is the indicator it
 *    is meant to read as.
 * ------------------------------------------------------------------------- */
static void
settings_css_install(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    task_app_css_install(
        "button.task-small-button {"
        "  padding: 1px 8px; min-height: 0; min-width: 0;"
        "}"
        "button.task-small-button > label { font-size: 85%; }"
        "spinbutton.task-small-spin {"
        "  min-width: 0; min-height: 0; padding: 1px 2px;"
        "}"
        "spinbutton.task-small-spin > text { min-width: 0; min-height: 0; }"
        "spinbutton.task-small-spin > button {"
        "  min-width: 0; min-height: 0; padding: 0 2px;"
        "}"
        "frame.task-plate {"
        "  background-color: @theme_base_color;"
        "  border: 1px solid @borders;"
        "  border-radius: 6px;"
        "  padding: 8px 10px;"
        "}"
        "button.task-sha-button {"
        "  padding: 0; margin: 0; border: none; min-height: 0; min-width: 0;"
        "}"
        "button.task-sha-button > label { font-family: monospace; }"
        "label.task-led-label { font-size: 70%; }");
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
 * min-height/min-width are named (settings_css_install) because the theme
 * floors both, so trimming the padding alone moves nothing (the same floor
 * small_spin has to name, where it is the whole of the difference).
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_button(const gchar *label)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "task-small-button");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    return btn;
}

/* ---------------------------------------------------------------------------
 * small_spin() — a spin button no wider than the digits it can hold.
 *   lo, hi — the range; `chars` — digits to size the entry for.
 *
 * A default GtkSpinButton is enormous for a three-digit number, and the
 * lever is NOT the obvious one: `width_chars` alone moves almost nothing,
 * because the theme floors `min-width` on the spin button and on both
 * stepper buttons, and a floor beats a request.  MEASURED on GTK3 over
 * the "Every N minutes, keeping M files" row: width_chars alone took it
 * from 432 px to 430 — two pixels, which reads exactly like "the setting
 * is ignored" and very nearly is.  Naming the floors as well took the same
 * row to 346.  (Same trap as small_button's min-height: a property being
 * DISCARDED looks identical to one that is too subtle, so measure rather
 * than nudging the number.)  Those numbers are GTK3's; the GTK4 row is
 * re-measured in Phase 4.
 *
 * Both levers are kept, because they do different jobs: the CSS
 * (settings_css_install) removes the floor, and `chars` is what then
 * decides the width — sized to the RANGE, so the widest value a user can
 * reach still fits without the entry scrolling under them.
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_spin(gdouble lo, gdouble hi, gdouble step, gint chars)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_editable_set_width_chars(GTK_EDITABLE(spin), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(spin), chars);
    gtk_widget_add_css_class(spin, "task-small-spin");
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
    gtk_label_set_wrap(GTK_LABEL(value), TRUE);
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
 * settings_scroller_new() — wrap the settings column in a vertical
 * scroller.  Both of the window's size problems come from the column
 * having been the window's DIRECT child: a plain GtkBox propagates its
 * whole content height as the toplevel's MINIMUM height, and GTK refuses
 * to size a window below its minimum — so the window could only ever be
 * grown, which reads as "it can't be resized" — and a column taller than
 * the screen ran off the bottom with no way to reach the last section.
 *
 * `propagate_natural_height` is what makes the window open at exactly the
 * height it needs; `max_content_height` caps that at SETTINGS_MAX_HEIGHT,
 * so a tall column opens scrolled instead of oversized;
 * `min_content_height` is what makes shrinking possible at all.
 * `propagate_natural_width` is the WIDTH's half of the same rule: there is
 * no default width on the window (a default size is what a window opens
 * AT, natural size or not), so the column's own request is what sets it.
 * Horizontal policy is NEVER — the column wraps its own explanatory
 * labels, so it must never scroll sideways.
 *
 * The child carries a SETTINGS_WIDTH width request because of those
 * wrapping labels: with NEVER, the scroller measures its natural height
 * for the child's MINIMUM width, and a label wrapped that narrow is
 * several lines taller than the same label at 470 px — the window would
 * open with a band of empty space under the last section.  Requesting the
 * real width makes the measurement match what is drawn.
 *
 * No overlay-scrolling call here: classic scrollbars are the
 * `gtk-overlay-scrolling` setting, set once in startup, which reaches
 * this scroller like every other (D11).
 * ------------------------------------------------------------------------- */
static GtkWidget *
settings_scroller_new(GtkWidget *child)
{
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_widget_set_size_request(child, SETTINGS_WIDTH, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(sc), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(sc), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc),
                                               240);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc),
                                               SETTINGS_MAX_HEIGHT);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), child);
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
    settings_css_install();

    TaskSettings *sw = g_new0(TaskSettings, 1);
    settings = sw;
    sw->app = app;
    sw->db_path = g_strdup(db_path);
    sw->loading = TRUE;

    sw->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(sw->window), "Tasks - Settings");
    gtk_window_set_transient_for(GTK_WINDOW(sw->window), parent);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    GtkWidget *scroller = settings_scroller_new(vbox);
    gtk_window_set_child(GTK_WINDOW(sw->window), scroller);

    /* --- Appearance --------------------------------------------------------- */
    gtk_box_append(GTK_BOX(vbox), section_label("Appearance"));

    GtkWidget *bold_check = gtk_check_button_new_with_label(
        "Show task titles in bold");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(bold_check),
        task_app_config_get_bool("bold_task_titles", FALSE));
    g_signal_connect(bold_check, "toggled",
                     G_CALLBACK(on_bold_titles_toggled), sw);
    gtk_box_append(GTK_BOX(vbox), bold_check);

    GtkWidget *shadow_check = gtk_check_button_new_with_label(
        "Show drop shadows on Kanban cards");
    task_app_set_tooltip(shadow_check,
        "Lifts each card off its lane.  The shadow is blurred, and GTK "
        "redraws that blur\nevery time the board paints \xe2\x80\x94 turning it "
        "off costs the board nothing\nand gives back about 1 ms per lane "
        "repaint.");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(shadow_check),
        task_app_config_get_bool("kanban_shadow", TRUE));
    g_signal_connect(shadow_check, "toggled",
                     G_CALLBACK(on_kanban_shadow_toggled), sw);
    gtk_box_append(GTK_BOX(vbox), shadow_check);

    GtkWidget *overdue_check = gtk_check_button_new_with_label(
        "Include all past-due tasks in the Due Today view");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(overdue_check),
        task_app_config_get_bool("due_today_show_overdue", FALSE));
    g_signal_connect(overdue_check, "toggled",
                     G_CALLBACK(on_due_today_overdue_toggled), sw);
    gtk_box_append(GTK_BOX(vbox), overdue_check);

    /* The rule between the two sections, with the 2 px of breathing room
     * above and below that the box's own spacing does not give it.       */
    GtkWidget *rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(rule, 2);
    gtk_widget_set_margin_bottom(rule, 2);
    gtk_box_append(GTK_BOX(vbox), rule);

    /* --- Database ---------------------------------------------------------- */
    gtk_box_append(GTK_BOX(vbox), section_label("Database"));

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
     * A GtkFrame: CSS padding and border sit on it properly, so the text
     * is not hard against the border.  The CSS border REPLACES the theme's
     * frame edge rather than doubling it up, since the rule restates the
     * whole `border` property (there is no shadow type to switch off in
     * GTK4).  The colours are named theme colours; the rule and the reason
     * are in settings_css_install.                                      */
    GtkWidget *plate = gtk_frame_new(NULL);
    gtk_widget_add_css_class(plate, "task-plate");
    gtk_frame_set_child(GTK_FRAME(plate), info);

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
    gtk_box_append(GTK_BOX(plate_box), plate);

    dbs->update_btn = small_button("Update");
    task_app_set_tooltip(dbs->update_btn,
        "Re-read every line above: run PRAGMA integrity_check and PRAGMA "
        "foreign_key_check against this database, then re-count its tasks "
        "and lists and re-read its size and SHA-256.");
    /* Right edge against the plate's, which is what the shared margins
     * above are for.                                                   */
    gtk_widget_set_halign(dbs->update_btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(plate_box), dbs->update_btn);

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
    gtk_widget_add_css_class(dbs->led_label, "task-led-label");
    gtk_box_append(GTK_BOX(health_row), dbs->led_label);
    dbs->health_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->health_label), 0.0);
    gtk_box_append(GTK_BOX(health_row), dbs->health_label);
    info_row_attach(info, 0, "Health:", health_row);

    dbs->path_label = info_row(info, 1, "Current database:");
    dbs->data_label = info_row(info, 2, "Data:");
    dbs->size_label = info_row(info, 3, "Size on disk:");

    dbs->sha_btn = gtk_button_new_with_label("\xe2\x80\x94");
    gtk_button_set_has_frame(GTK_BUTTON(dbs->sha_btn), FALSE);
    /* Every trace of the button's own box goes (settings_css_install), so
     * the digest starts at the same x as the four values above it.      */
    gtk_widget_add_css_class(dbs->sha_btn, "task-sha-button");
    /* Attached straight to the grid: with the button gone this row is one
     * widget, and a box holding a single child is a box that says nothing.
     */
    info_row_attach(info, 4, "SHA-256:", dbs->sha_btn);

    gtk_box_append(GTK_BOX(vbox), plate_box);

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
    task_app_set_tooltip(dbs->bk_check,
        "Writes a verified copy of the database into a folder of your "
        "choice on a timer, keeping only the most recent few.  Worth "
        "pointing at a disk INDEPENDENT of wherever the database itself "
        "lives, so one mishap cannot take both.");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dbs->bk_check),
        task_app_config_get_bool("backup_enabled", FALSE));
    gtk_box_append(GTK_BOX(vbox), dbs->bk_check);

    /* Interval and retention, DIRECTLY under the switch: they are the
     * schedule that switch turns on, where the destination and the two
     * buttons below are about WHERE it lands.  The retention cap is the
     * "don't fill the disk" guarantee, so it is a spin button with a hard
     * floor of 1 — a rotation that keeps nothing is not a rotation.       */
    GtkWidget *bk_opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_opts, 12);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("Every"));
    dbs->bk_interval_spin = small_spin(0, 10080, 15, 5);
    task_app_set_tooltip(dbs->bk_interval_spin,
        "Minutes between backups.  0 backs up only when you press "
        "Back Up Now.  A pass whose database has not changed since the "
        "last backup writes nothing.");
    gchar *bkiv = task_app_config_get("backup_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_interval_spin),
        bkiv != NULL ? atoi(bkiv) : TASK_BACKUP_INTERVAL_DEFAULT);
    g_free(bkiv);
    gtk_box_append(GTK_BOX(bk_opts), dbs->bk_interval_spin);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("minutes, keeping"));
    dbs->bk_keep_spin = small_spin(1, 500, 1, 3);
    task_app_set_tooltip(dbs->bk_keep_spin,
        "How many backup files to retain.  The oldest are removed once a "
        "NEW backup has been verified, never before.");
    gchar *bkkeep = task_app_config_get("backup_keep");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_keep_spin),
        bkkeep != NULL ? atoi(bkkeep) : TASK_BACKUP_KEEP_DEFAULT);
    g_free(bkkeep);
    gtk_box_append(GTK_BOX(bk_opts), dbs->bk_keep_spin);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("files"));
    gtk_box_append(GTK_BOX(vbox), bk_opts);

    /* Where they land, said once at the foot of the block — and the two
     * buttons that change it directly under, so the line and the control
     * that answers it read together.                                    */
    dbs->bk_path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->bk_path_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(dbs->bk_path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->bk_path_label), 40);
    gtk_widget_set_margin_start(dbs->bk_path_label, 12);
    gtk_widget_set_margin_end(dbs->bk_path_label, 12);
    gtk_widget_set_margin_top(dbs->bk_path_label, 6);
    gtk_box_append(GTK_BOX(vbox), dbs->bk_path_label);

    /* RIGHT-ALIGNED, and the right edge is the UPDATE button's: both rows
     * carry margin_end 12 and hug the right, so the section has one right
     * edge running down it rather than two that are nearly the same.
     * halign END shrinks the row to its natural width and parks it there,
     * which is what puts "Back Up Now" — appended last, so rightmost —
     * flush with Update above.  Do not swap this for prepending into a
     * full-width row: that reverses the pair, and the folder is chosen
     * before the backup is taken.  (MEASURED on GTK3 at SETTINGS_WIDTH
     * 470: both edges landed on x=444; re-measure in Phase 4.)          */
    GtkWidget *bk_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_btns, 12);
    gtk_widget_set_margin_end(bk_btns, 12);
    gtk_widget_set_halign(bk_btns, GTK_ALIGN_END);
    dbs->bk_choose_btn = small_button("Change Folder\xe2\x80\xa6");
    gtk_box_append(GTK_BOX(bk_btns), dbs->bk_choose_btn);
    dbs->bk_now_btn = small_button("Back Up Now");
    gtk_box_append(GTK_BOX(bk_btns), dbs->bk_now_btn);
    gtk_box_append(GTK_BOX(vbox), bk_btns);

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

    /* The scroller propagates the content's natural width, but a vertical
     * scrollbar it will show (content taller than the cap) is NOT in that
     * request — GtkScrolledWindow counts an AUTOMATIC scrollbar only once
     * it is up.  The bar then takes its width out of the content, and the
     * widgets on the right run under it with their margin clipped.  So
     * when the content will scroll, the window opens that much wider.
     * Measured before present, on the built tree (Notes' rule).          */
    gint nat_w, nat_h;               /* the column's natural size           */
    gtk_widget_measure(vbox, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &nat_w,
                       NULL, NULL);
    gtk_widget_measure(vbox, GTK_ORIENTATION_VERTICAL, nat_w, NULL, &nat_h,
                       NULL, NULL);
    if (nat_h > SETTINGS_MAX_HEIGHT) {
        gint bar_w;                  /* the scrollbar's own width           */
        gtk_widget_measure(gtk_scrolled_window_get_vscrollbar(
                               GTK_SCROLLED_WINDOW(scroller)),
                           GTK_ORIENTATION_HORIZONTAL, -1, NULL, &bar_w,
                           NULL, NULL);
        gtk_window_set_default_size(GTK_WINDOW(sw->window),
                                    nat_w + bar_w, -1);
    }

    gtk_window_present(GTK_WINDOW(sw->window));
}
