/* ===========================================================================
 * settings_window.h — the Tasks settings window
 *
 * One singleton window (File → Settings…), in the Notes settings
 * style: plain GtkWindow, bold section headings, write-through controls
 * (every change lands in the ini immediately — no OK/Apply buttons).
 *
 * Sections:
 *   Appearance — bold task titles, Due Today's overdue rows, Kanban
 *     shadows.
 *   Database — the health plate and the rotating backups.
 * =========================================================================== */

#ifndef TASK_SETTINGS_WINDOW_H
#define TASK_SETTINGS_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * task_settings_window_open() — show (or raise) the settings window.
 *   app     — the application context.
 *   parent  — transient parent (the library window).
 *   db_path — the database path shown in the Database section.
 * ------------------------------------------------------------------------- */
void task_settings_window_open(TaskApp *app, GtkWindow *parent,
                               const gchar *db_path);

#endif /* TASK_SETTINGS_WINDOW_H */
