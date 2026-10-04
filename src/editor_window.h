#import <Cocoa/Cocoa.h>
#include "task_app.h"

/*
 * editor_window — the per-task editor: one window per task, edits every
 * property of the task plus its subtasks, attachments and recurrence.
 *
 * Saves are write-through with a 600 ms debounce on text; closing a window
 * flushes its pending save.  Every save fires the app's task event
 * (task_app_notify_tasks), which is how the library window hears of it.
 *
 * The module owns its windows: a registry keyed by task id, one editor per
 * task, each removed from the registry (and so released) when its window
 * closes.  Nothing outside holds an editor.
 *
 * Behavioural reference: porting/gtk_reference/editor_window.c.
 * Main thread only.
 */

/*
 * tasks_editor_open — open the editor for a task, or bring its existing
 * editor to the front.
 *
 * Inputs:
 *   app     — the application context
 *   task_id — the task; a task that no longer exists opens nothing
 */
void tasks_editor_open(TaskApp *app, int64_t task_id);

/*
 * tasks_editor_open_new — tasks_editor_open for a task the New Task command
 * has JUST created: the same window plus a Cancel button, which closes the
 * editor and deletes the task again.  This is a separate entry point
 * because Cancel must never do that to a task that existed before.
 *
 * Inputs:
 *   app     — the application context
 *   task_id — the newly created task
 */
void tasks_editor_open_new(TaskApp *app, int64_t task_id);

/*
 * tasks_editor_close — close one task's editor if it is open, flushing its
 * pending save first.  The library window calls this BEFORE it deletes a
 * task, so the flush cannot land on a deleted row.
 *
 * Inputs:
 *   task_id — the task; no open editor for it is not an error
 */
void tasks_editor_close(int64_t task_id);

/*
 * tasks_editor_refresh_all — reload every open editor from the database
 * (the library window changed something).  An editor with a pending
 * unsaved edit is skipped; text is rewritten only where it differs, so a
 * cursor never jumps; an editor whose task has gone closes itself.
 */
void tasks_editor_refresh_all(void);

/*
 * tasks_editor_close_all — close every open editor, each flushing its
 * pending save.  Called by the library window as it shuts down, after it
 * has dropped its event listeners.
 */
void tasks_editor_close_all(void);
