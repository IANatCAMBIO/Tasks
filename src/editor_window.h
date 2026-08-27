/* ===========================================================================
 * editor_window.h — the per-task editor window for Tasks
 *
 * One window per task (double-click in the library), tracked in
 * app->editors keyed by task id.  Edits all task properties — title,
 * notes, due date, status, pinned — plus the task's attachments and (for
 * top-level tasks) its subtasks.  Subtasks cannot have subtasks: editing
 * a subtask shows a "part of" note instead of a subtask section.
 *
 * Status is a New / In Progress / Done dropdown, the full tri-state
 * (db.h): the task list's checkbox column can only reach two of those
 * three values, so this is the only place In Progress can be chosen
 * outright.
 *
 * Saves are write-through with a short debounce (like the Notes
 * editor autosave): every change lands in the database within ~600 ms
 * and the library refreshes.  Closing the window flushes a pending save.
 * A task mirroring a Notes action item is no different here — its
 * status and due reach Notes with the next mirror pass (the Notes plugin),
 * never from this window, and Notes sees only whether the status is
 * Done.
 *
 * Subtasks and Attachments are folded away behind an "Advanced" link at
 * the foot of the window: collapsed for a task that has neither, expanded
 * on open for one that already does.  Expanding grows the window by that
 * block's own height and collapsing gives the same pixels back.
 *
 * The foot row also carries a Save button (flush the pending save and
 * close) at the right of every editor; the New Task variant adds Cancel
 * to its right — see task_editor_open_new().
 * =========================================================================== */

#ifndef TASK_EDITOR_WINDOW_H
#define TASK_EDITOR_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * task_editor_open() — open (or raise) the editor for `task_id`.
 * ------------------------------------------------------------------------- */
void task_editor_open(TaskApp *app, gint64 task_id);

/* ---------------------------------------------------------------------------
 * task_editor_open_new() — task_editor_open() for the row the New Task action
 * has just created: the same window plus a Cancel button beside the Save
 * every editor has.  Cancel closes and DELETES the task again (tombstoned
 * with its subtasks, so the delete syncs), which is why this entry point
 * exists at all — Cancel must never do that to a pre-existing task.
 * Opens folded even if the row somehow has subtasks or attachments.
 * ------------------------------------------------------------------------- */
void task_editor_open_new(TaskApp *app, gint64 task_id);

/* ---------------------------------------------------------------------------
 * task_editor_refresh_all() — reload every open editor from the database
 * (called after a sync or library-side change).  Editors with a pending
 * unsaved edit are skipped; text widgets are only rewritten when the
 * stored content actually differs, so a cursor never jumps mid-typing.
 * Editors whose task disappeared are closed.
 * ------------------------------------------------------------------------- */
void task_editor_refresh_all(TaskApp *app);

/* task_editor_close_all() — destroy every open editor (flushing saves).    */
void task_editor_close_all(TaskApp *app);

#endif /* TASK_EDITOR_WINDOW_H */
