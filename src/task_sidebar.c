#include "task_sidebar.h"

/*
 * first_of_kind — the first row of one kind.
 *
 * Inputs:
 *   rows — the rows on screen
 *   n    — how many
 *   kind — the kind to look for
 *
 * Output:
 *   the row's index, or -1 when there is none.
 */
static int
first_of_kind(const task_sel_t *rows, int n, task_sel_kind_t kind)
{
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == kind) return i;
    }
    return -1;
}

/*
 * task_sidebar_pick — the row to select after a rebuild.
 * Inputs/output and the fallback order are documented in task_sidebar.h.
 */
int
task_sidebar_pick(const task_sel_t *rows, int n, task_sel_t wanted)
{
    int found = -1;

    if (wanted.kind != task_sel_header) {
        for (int i = 0; i < n && found < 0; i++) {
            if (rows[i].kind == wanted.kind && rows[i].id == wanted.id) found = i;
        }
    }
    if (found < 0) found = first_of_kind(rows, n, task_sel_view);

    for (int i = 0; i < n && found < 0; i++) {
        if (rows[i].kind != task_sel_header) found = i;
    }
    return found;
}
