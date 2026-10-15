/*
 * board_test.c — the Kanban board's logic (src/task_board.h): dealing rows
 * into lanes, the saved order, the Done lane's own order and cap, finding a
 * card, the insertion slot under a pointer, the order a drop asks for, and
 * the drop itself against a real database and config.
 *
 * The drop arithmetic is tested case by case because it is where the bugs
 * live: the GTK build inserted at the raw slot after lifting the moving
 * cards out, which landed a card dragged downwards one place too low.
 */

#include "fixture.h"
#include "task_board.h"
#include "task_order.h"

#include <string.h>

/* The most rows a hand-made row list holds. */
#define MAX_ROWS 16

/* The ids of the cards the reorder tests use: New holds A B C, In Progress
 * holds D E, Done holds F. */
enum { A = 1, B, C, D, E, F };

/* Backing store of the hand-made row list (one at a time). */
static task_row_t  row_store[MAX_ROWS];
static task_row_t *row_ptrs[MAX_ROWS];

/* The last status message posted, as the status listener saw it. */
static char last_message[128];

/*
 * rows_of — a row list made by hand: the board logic reads only a row's
 * id, status, completion stamp and title, so no database is needed.
 *
 * Inputs:
 *   n         — how many rows (at most MAX_ROWS)
 *   ids       — their task ids
 *   statuses  — their statuses
 *   completed — their completed_at stamps, or NULL for all zero
 *
 * Output:
 *   the list, valid until the next call.
 */
static task_row_list_t
rows_of(size_t n, const int64_t *ids, const TaskStatus *statuses, const int64_t *completed)
{
    task_row_list_t list = { row_ptrs, n };

    memset(row_store, 0, sizeof row_store);
    for (size_t i = 0; i < n; i++) {
        row_store[i].id           = ids[i];
        row_store[i].status       = statuses[i];
        row_store[i].completed_at = completed != NULL ? completed[i] : 0;
        row_ptrs[i]               = &row_store[i];
    }
    return list;
}

/*
 * lane_is — whether a lane shows exactly these cards, top to bottom.
 *
 * Inputs:
 *   board — the board
 *   lane  — the lane
 *   want  — the expected ids
 *   n     — how many
 *
 * Output:
 *   non-zero when they match.
 */
static int
lane_is(const task_board_t *board, TaskStatus lane, const int64_t *want, size_t n)
{
    if (board->lanes[lane].count != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (board->lanes[lane].cards[i]->id != want[i]) return 0;
    }
    return 1;
}

/*
 * reorder_is — whether dropping cards at a slot asks for the expected
 * order.
 *
 * Inputs:
 *   board    — the board
 *   moving   — the ids dropped
 *   n_moving — how many
 *   lane     — the lane dropped on
 *   slot     — the slot
 *   want     — the expected order, or NULL for "nothing moved"
 *   n_want   — its length
 *
 * Output:
 *   non-zero when the result matches.
 */
static int
reorder_is(const task_board_t *board, const int64_t *moving, size_t n_moving,
           TaskStatus lane, size_t slot, const int64_t *want, size_t n_want)
{
    size_t   count = 0;
    int64_t *order = task_board_reorder(board, moving, n_moving, lane, slot, &count);
    int      ok;

    if (want == NULL) {
        ok = order == NULL;
    } else {
        ok = order != NULL && count == n_want &&
             memcmp(order, want, n_want * sizeof *want) == 0;
    }
    free(order);
    return ok;
}

/*
 * on_status — the status listener: keep the message.
 *
 * Inputs:
 *   app     — the context (unused)
 *   message — the message (borrowed)
 *   data    — unused
 */
static void
on_status(TaskApp *app, const gchar *message, gpointer data)
{
    (void)app;
    (void)data;
    snprintf(last_message, sizeof last_message, "%s", message);
}

/*
 * test_build — rows are dealt into lanes by status, in the saved order;
 * the Done lane sorts and caps itself.
 */
static void
test_build(void)
{
    task_sel_t       sel        = { task_sel_tag, 900 };
    const int64_t    ids[]      = { 1, 2, 3, 4, 5, 6, 7 };
    const TaskStatus statuses[] = {
        TASK_STATUS_NEW, TASK_STATUS_DONE, TASK_STATUS_IN_PROGRESS, TASK_STATUS_NEW,
        TASK_STATUS_DONE, (TaskStatus)9, TASK_STATUS_DONE,
    };
    const int64_t    completed[] = { 0, 100, 0, 0, 300, 0, 100 };
    task_row_list_t  rows        = rows_of(7, ids, statuses, completed);

    task_row_list_t none  = { NULL, 0 };
    task_board_t   *board = task_board_build(sel, &none, SIZE_MAX);
    check(board->lanes[TASK_STATUS_NEW].count == 0 && board->lanes[TASK_STATUS_DONE].total == 0,
          "build: no rows is three empty lanes");
    task_board_free(board);

    board = task_board_build(sel, &rows, SIZE_MAX);
    check(lane_is(board, TASK_STATUS_NEW, (int64_t[]){ 1, 4, 6 }, 3),
          "build: New in query order; a status outside the enum lands in New");
    check(lane_is(board, TASK_STATUS_IN_PROGRESS, (int64_t[]){ 3 }, 1), "build: In Progress");
    check(lane_is(board, TASK_STATUS_DONE, (int64_t[]){ 5, 2, 7 }, 3),
          "build: Done is most recently completed first; equal stamps keep their order");
    check(board->lanes[TASK_STATUS_NEW].total == 3 && board->lanes[TASK_STATUS_DONE].total == 3,
          "build: an uncapped lane's total is its count");
    task_board_free(board);

    /* The cap hides cards but not their number. */
    board = task_board_build(sel, &rows, 2);
    check(lane_is(board, TASK_STATUS_DONE, (int64_t[]){ 5, 2 }, 2) &&
          board->lanes[TASK_STATUS_DONE].total == 3,
          "build: the Done cap keeps the most recent and reports the total");
    check(board->lanes[TASK_STATUS_NEW].count == 3, "build: the cap is Done's alone");
    task_board_free(board);

    /* A saved order: ids it names come first, a stale id is skipped, Done
     * ignores it. */
    task_order_save(TASK_CFG_FAMILY_KANBAN, sel, (int64_t[]){ 7, 6, 99, 3, 1 }, 5);
    board = task_board_build(sel, &rows, SIZE_MAX);
    check(lane_is(board, TASK_STATUS_NEW, (int64_t[]){ 6, 1, 4 }, 3),
          "build: the saved order is applied; unmentioned cards follow");
    check(lane_is(board, TASK_STATUS_DONE, (int64_t[]){ 5, 7, 2 }, 3),
          "build: Done still sorts by completion; the saved order only breaks its ties");
    task_board_free(board);

    /* The order belongs to the selection it was saved for. */
    task_sel_t other = { task_sel_tag, 901 };
    board = task_board_build(other, &rows, SIZE_MAX);
    check(lane_is(board, TASK_STATUS_NEW, (int64_t[]){ 1, 4, 6 }, 3),
          "build: another selection does not see that order");
    task_board_free(board);
    task_order_drop(sel);
}

/*
 * test_find — a card is found by id, with its lane and position; a capped
 * card is not on the board.
 */
static void
test_find(void)
{
    task_sel_t       sel        = { task_sel_tag, 900 };
    const int64_t    ids[]      = { 1, 2, 3, 4 };
    const TaskStatus statuses[] = {
        TASK_STATUS_NEW, TASK_STATUS_DONE, TASK_STATUS_NEW, TASK_STATUS_DONE,
    };
    const int64_t    completed[] = { 0, 50, 0, 60 };
    task_row_list_t  rows        = rows_of(4, ids, statuses, completed);
    task_board_t    *board       = task_board_build(sel, &rows, 1);
    TaskStatus       lane        = TASK_STATUS_IN_PROGRESS;
    size_t           index       = 99;

    check(task_board_find(board, 3, &lane, &index) && lane == TASK_STATUS_NEW && index == 1,
          "find: lane and position");
    check(task_board_find(board, 4, &lane, NULL) && lane == TASK_STATUS_DONE,
          "find: the outputs are optional");
    check(!task_board_find(board, 2, &lane, &index) && lane == TASK_STATUS_DONE,
          "find: a Done card hidden by the cap is not on the board; outputs untouched");
    check(!task_board_find(board, 77, NULL, NULL), "find: an unknown id");
    task_board_free(board);
}

/*
 * test_slot — the two-half rule: the slot changes at a card's middle line.
 */
static void
test_slot(void)
{
    const double mids[] = { 20.0, 60.0, 100.0 };

    check(task_board_slot_at(NULL, 0, 50.0) == 0, "slot: an empty lane has only slot 0");
    check(task_board_slot_at(mids, 3, -5.0) == 0, "slot: above the lane");
    check(task_board_slot_at(mids, 3, 19.9) == 0, "slot: the first card's upper half");
    check(task_board_slot_at(mids, 3, 20.0) == 1, "slot: exactly on a middle line is below");
    check(task_board_slot_at(mids, 3, 39.0) == 1, "slot: the first card's lower half");
    check(task_board_slot_at(mids, 3, 41.0) == 1, "slot: the gap between two cards");
    check(task_board_slot_at(mids, 3, 59.9) == 1, "slot: the second card's upper half");
    check(task_board_slot_at(mids, 3, 99.0) == 2, "slot: the last card's upper half");
    check(task_board_slot_at(mids, 3, 100.5) == 3, "slot: the last card's lower half");
    check(task_board_slot_at(mids, 3, 5000.0) == 3, "slot: far below the last card");
}

/*
 * test_reorder — the order a drop asks for, case by case.  The board shows
 * New = A B C, In Progress = D E, Done = F.
 */
static void
test_reorder(void)
{
    task_sel_t       sel        = { task_sel_tag, 900 };
    const int64_t    ids[]      = { A, B, C, D, E, F };
    const TaskStatus statuses[] = {
        TASK_STATUS_NEW, TASK_STATUS_NEW, TASK_STATUS_NEW,
        TASK_STATUS_IN_PROGRESS, TASK_STATUS_IN_PROGRESS, TASK_STATUS_DONE,
    };
    task_row_list_t  rows  = rows_of(6, ids, statuses, NULL);
    task_board_t    *board = task_board_build(sel, &rows, SIZE_MAX);
    const TaskStatus new_lane = TASK_STATUS_NEW;
    const TaskStatus ip_lane  = TASK_STATUS_IN_PROGRESS;

    /* One card within its lane: every slot. */
    check(reorder_is(board, (int64_t[]){ A }, 1, new_lane, 0, NULL, 0),
          "reorder: the top card dropped above itself has not moved");
    check(reorder_is(board, (int64_t[]){ A }, 1, new_lane, 1, NULL, 0),
          "reorder: ... nor dropped just below itself");
    check(reorder_is(board, (int64_t[]){ A }, 1, new_lane, 2, (int64_t[]){ B, A, C, D, E }, 5),
          "reorder: down one place lands between its two neighbours");
    check(reorder_is(board, (int64_t[]){ A }, 1, new_lane, 3, (int64_t[]){ B, C, A, D, E }, 5),
          "reorder: down to the end");
    check(reorder_is(board, (int64_t[]){ A }, 1, new_lane, 99, (int64_t[]){ B, C, A, D, E }, 5),
          "reorder: a slot past the end means the end");
    check(reorder_is(board, (int64_t[]){ B }, 1, new_lane, 0, (int64_t[]){ B, A, C, D, E }, 5),
          "reorder: the middle card up one");
    check(reorder_is(board, (int64_t[]){ B }, 1, new_lane, 1, NULL, 0) &&
          reorder_is(board, (int64_t[]){ B }, 1, new_lane, 2, NULL, 0),
          "reorder: the middle card on either side of itself has not moved");
    check(reorder_is(board, (int64_t[]){ B }, 1, new_lane, 3, (int64_t[]){ A, C, B, D, E }, 5),
          "reorder: the middle card down one");
    check(reorder_is(board, (int64_t[]){ C }, 1, new_lane, 0, (int64_t[]){ C, A, B, D, E }, 5),
          "reorder: the bottom card to the top");
    check(reorder_is(board, (int64_t[]){ C }, 1, new_lane, 1, (int64_t[]){ A, C, B, D, E }, 5),
          "reorder: the bottom card up one");
    check(reorder_is(board, (int64_t[]){ C }, 1, new_lane, 2, NULL, 0) &&
          reorder_is(board, (int64_t[]){ C }, 1, new_lane, 3, NULL, 0),
          "reorder: the bottom card on either side of itself has not moved");

    /* Several neighbouring cards. */
    check(reorder_is(board, (int64_t[]){ A, B }, 2, new_lane, 0, NULL, 0) &&
          reorder_is(board, (int64_t[]){ A, B }, 2, new_lane, 1, NULL, 0) &&
          reorder_is(board, (int64_t[]){ A, B }, 2, new_lane, 2, NULL, 0),
          "reorder: a block dropped anywhere on itself has not moved");
    check(reorder_is(board, (int64_t[]){ A, B }, 2, new_lane, 3, (int64_t[]){ C, A, B, D, E }, 5),
          "reorder: a block moved below the next card keeps its own order");
    check(reorder_is(board, (int64_t[]){ B, C }, 2, new_lane, 0, (int64_t[]){ B, C, A, D, E }, 5),
          "reorder: a block moved to the top");

    /* Cards that were apart are gathered at the slot. */
    check(reorder_is(board, (int64_t[]){ A, C }, 2, new_lane, 0, (int64_t[]){ A, C, B, D, E }, 5) &&
          reorder_is(board, (int64_t[]){ A, C }, 2, new_lane, 1, (int64_t[]){ A, C, B, D, E }, 5),
          "reorder: separated cards gather above the card between them");
    check(reorder_is(board, (int64_t[]){ A, C }, 2, new_lane, 2, (int64_t[]){ B, A, C, D, E }, 5) &&
          reorder_is(board, (int64_t[]){ A, C }, 2, new_lane, 3, (int64_t[]){ B, A, C, D, E }, 5),
          "reorder: ... or below it");
    check(reorder_is(board, (int64_t[]){ C, A }, 2, new_lane, 3, (int64_t[]){ B, C, A, D, E }, 5),
          "reorder: the moving cards go back in the order given");
    check(reorder_is(board, (int64_t[]){ A, B, C }, 3, new_lane, 2, NULL, 0),
          "reorder: a whole lane dropped on itself has not moved");

    /* Across lanes. */
    check(reorder_is(board, (int64_t[]){ A }, 1, ip_lane, 0, (int64_t[]){ B, C, A, D, E }, 5),
          "reorder: to the top of the other lane");
    check(reorder_is(board, (int64_t[]){ A }, 1, ip_lane, 1, (int64_t[]){ B, C, D, A, E }, 5),
          "reorder: into the middle of the other lane");
    check(reorder_is(board, (int64_t[]){ A }, 1, ip_lane, 2, (int64_t[]){ B, C, D, E, A }, 5),
          "reorder: to the end of the other lane");
    check(reorder_is(board, (int64_t[]){ D }, 1, new_lane, 1, (int64_t[]){ A, D, B, C, E }, 5),
          "reorder: from In Progress back into New");
    check(reorder_is(board, (int64_t[]){ C }, 1, ip_lane, 0, NULL, 0),
          "reorder: New's last card to In Progress's top leaves the flat order as it was");
    check(reorder_is(board, (int64_t[]){ D }, 1, new_lane, 3, NULL, 0),
          "reorder: ... as does In Progress's top card to New's end");
    check(reorder_is(board, (int64_t[]){ B, D }, 2, ip_lane, 1, (int64_t[]){ A, C, B, D, E }, 5),
          "reorder: cards from both lanes gather at the slot");
    check(reorder_is(board, (int64_t[]){ B, D }, 2, ip_lane, 2, (int64_t[]){ A, C, E, B, D }, 5),
          "reorder: ... at the end of the lane");

    /* A card coming out of Done joins the order. */
    check(reorder_is(board, (int64_t[]){ F }, 1, new_lane, 1, (int64_t[]){ A, F, B, C, D, E }, 6),
          "reorder: a Done card dropped into New is inserted");
    check(reorder_is(board, (int64_t[]){ F }, 1, ip_lane, 2, (int64_t[]){ A, B, C, D, E, F }, 6),
          "reorder: ... or appended to In Progress");

    /* Refusals. */
    size_t count = 7;
    check(task_board_reorder(board, (int64_t[]){ A }, 1, TASK_STATUS_DONE, 0, &count) == NULL &&
          count == 7, "reorder: Done keeps no order; count untouched");
    check(task_board_reorder(board, (int64_t[]){ A }, 1, (TaskStatus)5, 0, &count) == NULL,
          "reorder: a lane outside the enum");
    check(task_board_reorder(board, NULL, 0, new_lane, 0, &count) == NULL,
          "reorder: nothing moving");
    task_board_free(board);

    /* An empty lane has one slot. */
    rows  = rows_of(3, ids, statuses, NULL);
    board = task_board_build(sel, &rows, SIZE_MAX);
    check(reorder_is(board, (int64_t[]){ B }, 1, ip_lane, 0, (int64_t[]){ A, C, B }, 3) &&
          reorder_is(board, (int64_t[]){ B }, 1, ip_lane, 4, (int64_t[]){ A, C, B }, 3),
          "reorder: into an empty lane, whatever the slot");
    check(reorder_is(board, (int64_t[]){ C }, 1, ip_lane, 0, NULL, 0),
          "reorder: the last New card into an empty In Progress keeps the flat order");
    task_board_free(board);
}

/*
 * shown — collect a list's rows and build its board, as a refresh does.
 *
 * Inputs:
 *   app     — the context
 *   sel     — the list's selection
 *   content — receives the pane content that owns the rows; free it after
 *             the board
 *
 * Output:
 *   new board, uncapped.
 */
static task_board_t *
shown(TaskApp *app, task_sel_t sel, task_pane_content_t **content)
{
    *content = task_pane_collect(app, sel, NULL);
    return task_board_build(sel, (*content)->rows, SIZE_MAX);
}

/*
 * status_of — a task's status in the database.
 *
 * Inputs:
 *   app — the context
 *   id  — the task
 *
 * Output:
 *   the status.
 */
static TaskStatus
status_of(TaskApp *app, int64_t id)
{
    Task      *task   = task_db_task_get(app->db, id);
    TaskStatus status = task->status;

    task_free(task);
    return status;
}

/*
 * updated_of — a task's updated_at stamp in the database.
 *
 * Inputs:
 *   app — the context
 *   id  — the task
 *
 * Output:
 *   the stamp.
 */
static int64_t
updated_of(TaskApp *app, int64_t id)
{
    Task   *task  = task_db_task_get(app->db, id);
    int64_t stamp = task->updated_at;

    task_free(task);
    return stamp;
}

/*
 * saved_is — whether the list's saved board order is exactly this text.
 *
 * Inputs:
 *   key  — the config key
 *   want — the expected value, or NULL for "no key"
 *
 * Output:
 *   non-zero when it matches.
 */
static int
saved_is(const char *key, const char *want)
{
    gchar *saved = task_config_get(key);
    int    ok    = want == NULL ? saved == NULL : saved != NULL && strcmp(saved, want) == 0;

    g_free(saved);
    return ok;
}

/*
 * test_drop — the drop against a real database and config: the status
 * half, the order half, the message, and the cases where nothing happens.
 *
 * Inputs:
 *   app — the context
 */
static void
test_drop(TaskApp *app)
{
    char       key[64];
    char       want[128];
    guint      listener = task_app_listen_status(app, on_status, NULL);

    task_db_task_create(app->db, 0, "One", "Board");
    task_db_task_create(app->db, 0, "Two", "Board");
    task_db_task_create(app->db, 0, "Three", "Board");
    task_db_task_create(app->db, 0, "", "Board");
    TaskTag   *tag = task_db_tag_find(app->db, "Board");
    task_sel_t sel = { task_sel_tag, tag->id };
    snprintf(key, sizeof key, "kanban_order_tag_%lld", (long long)tag->id);
    task_tag_free(tag);

    /* The four cards as the board shows them, top to bottom. */
    task_pane_content_t *content;
    task_board_t        *board = shown(app, sel, &content);
    int64_t              c[4];
    check(board->lanes[TASK_STATUS_NEW].count == 4, "drop: four New cards to start from");
    for (size_t i = 0; i < 4; i++) c[i] = board->lanes[TASK_STATUS_NEW].cards[i]->id;

    /* Dropped where it was: nothing is written. */
    last_message[0] = '\0';
    check(!task_board_drop(app, sel, board, &c[1], 1, TASK_STATUS_NEW, 1, true) &&
          !task_board_drop(app, sel, board, &c[1], 1, TASK_STATUS_NEW, 2, true),
          "drop: a card dropped on its own place is a no-op");
    check(saved_is(key, NULL) && last_message[0] == '\0',
          "drop: ... which saves no order and says nothing");

    /* A pure reorder: the order is saved, no task row is touched.  The
     * stamps are whole seconds, so let one pass first. */
    int64_t stamp = updated_of(app, c[0]);
    check_spin(1.1);
    check(task_board_drop(app, sel, board, &c[0], 1, TASK_STATUS_NEW, 2, true),
          "drop: a reorder reports a change");
    snprintf(want, sizeof want, "%lld,%lld,%lld,%lld", (long long)c[1], (long long)c[0],
             (long long)c[2], (long long)c[3]);
    check(saved_is(key, want), "drop: the new order is saved under the list's board key");
    check(updated_of(app, c[0]) == stamp && status_of(app, c[0]) == TASK_STATUS_NEW,
          "drop: a reorder does not touch the task row");
    check(last_message[0] == '\0', "drop: a reorder posts no message");
    task_board_free(board);
    task_pane_content_free(content);

    /* The saved order is what the next refresh shows. */
    board = shown(app, sel, &content);
    check(lane_is(board, TASK_STATUS_NEW, (int64_t[]){ c[1], c[0], c[2], c[3] }, 4),
          "drop: the next refresh shows the saved order");

    /* One card to another lane: status and order, and the message. */
    check(task_board_drop(app, sel, board, &c[2], 1, TASK_STATUS_IN_PROGRESS, 0, true),
          "drop: a move to In Progress reports a change");
    check(status_of(app, c[2]) == TASK_STATUS_IN_PROGRESS, "drop: the status is written");
    check(strcmp(last_message, "\xe2\x80\x9cThree\xe2\x80\x9d \xe2\x80\x94 In Progress") == 0,
          "drop: one moved task is named in the message");
    snprintf(want, sizeof want, "%lld,%lld,%lld,%lld", (long long)c[1], (long long)c[0],
             (long long)c[3], (long long)c[2]);
    check(saved_is(key, want), "drop: the order follows the card into its new lane");
    task_board_free(board);
    task_pane_content_free(content);

    /* With a search up the order half is skipped, the status half is not. */
    board = shown(app, sel, &content);
    last_message[0] = '\0';
    check(!task_board_drop(app, sel, board, &c[1], 1, TASK_STATUS_NEW, 3, false),
          "drop: a reorder while a search filters the board is refused");
    check(task_board_drop(app, sel, board, &c[3], 1, TASK_STATUS_IN_PROGRESS, 0, false) &&
          status_of(app, c[3]) == TASK_STATUS_IN_PROGRESS,
          "drop: a move while a search filters the board still changes the status");
    check(saved_is(key, want), "drop: ... and leaves the saved order alone");
    check(strcmp(last_message, "\xe2\x80\x9cUntitled Task\xe2\x80\x9d \xe2\x80\x94 In Progress") == 0,
          "drop: an untitled task is named as the rows name it");
    task_board_free(board);
    task_pane_content_free(content);

    /* Several cards to Done: every status, the count in the message, and
     * no order saved (Done keeps none). */
    board = shown(app, sel, &content);
    int64_t pair[] = { c[1], c[2] };
    check(task_board_drop(app, sel, board, pair, 2, TASK_STATUS_DONE, 0, true) &&
          status_of(app, c[1]) == TASK_STATUS_DONE && status_of(app, c[2]) == TASK_STATUS_DONE,
          "drop: several cards to Done are all completed");
    check(strcmp(last_message, "2 tasks \xe2\x80\x94 Done") == 0,
          "drop: several moved tasks are counted in the message");
    check(saved_is(key, want), "drop: a drop on Done saves no order");
    task_board_free(board);
    task_pane_content_free(content);

    /* A mixed drop: one card already in the lane, one arriving.  Only the
     * arriving one is written and counted. */
    board = shown(app, sel, &content);
    int64_t mixed[] = { c[0], c[3] };
    last_message[0] = '\0';
    check(task_board_drop(app, sel, board, mixed, 2, TASK_STATUS_NEW, 0, true) &&
          status_of(app, c[3]) == TASK_STATUS_NEW,
          "drop: a card joining cards already in the lane changes status");
    check(strcmp(last_message, "\xe2\x80\x9cUntitled Task\xe2\x80\x9d \xe2\x80\x94 New") == 0,
          "drop: only the card that changed lane is reported");
    check(saved_is(key, want),
          "drop: the flat order did not change (the cards were already adjacent), so none is saved");
    task_board_free(board);
    task_pane_content_free(content);

    /* Tasks that have gone, and ids that are not cards, take no part. */
    board = shown(app, sel, &content);
    check(lane_is(board, TASK_STATUS_NEW, (int64_t[]){ c[0], c[3] }, 2),
          "drop: ... and the next refresh shows the two cards in the lane, in that order");
    task_db_task_delete(app->db, c[3]);
    last_message[0] = '\0';
    check(!task_board_drop(app, sel, board, &c[3], 1, TASK_STATUS_IN_PROGRESS, 0, true),
          "drop: a card whose task was deleted meanwhile is a no-op");
    int64_t stranger = 987654;
    check(!task_board_drop(app, sel, board, &stranger, 1, TASK_STATUS_IN_PROGRESS, 0, true),
          "drop: an id that is not a card is a no-op");
    int64_t both[] = { c[3], c[0] };
    check(task_board_drop(app, sel, board, both, 2, TASK_STATUS_IN_PROGRESS, 0, true) &&
          status_of(app, c[0]) == TASK_STATUS_IN_PROGRESS,
          "drop: the surviving card of a pair still moves");
    check(strcmp(last_message, "\xe2\x80\x9cOne\xe2\x80\x9d \xe2\x80\x94 In Progress") == 0,
          "drop: ... and is the one reported");
    snprintf(want, sizeof want, "%lld,%lld", (long long)c[3], (long long)c[0]);
    check(saved_is(key, want),
          "drop: the order saved is of the cards on screen (a stale id is harmless when read)");

    /* Refusals. */
    check(!task_board_drop(app, sel, board, &c[0], 1, (TaskStatus)7, 0, true) &&
          !task_board_drop(app, sel, board, &c[0], 0, TASK_STATUS_NEW, 0, true),
          "drop: a lane outside the enum, or nothing moving, is a no-op");
    task_board_free(board);
    task_pane_content_free(content);

    /* Out of Done and back into a lane, at a slot. */
    board = shown(app, sel, &content);
    check(task_board_drop(app, sel, board, &c[1], 1, TASK_STATUS_IN_PROGRESS, 0, true) &&
          status_of(app, c[1]) == TASK_STATUS_IN_PROGRESS,
          "drop: a Done card dragged back to In Progress is reopened");
    snprintf(want, sizeof want, "%lld,%lld", (long long)c[1], (long long)c[0]);
    check(saved_is(key, want), "drop: ... and takes the slot it was dropped at");
    task_board_free(board);
    task_pane_content_free(content);

    task_app_unlisten(app, listener);
}

/*
 * main — run the board tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    test_build();
    test_find();
    test_slot();
    test_reorder();
    test_drop(app);

    fixture_close(app);
    return check_result();
}
