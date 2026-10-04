/*
 * task_sort — the ARRANGEMENT of the task list's rows: sorting by the
 * column headers, the remembered header sort, and the block move a manual
 * drag performs.
 *
 * An arrangement is a DISPLAY ORDER: an int[n] holding every index 0..n-1
 * exactly once, so that position i shows rows[order[i]] — the same shape
 * task_order_load returns (task_order.h).  The rows themselves are never
 * moved; that is what lets a pane keep anything it holds per row (measured
 * heights) through a sort or a drag.
 *
 * The five column comparators are lifted from the GTK build's task_list.c.
 * Sorting is STABLE: rows a key cannot tell apart keep the order they came
 * in, so a header sort laid over a hand-made order keeps that order inside
 * each group of equals, and sorting the same rows twice gives the same
 * answer.
 *
 * Plain C (no GLib).  MAIN THREAD ONLY: the title comparator caches the
 * locale it folds case with.
 */

#ifndef TASK_SORT_H
#define TASK_SORT_H

#include <stdbool.h>
#include <stddef.h>

#include "task_rows.h"

/* The sortable columns. */
typedef enum {
    task_sort_title = 0,            /* the plain title, case-insensitive   */
    task_sort_done,                 /* not Done before Done                */
    task_sort_status,               /* New < In Progress < Done            */
    task_sort_due,                  /* soonest first; undated rows last    */
    task_sort_completed,            /* oldest first; never-completed last  */
    task_sort_column_count
} task_sort_column_t;

/*
 * task_sort_key_t — one level of a sort.  Descending is the exact reverse
 * of the comparison, so it also brings the undated / never-completed rows
 * to the top.
 */
typedef struct {
    task_sort_column_t column;
    bool               ascending;
} task_sort_key_t;

/* A sort has at most one key per column. */
#define TASK_SORT_MAX_KEYS task_sort_column_count

/*
 * task_sort_column_name — a column's name: "title", "done", "status",
 * "due", "completed".  It is what the saved sort spells (so it is part of
 * the ini format) and what the table's sort descriptors carry as their key.
 *
 * Inputs:
 *   column — the column
 *
 * Output:
 *   static string, or NULL for a value that is not a column.
 */
const char *task_sort_column_name(task_sort_column_t column);

/*
 * task_sort_keys_parse — read a saved sort: column names, comma separated,
 * most significant first, each with a leading '-' when descending
 * ("due,-title").  Forgiving, as a hand-edited ini requires: an unknown
 * name, and a second mention of a column, are skipped.
 *
 * Inputs:
 *   text — the saved value; NULL or "" means unsorted
 *   keys — receives the keys; room for TASK_SORT_MAX_KEYS
 *
 * Output:
 *   how many keys were written (0 = unsorted).
 */
size_t task_sort_keys_parse(const char *text, task_sort_key_t *keys);

/*
 * task_sort_keys_format — the saved spelling of a sort; what
 * task_sort_keys_parse reads back.
 *
 * Inputs:
 *   keys   — the keys, most significant first
 *   n_keys — how many
 *
 * Output:
 *   new string (free it), or NULL for no keys — which is also "remove the
 *   setting" to task_config_set.
 */
char *task_sort_keys_format(const task_sort_key_t *keys, size_t n_keys);

/*
 * task_sort_apply — sort a display order by the keys, stably.
 *
 * Inputs:
 *   rows   — the rows the order indexes (borrowed, not moved)
 *   order  — the display order to sort in place: n indices into rows, in
 *            the arrangement that ties should keep
 *   n      — how many
 *   keys   — the keys, most significant first
 *   n_keys — how many; 0 leaves the order as it is
 */
void task_sort_apply(task_row_t *const *rows, int *order, int n,
                     const task_sort_key_t *keys, size_t n_keys);

/*
 * task_sort_move — move a block of positions of a display order to a gap,
 * keeping the block's own sequence: what dropping dragged rows between two
 * others does.
 *
 * Inputs:
 *   order  — the display order, changed in place
 *   n      — its length
 *   from   — the positions to move: ascending, distinct, each 0..n-1
 *   n_from — how many
 *   to     — the gap they land in, 0..n, counted BEFORE the move: the block
 *            goes in front of what is now at position `to` (n = the end)
 *
 * Output:
 *   the position the block's first row ends up at.
 */
int task_sort_move(int *order, int n, const int *from, int n_from, int to);

#endif /* TASK_SORT_H */
