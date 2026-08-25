# Lists — Internals

How Lists is put together: the source layout, the database schema,
and the sync engine. For everyday use see the
[User Guide](User_Guide.md); for build instructions see the
[README](README.md).

## Code layout

| File                       | Purpose                                            |
|----------------------------|----------------------------------------------------|
| `src/main.c`               | GtkApplication entry point; config, database and OAuth init, auto-sync timer |
| `src/app.[ch]`             | Shared `BtApp` context: ini config, dialogs, toolbar styles, icon loading, date helpers |
| `src/db.[ch]`              | SQLite layer: lists, tasks, subtasks, attachments; tombstones and `updated_at` for sync |
| `src/library_window.[ch]`  | Sidebar (virtual views, list groups), tall task rows, toolbar, Compact Layout + floating button bar, Weekly Forecast panel, context menus, status bar |
| `src/editor_window.[ch]`   | Per-task editor; debounced write-through saves; Advanced fold for Subtasks/Attachments |
| `src/settings_window.[ch]` | The Settings window                                |
| `src/oauth.[ch]`           | OAuth 2.0 installed-app flow: PKCE, loopback redirect |
| `src/gtasks.[ch]`          | Two-way Google Tasks sync engine + move/clear jobs |
| `src/bnotes.[ch]`          | Notes CLI wrapper (never its database) |
| `src/bnsync.[ch]`          | Notes action-item mirror: worker pass, uid identity, bulk write-back |
| `src/http.[ch]`            | Small libcurl wrapper (blocking; worker threads only) |
| `src/json.[ch]`            | Minimal JSON parser/serializer (no external JSON dependency) |
| `icons/`                   | Bundled PNG toolbar icons + app logo; `icons/theme/hicolor/` holds SVG arrows for crisp HiDPI tree expanders |

## Database format

Everything lives in one ordinary SQLite file (see *Storage* in the
[User Guide](User_Guide.md)), so any standard SQLite tool can read it:

```sql
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
  group_id   INTEGER REFERENCES list_groups(id),  -- optional group (v5)
  gtasks_id  TEXT,                          -- bound Google tasklist
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
                                              -- 2 Done (v7; replaced the
                                              -- boolean `done`)
  pinned       INTEGER NOT NULL DEFAULT 0,    -- local-only, never synced
  priority     INTEGER NOT NULL DEFAULT 0,    -- local-only high-priority flag (v4)
  position     INTEGER NOT NULL DEFAULT 0,
  gtasks_id    TEXT,                          -- bound Google task
  updated_at   INTEGER NOT NULL DEFAULT 0,    -- UNIX seconds
  deleted      INTEGER NOT NULL DEFAULT 0,    -- tombstone until pushed
  completed_at INTEGER NOT NULL DEFAULT 0,
  etag         TEXT,                          -- push guard (If-Match)
  web_link     TEXT,                          -- Google mirror fields...
  glinks       TEXT,                          --   links[] as JSON
  assigned     TEXT,                          --   assignmentInfo origin
  bn_uid       INTEGER NOT NULL DEFAULT 0,    -- Notes item identity
  bn_done      INTEGER NOT NULL DEFAULT 0,    -- what Notes last held:
  bn_due       INTEGER NOT NULL DEFAULT 0     --   the bulk-push baseline
);

CREATE TABLE attachments (
  id         INTEGER PRIMARY KEY,
  task_id    INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  path       TEXT    NOT NULL,               -- a reference, not a copy
  added_at   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE sync_state (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE bn_deleted  (uid INTEGER PRIMARY KEY); -- mirror tasks the
                                                    -- user deleted here
CREATE TABLE bn_pins     (ref TEXT PRIMARY KEY);  -- LEGACY pre-mirror
CREATE TABLE bn_priority (ref TEXT PRIMARY KEY);  -- LEGACY pre-mirror

CREATE INDEX idx_tasks_list   ON tasks(list_id, parent_id, position);
CREATE INDEX idx_tasks_bn_uid ON tasks(bn_uid);
```

`idx_tasks_bn_uid` is created AFTER the guarded migrations, not with the
schema above: on an existing file `bn_uid` does not exist until the
`ALTER` has run, and a failing `CREATE INDEX` in that batch would take
the rest of the schema setup down with it.

The schema version rides in `PRAGMA user_version` (currently **7**);
older files are migrated in place at open.  Migration history: v2 adds
`lists.emoji`; v3 adds five Google-mirror task columns (`completed_at`,
`etag`, `web_link`, `glinks`, `assigned`); v4 adds `tasks.priority`;
v5 adds `lists.group_id`; v6 adds the three Notes-mirror task columns
(`bn_uid`, `bn_done`, `bn_due`); v7 adds `tasks.status`, copies
`done = 1` onto it as `2`, and DROPS `tasks.done`.

The v7 drop only runs if the backfill `UPDATE` returned `SQLITE_OK` —
dropping the source column after a copy that never happened would throw
every completion away.  An sqlite older than 3.35 has no `DROP COLUMN`
and simply leaves `done` behind, unread; its `NOT NULL DEFAULT 0` keeps
`INSERT`s working, so nothing breaks either way.

Semantics worth knowing when querying directly:

- **Tombstones**: `deleted = 1` rows are pending remote deletes — the
  app hides them and purges them once the delete is pushed (or
  immediately when the row never synced). Filter them out of any
  direct query.
- `due` is midnight *local time* on the due day, as UNIX seconds;
  0 means no due date. Google's side is date-only, so this loses
  nothing in sync.
- `gtasks_id`, `etag` and `updated_at` are the sync identity: a row
  with a NULL `gtasks_id` has never been pushed; `updated_at` newer
  than the remote copy means locally dirty.
- `position` (both tables) and `lists.emoji` are local-only.
  `tasks.web_link`, `glinks` and `assigned` are read-only mirrors of
  Google fields, shown in the editor's From Google section.
- Writes to the local-only flags `pinned` and `priority` deliberately
  **do not** touch `updated_at` — bumping it would mark the row
  sync-dirty and cost a no-op PATCH per toggle, and could starve a
  concurrent remote edit behind a 412 skip. Only fields that Google
  actually stores may stamp it. (A full-row `bt_db_task_update` still
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
- `sync_state` is a key/value scratchpad: `last_sync` (start time of
  the last successful pass), `default_list_gid` (Google's undeletable
  default tasklist), `lists_custom_order` (set once the user
  drag-reorders lists).
- `bn_pins` and `bn_priority` keys are Notes `NOTEID:ORD` refs —
  pinning and high-priority for action items live entirely on this side
  (Notes knows neither concept).

Two practical cautions: the app sets a 5-second busy timeout (the GUI
and the sync worker share the file), so brief external readers coexist
fine, but long write transactions from other tools will stall it; and
prefer backing up while the app is closed — a copy taken mid-sync can
catch a transaction in flight.

## Sync engine

The design goal is to be **non-destructive by default**: absence on
one side never deletes on the other; only explicit deletes propagate.

- Sync runs on a worker thread with its **own SQLite connection** (a
  connection never crosses threads); progress and completion are
  marshalled back to the main loop. `curl_global_init` happens in
  `main()` before any thread exists — libcurl's implicit init is not
  thread-safe.
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

Installed-app flow per RFC 8252: PKCE (S256, GLib SHA-256), a
loopback `GSocketService` on an ephemeral port for the redirect, and
`access_type=offline` for a refresh token. The client credentials
resolve in order: a `client_secret….json` next to the binary (or in
the user config dir) → legacy `google_client_id`/`google_client_secret`
ini keys → a baked-in default from `client_credentials.mk`. The
refresh token persists in `lists.ini` (`gtasks_refresh_token`);
access tokens live in memory only. The redirect listener redeems the
authorization code exactly once — browsers sometimes replay the
redirect GET, and a second exchange would revoke the first grant.
