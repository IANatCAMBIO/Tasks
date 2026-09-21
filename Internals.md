# Tasks — Internals

How Tasks is put together: the source layout and the database schema.
For everyday use see the [User Guide](User_Guide.md); for build
instructions see the [README](README.md).

## Code layout

The application is the list of files below and nothing else.

| File                       | Purpose                                            |
|----------------------------|----------------------------------------------------|
| `src/main.c`               | GtkApplication entry point: config → database → registries → window |
| `src/app.[ch]`             | Shared `TaskApp` context: ini config, dialogs, icon loading, date helpers |
| `src/backup.[ch]`          | Optional rotating database backups: worker thread, VACUUM INTO + verify, bounded rotation |
| `src/db.[ch]`              | SQLite layer: lists, tasks, subtasks, attachments; tombstones and `updated_at` for sync |
| `src/recur.[ch]`           | Recurring tasks: the preset table, GDateTime schedule arithmetic, and the periodic pass that rolls due repeats forward |
| `src/library_window.[ch]`  | Sidebar (virtual views, list groups), tall task rows, toolbar, compact controls + floating button bar, Kanban board, context menus, status bar |
| `src/editor_window.[ch]`   | Per-task editor; debounced write-through saves; Advanced fold for Recurrence/Subtasks/Attachments |
| `src/settings_window.[ch]` | The Settings window |
| `src/task_view.[ch]`       | The sidebar view registry |
| `src/task_ops.[ch]`        | Core operations (move, clear completed) |
| `src/task_worker.[ch]`     | The one background scheduler: timers, db path, re-arm on a database switch |
| `src/task_rows.[ch]`       | Task-row rendering |
| `src/core_views.c`         | The app's sidebar views (Favorites, All Tasks, Due Today), registered through the view registry |
| `icons/`                   | Bundled PNG toolbar icons + app logo; `icons/theme/hicolor/` holds SVG arrows for crisp HiDPI tree expanders |

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
  completed_at INTEGER NOT NULL DEFAULT 0,     -- UNIX; when LAST completed.
                                                -- Monotonic: nothing clears
                                                -- it, so it outlives a
                                                -- reopen.  0 = never.

  -- The recurrence schedule (v10) -- ALL local-only, never synced.
  recur_interval INTEGER NOT NULL DEFAULT 0,    -- repeat every N units;
                                                -- 0 = does not recur
  recur_unit     INTEGER NOT NULL DEFAULT 0,    -- 0 min, 1 hour, 2 day,
                                                -- 3 week, 4 month, 5 year
  recur_time     INTEGER NOT NULL DEFAULT 480,  -- minutes past local
                                                -- midnight (08:00); the
                                                -- day/week/month/year
                                                -- units only
  recur_lead     INTEGER NOT NULL DEFAULT 7200, -- minutes before an
                                                -- occurrence that a DONE
                                                -- task is reset (5 days)
  recur_next     INTEGER NOT NULL DEFAULT 0     -- UNIX; the next
                                                -- occurrence, 0 = not
                                                -- computed yet
);

CREATE TABLE attachments (
  id         INTEGER PRIMARY KEY,
  task_id    INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  path       TEXT    NOT NULL,               -- a reference, not a copy
  added_at   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE sync_state (key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX idx_tasks_list ON tasks(list_id, parent_id, position);

-- ------------------------------------------- owned by the integrations
-- Created by the v8/v9 migrations (and, when they return, by the
-- integrations themselves).  A database that never had them does not.

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
by the integration. `ON DELETE CASCADE`
so purging a task cannot leave its remote identity behind to be matched
against later. Schema **v8** moved the Google columns out of `tasks` and
`lists`; **v9** moved the Notes ones; **v10** added the five `recur_*`
columns.

Those two migrations still name the side tables, and must: a migration
moves data that already exists whether or not the integration that will
read it is present. Each creates what it needs itself, with
`IF NOT EXISTS`.

The schema version rides in `PRAGMA user_version` (currently **10**, from
`TASK_DB_SCHEMA_VERSION`).

**v10** is the cheap end of the migration spectrum and worth contrasting
with v8/v9: `ALTER TABLE ... ADD COLUMN` only, so nothing is copied,
nothing is dropped and the table is not rewritten. It is guarded on the
COLUMN not existing rather than on the version alone, which makes it
idempotent — the version stamp has been seen not to stick on a database
in a sync folder, and an `ALTER` re-run on a healthy file would log
"duplicate column name" on every launch. No index is created on those
columns: an index over an `ALTER`-added column would have to be created
after the migrations rather than in the schema block, and the recurrence
pass scans a few thousand rows every few minutes, which wants none.

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
  g.task_id = t.id`.  The Google and Notes side tables are created by
  the v8/v9 migrations and kept in the file; nothing in this build
  writes them, and the integrations that own them return later.
- `position` (both tables) and `lists.emoji` are local-only.
  `gtasks_task.web_link`, `glinks` and `assigned` are read-only mirrors
  of Google fields.
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
- `completed_at` is **monotonic**: stamped when a row enters status `2`,
  and never cleared by anything. It means "when was this last
  completed?", so it survives the task being reopened — a row can be
  `status = 1` and still carry a stamp, and both the editor and the
  list's Completion Date column show it. Re-completing moves it forward.
  The rule is an SQL `CASE` over the OLD row values in the local write
  paths, and a `MAX(completed_at, ?)` merge in `task_db_task_apply_remote`
  — every remote source reports `0` for "not currently done", so
  assigning there would let a Google or Notes un-tick erase local history
  that source never knew about. (Before 2026-08-27 leaving status `2` did
  clear it; history erased then is not recoverable.)
- The `recur_*` columns are the whole recurrence feature: there is no
  template row and no generated series, so a recurring task is an
  ordinary task that comes back round. `src/recur.c` walks
  `recur_interval > 0 AND deleted = 0` every few minutes on the main
  thread; when an occurrence is within `recur_lead` of now it writes
  `due` and resets a status-`2` row to `0`, both in one statement whose
  `CASE`s read the OLD status. Missed occurrences are **skipped**, not
  replayed. `recur_next` is bookkeeping and is written alone, with **no**
  `updated_at` bump, exactly like `pinned` and `priority`; a real
  roll-forward does stamp it, because `due` and `status` are synced
  fields. `recur_lead` is clamped at read time to shorter than one
  repeat period, so an hourly schedule cannot sit permanently inside its
  own lead window.
- `sync_state` is a key/value scratchpad shared by the app and its
  integrations, and its keys are NOT namespaced — an integration
  prefixes its own.  `lists_custom_order` and `backup_source_stamp` are
  the app's; `last_sync`, `default_list_gid` and `bn_*` belong to the
  syncs.
- `notes_task.done` / `.due` are a BASELINE, not a mirror: they hold
  what Notes was last known to have, so the rows whose done-ness or due
  date differs from them *are* the pending write set. There is no queue
  table to corrupt, and the set survives a crash.
- Pinning and high-priority for mirrored action items live entirely on
  this side, on the task row's own `pinned` / `priority` flags (Notes
  knows neither concept).

Two practical cautions: the app sets a 5-second busy timeout (the GUI
and the backup worker share the file), so brief external readers coexist
fine, but long write transactions from other tools will stall it; and
prefer backing up while the app is closed — a copy taken mid-write can
catch a transaction in flight.
