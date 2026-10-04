/*
 * task_config — the app's settings: one ini file in the data directory.
 *
 * The file is <data dir>/tasks.ini (task_paths.h says where the data
 * directory is; $TASKS_DATA_DIR redirects it together with the database).
 * It is a GKeyFile with the single group [tasks] and string values, loaded
 * ONCE by task_config_init, kept in memory, and written through in full on
 * every change that actually changes a value.  There is no defaults file
 * and no "ini beside the binary": the default of every key lives in ONE
 * place, the registry in task_config.c, so a missing file and an empty
 * file both read as the defaults below.
 *
 * ALL persisted app state goes through here — window sizes included — so
 * that redirecting the data directory redirects everything.  Do not use
 * NSUserDefaults or AppKit autosave names for app state.
 *
 * MAIN THREAD ONLY.  A background job copies the values it needs before it
 * starts.
 *
 * Carried from the GTK build's app.c; GKeyFile and the g* types stay for
 * this phase.  Returned strings are freed with g_free (or free: GLib has
 * used the system allocator since 2.46).
 */

#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

#include <glib.h>

/* ── The key registry ──────────────────────────────────────────────────
 * Every fixed key, with its type and default.  "bool" is "1"/"0".  A key
 * with no default reads as NULL / FALSE / 0 until it is written. */

/* Database */
#define TASK_CFG_DB_DIR              "db_dir"              /* dir holding tasks.db; none = data dir.
                                                            * Written only by File > Open Database File */
#define TASK_CFG_BACKUP_ENABLED      "backup_enabled"      /* bool, 0 */
#define TASK_CFG_BACKUP_DIR          "backup_dir"          /* dir; none = <data dir>/backups */
#define TASK_CFG_BACKUP_INTERVAL_MIN "backup_interval_min" /* int minutes, 60; 0 = manual only */
#define TASK_CFG_BACKUP_KEEP         "backup_keep"         /* int files, 10 (clamped 1-500) */

/* Appearance (Settings window) */
#define TASK_CFG_BOLD_TITLES         "bold_task_titles"        /* bool, 0 */
#define TASK_CFG_KANBAN_SHADOW       "kanban_shadow"           /* bool, 1 */
#define TASK_CFG_DUE_TODAY_OVERDUE   "due_today_show_overdue"  /* bool, 0 */

/* Library window state */
#define TASK_CFG_SHOW_COMPLETED      "show_completed"        /* bool, 1 */
#define TASK_CFG_MANUAL_SORT         "task_list_manual_sort" /* bool, 0 */
#define TASK_CFG_KANBAN_VIEW         "kanban_view"           /* bool, 0 */
#define TASK_CFG_COMPACT             "compact_layout"        /* bool, 0 */
#define TASK_CFG_SIDEBAR_VISIBLE     "sidebar_visible"       /* bool, 1 */
#define TASK_CFG_SIDEBAR_WIDTH       "sidebar_width"         /* int points; none = fit content */
#define TASK_CFG_WIN_X               "win_x"                 /* int; none = centred */
#define TASK_CFG_WIN_Y               "win_y"                 /* int; none = centred */
#define TASK_CFG_WIN_W               "win_w"                 /* int, 980 */
#define TASK_CFG_WIN_H               "win_h"                 /* int, 640 */
#define TASK_CFG_COMPACT_W           "compact_w"             /* int, 600: size in Compact mode */
#define TASK_CFG_COMPACT_H           "compact_h"             /* int, 500 */
#define TASK_CFG_COMPACT_PRE_W       "compact_pre_w"         /* int, 980: size to restore on leaving it */
#define TASK_CFG_COMPACT_PRE_H       "compact_pre_h"         /* int, 640 */

/* Task-list column visibility (the header menu) */
#define TASK_CFG_COL_DONE            "col_done_visible"      /* bool, 1 */
#define TASK_CFG_COL_STATUS          "col_status_visible"    /* bool, 0 */
#define TASK_CFG_COL_DUE             "col_due_visible"       /* bool, 1 */
#define TASK_CFG_COL_COMPLETED       "col_completed_visible" /* bool, 1 */

/* Per-view order keys are built by task_order_key (task_order.h):
 * "<family>_list_<id>", "<family>_group_<id>", "<family>_<view id>", with
 * these two families.  Value: task ids, comma separated.  No default. */
#define TASK_CFG_FAMILY_MANUAL       "manual_order"
#define TASK_CFG_FAMILY_KANBAN       "kanban_order"

/*
 * task_config_init — resolve the data directory (creating it), and load
 * <data dir>/tasks.ini if it exists.  Call once at startup, before
 * anything reads a setting; a second call is a no-op that returns TRUE.
 *
 * Output:
 *   TRUE when the config is usable.  FALSE when the data directory cannot
 *   be determined or created, or lies inside the GTK app's live data
 *   directory — the caller must then stop: nothing may be read or written.
 */
gboolean task_config_init(void);

/*
 * task_config_close — forget the loaded config and free it.  The file is
 * already up to date (every change is written through).  After this,
 * task_config_init may be called again — the tests do, to load a different
 * directory.
 */
void task_config_close(void);

/*
 * task_config_path — the ini file's path, for messages and diagnostics.
 *
 * Output:
 *   borrowed string valid until task_config_close, or NULL before
 *   task_config_init has succeeded.
 */
const gchar *task_config_path(void);

/*
 * task_config_get — read one setting as a string.
 *
 * Inputs:
 *   key — a TASK_CFG_* key or a per-view order key
 *
 * Output:
 *   new string (g_free): the stored value; else the registry default; NULL
 *   when the key is unset and has no default, or before task_config_init.
 *   A stored empty value counts as unset.
 */
gchar *task_config_get(const gchar *key);

/*
 * task_config_get_bool — read a 0/1 setting.  The app only ever writes "0"
 * and "1"; any other stored value reads as TRUE.
 *
 * Inputs:
 *   key — a bool key
 *
 * Output:
 *   the value; the registry default when unset; FALSE with no default.
 */
gboolean task_config_get_bool(const gchar *key);

/*
 * task_config_get_int — read an integer setting.
 *
 * Inputs:
 *   key — an int key
 *
 * Output:
 *   the value (atoi of the stored text — callers clamp); the registry
 *   default when unset; 0 with no default.
 */
gint task_config_get_int(const gchar *key);

/*
 * task_config_set — change one setting and write the ini through.  A value
 * equal to what is stored writes nothing.  A failed write is logged; the
 * value still applies for this run.
 *
 * Inputs:
 *   key   — the key
 *   value — the new value; NULL removes the key (it reads as its default
 *           again)
 */
void task_config_set(const gchar *key, const gchar *value);

/*
 * task_config_set_bool / task_config_set_int — task_config_set for a bool
 * ("1"/"0") or an integer.
 *
 * Inputs:
 *   key   — the key
 *   value — the new value
 */
void task_config_set_bool(const gchar *key, gboolean value);
void task_config_set_int(const gchar *key, gint value);

#endif /* TASK_CONFIG_H */
