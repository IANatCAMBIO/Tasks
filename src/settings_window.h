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

/* ---------------------------------------------------------------------------
 * Contributed settings sections.
 *
 * The Settings window is ONE scrolling column of sections, each a bold
 * heading followed by its controls.  A registered section builder is
 * called with that column and packs into it exactly as the built-in
 * sections do — there is no page, tab or stack to fit into, which is
 * what keeps this cheap for a contributor.
 *
 * Registered sections are built AFTER the app's own, in registration
 * order, so the window reads app-settings-then-extensions.
 *
 * `window` is the settings window, for parenting a dialog.  The builder
 * must not keep either widget: the window is destroyed and rebuilt every
 * time Settings is opened.  Anything a section needs to remember across
 * openings belongs in the config.
 *
 * Register at startup, before the window can be opened.
 * ------------------------------------------------------------------------- */
typedef void (*TaskSettingsSectionFn)(TaskApp *app, GtkWidget *column,
                                      GtkWindow *window, gpointer user_data);

void task_settings_add_section(TaskSettingsSectionFn fn, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_settings_init() — register the app's OWN contributed sections (the
 * Plugins list).  Call ONCE from main(), after the plugins have been
 * loaded so the list can show what was found.  It goes through
 * task_settings_add_section like anything else: an app section that
 * needed a shortcut would mean the registry was not good enough for a
 * plugin's.
 * ------------------------------------------------------------------------- */
void task_settings_init(void);

/* task_settings_section_heading() — a bold heading in the window's own
 * style, so a contributed section is indistinguishable from a built-in
 * one.  Pack it yourself; it is not added for you.                        */
GtkWidget *task_settings_section_heading(const gchar *text);

/* task_settings_section_note() — the wrapping explanatory paragraph that
 * sits under a heading, in the same style.                                */
GtkWidget *task_settings_section_note(const gchar *text);

#endif /* TASK_SETTINGS_WINDOW_H */
