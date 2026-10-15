/*
 * task_sidebar — the sidebar's rule that needs no toolkit: which row is
 * selected after a rebuild.
 *
 * The AppKit sidebar (sidebar.m) owns the table and asks this of plain
 * arrays, so the answer can be tested without a window.
 *
 * Plain C; no state, so any thread may call it.
 */

#ifndef TASK_SIDEBAR_H
#define TASK_SIDEBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "task_pane.h"

/*
 * task_sidebar_pick — the row to select after the sidebar was rebuilt.
 *
 * Exactly one row is always selected and the Tags header never is.  When
 * the row that was selected has gone (a tag no task carries any more), the
 * selection falls back, in order:
 *
 *   1. the row of the same kind and id (nothing has gone);
 *   2. the first row that is a view;
 *   3. the first row that is not the header.
 *
 * Inputs:
 *   rows   — the rows on screen, top to bottom
 *   n      — how many
 *   wanted — the selection before the rebuild
 *
 * Output:
 *   the index of the row to select, or -1 when no row can be selected.
 */
int task_sidebar_pick(const task_sel_t *rows, int n, task_sel_t wanted);

#endif /* TASK_SIDEBAR_H */
