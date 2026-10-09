/* ===========================================================================
 * json.h — minimal JSON parser + string escaping for Tasks
 *
 * json-glib is not a dependency of this app, and the
 * Google Calendar API only needs a small subset of JSON: parse a response
 * into a tree, read strings/objects/arrays out of it, and escape strings
 * into hand-built request bodies (GString).  This module is exactly that.
 * =========================================================================== */

#ifndef TASK_JSON_H
#define TASK_JSON_H

#include <glib.h>

/* The JSON value kinds.                                                    */
typedef enum {
    TASK_JSON_NULL = 0,
    TASK_JSON_BOOL,
    TASK_JSON_NUMBER,
    TASK_JSON_STRING,
    TASK_JSON_ARRAY,
    TASK_JSON_OBJECT
} task_json_type_t;

/* ---------------------------------------------------------------------------
 * task_json_t — one parsed JSON value (a tagged union).
 *   type     — which member is live.
 *   b/num/str — scalar payloads (str is owned, UTF-8, \u-escapes decoded).
 *   items    — TASK_JSON_ARRAY: task_json_t* children (owned).
 *   keys     — TASK_JSON_OBJECT: gchar* member names, parallel to items.
 * ------------------------------------------------------------------------- */
typedef struct task_json {
    task_json_type_t  type;
    gboolean    b;
    gdouble     num;
    gchar      *str;
    GPtrArray  *items;
    GPtrArray  *keys;
} task_json_t;

/* ---------------------------------------------------------------------------
 * task_json_parse() — parse a complete JSON document.
 *   text — the document; len — its byte length, or -1 for NUL-terminated.
 * Returns the root value (free with task_json_free), or NULL on any syntax
 * error (this client never needs error detail beyond "bad response").
 * ------------------------------------------------------------------------- */
task_json_t *task_json_parse(const gchar *text, gssize len);

/* task_json_free() — free a value and its whole subtree.  NULL-safe.       */
void task_json_free(task_json_t *v);

/* ---------------------------------------------------------------------------
 * Tree accessors — every getter is NULL-safe and type-checked, returning
 * NULL / the fallback when the path or type doesn't match, so response
 * handling can chain lookups without intermediate checks.
 * ------------------------------------------------------------------------- */
task_json_t      *task_json_get(task_json_t *obj, const gchar *key);   /* object member */
const gchar *task_json_str(task_json_t *obj, const gchar *key);   /* string member */
gboolean     task_json_bool(task_json_t *obj, const gchar *key, gboolean def);
guint        task_json_len(task_json_t *arr);                     /* array length */
task_json_t      *task_json_at(task_json_t *arr, guint i);             /* array element */

/* ---------------------------------------------------------------------------
 * task_json_escape() — append `s` to `out` as a JSON string INCLUDING the
 * surrounding quotes (control characters and "\ escaped; UTF-8 passes
 * through verbatim).  NULL appends the literal `null` token (no quotes).
 * ------------------------------------------------------------------------- */
void task_json_escape(GString *out, const gchar *s);


#endif /* TASK_JSON_H */
