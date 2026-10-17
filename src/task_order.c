#include "task_order.h"
#include "task_config.h"

#include "shared_order.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * order_key — the config key holding a selection's saved order.  The ini
 * spelling of both families exists in this one place.
 *
 * Inputs:
 *   family — the key family
 *   sel    — the sidebar selection
 *
 * Output:
 *   new string (free it), or NULL for a selection that keeps no order (the
 *   Tags header, an unregistered view).
 */
static char *
order_key(const char *family, task_sel_t sel)
{
    char *key = NULL;
    int   rc  = -1;

    if (sel.kind == task_sel_tag) {
        rc = asprintf(&key, "%s_tag_%" PRId64, family, sel.id);
    } else {
        /* A view's id is part of the ini format (task_view.h). */
        const TaskView *view = task_sel_view_of(sel);
        if (view != NULL) rc = asprintf(&key, "%s_%s", family, view->id);
    }
    return rc < 0 ? NULL : key;
}

/*
 * task_order_load — the saved order of a selection, as a permutation.
 * Inputs/output are documented in task_order.h.
 */
int *
task_order_load(const char *family, task_sel_t sel, const int64_t *ids, int n)
{
    if (ids == NULL || n <= 1) return NULL;

    char *key = order_key(family, sel);
    if (key == NULL) return NULL;

    gchar *saved = task_config_get(key);
    int   *order = shared_order_permutation(ids, n, saved);

    g_free(saved);
    free(key);
    return order;
}

/*
 * task_order_save — store a selection's order.
 * Inputs are documented in task_order.h.
 */
void
task_order_save(const char *family, task_sel_t sel, const int64_t *ids, int n)
{
    char *key = order_key(family, sel);
    if (key == NULL) return;

    char *text = shared_order_text(ids, n);
    task_config_set(key, text);                     /* no ids: no key */
    free(text);
    free(key);
}

/*
 * task_order_drop — remove a tag's saved orders once the tag has gone.
 * Inputs are documented in task_order.h.
 */
void
task_order_drop(task_sel_t sel)
{
    static const char * const families[] = {
        TASK_CFG_FAMILY_MANUAL, TASK_CFG_FAMILY_KANBAN,
    };

    if (sel.kind != task_sel_tag) return;

    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        char *key = order_key(families[i], sel);
        if (key == NULL) continue;
        task_config_set(key, NULL);
        free(key);
    }
}
