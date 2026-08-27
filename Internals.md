# Tasks — Internals

How Tasks is put together: the source layout, the plugin ABI, the
database schema, and the sync engine. For everyday use see the
[User Guide](User_Guide.md); for build instructions see the
[README](README.md).

## Code layout

The application is the list of files below and nothing else. **No file
here mentions Google Tasks or Notes** — both are plugins, and the core
does not know they exist.

| File                       | Purpose                                            |
|----------------------------|----------------------------------------------------|
| `src/main.c`               | GtkApplication entry point: config → database → registries → plugins → window |
| `src/app.[ch]`             | Shared `TaskApp` context: ini config, dialogs, toolbar styles, icon loading, date helpers |
| `src/backup.[ch]`          | Optional rotating database backups: worker thread, VACUUM INTO + verify, bounded rotation |
| `src/db.[ch]`              | SQLite layer: lists, tasks, subtasks, attachments; tombstones and `updated_at` for sync |
| `src/library_window.[ch]`  | Sidebar (virtual views, list groups), tall task rows, toolbar, compact controls + floating button bar, Kanban board, context menus, status bar |
| `src/editor_window.[ch]`   | Per-task editor; debounced write-through saves; Advanced fold for Subtasks/Attachments |
| `src/settings_window.[ch]` | The Settings window, including the Plugins list |
| `src/plugin.h`             | The plugin ABI: the `TaskHostApi` table a plugin sees, and what a `TaskPlugin` is |
| `src/plugin_loader.[ch]`   | Discovery, `dlopen`, ABI checks, enable/disable at run time |
| `src/plugin_owner.[ch]`    | Which plugin registered what, so switching one off can take exactly its registrations back out |
| `src/task_view.[ch]`       | The sidebar view registry (query views and panel views) |
| `src/task_ops.[ch]`        | Core operations (move, clear completed) and their hooks |
| `src/task_worker.[ch]`     | The one background scheduler: timers, db path, re-arm on a database switch |
| `src/task_rows.[ch]`       | Task-row rendering and the row-decoration registry |
| `src/task_ui.[ch]`         | Window chrome registries: toolbar items, menu items, editor sections |
| `src/core_views.c`         | The app's *own* sidebar views (Favorites, All Tasks, Due Today) — registered through the same registry a plugin uses |
| `icons/`                   | Bundled PNG toolbar icons + app logo; `icons/theme/hicolor/` holds SVG arrows for crisp HiDPI tree expanders |

Plugins live under `src/plugins/`, each building to `plugins/<id>.so`.
A plugin is either a single `<id>.c` or a directory `<id>/` of several
files linked into one module:

| Plugin | Purpose |
|---|---|
| `src/plugins/gtasks/` | [Google Tasks Sync](src/plugins/gtasks/README.md): sync engine, OAuth (PKCE + loopback), libcurl wrapper, minimal JSON parser. Owns `gtasks_list` / `gtasks_task`, and brings its own libcurl via `deps.mk` |
| `src/plugins/notes/`  | [Notes Action Items Sync](src/plugins/notes/README.md): the mirror plus its CLI wrapper. Owns `notes_task` / `notes_deleted` |
| `src/plugins/forecast.c` | [Weekly Forecast](src/plugins/forecast.README.md): a panel view of the week |
| `src/plugins/overdue.c`  | [Overdue](src/plugins/overdue.README.md): a query view — the small worked example |

## Plugin ABI

A plugin exports exactly one symbol, `task_plugin_entry`, and receives a
`TaskHostApi` table. It **imports nothing from the host**, so a module
links against no application object and the app is not built with
`-rdynamic`. The full contract is in `src/plugin.h`; the shape is:

- `init()` registers — views, a worker, op hooks, row decorations,
  toolbar and menu items, a settings section. It must be cheap: it runs
  before the window is shown.
- `db_open()` creates the plugin's **own tables**. Nothing belonging to
  an integration is on a core row.
- Config keys are namespaced by the plugin id automatically, so the
  `notes` plugin asking for `sync` reads `notes_sync`.

Two properties are worth stating because everything else follows from
them:

**A disabled plugin is never opened.** The loader reads
`<id>_plugin_enabled` from the *filename* before `dlopen`, so switching
one off means it is never mapped, never initialised, never resolved —
not merely ignored. That is also why the Settings list takes a plugin's
displayed name and description from its README rather than from inside
the module: the README is the only thing readable in both states.

**A plugin is never `dlclose`d.** Switching one off sweeps its
registrations (that is what `plugin_owner` is for) and stops its worker's
timer, and the change is immediate — but the code stays mapped for the
life of the process. A worker pass still in flight, or an idle callback
already queued, therefore remains valid code and simply finds itself
unregistered. Unmapping would turn each of those into a jump into freed
memory, and buys nothing.

Because a disable is a sweep rather than an unload, `init()` may be
called more than once — the app sweeps before re-calling, so registering
again is correct rather than duplicated. Anything genuinely
once-per-process belongs in `task_plugin_entry`.

## Database format

Everything lives in one ordinary SQLite file (see *Storage* in the
[User Guide](User_Guide.md)), so any standard SQLite tool can read it:

```sql
-- ---------------------------------------------------------------- core
CREATE TABLE list_groups (
  id       INTEGER PRIMARY KEY,
  name     TEXT    NOT NULL DEFAULT '',
  position INTEGER NOT NULL DEFAULT 0    -- display order (UI only)
);

CREATE TABLE lists (
  id         INTEGER PRIMARY KEY,
  name       TEXT    NOT NULL DEFAULT '',
  emoji      TEXT    NOT NULL DEFAULT '',   -- local-only, never synced
  position   INTEGER NOT NULL DEFAULT 0,    -- local-only display order
  group_id   INTEGER REFERENCES list_groups(id),  -- optional group
  updated_at INTEGER NOT NULL DEFAULT 0,    -- UNIX seconds
  deleted    INTEGER NOT NULL DEFAULT 0     -- tombstone until pushed
);

CREATE TABLE tasks (
  id           INTEGER PRIMARY KEY,
  list_id      INTEGER NOT NULL REFERENCES lists(id),
  parent_id    INTEGER REFERENCES tasks(id),  -- NULL = top level; one
                                              -- level only (no sub-subtasks)
  title        TEXT    NOT NULL DEFAULT '',
  notes        TEXT    NOT NULL DEFAULT '',
  due          INTEGER NOT NULL DEFAULT 0,    -- UNIX local midnight; 0 = none
  status       INTEGER NOT NULL DEFAULT 0,    -- 0 New, 1 In Progress,
                                              -- 2 Done (TaskStatus)
  pinned       INTEGER NOT NULL DEFAULT 0,    -- local-only, never synced
  priority     INTEGER NOT NULL DEFAULT 0,    -- local-only high-priority flag
  position     INTEGER NOT NULL DEFAULT 0,
  updated_at   INTEGER NOT NULL DEFAULT 0,    -- UNIX seconds
  deleted      INTEGER NOT NULL DEFAULT 0,    -- tombstone until pushed
  completed_at INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE attachments (
  id         INTEGER PRIMARY KEY,
  task_id    INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  path       TEXT    NOT NULL,               -- a reference, not a copy
  added_at   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE sync_state (key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX idx_tasks_list ON tasks(list_id, parent_id, position);

-- ------------------------------------------------- owned by the plugins
-- Created by each plugin's own db_open hook, not by the app.  A database
-- whose plugins are not installed simply does not have these.

CREATE TABLE gtasks_list (                    -- Google Tasks Sync
  list_id   INTEGER PRIMARY KEY REFERENCES lists(id) ON DELETE CASCADE,
  gtasks_id TEXT                              -- bound Google tasklist
);
CREATE TABLE gtasks_task (
  task_id   INTEGER PRIMARY KEY REFERENCES tasks(id) ON DELETE CASCADE,
  gtasks_id TEXT,                             -- bound Google task
  etag      TEXT,                             -- push guard (If-Match)
  web_link  TEXT,                             -- Google mirror fields...
  glinks    TEXT,                             --   links[] as JSON
  assigned  TEXT                              --   assignmentInfo origin
);

CREATE TABLE notes_task (                     -- Notes Action Items Sync
  task_id INTEGER PRIMARY KEY REFERENCES tasks(id) ON DELETE CASCADE,
  uid     INTEGER NOT NULL,                   -- the item's stable identity
  done    INTEGER NOT NULL DEFAULT 0,         -- what Notes last held:
  due     INTEGER NOT NULL DEFAULT 0          --   the bulk-push baseline
);
CREATE INDEX idx_notes_task_uid ON notes_task(uid);
CREATE TABLE notes_deleted (uid INTEGER PRIMARY KEY); -- mirror tasks the
                                                      -- user deleted here
```

**A task row carries nothing belonging to a particular integration.**
A remote id, an etag, a deep link, the baseline a done-only source was
last known to hold — each lives in a SIDE TABLE keyed by row id, owned
and created by whichever plugin the integration is. `ON DELETE CASCADE`
so purging a task cannot leave its remote identity behind to be matched
against later. Schema **v8** moved the Google columns out of `tasks` and
`lists`; **v9** moved the Notes ones.

Those two migrations still name the side tables, and must: a migration
moves data that already exists whether or not the plugin that will read
it is installed. Each creates what it needs itself, with
`IF NOT EXISTS`, so it agrees with the plugin's own `db_open`.

The schema version rides in `PRAGMA user_version` (currently **9**, from
`TASK_DB_SCHEMA_VERSION`).

Before a migration runs, `task_db_open` backs the file up to
`<db>.pre-v<N>.bak` via `VACUUM INTO` — one per from-version, never
overwritten. The v8/v9 pattern is worth copying for any future one:
**copy, verify, and only then drop**. Each copies into the new tables,
checks that every row that had an identity has the *same* identity now
(counting is not enough — a copy that wrote the right number of wrong
rows would pass that), and drops the old columns only if that check
passed. A failed verify leaves every column in place and says so.

Semantics worth knowing when querying directly:

- **Tombstones**: `deleted = 1` rows are pending remote deletes — the
  app hides them and purges them once the delete is pushed (or
  immediately when the row never synced). Filter them out of any
  direct query.
- `due` is midnight *local time* on the due day, as UNIX seconds;
  0 means no due date. Google's side is date-only, so this loses
  nothing in sync.
- `gtasks_task.gtasks_id`, its `etag` and the task's own `updated_at`
  are the Google sync identity: a task with no `gtasks_task` row has
  never been pushed; `updated_at` newer than the remote copy means
  locally dirty. Joining is how you ask — `LEFT JOIN gtasks_task g ON
  g.task_id = t.id` — and a database without that plugin has no such
  table at all.
- `position` (both tables) and `lists.emoji` are local-only.
  `gtasks_task.web_link`, `glinks` and `assigned` are read-only mirrors
  of Google fields, shown in an editor section the plugin contributes.
- Writes to the local-only flags `pinned` and `priority` deliberately
  **do not** touch `updated_at` — bumping it would mark the row
  sync-dirty and cost a no-op PATCH per toggle, and could starve a
  concurrent remote edit behind a 412 skip. Only fields that Google
  actually stores may stamp it. (A full-row `task_db_task_update` still
  stamps, since it writes the synced fields too.)
- `status` is **not** in that category: it is the successor of the
  synced `done` column, so every write to it stamps `updated_at` and
  dirties the row — a `0 ↔ 1` move (New ↔ In Progress) included, even
  though neither Google nor Notes can represent it. The alternative
  would leave such a move invisible to everything that reads
  `updated_at`. The cost is that on the incremental sync path (where an
  unchanged remote task is simply absent from the listing) a dirty row
  gets an etag-guarded PATCH whose body matches what the remote already
  holds; on a full listing nothing is sent, since the content compare
  finds no difference.
- `completed_at` is stamped when a row enters status `2` and cleared
  when it leaves; re-marking an already-Done row keeps its first stamp.
  That rule is written as an SQL `CASE` over the OLD row values.
- `sync_state` is a key/value scratchpad shared by the app and the
  plugins, and its keys are NOT namespaced — a plugin prefixes its own.
  `lists_custom_order` is the app's (set once the user drag-reorders
  lists); `last_sync`, `default_list_gid` and `bn_*` belong to the sync
  plugins.
- `notes_task.done` / `.due` are a BASELINE, not a mirror: they hold
  what Notes was last known to have, so the rows whose done-ness or due
  date differs from them *are* the pending write set. There is no queue
  table to corrupt, and the set survives a crash.
- Pinning and high-priority for mirrored action items live entirely on
  this side, on the task row's own `pinned` / `priority` flags (Notes
  knows neither concept).

Two practical cautions: the app sets a 5-second busy timeout (the GUI
and the sync worker share the file), so brief external readers coexist
fine, but long write transactions from other tools will stall it; and
prefer backing up while the app is closed — a copy taken mid-sync can
catch a transaction in flight.

## Sync engine

All of this is the **Google Tasks plugin** (`src/plugins/gtasks/`), not
the application. See also its
[README](src/plugins/gtasks/README.md).

The design goal is to be **non-destructive by default**: absence on
one side never deletes on the other; only explicit deletes propagate.
The Notes mirror follows the same rule — see
[its README](src/plugins/notes/README.md) — including refusing to reap
anything when a listing comes back empty, since an empty listing is
indistinguishable from a stale Notes answering on the socket.

- Sync runs on a worker thread with its **own SQLite connection** (a
  connection never crosses threads); progress and completion are
  marshalled back to the main loop through `host->notify->invoke_main`.
  `curl_global_init` happens in the plugin's `task_plugin_entry`, before
  any worker of its own can exist — libcurl's implicit init is not
  thread-safe, and the application does not link libcurl at all.
- After the first full pass, task fetches are incremental
  (`updatedMin = last_sync − 300` — the overlap absorbs clock skew)
  with deleted/completed/hidden items included. With a partial
  listing, an absent item means *unchanged*, never deleted — remote
  deletions arrive as explicit `deleted: true` items.
- Local tombstones DELETE remotely, then purge. A local task whose
  `gtasks_id` is missing from a **full** listing (deleted on Google
  with no local tombstone) drops its stale identity and is pushed
  back as a new remote task; a local list whose remote list vanished
  is re-created and all its tasks re-pushed.
- Pushes: creates POST (parents before subtasks), edits PATCH with
  `If-Match: etag` — a 412 skips the push (remote wins; the next pull
  reconciles). Otherwise conflicts resolve newest-wins per item, and
  a deletion beats a concurrent edit. Every push reply stamps the row
  clean (fresh etag, remote update time).
- `hidden` remote tasks (completed and cleared) are never re-created
  locally — that keeps Clear Completed from resurrecting rows.
- Cross-list moves use `tasks.move` with `destinationTasklist` on a
  worker job, children re-parented afterwards; when offline the
  fallback is a tombstone in the source list plus stripped ids so the
  rows push as new. Clear Completed uses `tasks.clear` when signed
  in, tombstone deletes otherwise.
- Google's default tasklist cannot be deleted by any client (their
  API returns 400). Every sync stores its id in `sync_state`, the
  delete action refuses it up front, and a stale tombstone for it is
  restored rather than retried forever.

## OAuth

Also the Google Tasks plugin. Installed-app flow per RFC 8252: PKCE (S256, GLib SHA-256), a
loopback `GSocketService` on an ephemeral port for the redirect, and
`access_type=offline` for a refresh token. The client credentials
resolve in order: a `client_secret….json` next to the binary (or in
the user config dir) → legacy `google_client_id`/`google_client_secret`
ini keys → a baked-in default from `client_credentials.mk`. The
refresh token persists in `tasks.ini` (`gtasks_refresh_token`);
access tokens live in memory only. The redirect listener redeems the
authorization code exactly once — browsers sometimes replay the
redirect GET, and a second exchange would revoke the first grant.
