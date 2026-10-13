/*
 * rows_test.c — the toolkit-neutral row description (src/task_rows.h) and
 * the pane collection built on it (src/task_pane.h): which lines a task
 * gets, their emphasis, the notes-preview rules, the completed filter, the
 * search filter and the status line.
 */

#include "fixture.h"
#include "task_pane.h"
#include "task_rows.h"

#include <string.h>

/*
 * line_of — a row's first line of a given kind.
 *
 * Inputs:
 *   row  — the row
 *   kind — the wanted line kind
 *
 * Output:
 *   the line, or NULL when the row has none of that kind.
 */
static const task_row_line_t *
line_of(const task_row_t *row, task_row_line_kind_t kind)
{
    for (size_t i = 0; i < row->line_count; i++) {
        if (row->lines[i].kind == kind) return &row->lines[i];
    }
    return NULL;
}

/*
 * count_of — how many lines of a kind a row has.
 *
 * Inputs:
 *   row  — the row
 *   kind — the line kind
 *
 * Output:
 *   the count.
 */
static size_t
count_of(const task_row_t *row, task_row_line_kind_t kind)
{
    size_t n = 0;

    for (size_t i = 0; i < row->line_count; i++) {
        if (row->lines[i].kind == kind) n++;
    }
    return n;
}

/*
 * set_notes — replace a task's notes.
 *
 * Inputs:
 *   app   — the context
 *   id    — the task
 *   notes — the new notes
 */
static void
set_notes(TaskApp *app, gint64 id, const char *notes)
{
    Task *before = task_db_task_get(app->db, id);
    Task  after  = *before;

    after.notes = (gchar *)notes;
    task_db_task_update(app->db, before, &after);
    task_free(before);
}

/*
 * describe — describe one task through a fresh context.
 *
 * Inputs:
 *   app          — the context
 *   id           — the task
 *   virtual_view — whether rows carry their "in <list>" line
 *
 * Output:
 *   new row (task_row_free).
 */
static task_row_t *
describe(TaskApp *app, gint64 id, gboolean virtual_view)
{
    TaskRowCtx ctx;
    Task      *t = task_db_task_get(app->db, id);

    task_row_ctx_init(app, &ctx, virtual_view);
    task_row_t *row = task_row_describe(t, &ctx);
    task_row_ctx_clear(&ctx);
    task_free(t);
    return row;
}

/*
 * test_title_line — the title line's text, glyph stack and emphasis.
 *
 * Inputs:
 *   app  — the context
 *   list — a list to create tasks in
 */
static void
test_title_line(TaskApp *app, gint64 list)
{
    gint64      id  = task_db_task_create(app->db, list, 0, "Plain task");
    task_row_t *row = describe(app, id, FALSE);

    check(row->id == id && row->status == TASK_STATUS_NEW, "title: id and status carried");
    check(strcmp(row->title, "Plain task") == 0, "title: plain title carried");
    check(row->line_count == 1, "title: a bare task is one line");
    check(row->lines[0].kind == task_row_line_title, "title: the first line is the title");
    check(strcmp(row->lines[0].text, "Plain task") == 0, "title: text");
    check(strcmp(row->lines[0].prefix, "") == 0, "title: no glyphs on a plain task");
    check(!row->lines[0].bold && !row->lines[0].strike && !row->lines[0].small &&
          !row->lines[0].italic && row->lines[0].alpha_pct == TASK_ROW_ALPHA_FULL,
          "title: no emphasis by default");
    check(strcmp(row->status_text, "New") == 0, "title: status label");
    check(strcmp(row->due_text, "") == 0 && row->due_urgency == task_due_none,
          "title: no due date, no tint");
    check(line_of(row, task_row_line_list) == NULL, "title: no list line in a list view");
    task_row_free(row);

    /* Favourite + priority + done, with the bold setting on. */
    task_db_task_set_pinned(app->db, id, TRUE);
    task_db_task_set_priority(app->db, id, TRUE);
    task_db_task_set_status(app->db, id, TASK_STATUS_DONE);
    task_config_set_bool(TASK_CFG_BOLD_TITLES, TRUE);
    row = describe(app, id, FALSE);
    check(strcmp(row->lines[0].prefix,
                 "\xf0\x9f\x9a\xa8  \xe2\xad\x90\xef\xb8\x8f  ") == 0,
          "title: siren then star, each with two spaces");
    check(row->lines[0].strike, "title: a Done task is struck through");
    check(row->lines[0].bold, "title: bold follows the setting");
    check(strcmp(row->lines[0].text, "Plain task") == 0,
          "title: the glyphs are in the prefix, not the text");
    check(row->completed_at > 0 && row->completed_text[0] != '\0',
          "title: completion stamp and text");
    check(strcmp(row->status_text, "Done") == 0, "title: Done label");
    task_row_free(row);
    task_config_set_bool(TASK_CFG_BOLD_TITLES, FALSE);

    /* An empty title gets a stand-in. */
    gint64 blank = task_db_task_create(app->db, list, 0, "");
    row = describe(app, blank, FALSE);
    check(strcmp(row->title, "Untitled Task") == 0 &&
          strcmp(row->lines[0].text, "Untitled Task") == 0,
          "title: an empty title reads Untitled Task");
    task_row_free(row);

    /* Bytes that are not UTF-8 are replaced, not passed on. */
    gint64 bad = task_db_task_create(app->db, list, 0, "caf\xe9 run");
    row = describe(app, bad, FALSE);
    check(g_utf8_validate(row->lines[0].text, -1, NULL) &&
          g_utf8_validate(row->title, -1, NULL),
          "title: invalid UTF-8 from the database is made valid");
    task_row_free(row);
}

/*
 * test_notes_line — the notes preview's content gate, cap and ellipsis.
 *
 * Inputs:
 *   app  — the context
 *   list — a list to create tasks in
 */
static void
test_notes_line(TaskApp *app, gint64 list)
{
    gint64                 id = task_db_task_create(app->db, list, 0, "Notes");
    task_row_t            *row;
    const task_row_line_t *line;

    set_notes(app, id, "  \n\xc2\xa0\n");
    row = describe(app, id, FALSE);
    check(line_of(row, task_row_line_notes) == NULL,
          "notes: blank lines (even a no-break space) earn no preview");
    task_row_free(row);

    set_notes(app, id, "\n\n   first real line  \nsecond line");
    row  = describe(app, id, FALSE);
    line = line_of(row, task_row_line_notes);
    check(line != NULL && strcmp(line->text, "first real line\xe2\x80\xa6") == 0,
          "notes: first non-blank line, trimmed, ellipsis because more follows");
    check(line != NULL && line->small && line->alpha_pct == TASK_ROW_ALPHA_DETAIL &&
          !line->strike && !line->bold,
          "notes: small and dimmed by opacity");
    task_row_free(row);

    set_notes(app, id, "only line");
    row  = describe(app, id, FALSE);
    line = line_of(row, task_row_line_notes);
    check(line != NULL && strcmp(line->text, "only line") == 0,
          "notes: no ellipsis when nothing follows");
    task_row_free(row);

    /* 119 ASCII bytes then a 2-byte character straddling the 120-byte cap:
     * the cut must back up to the character boundary. */
    char longline[200];
    memset(longline, 'a', 119);
    strcpy(longline + 119, "\xc3\xa9 tail");
    set_notes(app, id, longline);
    row  = describe(app, id, FALSE);
    line = line_of(row, task_row_line_notes);
    check(line != NULL && g_utf8_validate(line->text, -1, NULL),
          "notes: the cap never splits a character");
    check(line != NULL && strlen(line->text) == 119 + 3,
          "notes: the straddling character is dropped, ellipsis added");
    task_row_free(row);
}

/*
 * test_detail_lines — attachments, subtasks, "+N more" and the list line.
 *
 * Inputs:
 *   app  — the context
 *   list — a list to create tasks in
 */
static void
test_detail_lines(TaskApp *app, gint64 list)
{
    gint64 id = task_db_task_create(app->db, list, 0, "Parent");

    task_db_attachment_add(app->db, id, "/tmp/a.pdf");
    task_row_t            *row  = describe(app, id, TRUE);
    const task_row_line_t *line = line_of(row, task_row_line_attachments);
    check(line != NULL && strcmp(line->text, "\xf0\x9f\x93\x8e 1 attachment") == 0,
          "attachments: singular");
    task_row_free(row);

    task_db_attachment_add(app->db, id, "/tmp/b.pdf");
    row  = describe(app, id, TRUE);
    line = line_of(row, task_row_line_attachments);
    check(line != NULL && strcmp(line->text, "\xf0\x9f\x93\x8e 2 attachments") == 0,
          "attachments: plural");
    line = line_of(row, task_row_line_list);
    check(line != NULL && strcmp(line->text, "in Rows") == 0 && line->italic &&
          line->small && line->alpha_pct == TASK_ROW_ALPHA_LIST,
          "list line: \"in <list>\" in a virtual view, italic and dimmed");
    task_row_free(row);

    gint64 subs[6];
    for (int i = 0; i < 6; i++) {
        char title[32];
        snprintf(title, sizeof title, "Sub %d", i + 1);
        subs[i] = task_db_task_create(app->db, list, id, title);
    }
    task_db_task_set_status(app->db, subs[1], TASK_STATUS_DONE);

    row = describe(app, id, FALSE);
    check(count_of(row, task_row_line_subtask) == TASK_ROW_MAX_SUBTASKS,
          "subtasks: at most four lines");
    line = line_of(row, task_row_line_more);
    check(line != NULL && strcmp(line->text, "\xe2\x80\xa6 +2 more subtasks") == 0,
          "subtasks: the rest are counted");

    const task_row_line_t *open = NULL;
    const task_row_line_t *done = NULL;
    for (size_t i = 0; i < row->line_count; i++) {
        if (row->lines[i].kind != task_row_line_subtask) continue;
        if (row->lines[i].strike) done = &row->lines[i]; else if (open == NULL) open = &row->lines[i];
    }
    check(open != NULL && strcmp(open->prefix, "\xe2\x98\x90 ") == 0 &&
          open->alpha_pct == TASK_ROW_ALPHA_FULL,
          "subtasks: an open one has an empty box and full opacity");
    check(done != NULL && strcmp(done->prefix, "\xe2\x98\x91 ") == 0 &&
          done->alpha_pct == TASK_ROW_ALPHA_SUB_DONE && strcmp(done->text, "Sub 2") == 0,
          "subtasks: a done one is ticked, struck and dimmed");
    check(row->status == TASK_STATUS_IN_PROGRESS,
          "subtasks: completing one started the parent");
    task_row_free(row);

    /* A subtask shown as its own row wears the arrow and has no sub-lines. */
    row = describe(app, subs[0], TRUE);
    check(strncmp(row->lines[0].prefix, "\xe2\x86\xb3 ", 4) == 0,
          "subtask row: the arrow leads the glyph stack");
    check(count_of(row, task_row_line_subtask) == 0, "subtask row: no nested subtasks");
    task_row_free(row);
}

/*
 * test_collect — the pane collection: completed filter, search, status.
 *
 * Inputs:
 *   app — the context (holding what the earlier tests created)
 */
static void
test_collect(TaskApp *app)
{
    gint64 list = task_db_list_create(app->db, "Errands", NULL);
    gint64 a    = task_db_task_create(app->db, list, 0, "Buy milk");
    gint64 b    = task_db_task_create(app->db, list, 0, "Post letter");
    gint64 c    = task_db_task_create(app->db, list, 0, "Call plumber");

    task_db_task_create(app->db, list, b, "find a stamp");
    task_db_task_set_status(app->db, c, TASK_STATUS_DONE);

    task_sel_t           sel = { task_sel_list, list };
    task_pane_content_t *content = task_pane_collect(app, sel, NULL);
    check(content->rows->count == 3, "collect: a list shows its top-level tasks");
    check(strcmp(content->status_text, "Errands - 3 tasks") == 0, "collect: status line");
    check(line_of(content->rows->rows[0], task_row_line_list) == NULL,
          "collect: a list view carries no \"in <list>\" lines");
    task_pane_content_free(content);

    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    content = task_pane_collect(app, sel, NULL);
    check(content->rows->count == 2, "collect: completed tasks hidden by the setting");
    check(strcmp(content->status_text, "Errands - 2 tasks") == 0,
          "collect: the count is what is shown");
    task_pane_content_free(content);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, TRUE);

    TaskSearch *q = task_search_parse("stamp");
    content = task_pane_collect(app, sel, q);
    check(content->rows->count == 1 && content->rows->rows[0]->id == b,
          "collect: a subtask match surfaces its parent");
    check(strcmp(content->status_text, "Errands - 1 matching task") == 0,
          "collect: the status says matching, singular");
    task_pane_content_free(content);
    task_search_free(q);

    /* The default selection is All Tasks: virtual rows across lists. */
    task_sel_t all = task_sel_default();
    check(all.kind == task_sel_view && task_sel_view_of(all) != NULL &&
          strcmp(task_sel_view_of(all)->id, "all") == 0,
          "collect: the default selection is All Tasks");
    check(task_sel_is_virtual(all) && !task_sel_is_virtual(sel),
          "collect: a view is virtual, a list is not");
    check(task_sel_list_id(sel) == list && task_sel_list_id(all) == 0,
          "collect: only a list selection has a list id");
    content = task_pane_collect(app, all, NULL);
    check(content->rows->count > 3 &&
          strncmp(content->status_text, "All Tasks - ", 12) == 0,
          "collect: All Tasks gathers every list");
    const task_row_line_t *in = line_of(content->rows->rows[0], task_row_line_list);
    check(in != NULL && strncmp(in->text, "in ", 3) == 0,
          "collect: virtual rows say which list they are in");
    task_pane_content_free(content);

    /* A group aggregates its lists. */
    gint64 group = task_db_group_create(app->db, "Home");
    task_db_list_set_group(app->db, list, group);
    task_sel_t gsel = { task_sel_group, group };
    content = task_pane_collect(app, gsel, NULL);
    check(content->rows->count == 3 && strcmp(content->status_text, "Home - 3 tasks") == 0,
          "collect: a group shows its lists' tasks under its own name");
    check(task_sel_is_virtual(gsel), "collect: a group is virtual");
    task_pane_content_free(content);

    (void)a;
}

/*
 * main — run the row and pane tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app  = fixture_open();
    gint64   list = task_db_list_create(app->db, "Rows", NULL);

    test_title_line(app, list);
    test_notes_line(app, list);
    test_detail_lines(app, list);
    test_collect(app);
    fixture_close(app);
    return check_result();
}
