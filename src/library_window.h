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
 * The sidebar's top four rows are VIRTUAL lists — aggregates over every
 * real list (pinned flag / all / due today / the current week, Sunday
 * through Saturday, as seven day sections; hidden entirely while the
 * "weekly_forecast" setting is off).  Tasks cannot be created inside
 * them; New Task needs a real list selected.
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
 * task_library_apply_native_menubar() — move the library menu into (or out
 * of) the native macOS menu bar.  A no-op unless built with HAVE_GTKOSX
 * (gtk-mac-integration-gtk3).  Driven by the "native_menubar" setting:
 * applied at startup by main() and live from the Settings window.
 * ------------------------------------------------------------------------- */
void task_library_apply_native_menubar(TaskApp *app, gboolean native);

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
