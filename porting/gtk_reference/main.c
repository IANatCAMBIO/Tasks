/* ===========================================================================
 * main.c — Tasks entry point
 *
 * GTK4 + SQLite task-list app in plain C — the companion app to Notes.
 * Boot order: config (needs argv[0] for the portable ini) → optional
 * synchronous database open → GtkApplication → async Welcome chain if
 * needed → library window → workers armed.
 *
 * GTK4 port of the GTK3 main.c.  All synchronous GTK3 dialog loops are
 * replaced with an async chain (startup_first_run / first_run_chosen /
 * first_run_picked / first_run_proceed) following the Notes app pattern
 * (~/salt_development/notes/src/main.c).
 * =========================================================================== */

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <sqlite3.h>
#include <string.h>
#include "app.h"
#include "db.h"
#include "backup.h"
#include "recur.h"
#include "task_worker.h"
#include "core_views.h"
#include "library_window.h"

#ifdef __APPLE__
/* ---------------------------------------------------------------------------
 * quartz_log_filter() — GLogFunc that drops three specific, benign messages
 * emitted on macOS and forwards everything else unchanged.
 *
 * 1. Gdk-CRITICAL "gdk_atom_intern: assertion 'atom_name != NULL'" —
 *    GDK Quartz calls gdk_atom_intern(uti.preferredMIMEType.UTF8String) for
 *    each NSPasteboard type; modern macOS types whose UTI has no MIME string
 *    return a NULL UTF8String, tripping the g_return_if_fail.  Non-fatal:
 *    GDK_NONE is returned and enumeration continues.
 *
 * 2. Gtk-CRITICAL "gtk_menu_tracker_remove_items: assertion
 *    '*change_point != NULL'" — MacPorts' patch-gtk-menu-crash.diff adds
 *    this guard, but the end-of-section change_point that every append to a
 *    live menu model produces is exactly that case.  The menu is built and
 *    works; the check is misplaced.
 *
 * 3. GLib-WARNING "poll(2) failed due to: …" — GDK's macOS poll deliberately
 *    returns -1 WITHOUT setting errno when Cocoa re-enters the GLib main loop
 *    from inside nextEventMatchingMask; GLib then sees the changed fd set and
 *    re-runs the iteration.  The errno in the message is a leftover from an
 *    earlier syscall.
 *
 * (See notes/src/main.c for the full analysis.  The GTK3 texts are kept
 * across the port; a filter that never matches costs nothing.)
 *   domain  — log domain ("Gdk"/"Gtk"/"GLib" for the three messages).
 *   level   — log level flags.
 *   message — the formatted log text.
 *   data    — unused.
 * ------------------------------------------------------------------------- */
static void
quartz_log_filter(const gchar   *domain,
                  GLogLevelFlags level,
                  const gchar   *message,
                  gpointer       data)
{
    (void)domain; (void)level; (void)data;
    if (message != NULL &&
        strstr(message, "gdk_atom_intern") != NULL &&
        strstr(message, "atom_name != NULL") != NULL)
        return;                       /* benign macOS pasteboard artifact      */
    if (message != NULL &&
        strstr(message, "gtk_menu_tracker_remove_items") != NULL &&
        strstr(message, "*change_point != NULL") != NULL)
        return;                       /* MacPorts' misplaced tracker guard     */
    if (message != NULL &&
        strstr(message, "poll(2) failed due to:") != NULL)
        return;                       /* GDK's macOS stale-fd bail-out         */
    g_log_default_handler(domain, level, message, data);
}
#endif /* __APPLE__ */

/* Exit status main() returns when startup fails inside the main loop
 * (database open deferred to the Welcome chain).                             */
static int startup_status = 0;

/* TRUE while the Welcome dialog chain is up, so a second activation cannot
 * start a second chain.                                                       */
static gboolean first_run_pending = FALSE;

/* ---------------------------------------------------------------------------
 * startup_db_path() — where the database is expected: the configured db_dir
 * plus the fixed filename, or the per-user default.
 * Returns a new string; caller must g_free() it.
 * ------------------------------------------------------------------------- */
static gchar *
startup_db_path(void)
{
    gchar *db_dir  = task_app_config_get("db_dir");
    gchar *db_path = task_db_resolve_path(db_dir);
    g_free(db_dir);
    return db_path;
}

/* ---------------------------------------------------------------------------
 * startup_open_db() — open (or create) the database and run migrations.
 * On failure the error goes to stderr.
 *   app — the application context; app->db is set on success.
 * Returns TRUE when the database is open.
 * ------------------------------------------------------------------------- */
static gboolean
startup_open_db(TaskApp *app)
{
    gchar  *db_path = startup_db_path();
    GError *gerr    = NULL;
    app->db = task_db_open(db_path, &gerr);
    if (app->db == NULL) {
        g_printerr("tasks: %s\n",
                   gerr != NULL ? gerr->message : "cannot open database");
        g_clear_error(&gerr);
        g_free(db_path);
        return FALSE;
    }
    g_free(db_path);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * startup_finish() — everything activation does once the database is open:
 * the library window, the PRAGMA health check behind it, and the worker
 * timers.  Reached directly when the database was already there, or at the
 * end of the Welcome dialog chain.
 *
 * PRAGMA integrity_check + foreign_key_check: EVERY launch, with NO setting
 * to switch it off (the switch was removed 2026-09-09).  Two PRAGMAs on a
 * database this size.  The only thing that catches a file gone bad before
 * more is written into it.  An off-switch on a health check is a way to be
 * told nothing is wrong by a check that never ran — the one outcome this
 * code exists to avoid.
 *   app — the application context, its database open.
 * ------------------------------------------------------------------------- */
static void
startup_finish(TaskApp *app)
{
    task_library_window_new(app);

    /* Arm every registered worker AFTER the window, so a status message
     * posted during arming has somewhere to land.                             */
    task_worker_arm_all(app, app->db->path);

    gboolean db_ok = task_db_health_check(app->db);
    if (!db_ok) {
        const TaskDbHealth *h = task_db_health(app->db);
        task_app_notice(GTK_WINDOW(app->library_window),
                        "Tasks \xe2\x80\x94 Database Integrity Check",
                        h->ran ? "The database integrity check found "
                                 "issues:\n\n%s"
                               : "The database integrity check did not "
                                 "complete:\n\n%s",
                        h->detail != NULL ? h->detail : "?");
    }

    if (db_ok)
        task_app_status(app, "DB at %s loaded, integrity check passed",
                        app->db->path);
}

/* Forward declarations for the async chain.                                  */
static void startup_first_run(TaskApp *app);

/* ---------------------------------------------------------------------------
 * first_run_proceed() — the Welcome chain's one exit: open the database at
 * its (possibly just chosen) location and carry on with activation, or fail
 * the launch.  Either way the hold on_activate() took is released.
 * ------------------------------------------------------------------------- */
static void
first_run_proceed(TaskApp *app)
{
    first_run_pending = FALSE;
    if (startup_open_db(app))
        startup_finish(app);
    else
        startup_status = 1;
    g_application_release(G_APPLICATION(app->gtk_app));
}

/* ---------------------------------------------------------------------------
 * first_run_picked() — TaskPickFn for the Welcome dialog's "Open a
 * tasks.db File" option: remember the file's directory in the ini and
 * proceed.  A cancelled chooser returns to the choice dialog.
 *   path      — the chosen file (owned by this callback), or NULL.
 *   user_data — the TaskApp context.
 * ------------------------------------------------------------------------- */
static void
first_run_picked(gchar *path, gpointer user_data)
{
    TaskApp *app = user_data;
    if (path == NULL) {
        startup_first_run(app);       /* cancelled — back to the choice       */
        return;
    }
    gchar *dir = g_path_get_dirname(path);
    g_free(path);
    task_app_config_set("db_dir", dir);
    g_free(dir);
    first_run_proceed(app);
}

/* ---------------------------------------------------------------------------
 * first_run_chosen() — GAsyncReadyCallback for the Welcome dialog.
 * button 0 = "Open a tasks.db File" (file chooser),
 * button 1 = "Create a New tasks.db" (proceed directly),
 * -1        = dismissed (quit).
 *   source    — the GtkAlertDialog.
 *   result    — the async result.
 *   user_data — the TaskApp context.
 * ------------------------------------------------------------------------- */
static void
first_run_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TaskApp        *app    = user_data;
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    gint button = gtk_alert_dialog_choose_finish(dialog, result, NULL);
    g_object_unref(dialog);

    if (button == 0) {
        task_app_pick_path(NULL, "Tasks - Open Database",
                           TASK_PICK_OPEN, "_Open",
                           "Tasks Database (" TASK_DB_FILENAME ")",
                           TASK_DB_FILENAME, NULL,
                           first_run_picked, app);
    } else if (button == 1) {
        first_run_proceed(app);
    } else {                          /* dismissed (Escape / close button)    */
        first_run_pending = FALSE;
        g_application_release(G_APPLICATION(app->gtk_app));
    }
}

/* ---------------------------------------------------------------------------
 * startup_first_run() — no tasks.db at the expected location: ask whether to
 * open an existing file or create a new one.  Asynchronous; the answer
 * arrives in first_run_chosen() and the chain ends in first_run_proceed() or
 * a quit.
 * ------------------------------------------------------------------------- */
static void
startup_first_run(TaskApp *app)
{
    static const gchar *const BUTTONS[] = {
        "_Open a tasks.db File", "Create a _New tasks.db", NULL
    };
    gchar *expected = startup_db_path();
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Tasks - Welcome");
    gchar *detail = g_strdup_printf(
        "No tasks database was found at\n%s", expected);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_buttons(dialog, BUTTONS);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_choose(dialog, NULL, NULL, first_run_chosen, app);
    g_free(detail);
    g_free(expected);
}

/* ---------------------------------------------------------------------------
 * on_startup() — GtkApplication "startup" handler, run once after GTK has a
 * display.
 * ------------------------------------------------------------------------- */
static void
on_startup(GtkApplication *gtk_app, gpointer user_data)
{
    (void)gtk_app;
    TaskApp *app = user_data;

    /* Text rendering: off, GTK's default rounds every glyph advance to a
     * whole logical pixel, putting letters up to a device pixel off their
     * true position on Retina.  Off keeps fractional advances.  (The GL
     * renderer then clips glyphs at fractional positions; see main() for
     * why the cairo renderer is the macOS default.)                          */
    g_object_set(gtk_settings_get_default(),
                 "gtk-hint-font-metrics", FALSE, NULL);

    /* The icon theme serves every image the app draws by name.  icons/ holds
     * the toolbar PNGs flat by basename; GTK picks them up as "unthemed"
     * icons.  icons/theme/hicolor/… holds the bundled SVG pan-*-symbolic
     * arrows for crisp HiDPI tree expanders, and the app logo as the named
     * icon for the default window icon below.                               */
    GtkIconTheme *theme =
        gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(theme, app->icons_dir);
    gchar *theme_dir = g_build_filename(app->icons_dir, "theme", NULL);
    gtk_icon_theme_add_search_path(theme, theme_dir);
    g_free(theme_dir);
    gtk_window_set_default_icon_name("tasks");

    /* App-wide stylesheet: status-bar font, row stripes and kanban CSS.
     * Each module installs its own (task_app_css_install is idempotent-safe).
     * This one covers the baseline that every window shares.                 */
    task_app_css_install("label.task-status-label { font-size: 85%; }");
}

/* ---------------------------------------------------------------------------
 * on_activate() — GtkApplication "activate" handler: show the library window,
 * or raise it if the app is activated again.  When main() found no database
 * at the expected location the Welcome dialog runs first.
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    TaskApp *app = user_data;

    if (app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(app->library_window));
        return;
    }
    if (first_run_pending)
        return;                       /* the Welcome dialog is already up     */

    if (app->db == NULL) {
        /* No window keeps the application alive while the dialog is up.      */
        first_run_pending = TRUE;
        g_application_hold(G_APPLICATION(gtk_app));
        startup_first_run(app);
        return;
    }
    startup_finish(app);
}

/* ---------------------------------------------------------------------------
 * on_sigterm() — graceful shutdown on SIGTERM (pkill, logout, system
 * shutdown): destroying every window flushes editor autosaves and lets the
 * main loop end cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sigterm(gpointer user_data)
{
    TaskApp *app = user_data;
    GList *windows =
        g_list_copy(gtk_application_get_windows(app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * main() — set up the context, run the GTK main loop, tear down.
 * ------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
#ifdef __APPLE__
    /* Silence three benign macOS-only messages (see quartz_log_filter).
     * Installed before GTK so it covers every paste and the first menubar.   */
    g_log_set_handler("Gdk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
    g_log_set_handler("Gtk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
    g_log_set_handler("GLib",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
#endif

    /* Config first: everything else may read it.  argv[0] is needed to
     * locate the ini next to the binary (portable mode).                     */
    task_app_config_init(argc > 0 ? argv[0] : NULL);

    /* Classic full-width scrollbars everywhere, matching Notes.  Must be set
     * before GTK reads GtkSettings (first g_application_run call does it).   */
    g_setenv("GTK_OVERLAY_SCROLLING", "0", TRUE);

    /* Build the shared context.  All heap fields are set before the first
     * window, because the plugin registries are written without locks.        */
    TaskApp *app = g_new0(TaskApp, 1);
    app->editors = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);
    task_app_init_icons_dir(app);

    /* Register subsystems (sidebar views, workers) BEFORE any thread starts:
     * the registries are unlocked.                                            */
    task_core_views_init();
    task_recur_init(app);
    task_backup_init(app);

    /* Open the database now when it already exists at the expected location.
     * If it does not, app->db stays NULL and on_activate() runs the async
     * Welcome chain instead.                                                  */
    gchar *expected = startup_db_path();
    gboolean db_present = g_file_test(expected, G_FILE_TEST_EXISTS);
    g_free(expected);
    if (db_present && !startup_open_db(app)) {
        g_hash_table_destroy(app->editors);
        g_free(app->icons_dir);
        g_free(app);
        return 1;
    }

#ifdef __APPLE__
    /* The cairo renderer: compared side by side on a Retina display (GTK
     * 4.22), the GL renderer clips the left column of glyphs drawn at
     * fractional positions (a "G" loses its leftmost pixel once
     * gtk-hint-font-metrics is off).  Cairo draws straight through Pango —
     * no cache, fractional positions, the same grayscale rasterizer — and
     * looked best.  Must be set before GTK creates its first renderer.       */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
#endif

    app->gtk_app = gtk_application_new("org.example.tasks",
                                       G_APPLICATION_DEFAULT_FLAGS);
#ifdef __APPLE__
    /* Dock → Quit and logout route through "app.quit", which destroys every
     * window so editor autosaves flush.  Without register-session, AppKit's
     * default terminate exits the process immediately.                        */
    g_object_set(app->gtk_app, "register-session", TRUE, NULL);
#endif
    g_signal_connect(app->gtk_app, "startup",
                     G_CALLBACK(on_startup),  app);
    g_signal_connect(app->gtk_app, "activate",
                     G_CALLBACK(on_activate), app);
    g_unix_signal_add(SIGTERM, on_sigterm, app);

    int status = g_application_run(G_APPLICATION(app->gtk_app), argc, argv);

    /* Windows (and their final autosaves) are done by the time run() returns,
     * so the database can be closed safely now.                               */
    g_object_unref(app->gtk_app);
    g_hash_table_destroy(app->editors);
    /* Any listener still subscribed here outlived its window — not an error. */
    g_slist_free_full(app->changed_l, g_free);
    g_slist_free_full(app->tasks_l,   g_free);
    g_slist_free_full(app->status_l,  g_free);
    task_db_close(app->db);
    g_free(app->icons_dir);
    g_free(app);
    return startup_status != 0 ? startup_status : status;
}
