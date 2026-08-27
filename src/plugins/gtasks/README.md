# Google Tasks Sync

Two-way, non-destructive sync between Tasks and your Google Tasks account.

## Non-destructive means non-destructive

**Absence never deletes, on either side.** Only an explicit delete
propagates: deleting a task here deletes it on Google, and a task deleted
on Google is removed here. A task that merely fails to appear in a
listing is treated as *unchanged*, never as *gone*.

If a task's Google counterpart disappears with no local delete to explain
it, the task keeps its content and is pushed back as a new remote task
rather than being thrown away. The same holds for a whole list.

Where both sides changed, the newer edit wins, and a delete beats a
concurrent edit.

## What does not travel

| Stays local | Why |
|---|---|
| Favorite (pin) | Google Tasks has no starring. |
| High priority | No equivalent field. |
| List emoji | Ditto. |
| Attachments | Ditto. |
| *New* vs *In Progress* | Google's own status is binary — both push as "needs action", so the difference between them never leaves this machine. |
| Time of day on a due date | Google's `due` is date-only; the time is documented as discarded. |

## Signing in

**File → Settings… → Google Tasks → Sign In to Google…** opens your
browser for consent. Tasks uses the standard installed-app flow (PKCE,
with a loopback listener on a temporary port); the refresh token is
stored in `tasks.ini` and access tokens are held in memory only. **Sign
Out** removes the stored token.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `gtasks_plugin_enabled` | `1` | Load the plugin at all. Off means it is never opened — and the app then links no network library. |
| `gtasks_sync_enabled` | `1` | Run the sync. |
| `gtasks_interval_min` | `5` | Minutes between automatic syncs while signed in. `0` disables the timer; the Sync button always works. |
| `gtasks_toolbar_button` | `1` | Show the Sync button in the toolbar. |
| `gtasks_refresh_token` | *(unset)* | Written by sign-in. Treat `tasks.ini` as a secret once this is set. |

## Requirements

libcurl — asked for by this plugin alone (`deps.mk`), not by the
application. That is the point of it being a plugin: an installation
without it links no network library at all, which you can check with
`ldd tasks` (or `otool -L tasks` on macOS).

It also needs an OAuth client for the Google Tasks API. Builds from this
repository ship with one baked in, so signing in just works. If you are
building it yourself and want your own client, set one up as below.

### Setting up `client_credentials.mk`

Optional. Without it the build still succeeds and everything except the
Sync sign-in works; add the file and rebuild whenever you are ready — the
Makefile tracks it, so a plain `make` picks up changes.

1. In the [Google Cloud console](https://console.cloud.google.com/),
   create a project (any name) and enable the **Google Tasks API**
   (*APIs & Services → Library*).
2. Configure the OAuth consent screen (*APIs & Services → OAuth consent
   screen*) — the app name you enter there is what the browser's consent
   page will show.
3. Create the client (*APIs & Services → Credentials → Create Credentials
   → OAuth client ID*, application type **Desktop app**) and note the
   client id and secret.
4. In the source directory:
   `cp client_credentials.mk.example client_credentials.mk`, fill in the
   two values, then `make clean && make`.

`client_credentials.mk` is gitignored, so your credentials never end up
in a commit — and Desktop-app client secrets are, per Google's own docs,
not confidential, so shipping them inside a binary you distribute is the
standard pattern.

Alternatively the app accepts the console's downloaded
`client_secret….json` placed next to the binary at runtime — also
gitignored — and that file takes precedence over the baked-in client if
both exist.

## Installing

Copy `gtasks.so` — and this file, if you want the README link to work —
into the `plugins` folder next to the Tasks program, then restart Tasks.

## Notes for plugin authors

The largest of the worked examples:

- It brings its **own dependency**. `deps.mk` adds libcurl to this
  module's flags and to nothing else. GTK, GLib and SQLite are
  deliberately *not* there — those are the host's, shared across the
  `dlopen` boundary, and a second copy of any of them in one process is a
  crash or silent corruption.
- It is **five source files in one module** — sync engine, OAuth flow,
  HTTP wrapper, JSON parser — sharing one host table through
  `plugin_ctx.h`.
- It owns `gtasks_list` and `gtasks_task`, created from its `db_open`
  hook. A remote id, an etag and a deep link are *its* state, not
  something a core task row carries.
- It contributes across nearly the whole API surface: a toolbar button, a
  task-menu item, a read-only editor section, a settings section, a
  worker, and hooks on move / clear-completed / list-delete.
- `init()` only registers. No network call happens there — `init()` runs
  before the window is shown.

See `src/plugins/gtasks/` and `src/plugin.h`.
