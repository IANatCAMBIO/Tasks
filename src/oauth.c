/* ===========================================================================
 * oauth.c — Google OAuth 2.0 installed-app flow with PKCE (see oauth.h)
 * =========================================================================== */

#include "oauth.h"
#include "http.h"
#include "json.h"
#include "task_loop.h"
#include "task_paths.h"

#include <Security/Security.h>
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OAUTH_SCOPE     "https://www.googleapis.com/auth/calendar.events " \
                        "https://www.googleapis.com/auth/calendar.readonly"
#define OAUTH_AUTH_URL  "https://accounts.google.com/o/oauth2/v2/auth"
#define OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define OAUTH_TIMEOUT_S 300.0          /* give up waiting for the redirect  */
#define OAUTH_REQUEST_MAX 8192         /* longest redirect request read     */

/* The keychain item holding the refresh token: one service, and one
 * account per data directory (oauth.h says why).                          */
#define KEYCHAIN_SERVICE TASKS_DATA_DIR_NAME " Google Calendar"

/* The client baked in at build time from client_credentials.mk (the
 * Makefile turns its two values into these macros).  Desktop-app client
 * secrets are not confidential by Google's own documentation: they
 * identify the app and grant nothing.                                     */
#ifndef TASK_GOOGLE_CLIENT_ID
#define TASK_GOOGLE_CLIENT_ID ""
#endif
#ifndef TASK_GOOGLE_CLIENT_SECRET
#define TASK_GOOGLE_CLIENT_SECRET ""
#endif

static gboolean token_post(const gchar *form, gchar **access, gchar **refresh,
                           gint64 *expires_in, gchar **error);

/* ── Credentials ──────────────────────────────────────────────────────────
 * Written on the main thread (task_oauth_init, the sign-in, sign-out),
 * read and refreshed by the sync thread, always under cred_lock.          */
static GMutex  cred_lock;
static gchar  *cred_client_id;
static gchar  *cred_client_secret;
static gchar  *cred_refresh_token;     /* the persistent grant              */
static gchar  *session_access;         /* the current access token          */
static gint64  session_expiry;         /* unix time it goes stale           */

/*
 * esc — URI-escape a string for a form body or a query.
 *
 * Inputs:
 *   s — the string, not NULL
 *
 * Output:
 *   the escaped string (g_free).
 */
static gchar *
esc(const gchar *s)
{
    return g_uri_escape_string(s, NULL, FALSE);
}

/*
 * session_cache — keep a fresh access token.  Call under cred_lock.  The
 * 60 s haircut keeps a token from expiring in the middle of a request.
 *
 * Inputs:
 *   access     — the token (copied)
 *   expires_in — its lifetime in seconds
 */
static void
session_cache(const gchar *access, gint64 expires_in)
{
    g_free(session_access);
    session_access = g_strdup(access);
    session_expiry = g_get_real_time() / G_USEC_PER_SEC + expires_in - 60;
}

/* ── Keychain ─────────────────────────────────────────────────────────── */

/*
 * keychain_query — the attributes naming this data directory's item.
 *
 * Output:
 *   a mutable dictionary (CFRelease it), or NULL when the data directory
 *   cannot be determined.
 */
static CFMutableDictionaryRef
keychain_query(void)
{
    char *dir = task_data_dir();
    if (dir == NULL) return NULL;

    CFStringRef account = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
    free(dir);
    if (account == NULL) return NULL;

    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR(KEYCHAIN_SERVICE));
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(account);
    return q;
}

/*
 * keychain_load — read the stored refresh token.
 *
 * Output:
 *   the token (g_free), or NULL when none is stored.
 */
static gchar *
keychain_load(void)
{
    CFMutableDictionaryRef q = keychain_query();
    if (q == NULL) return NULL;

    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef data = NULL;
    OSStatus  rc   = SecItemCopyMatching(q, &data);
    CFRelease(q);
    if (rc != errSecSuccess || data == NULL) {
        if (rc != errSecItemNotFound)
            g_warning("oauth: cannot read the keychain item (OSStatus %d)", (int)rc);
        return NULL;
    }
    gchar *token = g_strndup((const gchar *)CFDataGetBytePtr(data),
                             (gsize)CFDataGetLength(data));
    CFRelease(data);
    return token;
}

/*
 * keychain_store — replace the stored refresh token, or delete it.
 *
 * Inputs:
 *   token — the token, or NULL to delete the item
 */
static void
keychain_store(const gchar *token)
{
    CFMutableDictionaryRef q = keychain_query();
    if (q == NULL) return;

    SecItemDelete(q);
    if (token != NULL) {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)token, (CFIndex)strlen(token));
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrLabel, CFSTR(KEYCHAIN_SERVICE));
        OSStatus rc = SecItemAdd(q, NULL);
        CFRelease(data);
        if (rc != errSecSuccess)
            g_warning("oauth: cannot store the keychain item (OSStatus %d)", (int)rc);
    }
    CFRelease(q);
}

/* ── The client ───────────────────────────────────────────────────────── */

/*
 * load_client_file — read the client id and secret from the console's
 * downloaded client file in the data directory ({"installed": {...}}; a
 * "web" client is accepted too).
 *
 * Inputs:
 *   id     — out: the client id (g_free)
 *   secret — out: the client secret (g_free), possibly NULL
 *
 * Output:
 *   TRUE when a usable file was found.
 */
static gboolean
load_client_file(gchar **id, gchar **secret)
{
    char *dir = task_data_dir();
    if (dir == NULL) return FALSE;

    gchar *path = g_build_filename(dir, TASK_OAUTH_CLIENT_FILE, NULL);
    free(dir);
    gchar   *text   = NULL;
    gboolean loaded = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!loaded) return FALSE;

    task_json_t *root   = task_json_parse(text, -1);
    g_free(text);
    task_json_t *client = task_json_get(root, "installed");
    if (client == NULL) client = task_json_get(root, "web");
    const gchar *cid = task_json_str(client, "client_id");
    gboolean     ok  = cid != NULL;
    if (ok) {
        *id     = g_strdup(cid);
        *secret = g_strdup(task_json_str(client, "client_secret"));
    }
    task_json_free(root);
    return ok;
}

/*
 * task_oauth_init — snapshot the client and the stored grant.
 * Documented in oauth.h.
 */
void
task_oauth_init(void)
{
    gchar *id = NULL, *secret = NULL;
    if (!load_client_file(&id, &secret) && *TASK_GOOGLE_CLIENT_ID != '\0') {
        id     = g_strdup(TASK_GOOGLE_CLIENT_ID);
        secret = g_strdup(TASK_GOOGLE_CLIENT_SECRET);
    }
    gchar *refresh = keychain_load();

    g_mutex_lock(&cred_lock);
    g_free(cred_client_id);
    g_free(cred_client_secret);
    g_free(cred_refresh_token);
    cred_client_id     = id;
    cred_client_secret = secret;
    cred_refresh_token = refresh;
    g_clear_pointer(&session_access, g_free);
    session_expiry = 0;
    g_mutex_unlock(&cred_lock);
}

/*
 * task_oauth_have_client — whether a client id is known.
 * Documented in oauth.h.
 */
gboolean
task_oauth_have_client(void)
{
    g_mutex_lock(&cred_lock);
    gboolean yes = cred_client_id != NULL;
    g_mutex_unlock(&cred_lock);
    return yes;
}

/*
 * task_oauth_authenticated — whether a token can be had without the
 * browser.  Documented in oauth.h.
 */
gboolean
task_oauth_authenticated(void)
{
    g_mutex_lock(&cred_lock);
    gboolean yes = cred_refresh_token != NULL ||
                   (session_access != NULL &&
                    g_get_real_time() / G_USEC_PER_SEC < session_expiry);
    g_mutex_unlock(&cred_lock);
    return yes;
}

/*
 * task_oauth_signout — drop the session token and the stored grant.
 * Documented in oauth.h.
 */
void
task_oauth_signout(void)
{
    keychain_store(NULL);
    g_mutex_lock(&cred_lock);
    g_clear_pointer(&session_access, g_free);
    g_clear_pointer(&cred_refresh_token, g_free);
    session_expiry = 0;
    g_mutex_unlock(&cred_lock);
}

/*
 * task_oauth_access_token — the cached token, or a refresh through the
 * stored grant.  Documented in oauth.h.
 */
gchar *
task_oauth_access_token(gchar **err)
{
    *err = NULL;
    g_mutex_lock(&cred_lock);
    if (session_access != NULL && g_get_real_time() / G_USEC_PER_SEC < session_expiry) {
        gchar *tok = g_strdup(session_access);
        g_mutex_unlock(&cred_lock);
        return tok;
    }
    if (cred_refresh_token == NULL || cred_client_id == NULL) {
        g_mutex_unlock(&cred_lock);
        *err = g_strdup("not signed in to Google (Settings, Google Calendar)");
        return NULL;
    }
    gchar *cid  = esc(cred_client_id);
    gchar *csec = esc(cred_client_secret != NULL ? cred_client_secret : "");
    gchar *rtok = esc(cred_refresh_token);
    g_mutex_unlock(&cred_lock);

    /* The round trip runs outside the lock. */
    gchar *form = g_strdup_printf("grant_type=refresh_token&refresh_token=%s"
                                  "&client_id=%s&client_secret=%s", rtok, cid, csec);
    gchar   *access     = NULL;
    gint64   expires_in = 0;
    gchar   *perr       = NULL;
    gboolean ok         = token_post(form, &access, NULL, &expires_in, &perr);
    g_free(form);
    g_free(rtok);
    g_free(cid);
    g_free(csec);

    if (!ok) {
        *err = g_strdup_printf("%s; sign in again in Settings",
                               perr != NULL ? perr : "token refresh failed");
        g_free(perr);
        return NULL;
    }

    /* Cache only while still signed in: a Sign Out during the round trip
     * must not be undone by the answer arriving after it. */
    g_mutex_lock(&cred_lock);
    if (cred_refresh_token != NULL) session_cache(access, expires_in);
    g_mutex_unlock(&cred_lock);
    return access;
}

/* ── PKCE ─────────────────────────────────────────────────────────────── */

/*
 * base64url — RFC 4648 §5 unpadded base64url of a buffer.
 *
 * Inputs:
 *   data — the bytes
 *   len  — how many
 *
 * Output:
 *   the encoding (g_free).
 */
static gchar *
base64url(const guchar *data, gsize len)
{
    gchar *b64 = g_base64_encode(data, len);
    for (gchar *p = b64; *p != '\0'; p++) {
        if (*p == '+')      *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }
    }
    return b64;
}

/*
 * random_urlsafe — base64url of n random bytes: the PKCE verifier and
 * the CSRF state.
 *
 * Inputs:
 *   n — byte count, at most 64
 *
 * Output:
 *   the string (g_free).
 */
static gchar *
random_urlsafe(gsize n)
{
    guchar buf[64];
    g_assert(n <= sizeof buf);
    arc4random_buf(buf, n);
    return base64url(buf, n);
}

/*
 * sha256_base64url — the PKCE S256 challenge of a verifier.
 *
 * Inputs:
 *   verifier — the verifier
 *
 * Output:
 *   the challenge (g_free).
 */
static gchar *
sha256_base64url(const gchar *verifier)
{
    guchar     digest[32];
    gsize      dlen = sizeof digest;
    GChecksum *ck   = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(ck, (const guchar *)verifier, -1);
    g_checksum_get_digest(ck, digest, &dlen);
    g_checksum_free(ck);
    return base64url(digest, dlen);
}

/*
 * token_post — POST a form to the token endpoint and read the token
 * fields from the reply; shared by the code exchange and the refresh.
 * BLOCKING.
 *
 * Inputs:
 *   form       — the urlencoded body
 *   access     — out: the access token (g_free)
 *   refresh    — out: the refresh token (g_free), or NULL when the caller
 *                expects none
 *   expires_in — out: the access token's lifetime in seconds
 *   error      — out: why it failed (g_free)
 *
 * Output:
 *   TRUE with the tokens set, FALSE with *error set.
 */
static gboolean
token_post(const gchar *form, gchar **access, gchar **refresh,
           gint64 *expires_in, gchar **error)
{
    *access = NULL;
    *error  = NULL;
    if (refresh != NULL) *refresh = NULL;

    glong  status = 0;
    gchar *terr   = NULL;
    gchar *body   = task_http_request("POST", OAUTH_TOKEN_URL, NULL,
                                      "application/x-www-form-urlencoded",
                                      NULL, form, &status, &terr);
    if (body == NULL) {
        *error = terr != NULL ? terr : g_strdup("network failure");
        return FALSE;
    }
    g_free(terr);

    task_json_t *root = task_json_parse(body, -1);
    gboolean     ok   = status == 200 && task_json_str(root, "access_token") != NULL;
    if (ok) {
        *access = g_strdup(task_json_str(root, "access_token"));
        if (refresh != NULL) *refresh = g_strdup(task_json_str(root, "refresh_token"));
        task_json_t *exp = task_json_get(root, "expires_in");
        *expires_in = (exp != NULL && exp->type == TASK_JSON_NUMBER) ? (gint64)exp->num : 3600;
    } else {
        const gchar *code = task_json_str(root, "error");
        const gchar *desc = task_json_str(root, "error_description");
        *error = g_strdup_printf("token request failed (HTTP %ld): %s%s%s", status,
                                 code != NULL ? code : "unknown error",
                                 desc != NULL ? " - " : "", desc != NULL ? desc : "");
    }
    task_json_free(root);
    g_free(body);
    return ok;
}

/* ── The interactive flow ─────────────────────────────────────────────── */

/* One browser connection to the loopback listener. */
typedef struct {
    int                fd;
    dispatch_source_t  source;
    GString           *request;
} redirect_conn_t;

/* The single flow in progress; `active` is FALSE when there is none. */
static struct {
    gboolean           active;
    int                fd;               /* the listening socket           */
    dispatch_source_t  accept_source;
    guint16            port;
    gchar             *verifier;         /* PKCE code_verifier             */
    gchar             *state;            /* CSRF state parameter           */
    unsigned int       timeout_id;       /* the give-up timer              */
    task_oauth_done_fn done;
    gpointer           user_data;
    gboolean           exchanging;       /* the code has been sent off     */
    GSList            *conns;            /* redirect_conn_t*, still open   */
} flow;

/*
 * conn_cancelled — a connection's source was cancelled: close its socket
 * and free it (the dispatch cancel handler, so the descriptor is closed
 * only once the source can no longer fire).
 *
 * Inputs:
 *   ctx — the redirect_conn_t
 */
static void
conn_cancelled(void *ctx)
{
    redirect_conn_t *conn = ctx;
    close(conn->fd);
    g_string_free(conn->request, TRUE);
    dispatch_release(conn->source);
    g_free(conn);
}

/*
 * conn_close — stop reading a connection and forget it.
 *
 * Inputs:
 *   conn — the connection
 */
static void
conn_close(redirect_conn_t *conn)
{
    flow.conns = g_slist_remove(flow.conns, conn);
    dispatch_source_cancel(conn->source);
}

/*
 * listener_cancelled — the listening socket's source was cancelled:
 * close the socket.
 *
 * Inputs:
 *   ctx — the descriptor, as a pointer-sized integer
 */
static void
listener_cancelled(void *ctx)
{
    close((int)(intptr_t)ctx);
}

/*
 * flow_finish — tear the flow down and fire its callback once.
 *
 * Inputs:
 *   ok    — the outcome
 *   error — the reason on failure (borrowed), or NULL
 */
static void
flow_finish(gboolean ok, const gchar *error)
{
    if (!flow.active) return;

    task_oauth_done_fn done = flow.done;
    gpointer           user = flow.user_data;

    task_loop_timer_cancel(flow.timeout_id);
    while (flow.conns != NULL) conn_close(flow.conns->data);
    dispatch_source_cancel(flow.accept_source);
    dispatch_release(flow.accept_source);
    g_free(flow.verifier);
    g_free(flow.state);
    memset(&flow, 0, sizeof flow);
    done(ok, error, user);
}

/* A code exchange, run on a background thread. */
typedef struct {
    gchar  *code;
    gchar  *redirect_uri;
    gchar  *verifier;
    gchar  *access;                      /* out                            */
    gchar  *refresh;                     /* out                            */
    gint64  expires_in;                  /* out                            */
    gchar  *error;                       /* out                            */
} exchange_job_t;

/*
 * exchange_job_free — free a job and everything it holds.
 *
 * Inputs:
 *   job — the job
 */
static void
exchange_job_free(exchange_job_t *job)
{
    g_free(job->code);
    g_free(job->redirect_uri);
    g_free(job->verifier);
    g_free(job->access);
    g_free(job->refresh);
    g_free(job->error);
    g_free(job);
}

/*
 * exchange_apply — main thread: keep the tokens and finish the flow.  A
 * flow that already ended (it timed out while the exchange ran) keeps
 * nothing: the user was told sign-in failed.
 *
 * Inputs:
 *   data — the exchange_job_t, freed here
 */
static void
exchange_apply(void *data)
{
    exchange_job_t *job = data;

    if (!flow.active) {
        exchange_job_free(job);
        return;
    }
    if (job->error == NULL && job->access != NULL && job->refresh != NULL) {
        keychain_store(job->refresh);
        g_mutex_lock(&cred_lock);
        g_free(cred_refresh_token);
        cred_refresh_token = g_strdup(job->refresh);
        session_cache(job->access, job->expires_in);
        g_mutex_unlock(&cred_lock);
        flow_finish(TRUE, NULL);
    } else {
        flow_finish(FALSE, job->error != NULL ? job->error
                                              : "Google returned no refresh token");
    }
    exchange_job_free(job);
}

/*
 * exchange_run — background thread: authorization code to tokens.
 *
 * Inputs:
 *   data — the exchange_job_t, handed back to the main thread
 */
static void
exchange_run(void *data)
{
    exchange_job_t *job = data;

    g_mutex_lock(&cred_lock);
    gchar *cid  = esc(cred_client_id != NULL ? cred_client_id : "");
    gchar *csec = esc(cred_client_secret != NULL ? cred_client_secret : "");
    g_mutex_unlock(&cred_lock);

    gchar *code = esc(job->code);
    gchar *uri  = esc(job->redirect_uri);
    gchar *form = g_strdup_printf("grant_type=authorization_code&code=%s&client_id=%s"
                                  "&client_secret=%s&redirect_uri=%s&code_verifier=%s",
                                  code, cid, csec, uri, job->verifier);
    token_post(form, &job->access, &job->refresh, &job->expires_in, &job->error);
    g_free(form);
    g_free(code);
    g_free(uri);
    g_free(cid);
    g_free(csec);
    task_loop_main_async(exchange_apply, job);
}

/*
 * redirect_param — one query parameter of the redirect's request line
 * ("GET /?code=…&state=… HTTP/1.1").
 *
 * Inputs:
 *   request — the request text
 *   name    — the parameter
 *
 * Output:
 *   the decoded value (g_free), or NULL when absent.
 */
static gchar *
redirect_param(const gchar *request, const gchar *name)
{
    const gchar *q = strchr(request, '?');
    if (q == NULL) return NULL;
    q++;
    const gchar *end = strpbrk(q, " \r\n");
    if (end == NULL) end = q + strlen(q);

    gchar  *query = g_strndup(q, (gsize)(end - q));
    gchar  *value = NULL;
    gchar **parts = g_strsplit(query, "&", -1);
    for (gint i = 0; parts[i] != NULL && value == NULL; i++) {
        gchar *eq = strchr(parts[i], '=');
        if (eq == NULL) continue;
        *eq = '\0';
        if (strcmp(parts[i], name) == 0) value = g_uri_unescape_string(eq + 1, NULL);
    }
    g_strfreev(parts);
    g_free(query);
    return value;
}

/*
 * redirect_respond — answer the browser with a small page and close.
 *
 * Inputs:
 *   conn    — the connection, closed here
 *   message — the page's one line
 */
static void
redirect_respond(redirect_conn_t *conn, const gchar *message)
{
    gchar *page = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><head><title>" TASKS_APP_NAME "</title></head>"
        "<body style=\"font-family: -apple-system, sans-serif; margin: 3em\">"
        "<h2>" TASKS_APP_NAME "</h2><p>%s</p>"
        "<p>You can close this tab and return to the app.</p></body></html>", message);
    gsize len = strlen(page), off = 0;
    while (off < len) {
        ssize_t n = write(conn->fd, page + off, len - off);
        if (n <= 0) break;
        off += (gsize)n;
    }
    g_free(page);
    conn_close(conn);
}

/*
 * redirect_handle — a whole request line has arrived: check it, answer
 * it, and send the code to be exchanged.  Requests that are not the
 * redirect (the browser's /favicon.ico) are closed unanswered.
 *
 * Inputs:
 *   conn — the connection
 */
static void
redirect_handle(redirect_conn_t *conn)
{
    const gchar *req   = conn->request->str;
    gchar       *code  = redirect_param(req, "code");
    gchar       *error = redirect_param(req, "error");
    gchar       *state = redirect_param(req, "state");

    if (code == NULL && error == NULL) {
        conn_close(conn);
    } else if (error != NULL) {
        redirect_respond(conn, "Access was denied.");
        gchar *msg = g_strdup_printf("Google reported: %s", error);
        flow_finish(FALSE, msg);
        g_free(msg);
    } else if (state == NULL || strcmp(state, flow.state) != 0) {
        redirect_respond(conn, "State mismatch: try again.");
        flow_finish(FALSE, "OAuth state mismatch");
    } else if (flow.exchanging) {
        /* A repeated redirect (a retry, a reload) must not redeem the code
         * twice: Google answers the reuse by revoking the grant. */
        redirect_respond(conn, "Signed in \xe2\x9c\x93");
    } else {
        flow.exchanging = TRUE;
        exchange_job_t *job = g_new0(exchange_job_t, 1);
        job->code         = g_strdup(code);
        job->redirect_uri = g_strdup_printf("http://127.0.0.1:%u", (guint)flow.port);
        job->verifier     = g_strdup(flow.verifier);
        redirect_respond(conn, "Signed in \xe2\x9c\x93");
        task_loop_background(exchange_run, job);
    }
    g_free(code);
    g_free(error);
    g_free(state);
}

/*
 * conn_readable — bytes arrived on a browser connection: collect them
 * until the request line is complete.
 *
 * Inputs:
 *   ctx — the redirect_conn_t
 */
static void
conn_readable(void *ctx)
{
    redirect_conn_t *conn = ctx;
    char             buf[1024];
    ssize_t          n = read(conn->fd, buf, sizeof buf);

    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0 || conn->request->len + (gsize)n > OAUTH_REQUEST_MAX) {
        conn_close(conn);
        return;
    }
    g_string_append_len(conn->request, buf, n);
    if (strstr(conn->request->str, "\r\n") != NULL) redirect_handle(conn);
}

/*
 * listener_readable — a browser connected: accept it and read it.
 *
 * Inputs:
 *   ctx — unused
 */
static void
listener_readable(void *ctx)
{
    (void)ctx;
    int fd = accept(flow.fd, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

    redirect_conn_t *conn = g_new0(redirect_conn_t, 1);
    conn->fd      = fd;
    conn->request = g_string_new(NULL);
    conn->source  = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
                                           dispatch_get_main_queue());
    dispatch_set_context(conn->source, conn);
    dispatch_source_set_event_handler_f(conn->source, conn_readable);
    dispatch_source_set_cancel_handler_f(conn->source, conn_cancelled);
    flow.conns = g_slist_prepend(flow.conns, conn);
    dispatch_resume(conn->source);
}

/*
 * flow_timeout — nobody came back from the browser.
 *
 * Inputs:
 *   data — unused
 */
static void
flow_timeout(void *data)
{
    (void)data;
    flow.timeout_id = 0;
    flow_finish(FALSE, "timed out waiting for the browser sign-in");
}

/*
 * open_listener — a listening socket on 127.0.0.1 at an ephemeral port.
 *
 * Inputs:
 *   port — out: the port bound
 *
 * Output:
 *   the descriptor, or -1 (errno set).
 */
static int
open_listener(guint16 *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = { 0 };
    addr.sin_len         = sizeof addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len        = sizeof addr;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 8) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    *port = ntohs(addr.sin_port);
    return fd;
}

/*
 * task_oauth_begin — start the interactive sign-in.  access_type=offline
 * asks for a refresh token; prompt=consent makes Google issue one on
 * EVERY sign-in, not only the first ever.  Documented in oauth.h.
 */
gchar *
task_oauth_begin(task_oauth_done_fn done, gpointer user_data, gchar **error)
{
    *error = NULL;
    task_oauth_init();

    g_mutex_lock(&cred_lock);
    gchar *cid = g_strdup(cred_client_id);
    g_mutex_unlock(&cred_lock);

    if (cid == NULL) {
        *error = g_strdup("No Google OAuth client is configured. Put the Google Cloud "
                          "console's Desktop-app client file (" TASK_OAUTH_CLIENT_FILE
                          ") in the data folder, or build with client_credentials.mk.");
        return NULL;
    }
    if (flow.active) {
        *error = g_strdup("A sign-in is already in progress in the browser.");
        g_free(cid);
        return NULL;
    }

    guint16 port = 0;
    int     fd   = open_listener(&port);
    if (fd < 0) {
        *error = g_strdup_printf("Cannot open a loopback port: %s", g_strerror(errno));
        g_free(cid);
        return NULL;
    }

    flow.active        = TRUE;
    flow.fd            = fd;
    flow.port          = port;
    flow.verifier      = random_urlsafe(32);
    flow.state         = random_urlsafe(16);
    flow.done          = done;
    flow.user_data     = user_data;
    flow.timeout_id    = task_loop_timer_once(OAUTH_TIMEOUT_S, flow_timeout, NULL, NULL);
    flow.accept_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
                                                dispatch_get_main_queue());
    dispatch_set_context(flow.accept_source, (void *)(intptr_t)fd);
    dispatch_source_set_event_handler_f(flow.accept_source, listener_readable);
    dispatch_source_set_cancel_handler_f(flow.accept_source, listener_cancelled);
    dispatch_resume(flow.accept_source);

    gchar *challenge = sha256_base64url(flow.verifier);
    gchar *cid_esc   = esc(cid);
    gchar *scope_esc = esc(OAUTH_SCOPE);
    gchar *redirect  = g_strdup_printf("http://127.0.0.1:%u", (guint)port);
    gchar *redir_esc = esc(redirect);
    gchar *url = g_strdup_printf(OAUTH_AUTH_URL "?client_id=%s&redirect_uri=%s"
                                 "&response_type=code&scope=%s&code_challenge=%s"
                                 "&code_challenge_method=S256&state=%s"
                                 "&access_type=offline&prompt=consent",
                                 cid_esc, redir_esc, scope_esc, challenge, flow.state);
    g_free(challenge);
    g_free(cid_esc);
    g_free(scope_esc);
    g_free(redirect);
    g_free(redir_esc);
    g_free(cid);
    return url;
}
