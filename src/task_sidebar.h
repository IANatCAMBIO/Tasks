/*
 * task_sidebar — the sidebar's rules that need no toolkit: what a list's
 * row says, which row is selected after a rebuild, and where a dragged
 * list lands in the saved order.
 *
 * The AppKit sidebar (sidebar.m) owns the outline and asks these questions
 * of plain arrays, so the answers can be tested without a window.
 *
 * Lifted from the GTK build's sidebar.c (lib_refresh_sidebar's selection
 * fallback, on_sb_drag_end's index arithmetic) and library_window.c
 * (lib_list_label).  Plain C; no state, so any thread may call it.
 */

#ifndef TASK_SIDEBAR_H
#define TASK_SIDEBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "db.h"
#include "task_pane.h"

/*
 * task_sidebar_list_label — what a list is called wherever it is shown by
 * name: its emoji, two spaces, its name; the name alone when it has no
 * emoji.  The same shape as a view's label (task_view.h).
 *
 * Inputs:
 *   list — the list
 *
 * Output:
 *   new string (free it).
 */
char *task_sidebar_list_label(const TaskList *list);

/*
 * task_sidebar_group_label — what a group's button in the lists bar says:
 * ▼, two spaces, its name — the ▼ marking it as a button that opens its
 * lists, in the place a list's emoji takes.
 *
 * Inputs:
 *   group — the group
 *
 * Output:
 *   new string (free it).
 */
char *task_sidebar_group_label(const TaskGroup *group);

/*
 * task_sidebar_tag_label — what a tag's proto-list is called wherever it is
 * shown by name: the tag emoji (🏷️), two spaces, the tag's name — the
 * same shape as a list's label.
 *
 * Inputs:
 *   tag — the tag
 *
 * Output:
 *   new string (free it).
 */
char *task_sidebar_tag_label(const TaskTag *tag);

/*
 * task_sidebar_pick — the row to select after the sidebar was rebuilt.
 *
 * Exactly one row is always selected and the Lists header never is.  When
 * the row that was selected has gone, the selection falls back, in order:
 *
 *   1. the row of the same kind and id (nothing has gone);
 *   2. for a VIEW or a TAG: the first row that is a view (Favorites
 *      vanishes when the last favourite is removed, a tag when the last
 *      task carrying it loses it; the user stays among the views);
 *   3. the first list (a deleted list or group leaves the user on a list);
 *   4. the first row that is not the header.
 *
 * Inputs:
 *   rows   — the rows on screen, top to bottom (rows hidden inside a
 *            collapsed parent are left out: a hidden row cannot be selected)
 *   n      — how many
 *   wanted — the selection before the rebuild
 *
 * Output:
 *   the index of the row to select, or -1 when no row can be selected.
 */
int task_sidebar_pick(const task_sel_t *rows, int n, task_sel_t wanted);

/*
 * task_sidebar_move_list — reorder the lists after one was dragged.
 *
 * The lists have ONE saved order across the whole sidebar (grouped lists
 * first, group by group, then the ungrouped ones: the order they are
 * shown in), which task_db_lists_reorder stores.  A drop names the place
 * by the list it lands in front of, counted in that order BEFORE the move.
 * Which group the list then belongs to is the caller's business: the order
 * can stay the same while the group changes (the last list of a group
 * dropped at the head of the ungrouped ones).
 *
 * Inputs:
 *   ids     — every list id in display order; reordered in place
 *   n       — how many
 *   drag_id — the list that was dragged
 *   before  — the index (in the order as passed in) of the list the
 *             dragged one lands in front of; n means "after the last".
 *             Values outside 0..n are clamped.
 *
 * Output:
 *   true when the order changed; false when the list stays where it was or
 *   drag_id is not among ids (ids is untouched).
 */
bool task_sidebar_move_list(int64_t *ids, int n, int64_t drag_id, int before);

#endif /* TASK_SIDEBAR_H */
