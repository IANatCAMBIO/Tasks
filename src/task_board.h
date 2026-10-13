/*
 * task_board — what the Kanban board shows and what a card drop does, as
 * toolkit-neutral logic.
 *
 * The board is three lanes, one per TaskStatus (the lane index IS the
 * status value).  This module decides which card goes in which lane and in
 * what order, finds a card, turns a pointer height into an insertion slot,
 * and carries out a drop.  The AppKit board (kanban.h) draws the result and
 * feeds the mouse in; none of the arithmetic lives there, so all of it can
 * be tested without a display or a mouse.
 *
 * THE ORDER OF THE LANES.  New and In Progress are HAND-ORDERED: the user
 * drags cards into place and the order is saved per sidebar selection
 * (task_order.h, family TASK_CFG_FAMILY_KANBAN) as one flat list of ids,
 * New's cards then In Progress's.  Done is never hand-ordered: it shows
 * the most recently completed first and takes no part in the saved order —
 * it can be capped to a few cards, and saving a capped lane would discard
 * the place of every card the cap hides.
 *
 * A DROP HAS TWO INDEPENDENT HALVES: the status of the dropped cards (a
 * database write per card that changes lane) and the saved order (config
 * only; no task row is touched).  Either may happen without the other, and
 * a drop that leaves the cards where they were does neither.
 *
 * Lifted from the GTK build's kanban.c.  Plain C.  task_board_build and
 * task_board_drop read the config and the database: MAIN THREAD ONLY.
 */

#ifndef TASK_BOARD_H
#define TASK_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "task_app.h"
#include "task_pane.h"
#include "task_rows.h"

/*
 * task_board_lane_t — one lane.
 *
 *   cards — the cards shown, top to bottom.  The array is owned; the rows
 *           are BORROWED from the row list the board was built from, which
 *           must outlive the board.
 *   count — how many cards are shown
 *   total — how many tasks the lane holds; more than `count` only for a
 *           capped Done lane.  The lane's heading reports this number.
 */
typedef struct {
    const task_row_t **cards;
    size_t             count;
    size_t             total;
} task_board_lane_t;

/* The board of one refresh: its lanes, indexed by TaskStatus. */
typedef struct {
    task_board_lane_t lanes[TASK_STATUS_N_VALUES];
} task_board_t;

/*
 * task_board_build — sort a refresh's rows into lanes.
 *
 * The selection's saved card order is applied first (rows it does not
 * mention keep their order at the tail), then each row goes to the lane of
 * its status; a status outside the enum (a hand-edited database) goes to
 * New.  The Done lane is then re-sorted, most recently completed first —
 * tasks completed in the same second keep the order they arrived in — and
 * cut to `done_cap` cards.
 *
 * Inputs:
 *   sel      — the sidebar selection, which names the saved order
 *   rows     — the rows to show; not NULL.  Borrowed, and must outlive the
 *              board
 *   done_cap — the most Done cards to show; SIZE_MAX shows them all
 *
 * Output:
 *   new board (task_board_free); never NULL.
 */
task_board_t *task_board_build(task_sel_t sel, const task_row_list_t *rows,
                               size_t done_cap);

/*
 * task_board_free — free a board.  The rows it borrowed are not touched.
 *
 * Inputs:
 *   board — the board, or NULL (ignored)
 */
void task_board_free(task_board_t *board);

/*
 * task_board_find — where a card is on the board.
 *
 * Inputs:
 *   board — the board
 *   id    — the task id to look for
 *   lane  — receives the card's lane; may be NULL
 *   index — receives its position in that lane, 0 = top; may be NULL
 *
 * Output:
 *   true when the card is shown; false (nothing written) when it is not —
 *   which includes a Done task hidden by the cap.
 */
bool task_board_find(const task_board_t *board, int64_t id, TaskStatus *lane,
                     size_t *index);

/*
 * task_board_slot_at — the insertion slot a pointer height asks for in one
 * lane: slot 0 is above the first card, slot n below the last.  THE
 * TWO-HALF RULE: over the upper half of a card the slot is above that
 * card, over its lower half below it, so the slot changes as the pointer
 * crosses a card's middle line and never while it is between two cards.
 *
 * Inputs:
 *   mids  — the middle line (y) of each card, top to bottom, in any
 *           coordinate that grows downwards; may be NULL when count is 0
 *   count — how many cards the lane shows
 *   y     — the pointer, in the same coordinate
 *
 * Output:
 *   the slot, 0 .. count.  A pointer exactly on a middle line takes the
 *   slot below that card.
 */
size_t task_board_slot_at(const double *mids, size_t count, double y);

/*
 * task_board_reorder — the hand-made order a drop asks for.
 *
 * The moving cards are lifted out of wherever they are and put back
 * together, in the order given, at `slot` of `lane`.  The slot counts the
 * cards ON SCREEN, the moving ones among them (they stay in place, dimmed,
 * during a drag) — so a card dropped just above or just below itself has
 * not moved, and a block of neighbouring cards dropped anywhere on itself
 * has not moved either.
 *
 * Inputs:
 *   board    — the board as shown
 *   moving   — the ids being dropped, in display order
 *   n_moving — how many
 *   lane     — the lane dropped on: New or In Progress (Done keeps no order)
 *   slot     — the insertion slot in that lane (task_board_slot_at); a
 *              value past the last card means the end
 *   count    — receives the length of the result
 *
 * Output:
 *   new array (free it) of the ids of the hand-ordered lanes after the
 *   drop, New's cards then In Progress's — what task_order_save stores;
 *   or NULL (count untouched) when that order is exactly the one already
 *   shown, when lane is Done or outside the enum, or when nothing is
 *   moving.
 */
int64_t *task_board_reorder(const task_board_t *board, const int64_t *moving,
                            size_t n_moving, TaskStatus lane, size_t slot,
                            size_t *count);

/*
 * task_board_drop — carry out a drop: both halves, and the status message.
 *
 * Only cards that are on the board and whose task still exists take part.
 * Each one whose status differs from the lane is written (which stamps its
 * updated_at, and completed_at on entering Done); cards already in the
 * lane are not touched.  The new order is saved when it differs from the
 * one shown, unless the lane is Done or `save_order` is false.  The
 * message — "“Title” — In Progress", or "3 tasks — Done" — reports the
 * status changes; a pure reorder says nothing.  The status changes are one
 * transaction: when it cannot be written (another writer held the file
 * past the busy timeout) nothing moves, the order is not saved, and
 * task_app_write_failed says so and refreshes to the stored state.
 *
 * Inputs:
 *   app        — the application context (database open)
 *   sel        — the sidebar selection, which names the saved order
 *   board      — the board as shown
 *   moving     — the ids being dropped, in display order
 *   n_moving   — how many
 *   lane       — the lane dropped on
 *   slot       — the insertion slot in that lane (ignored for Done)
 *   save_order — false while a search filters the board: the cards on
 *                screen are then only the matches, and saving their order
 *                would discard the place of every hidden card
 *
 * Output:
 *   true when anything changed, or a write failed — the caller refreshes,
 *   and moves its selection to the dropped cards; false when the drop was
 *   a no-op (nothing was written).
 */
bool task_board_drop(TaskApp *app, task_sel_t sel, const task_board_t *board,
                     const int64_t *moving, size_t n_moving, TaskStatus lane,
                     size_t slot, bool save_order);

#endif /* TASK_BOARD_H */
