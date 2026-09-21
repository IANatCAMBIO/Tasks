/* ===========================================================================
 * app.h — shared application context for Tasks
 *
 * A single TaskApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open task-editor windows, and hosts
 * the change-notification events the windows subscribe to.  Companion app
 * to Notes — same design language: plain C + GTK3 + SQLite, no
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
 *                    (gint64* keys, GtkWindow* values).
 *   library_window — the (single) library window, or NULL before startup.
 *   changed_l      — listeners for a FULL refresh (sidebar + task pane +
 *                    open editors).  For structural changes: lists
 *                    created/renamed/deleted.  The library window is
 *                    normally one of them.  Fire through
 *                    task_app_notify_changed().
 *   tasks_l        — listeners for the LIGHTER event: the task pane only.
 *                    Editor saves and subtask/attachment edits use this —
 *                    they can never change the sidebar, and the saving
 *                    editor is itself the source of truth.
 *                    Fire through task_app_notify_tasks(), which falls
 *                    back to the full event when nothing is listening for
 *                    the light one.
 *   status_l       — listeners for a one-line event message.  Post through
 *                    task_app_status().
 *   backup_running — TRUE while the optional rotating backup's worker
 *                    (backup.h) is in flight (main-thread flag; blocks a
 *                    second concurrent pass).
 *   backup_timer   — its periodic GSource id, or 0.
 *   icons_dir      — absolute path of the local icons/ folder the
 *                    toolbar button PNGs are loaded from (owned string).
 *
 * There is no db_dir member: where the database is kept is answered by
 * the ini key of that name, read ONCE at startup to resolve the path,
 * and written only by File → Open Database File… choosing "Set as
 * Default".  A copy of it on the app was a second answer to the same
 * question that nothing ever read back — it existed for the Settings
 * control that MOVED the database, which is gone (gotcha 14).
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
    gboolean         backup_running;     /* rotating-backup worker in flight */
    guint            backup_timer;       /* its periodic GSource, or 0      */
    gchar           *icons_dir;
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
 * task_app_tool_item_new() — create a toolbar button: `icon_name` names a
 * local icon file (see task_app_icon_image_sized), `fallback_markup` is
 * Pango markup rendered as the "icon" when that file is missing (NULL
 * falls back to the plain label).  Toolbars are ICONS-ONLY, so `label`
 * never shows on the button itself — it names the item in the toolbar's
 * overflow menu and to accessibility; `tooltip` is what a user reads.
 * ------------------------------------------------------------------------- */
GtkToolItem *task_app_tool_item_new(TaskApp *app, const gchar *icon_name,
                                    const gchar *fallback_markup,
                                    const gchar *label,
                                    const gchar *tooltip);

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
 * task_app_menu_popup() — show `model` as a context menu at the pointer.
 *   attach — a LONG-LIVED widget the menu is attached to and resolves its
 *            actions through (the library window).  Never the widget that
 *            was clicked: an attached menu dies with its widget, and a
 *            card or a row is often destroyed by the very action chosen.
 *   model  — the menu; ownership passes to this call.
 *   event  — the button press, for placement.
 * The menu destroys itself once it closes ("selection-done" fires AFTER
 * the chosen item's action, so the destroy never races it).
 * ------------------------------------------------------------------------- */
void task_app_menu_popup(GtkWidget *attach, GMenuModel *model,
                         GdkEventButton *event);

/* ---------------------------------------------------------------------------
 * task_app_menu_section_end() — close the section being built into `menu`
 * and start a fresh one: appends *section to `menu` (as a separated
 * section), drops the reference, and replaces *section with a new empty
 * GMenu.  Close the last section the same way and unref the empty one
 * left behind.
 * ------------------------------------------------------------------------- */
void task_app_menu_section_end(GMenu *menu, GMenu **section);

/* ---------------------------------------------------------------------------
 * Config — tasks.ini lives in the app's SHARED DIRECTORY,
 * task_db_default_dir() (<user data dir>/tasks), with the database.
 * Resolved ONCE, in three steps: that file if it EXISTS;
 * else tasks.ini NEXT TO THE BINARY if it EXISTS (portable mode, so a
 * source tree or a USB copy keeps the ini it came with); else CREATED in
 * the shared directory.  Both tests are for EXISTENCE — a writability
 * test is what used to let a development tree outrank the user's real
 * settings.  There is no ~/.config/tasks fallback and no migration to
 * one.  Loaded ONCE into memory; written through on every change.
 * Keys used (see tasks.ini.defaults):
 *   database   — db_dir (the directory holding tasks.db; absent = the
 *                default location.  Written only by File → Open
 *                Database File…; there is no Settings control for it),
 *                backup_enabled,
 *                backup_dir, backup_interval_min, backup_keep
 *   UI         — bold_task_titles,
 *                show_completed, sidebar_visible, compact_layout,
 *                due_today_show_overdue,
 *                task_list_manual_sort, kanban_view,
 *                col_done_visible, col_status_visible, col_due_visible,
 *                col_completed_visible, win_w, win_h
 *   per-view   — manual_order_list_<id>, manual_order_group_<id>,
 *                manual_order_all, manual_order_pinned,
 *                manual_order_today (task-pane drag-reorder), and
 *                kanban_order_* under the same five
 *                names (the board's own card order — a separate family
 *                on purpose, see kanban_order_key).  A group's aggregate
 *                orders separately from the lists under it: they are
 *                different panes of tasks.
 * ------------------------------------------------------------------------- */
void      task_app_config_init(const gchar *argv0);
gchar    *task_app_config_get(const gchar *key);         /* NULL when unset */
void      task_app_config_set(const gchar *key, const gchar *value);

/* task_app_config_get_bool() — read a 0/1 setting; `def` when unset.  The
 * app only ever writes "0"/"1", so any other stored value reads as "1".    */
gboolean  task_app_config_get_bool(const gchar *key, gboolean def);

/* task_app_exe_dir() — the directory holding the binary, resolved once by
 * task_app_config_init().  Borrowed string; do not free.                   */
const gchar *task_app_exe_dir(void);

/* ---------------------------------------------------------------------------
 * Date helpers shared by the windows and the recurrence pass.
 *
 * USE THESE TWO RATHER THAN GLib's "_local" CONSTRUCTORS.  Every
 * g_date_time_new_now_local() / _new_from_unix_local() / _new_local()
 * resolves the local timezone from scratch, and that resolution IS the
 * cost: 7318 ns against 175 ns to build a GDateTime once a GTimeZone is in
 * hand (measured, GLib 2.88.2).  These helpers run per row and per draw,
 * where that was 13.5 ms of a 500-row refresh.
 * ------------------------------------------------------------------------- */

/* task_local_tz() — the local timezone, CACHED and re-resolved when the
 * local day rolls over.  Borrowed: do NOT unref it.  Pass it to
 * g_date_time_new_now(tz) and g_date_time_new(tz, …), which are the
 * timezone-taking forms of the two "_local" constructors.
 *
 * DST is unaffected — a GTimeZone carries the zone's full transition
 * table.  What the cache defers is a change to the SYSTEM's zone (travel,
 * a TZ edit), which is noticed at the next local midnight.               */
GTimeZone *task_local_tz(void);

/* task_local_dt() — `unix_ts` as a LOCAL-time GDateTime, or NULL.  Free
 * with g_date_time_unref.
 *
 * GLib has no g_date_time_new_from_unix(tz, t), which is why this goes
 * through _from_unix_utc() + g_date_time_to_timezone(): 160 ns against
 * 8316 for _new_from_unix_local().  One spelling of that detour.        */
GDateTime *task_local_dt(gint64 unix_ts);

/* task_day_bounds() — local midnight bounds of "today + offset_days":
 * lo = that day's local midnight, hi = the next day's.                     */
void task_day_bounds(gint offset_days, gint64 *lo, gint64 *hi);

/* task_due_format() — "" for no date, else e.g. "Jul 13, 2026".  Returns a
 * new string (g_free it).                                                  */
gchar *task_due_format(gint64 due);

/* ---------------------------------------------------------------------------
 * task_clock_format() — "8:00 AM" for minutes past local midnight.
 * Returns a new string (g_free); "" if the clock cannot be built.  Out of
 * range clamps into the day rather than failing.  It is here rather than
 * in recur.c because both the due date's time and the recurrence
 * schedule's render one, and gotcha 23 is not worth learning twice.
 * ------------------------------------------------------------------------- */
gchar *task_clock_format(gint minutes);

/* ---------------------------------------------------------------------------
 * task_due_instant() — the MOMENT a task is due: its date plus its time of
 * day.  0 when `due` is 0 (no date means no moment).
 *
 * `due` is DATE-ONLY on disk — always local midnight, because Google's due
 * is date-only and every day-bucketing view compares against it — so this
 * is the one place that folds the two columns back together.  Use it for
 * SORTING and for anything that means "when is this actually due"; use
 * `due` alone for anything that buckets by calendar day.
 * ------------------------------------------------------------------------- */
gint64 task_due_instant(gint64 due, gint due_time);

/* ---------------------------------------------------------------------------
 * task_due_format_at() — "Jul 13, 2026", plus " 2:30 PM" when `due_time`
 * is NOT the 08:00 default.  Returns a new string (g_free); "" for no date.
 *
 * The default is left unsaid on purpose: every task has a due time now, so
 * printing it always would add a clock to every row in the list and say
 * nothing — the same reason the editor's lead spin shows "5 days" rather
 * than "7200 minutes".  A time somebody actually chose is the thing worth
 * showing, and it stands out precisely because its neighbours are bare.
 * ------------------------------------------------------------------------- */
gchar *task_due_format_at(gint64 due, gint due_time);

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
