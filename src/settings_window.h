/* ===========================================================================
 * settings_window.h — the Tasks settings window
 *
 * One singleton window (File → Settings…), in the Notes settings
 * style: plain GtkWindow, bold section headings, write-through controls
 * (every change lands in the ini immediately — no OK/Apply buttons).
 *
 * Sections:
 *   Google Tasks Sync — master enable switch, Sign In / Sign Out with
 *     the session state, auto-sync interval.  Sign in once, stay signed
 *     in: the browser flow stores a refresh token in the ini (see
 *     oauth.h); Sign Out removes it.  The OAuth client itself is not
 *     entered here — it comes from the client-secret JSON file, the
 *     legacy ini keys, or the baked-in build default (oauth.h).
 *   Notes — the action-items integration switch and CLI path.
 *   Appearance — toolbar style, bold task titles, native macOS menubar.
 *   Database — where the SQLite file lives (informational).
 * =========================================================================== */

#ifndef TASK_SETTINGS_WINDOW_H
#define TASK_SETTINGS_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * task_settings_window_open() — show (or raise) the settings window.
 *   app     — the application context.
 *   parent  — transient parent (the library window).
 *   db_path — the database path shown in the Database section and used
 *             to restart the auto-sync timer when the interval changes.
 * ------------------------------------------------------------------------- */
void task_settings_window_open(TaskApp *app, GtkWindow *parent,
                               const gchar *db_path);

#endif /* TASK_SETTINGS_WINDOW_H */
