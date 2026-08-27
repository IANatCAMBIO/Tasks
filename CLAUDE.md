# Tasks — project guide

Task-list app in **plain C + GTK3 + SQLite**, the companion app to
Notes.  Two window types: a Library (lists sidebar + tall task rows)
and one editor window per task.  Two-way Google Tasks sync.  No GNOME
HeaderBars anywhere — plain `GtkWindow` titlebars, formatted
`"Tasks - <thing>"`.

## Target platform: LINUX FIRST

**This is a Linux / Debian / XFCE-first application.  macOS is SECONDARY
support.**  It is developed on a Mac, which makes it easy to get this
backwards — don't.  What that means in practice:

- Choose the PORTABLE mechanism, and judge it by how it behaves on
  X11/GTK.  macOS behavior is a compatibility check, never the reason a
  design is picked.
- Where a quartz quirk has to be worked around, the workaround must not
  degrade the X11 path, and it gets a comment saying which platform it
  is for (`#ifndef GDK_WINDOWING_QUARTZ` around the column-header
  flattening is the model: it compiles OUT everywhere else, and an X11
  build on a Mac keeps the standard theme).
- Several gotchas below are quartz-specific.  They are recorded so nobody
  re-investigates them, NOT because the Mac drives the design.  A fix
  justified only by "AppKit does X" is the wrong shape — restate it in
  terms of what is portable.
- The GTK version, the widget set and the theming assumptions are all
  stock GTK3, so an XFCE desktop with Adwaita is the reference look.

## Naming

The app is **Tasks**.  It has never shipped, so there is no older
spelling anywhere in the wild and NO migration or compatibility code for
one — not for the config, not for the database, not for file names.  If
you find something that reads like an upgrade path, it is dead code.

**"Lists" is a real word in this app** and most occurrences are the DATA
TYPE, not an old name — do not sweep them.  The sidebar has a collapsible
**Lists** section holding the user's task lists; `TaskList`, the `lists` and
`list_groups` TABLES, `task_db_lists*`, `SB_KIND_LIST`,
`manual_order_list_<id>`, `kanban_order_list_<id>` and Google's own
`/users/@me/lists` API paths all mean lists-the-data-type.

The repo DIRECTORY is still `~/salt_development/lists` and the git remote
is still `IANatCAMBIO/Lists.git`.  Both are deliberate: GitHub redirects,
so renaming the repo would only mean chasing every doc link.

Public symbols are prefixed `task_`, types `Task`, macros `TASK_`.  These
were `bt_`/`Bt`/`BT_` — "Blue Tasks", the app's first name — renamed on
2026-08-26.  Three names would have stuttered and were COLLAPSED rather
than mechanically prefixed, so do not "restore" the pattern:

- `BtTask` → **`Task`** (the domain object needs no prefix)
- `BtTaskStatus` → **`TaskStatus`**
- `bt_task_free` → **`task_free`**

`BT_TASKS_API` became the file-local `GTASKS_API` in gtasks.c, since a
file-local macro does not need the app prefix.  Everything else is the
plain substitution, `"task-*"` object-data keys and CSS classes included.
Functions that name their subject still read `task_db_task_get` — the
namespace, the module, then the subject; that is not a stutter to
"fix".

The companion app is **Notes**.  Its source names here carry a `bn_`
prefix — `src/bnotes.[ch]`, `src/bnsync.[ch]`, `task_bnotes_*`,
`task_bnsync_*`, `bn_*`, `SB_KIND_BN_ACTIONS`, the `bn_deleted` table and
`tasks.bn_uid` / `bn_done` / `bn_due`.  Internal only; nothing a user sees
is derived from them.

**Where Notes is**: `~/salt_development/records` (the directory was not
renamed), git remote `orange_notes.git`, GitHub repo `IANatCAMBIO/Records`
— all redirect.  The live CLI binary is **`notes`** (`make` there builds
it; app bundle `dist/Notes.app`, ini `notes.ini`, socket
`~/.cache/notes.sock`).  `task_bnotes_cli_path` looks for `notes` and
nothing else, deliberately: a stale build left beside the current one
answers `action list` with an EMPTY result and exit 0, which reads as "no
action items" rather than as an error.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./tasks  (-Wall -Wextra must stay clean)
make run
```

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `curl`, `pkgconf`,
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**; the Makefile auto-detects it
and defines `HAVE_GTKOSX`).  After toggling a dependency run
`make clean && make`.

Every module goes in the `PKGS` list for ONE pkg-config query — the
optional gtk-mac-integration module must not get its own `--libs` run,
because it depends on GTK and a second run hands the linker every GTK
library twice (`ld: warning: ignoring duplicate libraries`).  That is
why `HAVE_GTKOSX` is detected above the flag definitions; the link must
stay warning-free so a real warning is visible.

Launch for testing: `pkill -f './tasks'; nohup ./tasks
>/tmp/task_launch.log 2>&1 &` then `screencapture -x` for screenshots.
Do NOT drive the GUI with osascript accessibility clicks (rejected by
the user).  A logic test harness lives in the session scratchpad
(`test_bt.c`, links against `build/{json,db,app}.o`) — keep it passing.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config → db → registries → plugins → window; icon-theme path for HiDPI expanders |
| `src/app.[ch]` | Shared `TaskApp` context; ini config; dialogs; toolbar style system (icons/both/text + right-click menu); HiDPI icon loader; CSS helper; date helpers |
| `src/backup.[ch]` | OPTIONAL rotating db backups: own worker + connection, VACUUM INTO + verify, bounded rotation; off by default |
| `src/db.[ch]` | SQLite schema (user_version 9) + CRUD; `TaskStatus` tri-state; tombstones + `updated_at` for sync; `step_done`/`exec_txn` error discipline |
| `src/library_window.[ch]` | Sidebar (virtual lists + collapsible Lists section with list groups), tall task rows, toolbar, Kanban board, multi-select context menu, status bar |
| `src/editor_window.[ch]` | Per-task editor (debounced write-through saves); Status dropdown; plugin-contributed sections |
| `src/settings_window.[ch]` | Singleton settings: appearance, database, Plugins list; contributed sections |
| `src/plugin.[h]` / `src/plugin_loader.[ch]` | The plugin ABI and the loader: the `TaskHostApi` table, `dlopen`, enable keys |
| `src/task_view.[ch]`, `src/task_ops.[ch]`, `src/task_worker.[ch]`, `src/task_rows.[ch]`, `src/task_ui.[ch]` | The registries a plugin contributes through: sidebar views, core ops + hooks, the one scheduler, row rendering + decorations, window chrome |
| `src/core_views.c` | The app's OWN sidebar views (Favorites, All Tasks, Due Today) — registered through the same registry a plugin uses |

### Plugins (`src/plugins/<id>/` or `src/plugins/<id>.c` → `plugins/<id>.so`)

| Plugin | Purpose |
|---|---|
| `gtasks/` | Google Tasks: two-way sync engine, OAuth (PKCE + loopback), libcurl HTTP wrapper, minimal JSON parser.  Owns `gtasks_list` / `gtasks_task`.  Brings its OWN libcurl (`deps.mk`) |
| `notes/` | Notes action-item mirror: worker-thread pass, bulk write-back, uid identity, `notes.c` + the `bnotes.c` CLI wrapper.  Owns `notes_task` / `notes_deleted` |
| `forecast.c` | Weekly Forecast panel |
| `overdue.c` | Overdue sidebar view — the small worked example |
| `icons/` | Curated toolbar images directly in icons/ (icon names are extension-less basenames — the loader tries `.png` then `.svg`, case-exact for Linux; spares live in `icons/Unused/`) |
| `icons/theme/hicolor/` | Bundled SVG `pan-*-symbolic` arrows → crisp HiDPI tree expanders (needs librsvg loader) |

## Conventions

- `task_` prefix for public symbols, `Bt` for types; every function gets a
  banner comment.  **Headers carry the full public contract** (purpose,
  params, returns, ownership, failure behavior); `.c` banners say "see
  x.h" plus the how.  Non-obvious variables get column-aligned trailing
  comments; ~78-col lines.  UTF-8 escapes (`\xe2\x80\xa6`) for …/—/✓ in
  source strings.
- Config: `tasks.ini` NEXT TO THE BINARY (portable mode), fallback
  `~/.config/tasks/` when unwritable; seeded from
  `tasks.ini.defaults`; loaded ONCE, written through on change,
  never re-read.  Everything except the OAuth client keys and the
  window geometry is editable in File → Settings….
  The ini GROUP NAME is `[tasks]` and it is part of the file format —
  the app reads only that group.  There are no config migrations: this
  build has never shipped, so no other spelling exists in the wild.
  Every `<db>.pre-v<N>.bak` a future migration leaves holds the same
  refresh token as the live file, so `*.bak` is gitignored.
- **Error discipline**: every prepared WRITE goes through `step_done()`
  (logs sqlite's message on prepare/step failure — silent write loss is
  the unacceptable outcome); multi-statement writes go through
  `exec_txn()` (BEGIN IMMEDIATE + ROLLBACK on failure — a bare
  `BEGIN;…;COMMIT` via sqlite3_exec wedges the connection in an open
  transaction on SQLITE_BUSY).  Create failures (id 0) must surface a
  status-bar message at the call site.  The same rule binds READS whose
  whole purpose is to report health: `startup_integrity_check` checks
  both `sqlite3_exec` return codes, because a PRAGMA that never ran
  collects no rows and is otherwise indistinguishable from a clean
  result — "checked, all good" when nothing was checked is the one
  outcome a health check must never produce, so a failed exec reports
  sqlite's own message and the dialog says "did not complete" rather
  than "found issues".
- Notify hooks on TaskApp: `notify_changed` = FULL refresh (sidebar +
  tasks + reload all editors) for structural changes; `notify_tasks` =
  task pane only — editor saves and subtask/attachment edits use this
  (the full path would re-run the Notes CLI per autosave).
  `task_app_status()` for events.  Teardown: NULL the hooks BEFORE
  `task_editor_close_all` (a closing editor's flush otherwise cascades
  refreshes into destroyed windows).
- Async callback lifetime: never capture the TaskLibrary pointer in a
  worker/idle callback — re-resolve via `lib_of(app)` and no-op when
  NULL (the window may close mid-flight).  The settings window guards
  the same way (`settings != sw`).

## Task status (the tri-state that replaced `done`)

A task's completion is ONE field, `tasks.status` (`TaskStatus`: 0 New,
1 In Progress, 2 Done — the values are the on-disk encoding, do not
renumber).  Schema v7 added it, backfilled `done = 1` → Done, and
**DROPPED `tasks.done`**; the drop is conditional on the backfill's
`sqlite3_exec` having returned OK, because dropping the source column
after a copy that never ran would throw every completion away.  An
sqlite too old for DROP COLUMN (< 3.35) just leaves `done` behind
unread, which is harmless — its `NOT NULL DEFAULT 0` keeps INSERTs
working.  There is no `t->done` any more: every "is it complete?" test
is `t->status == TASK_STATUS_DONE`.

Google Tasks and Notes are both BINARY, so the third state is local by
construction.  `task_status_apply_done(cur, done)` is the single rule
every done-only source folds through — the ✓ column, the context menu's
Mark Complete/Incomplete, the subtask checkboxes, a Google pull, a Notes
listing:

- `done` → **Done**;
- `!done` → **In Progress** if it WAS Done (a ticked task has plainly
  been worked on), else `cur` **unchanged** — so a New task survives a
  round trip through a done-only system instead of being promoted.

The SQL paths that need the row's OLD status (`task_db_task_apply_notes`)
spell the same rule as a CASE rather than reading it back in a second
statement.  **New is reachable only from the editor's dropdown.**

**Every status change stamps `updated_at`** — `task_db_task_set_status`
and `task_db_task_update` alike, New ↔ In Progress included.  Status is
the successor of a SYNCED field, NOT a local flag like
`pinned`/`priority`, so it does not get their deliberate missing bump: a
status move that stamps nothing is invisible to every consumer of
`updated_at`, leaving the row reading as untouched since the last sync
with no record that anything happened.  The known cost, accepted
deliberately (2026-08-25): a New ↔ In Progress move dirties a row whose
REMOTE content is unchanged, and on the incremental-listing path — where
an unchanged remote task is simply absent — `sync_tasks` answers a dirty
row with an etag-guarded PATCH carrying a body identical to what is
already there.  On a FULL listing nothing is sent, because `differs`
compares done-ness and finds none.  Don't "optimize" this back into a
conditional bump.

`completed_at` is stamped on ENTERING Done and cleared on leaving; an
already-Done task keeps its first stamp.

**A completed SUBTASK starts its parent**: `parent_started()` in db.c
moves the parent New → In Progress, and every write path that can
complete a task folds through it (`task_db_task_set_status`,
`task_db_task_update`, `task_db_task_apply_remote`,
`task_db_task_apply_notes`), so the editor checkbox, the ✓ column, a
Kanban drag, a Google pull and a Notes listing all get it without five
copies of the rule.  It is ONE guarded UPDATE
(`WHERE id = (SELECT parent_id …) AND status = New`), which is why no
branch is needed: a top-level task's `parent_id` is NULL and matches no
row.  Only New → In Progress, so it never touches `completed_at` and
cannot cascade — the rule fires on Done, and In Progress is not Done, so
a promoted parent cannot promote a grandparent.  **A DONE parent is left
Done** (one more finished child is progress, not regress) and unticking a
subtask drags the parent nowhere.  Not wrapped in a transaction with the
child's own write: a crash between the two leaves the parent New, which
is exactly the old behavior.
The parent's open EDITOR must be resynced (`editor_status_resync`) when
its own subtask list ticks something — `editor_save_now` reads the status
combo and writes it back, so a combo left reading New silently undoes the
promotion on the next debounced save.  That was measured, not assumed:
with the resync removed the parent comes back as New ~600 ms later.

## GUI rules (visual parity with Notes)

- Toolbar: `GTK_ICON_SIZE_SMALL_TOOLBAR` metrics; buttons via
  `task_app_tool_item_new` (local PNG at 24 px logical, Pango-markup glyph
  fallback); registered with `task_app_register_toolbar` so the
  icons/both/text style applies live (Settings combo + right-click
  radio menu).  Layout (all left-packed): the Sidebar toggle, a drawn
  divider, Sync, the completed-visibility toggle, the Manual Sort toggle,
  the pane toggle, a second divider, then New Task and Delete Task.
  The pane toggle sits WITH the sort toggle rather than with the task
  pair: both change how the tasks are PRESENTED instead of acting on a
  task, and the sort toggle is the control it pairs with (the board is
  always drag-sorted, which is why that one greys out while this one is
  on).  It is the toolbar twin of View → Kanban View / List View:
  same `on_toggle_kanban`, and `task_pane_mode_apply` gives it its icon,
  label and tooltip so both controls move together.  Its ICON names the
  action like every other toggle here, and **BOTH faces come from
  menu.png** (the bulleted list): upright it offers the LIST, turned a
  quarter turn CLOCKWISE its bullets sit atop three vertical bars and it
  offers the BOARD.  One image means the two faces cannot drift apart, and
  the turn goes through `task_app_icon_image_rotated` — on the PIXBUF, in a
  whole quarter turn, so a square icon comes back the same size and
  pixel-exact rather than resampled.  `kanban.png` was the board face
  before that and is now unreferenced.
  The Manual Sort toggle follows the same rule with a gearbox pair:
  **manual.png** (a gearstick) while sorting is AUTOMATIC — the column
  headers doing it — since the click on offer is "let me drag them", and
  **automatic.png** (a gear selector) while manual sorting is in force.  It wore menu.png until the
  pane button existed, and two buttons in one toolbar wearing the same
  picture read as one control.  `interactive.png` and `slide.png` are the
  images this button used to carry and are now unreferenced (as is
  kanban.png, see below).
  Every image the loader builds carries its icon name as `"task-icon-name"`
  object data and its turn as `"task-icon-rotation"` — the images are
  surface-backed, so `gtk_image_get_pixbuf` answers NULL and there is
  otherwise no way to ask which picture a state-swapping button is
  showing.  The rotation has to be part of that answer now that one file
  dresses both faces of the pane button.
  The completed-visibility toggle (`show_completed`, default 1) shows
  hidden.png while completed tasks are visible and visible.png while
  hidden (the icon names the ACTION), swapped live via
  `hide_done_icon_refresh`; the filter applies to every task-pane view.
  The Manual Sort toggle
  (`task_list_manual_sort`, default 0) enables drag-reorder of the task
  pane: a ⠿ drag-handle column (26 px wide, `cdrag`) appears; order is
  persisted per view in config keys `manual_order_list_<id>` (built by
  `list_order_key`, the single source of that format),
  `manual_order_all`, `manual_order_pinned`, `manual_order_today`,
  `manual_order_bn_actions`.  The ⠿ glyph and its dimming live on the
  `cdrag` RENDERER, not in `drag_handle_func` — a data func runs per
  DRAW, and the glyph is the same on every row; the func does the row
  stripe only.  `task_view_apply_manual_order` must be called from
  EVERY view's populate path (the Action Items view is now an ordinary
  branch of `refresh_tasks`, so it reaches the shared call; it used to
  return early and silently threw its drags away).  Saved orders are
  task ids only — the old "NOTEID:ORD" token form is gone, and a
  pre-mirror order still holding those tokens parses them to 0 and
  skips them.  `on_delete_list` removes the deleted list's order key — nothing else
  would, and the ini otherwise grows one dead entry per deleted list.
  `lw->manual_sort` caches the config flag (read per motion event and
  per refresh); `task_manual_sort_apply` is the only writer, and
  `task_library_window_new` seeds it before building the toolbar and View
  menu, which read the cache.  A drag-lock (`drag_lock_ref`) prevents
  rapid row flicker at boundaries; the "ns-resize" cursor is made once
  and kept on `lw->drag_cursor` (the motion path must not allocate).
  Far right (after an expanding blank
  separator): the About button — document.png logo in every style
  except text-only, which swaps in an "About" label
  (`about_button_fit_style` on "style-changed"); it opens the
  Notes-style about dialog (`on_menu_about`: HiDPI logo, compile
  stamp, `task_db_totals` vitals), shared with File → About Tasks.
  document.png is also the .app bundle icon (Makefile `app` target).
- View menu, top to bottom: the **completed-visibility** item, the **sort
  toggle**, divider, the **sidebar** item, the **controls** item, the
  **pane** item.  The divider separates what the task PANE shows from what
  the WINDOW looks like.
  **There are NO check items in this menu.**  Every one of the five is an
  ACTION item whose LABEL is what a click DOES — the same idiom as the
  completed-visibility toolbar button, whose icon names the action it
  offers.  The pairs live in `*_LABEL_TO_*` macros, read as
  TO_&lt;destination&gt;:
  `SORT_LABEL_TO_MANUAL`/`_AUTO` ("Manual Sorting" while sorting is
  automatic, i.e. by column header; "Automatic Sorting" while dragging
  is),
  `DONE_LABEL_TO_HIDE`/`_SHOW` ("Hide Completed" while they are visible),
  `SIDEBAR_LABEL_TO_HIDE`/`_SHOW`, `CTRL_LABEL_TO_COMPACT`/`_FULL`
  ("Full Controls" while compact is on), and
  `PANE_LABEL_TO_KANBAN`/`_LIST` ("List View" while the board is up).
  The compact pair names the **CONTROLS**, not the layout: what the
  setting actually swaps is the toolbar for the floating New/Delete pair,
  and "Full Layout" would promise something about the window it does not
  change (the sidebar follows its own item in both modes).  **"List View" is singular on
  purpose**: "Lists" in this app is the sidebar's data type, so "Lists
  View" would read as "show me the lists" rather than "put the tasks back
  in a list".
  Three consequences bind all five:
  every handler FLIPS the persisted flag or the cache rather than reading
  the widget (the widget no longer carries the current state); every
  re-labeller sets the label with NO handler blocking — `set_label` cannot
  emit "activate", where `set_active` on a check item would have; and the
  label is written by the SINGLE APPLIER for that state, never by the
  handler — `hide_done_icon_refresh`, `manual_sort_icon_refresh`,
  `sidebar_menu_sync`, `compact_layout_apply` and `task_pane_mode_apply`
  respectively, so a flag changed by any other route (the toolbar twin, a
  config load) still reaches the menu.  That is also why the Completed,
  Sorting and Sidebar items are wired STRAIGHT to their toolbar twins'
  handlers (`on_toggle_done_visible`, `on_toggle_manual_sort`,
  `on_toggle_sidebar`) instead of keeping a menu copy of each: two copies
  of "what does this toggle do" is how the two controls drift.  The
  controls and pane items have no toolbar twin, so they keep their own
  handlers — which still only flip the flag and call the applier.
  It is GREYED OUT while Kanban View is on (with the toolbar twin), from
  `task_pane_mode_apply` — which also sets the pane item's own label: the board is always drag-sorted by its own
  per-lane `kanban_order_*`, and the list view the setting governs is
  unreachable in that mode, so the control would silently do nothing.
  Keyed on `lw->kanban`, NOT on "the board is showing" — with Kanban on
  and the forecast selected the list is still unreachable, and flickering
  sensitivity as the sidebar selection moves reads worse than a steady
  "unavailable while Kanban is on".
  The sidebar item is the menu twin of the toolbar Sidebar button —
  both route through `sidebar_set_visible` (write-through
  `sidebar_visible` + `sidebar_menu_sync`, which re-labels from the pane's
  LIVE visibility).
  **Compact Layout** (`compact_layout`, default 0) hides the whole
  toolbar and its rule, and shows `float_bar` instead:
  a two-button pill (New Task + Delete Task, icons only, never
  registered with `task_app_register_toolbar`) added as a `GtkOverlay`
  child over the paned with halign/valign END and 20 px end/bottom
  margins.  Its plate is themed through `themed_bg_css_apply`
  (`float_bar_css`), NOT hardcoded — the light-theme grays it shipped
  with put a white slab over a dark theme's rows; the border is a
  `shade()` of the plate, lightened instead of darkened when the plate
  is dark.  The overlay wraps the PANED, not the whole vbox, so the
  float never covers the status bar's event messages.
  `compact_layout_apply` is the single applier (also called after the
  construction-time `show_all`, where it replaces the old
  `sidebar_visible` hide); it uses `gtk_widget_show`, never `show_all`,
  on the toolbar so a hidden Sync button stays hidden.  Compact does not
  affect the SIDEBAR at all any more (2026-08-06): the pane simply
  follows `sidebar_visible` in both modes, so entering compact no longer
  makes an open sidebar vanish.  It used to force-hide it, which read as
  compact silently overriding the toggle, with the Show Sidebar override
  as the only way back.
- Thin `gtk_separator_new` rules under the toolbar and above the status
  bar.  Status bar: margins 8/8/3/3 (NOT border_width) and
  `label { font-size: 85%; }` on both labels — measured pixel-identical
  to Notes.  Event messages posted via `task_app_status()` hold for
  3 s then fade out over 1 s (20 × 50 ms alpha steps via Pango markup);
  a new message resets the timer.
- Task list: alternating white/`ROW_TINT` (#e8f2fb) stripes via a
  cell-background data func on EVERY column's renderer (the Due column's
  func does stripe + urgency tint in one, since a renderer gets one data
  func).  Dimmed markup uses Pango `alpha`, NEVER a fixed gray —
  hardcoded grays are unreadable on the blue selection.  Due tint:
  overdue #c01c28, today #d19a00, ahead #26a269, computed at draw time.
  The **Status** column (New / In Progress / Done) sits between Task and
  Due Date, sorted by `TL_STATUS` (the enum, so the order is the
  workflow's and not the alphabet's) while its text comes from
  `TL_STATUS_TEXT`.  It defaults to **HIDDEN** (`col_status_visible`,
  default 0) — the ✓ column is the same field seen as a tick.  That ✓
  column is kept as a CONVENIENCE VIEW, never a second field: `TL_DONE`
  is `status == TASK_STATUS_DONE`, and a click writes Done (ticking) or In
  Progress (unticking) back through the status rule above.
  Right-clicking any column header shows a hide/show menu for the Done,
  Status, Due Date and Completion Date columns; visibility persists in
  `col_done_visible` / `col_status_visible` / `col_due_visible` /
  `col_completed_visible`.  On **macOS only** (`#ifndef GDK_WINDOWING_QUARTZ`
  compiles it out — the quartz backend's button drawing is the reason to
  restyle, so an X11 build on a Mac keeps its theme, and Linux keeps the
  standard GTK theme untouched) column headers are flattened to the
  status bar's color by `header_button_flatten`: headers are real
  GtkButtons and ship the theme's button gradient, which reads lighter
  than the rest of the chrome.  It resolves `@theme_bg_color` (rgb 246,245,244 in Adwaita —
  the same color the window paints and the status bar therefore shows)
  instead of hardcoding a gray, and bails when the theme doesn't name it.
  The provider must go on each header BUTTON, not the tree view — a
  per-widget provider styles only that widget, and the header buttons are
  separate widgets (get them via `gtk_tree_view_column_get_button`, as
  the header right-click wiring already does).  `:hover` / `:active` keep
  `shade()`s of the same color so a sortable header still responds.
  Both this and the compact float bar go through
  `themed_bg_css_apply(widget, builder)`: it keeps ONE provider per
  widget as object data and RELOADS it on "style-updated" (a macOS
  light/dark switch or a GTK theme swap), because resolving the color
  once at construction leaves the stale value behind, and
  `task_app_widget_add_css` would stack a fresh provider per change.  It
  stores the last color it wrote and returns early when unchanged —
  that guard is what stops the recursion, since our own reload re-emits
  "style-updated".
- Task-cell notes preview: gate it on the first line that actually HAS
  content (`line_is_blank`, Unicode-aware), NOT on `*notes != '\0'` —
  a note holding one space previewed as an empty line, which reads as
  nothing while making that row a whole line taller than its neighbors
  (a real bug report).  The previewed line is `g_strstrip`ed, and no
  line is emitted at all when nothing survives that.  The 120-char cap
  must land on a UTF-8 CHARACTER boundary (walk back with
  `g_utf8_find_prev_char`): the cap counts BYTES, a whole task cell is
  ONE Pango markup string, and a partial sequence anywhere in it makes
  `pango_parse_markup` reject the lot — the row then draws completely
  blank, title and all, not merely without its preview.  For the same
  reason every DB- or CLI-sourced string entering that markup is
  escaped through `markup_escape_db` (`g_utf8_make_valid` then
  `g_markup_escape_text`): `g_markup_escape_text` does NOT validate, so
  a bad byte from a sync payload or a hand-edited database would reach
  Pango and blank the row.  The status bar is the opposite case — a
  plain-text label (`set_text`), so text bound for `task_app_status` must
  NOT be markup-escaped or the user reads a literal "&amp;"; the fade
  animation escapes what it reads back off the label itself.
- Sidebar: gray backdrop CSS (rgb 230,230,230 / text 65,65,65 /
  selection rgb 86,131,224 white); meta rows bold (Favorites ⭐️, All
  Tasks 🔮, Due Today ☀️, Weekly Forecast 🌤️).  Due Today optionally
  includes all past-due tasks via `due_today_show_overdue` (Settings →
  Appearance; default off).  Weekly Forecast is its OWN
  panel, not rows in the task store: seven full-width day sections
  (Sunday–Saturday) stacked vertically, 6 px apart, each a heading
  label + framed (NOT individually scrolled) two-column (done ✓ +
  markup) tree view with its own store at natural full-content
  height, selection mode NONE (seven views would each keep their own
  selection; double-click activation works without one) — the whole
  week scrolls together in one outer scroller
  (position restored via scroll_keep_queue_win); refresh_tasks swaps
  task_scroll ↔ forecast_box visibility (re-applied after the
  construction-time show_all, which shows both) and clears the hidden
  regular store (a stale selection there would feed the toolbar's
  Delete Task).  Day headings are set per refresh (today: blue ● + "— Today"); the
  day checkbox resolves its store via "task-model" object data on the
  renderer.  Empty days show an inert dimmed "No tasks due" row
  (id 0 — checkbox hidden by forecast_toggle_bg_func, activation
  ignored).
  In-list day-section headers and side-by-side day columns were both
  tried and rejected FOR THE FORECAST (2026-07-16) — don't reintroduce
  them there.  That rejection is about the forecast's seven dated days,
  not about columns generally: the Kanban board below is deliberately
  columnar, because three named statuses are what a board is.
  The row exists only while `weekly_forecast`=1 (Settings →
  Appearance; default on).  The Favorites row exists ONLY while
  something is pinned (`task_db_has_pinned` — mirrored Notes items carry
  the ordinary `pinned` flag like anything else); editor pin
  flips arrive via
  notify_tasks, so `notify_tasks_hook` rebuilds the sidebar on the
  0 ↔ nonzero transition (tracked in `pinned_row_shown`) — the
  context-menu pin actions already full_refresh.  There is NO pinned
  column in the task list (removed 2026-07-16): pinning happens in the
  editor or the right-click menu — which mirrored Notes items now get
  like any other task.  Real lists nest under a collapsible bold
  "Lists" header whose expansion is SNAPSHOTTED before every rebuild
  (first population expands; force-open when the selection lives
  inside).  Lists can be organized into named **list groups**
  (`SB_KIND_GROUP` rows, stored in `list_groups` table): right-click
  the Lists header or a group → New Group / Rename Group / Remove Group;
  right-click a list → Move to Group / Remove from Group.  Group
  expansion state is snapshotted separately in `lw->group_expanded`
  (GHashTable of group_id → bool); groups expand by default on first
  population and force-open when the selected list is inside.  Selecting
  a group row does NOT refresh the task pane — it just tracks
  `sel_kind`/`sel_id`.  List labels: `emoji + two spaces + name` when
  an emoji is set.  Double-clicking a list row opens the Edit List
  dialog (metas/header/BN row: no-op).  Sidebar starts HIDDEN by default
  (`sidebar_visible`, write-through on toggle).  Lists are ALPHABETICAL
  by default and drag-reorderable: `task_db_lists` sorts by lower(name)
  until sync_state `lists_custom_order` exists (set by
  `task_db_lists_reorder`, which writes position = display index; order
  is LOCAL-ONLY — Google tasklists have none — so reorders never dirty
  rows for sync).  The DnD dest protocol is fully custom, copied from
  Notes' quirk #13: GtkTreeView's default drag-motion handler
  requests row data per motion, and on quartz the replies land before
  the release, finishing the drag mid-air — so on_sb_drag_motion
  answers gdk_drag_status itself (returning TRUE), only
  on_sb_drag_drop requests the data, and on_sb_drag_received stops the
  default emission and performs the move.  Only SB_KIND_LIST rows may
  drag or anchor a drop (never the metas, header, group, or Notes
  row).
- Window size: tracked via configure-event, persisted as `win_w/win_h`
  on clean close, restored at launch (980×640 fallback).
- Model rebuilds: capture the scrolled window's vadjustment and restore
  it idle-deferred (`scroll_keep_queue`) — clearing a store zeroes the
  scrollbar.
- Context menus built per popup MUST self-destroy via
  `g_signal_connect(menu, "selection-done", gtk_widget_destroy)` —
  attached menus otherwise live until the widget dies (fires after the
  chosen item's activate, so it is safe).
- Task view is `GTK_SELECTION_MULTIPLE`; right-click INSIDE an existing
  selection keeps it, outside collapses to the clicked row; context
  actions (mark complete/incomplete, pin/unpin, set/clear high
  priority, move, delete) apply to the whole selection — single rows
  get only the pin/priority direction that applies; "Open in Google
  Tasks" is single-row only.  Bulk data
  rides on menu items as `g_array_ref`'d id arrays with destroy
  notifies.
- Editor: 600 ms debounced write-through saves; status/pinned save
  immediately.  The first row is `Status: [combo]` … `Due: [entry] 📅`
  and the Favorite / High Priority checkboxes moved to a row of their
  own — TWO rows, because the window asks for 490 px and takes its
  NATURAL height, so an over-wide row would silently widen every editor
  while an extra row costs one row of height (measured 307 → 335
  folded).  The combo's rows are the `TaskStatus` values IN ORDER, so
  the active index IS the enum value (`editor_status_get`, which clamps
  an out-of-range value off disk to New rather than leaving the combo
  blank).  NEVER rewrite the due entry while it has focus, and a
  save must not clobber the stored date when the entry holds partial/
  invalid text (`editor_due_entry_parse`).  Editors are singletons per
  task (`app->editors` gint64 keys) / per Notes ref
  (`app->bn_editors` string keys).
- Editor foot row (packed LAST so it stays at the window's bottom in both
  fold states): an "Advanced ▾/▴" link at the left, then Save at the
  right in EVERY editor, with Cancel to its right only in the
  `task_editor_open_new` variant — so the order reads Save, Cancel
  left-to-right, which means Cancel is `pack_end`ed FIRST (pack_end puts
  the first-packed child rightmost).  Save flushes the write-through save
  and closes;
  **Cancel closes and tombstones the task** (`task_db_task_delete`, so the
  delete syncs), which is why New Task uses its own entry point —
  `task_editor_open` must never offer that.  Cancel drops the pending
  debounce and destroys the window BEFORE deleting: `on_editor_destroy`
  would otherwise flush a save into the row being tombstoned.  It then
  notifies through `notify_changed` (a vanishing task is structural), and
  `ed` is dead by then, so it captures app/task_id first.
- Advanced disclosure: Subtasks + Attachments live in `adv_box`, folded by
  default and expanded on open when the task already HAS either
  (`editor_has_advanced_content`, read off the loaded stores, so it runs
  after `editor_load`).  TWO entry points, and the difference matters:
  `editor_advanced_reveal` shows the block and records `adv_height`;
  `editor_advanced_set` is the applier for a window ALREADY ON SCREEN and
  adds the window resize, so a collapse gives back exactly the pixels the
  expand took — measured round trip 307 → 581 → 307.
  **The OPEN path reveals BEFORE `show_all` and never resizes**, so the
  window is presented once, at its final size.  It used to show_all and
  then grow, which asks the window manager to present a folded window and
  resize it a moment later: two `configure-event`s (measured 335 then 609),
  and the second only lands once the main loop gets back to it.  Off the
  Kanban board — where the same click also restyles every card — that gap
  was long enough to WATCH the window scale and then unfold (reported
  2026-08-26); from the list it usually beat the first frame, which is why
  it read as a board-only problem.  It was neither view's fault: the
  sequence was wrong for both, and the board only made it visible.  The
  click-to-editor time itself is the same in both panes (measured 141 ms
  board vs 130 ms list), so don't go looking for the cost in the card
  handlers.
  The block's height still measures true before the show: a GtkBox counts
  only VISIBLE children and `adv_box` is visible by the time it is
  measured (gotcha 15), and a size request does not need realization.
- Editor geometry: default size is **490 × -1** — the -1 means the
  layout's NATURAL height, which is only correct because `adv_box`
  carries `no_show_all` and so is absent from that measurement (gotcha
  15).  A fixed window height was the bug: the notes box is packed
  expand=TRUE, so it swallowed every pixel of slack and opened huge.
  The notes scroller is pinned to **8 lines** via BOTH
  `min_content_height` and `max_content_height`, measured by laying out
  eight "X\n" lines in the view's own Pango context (+12 px: the view's
  4 px top/bottom margins and 4 px slack so the caret on the 8th line
  is not flush against the frame).  Font metrics' ascent+descent is NOT
  the right measure — it omits Pango's inter-line gap and lands about
  half a line short.  The max is what keeps a task with 50 lines of
  notes from opening a screen-tall window; it caps the size REQUEST
  only, so a hand-resized window still stretches the box.
- Emoji picking: a bare 18 px single-char entry; click clears it and
  emits `insert-emoji` (GTK stashes its chooser on the entry as
  `"gtk-emoji-chooser"` object data).  GTK3 popovers render INSIDE
  their toplevel — the dialog grows to 440×470 while the chooser is
  open and shrinks back on its "closed" signal.

## Kanban board (the THIRD task-pane variant)

`kanban_view` (default 0; View → Kanban View) renders the current view's
tasks as a board instead of a list.  Built from the Weekly Forecast's
parts — heading label over a framed body, everything at natural height
inside ONE outer scroller so the board scrolls as a page — with the
sections turned through 90°: three side-by-side lanes, homogeneous, in a
`GTK_POLICY_NEVER` horizontal scroller so the board always fits the pane
and only ever grows downwards.

- **Lane INDEX IS the `TaskStatus` value.**  That is the whole drop
  rule: a lane carries its status as `"task-status"` object data, and
  `on_lane_drop` reads the status straight off the lane the card landed
  on.  Don't reorder the lanes without reordering the enum.
- `task_pane_mode_apply` is the SINGLE place that answers "which pane is
  on screen", called from `refresh_tasks` and again after the
  construction-time `show_all` (which reveals all three at once).
  **Weekly Forecast OUTRANKS Kanban** — it is its own panel of seven
  dated day views, not a task list with a layout, so there is nothing
  for a board to lay out; leaving the forecast puts the board back.
- The task COLLECTION in `refresh_tasks` is shared: every view that
  yields a task list can be shown as a board, list views and virtual
  views alike.  Only the presentation branches
  (`refresh_kanban` vs `append_task_rows`).
- **The drag is HAND-ROLLED — GTK DnD is NOT used on the board.**  The
  reason is PORTABILITY, not any one platform: a pointer grab plus a
  popup ghost is plain GDK and behaves the same on X11 and everywhere
  else, while GTK's DnD delegates to whatever the platform provides and
  so looks and behaves differently per backend (gotcha 19 records what
  quartz does with it; gotchas 12 and 13 are more of the same).  Owning
  the gesture is also the only way to decide what a drag LOOKS like —
  the cursor, a translucent copy of the card, and a highlighted target
  lane are all ours.  The manual-sort row drag works the same way, so
  this is the established shape here.
- The engine: `on_card_press` ARMS (records the card, its task, the press
  in ROOT coords and the pointer's offset inside the card) but does not
  drag — `on_card_motion` starts one only once
  `gtk_drag_check_threshold` passes, so a click that wobbles a pixel is
  still a click.  Starting takes a `gdk_seat_grab` whose CURSOR argument
  is the whole point: it holds over every widget the pointer crosses.
  `card_drag_stop` is the ONE way out — release, Escape, a broken grab
  and window teardown all funnel through it, so the grab can never be
  left held (which would kill the pointer app-wide) and the ghost can
  never be orphaned.  `on_library_destroy` calls it too.
- The ghost is a `GTK_WINDOW_POPUP` holding the card drawn into a surface
  made from the card's OWN window, so it inherits the display scale and
  stays sharp on HiDPI.  Translucency is a PAINTED alpha
  (`CARD_GHOST_ALPHA`) on an RGBA visual in the window's own `"draw"`
  handler, NOT `gtk_widget_set_opacity`: window opacity is a compositor
  feature that plenty of X11 setups quietly ignore, which is exactly how
  this shipped opaque the first time.  The window is `app_paintable` and
  draws nothing else, which is what stops the theme's window background
  showing as a grey plate around the card.  On a screen with no
  compositor (`gdk_screen_is_composited` false) the clear-to-transparent
  would land as BLACK, so the handler paints the card opaque there
  instead — a solid card that follows the pointer is the honest
  degradation.
- The LANDING INDICATOR is two parts, both driven from `card_drag_move`
  and cleared by `card_drag_stop`: a `.task-lane-target` tint on the lane
  says which COLUMN, and a `.task-card-mark` bar inserted into that lane
  says which SLOT.  The tint's rule is listed AFTER `.task-lane` so it wins
  at equal specificity (both classes sit on the same widget).  The
  dragged card keeps its place, dimmed with `.task-card-dragging`, and is
  NOT hidden — its GdkWindow is the grab window, and unmapping that would
  break the grab and end the drag on the spot.
- The marker is rebuilt only when (lane, slot) CHANGES, tracked in
  `card_mark_lane`/`card_mark_slot`.  It occupies a slot in the lane, so
  re-inserting it per motion event would shuffle the cards under the
  pointer continuously; `CARD_MARK_H` is 3 px for the same reason.
  Rebuilding rather than reparenting avoids ref juggling —
  `gtk_container_remove` would drop the last reference and destroy the
  widget being moved.  `refresh_kanban` NULLs the pointer without freeing
  it, because the rebuild already destroyed it with the rest of the lane
  (a refresh can land mid-drag: an editor autosave does it).
- The drop reads its slot from the MARKER, not by re-measuring: the
  marker is what the user was looking at, and measuring again would
  answer against a lane whose geometry the marker itself has shifted.
- **CARD ORDER** lives in ONE config key per view
  (`kanban_order_<view>`, `kanban_order_key`) holding every card of that
  view as a comma-separated id list, lane by lane in display order.  One
  list rather than three because the lanes already filter by status, so
  the concatenation projects onto each lane correctly — and one key per
  view is one key for `on_delete_list` to remove.  It is its OWN key
  family, NOT the manual sort's: a board drag must not silently rearrange
  a list the user hand-sorted in the list view, and board ordering is
  always live where manual sort is behind a toggle.
  `kanban_order_apply` runs in `refresh_kanban` BEFORE the tasks are
  handed out to lanes.
- `card_drop_apply` has two independent halves, either of which may be a
  no-op: the STATUS (only when the lane changed — a real write that
  stamps `updated_at` and syncs) and the ORDER (local-only config, never
  touches the row).  A drag that lands the card exactly where it was does
  NEITHER.  Only a status move posts to the status bar — a reorder is its
  own feedback, and announcing it would spam "— New" for every nudge.
- Drops hit-test in ROOT coordinates against `lw->kanban_drops[]`
  (`card_lane_at_root`), because the pointer spends the drag over other
  widgets.  The refresh after a drop is `g_idle_add`-DEFERRED: the drop
  runs inside the dragged card's own handler and `full_refresh` destroys
  every card including that one, so refreshing inline would return into
  a freed widget.
- The lane body is an EVENT BOX wrapping the card box, never the box
  itself: a GtkBox is a no-window widget, so it has no window origin to
  hit-test against and no surface to paint the lane tint on.  It is
  packed `expand=TRUE` so a short lane is still a target all the way
  down.
- `on_card_press` returns FALSE for a single click so the press keeps
  propagating; a double-click opens the editor (cancelling the drag its
  own first click armed) and returns TRUE.  The click's selection
  restyle walks the lanes IN PLACE rather than calling a refresh — a
  refresh here would destroy the very widget the drag is about to start
  from, and the click would never become one.
- `lw->kanban_sel` is the board's answer to a tree selection, and
  `selected_task_ids` returns it while the board is up — that is what
  keeps Delete Task working from the toolbar, the File menu AND the
  Compact Layout floating pair without any of those knowing which pane
  is showing.  It is cleared when the card's task disappears, and on
  every Kanban toggle (the two panes track selection differently).
- The floating New/Delete pair keeps working for free: the overlay wraps
  the PANED and `kanban_box` is just another child of the task pane
  inside it.
- `show_completed` applies as it does everywhere else, so with completed
  hidden the Done lane simply empties.  The lane stays on screen as a
  drop target, so dragging a task there still completes it — the card
  vanishing afterwards is the same behavior as the list's fade-out.
- The MANUAL SORT toggle governs the list view only: the board is always
  drag-orderable (see CARD ORDER below), and
  `task_view_apply_manual_order` walks `task_store`, which the board
  deliberately leaves empty (a selection left there would feed Delete
  Task).  The two orders are separate keys and never overwrite each
  other.
- A same-lane drop is a NO-OP by design — every status write stamps
  `updated_at`, so letting it through would buy a sync round trip for a
  drag that changed nothing.
- **Corners are SQUARE**, matching the forecast's framed day sections (a
  plain `GTK_SHADOW_IN` GtkFrame, which has no radius).  Rounded lanes
  and cards were the first cut and were rejected (2026-08-25): a rounded
  tint inside a square frame reads as a mistake, and rounded cards made
  the board the odd view out.  No `border-radius` in the board's CSS is
  deliberate — don't add one back.
- Inner spacing is WIDGET MARGINS on the child (`pad_widget`, CARD_PAD 8
  / LANE_PAD 6), not CSS `padding` and not `border_width` — see gotcha
  18.  The card still paints its background and border at its own edge.
- **A card has a ⠿ GRIP down its left edge, and that is the ONLY place a
  drag starts** — the same division the list view's handle column makes.
  It is its own event box (`.task-card-handle`) for two reasons: a
  different cursor needs a different GdkWindow, and it gives the press
  handler somewhere to live.  `on_handle_press` arms the drag and returns
  FALSE so the press still reaches the CARD and selects — gripping selects
  too, and a double-click on the grip still opens the editor.  The hot
  spot is translated into the CARD's coordinates
  (`gtk_widget_translate_coordinates`), because the ghost is a picture of
  the whole card and must hang off the pointer where the card was gripped.
  The GRAB goes on the grip's window, since that is where motion and
  release are delivered.  `on_card_release` is connected to the card AS
  WELL as the grip: press the grip, drift onto the text, let go — with no
  grab yet that release lands on the card, and the armed flag would
  otherwise be left set.
- Cursors: an open hand (`"grab"`) over the GRIP ONLY, a closed one
  (`"grabbing"`) while dragging.  Both are made ONCE and cached on
  `lw->card_grab` / `card_grabbing` (`card_cursor`), like the task view's
  `drag_cursor` — a card is realized per refresh, so building one per
  card would allocate on every rebuild.  The hover cursor goes on the
  GRIP's own GdkWindow from its `"realize"` handler rather than being
  tracked with enter/leave, so hovering costs nothing per motion event —
  and the card body is given NO cursor at all, which is exactly what
  leaves it showing the ordinary arrow for clicking and selecting.
  The CLOSED hand is set THREE ways at drag start — the `gdk_seat_grab`
  cursor argument (the portable lever, and what X11 honors), plus the
  card's and the toplevel's window cursors, because some backends apply
  the cursor of the window under the pointer instead.  All three cost
  nothing and leave no backend showing an arrow mid-drag;
  `card_drag_stop` puts them all back.  `gdk_cursor_new_from_name`
  answers NULL for a name a display cannot supply, and the code then
  falls back to the window default rather than a guessed stock cursor.
- **Multi-select** is a GHashTable id SET (`kanban_sel`) plus a
  `kanban_anchor`: plain click selects one, MODIFY_SELECTION-click
  toggles a card in or out, EXTEND_SELECTION-click takes the run between
  the anchor and the card WITHIN one lane (across lanes it merely adds —
  a "run" spanning two lanes has no meaning on a board).  Both modifiers
  come from `gtk_widget_get_modifier_mask`, never hardcoded: MODIFY is
  Ctrl on X11 and Cmd on quartz, and asking the widget is the only way
  to be right on both.  `selected_task_ids` returns `card_sel_ids` (board
  display order) while Kanban is up, so every existing bulk action — the
  context menu's plural variants, the toolbar Delete — acts on the whole
  selection with no extra wiring.  A plain PRESS inside an existing
  selection deliberately KEEPS it, which is what lets a multi-card drag
  start from any of its cards; the collapse to the clicked card happens
  on RELEASE, and only when no drag took place.  The anchor moves on
  every plain press either way — it means "the last card plainly
  clicked", and tying it to the collapse left a following shift-click
  measuring its run from a card the user had touched several clicks
  earlier.  `refresh_kanban` repaints from the set and PRUNES it to the
  ids that survived the rebuild, so a deleted or filtered-out card
  cannot linger in it.
- A drag by the grip carries the WHOLE selection when the gripped card is
  part of it, else just that card, and the ghost paints a count badge
  whenever more than one is in flight — a snapshot of the gripped card
  alone would claim a single-card move.  `card_drop_apply` therefore
  takes the moving ids as a GArray: it re-inserts them at the target slot
  preserving their relative order, writes the lane's status to each that
  needs it, reselects them, and reports "N tasks â <status>".
- The board's CSS is installed ONCE for the screen
  (`kanban_css_install`, `.task-lane` / `.task-card` / `.task-card-selected`),
  not per widget like `themed_bg_css_apply`: there would otherwise be one
  provider per card, and every color is a NAMED theme color, so GTK
  re-resolves them itself on a light/dark switch.  That staleness problem
  only exists in the per-widget helper because it bakes a resolved
  literal into its CSS from C.

## Data safety (read this before touching the database file)

A 1965-task production database was destroyed on 2026-08-26.  These rules
are the post-mortem; none of them is optional.

- **"It opened" is NOT "it is intact."**  SQLite opens a malformed file
  happily and errors only when a damaged page is READ.  Anything that
  copies, moves or migrates the database must check with
  `task_db_verify_file` (integrity_check + foreign_key_check on its own
  read-only connection, BOTH exec return codes honored) — never by
  opening it.  That mistake is precisely what turned a bad copy into
  data loss: `switch_database` discarded `copy_file`'s return value,
  treated a successful `task_db_open` as proof, and deleted the original.
- **Copy with `task_db_copy_file` (VACUUM INTO), never a byte copy.**  It
  runs in a read transaction, so it cannot capture a torn page.  The old
  `copy_file` helper in app.c was DELETED; a comment stands in its place
  so it does not come back.
- **COPY → VERIFY → and only then delete anything.**  In that order, with
  the delete conditional on the verify.  A copy that fails verification
  is left in place and reported, and the original is kept.
- **A migration backs the file up FIRST.**  `task_db_open` writes
  `<db>.pre-v<N>.bak` via VACUUM INTO before running any migration, one
  per from-version, never overwritten.  `ALTER TABLE … DROP COLUMN` (v7)
  rewrites the whole tasks table; doing that to someone's only copy with
  no backup is how this happened.
- **The database routinely lives in a SYNC FOLDER** (iCloud Drive is the
  user's normal setup).  Assume the file can be replaced, evicted or
  re-generated underneath an open connection.  That is not a hypothetical
  — it is the standing operating environment, so partial copies and
  surprise generations are REALISTIC failures to design against.
- **`task_app_switch_database` re-arms ALL THREE timers** (sync, Notes
  mirror, backup) on the new path.  Each captured the old path when
  installed, and that file has just been deleted — left alone the workers
  would open a nonexistent path and CREATE an empty database there.  This
  doc claimed it happened long before the code did; it does now.
- The optional rotating backup (`backup.[ch]`, off by default) is the
  independent-copy safety net: own worker + connection, VACUUM INTO,
  verify, and prune ONLY after a new backup verifies — so a run of
  failures cannot erode good history.  It matches only its own
  `tasks-*.db` filenames when pruning (so anything else in the folder is
  safe, and the live `tasks.db` is not a candidate — `tasks.` is not
  `tasks-`), and is bounded by `backup_keep`.
  `task_backup_dir` is the single answer to "where": `backup_dir` when set,
  else the DEFAULT DATABASE DIRECTORY under the home dir, created on
  demand — so there is no enabled-but-inert state, and Settings displays
  that same resolved value so the label cannot promise a folder the
  worker does not use.  A pass whose source is unchanged writes nothing;
  the stamp (`sync_state.backup_source_stamp`) includes the DESTINATION,
  because otherwise choosing a new folder would leave it empty until the
  database happened to change and the feature would look broken.

## Sync architecture (Google Tasks)

**This is a PLUGIN** — `src/plugins/gtasks/`, built to `plugins/gtasks.so`
and reaching the app only through the `TaskHostApi` table.  The core does
not name it, and does not link libcurl for it: the plugin asks for that
itself in its own `deps.mk`, which is checkable with `ldd tasks` /
`otool -L tasks`.  Everything below is how the sync BEHAVES; none of it
is core code any more.

- Worker thread with its OWN SQLite connection (a connection never
  crosses threads); status/completion marshalled through
  `host->notify->invoke_main`; `curl_global_init` happens in the
  plugin's `task_plugin_entry` BEFORE any worker of its own exists.
- Identity: `gtasks_task` / `gtasks_list` (schema v8) carry `gtasks_id` +
  `etag` against the row id, and the row's own `updated_at` says when it
  changed.  The tables are the PLUGIN's, created from its `db_open`
  hook; nothing about Google is on a core row.  Deletes are
  tombstones until pushed, then purged.  `sync_state.last_sync` = the
  START time of the last successful pass.
- Incremental: after the first full pass, task fetches use
  `updatedMin = last_sync - 300` (overlap for clock skew) with
  showDeleted/showCompleted/showHidden.  CRITICAL: with a partial
  listing, "absent" means UNCHANGED (push if locally dirty), never
  deleted — deletions arrive as `deleted:true` items.  First-sync
  title dedup only runs against a full listing.
- NON-DESTRUCTIVE by default: ABSENCE NEVER DELETES on either side.
  Only explicit deletes propagate — a local tombstone DELETEs
  remotely; a remote `deleted:true` purges locally.  A local task
  whose gtasks_id is missing from a FULL listing (deleted on Google
  with no local tombstone) drops its stale Google identity and is
  pushed back as a NEW remote task; a local list whose bound remote
  list vanished is re-created remotely and ALL its tasks' gtasks_ids/
  etags are cleared (`task_db_tasks_clear_gtasks_ids`) so the task pass
  re-pushes them.  Remote items unknown locally are pulled (created)
  as before.
- Pushes: creates POST (parents before subtasks — the per-list query
  orders `parent_id IS NOT NULL` last); edits PATCH with `If-Match:
  etag`, and a 412 SKIPS the push (remote wins; next pull reconciles).
  Conflicts otherwise resolve newest-wins; deletion beats concurrent
  edit.  Replies stamp rows clean (remote updated + fresh etag).
- LOCAL-ONLY fields (never sent): `pinned`, task `priority` (binary
  high-priority flag; every view query sorts `priority DESC` first so
  flagged tasks top any list they appear in, 🚨 (siren emoji) in the task cell,
  which also carries ❗ on every mirrored Notes item (innermost prefix,
  nearest the title — see `task_desc_markup`; the glyphs stack outwards
  ↳ 🚨 ⭐️ ❗ Title),
  "High Priority" editor checkbox), list `emoji`, attachments.
  A write to a local-only field must NOT bump `updated_at`
  (`task_db_task_set_pinned` / `_set_priority` deliberately don't): the
  bump marks the row sync-dirty, so every Favorite/priority toggle buys
  a no-op PATCH, and a concurrent remote edit can be starved behind a
  412 skip for as long as the flag keeps changing.
  The API has NO starring and `due` is DATE-ONLY (time is documented as
  discarded and unreadable) — both confirmed against the docs; don't
  re-attempt.  Its own `status` is BINARY too (completed /
  needsAction), so New and In Progress both push needsAction and the
  difference between them never leaves this machine; the dirty-compare
  therefore tests `(t->status == TASK_STATUS_DONE) != match->done`, not
  the whole field, or a New → In Progress move would push a body
  identical to what is already there.
- `hidden` remote tasks (completed + cleared) are never re-created
  locally — that's what keeps Clear Completed from resurrecting rows.
- Cross-list move: `tasks.move` + `destinationTasklist` on a worker
  (children moved under the parent afterwards); offline/failure
  fallback = stub tombstone in the source list + strip gtasks_ids so
  the rows push as new.  Clear Completed: `tasks.clear` + local purge
  when synced/signed-in, tombstone deletes otherwise.
- OAuth: installed-app flow, PKCE (GLib SHA-256), loopback
  GSocketService on an ephemeral port, `access_type=offline` +
  `prompt=consent`; refresh token persisted in the ini
  (`gtasks_refresh_token`), access tokens in memory only.  The OAuth
  client resolves as: client-secret JSON file next to the binary (or
  user config dir) → legacy ini keys `google_client_id`/`_secret` (no
  UI writes them) → baked-in default via gitignored
  `client_credentials.mk`.  THIS machine bakes via
  client_credentials.mk ONLY — the JSON was deliberately deleted
  (2026-07-15); don't look for it or recreate it.  Objects and
  compile_commands.json depend on the mk file, so a plain `make`
  picks up credential edits.  Races handled: a
  sign-out mustn't be resurrected by an in-flight refresh (re-check
  `cred_refresh_token` before caching); a timed-out flow discards a
  late exchange result.  The consent screen's app name comes from the
  Google Cloud OAuth registration, not from this app.

## Notes integration (the action-item MIRROR)

**This is a PLUGIN** — `src/plugins/notes/`, built to `plugins/notes.so`.
`notes.c` is the mirror, `bnotes.c` the CLI wrapper; they share one host
table and one identity through `plugin_ctx.h`.  It owns `notes_task`
(uid + the done/due BASELINE) and `notes_deleted`, created from its
`db_open` hook — schema v9 moved them off the task row.

Rewritten 2026-08-05: action items are no longer a special row type.
Each one is MIRRORED as an ordinary task, so it carries notes,
subtasks, attachments, a pin and a priority like anything else — and,
living in a real list, it syncs on to Google Tasks too.

The mirror's worker declares `sort = -10` so it runs BEFORE the Google
sync: one press of Sync then carries a new action item all the way to
Google rather than taking two.  Registration order would otherwise
decide it, and that is whatever order the plugin loader's directory read
happened to return.

- ALL access via the `notes` CLI (`action list --uid`, `action
  done/undone/due`), NEVER its database file — Notes' GUI/CLI
  coexistence is a single-writer design (CLI routes through the running
  GUI's socket).  Row format with `--uid`:
  `UID \t NOTEID:ORD \t [x]|[ ] \t YYYY-MM-DD|- \t text`.
- **Identity is the UID, never the position.** `NOTEID:ORD` renumbers
  whenever a note gains or loses a '!' line (Notes assigns ord by
  position), so a stored ref silently comes to mean a different item —
  a "done" tick would strike the wrong line.  The uid is stable across
  rewording and renumbering.  A Notes too old to know `--uid` makes
  the pass REFUSE to run ("Notes is too old…") rather than fall back
  to positional addressing: `task_bnotes_supports_uid()` tells that case
  apart on the failure path only, so a healthy pass costs ONE spawn.
  The listing's positional second field is validated as a format guard
  and then DISCARDED — nothing keys off a positional address.
- Field ownership: Notes owns TITLE, DONE and DUE; a title edited in
  Tasks is overwritten next pass (the CLI has no verb to rewrite an
  item's text).  Everything else is Tasks-only and never leaves.
  Notes' DONE is binary, so the mirror speaks only in the DONE-ness of
  `status` (`local_done` in `sync_item`): a New ↔ In Progress move is
  not a pending write and has nothing to push, and an item Notes
  reports as unfinished keeps whichever of the two it already had.
- **Writes are cached, not live.** `tasks.bn_done`/`bn_due` hold what
  Notes was last known to have, so the rows whose done-ness
  (`status == TASK_STATUS_DONE`) or `due` differs
  from that baseline ARE the pending-write set — no queue table to
  corrupt, and it survives a crash.  Each pass pushes them in bulk on
  `notes_sync_interval_min` (default 5; 0 = only on Sync).  A local
  change WINS over a concurrent Notes-side one: it is pushed first
  and the listing reads back what was just written.  A REFUSED push
  keeps the user's local value on the task but leaves the baseline
  alone, so the delta is retried — which is why
  `task_db_task_apply_notes` takes the baselines separately from the
  applied values.
- Existence: Notes is authoritative, so an item that leaves it
  tombstones its task.  The reverse has no CLI verb, so deleting a
  mirror task in Tasks parks its uid in `bn_deleted` (done inside
  `task_db_task_delete`'s transaction) — without that the very next pass
  would helpfully re-create what the user just deleted.  The
  suppression is dropped once the item is gone from Notes too.
- The sidebar's "Action Items" row is a META VIEW (`SB_KIND_BN_ACTIONS`
  among Favorites / All Tasks / Due Today), not a list: it queries
  `bn_uid > 0` across every list, so an item filed anywhere still shows
  up in one place.  Toggled by `notes_meta_row`; `virtual_view` stays
  TRUE so each row keeps its "in <list>" line.
- Items live in `notes_embed_list` when it names a live list, else
  the managed "Action Items" list (❗), created on first use.  The
  target is consulted when a task is CREATED, so changing the setting
  needs `task_bnsync_reconcile_target` to carry the existing items over —
  without it the setting silently only affects the next new item, which
  reads as "the setting does nothing".  It compares against the applied
  value in `sync_state.bn_target_list` and moves only on a real change,
  so a task moved to another list BY HAND stays there; an ABSENT
  applied value counts as "not yet applied" (the upgrade case).  It
  runs on the main thread from `task_bnsync_auto_start` and the Settings
  combo, and goes through `task_gtasks_move_task` because a cross-list
  move has a remote half — a bare `list_id` update would strand the
  Google copy in the old list.
- Threading matches gtasks: own worker, own SQLite connection, CLI
  spawned there too, results marshalled with `g_idle_add`.  The toolbar
  Sync runs the mirror FIRST so a new action item reaches Google in one
  press.  BOTH timers carry their db path, so `task_app_switch_database`
  must re-arm both.
- The Settings CLI-path entry persists per keystroke but only runs a
  pass on Enter/focus-out (a per-keystroke pass would spawn the
  half-typed command).

## Hard-won gotchas (do not re-learn)

1. GTK3 popovers cannot escape their toplevel window — size the window,
   don't fight the popover (emoji chooser).
2. `g_clear_pointer` is a statement-style macro; it cannot sit inside
   an expression.
3. A `GtkMenu` built per right-click and attached to a widget leaks
   until that widget dies unless destroyed on "selection-done".
4. Notes gotcha inherited: clearing a tree/list store zeroes the
   view's scrollbar (restore idle-deferred) and collapses expansion
   state (snapshot before clear).
5. `gtk_tree_view_set_enable_search(view, FALSE)` on every tree view —
   the auto search column is the int64 id and matches nothing.
6. Status-bar height parity needs margins (8/8/3/3), not
   `border_width` — border pads every edge.
7. libcurl's implicit global init is not thread-safe; init in main().
8. sqlite `UPDATE` SET expressions read the OLD row values — the
   `completed_at CASE WHEN ?done=1 AND done=0` transition relies on it.
9. Toolbar right-click style menu fires on EMPTY toolbar area only
   ("popup-context-menu").
10. macOS AX geometry (osascript) reports frame incl. titlebar;
    `gtk_window_get_size` is the client area (~28 pt difference).
11. The live ini rewrites drop comments and carry per-machine values —
    it stays gitignored; document defaults in `tasks.ini.defaults`.
12. `Gdk-CRITICAL … gdk_atom_intern: assertion 'atom_name != NULL'` on
    the console during a sidebar list drag (or exotic clipboard
    flavors) is a GTK-quartz bug, NOT ours — do not re-investigate:
    `gdkselection-quartz.c:199` (3.24.52) interns
    `uti.preferredMIMEType.UTF8String`, which is NULL for the `dyn.*`
    UTIs macOS wraps around custom pasteboard types like
    GTK_TREE_MODEL_ROW.  Harmless (that flavor is skipped, the row
    flavor still resolves); documented in User_Guide.md.  Don't
    suppress with a log filter.
14. `task_app_switch_database` always **removes the old database file**
    after a successful switch — it is a move, not a copy.  This holds
    even when the user picks "Use Existing Database" (no copy was made,
    but the old file is still removed so no orphan is left behind).
    Do not add logic that skips the delete based on whether a copy was
    performed; that was the bug that caused orphaned files.
13. Google's DEFAULT tasklist cannot be deleted — `tasklists.delete`
    returns 400 "Invalid Value" from any client, and an unhandled
    failure there aborts the whole sync pass (blocking every later
    push).  Handled ACCURATELY (no hidden state): every sync GETs
    `…/lists/@default` and stores its id as
    `sync_state.default_list_gid`; on_delete_list refuses that list up
    front (like the Notes list), and if a stale tombstone for it
    exists anyway, sync_lists RESTORES the list + its same-moment task
    tombstones (`task_db_list_restore`) instead of deleting or hiding
    anything.  sync_lists also seeds the default list's emoji to 🔴
    (`task_db_list_emoji_if_empty` — only while empty, no updated_at
    bump) so it is visibly marked; a user's later emoji edit sticks.
15. `gtk_widget_show_all(w)` returns EARLY when `w`'s OWN `no_show_all`
    flag is set — the flag is not just "skip me when a parent recurses".
    So the "hide it, then show it explicitly later" pattern silently
    never shows anything.  Both deferred sections in the editor keep the
    flag and LIFT it across their own show_all
    (`set_no_show_all(FALSE)` → `show_all` → `set_no_show_all(TRUE)`):
    the Advanced block (`adv_box`, in `editor_advanced_set`) and the
    "From Google" box (`google_section_load` — without the lift that
    section could never appear at all; found and fixed 2026-08-05).
    Keeping the flag is load-bearing, not just tidy: it is what keeps
    both boxes out of the window's NATURAL height, which the editor
    passes -1 to use.  Related: a GtkBox's preferred height counts only
    VISIBLE children, so measure a disclosure block AFTER showing it or
    the grow-the-window arithmetic reads 0.
16. A `CREATE INDEX` naming a column added by a guarded `ALTER` must run
    AFTER the migrations, never inside the schema block at the top of
    `task_db_open`.  On an EXISTING file the column does not exist yet at
    that point, the statement fails, and — because it rides in the same
    batch — it takes the rest of the schema setup with it, leaving the
    database unopenable.  Fresh files hide this completely: they only
    ever exercise the path where every column already exists.  (Learned
    the same day in Notes, which hit it for real on `action_items`.)
17. A Notes CLI call is answered by whichever Notes instance owns
    `~/.cache/notes.sock`, NOT by the binary on disk: `cli.c` forwards
    argv to the running GUI.  So an old GUI left running makes a new
    CLI feature look missing — `action list --uid` answers with usage
    and exit 1 while the on-disk binary supports it perfectly.  Check
    `lsof -U | grep notes.sock` before concluding a verb is absent.
    The socket was `records.sock` before the 2026-08-11 rename, so a GUI
    started before it still owns the OLD path and answers nothing on the
    new one — restart that GUI rather than debugging the CLI.
18. A visible-window `GtkEventBox` honors NEITHER CSS `padding` NOR
    `gtk_container_set_border_width` for its own size: it comes out
    exactly as big as its child, with the content hard against the
    border its CSS just drew.  Both were tried on the Kanban cards and
    both silently did nothing — measured 214x15 around a 15 px label.
    Use WIDGET MARGINS on the CHILD (`pad_widget`); GTK folds those into
    the preferred size everywhere, so the parent grows by them while its
    background and border still paint at its own edge.  Note this is the
    OPPOSITE lever from gotcha 6, where the status bar needed margins
    because `border_width` pads every edge — here the widget ignores it
    outright.  If a padding looks like it did nothing, MEASURE
    (`gtk_widget_get_allocation` on the parent and the child) rather
    than nudging the number: a value that is being ignored looks exactly
    like a value that is too small.
19. GTK's drag-and-drop DELEGATES to the platform, so what a drag looks
    like is not yours to decide through it.  On quartz specifically it
    becomes an AppKit `NSDraggingSession`, which owns the cursor for the
    duration (arrow-with-green-plus badge) and ignores
    `gdk_window_set_cursor` anywhere, including from `"drag-begin"`.
    When a drag has to LOOK a particular way — the Kanban board wants a
    closed-hand cursor, a translucent copy of the card, and a highlighted
    target — own the gesture instead of delegating: arm on press, start
    past `gtk_drag_check_threshold`, `gdk_seat_grab` with the cursor you
    want, follow the pointer with a `GTK_WINDOW_POPUP` ghost, hit-test in
    root coordinates on release.  That is plain GDK and behaves the same
    on X11, which is the point — the manual-sort row drag already worked
    this way.  Two traps inside it: set the drag cursor on the GRAB *and*
    on the card and toplevel windows (backends disagree about which
    wins), and funnel EVERY exit (release, Escape, grab-broken, window
    teardown) through one stop function — a grab left held kills the
    pointer for the whole app, not just this window.
20. `gtk_widget_set_opacity` on a toplevel is a COMPOSITOR feature and is
    quietly ignored where none is running (and on some backends' popups),
    so a "translucent" window can ship fully opaque with no warning.  For
    a window that must be see-through, paint the alpha yourself: set an
    RGBA visual from `gdk_screen_get_rgba_visual`, make the window
    `app_paintable` (otherwise the theme's own background draws as a grey
    plate around your content), and in `"draw"` clear to transparent with
    `CAIRO_OPERATOR_SOURCE` before `cairo_paint_with_alpha`.  Guard the
    clear on `gdk_screen_is_composited` — without a compositor it lands
    as BLACK, so paint opaque there instead.
21. **TEST A FRESH DATABASE, not just the one on this machine.**  The
    `CREATE TABLE tasks` in `task_db_open` declared `status` TWICE (the
    v7 column plus a leftover appended copy).  SQLite rejects the whole
    statement on `duplicate column name`, so **every brand-new database
    came up with no `tasks` table at all** — found 2026-08-26 while
    verifying the plugin port, having survived several commits.  It was
    invisible for the exact reason gotcha 16 is invisible in reverse:
    `IF NOT EXISTS` on an EXISTING file is a no-op, so every developer
    machine and every real user database sailed past it, and only a
    first run could ever hit it.  The error discipline did its job —
    `exec()` logged sqlite's own message and the follow-on `CREATE INDEX`
    logged "no such table: main.tasks" — but nobody was running a fresh
    file to read them.  When touching the schema block, make an empty
    database and open it (a zero-byte `tasks.db` skips the first-run
    dialog and exercises the create path), then check
    `PRAGMA table_info(tasks)` and the log for warnings.  "It opened" is
    not "it is intact" applies to a NEW file just as much as to a copy.
22. A plugin's SIDE TABLE is created twice on purpose, and both are
    right.  The plugin's `db_open` hook creates it because a database
    that never had the old columns still needs it; the MIGRATION in
    db.c creates it because it must move existing data whether or not
    that plugin is installed.  `IF NOT EXISTS` makes the pair harmless.
    What must NOT happen is the core schema block creating it — that is
    the app declaring a table it knows nothing about, and it was
    removed when Google Tasks became a plugin.
