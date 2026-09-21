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
 * list (pinned flag / all / due today), registered through task_view.h.
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

#endif /* TASK_LIBRARY_WINDOW_H */
