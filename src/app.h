/* ===========================================================================
 * app.h — shared application context for Tasks
 *
 * A single TaskApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open task-editor windows, and hosts
 * the change-notification events windows and plugins subscribe to.  Companion app to
 * Notes — same design language: plain C + GTK3 + SQLite, no
 * HeaderBars, window titles "Tasks - <thing>".
 * =========================================================================== */

#ifndef TASK_APP_H
#define TASK_APP_H

#include <gtk/gtk.h>
#include "db.h"

/* Semantic version, baked in by the Makefile (-DTASK_VERSION="x.y.z").     */
#ifndef TASK_VERSION
#define TASK_VERSION "dev"
#endif

/* ---------------------------------------------------------------------------
 * TaskApp — global application state.
 *
 * Fields:
 *   gtk_app        — the GtkApplication driving the main loop.
 *   db             — open tasks database (owned; closed at shutdown).
 *   editors        — map of open editor windows keyed by task id
 *                    (gint64* keys, GtkWindow* values).  Notes action
 *                    items are ordinary tasks (the Notes plugin), so they
 *                    live in this map like everything else.
 *   library_window — the (single) library window, or NULL before startup.
 *   changed_l      — listeners for a FULL refresh (sidebar + task pane +
 *                    open editors).  For structural changes: lists
 *                    created/renamed/deleted, sync applied.  The library
 *                    window is normally one of them; a plugin may be
 *                    another.  Fire through task_app_notify_changed().
 *   tasks_l        — listeners for the LIGHTER event: the task pane only.
 *                    Editor saves and subtask/attachment edits use this —
 *                    they can never change the sidebar, and the saving
 *                    editor is itself the source of truth (reloading every
 *                    editor per autosave would also re-run the Notes CLI).
 *                    Fire through task_app_notify_tasks(), which falls
 *                    back to the full event when nothing is listening for
 *                    the light one.
 *   status_l       — listeners for a one-line event message.  Post through
 *                    task_app_status().
 *   sync_running   — TRUE while the Google Tasks sync worker is running
 *                    (main-thread flag; blocks a second concurrent sync).
 *   sync_timer     — the periodic auto-sync GSource id, or 0.
 *   backup_running — the same guard again for the optional rotating
 *                    backup (backup.h), a third worker on its own
 *                    schedule.
 *   backup_timer   — its periodic GSource id, or 0.
 *   toolbar_style  — how toolbar buttons render (icons only, text below
 *                    icons, or text only); persisted as "toolbar_style".
 *   toolbars       — every live toolbar, so a style change can be
 *                    applied to all open windows at once.  Entries
 *                    remove themselves on destroy.
 *   icons_dir      — absolute path of the local icons/ folder the
 *                    toolbar button PNGs are loaded from (owned string).
 *   db_dir         — custom directory holding tasks.db (owned string),
 *                    or NULL for the default location.  Persisted in the
 *                    ini as "db_dir"; not stored in the database itself.
 *
 * The three event lists are lists rather than single hooks because more
 * than one subscriber is now normal, and because a subscriber that
 * cannot register without displacing the previous one is not a
 * subscriber.  They start empty, and firing an empty list is a no-op —
 * which is what makes every notify safe both before the library window
 * exists and after it has gone.
 * ------------------------------------------------------------------------- */
typedef struct TaskApp {
    GtkApplication  *gtk_app;
    TaskDatabase      *db;
    GHashTable      *editors;
    GtkWidget       *library_window;
    GSList          *changed_l;          /* TaskAppListener*, in order      */
    GSList          *tasks_l;
    GSList          *status_l;
    guint            listener_next;      /* next subscription id            */
    gboolean         sync_running;
    guint            sync_timer;
    gboolean         backup_running;     /* rotating-backup worker in flight */
    guint            backup_timer;       /* its periodic GSource, or 0      */
    GtkToolbarStyle  toolbar_style;
    GPtrArray       *toolbars;
    gchar           *icons_dir;
    gchar           *db_dir;
    gboolean         db_integrity_check; /* run PRAGMA checks on startup    */
    gint             pending_fades;      /* row fade-outs in flight; the
                                          * refresh waits for the last one
                                          * (see task_rows_toggle_done)     */
} TaskApp;

/* ---------------------------------------------------------------------------
 * task_app_widget_add_css() — attach a one-off CSS snippet to a single
 * widget's style context (application priority).  The provider is owned
 * by the style context after this call.
 * ------------------------------------------------------------------------- */
void task_app_widget_add_css(GtkWidget *widget, const gchar *css_text);

/* ---------------------------------------------------------------------------
 * task_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (via task_app_exe_dir(); task_app_config_init() must have run)
 * and remember it in app->icons_dir.
 * ------------------------------------------------------------------------- */
void task_app_init_icons_dir(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_app_icon_image_sized() — build a GtkImage for icon `name` from
 * "<icons_dir>/<name>.png" (or .svg), rendered at an explicit logical
 * pixel size, HiDPI-sharp (backing pixels scale with the display).
 * Returns NULL when no loadable file exists — callers fall back to a
 * text label.
 *
 * The returned image carries `name` as the "task-icon-name" object data
 * and its rotation as "task-icon-rotation" (a GdkPixbufRotation, owned by
 * the image): it is surface-backed, so gtk_image_get_pixbuf answers NULL
 * and there is otherwise no way to ask which picture a widget is
 * currently showing.
 * ------------------------------------------------------------------------- */
GtkWidget *task_app_icon_image_sized(TaskApp *app, const gchar *name,
                                     gint size);

/* ---------------------------------------------------------------------------
 * task_app_icon_image_rotated() — as above, turned by whole quarter turns.
 *
 * `rotation` is a GdkPixbufRotation; GDK_PIXBUF_ROTATE_NONE is exactly
 * task_app_icon_image_sized.  The turn happens on the PIXBUF before the
 * surface is made, so a square icon comes back the same size, pixel-exact
 * and unresampled.  Use it where one image serves two states that differ
 * only in orientation — a horizontal list icon turned a quarter turn is a
 * columnar board icon, which is how the pane toggle dresses both of its
 * faces from menu.png alone.
 * ------------------------------------------------------------------------- */
GtkWidget *task_app_icon_image_rotated(TaskApp *app, const gchar *name,
                                       gint size,
                                       GdkPixbufRotation rotation);

/* ---------------------------------------------------------------------------
 * task_app_tool_item_new() — create a toolbar button that honors the
 * app-wide toolbar style: `icon_name` names a local icon file (see
 * task_app_icon_image_sized), `fallback_markup` is Pango markup rendered
 * as the "icon" when that file is missing (NULL falls back to the plain
 * label).  The label shows in text/both modes.
 * ------------------------------------------------------------------------- */
GtkToolItem *task_app_tool_item_new(TaskApp *app, const gchar *icon_name,
                                    const gchar *fallback_markup,
                                    const gchar *label,
                                    const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * task_app_register_toolbar() — apply the current style to `toolbar`, keep
 * it updated when the style changes, and offer the icons/both/text radio
 * menu on right-click.  The toolbar unregisters itself when destroyed.
 * ------------------------------------------------------------------------- */
void task_app_register_toolbar(TaskApp *app, GtkWidget *toolbar);

/* ---------------------------------------------------------------------------
 * task_app_set_toolbar_style() — change the style on every live toolbar
 * and persist the choice ("toolbar_style" = icons|both|text).
 * ------------------------------------------------------------------------- */
void task_app_set_toolbar_style(TaskApp *app, GtkToolbarStyle style);

/* task_app_load_toolbar_style() — read the persisted style into the app
 * context (default: icons only).                                           */
void task_app_load_toolbar_style(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  Safe to call from anywhere on the main
 * thread: a no-op until something is listening for status events.
 * ------------------------------------------------------------------------- */
void task_app_status(TaskApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * Change notification.
 *
 * task_app_notify_changed() — FULL refresh: sidebar + task pane + open
 * editors.  For structural changes (a list created, renamed or deleted;
 * a sync applied).
 *
 * task_app_notify_tasks() — the task pane only, for changes that cannot
 * touch the sidebar.  Falls back to the full event when nothing is
 * listening for this one, so a caller never has to ask which listeners
 * exist.
 *
 * Both are safe with no listeners at all, which is what makes them safe
 * before the library window is built and after it has gone.  Main thread
 * only — a worker marshals through g_idle_add first.
 * ------------------------------------------------------------------------- */
void task_app_notify_changed(TaskApp *app);
void task_app_notify_tasks(TaskApp *app);

/* ---------------------------------------------------------------------------
 * Subscribing to those events.
 *
 * Each listen call returns a subscription id for task_app_unlisten();
 * ids are never reused.  Listeners fire in registration order.
 *
 * A listener MUST be removed before the thing it refreshes is destroyed.
 * The library window does exactly this, and the ordering is load-bearing
 * rather than tidy: a closing editor's final save fires the task event,
 * which would otherwise reach a window mid-teardown.
 *
 * Removing a listener from inside its own callback is safe; adding one
 * during a fire is not observed until the next fire.
 * ------------------------------------------------------------------------- */
typedef void (*TaskAppNotifyFn)(TaskApp *app, gpointer user_data);
typedef void (*TaskAppStatusFn)(TaskApp *app, const gchar *message,
                                gpointer user_data);

guint task_app_listen_changed(TaskApp *app, TaskAppNotifyFn fn,
                              gpointer user_data);
guint task_app_listen_tasks(TaskApp *app, TaskAppNotifyFn fn,
                            gpointer user_data);
guint task_app_listen_status(TaskApp *app, TaskAppStatusFn fn,
                             gpointer user_data);
void  task_app_unlisten(TaskApp *app, guint id);

/* ---------------------------------------------------------------------------
 * task_app_switch_database() — move tasks.db to `new_dir` (or back to
 * the default location when `new_dir` is NULL): closes all editors, copies
 * the database to the new home (if target folder has no existing db),
 * reopens, removes the old file, updates app->db_dir + config, and fires
 * notify_changed.  If target already has a db the user chooses whether to
 * use it or overwrite it; either way the old file is removed on success.
 * Returns TRUE on success; on failure the previous database is still open.
 * ------------------------------------------------------------------------- */
gboolean task_app_switch_database(TaskApp *app, const gchar *new_dir);

/* ---------------------------------------------------------------------------
 * task_app_notice() — run a modal OK message dialog and destroy it.
 * ------------------------------------------------------------------------- */
void task_app_notice(GtkWindow *parent, GtkMessageType type,
                     const gchar *title, const gchar *fmt, ...)
                   G_GNUC_PRINTF(4, 5);

/* ---------------------------------------------------------------------------
 * task_app_confirm() — run a modal Yes/No question dialog; TRUE on Yes.
 * ------------------------------------------------------------------------- */
gboolean task_app_confirm(GtkWindow *parent, const gchar *title,
                          const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

/* ---------------------------------------------------------------------------
 * Config — same model as Notes: tasks.ini next to the binary
 * (portable mode) falling back to ~/.config/tasks/tasks.ini when that
 * directory is unwritable.  Loaded ONCE into memory; written through on
 * every change.  Keys used (see tasks.ini.defaults):
 *   sync       — google_sync_enabled, google_client_id,
 *                google_client_secret, gtasks_refresh_token,
 *                sync_interval_min, sync_toolbar_button
 *   Notes — notes_sync, notes_cli, notes_embed_list,
 *                notes_sync_interval_min, notes_meta_row
 *   database   — db_dir (custom directory for tasks.db; absent = default
 *                location), db_integrity_check, backup_enabled,
 *                backup_dir, backup_interval_min, backup_keep
 *   plugins    — plugin_dir (folder to load plugins from; absent =
 *                beside the binary if that exists, else
 *                <data>/tasks/plugins), and one <id>_plugin_enabled per
 *                plugin found.  A plugin's OWN keys are namespaced by
 *                its id (see task_app_config_get_ns).
 *   UI         — toolbar_style, bold_task_titles, native_menubar,
 *                show_completed, sidebar_visible, compact_layout,
 *                weekly_forecast, due_today_show_overdue,
 *                task_list_manual_sort, kanban_view,
 *                col_done_visible, col_status_visible, col_due_visible,
 *                col_completed_visible, win_w, win_h
 *   per-view   — manual_order_list_<id>, manual_order_all,
 *                manual_order_pinned, manual_order_today,
 *                manual_order_bn_actions (task-pane drag-reorder), and
 *                kanban_order_* under the same five names (the board's
 *                own card order — a separate family on purpose, see
 *                kanban_order_key)
 * ------------------------------------------------------------------------- */
void      task_app_config_init(const gchar *argv0);
gchar    *task_app_config_get(const gchar *key);         /* NULL when unset */
void      task_app_config_set(const gchar *key, const gchar *value);

/* task_app_config_get_bool() — read a 0/1 setting; `def` when unset.  The
 * app only ever writes "0"/"1", so any other stored value reads as "1".    */
gboolean  task_app_config_get_bool(const gchar *key, gboolean def);

/* ---------------------------------------------------------------------------
 * Namespaced config — the same store, with the key prefixed by a
 * feature's id: task_app_config_get_ns("notes", "sync") reads
 * "notes_sync", exactly the key that is already in users' ini files.
 *
 * There is ONE ini group ("[tasks]", part of the file format), so a
 * feature's key space is carved out by name rather than by section.  That
 * is not a workaround: prefixes are already the convention here
 * (google_*, notes_*, backup_*), so this only makes the existing rule
 * something a caller cannot get wrong, and lets an integration that does
 * not know the app's key inventory still avoid colliding with it.
 *
 * `ns` must not be NULL; use the plain accessors above for core keys.
 * ------------------------------------------------------------------------- */
gchar    *task_app_config_get_ns(const gchar *ns, const gchar *key);
void      task_app_config_set_ns(const gchar *ns, const gchar *key,
                                 const gchar *value);
gboolean  task_app_config_get_bool_ns(const gchar *ns, const gchar *key,
                                      gboolean def);

/* task_app_exe_dir() — the directory holding the binary, resolved once by
 * task_app_config_init().  Borrowed string; do not free.                   */
const gchar *task_app_exe_dir(void);

/* ---------------------------------------------------------------------------
 * Date helpers shared by the two windows and the sync engine.
 * ------------------------------------------------------------------------- */

/* task_day_bounds() — local midnight bounds of "today + offset_days":
 * lo = that day's local midnight, hi = the next day's.                     */
void task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi);

/* task_due_format() — "" for no date, else e.g. "Jul 13, 2026".  Returns a
 * new string (g_free it).                                                  */
gchar *task_due_format(gint64 due);

/* task_due_format_iso() — "" for no date, else the canonical "YYYY-MM-DD"
 * spelling (local calendar day).  Returns a new string (g_free it).        */
gchar *task_due_format_iso(gint64 due);

/* task_due_color() — urgency tint for a due timestamp: overdue red, today
 * gold, ahead green (the Notes action-item palette), or NULL for no
 * tint (due == 0).  Static string; do not free.                            */
const gchar *task_due_color(gint64 due);

/* task_due_parse() — parse "YYYY-MM-DD" (also "M/D/YY[YY]") into a local-
 * midnight unix timestamp; 0 when the text is empty/unparseable.           */
gint64 task_due_parse(const gchar *text);

/* task_due_from_ymd() — validated year/month/day → local-midnight unix
 * timestamp; 0 when the fields are out of range.                           */
gint64 task_due_from_ymd(gint y, gint m, gint d);

#endif /* TASK_APP_H */
