/* ===========================================================================
 * notes_api.c — Notes integration via its CLI (see notes_api.h)
 * =========================================================================== */

#include "notes_api.h"
#include "plugin_ctx.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * task_notes_cli_path() — resolve the Notes CLI binary: the
 * "notes_cli" setting (path or bare command name), else PATH.
 * Returns a new string (g_free), or NULL when nothing resolves.
 *
 * The program looked for is `notes`, and ONLY `notes` — no probing for
 * other spellings.  A stale build left beside the current one answers
 * `action list` with an empty result and exit 0, i.e. reads as "no action
 * items" rather than as an error, so a wider search can only do harm.
 * ------------------------------------------------------------------------- */
static gchar *
task_notes_cli_path(void)
{
    gchar *configured = host->config->get(self, "cli");
    if (configured != NULL) {
        if (g_file_test(configured, G_FILE_TEST_IS_EXECUTABLE))
            return configured;
        /* A bare command name in the setting still searches PATH.          */
        gchar *found = g_find_program_in_path(configured);
        g_free(configured);
        return found;
    }
    return g_find_program_in_path("notes");
}

/* ---------------------------------------------------------------------------
 * run_cli() — spawn the Notes CLI with up to four arguments and
 * collect stdout.  TRUE on a zero exit; FALSE with *err set otherwise.
 * `out` may be NULL when only success matters.
 * ------------------------------------------------------------------------- */
static gboolean
run_cli(const gchar *a1, const gchar *a2, const gchar *a3,
        const gchar *a4, gchar **out, gchar **err)
{
    if (out != NULL)
        *out = NULL;
    gchar *cli = task_notes_cli_path();
    if (cli == NULL) {
        *err = g_strdup("Notes CLI not found \xe2\x80\x94 set its "
                        "path in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return FALSE;
    }
    gchar *argv[] = { cli, (gchar *)a1, (gchar *)a2, (gchar *)a3,
                      (gchar *)a4, NULL };
    gchar *sout = NULL;              /* captured stdout                     */
    gchar *serr = NULL;              /* captured stderr                     */
    gint   wait_status = 0;
    GError *gerr = NULL;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL,
                                    G_SPAWN_DEFAULT, NULL, NULL,
                                    &sout, &serr, &wait_status, &gerr);
    g_free(cli);
    if (!spawned) {
        *err = g_strdup_printf("cannot run the Notes CLI: %s",
                               gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(sout);
        g_free(serr);
        return FALSE;
    }
    if (!g_spawn_check_wait_status(wait_status, NULL)) {
        gchar *detail = serr != NULL ? g_strstrip(serr) : NULL;
        *err = g_strdup_printf("Notes reported: %s",
                               detail != NULL && *detail != '\0'
                               ? detail : "command failed");
        g_free(sout);
        g_free(serr);
        return FALSE;
    }
    g_free(serr);
    if (out != NULL)
        *out = sout;
    else
        g_free(sout);
    return TRUE;
}

/* task_notes_action_free() — free ONE item.  NULL-safe.                    */
void
task_notes_action_free(TaskNoteAction *na)
{
    if (na == NULL)
        return;
    g_free(na->text);
    g_free(na);
}

/* task_notes_actions_free() — free an array of TaskNoteAction*.  NULL-safe. */
void
task_notes_actions_free(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        task_notes_action_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* ---------------------------------------------------------------------------
 * parse_action_row() — one listing line → a TaskNoteAction, or NULL when
 * the line is not a format we understand.  THE one parser for the record
 * `action list --uid` and `action show` both print, so the two can never
 * come to disagree about what a field means.
 *
 *     UID <TAB> NOTEID:ORD <TAB> [x]|[ ] <TAB> due|- <TAB> text
 *
 * A row counts only with a well-formed uid and the positional address
 * still present in field 2 — anything else is a format we do not
 * understand, and silently mirroring it would bind a task to the wrong
 * item.  Field 2 is VALIDATED but not kept: nothing uses a positional
 * address, and storing one would invite it.  The text may itself contain
 * tabs, so the split stops at five fields and the text keeps the rest.
 * ------------------------------------------------------------------------- */
static TaskNoteAction *
parse_action_row(const gchar *line)
{
    gchar **f = g_strsplit(line, "\t", 5);
    gchar  *endp = NULL;             /* end of the parsed uid               */
    gint64  uid  = f[0] != NULL ? g_ascii_strtoll(f[0], &endp, 10) : 0;
    TaskNoteAction *na = NULL;       /* the parsed row, NULL if malformed   */
    if (uid > 0 && endp != NULL && *endp == '\0' &&
        f[1] != NULL && f[2] != NULL && f[3] != NULL &&
        f[4] != NULL && strchr(f[1], ':') != NULL) {
        na = g_new0(TaskNoteAction, 1);
        na->uid  = uid;
        na->done = strcmp(f[2], "[x]") == 0;
        na->due  = host->util->due_parse(f[3]);  /* "-" parses to 0          */
        na->text = g_strdup(f[4]);
    }
    g_strfreev(f);
    return na;
}

/* ---------------------------------------------------------------------------
 * task_notes_actions() — `action list --uid` → parsed rows (see notes_api.h).
 * ------------------------------------------------------------------------- */
GPtrArray *
task_notes_actions(gchar **err)
{
    *err = NULL;
    gchar *out = NULL;               /* the CLI's stdout                    */
    if (!run_cli("action", "list", "--uid", NULL, &out, err))
        return NULL;

    GPtrArray *items = g_ptr_array_new();
    gchar **lines = g_strsplit(out != NULL ? out : "", "\n", -1);
    for (gint i = 0; lines[i] != NULL; i++) {
        if (*lines[i] == '\0')
            continue;
        TaskNoteAction *na = parse_action_row(lines[i]);
        if (na != NULL)
            g_ptr_array_add(items, na);
    }
    g_strfreev(lines);
    g_free(out);
    return items;
}

/* ---------------------------------------------------------------------------
 * task_notes_supports_uid() — is --uid understood (see notes_api.h)?
 * ------------------------------------------------------------------------- */
gboolean
task_notes_supports_uid(void)
{
    gchar *err = NULL;               /* discarded — the verdict is the
                                      * exit status alone                   */
    gboolean ok = run_cli("action", "list", "--uid", NULL, NULL, &err);
    g_free(err);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_notes_action_set_done() — `action done|undone UID` (see notes_api.h).
 * ------------------------------------------------------------------------- */
gboolean
task_notes_action_set_done(gint64 uid, gboolean done, gchar **err)
{
    *err = NULL;
    gchar *tok = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    gboolean ok = run_cli("action", done ? "done" : "undone", tok, NULL,
                          NULL, err);
    g_free(tok);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_notes_action_set_due() — `action due UID DATE|-` (see notes_api.h).
 * ------------------------------------------------------------------------- */
gboolean
task_notes_action_set_due(gint64 uid, gint64 due, gchar **err)
{
    *err = NULL;
    gchar *tok  = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    /* ISO date, or "-" to clear.                                           */
    gchar *date = due == 0 ? g_strdup("-")
                           : host->util->due_format_iso(due);
    gboolean ok = run_cli("action", "due", tok, date, NULL, err);
    g_free(date);
    g_free(tok);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_notes_action_set_text() — `action text UID TEXT` (see notes_api.h).
 * ------------------------------------------------------------------------- */
gboolean
task_notes_action_set_text(gint64 uid, const gchar *text, gchar **err)
{
    *err = NULL;
    gchar *tok = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    gboolean ok = run_cli("action", "text", tok, text, NULL, err);
    g_free(tok);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_notes_action_show() — `action show UID` → the one row it prints
 * (see notes_api.h).
 * ------------------------------------------------------------------------- */
TaskNoteAction *
task_notes_action_show(gint64 uid, gchar **err)
{
    *err = NULL;
    gchar *tok = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    gchar *out = NULL;               /* the CLI's stdout                    */
    gboolean ok = run_cli("action", "show", tok, NULL, &out, err);
    g_free(tok);
    if (!ok) {
        g_free(out);
        return NULL;
    }

    /* One record, but read it the same way a listing line is read, and
     * take the FIRST parseable line rather than assuming line 0 — an
     * older Notes that does not know the verb has already failed above,
     * so anything here is either the record or noise.                    */
    TaskNoteAction *na = NULL;       /* the item, NULL if unparseable       */
    gchar **lines = g_strsplit(out != NULL ? out : "", "\n", -1);
    for (gint i = 0; lines[i] != NULL && na == NULL; i++) {
        if (*lines[i] != '\0')
            na = parse_action_row(lines[i]);
    }
    g_strfreev(lines);
    g_free(out);

    if (na == NULL)
        *err = g_strdup_printf("Notes gave no readable record for item %"
                               G_GINT64_FORMAT, uid);
    return na;
}
