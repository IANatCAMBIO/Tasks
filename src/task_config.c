#include "task_config.h"
#include "task_paths.h"

#include <stdlib.h>
#include <string.h>

#define CONFIG_GROUP "tasks"        /* the ini's single group; part of the
                                     * file format */
#define CONFIG_FILE  "tasks.ini"

/* The default of every key that has one.  The single source of truth: no
 * call site passes a default of its own. */
static const struct {
    const gchar *key;
    const gchar *value;
} config_defaults[] = {
    { TASK_CFG_BOLD_TITLES,         "0"   },
    { TASK_CFG_KANBAN_SHADOW,       "1"   },
    { TASK_CFG_FORECAST_BUSINESS_DAYS, "1" },
    { TASK_CFG_NOTE_CONTEXT,        "1"   },
    { TASK_CFG_QUICK_TASK_HOTKEY,   "1"   },
    { TASK_CFG_SHOW_COMPLETED,      "1"   },
    { TASK_CFG_KANBAN_VIEW,         "0"   },
    { TASK_CFG_COMPACT,             "0"   },
    { TASK_CFG_SIDEBAR_VISIBLE,     "1"   },
    { TASK_CFG_FAVORITES_BAR_VISIBLE, "1" },
    { TASK_CFG_WIN_W,               "980" },
    { TASK_CFG_WIN_H,               "640" },
    { TASK_CFG_COMPACT_W,           "600" },
    { TASK_CFG_COMPACT_H,           "500" },
    { TASK_CFG_COMPACT_PRE_W,       "980" },
    { TASK_CFG_COMPACT_PRE_H,       "640" },
    { TASK_CFG_COL_DONE,            "1"   },
    { TASK_CFG_COL_STATUS,          "0"   },
    { TASK_CFG_COL_DUE,             "1"   },
    { TASK_CFG_COL_COMPLETED,       "1"   },
    { TASK_CFG_NOTIF_MODE,          "tasks" },
    { TASK_CFG_NOTIF_CHECK_MIN,     "1"   },
    { TASK_CFG_GCAL_ENABLED,        "0"   },
    { TASK_CFG_GCAL_INTERVAL_MIN,   "5"   },
};

static GKeyFile *config_kf;         /* the in-memory config, or NULL      */
static gchar    *config_path;       /* written through on every change    */

/*
 * task_config_init — resolve the data directory and load the ini.
 * Output is documented in task_config.h.
 */
gboolean
task_config_init(void)
{
    if (config_kf != NULL) return TRUE;

    char *dir = task_data_dir_ensure();
    if (dir == NULL) return FALSE;

    config_path = g_build_filename(dir, CONFIG_FILE, NULL);
    free(dir);

    /* The directory passed the fence; the file could still be a link into
     * the live directory. */
    if (task_path_is_live_data(config_path)) {
        g_clear_pointer(&config_path, g_free);
        return FALSE;
    }

    /* A missing file is the ordinary first run and loads as empty: every
     * key then reads as its registry default. */
    config_kf = g_key_file_new();
    g_key_file_load_from_file(config_kf, config_path, G_KEY_FILE_NONE, NULL);
    return TRUE;
}

/*
 * task_config_close — free the loaded config.
 */
void
task_config_close(void)
{
    g_clear_pointer(&config_kf, g_key_file_unref);
    g_clear_pointer(&config_path, g_free);
}

/*
 * task_config_path — the ini file's path.
 * Output is documented in task_config.h.
 */
const gchar *
task_config_path(void)
{
    return config_path;
}

/*
 * default_of — the registry default of a key.
 *
 * Inputs:
 *   key — the key
 *
 * Output:
 *   borrowed static string, or NULL when the key has no default.
 */
static const gchar *
default_of(const gchar *key)
{
    for (gsize i = 0; i < G_N_ELEMENTS(config_defaults); i++) {
        if (strcmp(config_defaults[i].key, key) == 0) {
            return config_defaults[i].value;
        }
    }
    return NULL;
}

/*
 * task_config_get — read one setting, falling back to its default.
 * Inputs/output are documented in task_config.h.
 */
gchar *
task_config_get(const gchar *key)
{
    if (config_kf == NULL) return NULL;

    gchar *v = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    if (v != NULL && *v != '\0') return v;
    g_free(v);
    return g_strdup(default_of(key));
}

/*
 * task_config_get_bool — read a 0/1 setting.
 * Inputs/output are documented in task_config.h.
 */
gboolean
task_config_get_bool(const gchar *key)
{
    gchar   *v = task_config_get(key);
    gboolean b = v != NULL && strcmp(v, "0") != 0;

    g_free(v);
    return b;
}

/*
 * task_config_get_int — read an integer setting.
 * Inputs/output are documented in task_config.h.
 */
gint
task_config_get_int(const gchar *key)
{
    gchar *v = task_config_get(key);
    gint   n = v != NULL ? atoi(v) : 0;

    g_free(v);
    return n;
}

/*
 * task_config_set — change one setting and write the ini through.
 * Inputs are documented in task_config.h.
 */
void
task_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL) return;

    gchar *old = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    gboolean same = g_strcmp0(old, value) == 0;
    g_free(old);
    if (same) return;

    if (value != NULL) {
        g_key_file_set_string(config_kf, CONFIG_GROUP, key, value);
    } else {
        g_key_file_remove_key(config_kf, CONFIG_GROUP, key, NULL);
    }

    GError *err = NULL;
    if (!g_key_file_save_to_file(config_kf, config_path, &err)) {
        g_warning("config: could not write %s: %s", config_path,
                  err->message);
        g_clear_error(&err);
    }
}

/*
 * task_config_set_bool — write a 0/1 setting.
 * Inputs are documented in task_config.h.
 */
void
task_config_set_bool(const gchar *key, gboolean value)
{
    task_config_set(key, value ? "1" : "0");
}

/*
 * task_config_set_int — write an integer setting.
 * Inputs are documented in task_config.h.
 */
void
task_config_set_int(const gchar *key, gint value)
{
    gchar text[16];

    g_snprintf(text, sizeof text, "%d", value);
    task_config_set(key, text);
}
