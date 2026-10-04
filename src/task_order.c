#include "task_order.h"
#include "task_config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One on-screen task for the id lookup: its id and where it sits now. */
typedef struct {
    int64_t id;
    int     index;
} order_slot_t;

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
 *   Lists header, an unregistered view).
 */
static char *
order_key(const char *family, task_sel_t sel)
{
    char *key = NULL;
    int   rc  = -1;

    if (sel.kind == task_sel_list) {
        rc = asprintf(&key, "%s_list_%" PRId64, family, sel.id);
    } else if (sel.kind == task_sel_group) {
        rc = asprintf(&key, "%s_group_%" PRId64, family, sel.id);
    } else {
        /* A view's id is part of the ini format (task_view.h). */
        const TaskView *view = task_sel_view_of(sel);
        if (view != NULL) rc = asprintf(&key, "%s_%s", family, view->id);
    }
    return rc < 0 ? NULL : key;
}

/*
 * slot_compare — qsort order for the lookup: by id, then by position, so
 * that rows sharing an id are claimed first to last.
 *
 * Inputs:
 *   a, b — order_slot_t pointers
 *
 * Output:
 *   negative, zero or positive, as qsort expects.
 */
static int
slot_compare(const void *a, const void *b)
{
    const order_slot_t *sa = a;
    const order_slot_t *sb = b;

    if (sa->id != sb->id) return sa->id < sb->id ? -1 : 1;
    return sa->index - sb->index;
}

/*
 * slot_claim — the position of the first row with this id that the saved
 * order has not yet placed.
 *
 * Inputs:
 *   slots  — the lookup, sorted by slot_compare
 *   n      — its length
 *   id     — the id to find
 *   placed — which positions are already claimed
 *
 * Output:
 *   the row's current index, or -1 when no unplaced row has that id.
 */
static int
slot_claim(const order_slot_t *slots, int n, int64_t id, const bool *placed)
{
    int lo = 0;
    int hi = n;

    while (lo < hi) {               /* first slot whose id is >= id       */
        int mid = lo + (hi - lo) / 2;
        if (slots[mid].id < id) lo = mid + 1; else hi = mid;
    }
    for (; lo < n && slots[lo].id == id; lo++) {
        if (!placed[slots[lo].index]) return slots[lo].index;
    }
    return -1;
}

/*
 * order_permutation — the permutation a saved id list asks for.  A sorted
 * lookup, not a nested scan: the scan was a quarter of a million
 * comparisons on a 500-row list, repeated on every refresh.
 *
 * Inputs:
 *   ids   — the ids on screen, in their current order
 *   n     — how many (at least 2)
 *   saved — the config value: ids, comma separated; not empty
 *
 * Output:
 *   new int[n] (free it): every index exactly once.
 */
static int *
order_permutation(const int64_t *ids, int n, const char *saved)
{
    order_slot_t *slots  = malloc((size_t)n * sizeof *slots);
    int          *order  = malloc((size_t)n * sizeof *order);
    bool         *placed = calloc((size_t)n, sizeof *placed);
    if (slots == NULL || order == NULL || placed == NULL) abort();

    for (int i = 0; i < n; i++) {
        slots[i].id    = ids[i];
        slots[i].index = i;
    }
    qsort(slots, (size_t)n, sizeof *slots, slot_compare);

    int fill = 0;
    for (const char *p = saved; p != NULL && *p != '\0'; ) {
        int64_t id = strtoll(p, NULL, 10);
        int     j  = id != 0 ? slot_claim(slots, n, id, placed) : -1;
        if (j >= 0) {
            order[fill++] = j;
            placed[j]     = true;
        }
        p = strchr(p, ',');
        if (p != NULL) p++;
    }

    /* Everything the saved list did not claim, in the order it had. */
    for (int i = 0; i < n; i++) {
        if (!placed[i]) order[fill++] = i;
    }
    free(placed);
    free(slots);
    return order;
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
    int   *order = saved != NULL ? order_permutation(ids, n, saved) : NULL;

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

    /* An int64 is at most 20 characters, plus its comma. */
    size_t  room = (size_t)(n > 0 ? n : 0) * 21 + 1;
    char   *text = malloc(room);
    size_t  used = 0;
    if (text == NULL) abort();

    text[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (ids[i] == 0) continue;
        used += (size_t)snprintf(text + used, room - used, "%s%" PRId64,
                                 used > 0 ? "," : "", ids[i]);
    }
    task_config_set(key, text);
    free(text);
    free(key);
}

/*
 * task_order_drop — remove a deleted list's or group's saved orders.
 * Inputs are documented in task_order.h.
 */
void
task_order_drop(task_sel_t sel)
{
    static const char * const families[] = {
        TASK_CFG_FAMILY_MANUAL, TASK_CFG_FAMILY_KANBAN,
    };

    if (sel.kind != task_sel_list && sel.kind != task_sel_group) return;

    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        char *key = order_key(families[i], sel);
        if (key == NULL) continue;
        task_config_set(key, NULL);
        free(key);
    }
}
