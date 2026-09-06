/* ===========================================================================
 * notes_api.h — Notes CLI access for Tasks
 *
 * Tasks MIRRORS the companion Notes app's action items ('!' lines) as
 * ordinary tasks (see notes_to_tasks.c for the mirror itself); this module is the
 * thin CLI wrapper underneath it.  ALL access goes through the notes
 * CLI ("action list --uid" / "action show" / "action done|undone" /
 * "action due" / "action text"), never
 * the Notes database file: Notes' GUI/CLI coexistence is a
 * single-writer design — CLI invocations route through a running GUI's
 * unix socket — so the CLI is the one safe automation surface.
 *
 * Output format parsed ("action list --uid", one row per item):
 *
 *     UID <TAB> NOTEID:ORD <TAB> [x]|[ ] <TAB> YYYY-MM-DD|- <TAB> text
 *
 * UID is the item's STABLE identity: a bare positive integer, assigned
 * once by Notes, never reused, and unchanged when the item is reworded or
 * when other lines are inserted above it.  It is the ONLY identity used
 * here.  The listing's second field is a POSITIONAL address that
 * renumbers whenever a note gains or loses a '!' line; its presence is
 * checked as a format guard and its value is deliberately discarded.
 *
 * The binary is resolved from the "notes_cli" ini key (set in
 * File → Settings…), falling back to `notes` on PATH.
 * =========================================================================== */

#ifndef TASK_NOTES_API_H
#define TASK_NOTES_API_H

#include <glib.h>

/* One Notes action item.  Strings are owned.                              */
typedef struct {
    gint64    uid;                   /* stable identity (> 0)               */
    gchar    *text;                  /* the item text                       */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gboolean  done;
} TaskNoteAction;

/* ---------------------------------------------------------------------------
 * task_notes_actions() — run `notes action list --uid` and parse the
 * rows (list order preserved: newest note first, like Notes prints
 * it).  Returns TaskNoteAction* elements (free with
 * task_notes_actions_free), or NULL with *err set (g_free) — CLI
 * missing, spawn failure, non-zero exit.  A Notes too old to know
 * --uid also fails here; call task_notes_supports_uid() on the failure
 * path to tell that case apart and report it usefully.
 *
 * BLOCKING for the CLI round trip, so callers keep it to the sync
 * worker thread or a user-triggered refresh.
 * ------------------------------------------------------------------------- */
GPtrArray *task_notes_actions(gchar **err);

void task_notes_actions_free(GPtrArray *a);

/* Free ONE item.  task_notes_actions_free() frees an array of these,
 * so both paths release a row exactly one way.  NULL-safe.          */
void task_notes_action_free(TaskNoteAction *na);

/* ---------------------------------------------------------------------------
 * task_notes_supports_uid() — does the installed Notes understand
 * stable uids?  Runs `action list --uid` and reports whether it was
 * accepted (an older build answers an unknown flag with usage and exit
 * 1).  Diagnostic only: the happy path never calls this, so a normal
 * sync pass costs ONE CLI round trip.
 * ------------------------------------------------------------------------- */
gboolean task_notes_supports_uid(void);

/* ---------------------------------------------------------------------------
 * task_notes_action_set_done() — run `notes action done|undone UID`.
 * The write lands in the Notes note itself (striking/un-striking
 * the '!' line), routed through its GUI when one is running.  TRUE on
 * success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean task_notes_action_set_done(gint64 uid, gboolean done, gchar **err);

/* ---------------------------------------------------------------------------
 * task_notes_action_set_due() — run `notes action due UID DATE|-`
 * (due == 0 clears).  Rewrites the item's trailing "due <date>" suffix
 * in the note text.  TRUE on success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean task_notes_action_set_due(gint64 uid, gint64 due, gchar **err);

/* ---------------------------------------------------------------------------
 * task_notes_action_set_text() — run `notes action text UID TEXT`.
 * RENAMES the item: Notes replaces the text of its '!' line, keeping the
 * line's prefix and spacing, its done state and any trailing
 * "due <date>" — and keeping the UID, which is what lets a mirror push a
 * rename for an item it has pinned.  TRUE on success; FALSE with *err
 * set (g_free).
 *
 * Notes REFUSES a blank text or one containing a newline (either would
 * stop the line being that item and retire its uid), so those come back
 * as an ordinary failure and must not be retried as-is.  A trailing
 * "due <date>" inside `text` is parsed by Notes AS a due date, exactly
 * as typing it into the note would be — so never pass an item's text
 * straight back through here after reading it from a source that may
 * have appended one.
 *
 * Needs a Notes that knows the verb; an older one answers with usage
 * and exit 1, which arrives here as a failed push (retried next pass,
 * never silent).
 * ------------------------------------------------------------------------- */
gboolean task_notes_action_set_text(gint64 uid, const gchar *text,
                                    gchar **err);

/* ---------------------------------------------------------------------------
 * task_notes_action_show() — run `notes action show UID` and parse the
 * ONE row it prints (the same format `action list --uid` emits).
 *
 * The point is confirming a push without re-listing: one round trip
 * reads back what Notes now holds for a single pinned item, instead of
 * a full listing that has to be diffed.  Returns a TaskNoteAction*
 * (free with task_notes_action_free), or NULL with *err set (g_free) —
 * CLI missing, spawn failure, or an item that no longer exists, which
 * Notes reports as a non-zero exit.
 *
 * NULL therefore means "could not read it back", NOT "it is gone":
 * telling those apart needs the listing, which is authoritative for
 * existence.  Do not reap on this.
 * ------------------------------------------------------------------------- */
TaskNoteAction *task_notes_action_show(gint64 uid, gchar **err);

#endif /* TASK_NOTES_API_H */
