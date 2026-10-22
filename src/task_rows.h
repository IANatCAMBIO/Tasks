/*
 * task_rows — what a task row SAYS, as a toolkit-neutral description.
 *
 * A "task row" in this app is not a line of text.  It is a tall cell
 * carrying the title, a glyph stack, the task's tags, a notes preview,
 * an attachment count and up to four subtasks.  Both panes that show tasks
 * — the task list and the Kanban cards — show exactly this content.
 *
 * This module decides the CONTENT and the EMPHASIS of each line (which
 * text, bold, struck through, dimmed and by how much) and nothing about
 * fonts, sizes or colours.  One renderer, row_text.h, turns a description
 * into an attributed string for both panes; neither pane formats row text
 * itself, which is what keeps the list and the board from drifting apart.
 *
 * The traps recorded here and nowhere else: the UTF-8 boundary the
 * 120-byte notes preview must land on, the validation every database
 * string needs before it reaches a text system, and the blank-line gate
 * that keeps a one-space note from silently making a row taller.
 *
 * PERFORMANCE.  The expensive lookups — attachment counts, subtasks, every
 * task's tags — are gathered ONCE per refresh into a TaskRowCtx and shared by
 * every row, because the alternative is a query per row.
 *
 * The description types are plain C (malloc/free, no GLib).  TaskRowCtx
 * and the task arrays coming from db.h still use GLib this phase.
 */

#ifndef TASK_ROWS_H
#define TASK_ROWS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "task_app.h"
#include "task_dates.h"

/* What a line of a row is; the renderer may use it, the flags carry the
 * emphasis. */
typedef enum {
    task_row_line_title = 0,        /* glyph stack + title; always first  */
    task_row_line_tags,             /* the task's tags, labelled          */
    task_row_line_notes,            /* first non-blank notes line, capped */
    task_row_line_attachments,      /* "📎 N attachments"                 */
    task_row_line_subtask,          /* "☐ title" / "☑ title", at most 4   */
    task_row_line_more              /* "… +N more subtasks"               */
} task_row_line_kind_t;

/*
 * task_row_line_t — one line of a row.
 *
 *   prefix    — glyphs drawn before the text ("🚨  ", "☑ "): never
 *               bold, struck or dimmed.  "" when there are none.
 *   text      — the line's text, valid UTF-8, never NULL.
 *   bold      — title only, per the bold_task_titles setting.
 *   italic    — the tags line.
 *   strike    — a Done task's title, a Done subtask's title.
 *   small     — every line but the title.
 *   alpha_pct — opacity of `text`, 100 = full.  Dimming is ALWAYS opacity,
 *               never a fixed grey, so it survives a selected row's colour.
 */
typedef struct {
    task_row_line_kind_t kind;
    char *prefix;
    char *text;
    bool  bold;
    bool  italic;
    bool  strike;
    bool  small;
    int   alpha_pct;
} task_row_line_t;

/* The alpha values in use, so the renderer and the tests name them once. */
#define TASK_ROW_ALPHA_FULL      100
#define TASK_ROW_ALPHA_TAGS       60    /* the tags line                  */

/* Between two tags' labels on the tags line. */
#define TASK_ROW_TAG_SEPARATOR "   "
#define TASK_ROW_ALPHA_DETAIL     65    /* notes, attachments, "+N more"  */
#define TASK_ROW_ALPHA_SUB_DONE   55    /* a completed subtask's title    */

/* At most this many subtask lines; the rest become one "+N more" line. */
#define TASK_ROW_MAX_SUBTASKS 4

/* The notes preview is cut at this many BYTES, backed up to a character
 * boundary. */
#define TASK_ROW_NOTES_BYTES 120

/*
 * task_row_t — one task as a pane shows it.  The columns sort on the raw
 * fields and show the formatted ones.
 *
 *   id             — task id (never 0)
 *   status         — the tri-state; "done" is status == TASK_STATUS_DONE
 *   tags_text      — the tag names, ", " apart, "" for none: the Tags
 *                    column's text and sort key
 *   title          — the title as shown ("Untitled Task" when empty):
 *                    the Task column's sort
 *                    key and what status messages quote
 *   lines          — the tall cell, `line_count` lines, title first
 *   due, due_time  — as stored (local midnight; minutes past it)
 *   due_instant    — task_due_instant(due, due_time): the Due sort key
 *   due_text       — task_due_format_at, "" for none
 *   due_urgency    — the Due cell's tint, as of when the row was built
 *   completed_at   — the Completed column's sort key (0 = never)
 *   completed_text — formatted, "" for none
 *   created_at     — the Created column's sort key (0 = unknown)
 *   created_text   — formatted, "" for unknown
 *   status_text    — "New" / "In Progress" / "Done" (static, not owned)
 */
typedef struct {
    int64_t             id;
    TaskStatus          status;
    char               *title;
    char               *tags_text;
    task_row_line_t    *lines;
    size_t              line_count;
    int64_t             due;
    int                 due_time;
    int64_t             due_instant;
    char               *due_text;
    task_due_urgency_t  due_urgency;
    int64_t             completed_at;
    char               *completed_text;
    int64_t             created_at;
    char               *created_text;
    const char         *status_text;
} task_row_t;

/* The rows of one refresh, in the order the tasks were given. */
typedef struct {
    task_row_t **rows;
    size_t       count;
} task_row_list_t;

/*
 * TaskRowCtx — the shared lookups behind one refresh.
 *
 * Fields are private to task_rows.c and to search filtering (which reads
 * subs_by_parent through task_row_ctx_subtasks); the struct is exposed only
 * so a caller can put one on the stack.  Build with task_row_ctx_init, use
 * for as many rows as needed, release with task_row_ctx_clear.
 */
typedef struct {
    GHashTable *att_counts;         /* task id -> attachment count        */
    GPtrArray  *all_subs;           /* owns the subtask rows below        */
    GHashTable *subs_by_parent;     /* parent id -> GPtrArray, borrowed   */
    GHashTable *tags_by_task;       /* task id -> GPtrArray of TaskTag*   */
    gboolean    bold;               /* the bold_task_titles setting       */
    gboolean    show_done;          /* the show_completed setting         */
} TaskRowCtx;

/*
 * task_row_ctx_init — gather the lookups for one refresh.
 *
 * Inputs:
 *   app — the application context (database must be open)
 *   ctx — the context to fill (typically on the caller's stack)
 */
void task_row_ctx_init(TaskApp *app, TaskRowCtx *ctx);

/*
 * task_row_ctx_clear — release what task_row_ctx_init gathered.
 *
 * Inputs:
 *   ctx — an initialised context
 */
void task_row_ctx_clear(TaskRowCtx *ctx);

/*
 * task_row_ctx_subtasks — a top-level task's visible subtasks, as the
 * context grouped them.  What the search filter matches subtask titles
 * against, so it need not query per task.
 *
 * Inputs:
 *   ctx — an initialised context
 *   t   — the task
 *
 * Output:
 *   borrowed GPtrArray of borrowed Task*, or NULL when the task has none
 *   or is itself a subtask.
 */
GPtrArray *task_row_ctx_subtasks(const TaskRowCtx *ctx, const Task *t);

/*
 * task_row_describe — build the description of ONE task.  Used for every
 * row of a refresh and to rebuild a single row in place (after its
 * checkbox is toggled, say).  Does not apply the show_completed filter.
 *
 * Inputs:
 *   t   — the task, not NULL
 *   ctx — an initialised context
 *
 * Output:
 *   new row (task_row_free); never NULL (out of memory aborts, as it does
 *   throughout the core).
 */
task_row_t *task_row_describe(const Task *t, const TaskRowCtx *ctx);

/*
 * task_row_free — free a row and everything it owns.
 *
 * Inputs:
 *   row — the row, or NULL (ignored)
 */
void task_row_free(task_row_t *row);

/*
 * task_rows_build — describe every task of a refresh, honouring the
 * show_completed setting (Done tasks are left out when it is off).  The
 * count of the result is therefore what the status bar reports as shown.
 *
 * Inputs:
 *   tasks — GPtrArray of Task* (borrowed), in display order
 *   ctx   — an initialised context
 *
 * Output:
 *   new list (task_row_list_free); never NULL, possibly empty.
 */
task_row_list_t *task_rows_build(GPtrArray *tasks, const TaskRowCtx *ctx);

/*
 * task_row_list_free — free a list and its rows.
 *
 * Inputs:
 *   list — the list, or NULL (ignored)
 */
void task_row_list_free(task_row_list_t *list);

#endif /* TASK_ROWS_H */
