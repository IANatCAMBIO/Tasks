# Tasks — project guide

Task-list app in **plain C + GTK4 + SQLite**, the companion app to
Notes.  Two window types: a Library (lists sidebar + tall task rows)
and one editor window per task.  No GNOME HeaderBars anywhere — plain
`GtkWindow` titlebars, formatted `"Tasks - <thing>"`.

Google Tasks sync and the Notes action-item mirror are not in this build.
They return as integrated features after the GTK4 port lands.

## Target platform: LINUX FIRST

**This is a Linux / Debian / XFCE-first application.  macOS is SECONDARY
support.**  It is developed on a Mac, which makes it easy to get this
backwards — don't.  What that means in practice:

- Choose the PORTABLE mechanism, and judge it by how it behaves on
  X11/GTK.  macOS behavior is a compatibility check, never the reason a
  design is picked.
- Where a quartz quirk has to be worked around, the workaround must not
  degrade the X11 path, and it gets a comment saying which platform it
  is for.
- Several gotchas below are quartz-specific.  They are recorded so nobody
  re-investigates them, NOT because the Mac drives the design.  A fix
  justified only by "AppKit does X" is the wrong shape — restate it in
  terms of what is portable.
- The GTK version, the widget set and the theming assumptions are all
  stock GTK4, so an XFCE desktop with Adwaita is the reference look.

## Naming

The app is **Tasks**.  It has never shipped, so there is no older
spelling anywhere in the wild and NO migration or compatibility code for
one — not for the config, not for the database, not for file names.  If
you find something that reads like an upgrade path, it is dead code.

**"Lists" is a real word in this app** and most occurrences are the DATA
TYPE, not an old name — do not sweep them.  The sidebar has a collapsible
**Lists** section holding the user's task lists; `TaskList`, the `lists` and
`list_groups` TABLES, `task_db_lists*`, `SB_KIND_LIST`,
`manual_order_list_<id>`, `kanban_order_list_<id>`,
`manual_order_group_<id>` all mean lists-the-data-type.

The repo DIRECTORY is `~/salt_development/tasks-gtk4` (the gtk4 worktree),
the GitHub repo is `IANatCAMBIO/Tasks`.

Public symbols are prefixed `task_`, types `Task`, macros `TASK_`.  These
were `bt_`/`Bt`/`BT_` — "Blue Tasks", the app's first name — renamed on
2026-08-26.  Three names would have stuttered and were COLLAPSED rather
than mechanically prefixed, so do not "restore" the pattern:

- `BtTask` → **`Task`** (the domain object needs no prefix)
- `BtTaskStatus` → **`TaskStatus`**
- `bt_task_free` → **`task_free`**

Functions that name their subject still read `task_db_task_get` — the
namespace, the module, then the subject; that is not a stutter to "fix".

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./tasks  (-Wall -Wextra must stay clean)
make run
```

Dependencies (MacPorts): `gtk4 +quartz`, `sqlite3`, `pkgconf`.  After
toggling a dependency run `make clean && make`.

**SANDBOX RULE**: Always use `make run-dev` for testing.  It runs
`XDG_DATA_HOME=$(pwd)/dev/data ./tasks`, keeping all data in `dev/data/`
and leaving the real database at `~/.local/share/tasks/tasks.db`
untouched.  A zero-byte `dev/data/tasks/tasks.db` is seeded automatically
to skip the first-run dialog and exercise the fresh-schema path (gotcha
21).  Delete it to test the first-run dialog; delete `dev/` to start over.

Launch for testing: `pkill -f './tasks'; nohup ./tasks
>/tmp/task_launch.log 2>&1 &` then `screencapture -x` for screenshots.
Do NOT drive the GUI with osascript accessibility clicks (rejected by the
user).  Note: on macOS/quartz, GTK4's stderr goes to Console.app, not to
the nohup log — use Console.app to see GLib warnings and GTK messages
(gotcha 34).

Run the logic test suite (no display required): `make test`.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config → db → registries → window |
| `src/app.[ch]` | Shared `TaskApp` context; ini config; dialogs; icon loader; CSS helper; date helpers |
| `src/backup.[ch]` | OPTIONAL rotating db backups: own worker + connection, VACUUM INTO + verify, bounded rotation; off by default |
| `src/db.[ch]` | SQLite schema (user_version 12) + CRUD; `TaskStatus` tri-state; tombstones + `updated_at`; `step_done`/`exec_txn` error discipline |
| `src/search.[ch]` | The toolbar search box's query language: parse once, match per task.  Pure GLib — no GTK, no SQLite |
| `src/recur.[ch]` | Recurring tasks: GDateTime schedule arithmetic, deadline-armed pass — the ONE core worker that runs on the main thread |
| `src/library_window.[ch]` | Top-level window: window creation, paned layout, toolbar, View menu, status bar, `notify_changed`/`notify_tasks` dispatch |
| `src/sidebar.[ch]` | Sidebar: GtkListView over GListStore; virtual lists + collapsible Lists section with list groups; right-click menus |
| `src/task_list.[ch]` | GtkColumnView task list: column setup, cell factories, manual-sort drag, search, header column menu |
| `src/kanban.[ch]` | Kanban board: three lanes, GtkGestureDrag card drag, drop indicator, multi-select |
| `src/library_priv.h` | Shared private header: `TaskLibrary` struct, shared helpers, `lib_of(app)` |
| `src/editor_window.[ch]` | Per-task editor (debounced write-through saves); Status dropdown; the Recurrence block |
| `src/settings_window.[ch]` | Singleton settings: appearance, database |
| `src/task_view.[ch]`, `src/task_ops.[ch]`, `src/task_worker.[ch]`, `src/task_rows.[ch]` | Model-layer registries: sidebar views, core ops + hooks, the scheduler, row data |
| `src/list_rows.[ch]` | Row object for GListStore (sidebar and task list) |
| `src/core_views.c` | The app's own sidebar views (Favorites, All Tasks, Due Today) |

## Conventions

- `task_` prefix for public symbols, `Task` for types; every function gets a
  banner comment.  **Headers carry the full public contract** (purpose,
  params, returns, ownership, failure behavior); `.c` banners say "see
  x.h" plus the how.  Non-obvious variables get column-aligned trailing
  comments; ~78-col lines.  UTF-8 escapes (`\xe2\x80\xa6`) for …/—/✓ in
  source strings (gotcha 25).
- **ONE DIRECTORY holds everything this app keeps per user**:
  `task_db_default_dir()` — `<user data dir>/tasks`, which is
  `~/.local/share/tasks` on Linux.  The DATABASE and the INI both default
  there, so there is one place to back up, one place to look, and no
  answer to "where are my settings?" that depends on how the binary was
  started.
- Config: `tasks.ini` is resolved ONCE, in three steps —
  `<data dir>/tasks/tasks.ini` if it EXISTS; else `tasks.ini` NEXT TO THE
  BINARY if it EXISTS (portable mode); else CREATED at
  `<data dir>/tasks/tasks.ini`.  Seeded from `tasks.ini.defaults` which
  ships NEXT TO THE BINARY; loaded ONCE, written through on change, never
  re-read.  The ini GROUP NAME is `[tasks]` — the app reads only that
  group.  There are no config migrations.
- **Error discipline**: every prepared WRITE goes through `step_done()`
  (logs sqlite's message on prepare/step failure — silent write loss is
  the unacceptable outcome); multi-statement writes go through
  `exec_txn()` (BEGIN IMMEDIATE + ROLLBACK on failure).  Create failures
  (id 0) must surface a status-bar message at the call site.  The
  startup integrity check is UNCONDITIONAL and has NO setting: two
  PRAGMAs are cheap, they catch a file gone bad before more is written,
  and an off switch is just another way to be told nothing is wrong by
  something that never looked.
- Notify hooks on TaskApp: `notify_changed` = FULL refresh (sidebar +
  tasks + reload all editors) for structural changes; `notify_tasks` =
  task pane only — editor saves and subtask/attachment edits use this
  (the full path is expensive; use the narrow one when the structure
  hasn't changed).  `task_app_status()` for events.  Teardown: NULL the
  hooks BEFORE `task_editor_close_all` (a closing editor's flush otherwise
  cascades refreshes into destroyed windows).
- Async callback lifetime: never capture the TaskLibrary pointer in a
  worker/idle callback — re-resolve via `lib_of(app)` and no-op when
  NULL (the window may close mid-flight).  The settings window guards
  the same way (`settings != sw`).

## Task status (the tri-state that replaced `done`)

A task's completion is ONE field, `tasks.status` (`TaskStatus`: 0 New,
1 In Progress, 2 Done — the values are the on-disk encoding, do not
renumber).  Schema v7 added it, backfilled `done = 1` → Done, and
**DROPPED `tasks.done`**; the drop is conditional on the backfill's
`sqlite3_exec` having returned OK.  An sqlite too old for DROP COLUMN
(< 3.35) just leaves `done` behind unread, which is harmless.  There is
no `t->done` any more: every "is it complete?" test is
`t->status == TASK_STATUS_DONE`.

The third state is local — it never leaves this app.
`task_status_apply_done(cur, done)` is the single rule every
done-only source folds through — the ✓ column, the context menu's
Mark Complete/Incomplete, the subtask checkboxes:

- `done` → **Done**;
- `!done` → **In Progress** if it WAS Done (a ticked task has plainly
  been worked on), else `cur` **unchanged**.

**New is reachable only from the editor's dropdown.**

**Every status change stamps `updated_at`** — `task_db_task_set_status`
and `task_db_task_update` alike, New ↔ In Progress included.  Status is
a SYNCED field (even though sync is not in this build), so it does not
get the deliberate missing bump that `pinned`/`priority` get.  The known
cost, accepted deliberately (2026-08-25): a New ↔ In Progress move
dirties a row whose content is otherwise unchanged.  Don't "optimize"
this back into a conditional bump.

**`completed_at` is stamped on ENTERING Done and NOTHING ever clears
it** — it answers "when was this LAST completed?", and that stays true
after someone reopens the task.  An already-Done task keeps its stamp;
re-completing moves it forward.  The stamp is MONOTONIC:
`task_db_task_update` and `task_db_task_set_status` share a CASE that
stamps only on the transition IN; `task_db_task_recur_apply` does not
name the column at all (reopening for a repeat does not un-complete the
last one).
Two consequences: the editor shows the stamp for ANY task that has one,
reading "Completed <date>" while Done and "Last completed <date>" once
reopened (`COMPLETED_LABEL_DONE`/`_PAST`), and the task list's Completion
Date column shows a date on reopened rows too.

**A completed SUBTASK starts its parent**: `parent_started()` in db.c
moves the parent New → In Progress, and every write path that can
complete a task folds through it (`task_db_task_set_status`,
`task_db_task_update`), so the editor checkbox, the ✓ column, a
Kanban drag all get it without multiple copies of the rule.  It is ONE
guarded UPDATE (`WHERE id = (SELECT parent_id …) AND status = New`).
Only New → In Progress, so it never touches `completed_at` and cannot
cascade.  **A DONE parent is left Done.**
The parent's open EDITOR must be resynced (`editor_status_resync`) when
its own subtask list ticks something — `editor_save_now` reads the status
combo and writes it back, so a combo left reading New silently undoes the
promotion on the next debounced save.  That was measured, not assumed:
with the resync removed the parent comes back as New ~600 ms later.

## Due dates carry a time (schema v12)

`tasks.due` is still **DATE-ONLY** — always local midnight — and that is
not going to change: every day-bucketing view (Due Today, the Weekly
Forecast, `task_due_color`'s tint) compares against it.  The time of day
lives in a SECOND column, **`due_time`** (minutes past local midnight,
`TASK_DUE_TIME_DEFAULT` = 480 / 08:00).

- **There is no "has a time" flag**: every due date has a time, and the
  column's `DEFAULT` is the whole migration — existing rows come back
  meaning 08:00 without a single row being rewritten.
- `task_due_instant(due, due_time)` folds the two back into the MOMENT a
  task is due.  Use it for sorting; use `due` alone for bucketing by day.
  `task_db_tasks_due_between` sorts by `due, due_time`.
- **The default is left UNSAID in the list.**  `task_due_format_at` prints
  the time only when it is not 08:00.
- The editor's first row is `Due: [YYYY-MM-DD] at [HH:MM]`.  The time
  entry is **5 chars wide, not 6**: at 6 it measured 4 px past the 490
  the window asks for and silently grew every editor to fit.
  `editor_time_entry_parse` / `_set` are shared with the recurrence
  block's time entry.
- **The recurrence pass writes it**: `recur_due_of` splits an occurrence
  into `due` + `due_time`, so "every Monday at 9:00 AM" produces a due
  date that SAYS 9:00 AM.  `due_time` joins the pass's "has anything
  changed?" test, because an hourly schedule keeps the same DAY and the
  o'clock is then the only thing that moved.
- Known and accepted: changing ONLY the due time stamps `updated_at` and
  dirties the row.  Don't "optimize" it into a conditional bump.

## GUI rules (visual parity with Notes)

- Toolbar: a plain `GtkBox` with `GtkButton`/`GtkToggleButton` children,
  24 px PNG icons loaded by `task_app_icon_new`.  It is **ICONS ONLY** —
  every button carries a tooltip that says more than a one-word label
  would.  Layout (all left-packed): New Task and Delete Task, a
  separator, then the Sidebar toggle, the completed-visibility toggle,
  the Manual Sort toggle and the pane toggle.
  **ONE separator in the window's own block**, after the task pair: it
  separates the buttons that ACT on a task from the controls that change
  what the PANE SHOWS.  **THE TASK VERBS LEAD** because they are what the
  window is for.
  The pane toggle sits WITH the sort toggle rather than with the task
  pair: both change how the tasks are PRESENTED.  Its ICON names the
  action: **BOTH faces come from menu.png** (the bulleted list): upright
  it offers the LIST, turned a quarter turn CLOCKWISE it offers the BOARD.
  One image means the two faces cannot drift apart.  `kanban.png` was the
  board face before that and is now unreferenced.
  The Manual Sort toggle: **manual.png** (a gearstick) while sorting is
  AUTOMATIC, **automatic.png** (a gear selector) while manual sorting is
  in force.
  **The Sidebar toggle** wears **left-and-right.png**, a double-headed
  arrow, set ONCE and never swapped (the glyph is symmetric about its
  vertical axis).  `sidebar_ui_sync` updates only the tooltip and View
  menu label.
  **add.png is DERIVED from remove.png** (2026-09-14): its circled X is
  turned 45° CLOCKWISE and recoloured.  Both New Task buttons wear it.
  See Conventions above for the pixel-decomposition rule if it ever needs
  to be remade.
  The completed-visibility toggle (`show_completed`, default 1) shows
  hidden.png while completed tasks are visible and visible.png while
  hidden (the icon names the ACTION).
  The Manual Sort toggle (`task_list_manual_sort`, default 0) enables
  drag-reorder of the task pane: a ⠿ drag-handle column appears; order
  is persisted per view in `manual_order_list_<id>`, `manual_order_group_<id>`,
  `manual_order_all`, `manual_order_pinned`, `manual_order_today`.
  **Reading a saved order back is ONE function**, `row_order_permutation`
  — a hash lookup, O(1), returning a full permutation.  `on_delete_list`
  removes the deleted list's order keys; `on_sb_ctx_delete_group` does the
  same for a group's aggregate (both call `row_order_keys_drop`).
  Far right (after an expanding separator): the **search box**.
- **Searching the task pane** (`src/search.[ch]`, the toolbar's right
  edge).  The box FILTERS THE SELECTED VIEW IN PLACE.  Its SCOPE is
  whatever the sidebar has selected.  Searched: the task TITLE, its
  NOTES, and its subtasks' TITLES joined with newlines.  A subtask match
  surfaces its PARENT.
  Terms AND together; `"quoted words"` is one phrase, `-word` excludes.
  Matching is `g_utf8_casefold`.  A query with no usable term parses to
  **NULL** — the ONE test for "is a search active".
  The filter is applied in `refresh_tasks` BETWEEN the collection and the
  presentation, so the Kanban board gets the same filter as the list for
  nothing.
  **A search SUSPENDS hand-sorting** (`manual_sort_live`): saving the
  current order while filtered would discard every hidden row's position
  (same "ABSENCE NEVER DELETES" trap as sync).  The sort toggle greys and
  the ⠿ handle disappears.  `card_order_save` returns FALSE early while
  the board's STATUS half still runs.
  **Compact Controls CLEARS the search**: the toolbar goes away, and a
  filter still running with nothing on screen is worse than a control that
  does nothing.  Cleared by emptying the ENTRY so the drop goes through
  the one "search-changed" path.
- View menu, top to bottom: the **completed-visibility** item, the **sort
  toggle**, divider, the **sidebar** item, the **controls** item, the
  **pane** item.  The divider separates what the task PANE shows from what
  the WINDOW looks like.
  **There are NO check items.**  Every one of the five is an action item
  whose LABEL is what a click DOES.  The pairs live in `*_LABEL_TO_*`
  macros:
  `SORT_LABEL_TO_MANUAL`/`_AUTO`, `DONE_LABEL_TO_HIDE`/`_SHOW`,
  `SIDEBAR_LABEL_TO_HIDE`/`_SHOW`, `CTRL_LABEL_TO_COMPACT`/`_FULL`,
  `PANE_LABEL_TO_KANBAN`/`_LIST`.
  Three consequences bind all five: every handler FLIPS the flag rather
  than reading the widget; every re-labeller sets the label with NO handler
  blocking (`set_label` cannot emit "activate"); the label is written by
  the SINGLE APPLIER — `hide_done_icon_refresh`, `manual_sort_icon_refresh`,
  `sidebar_ui_sync`, `compact_layout_apply`, `task_pane_mode_apply`.
  Menus use `GAction`/`GMenu` + `GtkPopoverMenuBar` (Phase 2 of the GTK4
  port).  The `app.` namespace carries application-level actions; `win.`
  carries library-window actions.  Pairs are implemented as
  `hidden-when=action-disabled` pairs synced by `lib_menu_pair_sync`.
- Menu bar: **File** then **View**, built at construction.  **File** has
  exactly ONE separator, after New Task / New List… / Clear Completed
  Tasks.  **View** items are the same five as the toolbar.
- **Classic scrollbars**, not overlay indicators: the `gtk-overlay-scrolling`
  GtkSettings property is set to FALSE in `main()` before GTK initialises
  (`g_object_set(gtk_settings_get_default(), "gtk-overlay-scrolling", FALSE, NULL)`).
- Thin separator rules under the toolbar and above the status bar.
  Status bar: margins 8/8/3/3 and `label { font-size: 85%; }` on both
  labels — measured pixel-identical to Notes.  Event messages posted via
  `task_app_status()` hold for 3 s then fade out over 1 s (20 × 50 ms
  alpha steps via Pango markup); a new message resets the timer.
- Task list: alternating row stripes via a `GtkListItemFactory` bind
  callback.  Dimmed markup uses Pango `alpha`, NEVER a fixed gray —
  hardcoded grays are unreadable on the blue selection.  Due tint:
  overdue #c01c28, today #d19a00, ahead #26a269, set when the row binds
  (not at draw time — the tint rolls over at the next refresh).
  The **Status** column (New / In Progress / Done) defaults to **HIDDEN**
  (`col_status_visible`, default 0) — the ✓ column is the same field
  seen as a tick.  Right-clicking any column header shows a hide/show
  menu for the Done, Status, Due Date and Completion Date columns.
  Context menus on the task view use a `GtkEventControllerLegacy` at
  CAPTURE phase on the `GtkColumnView` (gotcha 33).
- Task-cell notes preview: gated on the first line with real content
  (`line_is_blank`, Unicode-aware).  The 120-char cap must land on a
  UTF-8 CHARACTER boundary (`g_utf8_find_prev_char`): a partial sequence
  anywhere in a Pango markup string makes `pango_parse_markup` reject the
  lot — the row then draws completely blank.  Every DB-sourced string
  entering that markup is escaped through `markup_escape_db`
  (`g_utf8_make_valid` then `g_markup_escape_text`).
- Sidebar: gray backdrop CSS; meta rows bold (Favorites ⭐️, All Tasks 🔮,
  Due Today ☀️, Weekly Forecast 🌤️).  Due Today optionally includes all
  past-due tasks via `due_today_show_overdue` (Settings → Appearance;
  default off).  Lists can be organized into named **list groups**:
  right-click the Lists header or a group → New Group / Rename Group /
  Remove Group; right-click a list → Move to Group / Remove from Group.
  Group expansion state is snapshotted in `lw->group_expanded` (GHashTable
  of group_id → bool).  **Selecting a group row shows its child lists'
  tasks AGGREGATED** (`task_db_tasks_in_group`; `virtual_view` = TRUE).
  List labels: `emoji + two spaces + name` when an emoji is set.
  Lists are ALPHABETICAL by default and drag-reorderable: sidebar list
  reorder goes through `task_db_lists_reorder`.
  The **Weekly Forecast** is its own panel, not rows in the task store:
  seven full-width day sections (Sunday–Saturday) stacked vertically,
  each a heading label + framed two-column tree view at natural full
  height.  Empty days show an inert dimmed "No tasks due" row (id 0).
  The Favorites row exists ONLY while something is pinned
  (`task_db_has_pinned`).
  Sidebar starts HIDDEN by default (`sidebar_visible`, write-through on
  toggle).
- Window size: tracked via `notify::default-width` / `notify::default-height`,
  persisted as `win_w/win_h` on clean close, restored at launch (980×640
  fallback).
- Model rebuilds: capture the scroll position and restore it idle-deferred
  (`scroll_keep_queue`) — clearing a store zeroes the scrollbar.
- Context menus are built per-popup as `GMenu` models shown via
  `task_app_menu_popup`; they are discarded after use (no leak).
- Task view is `GtkMultiSelection`; right-click INSIDE an existing
  selection keeps it, outside collapses to the clicked row (gotcha 36);
  context actions apply to the whole selection.
- Editor: 600 ms debounced write-through saves; status/pinned save
  immediately.  The first row is `Status: [dropdown]` … `Due: [entry] at
  [HH:MM]`.  **The due entry IS its own date picker** (a left click opens
  the calendar, `on_due_entry_press`), so there is no 📅 button beside it.
  Only BUTTON 1 is taken; every other button falls through.  The
  Recurrence block's `Starting:` entry works the SAME way.
  The Favorite / High Priority checkboxes are on a row of their own.  That
  second row also carries the read-only **completion date** at its right
  end (`editor_completed_refresh`, dimmed with Pango alpha): empty (not
  hidden) when it does not apply.
  The `GtkDropDown` status combo is sized from `theme_field_height` so its
  plate matches the entries beside it — re-measure under the user's theme,
  not only Adwaita (gotcha 32's lesson carries to GTK4: different widget,
  same potential mismatch).
  NEVER rewrite the due entry while it has focus; a save must not clobber
  a partial entry (`editor_due_entry_parse`).  Editors are singletons per
  task (`app->editors` gint64 keys).
- Editor foot row: an "Advanced ▾/▴" link at the left, then Save at the
  right in EVERY editor, with Cancel to its right only in the
  `task_editor_open_new` variant (order reads Save, Cancel left-to-right).
  **Cancel closes and tombstones the task** — Cancel drops the pending
  debounce and destroys the window BEFORE deleting.
- Advanced disclosure: Subtasks + Attachments + Recurrence live in
  `adv_box`, IN THAT ORDER, folded by default and expanded on open when
  the task already HAS any of the three.  Recurrence is LAST.
  Two entry points: `editor_advanced_reveal` shows the block and records
  `adv_height`; `editor_advanced_set` is the applier for a window already
  on screen and adds the resize, so a collapse gives back exactly the
  pixels the expand took.  **The OPEN path reveals BEFORE the window is
  presented and never resizes**, so the window is presented once at its
  final size.
  The block's height measures true before the show: a GtkBox counts only
  VISIBLE children.
  **The LINK's folded label is written where the label is BUILT**, not only
  by the appliers.  The two faces are the `ADV_LABEL_TO_SHOW` /
  `ADV_LABEL_TO_FOLD` macros (▾ unfolds, ▴ folds).
- Editor geometry: default size is **490 × -1** (natural height).  The
  notes scroller is pinned to **8 lines** via BOTH `min_content_height`
  and `max_content_height`, measured by laying out eight "X\n" lines in
  the view's own Pango context (+12 px slack).  Font metrics' ascent+descent
  is NOT the right measure — it omits the inter-line gap.
- **Editor window resize expands upward on macOS** (gotcha 39): AppKit
  anchors `NSWindow` at bottom-left; `gtk_window_move` was removed in
  GTK4, so the GTK3 resize+reposition workaround is not available.
  Accepted as a macOS/GTK4 limitation; X11/Wayland anchor top-left
  correctly.
- Emoji picking: a bare 18 px single-char entry; click clears it and
  emits `insert-emoji`.  In GTK4 the popover renders outside its toplevel
  as its own surface (unlike GTK3 — see removed gotcha 1), so the dialog
  does NOT grow to fit it.
- **Compact Layout** (`compact_layout`, default 0) hides the whole
  toolbar and shows `float_bar` instead: a two-button pill (New Task +
  Delete Task, icons only) as a `GtkOverlay` child over the paned with
  halign/valign END and 20 px end/bottom margins.  Its plate is themed
  through `themed_bg_css_apply` (`float_bar_css`), NOT hardcoded.
  `compact_layout_apply` is the single applier.  Compact does not affect
  the sidebar.

## Kanban board (the THIRD task-pane variant)

**The board's state is GROUPED**: all its fields live in a nested
`struct { … } board;` inside `TaskLibrary`, so every use reads
`lw->board.…`.

`kanban_view` (default 0; View → Kanban View) renders the current view's
tasks as a board.  Three side-by-side lanes, homogeneous, in a
`GTK_POLICY_NEVER` horizontal scroller so the board always fits the pane.

- **Lane INDEX IS the `TaskStatus` value.**  Don't reorder the lanes
  without reordering the enum.
- `task_pane_mode_apply` is the SINGLE place that answers "which pane is
  on screen".  **Weekly Forecast OUTRANKS Kanban.**
- The task COLLECTION in `refresh_tasks` is shared; only the presentation
  branches (`refresh_kanban` vs `append_task_rows`).
- **The drag is HAND-ROLLED — GTK DnD (`GtkDragSource`/`GtkDropTarget`) is
  NOT used.**  A `GtkGestureDrag` on the card's grip handle provides an
  implicit pointer grab; the ghost is a `GtkWindow` overlay child (set
  `gtk_widget_set_opacity` for translucency — on a non-toplevel in GTK4
  this is a render-node opacity, not a compositor feature); the cursor
  changes via `gtk_widget_set_cursor`.  This is in-process, no native
  drag surface, and behaves identically on X11/Wayland/macOS.
- The engine: pressing the grip ARMS; `GtkGestureDrag`'s "drag-begin"
  fires once the threshold passes.  `card_drag_stop` is the ONE way out —
  release, Escape, a cancelled gesture, and window teardown all funnel
  through it.  `on_library_destroy` calls it too.
- The ghost is a GtkWindow overlay child positioned over the GtkOverlay
  covering the paned.  It inherits the display scale and stays sharp on
  HiDPI.  Translucency via `gtk_widget_set_opacity` (a render-node opacity
  in GTK4 — not a compositor-only feature like it was in GTK3 — so this
  works reliably including on X11 without a compositor).
- The LANDING INDICATOR is: a `.task-lane-target` CSS class on the lane
  (which column), and a `.task-card-mark` bar inserted into that lane
  (which slot).  The dragged card keeps its place, dimmed with
  `.task-card-dragging`, and is NOT hidden — hiding it would cancel the
  gesture.
- The marker is rebuilt only when (lane, slot) CHANGES to avoid shuffling
  cards continuously.  `refresh_kanban` NULLs the pointer without freeing
  it (the rebuild destroyed it with the lane).
- The drop reads its slot from the MARKER, not by re-measuring.
- **CARD ORDER** lives in ONE config key per view
  (`kanban_order_<view>`, `kanban_order_key`), comma-separated id list,
  lane by lane.  `kanban_order_apply` runs in `refresh_kanban` BEFORE the
  tasks are handed to lanes.
- `card_drop_apply` has two independent halves: the STATUS (stamps
  `updated_at`) and the ORDER (local-only config).  A same-lane drop is a
  NO-OP.
- Drops hit-test in ROOT coordinates against `lw->board.kanban_drops[]`
  (`card_lane_at_root`) via `gtk_widget_compute_point`.
- The refresh after a drop is `g_idle_add`-DEFERRED: the drop runs inside
  the dragged card's own handler and a synchronous refresh would destroy
  the widget currently executing.
- **The lane IS the content box** (plain `GtkBox` — no `GtkEventBox`
  wrapper in GTK4).  It is packed `expand=TRUE` so a short lane is still
  a target all the way down.  Inner padding (between the lane border and
  its cards) is CSS `padding:` on `.task-lane`, NOT `pad_widget(lane, ...)` on the
  GtkBox itself — `pad_widget` only adds outer margin (gotcha 40).
- `on_card_press` returns FALSE for a single click so the press keeps
  propagating; a double-click opens the editor (gotcha 35).
- `lw->board.kanban_sel` is the board's selection answer; `selected_task_ids`
  returns it while the board is up.
- `show_completed` applies as it does everywhere: with completed hidden,
  the Done lane empties but stays as a drop target.
- **Corners are SQUARE**, matching the forecast.  No `border-radius` in the
  board's CSS.
- **Each card CASTS A DROP SHADOW.**  In GTK4, `box-shadow` on a plain
  `GtkBox` DOES paint correctly (render-node renderer, no GdkWindow
  clipping — the GTK3 `GtkEventBox` limitation is gone).  Use `rgba()`
  for the shadow colour — the GTK-CSS `alpha()` extension does NOT parse
  inside `box-shadow` values (gotcha 38).  The shadow spec is
  `2px 2px 3px -1px rgba(0,0,0,0.28)` (negative spread keeps the blur off
  the top and left edges; the blur is done by GTK's own CSS renderer, CPU
  work, ~1 ms per repaint of a lane).  The setting **`kanban_shadow`**
  (default 1, Settings → Appearance) hands that millisecond back on a
  machine that wants it.
- Inner spacing is WIDGET MARGINS on children (`pad_widget`, CARD_PAD 8).
- **A card has a ⠿ GRIP down its left edge, and that is the ONLY place a
  drag starts.**  A `GtkGestureClick` on the grip handles press/release;
  `GtkGestureDrag` provides the drag.  `on_card_press` returns FALSE so
  selection and double-click still work on the grip.
- Cursors: `"grab"` over the grip, `"grabbing"` while dragging.  Both
  made ONCE and cached (`card_grab`/`card_grabbing`).  The hover cursor
  goes on the grip widget via `gtk_widget_set_cursor`.
- **Multi-select**: plain click selects one, MODIFY_SELECTION-click
  toggles, EXTEND_SELECTION-click takes the run within one lane.
  Modifiers come from `gtk_widget_get_modifier_mask` (Ctrl on X11, Cmd on
  quartz).  `refresh_kanban` prunes the selection to surviving ids.
- A drag carries the WHOLE selection when the gripped card is part of it.
  The ghost paints a count badge when more than one is in flight.
- **`vexpand=TRUE` propagation**: watch for it on card children — a leaf
  widget with vexpand=TRUE cascades up its ancestor chain and can make the
  lane fill the window (gotcha 37).
- The board's CSS is installed ONCE for the screen (`kanban_css_install`),
  not per widget: every color is a named theme color, so GTK re-resolves
  on a light/dark switch.

## Recurring tasks (schema v10, `recur_start` at v11)

A recurring task is an ORDINARY task carrying a schedule — the six
`recur_*` columns on its own row.  There is NO template row, no generated
series and no second row type.

**THE RULE, in one sentence**: a set interval BEFORE each occurrence, a
task that has been COMPLETED is reset to New and its due date is moved
to that occurrence.  `src/recur.[ch]` owns it; the editor's Advanced
block is the only place it is set.

- `recur_interval` + `recur_unit` are "every N units"
  (`TaskRecurUnit`: 0 minute, 1 hour, 2 day, 3 week, 4 month, 5 year —
  the values are the on-disk encoding, in ASCENDING DURATION ORDER, which
  is what makes `recur_dated()` one comparison; do not renumber).
  `recur_interval = 0` means "does not recur".
- **There are NO PRESETS** (2026-09-08).  Every schedule is written the
  one way, "every N units".  Don't reintroduce them.
- `recur_time` is minutes past local midnight, **480 (08:00)** by default,
  and is the ANCHOR'S TIME OF DAY for **every** unit.
- **`recur_start` (v11) is the schedule's ANCHOR DAY.**  0 = unset, and
  unset is the ordinary case (falls back to the task's due date, then
  today).  An anchor still in the FUTURE is itself the first occurrence.
- The Recurrence block opens with a **MASTER SWITCH**, "Repeat this task".
  It holds **NO STATE OF ITS OWN**: ticked means `recur_interval > 0`.
  Everything below the switch lives in ONE box, `recur_body`, **hidden,
  never greyed**.  **NOTHING IN THE BLOCK IS EVER GREYED, and no row comes
  and goes.**
- **Two rows and one sentence**: `Starting [YYYY-MM-DD] at [HH:MM]` then
  `Repeat every [N] [unit]`.  Their three leading labels share a
  `GtkSizeGroup` so the controls start at the same x.
- The summary is TWO LINES: `task_recur_phrase` then "Next … — resets to
  New …".
- `recur_lead` is minutes, clamped by `task_recur_lead_seconds` to a
  minute short of one period.  The clamp test is `lead >= period` — not
  `period > 60 && lead > period - 60`, which leaves the ONE-minute period
  unclamped.
- `recur_next` is BOOKKEEPING, not a setting.  Written by
  `task_db_task_recur_set_next` with **no** `updated_at` stamp (local-only).
  A real roll-forward (`task_db_task_recur_apply`) DOES stamp.
- **Occurrences are SKIPPED, never replayed.**  `recur_catch_up` finds
  the LAST occurrence already due.
- ALL the date arithmetic is GDateTime (`task_recur_advance`).  Nothing
  adds 86400 to a timestamp.  Known: a month clamp is STICKY.
- **There is NO SETTING for any of this** — no `recur_enabled`, no
  `recur_check_min`.  Don't reintroduce either key.
- **It is NOT PERIODIC.**  `task_recur_pass` ends by arming a ONE-SHOT
  `g_timeout_add_seconds` for the earliest fire time (`recur_arm_deadline`).
  `RECUR_MAX_SLEEP_SEC` (900) caps the sleep as a safety net against
  suspend/clock-step lateness.
- The pass is the only registered worker; it runs ON THE MAIN THREAD.
  `INITIAL_ALWAYS`, so repeats that came round while the app was closed
  are applied at launch.
- Recurrence is LOCAL-ONLY.
- The schedule rides `task_db_task_update` (the editor's debounced save).

## Settings: the Database section

- The health PLATE (verdict + LED, path, counts, size, SHA-256) with
  **Update** under it, then the rotating-backup controls.
- The rotating-backup block reads TOP DOWN: the switch, then **Every N
  minutes, keeping M files**, then **Backing up to `<folder>`**, then the
  two buttons (change folder, force a pass).
- **The ⚠ tests CONTAINMENT, not equality** (`dir_shares_fate`): the
  default destination is INSIDE the database's directory.
- **That destination line is ONE SENTENCE naming the RESOLVED folder**,
  no "(default)" variant: backups always go somewhere.
- **The two spin buttons go through `small_spin`**: `gtk_entry_set_width_chars`
  alone barely moves a GTK4 spin button (Adwaita floors `min-width`); the
  CSS removes the floor, the char count decides the width.
- **The button row's RIGHT EDGE is the Update button's**: both carry
  `margin_end` 12 and hug the right.
- **EVERY push button comes from `small_button`** — appearance is
  consistent across the settings sections.

## Data safety (read this before touching the database file)

A 1965-task production database was destroyed on 2026-08-26.  These rules
are the post-mortem; none of them is optional.

- **"It opened" is NOT "it is intact."**  SQLite opens a malformed file
  happily.  Anything that copies, moves or migrates the database must
  check with `task_db_verify_file` (integrity_check + foreign_key_check
  on its own read-only connection) — never by opening it.
- **Copy with `task_db_copy_file` (VACUUM INTO), never a byte copy.**  It
  runs in a read transaction, so it cannot capture a torn page.
- **COPY → VERIFY → and only then delete anything.**  A copy that fails
  verification is left in place and reported, and the original is kept.
- **A migration backs the file up FIRST.**  `task_db_open` writes
  `<db>.pre-v<N>.bak` via VACUUM INTO before running any migration.
- **Assume the database file can be REPLACED, EVICTED or RE-GENERATED
  underneath an open connection.**  The database lives on local disk
  (`~/.local/share/tasks/tasks.db`) and the BACKUPS are what go to
  iCloud/a remote folder; the live file must never be in a synced folder.
- **Anything that opens a DIFFERENT database re-arms the backup timer**
  through `task_worker_arm_all`.
- The optional rotating backup (`backup.[ch]`, off by default): own worker
  + connection, VACUUM INTO, verify, and prune ONLY after a new backup
  verifies.  Matches only `tasks-*.db` filenames when pruning.
  `task_backup_dir` is the single answer to "where": `backup_dir` when
  set, else `backups/` INSIDE the default database directory
  (`BACKUP_SUBDIR`), created on demand.

## Hard-won gotchas (do not re-learn)

2. `g_clear_pointer` is a statement-style macro; it cannot sit inside
   an expression.
4. Clearing a GListStore can zero the view's scrollbar (restore
   idle-deferred) and collapse sidebar expansion state (snapshot before
   clear).  Re-measure with GListStore splice to see if `scroll_keep` is
   still needed.
6. Status-bar height parity needs margins (8/8/3/3), not
   `border_width` — border pads every edge.
7. libcurl's implicit global init is not thread-safe; init in main().
   (Not relevant for this build — no libcurl — but the backup worker uses
   threading; same principle.)
8. sqlite `UPDATE` SET expressions read the OLD row values — the
   `completed_at CASE WHEN ?done=1 AND done=0` transition relies on it.
10. macOS AX geometry (osascript) reports frame incl. titlebar;
    `gtk_window_get_size` is the client area (~28 pt difference).
11. The live ini rewrites drop comments and carry per-machine values —
    it stays gitignored; document defaults in `tasks.ini.defaults`.
13. Google's DEFAULT tasklist cannot be deleted (not in this build, but
    recorded for when sync returns: `tasklists.delete` returns 400, so
    handle that tombstone case explicitly).
14. **There is NO "move the database somewhere else" flow, and there must
    not be one again** (removed 2026-09-09).  The database lives at
    `task_db_default_path()`; a different database is OPENED, through
    File → Open Database File….
16. A `CREATE INDEX` naming a column added by a guarded `ALTER` must run
    AFTER the migrations, never inside the schema block at the top of
    `task_db_open`.  On an EXISTING file the column does not exist yet at
    that point, the statement fails, and — because it rides in the same
    batch — it takes the rest of the schema setup with it.  Fresh files
    hide this completely.
17. A Notes CLI call is answered by whichever Notes instance owns
    `~/.cache/notes.sock` — an old GUI started before the rename owns the
    old path.  (Not in this build, recorded for when the integration
    returns.)
19. GTK DnD delegates to the platform, so what a drag looks like is not
    yours to decide through it.  This is the reason the board and the
    manual-sort row drag are hand-rolled: `GtkGestureDrag` provides an
    implicit grab that is in-process, identical on X11/Wayland/macOS.
21. **TEST A FRESH DATABASE, not just the one on this machine.**  The
    `CREATE TABLE tasks` schema block must be consistent: a duplicate
    column name makes SQLite reject the whole statement and leaves the app
    with no `tasks` table.  This is invisible on an existing file (the
    `IF NOT EXISTS` is a no-op).  Use `make run-dev` (which seeds a
    zero-byte `tasks.db`) or open an explicit empty file.
22. A plugin's SIDE TABLE is created twice on purpose (not relevant to
    this build, but `IF NOT EXISTS` makes the pair harmless if it recurs).
23. GLib's `g_date_time_format("%l")` pads with **U+2007 FIGURE SPACE**,
    not an ASCII space.  `g_strstrip` is ASCII-only and leaves it.
    Use `%I` and strip the zero yourself.
24. A CORE column added after the fact needs BOTH halves: the declaration
    in `CREATE TABLE` (fresh files) and a guarded `ALTER TABLE … ADD COLUMN`
    after the migrations (existing files).  Guard the ALTER on
    `table_has_column`, not the version number (idempotent and version-stamp
    independent).  Build the SQL from a macro so the DEFAULT is not spelled
    twice.
25. A C HEX ESCAPE IS GREEDY: `"\xe2\x80\x9capple"` reads as `\x9ca`.
    Fix by ending the literal after the escape:
    `"\xe2\x80\x9c" "apple"`.
27. **GLib's `_local` DATE CONSTRUCTORS RESOLVE THE TIMEZONE EVERY CALL**
    (~7300 ns each on GLib 2.88, against ~175 ns with a cached zone).
    Use `task_local_tz()` and `task_local_dt()` — never a `_local`
    constructor.  Caching the zone does NOT affect DST.
28. **MEASURE A CONTAINER ONLY ONCE ITS CONTENTS ARE FINAL.**  A wrapped
    label inside it reports the height of whatever it currently says; set
    the text first, measure second (`editor_recur_refresh` ends with
    `editor_recur_body_set` for this reason).
31. **`g_ptr_array_sort` HANDS ITS COMPARATOR POINTERS TO THE ELEMENTS**
    (`gchar **` for an array of strings), so
    `g_ptr_array_sort(a, (GCompareFunc)g_strcmp0)` compares pointer values.
    The cast is what hides it; the sort looks plausible and is arbitrary.
    Fix: a comparator that dereferences, or `g_ptr_array_sort_values`.
    **Never cast a function to `GCompareFunc` to make a sort compile.**

### GTK4-specific gotchas

33. **Secondary button events never reach factory-created leaf widgets in
    `GtkColumnView`.**  `GtkColumnView`'s internal row/cell widget tree has
    its own CAPTURE-phase gesture machinery that consumes secondary button
    events before they reach the factory widgets.  Fix: attach a
    `GtkEventControllerLegacy` at CAPTURE phase on the `GtkColumnView`
    itself (`on_task_view_event`, committed 6b37ba4).  Identify the clicked
    row by calling `gtk_widget_pick` with the event's surface coordinates,
    then walking ancestors for `"task-rclick-item"` object data.
    `gdk_event_get_position` returns surface/window coords; convert to
    view-local with `gtk_widget_compute_point(lw->window, view, ...)` for
    the pick call.

34. **`GtkGestureClick` "pressed" signal output does not appear in a
    `nohup … 2>&1` log on macOS.**  GTK4's quartz backend redirects
    stderr to the macOS Console.  Use Console.app when debugging event
    handling on macOS; the nohup log gets application output only.

35. **Do NOT use `GtkColumnView`'s "activate" signal for double-click
    detection.**  On macOS quartz, the system timestamps motion events with
    held buttons, which resets `GtkGestureClick`'s `n_press` counter,
    making double-clicks unreliable or impossible.  Use
    `task_app_double_click_watch` pattern instead.  Also cap
    `gtk-double-click-time` at 400 ms: macOS's accessibility preference can
    push it to 750 ms+, making the double-click window too wide
    (commit ffad70c).

36. **`GtkMultiSelection` + `GTK_INVALID_LIST_POSITION` silently wipes the
    selection.**  Calling
    `gtk_selection_model_select_item(model, GTK_INVALID_LIST_POSITION, TRUE)`
    calls `unselect_all` first, then fails to select — the entire selection
    is gone with no error or warning.  Always guard:
    `if (pos != GTK_INVALID_LIST_POSITION)` before every `select_item` call.

37. **`vexpand=TRUE` on a leaf widget propagates up the ancestor chain.**
    In a `GtkBox`, if any descendant reports `vexpand=TRUE`, the box
    reports it to its own parent, causing each ancestor to fill its
    container.  Break the chain with
    `gtk_widget_set_vexpand(container, FALSE)` on the immediate parent you
    do not want to expand.  This bit the Kanban card grip centering
    (commit 87ead14).

38. **GTK-CSS `alpha()` extension does NOT parse inside `box-shadow`
    values.**  `alpha(@theme_fg_color, 0.40)` silently produces no shadow.
    Use `rgba(r, g, b, a)` instead — `rgba(0,0,0,0.28)` works.
    `box-shadow` on a plain `GtkBox` DOES paint in GTK4 (render-node
    renderer; no GdkWindow clipping — the GTK3 limitation is gone; commits
    87ead14, dc32149).

39. **GTK4 editor window resize expands UPWARD on macOS.**  AppKit anchors
    `NSWindow` at its bottom-left when resizing programmatically.  GTK4
    removed `gtk_window_move`, so the GTK3 workaround (resize then
    reposition) is not available.  Accepted as a macOS/GTK4 limitation;
    X11 and Wayland anchor top-left correctly.

40. **In GTK4, lane inner padding requires CSS, not `pad_widget()` on the
    lane box.**  The lane IS the content `GtkBox` (there is no `GtkEventBox`
    wrapper).  `pad_widget(lane, LANE_PAD)` adds only a widget MARGIN,
    which is outer spacing.  Inner padding — between the lane's border and
    its first card — requires `padding:` in the `.task-lane` CSS rule.
    (commit 87ead14.)
