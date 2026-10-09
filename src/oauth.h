/* ===========================================================================
 * oauth.h — Google OAuth 2.0 for the calendar sync (installed-app flow)
 *
 * The "OAuth for native apps" flow (RFC 8252) with PKCE, carried from the
 * GTK build's Google Tasks plugin:
 *
 *   1. task_oauth_begin() opens a loopback listener on 127.0.0.1:<ephemeral>
 *      and returns Google's sign-in URL, which the caller opens in the
 *      browser (the core has no way to; the interface does).
 *   2. Google redirects the browser to the loopback URL with a one-time
 *      code; the listener catches it and answers with a "you can close
 *      this tab" page.
 *   3. The code is exchanged on a background thread (an HTTPS round trip)
 *      for an access token plus a REFRESH token.
 *
 * Sign in once, stay signed in: the refresh token is kept in the login
 * KEYCHAIN — never in tasks.ini — under one item per data directory, so
 * the development sandbox (make run) needs a sign-in of its own and can
 * never push test tasks into the calendar the installed app uses.  Access
 * tokens stay in memory.
 *
 * The OAuth client (it identifies the app; it grants nothing by itself)
 * resolves in order: the Google Cloud console's downloaded client file
 * (TASK_OAUTH_CLIENT_FILE) in the data directory, then the client baked
 * in at build time from client_credentials.mk (see the Makefile).
 *
 * Scopes: calendar.events (read and write events) and calendar.readonly
 * (the calendar list, for choosing which calendars to show).
 *
 * MAIN THREAD, except task_oauth_access_token (any thread).
 * =========================================================================== */

#ifndef TASK_OAUTH_H
#define TASK_OAUTH_H

#include <glib.h>

/* The client file's name: what the Google Cloud console calls the file it
 * downloads for a "Desktop app" client, without its per-client prefix.    */
#define TASK_OAUTH_CLIENT_FILE "client_secret.apps.googleusercontent.com.json"

/*
 * task_oauth_done_fn — completion of task_oauth_begin; runs on the main
 * thread.
 *
 * Inputs:
 *   ok        — whether the sign-in succeeded and a grant is stored
 *   error     — a human-readable reason on failure (borrowed), NULL on ok
 *   user_data — as passed to task_oauth_begin
 */
typedef void (*task_oauth_done_fn)(gboolean ok, const gchar *error,
                                   gpointer user_data);

/*
 * task_oauth_init — snapshot the OAuth client and the stored refresh
 * token (from the keychain) into mutex-guarded statics, so the sync
 * thread never touches the config or the keychain.  Call at startup, and
 * again after the data directory changes.
 */
void task_oauth_init(void);

/*
 * task_oauth_have_client — whether an OAuth client is configured.
 *
 * Output:
 *   TRUE when a client id is known.
 */
gboolean task_oauth_have_client(void);

/*
 * task_oauth_authenticated — whether the sync can get a token without a
 * browser trip: a live access token or a stored refresh token.
 *
 * Output:
 *   TRUE when signed in.
 */
gboolean task_oauth_authenticated(void);

/*
 * task_oauth_begin — start the interactive sign-in.  One flow at a time;
 * when this returns a URL, `done` fires exactly once, later, on the main
 * thread (after five minutes without a redirect it fires with a timeout).
 *
 * Inputs:
 *   done      — the completion callback, not NULL
 *   user_data — passed to done
 *   error     — out: why no flow was started (g_free), when NULL is
 *               returned
 *
 * Output:
 *   the sign-in URL to open in the browser (g_free), or NULL with *error
 *   set — in which case done is NOT called.
 */
gchar *task_oauth_begin(task_oauth_done_fn done, gpointer user_data,
                        gchar **error);

/*
 * task_oauth_signout — forget the access token and delete the stored
 * refresh token from the keychain.
 */
void task_oauth_signout(void);

/*
 * task_oauth_access_token — a currently valid access token, refreshing it
 * through the stored refresh token when needed.  BLOCKING while a refresh
 * runs (one HTTPS round trip): call it from the sync thread only.
 *
 * Inputs:
 *   err — out: why there is no token (g_free), when NULL is returned —
 *         never signed in, or Google revoked the grant
 *
 * Output:
 *   the token (g_free), or NULL with *err set.
 */
gchar *task_oauth_access_token(gchar **err);

#endif /* TASK_OAUTH_H */
