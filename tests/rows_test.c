/*
 * rows_test.c — the toolkit-neutral row description (src/task_rows.h) and
 * the pane collection built on it (src/task_pane.h): which lines a task
 * gets, their emphasis, the notes-preview rules, the completed filter, the
 * search filter, the status line, the note-context title prefix, the
 * Favorites view, and which tag New Task gives its task.
 */

#include "fixture.h"
#include "task_ops.h"
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
 *   app — the context
 *   id  — the task
 *
 * Output:
 *   new row (task_row_free).
 */
static task_row_t *
describe(TaskApp *app, gint64 id)
{
    TaskRowCtx ctx;
    Task      *t = task_db_task_get(app->db, id);

    task_row_ctx_init(app, &ctx);
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
 *   tag  — the tag to create tasks with
 */
static void
test_title_line(TaskApp *app, const char *tag)
{
    gint64      id  = task_db_task_create(app->db, 0, "Plain task", tag);
    task_row_t *row = describe(app, id);

    check(row->id == id && row->status == TASK_STATUS_NEW, "title: id and status carried");
    check(strcmp(row->title, "Plain task") == 0, "title: plain title carried");
    check(row->lines[0].kind == task_row_line_title, "title: the first line is the title");
    check(strcmp(row->lines[0].text, "Plain task") == 0, "title: text");
    check(strcmp(row->lines[0].prefix, "") == 0, "title: no glyphs on a plain task");
    check(!row->lines[0].bold && !row->lines[0].strike && !row->lines[0].small &&
          !row->lines[0].italic && row->lines[0].alpha_pct == TASK_ROW_ALPHA_FULL,
          "title: no emphasis by default");
    check(strcmp(row->status_text, "New") == 0, "title: status label");
    check(strcmp(row->due_text, "") == 0 && row->due_urgency == task_due_none,
          "title: no due date, no tint");
    const task_row_line_t *tags = line_of(row, task_row_line_tags);
    check(row->line_count == 2 && tags != NULL && strcmp(tags->text, "Rows") == 0 &&
          tags->italic && tags->small && tags->alpha_pct == TASK_ROW_ALPHA_TAGS,
          "title: a bare task is its title and its tags line, italic and dimmed");
    task_row_free(row);

    /* Priority + done, with the bold setting on. */
    task_db_task_set_priority(app->db, id, TRUE);
    task_db_task_set_status(app->db, id, TASK_STATUS_DONE);
    task_config_set_bool(TASK_CFG_BOLD_TITLES, TRUE);
    row = describe(app, id);
    check(strcmp(row->lines[0].prefix, "\xf0\x9f\x9a\xa8  ") == 0,
          "title: the siren, with two spaces");
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
    gint64 blank = task_db_task_create(app->db, 0, "", tag);
    row = describe(app, blank);
    check(strcmp(row->title, "Untitled Task") == 0 &&
          strcmp(row->lines[0].text, "Untitled Task") == 0,
          "title: an empty title reads Untitled Task");
    task_row_free(row);

    /* Bytes that are not UTF-8 are replaced, not passed on. */
    gint64 bad = task_db_task_create(app->db, 0, "caf\xe9 run", tag);
    row = describe(app, bad);
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
 *   tag  — the tag to create tasks with
 */
static void
test_notes_line(TaskApp *app, const char *tag)
{
    gint64                 id = task_db_task_create(app->db, 0, "Notes", tag);
    task_row_t            *row;
    const task_row_line_t *line;

    set_notes(app, id, "  \n\xc2\xa0\n");
    row = describe(app, id);
    check(line_of(row, task_row_line_notes) == NULL,
          "notes: blank lines (even a no-break space) earn no preview");
    task_row_free(row);

    set_notes(app, id, "\n\n   first real line  \nsecond line");
    row  = describe(app, id);
    line = line_of(row, task_row_line_notes);
    check(line != NULL && strcmp(line->text, "first real line\xe2\x80\xa6") == 0,
          "notes: first non-blank line, trimmed, ellipsis because more follows");
    check(line != NULL && line->small && line->alpha_pct == TASK_ROW_ALPHA_DETAIL &&
          !line->strike && !line->bold,
          "notes: small and dimmed by opacity");
    task_row_free(row);

    set_notes(app, id, "only line");
    row  = describe(app, id);
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
    row  = describe(app, id);
    line = line_of(row, task_row_line_notes);
    check(line != NULL && g_utf8_validate(line->text, -1, NULL),
          "notes: the cap never splits a character");
    check(line != NULL && strlen(line->text) == 119 + 3,
          "notes: the straddling character is dropped, ellipsis added");
    task_row_free(row);
}

/*
 * test_detail_lines — attachments, subtasks, "+N more" and the tags line.
 *
 * Inputs:
 *   app  — the context
 *   tag  — the tag to create tasks with
 */
static void
test_detail_lines(TaskApp *app, const char *tag)
{
    gint64 id = task_db_task_create(app->db, 0, "Parent", tag);

    task_db_attachment_add(app->db, id, "/tmp/a.pdf");
    task_row_t            *row  = describe(app, id);
    const task_row_line_t *line = line_of(row, task_row_line_attachments);
    check(line != NULL && strcmp(line->text, "\xf0\x9f\x93\x8e 1 attachment") == 0,
          "attachments: singular");
    task_row_free(row);

    task_db_attachment_add(app->db, id, "/tmp/b.pdf");
    row  = describe(app, id);
    line = line_of(row, task_row_line_attachments);
    check(line != NULL && strcmp(line->text, "\xf0\x9f\x93\x8e 2 attachments") == 0,
          "attachments: plural");
    task_db_tag_set_emoji(app->db, task_db_task_tag_add(app->db, id, "Errands"), "E");
    task_row_free(row);
    row  = describe(app, id);
    line = line_of(row, task_row_line_tags);
    check(line != NULL && strcmp(line->text, "E  Errands" TASK_ROW_TAG_SEPARATOR "Rows") == 0,
          "tags line: every tag, alphabetically, each with its emoji");
    task_row_free(row);

    gint64 subs[6];
    for (int i = 0; i < 6; i++) {
        char title[32];
        snprintf(title, sizeof title, "Sub %d", i + 1);
        subs[i] = task_db_task_create(app->db, id, title, NULL);
    }
    task_db_task_set_status(app->db, subs[1], TASK_STATUS_DONE);

    row = describe(app, id);
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
    row = describe(app, subs[0]);
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
    gint64 a = task_db_task_create(app->db, 0, "Buy milk", "Chores");
    gint64 b = task_db_task_create(app->db, 0, "Post letter", "Chores");
    gint64 c = task_db_task_create(app->db, 0, "Call plumber", "Chores");

    gint64 stamp = task_db_task_create(app->db, b, "find a stamp", NULL);
    task_db_task_set_status(app->db, c, TASK_STATUS_DONE);

    TaskTag   *chores = task_db_tag_find(app->db, "chores");
    task_sel_t sel    = { task_sel_tag, chores->id };
    task_tag_free(chores);
    task_pane_content_t *content = task_pane_collect(app, sel, NULL);
    check(content->rows->count == 4 && strcmp(content->status_text, "Chores - 4 tasks") == 0 &&
          content->rows->rows[1]->id == b && content->rows->rows[2]->id == stamp,
          "collect: a tag shows every task carrying it, a subtask on its own row after its parent");
    task_pane_content_free(content);

    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    content = task_pane_collect(app, sel, NULL);
    check(content->rows->count == 3 && strcmp(content->status_text, "Chores - 3 tasks") == 0,
          "collect: completed tasks hidden by the setting; the count is what is shown");
    task_pane_content_free(content);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, TRUE);

    TaskSearch *q = task_search_parse("stamp");
    content = task_pane_collect(app, sel, q);
    check(content->rows->count == 2 && content->rows->rows[0]->id == b,
          "collect: a subtask match surfaces its parent, and its own row after it");
    check(strcmp(content->status_text, "Chores - 2 matching tasks") == 0,
          "collect: the status says matching");
    task_pane_content_free(content);
    task_search_free(q);

    /* The default selection is All Tasks: every top-level task. */
    task_sel_t all = task_sel_default();
    check(all.kind == task_sel_view && task_sel_view_of(all) != NULL &&
          strcmp(task_sel_view_of(all)->id, "all") == 0,
          "collect: the default selection is All Tasks");
    check(task_sel_tag_id(sel) != 0 && task_sel_tag_id(all) == 0,
          "collect: only a tag selection has a tag id");
    content = task_pane_collect(app, all, NULL);
    check(content->rows->count > 3 &&
          strncmp(content->status_text, "All Tasks - ", 12) == 0,
          "collect: All Tasks gathers every tag's tasks");
    check(line_of(content->rows->rows[0], task_row_line_tags) != NULL,
          "collect: every row says what it is tagged");
    task_pane_content_free(content);

    /* Favorites: every task carrying a favorite tag, and only those. */
    task_sel_t fav = { task_sel_view, task_view_index_of(task_view_find("pinned")) };
    content = task_pane_collect(app, fav, NULL);
    check(content->rows->count == 0 && strcmp(content->status_text, "Favorites - 0 tasks") == 0,
          "favorites: empty while no tag is a favorite");
    task_pane_content_free(content);
    TaskTag *fav_tag = task_db_tag_find(app->db, "Chores");
    task_db_tag_set_favorite(app->db, fav_tag->id, TRUE);
    TaskTag *again = task_db_tag_get(app->db, fav_tag->id);
    check(again != NULL && again->favorite, "favorites: a tag is made a favorite");
    task_tag_free(again);
    content = task_pane_collect(app, fav, NULL);
    GPtrArray *tagged = task_db_tasks_tagged(app->db, fav_tag->id);
    check(content->rows->count == tagged->len, "favorites: the favorite tag's tasks");
    task_ptr_array_free_tasks(tagged);
    task_pane_content_free(content);
    task_db_tag_set_favorite(app->db, fav_tag->id, FALSE);
    task_tag_free(fav_tag);

    (void)a;
}

/*
 * test_note_context — the note-context setting: off (the default), a
 * Notes action item's row shows its plain title; on, the row's title, its
 * title line (the list and the Kanban card) and the search all use
 * "<folder> - <title>".
 *
 * Inputs:
 *   app — the context
 */
static void
test_note_context(TaskApp *app)
{
    gint64 id   = task_db_task_create(app->db, 0, "lock down my diet RE keto", "Notes items");
    char  *sql  = sqlite3_mprintf(
        "INSERT INTO folders (name) VALUES ('Captain''s Log');"
        "INSERT INTO notes (folder_id, title, created_at, updated_at)"
        " VALUES (last_insert_rowid(), 'Log', 0, 0);"
        "INSERT INTO note_task (task_id, note_id, ord) VALUES (%lld, last_insert_rowid(), 0);"
        "UPDATE action_uid_seq SET next = (SELECT max(uid) + 1 FROM note_task);",
        (long long)id);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    TaskTag    *tag = task_db_tag_find(app->db, "Notes items");
    task_sel_t  sel = { task_sel_tag, tag->id };
    TaskSearch *q   = task_search_parse("captain");
    task_tag_free(tag);

    task_config_set_bool(TASK_CFG_NOTE_CONTEXT, FALSE);
    task_row_t *row = describe(app, id);
    check(strcmp(row->title, "lock down my diet RE keto") == 0 &&
          strcmp(row->lines[0].text, "lock down my diet RE keto") == 0,
          "context: turned off, the row shows the plain title");
    task_row_free(row);
    task_pane_content_t *content = task_pane_collect(app, sel, q);
    check(content->rows->count == 0, "context: off, the folder's name does not match");
    task_pane_content_free(content);

    task_config_set_bool(TASK_CFG_NOTE_CONTEXT, TRUE);
    row = describe(app, id);
    check(strcmp(row->title, "Captain's Log - lock down my diet RE keto") == 0 &&
          strcmp(row->lines[0].text, row->title) == 0,
          "context: on, the row and its title line show it before the title");
    task_row_free(row);
    content = task_pane_collect(app, sel, q);
    check(content->rows->count == 1 && content->rows->rows[0]->id == id,
          "context: on, a search for the folder's name finds the item");
    task_pane_content_free(content);
    task_config_set_bool(TASK_CFG_NOTE_CONTEXT, FALSE);
    task_search_free(q);
}

/*
 * test_new_task_tag — the tag New Task gives its task: the selected tag,
 * else the default tag (by name, whether or not a task carries it yet).
 *
 * Inputs:
 *   app — the context
 */
static void
test_new_task_tag(TaskApp *app)
{
    TaskTag   *rows = task_db_tag_find(app->db, "Rows");
    task_sel_t tsel = { task_sel_tag, rows->id };
    task_sel_t all  = task_sel_default();
    task_tag_free(rows);

    gchar *name = task_ops_default_tag(app);
    check(name == NULL, "default tag: none until one is chosen");
    name = task_sel_new_task_tag(app, tsel);
    check(name != NULL && strcmp(name, "Rows") == 0, "new task: a selected tag is used");
    g_free(name);
    check(task_sel_new_task_tag(app, all) == NULL,
          "new task: a view with no default tag has no tag to give");

    task_config_set(TASK_CFG_DEFAULT_TAG, "  Inbox ");
    name = task_ops_default_tag(app);
    check(name != NULL && strcmp(name, "Inbox") == 0, "default tag: the chosen name, trimmed");
    g_free(name);
    name = task_sel_new_task_tag(app, all);
    check(name != NULL && strcmp(name, "Inbox") == 0, "new task: a view uses the default tag");
    g_free(name);
    name = task_sel_new_task_tag(app, tsel);
    check(name != NULL && strcmp(name, "Rows") == 0, "new task: a selected tag wins over the default");
    g_free(name);
    task_config_set(TASK_CFG_DEFAULT_TAG, NULL);
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
    TaskApp *app = fixture_open();

    test_title_line(app, "Rows");
    test_notes_line(app, "Rows");
    test_detail_lines(app, "Rows");
    test_collect(app);
    test_note_context(app);
    test_new_task_tag(app);
    fixture_close(app);
    return check_result();
}
