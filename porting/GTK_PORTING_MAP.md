# Tasks (GTK4) → AppKit porting map

Source of truth: `/Users/ian_campbell/salt_development/tasks` working tree at
HEAD `9ce1df8` (master) **plus one uncommitted change to `src/library_window.c`**
(see §7).  Everything below was read from `src/`, not from the docs, unless a
line says "per docs".  Nothing was built or run (read-only brief), so runtime
behaviour is inferred from source; see "Not verified" at the end.

All paths are relative to `/Users/ian_campbell/salt_development/tasks/`.

---

## 0. Headline numbers

| | |
|---|---|
| `src/` total | 18 `.c` + 15 `.h` = **18,425 lines** |
| UI + plumbing (must be rewritten) | `library_window.c` 2027, `editor_window.c` 2018, `sidebar.c` 1574, `kanban.c` 1444, `task_list.c` 1423, `settings_window.c` 1035, `app.c` 1359 (mixed), `main.c` 433, `list_rows.[ch]` 358, `library_priv.h` 328 → **~11,900 lines** |
| "Core" (no GTK symbol referenced) | `db`, `recur`, `backup`, `search`, `task_ops`, `task_worker`, `task_view`, `core_views`, `task_rows` → **5,774 lines** |
| `gtk_` token occurrences | 1,187 (+24 `gdk_`), all in 9 files |
| GLib call sites in the core (incl. the non-GTK third of `app.c`) | **~472** `g_*()` calls, ~520 `gchar/gint/…` typedef tokens |
| Files that are GTK-free **and** GLib-free | **none** in `src/` or `tests/` |
| Network / sync / JSON / OAuth code in this build | **none** |
| Keyboard accelerators | **none** (`library_window.c:1524` "There are no accelerators.") |
| Custom drawing (cairo / snapshot / GtkDrawingArea) | **none** — all visuals are widgets + CSS |
| Threads | exactly one kind: the backup worker (`backup.c:374`) |
| Tests | 1 file, 18 GLib `g_test` cases, pure logic, but **links `app.o` and therefore libgtk-4** |

---

## 1. Module inventory

Counts are token occurrences from `grep -o` (`gtk_`, `gdk_`, `g_`, `G_`,
`pango_`, `cairo_`, `GTK_/GDK_`).  `adw_`: 0 everywhere (no libadwaita).
`cairo_`: 0 everywhere.

### 1.1 `src/`

| File | Lines | gtk_ | gdk_ | g_ | G_ | pango | GTK_/GDK_ | Class | Purpose |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| `main.c` | 433 | 30 | 4 | 54 | 15 | 0 | 6 | (c) | GtkApplication entry, first-run async chain, startup/activate, SIGTERM, macOS log filter |
| `app.c` | 1359 | 115 | 8 | 171 | 36 | 0 | 70 | (c) **mixed** | 3 unrelated things in one file: event listeners (27–182, GLib only), GTK helpers — dialogs/menus/CSS/icons/tooltips/toolbar buttons/click helpers (183–1007), **config ini + date helpers (1008–1359, GLib only)** |
| `app.h` | 507 | 3 | 0 | 13 | 2 | 0 | 0 | (c) | `TaskApp` struct (holds `GtkApplication*`, `GtkWidget*`), all of the above; `#include <gtk/gtk.h>` (line 19) |
| `db.c` | 2148 | 0 | 0 | 137 | 6 | 0 | 0 | (b) | SQLite schema v12, migrations v8–v12, CRUD, health check, SHA-256, VACUUM INTO copy |
| `db.h` | 643 | 0 | 0 | 8 | 0 | 0 | 0 | (b) | Public data API; includes only `<glib.h>` + `<sqlite3.h>` |
| `recur.c` | 768 | 0 | 0 | 62 | 2 | 0 | 0 | (b) | Recurrence arithmetic (GDateTime), summary phrases, the deadline-armed pass |
| `recur.h` | 307 | 0 | 0 | 2 | 0 | 0 | 0 | (b)* | *includes `app.h` → `gtk/gtk.h` |
| `backup.c` | 422 | 0 | 0 | 83 | 5 | 0 | 0 | (b) | Rotating backups: GThread worker, own sqlite connection, verify, prune |
| `backup.h` | 118 | 0 | 0 | 3 | 0 | 0 | 0 | (b)* | *includes `app.h` |
| `search.c` | 153 | 0 | 0 | 29 | 0 | 0 | 0 | (b) | Query parser + matcher (UTF-8 casefold) |
| `search.h` | 78 | 0 | 0 | 1 | 0 | 0 | 0 | (b) | includes `<glib.h>` + `db.h` only |
| `task_ops.c` / `.h` | 42 / 43 | 0 | 0 | 1 | 0 | 0 | 0 | (b)* | move-to-list, clear-completed |
| `task_worker.c` / `.h` | 140 / 121 | 0 | 0 | 11 | 4 | 0 | 0 | (b)* | Worker registry + periodic timer (GLib main loop) |
| `task_view.c` / `.h` | 88 / 106 | 0 | 0 | 9 | 0 | 0 | 0 | (b)* | Virtual-view registry (Favorites / All / Due Today) |
| `core_views.c` / `.h` | 93 / 18 | 0 | 0 | 0 | 0 | 0 | 0 | (b)* | Registers the 3 built-in views |
| `task_rows.c` | 395 | 0 | 0 | 79 | 1 | 2† | 0 | (b) **UI-coupled** | Builds the tall-row text **as Pango markup** and fills a `GListStore` of `TaskRow` GObjects († the 2 "pango" hits are comments) |
| `task_rows.h` | 91 | 0 | 0 | 1 | 0 | 0 | 0 | (c) | includes `list_rows.h` → `gtk/gtk.h`; exposes `GListStore*`, `GHashTable*` |
| `list_rows.c` / `.h` | 233 / 125 | 3 | 0 | 31 | 22+3 | 0 | 0 | (c) | GObject row classes `TaskRow`, `TaskSbRow`, "changed" signal, `GtkSignalListItemFactory` wrapper |
| `library_window.c` | 2027 | 133 | 0 | 189 | 41 | 6 | 94 | (c) | Main window chrome, menubar, actions, toolbar, status bar, search, refresh orchestration, task context menu, compact mode, About |
| `library_window.h` | 56 | – | – | – | – | – | – | (c) | 2 entry points |
| `library_priv.h` | 328 | – | – | – | – | – | – | (c) | `TaskLibrary` struct + `lib_*` cross-file API for the 4 library files |
| `sidebar.c` | 1574 | 154 | 1 | 156 | 61 | 1 | 90 | (c) | Sidebar tree, list/group dialogs, context menus, list drag-reorder, auto-fit width |
| `task_list.c` | 1423 | 124 | 6 | 148 | 77 | 1 | 37 | (c) | Column view, cell factories, sorters, column-visibility menu, manual-sort drag, saved row order |
| `kanban.c` | 1444 | 144 | 4 | 113 | 19 | 2 | 71 | (c) | Board, cards, selection, hand-rolled drag, ghost, order persistence, CSS |
| `editor_window.c` | 2018 | 342 | 0 | 125 | 48 | 2 | 248 | (c) | Per-task editor, debounced save, subtasks, attachments, recurrence block, calendar picker |
| `editor_window.h` | 61 | – | – | – | – | – | – | (c) | 4 entry points |
| `settings_window.c` | 1035 | 142 | 1 | 87 | 16 | 0 | 89 | (c) | Settings singleton: Appearance, Database health plate, backups |
| `settings_window.h` | 28 | – | – | – | – | – | – | (c) | 1 entry point |

`*` = the `.c` uses only GLib, but its header pulls in `app.h`, which pulls in
`<gtk/gtk.h>` and a `TaskApp` struct with GTK members.  Only `db.c` and
`search.c` compile today without GTK headers on the include path.

`nm -u` on the existing objects confirms the call-level picture: `db.o`,
`search.o`, `recur.o`, `backup.o`, `task_worker.o`, `task_view.o`,
`task_ops.o`, `core_views.o`, `task_rows.o` reference **zero** `gtk_`/`gdk_`
symbols; `list_rows.o` references 2; `app.o` references many.

### 1.2 GLib facilities used by the core (what a strip has to replace)

Call-site counts across `db.c`, `recur.c`, `backup.c`, `search.c`,
`task_rows.c`, `task_view.c`, `task_worker.c`, `task_ops.c` and the two non-GTK
parts of `app.c`:

| Facility | Sites | Where | Replacement difficulty |
|---|---:|---|---|
| `g_free` / `g_strdup` / `g_strdup_printf` / `g_new0` / `g_strndup` / `g_malloc` | ~190 | everywhere | trivial (libc / `asprintf`) |
| `gchar/gint/gint64/gboolean/guint/gsize/gpointer` typedefs | ~520 tokens | everywhere | trivial (typedef header or sed) |
| `GPtrArray` | ~50 | db (12 public functions return one), task_rows, task_view, backup, search | **API-visible**; needs an agreed array type |
| `GString` | ~35 | db (pragma collectors, reorder SQL), search, task_rows | small string builder |
| `GHashTable` | ~13 | `task_db_attachment_counts` (public), `TaskRowCtx` (public struct), listeners no | int→ptr map |
| `GDateTime` / `GTimeZone` | ~50 | `app.c:1153–1337`, `recur.c:67–121, 339–346, 403, 512–522`, `backup.c:274–276` | **hard-ish**: calendar add with month clamp, DST wall-clock preservation, tz cache, `%b %-e, %Y` / `%A` / `%I:%M %p` / `%Y%m%d-%H%M%S` formats |
| `GKeyFile` | 7 | `app.c:1076–1136` | easy (flat `[tasks]` key=value) — but see ini-compat risk |
| UTF-8: `g_utf8_casefold`, `g_unichar_isspace`, `g_utf8_get_char/next_char/find_prev_char/validate/make_valid` | ~14 | `search.c`, `task_rows.c` | moderate (CFString / NSString or hand-rolled) |
| `g_markup_escape_text` | 3 | `task_rows.c` | disappears with Pango markup |
| `GChecksum` SHA-256 | 4 | `db.c:449–459` | CommonCrypto `CC_SHA256` (in SDK) |
| Main loop: `g_timeout_add_seconds(_full)`, `g_source_remove`, `g_idle_add` | 9 | `recur.c:579–591`, `task_worker.c:98–118`, `backup.c` ×5 | GCD / NSTimer — needs one agreed shim |
| `GThread` | 2 | `backup.c:374–375` | pthread / GCD |
| `GSList` | 7 | `app.c` listeners, `task_worker.c` | trivial |
| `GArray` | 6 | `search.c` | trivial |
| `GError` | 3 | `task_db_open` signature (`db.h:283`), `backup.c:244` | change signature |
| File utils: `g_build_filename`, `g_file_test`, `g_mkdir_with_parents`, `g_get_user_data_dir`, `g_canonicalize_filename`, `g_path_get_dirname/basename`, `g_dir_*`, `g_stat`, `g_unlink`, `g_fopen`, `g_access`, `g_get_current_dir` | ~30 | db, backup, app config | easy (POSIX) |
| Logging: `g_warning`, `g_message` | 17 | db (15), backup, task_view | `fprintf(stderr…)` / `os_log` |
| GIO: `GListStore`, `g_list_store_splice` | 2 | `task_rows.c:344` | disappears with the row model rewrite |

No `g_spawn`, no `GSettings`, no `GSubprocess`, no GResource, no GVariant in the
core.  `GApplication`, `GAction`, `GMenu`, `GVariant` appear only in the UI
files.

### 1.3 `tests/`, `tools/`, `plugins/`, `dev/`, others

| Path | Lines | What | Class |
|---|---:|---|---|
| `tests/test_main.c` | 359 | 18 `g_test` cases: 6 search, 9 recurrence, 3 db (fresh schema, create/read, status stamps `updated_at` — sleeps 1 s). Includes `app.h`; Makefile links `app.o db.o search.o recur.o backup.o task_worker.o` with full gtk4 flags (`Makefile:167–173`). No display needed (no `gtk_init`), but **GTK must be installed to link**. | (c) by linkage, (b) by content |
| `tools/import-apple-reminders.sh` | 232 | AppleScript export → python3/sqlite3 import into `tasks.db`. No GLib. | (a) |
| `tools/winshot.sh`, `tools/winlist.swift` | 20, 16 | Screenshot a window of the running `tasks` process by title (CoreGraphics). Hard-codes process name `tasks`. | (a) |
| `plugins/*.so` (+ `.dSYM`, 4 READMEs) | binaries | **Stale build products of a plugin system that no longer exists** (untracked; built 2026-09-14; source deleted in commit `9b86b50` "Remove the plugin system"). `forecast.so`, `overdue.so`, `notes.so`, `gtasks.so`. `gtasks.so` links `/opt/local/lib/libcurl.4.dylib`. Nothing in `src/` loads them (no `dlopen` / `g_module`). | dead |
| `src/plugins/` | – | Only a `.DS_Store` (ignored). | dead |
| `build/` | – | Objects; also stale `plugin_loader.o`, `plugin_owner.o`, `task_ui.o` from before the removal. Ignored. | – |
| `dev/data/tasks/` | – | Sandbox DB (32 KB) + ini for `make run-dev`. Ignored. | – |
| `dist/` | – | Empty (only `.DS_Store`). | – |
| `client_credentials.mk` | 5 | Google OAuth client id + secret for the deleted gtasks plugin. Untracked, **not included by the Makefile**, referenced by nothing. Contains a secret — do not copy it into a fork. | orphan |
| `icons/` | – | 10 PNGs used, 6 unused at top level, 16 in `icons/Unused/`, `theme/hicolor` (8 SVG arrows + `apps/tasks.png`) | resources |
| `compile_commands.json`, `.cache/clangd` | – | Generated; `.cache` index still names the deleted plugin files. | – |

---

## 2. Layering

### 2.1 Verdict

**The call direction is clean; the header/packaging boundary is not.**

- No core `.c` file calls a GTK function (verified by `nm -u`, §1.1).
- The UI calls down into `db.h`, `recur.h`, `backup.h`, `search.h`,
  `task_ops.h`, `task_worker.h`, `task_view.h`, `task_rows.h`.
- The core calls **up** into `app.c`, but only into its non-GTK functions:
  `task_app_status`, `task_app_notify_changed`, `task_app_config_get(_bool)`,
  `task_local_tz`, `task_local_dt`, `task_due_format`, `task_clock_format`,
  `task_due_from_ymd`, `task_day_bounds` (from `recur.c`, `backup.c`,
  `task_worker.c`, `core_views.c`, `task_rows.c`).

What stops the core from being lifted as-is:

1. **`app.h` is the hub and it is a GTK header.**  `recur.h:110`,
   `backup.h:52`, `task_worker.h:33`, `task_view.h:28`, `task_ops.h:16`,
   `task_rows.h:29` all `#include "app.h"`, which includes `<gtk/gtk.h>`
   (`app.h:19`) and defines `TaskApp` with `GtkApplication *gtk_app` and
   `GtkWidget *library_window` (`app.h:71–86`).
2. **`app.c` is three modules in one file** (listeners / GTK helpers /
   config + dates).  The config and date code is core, but lives in a file
   that cannot compile without GTK.
3. **`task_rows.c` is called "model layer" but its output is toolkit-specific**:
   `task_rows_desc_markup` returns a Pango markup string (`<b>`, `<s>`, `<i>`,
   `<small>`, `<span alpha="60%">`; `task_rows.c:75–213`) and
   `task_rows_append` fills a `GListStore` of GObject `TaskRow`s
   (`task_rows.c:296–349`).  Both the list (`task_list.c:1201`) and the board
   (`kanban.c:1159`) consume that markup.
4. **The core depends on the GLib main loop**: one-shot deadline timer
   (`recur.c:575–603`), periodic worker timers (`task_worker.c:116`), and
   thread→main marshalling via `g_idle_add` (`backup.c:251…345`).
5. **Tests link `app.o`** (for the date helpers), so they need GTK.

Nothing in the core calls back into a window: all UI refresh goes through the
listener bus (`task_app_notify_changed` / `_tasks` / `_status`).

### 2.2 Public API of the non-UI core

Everything below is what a UI engineer programs against today.  GLib types in
signatures are marked **[G]**.

#### `db.h`

Types (`db.h:75–274`):

```c
typedef struct { gboolean ok, ran; gint64 when; gchar *detail; } TaskDbHealth;
typedef struct { sqlite3 *sq; gchar *path; TaskDbHealth health; } TaskDatabase;
typedef enum { TASK_STATUS_NEW=0, TASK_STATUS_IN_PROGRESS=1, TASK_STATUS_DONE=2 } TaskStatus;   /* on-disk values */
typedef enum { TASK_RECUR_MINUTE=0, HOUR, DAY, WEEK, MONTH, YEAR=5 } TaskRecurUnit;            /* on-disk values */
typedef struct { gint64 id; gchar *name, *emoji; gint64 updated_at; gint position; gint64 group_id; gboolean deleted; } TaskList;
typedef struct { gint64 id; gchar *name; gint position; } TaskGroup;
typedef struct {
    gint64 id, list_id, parent_id;  gchar *title, *notes;
    gint64 due;  gint due_time;  TaskStatus status;  gboolean pinned, priority;
    gint position;  gint64 updated_at;  gboolean deleted;  gint64 completed_at;
    gint recur_interval;  TaskRecurUnit recur_unit;  gint recur_time, recur_lead;
    gint64 recur_next, recur_start;
} Task;
typedef struct { gint64 id, task_id; gchar *path; } TaskAttachment;
```

Constants: `TASK_DB_FILENAME "tasks.db"`, `TASK_APP_DIR "tasks"`,
`TASK_DB_SCHEMA_VERSION 12`, `TASK_STATUS_N_VALUES 3`, `TASK_RECUR_N_UNITS 6`,
`TASK_RECUR_TIME_DEFAULT 480`, `TASK_DUE_TIME_DEFAULT 480`,
`TASK_RECUR_LEAD_DEFAULT 10080`.

Function groups (line refs are `db.h`):

- **Open / paths / safety** (283–384): `task_db_open(path, GError **)` **[G]**,
  `task_db_close`, `task_db_default_dir`, `task_db_default_path`,
  `task_db_resolve_path(dir)`, `task_db_verify_file(path, &detail)`,
  `task_db_health_check(db)`, `task_db_health(db)`, `task_db_file_sha256(path)`,
  `task_db_copy_file(db, dest, &err)`.
- **Lists** (394–431): `task_db_lists(db, include_deleted)` → **[G] GPtrArray of TaskList\***,
  `task_db_lists_reorder(db, ids, n)`, `task_db_list_get`, `task_db_list_create(db, name, emoji)`,
  `task_db_list_update`, `task_db_list_delete` (tombstones list + tasks).
- **Groups** (620–641): `task_db_groups` **[G]**, `task_db_group_get`,
  `task_db_group_create`, `task_db_group_delete`, `task_db_group_rename`,
  `task_db_list_set_group(db, list_id, group_id)`.
- **Task queries** (435–471), all return **[G] GPtrArray of Task\***:
  `task_db_tasks_toplevel(list)`, `task_db_subtasks(parent)`,
  `task_db_subtasks_all_visible`, `task_db_tasks_pinned`,
  `task_db_tasks_all_visible`, `task_db_tasks_in_group(group)`,
  `task_db_tasks_due_between(lo, hi)`; plus `task_db_task_get(id)`,
  `task_db_has_pinned`.  Every list query orders `priority DESC` first
  (`db.c:1378–1462`).
- **Task writes** (476–536): `task_db_task_create(db, list, parent, title)` (0 on
  failure), `task_db_task_update(db, const Task *)`,
  `task_db_task_set_status / _set_pinned / _set_priority`,
  `task_db_task_delete` (tombstone + subtasks), `task_db_subtask_move(id, ±1)`,
  `task_db_task_move_list(id, dest)`.
- **Recurrence SQL** (515–518): `task_db_tasks_recurring` **[G]**,
  `task_db_task_recur_apply`, `task_db_task_recur_set_next`.
- **Attachments** (548–556): `task_db_attachments(task)` **[G]**,
  `task_db_attachment_add`, `task_db_attachment_remove`,
  `task_db_attachment_counts` → **[G] GHashTable** (ids packed into pointers).
- **Misc** (562–600): `task_db_totals(&n_tasks, &n_lists)`, `task_db_scalar(sql)`,
  `task_db_state_get/set(key)` (the `sync_state` k/v table).
- **Free helpers** (276–277, 614–621): `task_free`, `task_list_free`,
  `task_group_free`, `task_ptr_array_free_{lists,tasks,attachments,groups}` **[G]**.
- **Status helpers** (115, 130): `task_status_label`, `task_status_apply_done`.

`db.h` exposes `gboolean`, `gint`, `gint64`, `gsize`, `gchar *`, `GPtrArray *`
(12 functions + 4 free helpers), `GHashTable *` (1), `GError **` (1) and
`sqlite3 *`.  It does **not** expose `GDateTime`, `GString` or any GObject type.

**Declared but with no caller in this build** (sync-era API; candidates to
leave behind): `task_db_insert_remote_tombstone`, `task_db_list_apply_remote`,
`task_db_task_apply_remote`, `task_db_list_emoji_if_empty`, `task_db_list_purge`,
`task_db_task_purge`, `task_db_list_restore`, `task_db_purge_done`,
`task_db_task_apply_done_source`, `task_db_tasks_in_list_all`,
`task_status_apply_done`; also `task_rows_toggle_done` (duplicated by
`task_list.c:788`), `task_view_index_of`.

#### `app.h` — the non-GTK part (to be split out)

- **Events** (`app.h:202–229`): `task_app_notify_changed(app)` (full refresh),
  `task_app_notify_tasks(app)` (task pane only; falls back to full when nobody
  listens), `task_app_status(app, fmt, …)`;
  `task_app_listen_changed/tasks/status(app, fn, user_data)` → id,
  `task_app_unlisten(app, id)`.  Listener lists are copied before firing so a
  listener may unsubscribe mid-fire (`app.c:127–141`).
- **Config** (`app.h:408–418`): `task_app_config_init(argv0)`,
  `task_app_config_get(key)` (new string or NULL; empty reads as NULL),
  `task_app_config_get_bool(key, def)` (anything but `"0"` is true),
  `task_app_config_set(key, value)` (NULL removes; writes the whole file on
  every real change), `task_app_exe_dir()`.
- **Dates** (`app.h:439–505`): `task_local_tz()` **[G GTimeZone\*]**,
  `task_local_dt(ts)` **[G GDateTime\*]**, `task_day_bounds(offset, &lo, &hi)`,
  `task_due_format` ("Jul 13, 2026"), `task_due_format_at`, `task_due_format_iso`,
  `task_clock_format(minutes)` ("8:00 AM"), `task_due_instant(due, due_time)`,
  `task_due_color(due)` → `"#c01c28"` / `"#d19a00"` / `"#26a269"` / NULL,
  `task_due_parse("YYYY-MM-DD" | "M/D/YY[YY]")`, `task_due_from_ymd`.

#### Other core headers

- `recur.h:124–305`: `task_recur_unit_label`, `task_recur_period_seconds`,
  `task_recur_lead_default`, `task_recur_lead_seconds(const Task *)`,
  `task_recur_advance(from, unit, interval, at_minute)`,
  `task_recur_seed(const Task *, now)`, `task_recur_phrase`,
  `task_recur_describe(const Task *, now)` (two-line summary),
  `task_recur_pass(app)`, `task_recur_init(app)`, `task_recur_wake_by(app, ts)`.
- `backup.h:56–116`: `TASK_BACKUP_KEEP_DEFAULT 10`, `TASK_BACKUP_INTERVAL_DEFAULT 60`,
  `task_backup_dir()`, `task_backup_ready(&reason)`,
  `task_backup_start(app, db_path, done_fn, user_data)`,
  `task_backup_auto_start(app, db_path)`, `task_backup_init(app)`.
- `task_worker.h:46–119`: `TaskWorkerDef` (id, sort, enabled_key, interval_key,
  initial policy, `running*`, `timer*`, `run`, `ready`, `on_arm`),
  `task_worker_register`, `task_worker_arm`, `task_worker_arm_all(app, db_path)`.
- `task_view.h:32–104`: `struct TaskView { id, label, name, unit, sort,
  visible(), query() → GPtrArray* [G], virtual_rows, not_a_list, user_data }`,
  `task_view_register`, `task_view_count`, `task_view_nth`, `task_view_find`,
  `task_view_order_key(v, family)`.
- `search.h:57–76`: `task_search_parse(query)` (NULL = no filter),
  `task_search_free`, `task_search_matches(q, task, GPtrArray *subs [G])`.
- `task_ops.h:28–41`: `task_ops_move_to_list`, `task_ops_clear_completed`.
- `task_rows.h:47–89`: `TaskRowCtx { GHashTable *att_counts; GPtrArray *all_subs;
  GHashTable *subs_by_parent, *list_names; gboolean bold, show_done; }`,
  `task_row_ctx_init/clear`, `task_rows_append(GListStore *, tasks, ctx)`,
  `task_rows_desc_markup(...)`, `task_rows_toggle_done`.

---

## 3. UI feature inventory (parity checklist)

Legend for the last column: **E** easy in AppKit, **M** moderate, **H** hard.

### 3.1 Windows

| # | Window | Where | GTK | AppKit notes |
|---|---|---|---|---|
| W1 | **Library** — title "Tasks"; default 980×640; size restored from `win_w/win_h` (or `compact_w/compact_h` in compact mode — uncommitted) | `library_window.c:1762–2027` | `GtkApplicationWindow` > `GtkBox`: toolbar, rule, `GtkOverlay`(`GtkPaned`, ghost `GtkFixed`, float bar), rule, status bar | **M** — NSWindow + NSSplitView; overlay layers become sibling subviews |
| W2 | **Editor**, one per task (singleton keyed by task id in `app->editors`), title "Tasks - <title>", 490 × natural height | `editor_window.c:1459–1957` | plain `GtkWindow` | **H** — height is content-driven and changes with folds (§3.8) |
| W3 | **Settings** singleton, title "Tasks - Settings", 470 px column, scrolls past 600 px | `settings_window.c:740–1035` | `GtkWindow` + `GtkScrolledWindow` | **E** |
| W4 | **About** | `library_window.c:1295–1325` | `GtkAboutDialog` (name, `TASK_VERSION`, logo `document.png`@128, authors, "Compiled `__DATE__ __TIME__`", BSD-3 link) | **E** — `orderFrontStandardAboutPanelWithOptions:` |

Close/quit path: File → Quit destroys the library window
(`library_window.c:1328–1332`); `on_library_destroy` (1442–1510) persists
sizes, drops listeners, stops any card drag, closes every editor (each flushes
its pending save, `editor_window.c:1232–1241`), and the app exits when the last
window is gone.  SIGTERM does the same (`main.c:329–339`).

### 3.2 Dialogs, alerts, pickers

| # | What | Where | GTK |
|---|---|---|---|
| D1 | Modal custom-content scaffold: Cancel + accept button, Escape = cancel, close box = cancel | `app.c:418–467` | hand-built `GtkWindow` (no default widget is ever set — see "surprises") |
| D2 | New List / Edit List: `[emoji button][name entry]`, accept "_OK" | `sidebar.c:998–1053`, result 951–992 | D1 |
| D3 | New Group ("Create") / Rename Group ("Rename"): one entry | `sidebar.c:1080–1146` | D1 |
| D4 | Date picker: calendar + "Clear date" checkbox, accept "OK"; titles "Due Date" / "Start Date" | `editor_window.c:1054–1082`, result 1022–1047 | D1 + `GtkCalendar` |
| D5 | Notice (OK only): integrity-check failure at startup; "Could not open" database | `app.c:192–213`; `main.c:150`; `library_window.c:1069` | `GtkAlertDialog` |
| D6 | Confirm (No / Yes, async): Delete Task(s); Clear Completed; "Set as default database?"; Remove Group; Delete List | `app.c:243–259`; `library_window.c:755, 1046, 1138`; `sidebar.c:1178, 1291` | `GtkAlertDialog` |
| D7 | First-run Welcome: "Open a tasks.db File" / "Create a New tasks.db"; dismiss = quit; cancelled chooser returns to the choice | `main.c:172–258` | `GtkAlertDialog` + file dialog |
| D8 | File pickers: first-run open (filter `tasks.db`); File → Open Database (`*.db`); Attach File; backup folder (starts in current backup dir) | `app.c:302–347`; `main.c:223`; `library_window.c:1147`; `editor_window.c:968`; `settings_window.c:453` | `GtkFileDialog` |

AppKit: all **E** (`NSAlert` sheets, `NSOpenPanel`, a small sheet window).  The
existing API is callback-async, which maps directly onto completion blocks.

### 3.3 Menus

| # | Menu | Where | Items |
|---|---|---|---|
| M1 | **Menubar: File** | `library_window.c:1711–1721` | New Task · New List… · Clear Completed Tasks ‖ Open Database File… · Settings… · About · Quit |
| M2 | **Menubar: View** | `library_window.c:1728–1743` | Hide/Show Completed · Manual/Automatic Sorting ‖ Hide/Show Sidebar · Compact/Full Controls · Kanban/List View |
| M3 | **Task context menu** (shared by list and board, acts on the current selection) | `library_window.c:897–991` | Info… (single only) ‖ Mark Complete / Mark Incomplete (single: the applicable one; multi: "Mark All …" both) ‖ Add to/Remove from Favorites, Set/Clear High Priority (single: applicable direction; multi: all four "… All …") ‖ Move to List ▸ (every list except the current one for a single selection; omitted for a lone subtask) ‖ Delete Task / "Delete N Tasks" |
| M4 | **Sidebar row menu** | `sidebar.c:480–568` | New List · New Group ‖ (list row) Edit List · Delete List ‖ Move to Group ▸ <groups> · Remove from Group (only if grouped) ‖ (group row) Rename Group · Remove Group |
| M5 | **Sidebar background menu** (empty space) | `sidebar.c:577–594` | New List · New Group |
| M6 | **Column header menu** (right-click any header; check marks) | `task_list.c:352–405` | Done · Status · Due Date · Completion Date |

Mechanics: every command is a `GAction`.  "app." actions on the application for
the menubar (table at `library_window.c:1541–1559`), "win." actions on the
window for toolbar and context menus (1561–1568, `sidebar.c:1554–1574`,
`task_list.c:352–381`); parameterised actions `mark-done(b)`, `set-pinned(b)`,
`set-priority(b)`, `move-to-list(x)`, `move-to-group(x)`, `rename-group(x)`,
`delete-group(x)`.

**The View-menu idiom to preserve**: there are no check items.  Each toggling
item is a *pair* of items with `hidden-when=action-disabled`
(`library_window.c:1676–1688`); exactly one is enabled by the state's single
"applier" (`lib_menu_pair_sync`, 1638–1644).  The label always names what a
click will *do*.  The sort pair is greyed entirely while Kanban is on or a
search is active (`library_window.c:236–241`).  In AppKit this collapses to
one `NSMenuItem` whose title/enabled state is set by the same appliers —
**E**, but the five appliers must stay the single writers:
`hide_done_icon_refresh` (437), `manual_sort_icon_refresh` (480),
`task_pane_mode_apply` (192), `compact_layout_apply` (631),
`lib_sidebar_ui_sync` (`sidebar.c:886`).

Context menus are built per popup and shown through `task_app_menu_popup`
(`app.c:521–545`) → `NSMenu popUpMenuPositioningItem:` / `menuForEvent:` — **E**.

On macOS today GTK's quartz backend also builds the application menu from
`app.about` / `app.preferences` / `app.quit` (`library_window.c:1515–1519`).

### 3.4 Toolbar, compact bar, status bar, search

| # | Feature | Where | Notes |
|---|---|---|---|
| T1 | Toolbar (icons only, 24 px, flat, never takes focus, tooltip on each): **New Task**, **Delete Task** │ **Sidebar**, **Completed**, **Sort Mode**, **Pane** … spacer … search box | `library_window.c:1833–1893`; button factory `app.c:809–843` | **E** as a plain view strip (it must be hideable as a whole for compact mode, and the design language is "no header bars", so NSToolbar is a choice to make, not a given) |
| T2 | State-swapping icons: Completed shows `hidden`/`visible`; Sort shows `manual`/`automatic`; Pane shows `board`/`menu`; Sidebar is always `left-and-right` with only its tooltip changing | `library_window.c:437–447, 480–489, 221–231`; `sidebar.c:886–894` | icon = the action on offer |
| T3 | Sort button greyed with a *reason-specific tooltip* (Kanban on / search active) | `library_window.c:236–254` | **E** |
| T4 | Text fallback when an icon file is missing (markup glyph) | `app.c:792–804` | optional in a bundle |
| T5 | **Compact Controls**: hides toolbar + rule, shows a 2-button floating pill (New / Delete) 20 px from bottom-right of the pane area; clears an active search; **(uncommitted)** also hides the sidebar, force-shows it on exit, and resizes the window between two remembered sizes | `library_window.c:631–662, 1190–1291, 1355–1411` | **M** — window resize logic is GTK-workaround-shaped (`compact_resize_to`, 1202–1227) and becomes a plain `setFrame:` |
| T6 | Status bar: left = "`<view>` - N [matching ]task(s)" (unit noun from the view); right = last event message, shown 3 s then cross-faded out over 1 s; both 85 % font, ellipsized | `library_window.c:372–376, 538–566, 1952–1998` | **E** (`NSAnimationContext`) |
| T7 | **Search box** (right end of toolbar, placeholder "Search this view", long tooltip): filters the selected view in place; debounced by the widget; Enter applies at once; Escape clears; status bar says "matching"; suspends manual sort (handle column hidden) | `library_window.c:407–430, 1875–1888`; filter 316–349; `library_priv.h:238–243` | **E** (`NSSearchField`) |
| T8 | ~34 status-message call sites via `task_app_status` | all files | carry over verbatim |

### 3.5 Sidebar (`sidebar.c`)

| # | Feature | Where | GTK | AppKit |
|---|---|---|---|---|
| S1 | Tree: bold view rows (⭐️ Favorites — only while something is pinned; 🔮 All Tasks; ☀️ Due Today), a bold non-selectable expandable **Lists** header, bold expandable **group** rows, list rows (`emoji␠␠name`) | build 1476–1548; populate 632–841; bind 446–472 | `GtkListView` + `GtkTreeListModel` + `GtkSingleSelection` + `GtkTreeExpander` | **M** — `NSOutlineView` |
| S2 | Exactly one row always selected; header can never be selected; selection survives rebuilds with a 4-step fallback chain (same kind+id → same kind → first list → first selectable) | 766–837 | | **M** |
| S3 | Expansion state snapshot/restore across rebuilds; Lists header and the group holding the selection are forced open; groups default open | 638–764 | | **M** |
| S4 | **Selecting a group shows the aggregated tasks of its lists** (virtual rows with "in <list>") | `library_window.c:294–307` | | carries over (core query) |
| S5 | Double-click a list → Edit List dialog | 1303–1315 | `activate` | **E** |
| S6 | **Drag-reorder lists** by dragging the row; live feedback = tinted source + 2 px top rule on target; dropping across a group boundary changes group membership; persists via `task_db_lists_reorder` | 176–401 | hand-rolled `GtkGestureDrag` + `gtk_widget_pick` | **M** — NSOutlineView native drag-and-drop does this more simply, but the "drop before the marked row, adopt its parent group" rule (386–394) must be kept |
| S7 | Right-click selects the row first, then pops M4; right-click on empty space pops M5 | 480–594, 1523–1529 | capture/bubble gestures | **E** |
| S8 | **Auto-fit width**: after every model change the divider moves to the widest visible row (min 160 px, max 50 % of the pane) | 1317–1470 | frame-clock after-paint + `gtk_widget_measure` | **M** — measure attributed strings; note it fights the user's manual divider position, which is nevertheless persisted (`sidebar_width`) |
| S9 | Show/hide sidebar, persisted `sidebar_visible` | 886–910 | | **E** (but see §7: the key is currently write-only) |
| S10 | Emoji picker button in the list dialog (shows current emoji or "…") | 918–944, 1014–1030 | `GtkEmojiChooser` popover | **M** — the system character palette inserts into the first responder; needs a small text-field trick |
| S11 | Look: shaded background (`shade(@window_bg_color, 0.96)`), text rgb(65,65,65), selection rgb(86,131,224) on white text, 3 px top pad strip | 89–123 | CSS | **E** (source-list style or custom row view) |

### 3.6 Task list (`task_list.c`)

| # | Feature | Where | GTK | AppKit |
|---|---|---|---|---|
| L1 | Columns: ⠿ handle (26 px, manual mode only) · ✓ checkbox · **Task** (expands; tall multi-line rich text) · Status · Due Date · Completed.  Titled columns are user-resizable; reordering columns is off | 1312–1423 | `GtkColumnView` | **H** — view-based `NSTableView` with variable row heights |
| L2 | **Tall row content** (one rich string): optional bold title, struck through when Done; prefix glyphs 🚨 priority, ⭐️ pinned, ↳ subtask; "in <list>" line in virtual views; first non-blank notes line, ≤120 bytes on a UTF-8 boundary + "…"; "📎 N attachment(s)"; up to 4 subtask lines ☐/☑ (done ones struck + dimmed); "… +N more subtasks".  Dimming is by **alpha**, never a fixed grey, so it survives the selection colour | `task_rows.c:75–213` | Pango markup in a `GtkLabel` | **H** — needs an `NSAttributedString` builder and row-height measurement; must honour selected-row text colour |
| L3 | Alternating stripes (`#e8f2fb` on even rows, not over selection); no focus ring; 8 px vertical cell padding | 249–301 | CSS | **E** (`usesAlternatingRowBackgroundColors` gives system colours, not this tint) |
| L4 | Click-to-sort headers with custom comparators: title (casefolded), done, status enum, due instant (undated last), completed (never last) | 1210–1303 | `GtkCustomSorter` + `GtkSortListModel` | **M** — sort descriptors over a C array |
| L5 | **Manual sort mode**: headers stop sorting, handle column appears, rows reorder by dragging the ⠿ handle (source tinted amber `#fde68a`, 2 px rule on target, row moves on release); order saved per view as a comma-separated id list; saved order re-applied on every refresh, unknown ids appended | apply 418–447; drag 575–737; save 157–180; load 193–240; permutation 37–83 | hand-rolled `GtkGestureDrag` | **M** — native table row drag is fine; keep `lib_row_order_permutation` logic verbatim |
| L6 | Manual sort is *suspended* while a search is active (never save an order from a filtered list) | `library_window.c:39–43` | | rule must carry |
| L7 | Multi-selection (Cmd/Ctrl-click toggles, Shift extends); row selects on press | `app.c:920–1006` | `GtkMultiSelection` + custom press handling | **E** (native) |
| L8 | Right-click: inside the selection keeps it, outside collapses to the clicked row; then M3 | 1089–1136 | legacy event controller workaround | **E** (native `menuForEvent:` / `clickedRow`) |
| L9 | Double-click (on the Task cell) or Enter opens the editor | 904–916, 1147–1157; `app.c:852–918` | custom double-click timer (macOS quartz workaround) | **E** (`doubleAction`) |
| L10 | ✓ checkbox: tick → Done, untick → In Progress; updates the row **in place** (no reload, selection/hover/scroll kept); full refresh only when the row must vanish (Done + completed hidden); refreshes open editors; status message "“title” — Completed" | 788–866 | | **M** — `reloadDataForRowIndexes:` + `noteHeightOfRowsWithIndexesChanged:` |
| L11 | Due Date cell tinted: overdue `#c01c28`, today `#d19a00`, ahead `#26a269`; time shown only when not 08:00 | 996–1026; `app.c:1281–1324` | CSS classes | **E** |
| L12 | Column visibility persisted (`col_done_visible`, `col_status_visible` default 0, `col_due_visible`, `col_completed_visible`) | 320–381 | stateful actions | **E** |
| L13 | Scroll position preserved across rebuilds | `library_window.c:72–110` | idle-deferred adjustment restore | **E/M** |
| L14 | "Show completed" filter applied when rows are built | `task_rows.c:305–306` | | carry |

### 3.7 Kanban board (`kanban.c`)

| # | Feature | Where | GTK | AppKit |
|---|---|---|---|---|
| K1 | Three equal lanes New / In Progress / Done (lane index **is** `TaskStatus`), each: centred heading "**Name**\nN task(s)" over a bordered, tinted box; whole board scrolls vertically as one, never horizontally | 1381–1444 | `GtkBox` homogeneous in `GtkScrolledWindow` | **M** — custom views in an `NSScrollView` |
| K2 | Card = ⠿ grip strip + the same rich text as the list row, **ellipsized not wrapped** (max 22 chars natural width); hover darkens border; optional drop shadow | 1046–1114; CSS 83–146 | `GtkBox` + CSS `box-shadow` | **M** — layer shadow |
| K3 | Selection set (hash of ids) + shift anchor: click selects one; Cmd/Ctrl toggles; Shift selects a run within one lane; plain click inside a multi-selection keeps it until release; selection pruned to surviving cards on rebuild | 181–329, 342–431, 1320–1343 | manual | **M** |
| K4 | Double-click opens editor; right-click (selecting if needed) pops M3 | 356–378 | `GtkGestureClick` | **E** |
| K5 | **Hand-rolled drag from the grip only**: threshold; translucent (0.65) snapshot ghost follows the pointer at the grip offset; original stays in place dimmed (0.40) and shadowless; "grab"/"grabbing" cursors; target lane tinted; 3 px insertion bar at the slot (not shown over Done); count badge on the ghost for multi-card drags; Escape cancels; one exit function for every end path | 857–1031; move 558–583; hit-test 440–454, 532–552; marker 471–508; stop 606–638 | `GtkGestureDrag`, `gtk_widget_paintable_new` snapshot in a `GtkPicture` on a `GtkFixed` overlay | **H** — mouse-tracking loop, `bitmapImageRepForCachingDisplayInRect:` ghost on an overlay view, manual hit-testing |
| K6 | Drop = two independent halves: status change for every moved card (DB, stamps `updated_at`) and order (config key only).  Same-place drop is a no-op.  Selection moves with the cards.  Refresh is deferred to the next loop turn (the handler is running inside the card being destroyed) | 770–844 | | **M** |
| K7 | Card order per view in `kanban_order_<view>` (New + In Progress lanes only); Done lane always sorts by `completed_at` desc; order saving refused during a search | 643–761, 1122–1133 | | carry logic |
| K8 | Done lane capped at 10 with a "Show all N completed" / "Show recent only" link; expansion resets when the sidebar selection changes | 1119–1256; `sidebar.c:868` | markup `<a href>` in a label | **E** (button) |
| K9 | Empty lane shows italic dimmed "Drop a task here"; lane remains a drop target at full height | 1353–1360 | | **E** |
| K10 | Diffing refresh: if the same card ids are in the same lanes with the same counts, only changed text is rewritten; otherwise full rebuild with scroll kept | 1141–1215, 1264–1366 | | **M** (optional optimisation, but it is what keeps a drag source alive across refreshes) |
| K11 | Shadow on/off applied live from Settings | 244–262, 1412–1420 | CSS class toggle | **E** |
| K12 | While the board is up the list store is emptied and "selected task ids" is answered by the board | `library_window.c:277–278, 585–607` | | interface rule |

### 3.8 Editor (`editor_window.c`)

| # | Feature | Where | GTK | AppKit |
|---|---|---|---|---|
| E1 | Title entry; Enter = save and close | 1493–1499 | `GtkEntry` | **E** |
| E2 | Row: "Status:" dropdown (New / In Progress / Done, fixed 115 px) … "Due:" date entry (12 chars, `YYYY-MM-DD`) "at" time entry (5 chars `HH:MM`, 24 h) | 1506–1572 | `GtkDropDown`, `GtkEntry` | **E** |
| E3 | **The due entry is its own date picker**: a primary click opens D4; typing is still accepted (`YYYY-MM-DD` or `M/D/YY`); partial/invalid text never clobbers the stored value; a focused entry is never rewritten by a reload | 1093–1101, 92–110, 1133–1141 | capture-phase `GtkGestureClick` | **M** — custom field subclass / `NSDatePicker` popover |
| E4 | Row: Favorite ☐, High Priority ☐ … read-only "Completed <date>" / "Last completed <date>" (dimmed, small) | 1575–1594, 314–328 | `GtkCheckButton` | **E** |
| E5 | Notes: multiline, word wrap, pinned to exactly 8 lines high, scrolls, grows with the window | 1597–1637 | `GtkTextView` | **E** (`NSTextView`; remember `allowsUndo`) |
| E6 | **Write-through save**: 600 ms debounce on text; status / favourite / priority save immediately; close flushes; saves fire the light "tasks" event | 337–438, 1232–1241 | `g_timeout_add` | **E** logic, must be exact |
| E7 | **Advanced disclosure** ("Advanced ▾/▴" underlined link, bottom-left): shows Subtasks, Attachments, Recurrence in that order; folded by default, opened before first presentation if the task already has any; expanding grows the window by exactly the block's measured height and collapsing gives the same pixels back | 1285–1356, 1908–1923, 1953–1954 | visibility + `gtk_widget_measure` + `gtk_window_set_default_size` | **H** — Auto Layout fitting size + `setFrame:` keeping the **top-left** fixed (AppKit anchors bottom-left; the GTK build currently grows upward on macOS and documents that as a known defect, `BUILD.md`) |
| E8 | **Subtasks** (top-level tasks only): list of rows `[checkbox][always-editable flat entry]`; Add (creates "New subtask", focuses it, scrolls to bottom), Remove, ▲ ▼ reorder; entry saves on Enter and on focus loss; ticking a subtask promotes a New parent to In Progress and **resyncs the parent's status dropdown** so the next debounced save does not undo it | 584–832, 1150–1164, 1651–1692 | `GtkListBox` | **M** — view-based table with editable cells |
| E9 | A subtask's editor shows "This is a subtask of “…” — subtasks cannot have their own subtasks." instead of the Subtasks section | 1693–1705 | | **E** |
| E10 | **Attachments**: list of file basenames (middle-ellipsized); Add… (file picker), Remove (record only), Open (default app); double-click opens | 844–1002, 1707–1729 | `GtkListBox`, `GtkUriLauncher` | **E** (`NSWorkspace openURL:`) |
| E11 | **Recurrence**: heading + explanatory paragraph; master checkbox "Repeat this task"; body *hidden, never greyed* when off; rows "Starting [date] at [HH:MM]", "Repeat every [1–999] [minutes…years]", "Reset to New [0–999] [minutes/hours/days/weeks] beforehand"; leading labels share a width; two-line dimmed summary from `task_recur_describe`; lead follows its default while untouched; toggling the body re-measures and resizes the window | UI 1734–1904; logic 188–272, 448–568 | `GtkSpinButton`, `GtkDropDown`, `GtkSizeGroup` | **M** UI, logic carries |
| E12 | Foot row: Advanced link … **Save** (flush + close) and, for a just-created task only, **Cancel** (drops the pending save, closes, then tombstones the task) | 1908–1936, 1250–1280 | | **E** |
| E13 | `task_editor_refresh_all`: reload every open editor from the DB, skipping any with a pending save, rewriting text only when it differs; an editor whose task vanished closes itself | 1172–1227, 1995–2006 | | **M** — the "don't disturb the cursor" rules |
| E14 | Many long tooltips (due time, recurrence controls, Advanced) | 1538, 1563–1567, 1756–1868, 1919 | `gtk_widget_set_tooltip_text` | **E** |

### 3.9 Settings (`settings_window.c`)

| # | Feature | Where |
|---|---|---|
| P1 | **Appearance**: "Show task titles in bold" (`bold_task_titles`), "Show drop shadows on Kanban cards" (`kanban_shadow`, applied live via `task_library_apply_kanban_shadow`), "Include all past-due tasks in the Due Today view" (`due_today_show_overdue`) — all write-through, no OK/Apply | 36–75, 768–798 |
| P2 | **Database plate** (bordered, base-colour panel, label/value grid, selectable values): Health (LED emoji 🟢/🔴/⚪ + "Healthy — checked 13:24 today" / "Problems found" / "Check did not complete" / "Not checked", detail in tooltip), Current database path, Data ("N tasks in M lists"), Size on disk, SHA-256 (shown `8…4`, full digest in tooltip, **click copies to clipboard**) | 236–359, 820–912 |
| P3 | **Update** button: runs the two PRAGMA checks, re-reads all five lines, posts a status message | 375–390 |
| P4 | **Backups**: "Back up the database automatically"; "Every [0–10080 step 15] minutes, keeping [1–500] files"; "Backing up to <resolved folder>" with a ⚠ second line when the folder is inside the database's directory (containment test, `dir_shares_fate` 120–136); "Change Folder…", "Back Up Now"; controls greyed while off | 148–179, 408–490, 914–1000 |
| P5 | Scroller sizing: opens at natural height up to 600 px, then scrolls | 717–735, 1019–1032 |

AppKit: all **E**.  There is deliberately no control for the database location.

### 3.10 Cross-cutting

| Area | Finding | Where |
|---|---|---|
| **Keyboard shortcuts** | No accelerators are registered anywhere (no `set_accels_for_action`, no `GtkShortcut`).  Keys handled in code: Escape cancels a custom dialog (`app.c:397–408`), cancels a card drag (`kanban.c:587–596`), clears the search box (`library_window.c:425–430`); Enter in the title entry saves and closes (`editor_window.c:1497`); Enter in search applies immediately (`library_window.c:1884`); Enter in a subtask entry saves it (`editor_window.c:712`).  Dialog buttons carry Alt-mnemonics (`_Cancel`, `_OK`, `_Open`, `_Select`, `_No`, `_Yes`).  Everything else is toolkit default (list arrow keys, Enter = activate row, text editing keys and text undo). | — |
| **Drag and drop** | Three in-window, hand-rolled gesture drags: sidebar list reorder, manual-sort row handle, Kanban card.  No `GtkDragSource`/`GtkDropTarget`, no file drops, nothing crosses the window. | `sidebar.c:176–401`, `task_list.c:575–737`, `kanban.c:857–1031` |
| **Custom drawing** | None.  The only "rendered" thing is the card ghost, a widget snapshot. | `kanban.c:928–935` |
| **CSS / theming** | No stylesheet file.  Nine inline CSS strings installed at display level via `task_app_css_install` (`app.c:565–575`): `main.c:294` (dead rule — the class `task-status-label` is never applied); `library_window.c:1389–1395` (float bar), `1917` (6 px paned handle); `sidebar.c:96–122`; `task_list.c:264–300` (own provider); `kanban.c:83–146`; `editor_window.c:1407–1437` and `1917–1918` (**re-installed on every editor open, and it is a global `button.flat` rule**); `settings_window.c:547–568`.  Light/dark is handled by named theme colours (`@theme_bg_color`, `@theme_fg_color`, `@theme_base_color`, `@theme_selected_bg_color`, `@window_bg_color`, `@accent_color`, `@borders`).  Hard-coded colours: stripe `#e8f2fb`, drag row `#fde68a`, sidebar selection rgb(86,131,224), sidebar text rgb(65,65,65), due tints, link `#1c71d8`, badge rgba(47,93,192,.95), shadow rgba(0,0,0,.38). | **M** in AppKit: every rule becomes code; hard-coded light-theme colours (stripe, sidebar text) need dark-mode decisions the GTK build never made |
| **Icons** | PNGs in `icons/`, found next to the binary and added to the icon-theme search path (`main.c:283–289`, `app.c:591–661`).  Used by name: `add`, `remove`, `left-and-right`, `hidden`, `visible`, `manual`, `automatic`, `menu`, `board`, `document` (About logo and bundle icon), `tasks` (window icon).  Unreferenced: `add2`, `arrow`, `composition`, `google-symbol`, `to-do-list`, `trash-can`, all of `icons/Unused/`.  `theme/hicolor` SVG arrows exist only to give GTK crisp tree expanders.  Emoji are used as glyph icons throughout (⭐️ 🔮 ☀️ 🚨 📎 ☑ ☐ ↳ ⠿ 🟢 🔴 ⚪ ⚠ ▲ ▼ ▾ ▴ ✓).  No GResource. | **E** — copy the 10 PNGs into `Contents/Resources` |
| **Tooltips** | ~24 call sites; a custom helper (`app.c:663–761`) exists only to work around GTK-on-macOS tooltip bugs. | **E** — `toolTip`; the helper disappears |
| **Animations** | Only the status-message cross-fade (1 s) plus toolkit defaults.  The old row fade-out is gone. | **E** |
| **Clipboard** | One use: copy the SHA-256 digest. | `settings_window.c:400` |
| **Notifications, tray/status item, printing, export, app-level undo** | **None exist.** | grep-verified |
| **Scrollbars** | Classic (non-overlay) scrollbars forced via `GTK_OVERLAY_SCROLLING=0` (`main.c:367`). | AppKit follows the system setting |
| **Async-safety convention** | Idle/worker callbacks never capture the window; they re-resolve it via `lib_of(app)` and no-op when it is gone (`library_window.c:55–62, 508–531`). | keep the rule with blocks (weak refs) |

---

## 4. Application plumbing

### 4.1 `main()` and lifecycle (`main.c`)

Order in `main()` (344–433):

1. macOS only: install a GLib log filter for three benign GTK/GDK messages (57–77, 347–359).
2. `task_app_config_init(argv[0])` (363).
3. `setenv GTK_OVERLAY_SCROLLING=0` (367).
4. Allocate `TaskApp`; `editors` hash (int64 → window) (371–374); icons dir.
5. Register subsystems before any thread: `task_core_views_init()`, `task_recur_init(app)`, `task_backup_init(app)` (378–380).
6. If the database file exists at the expected path, open it synchronously; failure → exit 1 (385–393).  If it does not exist, `app->db` stays NULL.
7. macOS only: `GSK_RENDERER=cairo` (402).
8. `gtk_application_new("org.example.tasks", G_APPLICATION_DEFAULT_FLAGS)`; macOS: `register-session` so Dock-Quit/logout go through `app.quit` (405–412).
9. Connect `startup` (264–295: font-hinting setting, icon search paths, default window icon, one CSS rule) and `activate` (302–322).
10. `g_unix_signal_add(SIGTERM, …)` → destroy all windows (329–339, 417).
11. `g_application_run`; then free listeners, close db, return.

`activate`: raise the existing library window; else, if no db, run the async
Welcome chain (hold the application, D7) ; else `startup_finish` (138–162):
build the library window → `task_worker_arm_all(app, db->path)` →
`task_db_health_check` → notice dialog on failure or status message
"DB at … loaded, integrity check passed".

### 4.2 Single instance, IPC, command line

- Single-instance behaviour is whatever `GtkApplication` provides for the app
  id (D-Bus based uniqueness).  A second activation presents the existing
  window (`main.c:307–310`).  There is **no** socket, no IPC protocol and no
  CLI in this app (the `notes.sock` text in CLAUDE.md is about the Notes app).
- **No command-line options are defined.**  `argc/argv` go straight to
  `g_application_run`; `argv[0]` is used only to locate resources.

### 4.3 Config (`app.c:1017–1137`)

- File format: GKeyFile ini, single group `[tasks]`, string values.
- Resolution, once: `<data dir>/tasks/tasks.ini` if it exists; else
  `tasks.ini` beside the binary if it exists (portable mode); else created at
  the first location.  If the chosen file cannot be loaded, defaults are read
  from `tasks.ini.defaults` beside the binary (1076–1082).
- Loaded once, kept in memory, **the whole file is rewritten on every changed
  `set`** (1136); comments are lost on the first rewrite.
- Keys in use (grep of every `task_app_config_*` call): `db_dir`,
  `backup_enabled`, `backup_dir`, `backup_interval_min`, `backup_keep`,
  `bold_task_titles`, `show_completed`, `sidebar_visible`, `sidebar_width`,
  `compact_layout`, `compact_w`, `compact_h`, `compact_pre_w`, `compact_pre_h`
  (the four `compact_*` size keys are from the uncommitted change),
  `due_today_show_overdue`, `kanban_view`, `kanban_shadow`,
  `task_list_manual_sort`, `win_w`, `win_h`; dynamic keys
  `col_<done|status|due|completed>_visible` (`task_list.c:329, 368`),
  `manual_order_list_<id>`, `manual_order_group_<id>`, `manual_order_<view id>`,
  `kanban_order_list_<id>`, `kanban_order_group_<id>`, `kanban_order_<view id>`
  with view ids `pinned`, `all`, `today` (`task_list.c:100–145`,
  `task_view.c:82–88`, `kanban.c:642–649`).
- Two more local-state values live **in the database** `sync_state` table:
  `lists_custom_order` and `backup_source_stamp`.

### 4.4 Data paths

- `task_db_default_dir()` = `g_get_user_data_dir()/tasks`, created on demand
  (`db.c:223–229`).  GLib does **not** map this to `~/Library/Application
  Support` on macOS: on this machine the live data is at
  `~/.local/share/tasks/` (`tasks.db` 660 KB, modified today; `tasks.ini`).
  It honours `XDG_DATA_HOME`, which is the entire mechanism behind
  `make run-dev` (`Makefile:126–136`).
- `db_dir` (ini) overrides the directory; the file name is always `tasks.db`
  (`db.c:244–250`).
- Backups: `backup_dir`, else `<data dir>/tasks/backups/` (`backup.c:84–96`).
- Migration backups: `<db>.pre-v<N>.bak` beside the database (`db.c:630–649`).
- Resources are located relative to the executable: `icons/`,
  `tasks.ini.defaults` (`app.c:1030–1040, 590–594`).
- `make install` with the default prefix puts the binary and icons in
  `~/.local/share/tasks/` — **the same directory as the user's data**
  (`Makefile:149–161`; confirmed on disk).

### 4.5 Timers, idles, threads

| Kind | Where | Purpose |
|---|---|---|
| One-shot deadline (≤ 900 s) | `recur.c:575–603` | wake the recurrence pass at the earliest due occurrence |
| Periodic, minutes from config | `task_worker.c:98–118` | backup worker tick |
| 600 ms one-shot | `editor_window.c:392–400` | save debounce |
| 3 s one-shot | `library_window.c:564` | hide the status message |
| Idle | `library_window.c:103` (scroll restore), `1226` (compact resize), `kanban.c:1029, 1234` (deferred refresh), `editor_window.c:782` (scroll subtasks), `app.c:511` (drop popover) | "after this event" work |
| Frame-clock after-paint | `sidebar.c:1406–1420` | sidebar auto-fit |
| Thread | `backup.c:240–376` | one backup pass on its **own** SQLite connection; result posted to the main thread via `g_idle_add` |

Everything else, including the recurrence pass, runs on the main thread
against `app->db`.  SQLite busy timeout is 5 s (`db.c:527`).

### 4.6 Network / sync

**Nothing in this build talks to a network.**  `otool -L tasks` shows GTK,
Pango, HarfBuzz, gdk-pixbuf, cairo, graphene, GIO/GObject/GLib, libintl and
MacPorts sqlite3 — no curl, no libsoup, no JSON library.  The Makefile's
`PKGS := gtk4 sqlite3` (`Makefile:47`) and never references
`client_credentials.mk`.

`client_credentials.mk` and `plugins/gtasks.so` are leftovers of the Google
Tasks plugin deleted in `9b86b50`; per its README it used libcurl plus its own
`http.c`, `json.c` and `oauth.c` (PKCE, loopback listener).  That source exists
only in git history (`git show 9b86b50^:src/plugins/gtasks/…`).  The database
still carries sync scaffolding: tombstones, `updated_at`, the `sync_state`
table and, on migrated files, `gtasks_*` / `notes_*` side tables.

Also gone with the plugins (and therefore **not** part of the parity target,
despite CLAUDE.md and code comments still mentioning them): the Weekly Forecast
panel, the Overdue view, the Notes action-item mirror.

### 4.7 Backup (`backup.c`)

Pass order (240–347): stamp source (dest dir | size : mtime) and compare with
`sync_state.backup_source_stamp` → skip silently if unchanged → pick a unique
`tasks-YYYYMMDD-HHMMSS[-n].db` name → `VACUUM INTO` → verify
(`integrity_check` + `foreign_key_check`), delete on failure → store stamp →
prune oldest `tasks-*.db` beyond `backup_keep` (clamped 1–500).  Off by default.

---

## 5. Build

- **Makefile targets** (`Makefile`): `all` (default; binary + `compile_commands.json`),
  `run`, `run-dev` (sandbox: `XDG_DATA_HOME=$(CURDIR)/dev/data`, seeds a
  zero-byte db to skip the Welcome dialog), `install` (`PREFIX ?= ~/.local`;
  binary + `icons/` + defaults to `$(PREFIX)/share/tasks`, symlink in
  `$(PREFIX)/bin`), `test`, `clean`, `app` (macOS bundle).
- **Flags**: `cc -std=c11 -Wall -Wextra -g -Isrc -DTASK_VERSION='"<VERSION>"'`
  + `pkg-config --cflags gtk4 sqlite3`; link `pkg-config --libs gtk4 sqlite3 -lm`.
  No optimisation flag, no deployment target.  Every object depends on every
  header.
- **Version**: `VERSION` file (`4.2.0`) → `TASK_VERSION` macro → About dialog
  and Info.plist.
- **Resources**: not embedded.  Icons and the defaults ini are files beside the
  executable.
- **`make app`** (`Makefile:203–243`): `dist/Tasks.app` with the binary renamed
  `Tasks`, `icons/` and `tasks.ini.defaults` in `Contents/MacOS`, `.icns` made
  from `icons/document.png` with `sips`/`iconutil`, a minimal Info.plist
  (`org.example.tasks`).  Not self-contained (absolute MacPorts dylib paths), no
  code signing.
- **Tests**: `make test` builds `build/test_tasks` from `tests/test_main.c` +
  `app.o db.o search.o recur.o backup.o task_worker.o` and runs it.  GLib
  `g_test` harness.  Coverage: search parse/match (6), recurrence advance /
  lead defaults / month clamp (9), db fresh schema + create/read + status
  stamping (3).  Not covered: config, backup, row markup, order permutation,
  migrations, `task_recur_seed`/`_pass`, date formatting.
- **Sibling reference** (`../files`): pure Cocoa, `clang -fobjc-arc -O2 -Wall
  -Wextra -Werror=unguarded-availability -mmacosx-version-min=14.0`, frameworks
  only, bundle assembled by the Makefile with `plutil` + `codesign`, tests as
  `tests/*_test.m` linked against the app objects.  No GLib.

---

## 6. External dependencies

| Dependency | Version here | Used for | Stock macOS? |
|---|---|---|---|
| GTK4 | 4.22.4 (MacPorts `+quartz`); docs require ≥ 4.10 | all UI | **No** — MacPorts only |
| GLib / GObject / GIO | 2.88.2 (MacPorts) | core utilities, main loop, GObject rows, actions/menus | **No** — MacPorts only |
| Pango 1.57.1, cairo 1.18.4, HarfBuzz 14.2.1, graphene 1.10.8, gdk-pixbuf 2.44.6, libintl | MacPorts | pulled in by GTK; app code uses Pango markup and a few `graphene_point_t` | **No** |
| SQLite | linked: MacPorts 3.53.4; docs require ≥ 3.35 (DROP COLUMN; VACUUM INTO needs ≥ 3.27) | storage | **Yes** — system SQLite 3.51.0; `sqlite3.h` and `libsqlite3.tbd` are in the SDK |
| libcurl | MacPorts 8.21.0 installed, **not linked** by this build | (deleted gtasks plugin only) | Yes (SDK has `curl/curl.h`, `libcurl.tbd`) if sync ever returns |
| SHA-256 | via GLib `GChecksum` | db fingerprint | Replacement `CommonCrypto` is in the SDK |
| pkg-config (`pkgconf`) | MacPorts | build | not needed for an AppKit build |
| `sips`, `iconutil` | system | `make app` icon | Yes |
| Toolchain | Apple clang 21.0.0, Command Line Tools SDK, macOS 26.6.2, arm64 | | Yes |

An AppKit fork that also strips GLib needs **only** the system SDK
(Cocoa + libsqlite3 + CommonCrypto).

---

## 7. Git state (report only)

- Branch `master`, tracking `origin/master`, **ahead by 1** (`9ce1df8 Add BSD
  3-Clause LICENSE`, unpushed).  Only branch.  One worktree.  No stashes.
- Remote: `origin git@cambio.github.com:IANatCAMBIO/Tasks.git`.
- **Modified, unstaged**: `src/library_window.c` (+138 / −20).  It changes
  Compact Controls behaviour:
  - entering compact now **hides the sidebar**; leaving compact **force-shows
    it and writes `sidebar_visible=1`** (`library_window.c:640–647`);
  - toggling compact saves/restores two window sizes (`compact_w/h`,
    `compact_pre_w/h`) and force-resizes the window (1190–1291);
  - startup and close read/write the compact size (1447–1465, 1795–1813).
  This contradicts CLAUDE.md, the User Guide and `tasks.ini.defaults`, which
  all say compact mode leaves the sidebar alone.  Side effect: after this
  change **nothing reads `sidebar_visible` any more**, so in full mode the
  sidebar is always shown at launch regardless of the saved preference.
  The checked-in `tasks` binary and `build/library_window.o` have the same
  timestamp as the edit (2026-09-28 09:39), so the binary on disk includes it.
- **Untracked**: `client_credentials.mk`, `plugins/`.
- Ignored and present: `build/`, `dist/`, `dev/`, `tasks`, `compile_commands.json`,
  `.cache/`, `.claude/`, `src/plugins/` (only `.DS_Store`).
- History note: master is the merged GTK4 port (`76be626 Merge gtk4 branch`).
  CLAUDE.md still says the repo directory is `tasks-gtk4`; it is `tasks`.

---

## 8. Assessment

### 8.1 Carry / strip / rewrite

**Carry unchanged — only if the fork keeps linking GLib (not GTK):**
`db.c/h`, `search.c/h`, and — after a mechanical header fix (stop including
`app.h`) — `recur.c`, `backup.c`, `task_worker.c`, `task_view.c`,
`core_views.c`, `task_ops.c`.  That keeps a MacPorts `glib2` dependency and a
GLib main loop that would have to be bridged to the Cocoa run loop for the
timers and `g_idle_add` to fire (GLib timers do not run under `NSApplication`
by themselves).  Given `../files` is frameworks-only, this is probably not the
intended shape.

**Carry with GLib stripped** (logic unchanged; this is the realistic plan):

| File | `g_*()` calls | What the job actually is | Size |
|---|---:|---|---|
| `db.c` + `db.h` | 131 | mechanical alloc/string swaps (≈ 75 %); `GPtrArray` return type → agreed array type (touches every caller); `GString` in pragma collectors and reorder SQL; `GHashTable` for attachment counts; `GChecksum` → CommonCrypto; `GError**` → `char **err`; `g_get_user_data_dir` → explicit path policy | medium, low risk |
| `recur.c` | 60 | **`GDateTime` calendar arithmetic** (add minutes…years with month clamp and DST wall-clock preservation, weekday name) and the one-shot timer | medium, **highest logic risk** — tests exist for advance/clamp/lead |
| `app.c` config + dates (1008–1359) | 84 | `GKeyFile` → small ini reader/writer; tz-cached date helpers and 5 format strings | medium |
| `app.c` listeners (27–182) | 9 | `GSList` → array | trivial |
| `backup.c` | 75 | `GThread` + `g_idle_add` → GCD; `GDir`, stat, strings | small–medium |
| `task_rows.c` | 67 | not a strip: **rewrite the output** as a toolkit-neutral row description; keep the notes-preview/UTF-8-boundary rules | medium |
| `search.c` | 28 | Unicode casefold + Unicode whitespace + string builder | small (decide: Foundation or hand-rolled) |
| `task_worker.c` | 9 | timer shim | small |
| `task_view.c`, `task_ops.c`, `core_views.c` | 9 | array type | trivial |
| `tests/test_main.c` | 92 | port 18 cases off `g_test` | small |

Total ≈ **470 call sites**, of which ≈ 190 are `g_free`/`g_strdup`/
`g_strdup_printf`/`g_new0` and can be done by a 30-line compatibility header.
The rest concentrates in five facilities: array, string builder, int-keyed
map, date/time, ini.  A pragmatic middle path is a small `task_base.[ch]`
providing exactly those five, after which `db.c`, `search.c`, `task_view.c`,
`task_ops.c`, `core_views.c` port almost by search-and-replace.

**Rewrite (no code carries, behaviour is the spec):** `main.c`, the GTK part
of `app.c` (183–1007), `list_rows.c/h`, `library_window.c`, `library_priv.h`,
`sidebar.c`, `task_list.c`, `kanban.c`, `editor_window.c`,
`settings_window.c` — ≈ 11,900 lines.  Pure-logic fragments inside them that
should be *lifted into the core* rather than rewritten:

- `lib_row_order_permutation`, `lib_row_order_key`, `lib_row_order_keys_drop`
  (`task_list.c:37–134`);
- the sort comparators (`task_list.c:1210–1267`);
- Kanban order save/apply and drop rules (`kanban.c:655–831`), Done-lane
  ordering (1122–1133);
- the task collection + search filter + status-line text in
  `lib_refresh_tasks` (`library_window.c:263–381`);
- editor parsing helpers and recurrence-lead logic (`editor_window.c:92–272,
  448–472`);
- `dir_shares_fate`, `health_stamp` (`settings_window.c:120–136, 206–225`);
- sidebar reorder index arithmetic (`sidebar.c:337–397`).

### 8.2 Risks and hidden couplings

1. **A live production database is in play.**  `~/.local/share/tasks/tasks.db`
   was modified today.  A fork that keeps `task_db_default_dir()` semantics
   opens the *same file* as the GTK app, at the same schema version.  CLAUDE.md's
   data-safety section exists because a 1,965-task database was destroyed once.
   The fork needs a sandbox override from day one (today that is
   `XDG_DATA_HOME`, which disappears with GLib) and an explicit decision on
   whether it shares the real database.
2. **Shared ini is last-writer-wins.**  Both apps would cache the whole ini in
   memory and rewrite the whole file on every change.  If the fork shares
   `tasks.ini`, running both loses settings; if it does not (e.g.
   NSUserDefaults), per-view orders and column state diverge.  Needs a decision.
3. **The parity target is ambiguous in one place**: the uncommitted compact-mode
   change vs. every document (§7).  Someone has to say which behaviour is the spec.
4. **Docs are stale in several places** — do not hand CLAUDE.md to engineers as
   the spec.  Verified mismatches: Weekly Forecast described but absent; status
   fade described as 20 × 50 ms alpha steps but is a 1 s revealer cross-fade
   (`library_window.c:1979–1986`); window size "tracked via
   `notify::default-width`" but is read at close (`1432–1439`); Kanban ghost
   "a GtkWindow" but is a `GtkPicture` on a `GtkFixed`; recurrence block "two
   rows" but has three (`editor_window.c:1853–1887`); `theme_field_height`
   referenced but deleted; a v7 `done`→`status` migration described but **not
   present** in `db.c` (only v8–v12 blocks exist, `db.c:673–914`) ; User Guide
   says selecting a group "doesn't change the task pane" (it aggregates), that
   search ignores accents (it only casefolds), that About shows database
   statistics (it does not), and mentions gtk-mac-integration (removed);
   Internals.md says schema version 10 (it is 12); `tasks.ini.defaults` describes
   presets and a "Settings → Recurring Tasks" section that do not exist.
5. **`task_rows` markup is shared by two panes.**  If the list and board teams
   each build their own attributed-string renderer they will drift; one shared
   renderer must be agreed before either starts.
6. **Selection is pane-polymorphic.**  Every command acts on
   `selected_task_ids()`, which answers from the board when Kanban is on
   (`library_window.c:585–607`).  The shell, list and board must agree on that
   interface.
7. **Refresh-while-handling hazards.**  The GTK code defers refreshes that
   would destroy the widget whose handler is running (`kanban.c:833–844`),
   re-resolves the window in every async callback, and tears listeners down
   before closing editors (`library_window.c:1475–1497`).  The same ordering
   bugs are reachable in AppKit.
8. **Editor ↔ library write races are handled by convention**: editors skip
   reload while a save is pending; the parent's status dropdown is resynced
   after a subtask tick or the debounced save would revert the promotion
   (`editor_window.c:1150–1164`); Cancel must drop the pending save *before*
   destroying the window and *then* tombstone (`1265–1280`).
9. **Timer semantics.**  The recurrence deadline is capped at 15 min because
   GLib timers are monotonic and do not advance during sleep
   (`recur.c:537–552`).  A wall-clock GCD timer could remove the cap, but the
   pass must stay correct either way.
10. **Thread discipline.**  One SQLite connection per thread; the backup opens
    its own.  Keep that with GCD.
11. **Platform-convention gap.**  The current app has no accelerators, puts
    Quit/Settings/About under File, and names windows "Tasks - …".  A Mac user
    will expect ⌘N, ⌘W, ⌘Q, ⌘,, ⌘F, Delete.  Whether the fork is
    "pixel/behaviour parity" or "Mac-native" changes WP1 and the menu table.
12. **Dark mode.**  Several colours are light-only literals (stripe, sidebar
    text, drag tint).  GTK-on-quartz largely hid this; AppKit will not.
13. **Install layout.**  `make install` currently co-locates the binary with
    user data; an `.app` bundle changes resource lookup (`task_app_exe_dir`)
    and the "portable ini beside the binary" rule.
14. **Dead code that would be carried by accident**: 11 sync-era `task_db_*`
    functions, `task_rows_toggle_done`, `TaskApp.pending_fades`,
    `TaskLibrary.drag_mark_row`, `card_of`, the dead CSS rule in `main.c:294`.
    The owner's standing rule is "no dead code".
15. **Secrets.**  `client_credentials.mk` holds an OAuth client secret; the
    live `tasks.ini` may still hold a `gtasks_refresh_token` from the plugin era
    (not inspected).  Neither should be copied into a fork.

### 8.3 The five hardest pieces

1. **Tall task rows in the list** — attributed-string rendering of the row
   content, variable row heights, selection-aware dimming, in-place checkbox
   update without disturbing selection/scroll, header sorting vs. manual mode,
   stripe colours, right-click-inside-selection semantics (L1–L10).
2. **The Kanban board** — 1,400 lines with no AppKit analogue: custom card and
   lane views, multi-select model, hand-rolled drag with ghost, slot marker,
   lane hit-testing, multi-card badge, Escape cancel, two-half drop, diffing
   refresh (K1–K12).
3. **De-GLib-ing the core without changing behaviour** — above all the date
   arithmetic (month clamp, DST, local-midnight bucketing, formats) and Unicode
   search, plus replacing the main-loop/timer/thread glue, the ini, and the test
   harness — all against a live database with strict safety rules.
4. **The editor window** — content-driven window height with two nested folds
   that must grow/shrink by exact measured amounts while keeping the top-left
   corner fixed; debounced write-through that never clobbers a focused field;
   inline-editable subtask list; the click-to-pick date fields (E3, E6–E8, E11, E13).
5. **The sidebar outline** — mixed row kinds with a non-selectable header and
   selectable aggregating groups, expansion/selection restoration across full
   rebuilds, drag-reorder with cross-group semantics, auto-fit width, emoji
   picker (S1–S10).

Runner-up: the **shell's command/refresh architecture** — one command table
serving menubar, toolbar, floating bar and context menus; five single-writer
"appliers" that title/enable controls; the three-event listener bus.

### 8.4 Proposed work packages

Seven packages, one owner each, minimal file overlap.  WP0 defines headers
first (a day or two of interface work) so WP1–WP6 can start against stubs.

| WP | Scope | New files (suggested) | Replaces | Size |
|---|---|---|---|---|
| **WP0 Core** | Split `app.c` into config / dates / events; strip GLib (or add `task_base`); toolkit-free `TaskApp`; main-loop shim (timer, async-to-main, worker thread) on GCD; toolkit-neutral row model; lift the pure logic listed in §8.1; port tests to a plain C harness; sandbox data-dir override | `db.[ch]`, `recur.[ch]`, `backup.[ch]`, `search.[ch]`, `task_ops`, `task_worker`, `task_view`, `core_views`, `task_config.[ch]`, `task_dates.[ch]`, `task_events.[ch]`, `task_base.[ch]`, `task_row_model.[ch]`, `task_order.[ch]`, `task_loop.[hm]`, `tests/` | core + `task_rows.c` + logic lifted from UI files | L |
| **WP1 Shell** | `main`, app delegate (launch order, first-run chain, quit/terminate flushing, SIGTERM), main menu, command table + appliers, library window frame (toolbar strip, split view, overlay layers, status bar, search field, compact mode, window/divider persistence), refresh orchestration (`lib_refresh_tasks` / `lib_full_refresh`), task context menu, task commands (new, delete, mark, pin, priority, move, clear completed, open database), About, shared UI utilities (alert/confirm/pick/sheet scaffold, icon loader, colours, **the shared row attributed-string renderer**), Makefile, Info.plist, bundle, README/BUILD | `main.m`, `app_delegate.[hm]`, `library_window.[hm]`, `library_priv.h`, `ui_util.[hm]`, `row_text.[hm]`, `Makefile`, `resources/` | `main.c`, `app.c` GTK part, `library_window.c`, `list_rows.c` | L |
| **WP2 Sidebar** | Outline view, model rebuild with expansion/selection restore, list & group dialogs (incl. emoji picker), context menus, drag-reorder, auto-fit, show/hide | `sidebar.[hm]` | `sidebar.c` | M |
| **WP3 Task list** | Table view, columns, cell views, sorting, manual-sort drag + persistence, column-visibility menu, checkbox in-place update, double-click, selection reporting | `task_list.[hm]` | `task_list.c` | M–L |
| **WP4 Kanban** | Lanes, cards, selection, drag engine, drop, order persistence hooks, Done-lane cap, diffing refresh, shadow toggle | `kanban.[hm]` | `kanban.c` | L |
| **WP5 Editor** | Editor window, date/time fields + picker, notes, Advanced fold, subtasks, attachments, recurrence block, save/cancel, `refresh_all` / `close_all` | `editor_window.[hm]` | `editor_window.c` | L |
| **WP6 Settings** | Settings window (Appearance, Database plate, backups), clipboard copy | `settings_window.[hm]` | `settings_window.c` | S |

WP6 is small; its owner can also take the test port or bundle/packaging from
WP0/WP1.  If only four engineers are available: merge WP2+WP6, and WP3+WP4
*cannot* sensibly merge (both large) — merge WP6 into WP5 instead and give
WP1's packaging to WP0.

### 8.5 Interfaces to agree before work starts

1. **Core C API after the GLib decision** — scalar types (`int64_t`, `bool`,
   `char *`), the array type replacing `GPtrArray` and its ownership/free rules,
   the int-keyed map, error reporting for `task_db_open`.  This touches every
   package.
2. **`TaskApp`** — fields (db, listener lists, editor registry as an opaque
   handle, backup flags), and the events API (`listen_changed` / `listen_tasks`
   / `listen_status`, `notify_*`, `status`) with its "safe with zero listeners,
   safe to unsubscribe during fire" contract.
3. **Main-loop services** — `task_timer_once(seconds, fn)`, `task_timer_every`,
   `task_timer_cancel`, `task_main_async(fn)`, `task_thread_run(fn)`; main-thread
   only rules.
4. **Config** — key registry with defaults (the list in §4.3), file location,
   whether shared with the GTK app, and the sandbox override.
5. **Library-private interface** (the successor of `library_priv.h`): the
   shared state (`sel_kind`, `sel_id`, `search`, `manual_sort`, `kanban`), and
   the functions each pane provides and consumes — `lib_refresh_sidebar`,
   `lib_refresh_tasks`, `lib_full_refresh`, `lib_selected_list_id`,
   `lib_sel_view`, `lib_manual_sort_live`, `lib_view_refuse`,
   `task_context_menu_for_selection`.  Pane protocol for list and board:
   `build`, `refresh(tasks, ctx) → shown`, `selected_ids`, `clear_selection`,
   `cancel_drag`, `set_visible`.
6. **Row display contract** — the neutral row description produced by the core
   and the single `row_text` renderer both panes call (fonts, sizes, alpha
   values, glyphs, selected-state colours).
7. **Command table** — names, parameters and owners of every command in §3.3,
   and which applier titles/enables which control.
8. **UI utilities** — block-based signatures for notice / confirm / pick-path /
   sheet dialog; icon loader; colour constants (due tints, stripe, drag tints,
   sidebar, selection) with light/dark values.
9. **Cross-window entry points** — `task_editor_open`, `task_editor_open_new`,
   `task_editor_refresh_all`, `task_editor_close_all`;
   `task_settings_window_open`; `task_library_apply_kanban_shadow`.
10. **House rules** — `.m` in C style with snake_case as in `../files`, ARC,
    deployment target, the "no dead code / one definition" rules, and whether
    parity or Mac conventions wins (shortcuts, menu layout, toolbar type).

---

## Not verified

- **Nothing was built or run.**  All behaviour is from reading source.  In
  particular not confirmed at runtime: that `make test` currently passes;
  whether a second launch on macOS is actually prevented; whether GTK binds
  ⌘Q / ⌘, automatically on macOS; and whether Enter accepts the custom dialogs
  — `task_app_dialog_new` never calls `gtk_window_set_default_widget` (no call
  exists anywhere in `src/`), although `app.h:304–306` says Enter activates the
  accept button and the entries set `activates_default`.
- `db.c` lines 1060–2148 were surveyed by function list, `ORDER BY` clauses and
  GLib-symbol tally, not read line by line.  Lines 1–1060 and `read_task` were
  read in full.
- `GTK4_MIGRATION.md` (509 lines) was read to line 160 plus the Phase 4/5
  checklist; the "Port recipe", "Decisions" and "Session log" sections were not
  read.  Its Phase 4 runtime-check boxes are all unticked even though the
  branch is merged.
- The live `~/.local/share/tasks/tasks.ini` and `tasks.db` were not opened
  (existence, size and mtime only).
- "Stock macOS" availability was checked against the Command Line Tools SDK on
  this machine (macOS 26.6.2) only.
- `../files` was sampled (Makefile, `main.m`, two headers, class list) to
  characterise the target style, not reviewed.
