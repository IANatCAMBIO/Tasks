/* ===========================================================================
 * main.c — Tasks entry point
 *
 * A GTK3 + SQLite task-list application in plain C — the companion app to
 * Notes.  Boot order: config (needs argv[0] for the portable ini) →
 * database → the app's own registries (views, workers) → GtkApplication →
 * library window → the shared scheduler arms every registered worker.
 * =========================================================================== */

#include <gtk/gtk.h>
#include <sqlite3.h>
#include "app.h"
#include "db.h"
#include "backup.h"
#include "recur.h"
#include "task_worker.h"
#include "core_views.h"
#include "library_window.h"
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* ---------------------------------------------------------------------------
 * startup_integrity_check() — verify the database at launch and show a
 * warning dialog if anything is wrong.  Returns TRUE when it is sound.
 *
 * The checks themselves live in task_db_health_check, which also RECORDS
 * what it found — so the Settings window's health block normally shows a
 * result from this launch without running anything of its own.  All this
 * adds is the dialog, and the one distinction that dialog has to get
 * right: "found issues" would be a lie when the checks never ran at all,
 * which is exactly the case worth exposing.
 * ------------------------------------------------------------------------- */
static gboolean
startup_integrity_check(TaskApp *app)
{
    if (task_db_health_check(app->db))
        return TRUE;

    const TaskDbHealth *h = task_db_health(app->db);
    task_app_notice(NULL, GTK_MESSAGE_WARNING,
                    "Tasks \xe2\x80\x94 Database Integrity Check",
                    h->ran ? "The database integrity check found issues:"
                             "\n\n%s"
                           : "The database integrity check did not "
                             "complete:\n\n%s",
                    h->detail != NULL ? h->detail : "?");
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * startup_first_run() — no tasks.db found at the expected location:
 * ask whether to open an existing file or create a new one there, instead
 * of silently creating an empty database (a user pointing at a shared
 * folder usually means to OPEN a file that is already there).
 *   expected — the path where the db was looked for (shown in dialog text).
 *   db_path  — in/out: the path handed to task_db_open(); replaced when an
 *              existing file is opened.
 * Returns TRUE to proceed with task_db_open(*db_path), FALSE to quit.      */
static gboolean
startup_first_run(const gchar *expected, gchar **db_path)
{
    /* Loop so cancelling the file chooser returns to the choice dialog.    */
    for (;;) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "No tasks database was found at\n%s",
            expected);
        gtk_window_set_title(GTK_WINDOW(dlg), "Tasks - Welcome");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open a tasks.db File",  1,
            "Create a _New tasks.db", 2,
            NULL);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        if (resp == 2)
            return TRUE;             /* task_db_open() will create it       */
        if (resp != 1)
            return FALSE;            /* dialog closed — quit                */

        GtkWidget *chooser = gtk_file_chooser_dialog_new(
            "Open Tasks Database", NULL,
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT,
            NULL);
        gtk_window_set_title(GTK_WINDOW(chooser),
                             "Tasks - Open Database");
        /* Filter to tasks.db only — the ini stores db_dir, the
         * filename is always the fixed constant.                           */
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_add_pattern(ff, TASK_DB_FILENAME);
        gtk_file_filter_set_name(ff,
            "Tasks Database (" TASK_DB_FILENAME ")");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

        gchar *file_path = NULL;
        if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
            file_path = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);
        if (file_path == NULL)
            continue;                /* cancelled — back to the choice      */

        /* The chosen folder goes straight into the ini and nowhere else:
         * that key is what the NEXT launch resolves the path from, and
         * this one already has the path in hand.                        */
        gchar *dir = g_path_get_dirname(file_path);
        g_free(file_path);
        task_app_config_set("db_dir", dir);
        g_free(*db_path);  *db_path = g_build_filename(dir, TASK_DB_FILENAME,
                                                       NULL);
        g_free(dir);
        return TRUE;
    }
}

/* The single application context, shared with the activate handler.        */
typedef struct {
    TaskApp  *app;
    gchar  *db_path;
} TaskBoot;

/* ---------------------------------------------------------------------------
 * on_activate() — build the library window (or raise it on re-activate)
 * and arm the background workers.
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer data)
{
    (void)gtk_app;
    TaskBoot *boot = data;
    if (boot->app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(boot->app->library_window));
        return;
    }

    /* Bundled scalable theme icons (icons/theme/hicolor/...): provides
     * SVG pan-*-symbolic arrows so tree expanders render crisply on
     * HiDPI displays instead of GTK's built-in 1x raster fallbacks
     * (same set Notes ships; needs the librsvg pixbuf loader).           */
    gchar *theme_dir = g_build_filename(boot->app->icons_dir, "theme",
                                        NULL);
    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(),
                                       theme_dir);
    g_free(theme_dir);

    /* On Linux (and any non-macOS platform) the window manager shows a
     * generic icon unless we set one explicitly.  Load document.png from
     * the icons/ directory and register it as the default for all windows. */
#ifndef HAVE_GTKOSX
    gchar *icon_path = g_build_filename(boot->app->icons_dir,
                                        "document.png", NULL);
    GError *icon_err = NULL;
    gtk_window_set_default_icon_from_file(icon_path, &icon_err);
    g_clear_error(&icon_err);
    g_free(icon_path);
#endif

    /* PRAGMA integrity_check + foreign_key_check, EVERY launch and with no
     * setting to switch it off (removed 2026-09-09).  It is two PRAGMAs on
     * a database this size, it is the only thing that catches a file gone
     * bad before the user writes more into it, and an off switch on a
     * health check is a way to be told nothing is wrong by a check that
     * never ran — the one outcome this code exists to avoid.            */
    gboolean db_ok = startup_integrity_check(boot->app);

    task_library_window_new(boot->app);
    /* Arm every registered worker (see task_worker.h).  The window comes
     * first because an arming pass can post status, and a message posted
     * before anything is listening is simply dropped.                     */
    task_worker_arm_all(boot->app, boot->db_path);

    if (db_ok)
        task_app_status(boot->app, "DB at %s loaded, integrity check passed",
                        boot->app->db->path);

#ifdef HAVE_GTKOSX
    /* Honor the persisted native-menu-bar preference, then let the macOS
     * integration finish its launch handshake.                             */
    if (task_app_config_get_bool("native_menubar", FALSE))
        task_library_apply_native_menubar(boot->app, TRUE);
    gtkosx_application_ready(gtkosx_application_get());
#endif
}

/* ---------------------------------------------------------------------------
 * main() — set up the context and run the GTK main loop.
 * ------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    /* Config first: everything else may read it.                           */
    task_app_config_init(argc > 0 ? argv[0] : NULL);

    /* Classic full-width scrollbars everywhere instead of GTK's modern
     * overlay style, matching Notes.  This is the ONE lever: it is a
     * GtkSettings default, so it reaches every scroller in the process
     * and every one added later.  Must be set before GTK initializes,
     * which the first-run dialog's gtk_init_check() below may do.        */
    g_setenv("GTK_OVERLAY_SCROLLING", "0", TRUE);

    /* The key is consumed here and not kept: the resolved PATH is the
     * only thing the rest of the run needs, and the ini is the record of
     * where it came from.                                              */
    gchar *db_dir  = task_app_config_get("db_dir");
    gchar *db_path = task_db_resolve_path(db_dir);
    g_free(db_dir);

    /* First-run: if the database file does not yet exist, ask the user
     * whether to open an existing file or create a fresh one — silently
     * creating an empty database when the user meant to point at an
     * existing shared file is a common foot-gun.  Needs GTK up early for
     * the dialog; skipped without a display (headless / non-interactive). */
    if (!g_file_test(db_path, G_FILE_TEST_EXISTS)) {
        gchar *expected = g_strdup(db_path);
        if (gtk_init_check(&argc, &argv) &&
            !startup_first_run(expected, &db_path)) {
            g_free(expected);
            g_free(db_path);
            return 0;                /* user closed the welcome dialog      */
        }
        g_free(expected);
    }

    GError *gerr = NULL;
    TaskDatabase *db = task_db_open(db_path, &gerr);
    if (db == NULL) {
        g_printerr("tasks: %s\n",
                   gerr != NULL ? gerr->message : "cannot open database");
        g_clear_error(&gerr);
        g_free(db_path);
        return 1;
    }

    TaskApp *app = g_new0(TaskApp, 1);
    app->db     = db;
    app->editors = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);
    task_app_init_icons_dir(app);

    /* Register every subsystem: its sidebar views and its periodic worker
     * with the shared scheduler.  All of it must happen before the first
     * window or thread exists, because those registries are unlocked (see
     * task_worker.h).  The app has to be built first — a worker
     * definition points at the app's own in-flight flag and GSource id.  */
    task_core_views_init();          /* the app's own sidebar views first  */
    task_recur_init(app);            /* recurring tasks: earliest worker   */
    task_backup_init(app);

    TaskBoot boot = { app, db_path };
    app->gtk_app = gtk_application_new("org.example.tasks",
                                       G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app->gtk_app, "activate",
                     G_CALLBACK(on_activate), &boot);
    int status = g_application_run(G_APPLICATION(app->gtk_app), argc,
                                   argv);

    g_object_unref(app->gtk_app);
    g_hash_table_destroy(app->editors);
    /* Any listener still subscribed here outlived its window, which is
     * not an error.                                                      */
    g_slist_free_full(app->changed_l, g_free);
    g_slist_free_full(app->tasks_l,   g_free);
    g_slist_free_full(app->status_l,  g_free);
    task_db_close(app->db);
    g_free(app);
    g_free(db_path);
    return status;
}
