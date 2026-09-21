/* ===========================================================================
 * app.h — shared application context for Tasks
 *
 * A single TaskApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open task-editor windows, and hosts
 * the change-notification events the windows subscribe to.  Companion app
 * to Notes — same design language: plain C + GTK4 + SQLite, no
 * HeaderBars, window titles "Tasks - <thing>".
 *
 * The GTK4 helpers below (dialogs, the icon theme, tool items, tooltips,
 * popups, the list-row press and double-click helpers) are ports of the
 * same-named on_app_* helpers in Notes (~/salt_development/notes/src/app.c),
 * where each one's GTK4 measurement is recorded (its D-numbers).
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
 * task_app_css_install() — add one stylesheet to the DISPLAY at application
 * priority.  Each module installs its own once (a static guard at the call
 * site); rules go on `task-*` classes, never on a per-widget provider,
 * which GTK4 deprecated and which could not follow a theme change anyway.
 * A colour name the theme does not define is NOT an error: it renders
 * TRANSPARENT.  `@theme_bg_color` and `shade()` are both still defined by
 * GTK 4.22's Default theme (Notes measured it).
 * ------------------------------------------------------------------------- */
void task_app_css_install(const gchar *css);

/* ---------------------------------------------------------------------------
 * task_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (via task_app_exe_dir(); task_app_config_init() must have run)
 * and remember it in app->icons_dir.
 * ------------------------------------------------------------------------- */
void task_app_init_icons_dir(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_app_icon_image_sized() — a GtkImage showing icon `name`, at a
 * LOGICAL pixel size.  The icons are the PNGs in the app-local icons/
 * folder, which startup adds to the icon theme's search path: GTK picks
 * them up by basename as unthemed icons, loads them at the display's
 * scale factor (sharp on HiDPI), caches them, and draws at the logical
 * size.  Swapping a PNG in icons/ still re-themes a button.
 *
 * There is no rotated variant any more: the pane toggle's board face is
 * its own file, icons/board.png, derived ONCE from menu.png by a quarter
 * turn (the same rule as add.png from remove.png).  The returned image
 * carries `name` as the "task-icon-name" object data, so a state-swapping
 * button can be asked which picture it shows.
 *   name — icon file basename without extension (e.g. "add").
 *   size — logical pixel size to draw at.
 * Returns a new GtkImage, or NULL if the theme has no such icon — callers
 * fall back to a text label in that case.
 * ------------------------------------------------------------------------- */
GtkWidget *task_app_icon_image_sized(TaskApp *app, const gchar *name,
                                     gint size);

/* ---------------------------------------------------------------------------
 * task_app_icon_paintable() — the same icon as a paintable, for uses that
 * need one rather than a widget (the About dialog's logo).  Rendered at
 * the display's scale factor.  Returns a new paintable (g_object_unref
 * it), or NULL if the theme has no such icon.
 * ------------------------------------------------------------------------- */
GdkPaintable *task_app_icon_paintable(TaskApp *app, const gchar *name,
                                      gint size);

/* ---------------------------------------------------------------------------
 * task_app_tool_item_new() — create an icon toolbar button: a flat
 * GtkButton (or GtkToggleButton) whose child is the icon widget.  GTK4
 * has no GtkToolbar; a toolbar is a GtkBox with the "toolbar" style class
 * holding these.  Buttons never take focus on click.
 *   toggle          — TRUE for a GtkToggleButton, FALSE for a GtkButton.
 *   icon_name       — local icon file (see task_app_icon_image_sized), or
 *                     NULL for none.
 *   fallback_markup — Pango markup rendered as the "icon" when the file is
 *                     missing; NULL falls back to the plain label.
 *   label           — the accessible name; also the icon stand-in when
 *                     both the file and fallback_markup are absent.
 *   tooltip         — hover help text (through task_app_set_tooltip).
 * Returns the new button.
 * ------------------------------------------------------------------------- */
GtkWidget *task_app_tool_item_new(TaskApp *app, gboolean toggle,
                                  const gchar *icon_name,
                                  const gchar *fallback_markup,
                                  const gchar *label,
                                  const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * task_app_tool_item_set_icon() — re-point an existing toolbar button at a
 * different icon, for a button whose image names the ACTION it offers
 * (the completed, sort and pane toggles).  Same icon-file-else-markup
 * rule as task_app_tool_item_new; label and tooltip untouched.
 * ------------------------------------------------------------------------- */
void task_app_tool_item_set_icon(TaskApp *app, GtkWidget *button,
                                 const gchar *icon_name,
                                 const gchar *fallback_markup);

/* ---------------------------------------------------------------------------
 * task_app_set_tooltip() — THE way to give a widget a tooltip (NULL
 * removes it).  A plain gtk_widget_set_tooltip_text is broken on macOS:
 * GTK keeps ONE tooltip popup surface and the macOS backend's layer does
 * not follow its resize while hidden (Notes D32 — the next tooltip comes
 * out cut off), so this refuses a tooltip asked for within 550 ms of the
 * previous one hiding and asks again after; and it shows NO tooltip in a
 * window that is not the active one (Notes D35 — the popup would raise
 * that window).  A no-op cost on other backends.
 * ------------------------------------------------------------------------- */
void task_app_set_tooltip(GtkWidget *widget, const gchar *text);

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
 * task_app_notice() — show a modal OK message (a GtkAlertDialog) over
 * `parent` and return at once.  Fire-and-forget: GTK4 has no blocking
 * dialogs, and no caller ever needed the dismissal.
 *   parent — transient parent window, or NULL.
 *   title  — the message's heading, or NULL for the plain message only.
 *   fmt    — printf-style message.
 * ------------------------------------------------------------------------- */
void task_app_notice(GtkWindow *parent, const gchar *title,
                     const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

/* task_app_confirm()'s completion: `yes` is TRUE when the user chose Yes;
 * closing the dialog any other way (No, Escape, the close button) is FALSE.*/
typedef void (*TaskConfirmFn)(gboolean yes, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_app_confirm() — ask a modal Yes/No question (a GtkAlertDialog) and
 * hand the answer to `done`.  ASYNCHRONOUS: returns as soon as the dialog
 * is up; the rest of the caller's work lives in `done`, and anything it
 * needs travels in `user_data` (a small struct the callback frees, or a
 * ref'd object).  `message` is plain text, formatted by the caller.
 *   parent  — transient parent window, or NULL.
 *   title   — the question's heading (shown bold above the message).
 *   message — the question.
 *   done    — completion callback (always called, once).
 * ------------------------------------------------------------------------- */
void task_app_confirm(GtkWindow *parent, const gchar *title,
                      const gchar *message, TaskConfirmFn done,
                      gpointer user_data);

/* What task_app_pick_path() asks for.                                      */
typedef enum {
    TASK_PICK_OPEN,                  /* an existing file                    */
    TASK_PICK_SAVE,                  /* a file name to write                */
    TASK_PICK_FOLDER,                /* an existing directory               */
} TaskPickKind;

/* task_app_pick_path()'s completion: `path` is the chosen filesystem path
 * (OWNED by the callback: g_free it) or NULL when the chooser was
 * cancelled.                                                                */
typedef void (*TaskPickFn)(gchar *path, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_app_pick_path() — run a modal file chooser (a GtkFileDialog) and
 * hand the selection to `done`.  ASYNCHRONOUS, like task_app_confirm.
 *   parent         — transient parent window, or NULL.
 *   title          — dialog title.
 *   kind           — what to pick (see TaskPickKind).
 *   accept_label   — accept-button label (e.g. "_Open").
 *   filter_name    — display name of a single file filter, or NULL for
 *                    no filter (filter_pattern is ignored when NULL).
 *   filter_pattern — glob the filter matches (e.g. "*.db").
 *   start_dir      — folder the chooser opens in, or NULL for GTK's
 *                    choice.
 *   done           — completion callback (always called, once).
 * ------------------------------------------------------------------------- */
void task_app_pick_path(GtkWindow *parent, const gchar *title,
                        TaskPickKind kind, const gchar *accept_label,
                        const gchar *filter_name, const gchar *filter_pattern,
                        const gchar *start_dir,
                        TaskPickFn done, gpointer user_data);

/* task_app_dialog_new()'s completion: `accepted` is TRUE for the accept
 * button (or Enter on the default widget), FALSE for Cancel, Escape or the
 * close button.  `dialog` is still alive, so the callback can read the
 * widgets it stashed on it as object data; it is destroyed right after.   */
typedef void (*TaskDialogFn)(gboolean accepted, GtkWindow *dialog,
                             gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_app_dialog_new() — build and present a modal dialog with custom
 * content and a Cancel / <accept> pair: the list dialog, the group-name
 * dialog, the calendar.  GtkDialog is deprecated; this is the one
 * scaffold every custom dialog goes through.  Escape and the close button
 * mean Cancel; Enter activates the dialog's default widget, which is the
 * accept button unless the caller sets another.
 *   parent       — transient parent window.
 *   title        — window title ("Tasks - New List").
 *   content      — the widget above the buttons (the dialog takes it).
 *   accept_label — the accept button's label, with a mnemonic ("_Create").
 *   done         — the completion (see TaskDialogFn).
 * Returns the dialog window, already presented, so the caller can stash
 * its widgets on it as object data before the callback needs them.
 * ------------------------------------------------------------------------- */
GtkWindow *task_app_dialog_new(GtkWindow *parent, const gchar *title,
                               GtkWidget *content, const gchar *accept_label,
                               TaskDialogFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_app_menu_popup() — pop up a one-shot context menu built from a menu
 * model, at a point in a widget.  THE transient-popup scaffold for every
 * right-click menu in the app: a GtkPopoverMenu parented to the WINDOW's
 * child box (never to `attach`: a list view or a card cannot take one,
 * Notes D14), pointing at (x, y) translated into that box, that unparents
 * and drops itself from an idle once closed.  Actions resolve through the
 * window either way.
 *   attach — the widget the press landed in (any widget in a window).
 *   model  — the items; OWNERSHIP IS TAKEN (the popover keeps its own ref).
 *   x, y   — the press position in `attach`'s coordinates (what a
 *            GtkGestureClick "pressed" handler receives).
 * ------------------------------------------------------------------------- */
void task_app_menu_popup(GtkWidget *attach, GMenuModel *model,
                         gdouble x, gdouble y);

/* ---------------------------------------------------------------------------
 * task_app_double_click_watch() — call `cb` when `widget` is double-clicked
 * with the primary button, counted by THIS app from the time and distance
 * between two presses, never by GtkGestureClick's n_press: the macOS
 * backend can hand GTK a buttonless motion inside a quick first click,
 * which resets every click gesture in the window (Notes D34).  A
 * capture-phase gesture on `widget`; on the second press it CLAIMS the
 * sequence so GTK's own activation cannot fire a second time.
 *   widget — the widget to watch (a list row or cell, a card).
 *   cb     — called as cb(widget, data) on the double-click.
 * ------------------------------------------------------------------------- */
typedef void (*TaskDoubleClickFn)(GtkWidget *widget, gpointer data);
void task_app_double_click_watch(GtkWidget *widget, TaskDoubleClickFn cb,
                                 gpointer data);

/* ---------------------------------------------------------------------------
 * task_app_select_on_press() — make a list row select on the PRESS, not on
 * the release (Notes D37): GTK4's row widget selects from its click
 * gesture's "released", so a row looked unselected for the length of the
 * click.  A capture-phase primary-button gesture on `widget` runs the
 * view's own "list.select-item" action at press time with the modifiers
 * GTK would read (Shift extends, Control — Command on macOS — toggles),
 * except an unmodified press on a row that is ALREADY selected, which is
 * left alone so a drag can carry a multi-selection; the release CLAIMS
 * the sequence (cancelling the row's own gesture) and collapses to the
 * pressed row when the deferred press's drag never came.  Anything in
 * `widget` with a gesture of its own (a check button, an expander's
 * arrow) must NOT be inside it — the claim cancels every gesture below.
 *   widget — the cell or row child to watch.
 *   item   — its GtkListItem (the position is read at press time).
 * ------------------------------------------------------------------------- */
void task_app_select_on_press(GtkWidget *widget, GtkListItem *item);

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
