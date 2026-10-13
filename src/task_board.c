#include "task_board.h"
#include "task_config.h"
#include "task_order.h"

#include <stdlib.h>
#include <string.h>

/*
 * lane_of — the lane a row belongs in.
 *
 * Inputs:
 *   row — the row
 *
 * Output:
 *   its status as a lane index; New for a status outside the enum, so a
 *   hand-edited database cannot make a task vanish from the board.
 */
static int
lane_of(const task_row_t *row)
{
    int lane = (int)row->status;

    return lane >= 0 && lane < TASK_STATUS_N_VALUES ? lane : (int)TASK_STATUS_NEW;
}

/*
 * hand_ordered — whether a lane keeps a hand-made order.  The one place
 * that says "every lane but Done".
 *
 * Inputs:
 *   lane — a lane index; may be outside the enum
 *
 * Output:
 *   true for New and In Progress.
 */
static bool
hand_ordered(int lane)
{
    return lane >= 0 && lane < TASK_STATUS_N_VALUES && lane != (int)TASK_STATUS_DONE;
}

/*
 * done_recent_compare — the Done lane's order: most recently completed
 * first.  Equal stamps compare equal, and the sort that uses this is
 * stable, so such cards keep the order they arrived in.
 *
 * Inputs:
 *   a, b — pointers to `const task_row_t *`
 *
 * Output:
 *   negative, zero or positive, as the sort expects.
 */
static int
done_recent_compare(const void *a, const void *b)
{
    const task_row_t *row_a = *(const task_row_t * const *)a;
    const task_row_t *row_b = *(const task_row_t * const *)b;

    if (row_a->completed_at == row_b->completed_at) return 0;
    return row_a->completed_at > row_b->completed_at ? -1 : 1;
}

/*
 * task_board_build — sort a refresh's rows into lanes.
 * Inputs/output are documented in task_board.h.
 */
task_board_t *
task_board_build(task_sel_t sel, const task_row_list_t *rows, size_t done_cap)
{
    task_board_t *board = calloc(1, sizeof *board);
    size_t        n     = rows->count;

    if (board == NULL) abort();
    if (n == 0) return board;

    /* The saved order, as a permutation of the rows (NULL: as given). */
    int64_t *ids = malloc(n * sizeof *ids);
    if (ids == NULL) abort();
    for (size_t i = 0; i < n; i++) ids[i] = rows->rows[i]->id;
    int *order = task_order_load(TASK_CFG_FAMILY_KANBAN, sel, ids, (int)n);
    free(ids);

    /* Size each lane, then deal the rows out in the saved order. */
    for (size_t i = 0; i < n; i++) board->lanes[lane_of(rows->rows[i])].total++;
    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) {
        task_board_lane_t *lane = &board->lanes[s];
        if (lane->total == 0) continue;
        lane->cards = malloc(lane->total * sizeof *lane->cards);
        if (lane->cards == NULL) abort();
    }
    for (size_t i = 0; i < n; i++) {
        const task_row_t  *row  = rows->rows[order != NULL ? (size_t)order[i] : i];
        task_board_lane_t *lane = &board->lanes[lane_of(row)];
        lane->cards[lane->count++] = row;
    }
    free(order);

    /* Done sorts itself.  mergesort because it is stable: tasks completed
     * in the same second (Mark All Complete) must not swap places from one
     * refresh to the next. */
    task_board_lane_t *done = &board->lanes[TASK_STATUS_DONE];
    if (done->count > 1 &&
        mergesort(done->cards, done->count, sizeof *done->cards, done_recent_compare) != 0) {
        abort();
    }
    if (done->count > done_cap) done->count = done_cap;
    return board;
}

/*
 * task_board_free — free a board.
 * Inputs are documented in task_board.h.
 */
void
task_board_free(task_board_t *board)
{
    if (board == NULL) return;

    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) free(board->lanes[s].cards);
    free(board);
}

/*
 * task_board_find — where a card is on the board.
 * Inputs/output are documented in task_board.h.
 */
bool
task_board_find(const task_board_t *board, int64_t id, TaskStatus *lane, size_t *index)
{
    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) {
        for (size_t i = 0; i < board->lanes[s].count; i++) {
            if (board->lanes[s].cards[i]->id != id) continue;
            if (lane != NULL) *lane = (TaskStatus)s;
            if (index != NULL) *index = i;
            return true;
        }
    }
    return false;
}

/*
 * task_board_slot_at — the insertion slot a pointer height asks for.
 * Inputs/output are documented in task_board.h.
 */
size_t
task_board_slot_at(const double *mids, size_t count, double y)
{
    size_t slot = 0;

    while (slot < count && y >= mids[slot]) slot++;
    return slot;
}

/*
 * id_among — whether an id is one of a set.
 *
 * Inputs:
 *   id  — the id to look for
 *   ids — the set
 *   n   — its size
 *
 * Output:
 *   true when found.
 */
static bool
id_among(int64_t id, const int64_t *ids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

/*
 * order_is_shown — whether an order is exactly the hand-ordered lanes as
 * the board shows them: the test for "this drop moved nothing".
 *
 * Inputs:
 *   board — the board as shown
 *   order — ids, New's cards then In Progress's
 *   count — how many
 *
 * Output:
 *   true when the two sequences are identical.
 */
static bool
order_is_shown(const task_board_t *board, const int64_t *order, size_t count)
{
    size_t k = 0;

    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (!hand_ordered(s)) continue;
        for (size_t i = 0; i < board->lanes[s].count; i++, k++) {
            if (k >= count || order[k] != board->lanes[s].cards[i]->id) return false;
        }
    }
    return k == count;
}

/*
 * task_board_reorder — the hand-made order a drop asks for.
 * Inputs/output are documented in task_board.h.
 */
int64_t *
task_board_reorder(const task_board_t *board, const int64_t *moving, size_t n_moving,
                   TaskStatus lane, size_t slot, size_t *count)
{
    if (n_moving == 0 || !hand_ordered((int)lane)) return NULL;

    size_t room = n_moving;
    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (hand_ordered(s)) room += board->lanes[s].count;
    }
    int64_t *order = malloc(room * sizeof *order);
    size_t   fill  = 0;
    if (order == NULL) abort();

    for (int s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (!hand_ordered(s)) continue;

        const task_board_lane_t *from = &board->lanes[s];
        size_t                   at   = slot < from->count ? slot : from->count;

        /* Walk the lane as shown, leaving the moving cards out and putting
         * them back, together, on reaching the slot.  Counting the slot
         * against the cards on screen — the moving ones included — is what
         * makes a drop just above or just below a card's own place a
         * non-move; subtracting them first and inserting at the raw slot
         * lands one place too low for every moving card above it. */
        for (size_t i = 0; i <= from->count; i++) {
            if (s == (int)lane && i == at) {
                memcpy(order + fill, moving, n_moving * sizeof *order);
                fill += n_moving;
            }
            if (i < from->count && !id_among(from->cards[i]->id, moving, n_moving)) {
                order[fill++] = from->cards[i]->id;
            }
        }
    }

    if (order_is_shown(board, order, fill)) {
        free(order);
        return NULL;
    }
    *count = fill;
    return order;
}

/*
 * task_board_drop — carry out a drop.
 * Inputs/output are documented in task_board.h.
 */
bool
task_board_drop(TaskApp *app, task_sel_t sel, const task_board_t *board,
                const int64_t *moving, size_t n_moving, TaskStatus lane, size_t slot,
                bool save_order)
{
    if (n_moving == 0 || (int)lane < 0 || (int)lane >= TASK_STATUS_N_VALUES) return false;

    int64_t    *alive     = malloc(n_moving * sizeof *alive);
    size_t      n_alive   = 0;
    size_t      n_status  = 0;
    const char *one_title = NULL;   /* the first card that changed lane   */
    if (alive == NULL) abort();

    /* The status half, in one transaction.  The database, not the board,
     * says what a task's status is now and whether it still exists: a
     * refresh may be pending. */
    if (!task_db_transaction_begin(app->db)) {
        /* The busy timeout has already been waited out once: no card is
         * written on its own, each waiting it out again. */
        task_app_write_failed(app, "The move");
        free(alive);
        return true;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < n_moving; i++) {
        TaskStatus shown_in;
        size_t     index;
        if (!task_board_find(board, moving[i], &shown_in, &index)) continue;

        Task *task  = task_db_task_get(app->db, moving[i]);
        bool  gone  = task == NULL || task->deleted;
        bool  moves = !gone && task->status != lane;
        task_free(task);
        if (gone) continue;

        alive[n_alive++] = moving[i];
        if (!moves) continue;
        ok = task_db_task_set_status(app->db, moving[i], lane);
        if (n_status++ == 0) one_title = board->lanes[shown_in].cards[index]->title;
    }
    if (!task_db_transaction_end(app->db, ok)) {
        task_app_write_failed(app, "The move");   /* rolled back: nothing moved */
        free(alive);
        return true;
    }

    /* The order half: nothing to save when the order shown already is it. */
    size_t   count = 0;
    int64_t *order = save_order
        ? task_board_reorder(board, alive, n_alive, lane, slot, &count) : NULL;
    if (order != NULL) task_order_save(TASK_CFG_FAMILY_KANBAN, sel, order, (int)count);

    if (n_status == 1) {
        task_app_status(app, "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 %s", one_title,
                        task_status_label(lane));
    } else if (n_status > 1) {
        task_app_status(app, "%zu tasks \xe2\x80\x94 %s", n_status, task_status_label(lane));
    }

    bool changed = n_status > 0 || order != NULL;
    free(order);
    free(alive);
    return changed;
}
