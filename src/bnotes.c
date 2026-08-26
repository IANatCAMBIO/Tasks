/* ===========================================================================
 * bnotes.c — Notes integration via its CLI (see bnotes.h)
 * =========================================================================== */

#include "bnotes.h"
#include "app.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * task_bnotes_cli_path() — resolve the Notes CLI binary: the
 * "notes_cli" setting (path or bare command name), else PATH.
 * Returns a new string (g_free), or NULL when nothing resolves.
 *
 * The program looked for is `notes`, and ONLY `notes` — no probing for
 * other spellings.  A stale build left beside the current one answers
 * `action list` with an empty result and exit 0, i.e. reads as "no action
 * items" rather than as an error, so a wider search can only do harm.
 * ------------------------------------------------------------------------- */
static gchar *
task_bnotes_cli_path(void)
{
    gchar *configured = task_app_config_get("notes_cli");
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
    gchar *cli = task_bnotes_cli_path();
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

/* task_bnotes_actions_free() — free an array of TaskNoteAction*.  NULL-safe. */
void
task_bnotes_actions_free(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++) {
        TaskNoteAction *na = g_ptr_array_index(a, i);
        g_free(na->text);
        g_free(na);
    }
    g_ptr_array_free(a, TRUE);
}

/* ---------------------------------------------------------------------------
 * task_bnotes_actions() — `action list --uid` → parsed rows (see bnotes.h).
 * ------------------------------------------------------------------------- */
GPtrArray *
task_bnotes_actions(gchar **err)
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
        /* UID \t NOTEID:ORD \t [x]|[ ] \t due|- \t text — the text may
         * itself contain tabs, so split into at most five fields.          */
        gchar **f = g_strsplit(lines[i], "\t", 5);
        gchar  *endp = NULL;         /* end of the parsed uid               */
        gint64  uid  = f[0] != NULL
                     ? g_ascii_strtoll(f[0], &endp, 10) : 0;
        /* A row counts only with a well-formed uid and the positional
         * address still present in field 2 — anything else is a format we
         * do not understand, and silently mirroring it would bind a task
         * to the wrong item.  Field 2 is VALIDATED but not kept: nothing
         * uses a positional address, and storing one would invite it.      */
        if (uid > 0 && endp != NULL && *endp == '\0' &&
            f[1] != NULL && f[2] != NULL && f[3] != NULL &&
            f[4] != NULL && strchr(f[1], ':') != NULL) {
            TaskNoteAction *na = g_new0(TaskNoteAction, 1);
            na->uid  = uid;
            na->done = strcmp(f[2], "[x]") == 0;
            na->due  = task_due_parse(f[3]);     /* "-" parses to 0         */
            na->text = g_strdup(f[4]);
            g_ptr_array_add(items, na);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    return items;
}

/* ---------------------------------------------------------------------------
 * task_bnotes_supports_uid() — is --uid understood (see bnotes.h)?
 * ------------------------------------------------------------------------- */
gboolean
task_bnotes_supports_uid(void)
{
    gchar *err = NULL;               /* discarded — the verdict is the
                                      * exit status alone                   */
    gboolean ok = run_cli("action", "list", "--uid", NULL, NULL, &err);
    g_free(err);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_bnotes_action_set_done() — `action done|undone UID` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
task_bnotes_action_set_done(gint64 uid, gboolean done, gchar **err)
{
    *err = NULL;
    gchar *tok = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    gboolean ok = run_cli("action", done ? "done" : "undone", tok, NULL,
                          NULL, err);
    g_free(tok);
    return ok;
}

/* ---------------------------------------------------------------------------
 * task_bnotes_action_set_due() — `action due UID DATE|-` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
task_bnotes_action_set_due(gint64 uid, gint64 due, gchar **err)
{
    *err = NULL;
    gchar *tok  = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    /* ISO date, or "-" to clear.                                           */
    gchar *date = due == 0 ? g_strdup("-") : task_due_format_iso(due);
    gboolean ok = run_cli("action", "due", tok, date, NULL, err);
    g_free(date);
    g_free(tok);
    return ok;
}
