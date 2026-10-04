/*
 * task_order — the saved, hand-made order of the tasks in a view.
 *
 * The task list (in manual-sort mode) and the Kanban board each let the
 * user drag tasks into an order, kept per sidebar selection in the config
 * as a comma-separated list of task ids:
 *
 *   <family>_list_<id>      a list
 *   <family>_group_<id>     a group's aggregate (ordered separately from
 *                           the lists under it: a different pane of tasks)
 *   <family>_<view id>      a registered view: manual_order_all, ...
 *
 * with the two families TASK_CFG_FAMILY_MANUAL (the list) and
 * TASK_CFG_FAMILY_KANBAN (the board).  They are deliberately separate: a
 * board drag must not silently rearrange a list the user hand-sorted.
 *
 * The spelling of a key and the rule for reading an order back are the
 * same for both panes, so they live here, once.
 *
 * NEVER SAVE AN ORDER FROM A FILTERED PANE.  Saving serialises the rows
 * currently on screen and nothing else; while a search hides rows that
 * would discard every hidden task's place.  Callers refuse to save while a
 * search is active (TasksLibraryWindow manual_sort_live; the board skips
 * the order half of a drop).  READING an order is always safe.
 *
 * Lifted from the GTK build's task_list.c.  Plain C; MAIN THREAD ONLY (it
 * reads and writes the config).
 */

#ifndef TASK_ORDER_H
#define TASK_ORDER_H

#include <stdint.h>

#include "task_pane.h"

/*
 * task_order_load — the display order the saved id list asks for, as
 * indices into `ids`.
 *
 * Ids named by the saved list come first, in its sequence; anything it does
 * not mention (a task created since) keeps its current relative order at
 * the tail.  Forgiving by design: a saved id that no longer exists matches
 * nothing.
 *
 * Inputs:
 *   family — TASK_CFG_FAMILY_MANUAL or TASK_CFG_FAMILY_KANBAN
 *   sel    — the sidebar selection the order belongs to
 *   ids    — the task ids currently shown, in their current order
 *   n      — how many
 *
 * Output:
 *   new int[n] (free it) holding every index 0..n-1 exactly once, so that
 *   position i should show ids[result[i]]; or NULL when there is nothing
 *   to do (no saved order, n < 2, or the selection keeps no order).
 */
int *task_order_load(const char *family, task_sel_t sel, const int64_t *ids,
                     int n);

/*
 * task_order_save — store an order for a selection.
 *
 * Inputs:
 *   family — TASK_CFG_FAMILY_MANUAL or TASK_CFG_FAMILY_KANBAN
 *   sel    — the sidebar selection
 *   ids    — the task ids in the order to remember; ids of 0 are skipped
 *   n      — how many
 */
void task_order_save(const char *family, task_sel_t sel, const int64_t *ids,
                     int n);

/*
 * task_order_drop — remove BOTH families' saved orders of a list or group
 * that is being deleted.  Nothing else removes them, so the ini would
 * otherwise grow a dead entry per family for every list ever deleted.
 *
 * Inputs:
 *   sel — a list or group selection; anything else is ignored
 */
void task_order_drop(task_sel_t sel);

#endif /* TASK_ORDER_H */
