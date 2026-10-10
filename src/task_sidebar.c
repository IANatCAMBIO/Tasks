#include "task_sidebar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * icon_label — an icon, two spaces, a name; the name alone when there is
 * no icon.
 *
 * Inputs:
 *   icon — the icon text, "" for none
 *   name — the name
 *
 * Output:
 *   new string (free it).  Aborts when out of memory.
 */
static char *
icon_label(const char *icon, const char *name)
{
    char *label = NULL;
    int   rc    = icon[0] != '\0'
        ? asprintf(&label, "%s  %s", icon, name)
        : asprintf(&label, "%s", name);

    if (rc < 0) abort();
    return label;
}

/*
 * task_sidebar_list_label — a list's display label.
 * Inputs/output are documented in task_sidebar.h.
 */
char *
task_sidebar_list_label(const TaskList *list)
{
    return icon_label(list->emoji, list->name);
}

/*
 * task_sidebar_group_label — a group's display label.
 * Inputs/output are documented in task_sidebar.h.
 */
char *
task_sidebar_group_label(const TaskGroup *group)
{
    return icon_label("\xe2\x96\xbc", group->name);       /* ▼ */
}

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
    if (found < 0 && wanted.kind == task_sel_view) {
        found = first_of_kind(rows, n, task_sel_view);
    }
    if (found < 0) found = first_of_kind(rows, n, task_sel_list);

    for (int i = 0; i < n && found < 0; i++) {
        if (rows[i].kind != task_sel_header) found = i;
    }
    return found;
}

/*
 * task_sidebar_move_list — reorder the lists after a drag.
 * Inputs/output are documented in task_sidebar.h.
 */
bool
task_sidebar_move_list(int64_t *ids, int n, int64_t drag_id, int before)
{
    int from = -1;

    for (int i = 0; i < n && from < 0; i++) {
        if (ids[i] == drag_id) from = i;
    }
    if (from < 0) return false;

    if (before < 0) before = 0;
    if (before > n) before = n;

    /* Taking the list out shifts everything after it one place left, so a
     * landing place beyond it is one less once it is out. */
    int to = from < before ? before - 1 : before;
    if (to == from) return false;

    if (from < to) {
        memmove(&ids[from], &ids[from + 1], (size_t)(to - from) * sizeof *ids);
    } else {
        memmove(&ids[to + 1], &ids[to], (size_t)(from - to) * sizeof *ids);
    }
    ids[to] = drag_id;
    return true;
}
