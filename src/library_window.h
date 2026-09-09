/* ===========================================================================
 * library_window.h — the main Tasks window
 *
 * Layout (Notes design language: plain GtkWindow, one unified
 * toolbar, sidebar | content pane, bottom status bar):
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ menubar (File / Help)                                        │
 *   │ toolbar: Sidebar │ New Task  Delete Task  Sync  Show/Hide ✓  │
 *   ├───────────────┬──────────────────────────────────────────────┤
 *   │ Pinned Tasks  │  ✓ │ Task (tall rows: title, notes preview,  │
 *   │ All Tasks     │    │ attachments, subtasks) │ Due │ Pinned   │
 *   │ Due Today     │                                              │
 *   │ Wkly Forecast │                                              │
 *   │ ── Lists ──   │                                              │
 *   │ <the lists>   │                                              │
 *   │       + ✎ −   │  (sidebar mini bar: new/edit/delete list)    │
 *   ├───────────────┴──────────────────────────────────────────────┤
 *   │ selection info                          latest event message │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * The sidebar's top rows are VIRTUAL lists — aggregates over every real
 * list (pinned flag / all / due today), plus whatever the loaded PLUGINS
 * contribute to the view registry: Overdue and the Weekly Forecast are
 * both plugin views and are present exactly while their plugin is.
 * Tasks cannot be created inside any of them; New Task needs a real list
 * selected.
 * =========================================================================== */

#ifndef TASK_LIBRARY_WINDOW_H
#define TASK_LIBRARY_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * task_library_window_new() — build and show the library window; it
 * subscribes to the three TaskApp events (see app.h) and stores itself
 * in app->library_window.  Its subscriptions come down in its destroy
 * handler, BEFORE the open editors are closed.
 * ------------------------------------------------------------------------- */
GtkWidget *task_library_window_new(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_library_apply_kanban_shadow() — show or hide the Kanban cards'
 * drop shadow, live, on the board currently on screen.  Driven by the
 * "kanban_shadow" setting (default ON) from Settings -> Appearance; the
 * caller writes the key, this applies it.  A no-op when no library window
 * exists.
 *
 * It is needed because a plain refresh would NOT do it: refresh_kanban
 * skips the rebuild while the same cards are showing, so the setting
 * would look inert until the board changed for another reason.  The
 * shadow's whole cost is its blur, so turning it off restores the
 * pre-shadow paint time exactly (see gotcha 30).
 * ------------------------------------------------------------------------- */
void task_library_apply_kanban_shadow(TaskApp *app, gboolean on);

/* ---------------------------------------------------------------------------
 * task_library_apply_native_menubar() — move the library menu into (or out
 * of) the native macOS menu bar.  A no-op unless built with HAVE_GTKOSX
 * (gtk-mac-integration-gtk3).  Driven by the "native_menubar" setting:
 * applied at startup by main() and live from the Settings window.
 * ------------------------------------------------------------------------- */
void task_library_apply_native_menubar(TaskApp *app, gboolean native);

/* ---------------------------------------------------------------------------
 * task_library_rebuild_chrome() — put the window's CONTRIBUTED chrome back
 * in step with the plugin registries: the toolbar buttons behind their
 * divider, the items inside File and View, and the top-level menus a
 * plugin asked for.  A no-op when no library window is open.
 *
 * Called by the plugin loader when a plugin is switched on or off, and by
 * nobody else — it is NOT part of a full refresh.  A refresh happens on
 * every structural change to the tasks, and destroying menu items on that
 * path would take a menu apart while it was open, for no gain: the
 * registries only ever change when a plugin does.
 *
 * This is what makes the Settings checkbox honest in BOTH directions.  A
 * plugin switched on gets its button and its menu at once instead of at
 * the next launch; a plugin switched off loses them — its widgets would
 * otherwise stay on the toolbar still wired to the callback it
 * registered, and since a disabled plugin is never unmapped (see
 * plugin_loader.h) that callback goes on working.
 * ------------------------------------------------------------------------- */
void task_library_rebuild_chrome(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_library_scroll_keep() — capture a scrolled window's position and
 * restore it once the main loop settles.
 *
 * Call BEFORE clearing the model underneath it: clearing a store zeroes
 * the scrollbar, so there is nothing left to read afterwards.  Exposed
 * because a panel plugin rebuilding its own stores needs exactly this and
 * would otherwise rediscover the problem.
 * ------------------------------------------------------------------------- */
void task_library_scroll_keep(GtkWidget *scrolled_window);

/* ---------------------------------------------------------------------------
 * task_library_set_location() — set the status bar's LEFT label, the one
 * saying where you are and how much is here ("All Tasks - 12 tasks").
 *
 * Distinct from task_app_status(), which posts a transient event message
 * on the RIGHT and fades.  A panel view owns its pane, so it owns this
 * line too — the core sets it for the views it renders itself and cannot
 * know what a panel wants to say.  Plain text, not markup.
 * ------------------------------------------------------------------------- */
void task_library_set_location(TaskApp *app, const gchar *text);

#endif /* TASK_LIBRARY_WINDOW_H */
