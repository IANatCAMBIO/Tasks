/* ===========================================================================
 * bnotes.h — Notes CLI access for Tasks
 *
 * Tasks MIRRORS the companion Notes app's action items ('!' lines) as
 * ordinary tasks (see bnsync.h for the sync itself); this module is the
 * thin CLI wrapper underneath it.  ALL access goes through the notes
 * CLI ("action list --uid" / "action done|undone" / "action due"), never
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

#ifndef TASK_BNOTES_H
#define TASK_BNOTES_H

#include <glib.h>

/* One Notes action item.  Strings are owned.                              */
typedef struct {
    gint64    uid;                   /* stable identity (> 0)               */
    gchar    *text;                  /* the item text                       */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gboolean  done;
} TaskNoteAction;

/* ---------------------------------------------------------------------------
 * task_bnotes_actions() — run `notes action list --uid` and parse the
 * rows (list order preserved: newest note first, like Notes prints
 * it).  Returns TaskNoteAction* elements (free with
 * task_bnotes_actions_free), or NULL with *err set (g_free) — CLI
 * missing, spawn failure, non-zero exit.  A Notes too old to know
 * --uid also fails here; call task_bnotes_supports_uid() on the failure
 * path to tell that case apart and report it usefully.
 *
 * BLOCKING for the CLI round trip, so callers keep it to the sync
 * worker thread or a user-triggered refresh.
 * ------------------------------------------------------------------------- */
GPtrArray *task_bnotes_actions(gchar **err);

void task_bnotes_actions_free(GPtrArray *a);

/* ---------------------------------------------------------------------------
 * task_bnotes_supports_uid() — does the installed Notes understand
 * stable uids?  Runs `action list --uid` and reports whether it was
 * accepted (an older build answers an unknown flag with usage and exit
 * 1).  Diagnostic only: the happy path never calls this, so a normal
 * sync pass costs ONE CLI round trip.
 * ------------------------------------------------------------------------- */
gboolean task_bnotes_supports_uid(void);

/* ---------------------------------------------------------------------------
 * task_bnotes_action_set_done() — run `notes action done|undone UID`.
 * The write lands in the Notes note itself (striking/un-striking
 * the '!' line), routed through its GUI when one is running.  TRUE on
 * success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean task_bnotes_action_set_done(gint64 uid, gboolean done, gchar **err);

/* ---------------------------------------------------------------------------
 * task_bnotes_action_set_due() — run `notes action due UID DATE|-`
 * (due == 0 clears).  Rewrites the item's trailing "due <date>" suffix
 * in the note text.  TRUE on success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean task_bnotes_action_set_due(gint64 uid, gint64 due, gchar **err);

#endif /* TASK_BNOTES_H */
