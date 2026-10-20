# Tasks AppKit — interfaces and rules for the UI work packages

This is the contract for the six engineers porting the UI.  It describes the
tree as it stands on branch `appkit` after the foundation work.  Headers in
`src/` carry the full detail of every function; this document says how the
pieces fit, who owns what, and the rules everyone follows.

Read with it:

- `porting/GTK_PORTING_MAP.md` — what the GTK app does (the parity checklist
  is §3).  Its file paths now mean `porting/gtk_reference/`.
- `porting/FILES_PLAYBOOK.md` — house conventions and AppKit lessons (§1, §3,
  §5 above all).
- `porting/gtk_reference/` — the GTK sources: the behavioural specification.
  Not compiled.  Deleted when the port is integrated.  The `.md` files there
  describe the GTK app and are stale in places (map §8.2); where they and
  this document disagree, this document wins.

---

## 1. Hard rules

1. **Never touch the live data.**  The GTK app's data is in
   `~/.local/share/tasks/`.  Do not open, read, list or write anything there —
   not from code, not from a shell, not "just to look".  The code refuses:
   `task_db_open`, `task_db_verify_file`, `task_db_file_sha256`, the config
   loader and the backup folder all reject any path inside it
   (`src/task_paths.h`), whether it gets there as written, through
   `..`, or through a symbolic link.  Do not weaken or bypass the fence; a
   file chooser that lands there must show the refusal, not work around it.
   The fence knows that directory by its LOCATION only: it cannot recognise a
   GTK database kept somewhere else.  Anything that opens a database the
   user picks (Open Database File…) needs the owner's decision first (§15).
2. **Every build, run and test uses the sandbox.**  `make run`, `make seed`,
   `make test`, `make ui-check` set `TASKS_DATA_DIR` to a directory under
   `dev/` (gitignored) and `CAMBIO_SHARED_DB` to a database file inside it.
   Never run the binary by hand without BOTH set: a plain launch uses
   `~/Library/Application Support/Tasks/` and the shared database Notes
   uses too (`~/Library/Application Support/camb.io/`), which are the
   installed apps' real data.  The rules of the shared database
   (`../shared_data/PLAN.md`) apply as well.
3. **All persisted state goes through `task_config`.**  No `NSUserDefaults`,
   no `frameAutosaveName`, no `restorable` windows: they would write outside
   the data directory and escape the sandbox.
4. **Stay in your files** (§10).  If you need a change in a frozen header or
   in someone else's file, ask the integrator; do not fork a near-copy.
5. No remotes, no pushing.  Work on your own branch off `appkit`.

---

## 2. Layers and names

```
src/*.c, *.h      the core: plain C + GLib (no GTK, no AppKit, no Foundation)
src/*.m, *.h      the AppKit layer: Objective-C, ARC, written in a C style
tests/*_test.c    core tests (link the core objects only)
tools/seed.c      writes the sandbox test database through the core API
tools/ui_check.m  builds the real windows in-process: snapshots, ownership, memory
```

The Makefile compiles every `src/*.c` as core and every `src/*.m` as UI.  A
`.c` and a `.m` must not share a base name.

| Prefix | Meaning |
|---|---|
| `task_…`, `Task…` types, `TASK_…` | the core (carried names: `Task`, `TaskApp`, `TaskTag`, …) |
| `task_…_t` | NEW core types written for the fork (`task_row_t`, `task_sel_t`) |
| `tasks_…` | AppKit-layer C functions (`tasks_string`, `tasks_row_text`); `shared_ui_…` / `shared_look_…` / `Shared…` are the module shared with Notes |
| `Tasks…` | Objective-C classes, `NS_ENUM` types and their members |
| `tasks_look_…` | look constants |

**GLib stays in the core this phase** and is removed in a later one.  So:

- Core functions still take and return `gchar *`, `GPtrArray *`, `gboolean`,
  and those returns are freed with `g_free` / the `task_ptr_array_free_*`
  helpers, as their headers say.
- **Do not add GLib usage in new code.**  New C uses `<stdbool.h>`,
  `<stdint.h>`, `malloc`/`free`; Objective-C uses Foundation.  The new core
  modules (`task_paths`, `task_loop`, `task_pane`, `task_order`, the row
  description in `task_rows`) already follow this; their strings are freed
  with `free`.
- Never use GLib's main loop, timers, idles or threads (`g_timeout_add`,
  `g_idle_add`, `GThread`, `GMainLoop`).  Nothing runs a GLib main loop under
  `NSApplication`; such a timer silently never fires.

### Core modules

| Module | What it is |
|---|---|
| `task_paths` | the data directory for the ini, attachments and Google client file (`$TASKS_DATA_DIR`, else `~/Library/Application Support/Tasks`) and the fence round the live data |
| `task_config` | the settings: `<data dir>/tasks.ini`, one registry of keys and defaults |
| `task_app` | `TaskApp` (the open database + the event bus) and the outside-change watch |
| `task_loop` | timers, main-queue hand-back, background work, on libdispatch |
| `task_dates` | local-day arithmetic, due-date formats and parsers, `task_due_urgency` |
| `db` | Tasks' queries over the shared database; the schema, stamps, WAL, path and backups are the shared module's (`shared_db.h`, from `../shared_data`) |
| `search` | the search box's query language (unchanged) |
| `recur` | recurrence arithmetic and the deadline-armed pass |
| `backup` | backups through the shared module, on a background job; policy in `shared_settings` |
| `task_worker` | the scheduler the recurrence pass and the backup are registered with |
| `task_view`, `core_views` | the registry of sidebar views (Favorites, All Tasks, 3-Day Forecast, Action Items) |
| `task_ops` | clear-completed (a tag's), the default tag (`task_ops_default_tag`) |
| `task_rows` | what a task row SAYS, as toolkit-neutral data |
| `task_pane` | the sidebar selection type; collects a selection's rows + status line |
| `task_order` | saved manual and Kanban orders |

---

## 3. Threading

- **Everything is main-thread only** unless its header says otherwise: the
  `TaskApp`, its database connection, `task_config`, `task_dates` (its
  timezone cache), every AppKit object.
- A SQLite connection never crosses threads.  Background work that needs the
  database opens its own with `task_db_open_existing`, as the backup and
  the gcal sync do: only the main connection, at launch or "Create New
  Database", may create the file (`task_db_open`), so a worker never makes
  an empty database where the user moved the real one aside.  Its commits
  bump `data_version` for the main connection, so they reach the windows
  through the outside-change watch (§4) like any other app's.
- Background C work: `task_loop_background(fn, ctx)`; it hands its result back
  with `task_loop_main_async(fn, ctx)`.  It must copy every setting it needs
  BEFORE it starts (see `task_backup_start`).
- Objective-C may use GCD and blocks directly (playbook §3.14–3.15): capture
  every input as locals, hop back with
  `dispatch_async(dispatch_get_main_queue(), …)`, guard against stale results
  with a generation counter, and wrap a background block's body in
  `@autoreleasepool`.
- Timers in the core: `task_loop_timer_once` / `_every` / `_cancel`
  (ids: 0 = none, never reused, cancelling a dead id is a no-op).  UI timing in
  Objective-C uses `dispatch_after` with a superseding id, as
  `show_status_message:` does in `library_window.m`.

---

## 4. Events and refresh

`TaskApp` carries four events (`src/task_app.h`):

| Fire | Meaning | Who fires it |
|---|---|---|
| `task_app_notify_changed` | structure changed: refresh sidebar + task pane + open editors | the recurrence pass |
| `task_app_notify_tasks` | tasks changed, sidebar cannot have: task pane only | editor saves, subtask/attachment edits, Settings changes that alter what rows show |
| the notes event (`task_app_listen_notes`) | Notes' data changed elsewhere: re-read open editors' "From note" lines; while `TASK_CFG_NOTE_CONTEXT` is on, also everything the tasks event refreshes (a renamed folder renames the prefix) | `task_app_check_outside` only |
| `task_app_status(app, fmt, …)` | a one-line message for the status bar | anyone |

**Outside changes.**  The database is shared with Notes (and with scripts).
`task_app_check_outside` calls `shared_db_poll` on the main connection; when
another connection has committed, it fires `task_app_notify_changed` if the
tasks revision moved, else the notes event if the notes revision moved.  It
runs every second (`task_app_watch_start`) and when the app becomes active.
The main connection's own writes never count: after each of its commits it
polls again and adopts the revisions, unless another connection committed
too, in which case the moved revisions stay pending for the next check.  The refresh keeps selection
and scroll where the same tasks remain; an editor with a pending save saves
it first (only its changed fields) and then reloads.

**Writes name their columns.**  Never write a whole task row back:
`task_db_task_update(db, before, after)` writes only the editable fields in
which `after` differs from `before` (the row as loaded), so another app's
change to a field the user did not touch survives.  The other writers are
single-purpose setters (`task_db_task_set_status`, …).

The library window is the listener.  It drops its four subscriptions FIRST
when it shuts down, before the editors close, because a closing editor's final
save fires the task event.

Inside the library window's own code paths, call its refresh methods directly
(`refresh_tasks`, `refresh_sidebar`, `full_refresh`); from anywhere else
(editor, Settings, core) fire the event.

**Refresh-while-handling hazard.**  A refresh rebuilds views.  If the code
asking for it is running inside a handler of a view the refresh will destroy
(a card's mouse-up, a cell's action), defer it one run-loop pass:
`dispatch_async(dispatch_get_main_queue(), ^{ … })` with a weak window.

---

## 5. The library window and its panes

`src/library_window.h` is the contract.  In short:

**The window owns the shared state; each has one writer.**

| State | Reader | Single writer |
|---|---|---|
| `selection` (`task_sel_t`: view / tag + id) | everyone | `select:` (called by the sidebar and the Favorites bar) |
| `search` (`const TaskSearch *`, NULL = no filter) | everyone | the search field's action |
| `manual_sort` | task list | `TasksCommandToggleSort` |
| `kanban_on` | shell | `TasksCommandTogglePane` |

Panes read these through the window and never keep copies.

**The window owns the refresh.**  `refresh_tasks` calls
`task_pane_collect(app, selection, search)` — the tasks of the selection,
filtered by the search, completed ones left out when the setting hides them,
described as rows — sets the status bar's count, and hands the rows to the
visible pane with `show_rows:`.  A pane never queries the database for the rows
it shows.

**The pane protocol** (`TasksTaskPane`, implemented by `TasksTaskList` and
`TasksKanban`):

| Method | Contract |
|---|---|
| `show_rows:` | take OWNERSHIP of the row list (free the old one with `task_row_list_free`); NULL = the pane is being hidden, drop everything.  Apply your saved order; re-read the settings you draw by; keep selection and scroll where the same tasks remain |
| `selected_task_ids` | what every task command acts on, in display order |
| `view_changed` | the sidebar selection moved: drop selection, scroll, "show all completed" |
| `cancel_drag` | abandon a drag in progress |
| `shut_down` | free rows, stop timers, remove observers; idempotent |

**Selection is pane-polymorphic.**  Every task command acts on
`[library selected_task_ids]`, which asks whichever pane is visible.  No
command knows which pane is up.

**Context menu.**  Both panes show the same menu: make the clicked row/card
part of the selection (inside the selection keeps it, outside collapses to it),
then use `[library task_context_menu]`.

**Saved orders** (`src/task_order.h`).  The list uses family
`TASK_CFG_FAMILY_MANUAL`, the board `TASK_CFG_FAMILY_KANBAN`.  **Never save an
order while a search is active** — `[library manual_sort_live]` is the test;
the list hides its drag handle, the board skips the order half of a drop (its
status half still runs).  Reading an order is always allowed.

**There are no lists.**  Tags are the only organisation (db.h): every live
task carries at least one, the schema keeps that and deletes a tag no task
carries; the shared `lists` / `list_groups` tables are storage only.

**The sidebar** (`src/sidebar.h`) shows the views, the Tags header and every
tag; it reports selection changes with `[library select:]`; `reload`
rebuilds from the database and must leave exactly one row selected (never
the Tags header), moving the selection to the first view if its row has
gone (`task_sidebar_pick`).  It owns Edit Tag (emoji, favorite; never the
name).  **The Favorites bar** (`src/favorites_bar.h`) shows one button per
favorite tag and selects through `[library select:]` too.  The window
rebuilds both when the tags change (`tag_rows`: ids, emojis, favorites),
and drops a gone tag's saved orders (`task_order_drop`).

---

## 6. The row display contract

- `task_rows.h` decides the CONTENT and EMPHASIS of a row: lines, each with a
  glyph `prefix`, `text`, and flags `bold` / `italic` / `strike` / `small` and
  `alpha_pct`.  It decides nothing about fonts or colours.
- `row_text.h` is the ONE renderer: `tasks_row_text(row, emphasized, layout)`
  returns the attributed string; `tasks_row_text_height(text, width)` measures
  it.  The list uses `TasksRowLayoutWrap`, the cards `TasksRowLayoutTruncate`.
- **Neither pane formats row text itself.**  If the look of a row must change,
  change `row_text.m` (shell package) so both panes change together.
- **Titles go through `task_display_title(task, with_context)` (db.h)** —
  the ONE place the note-context prefix ("<context> - <title>",
  `TASK_CONTEXT_SEPARATOR`) is made.  `Task.context` is the
  `action_items_ctx.context` of the task's note (its folder's name), read
  with every task row by `TASK_COLS`; "" for anything else.  The rows
  (`task_row_describe`: list, Kanban, 3-Day Forecast, any tag) and the search
  (`task_search_matches`) take the setting from `TaskRowCtx.note_context`;
  the AppKit labels (day planner blocks, reminders, the subtask editor's
  parent line) use `tasks_title_string` (ui_util.h).  Display only: never
  for an editable field, never written to `tasks.title`, never pushed to
  Google Calendar (the sync pushes `t->title`).
- Dimming is alpha on the base colour, never a fixed grey.  Pass
  `emphasized:YES` for a selected row in a key window.
- To update one row in place (the checkbox): re-fetch the task, build a
  `TaskRowCtx` with `task_row_ctx_init(app, &ctx, task_sel_is_virtual(sel))`,
  `task_row_describe`, replace the row, reload that table row and its height.
- Core text → `NSString`: always `tasks_string()` (`ui_util.h`), never `@()` or
  `stringWithUTF8String:` (they return nil for invalid UTF-8, which a
  hand-edited database can hold).

---

## 7. Commands

One table (`src/commands.m`) drives menu items, toolbar items and validation.

- Every control made from it sends `run_command:` with the command in `tag`
  and an optional parameter (an `NSNumber`) in a menu item's
  `representedObject`.
- Main-menu items target the app delegate (so they work from any window); it
  forwards to the library window.  Toolbar and context-menu items target the
  library window.
- `perform_command:param:` dispatches by the table's `owner`; the sidebar and
  the task list receive theirs through their own `perform_command:param:`.
- `state_of_command:` is the single answer to "enabled? title? icon? tooltip?
  checked?".  Both `validateMenuItem:` and `validateToolbarItem:` apply it.
  The owner adjusts the default state in `update_state:for_command:`.
- **A toggling command names what a click will DO** ("Hide Completed" while
  they are shown; the toolbar icon likewise).  No check marks, except the
  column-visibility items.
- A menu item carrying a parameter keeps the title it was built with.
- The Edit menu's Undo / Redo / Cut / Copy / Paste / Delete / Select All are
  NOT commands: standard selectors, nil target, so text fields get them.

| Command | Owner | Shortcut | Param |
|---|---|---|---|
| About, Settings… | app | –, ⌘, | |
| New Task | shell | ⌘N | |
| Quick Task | shell | ⌃⌥⌘T (also system-wide, `hotkey.h`) | |
| Delete Task | shell | ⌘⌫ | |
| Info… (open editor) | shell | ⌘I | |
| Clear Completed Tasks | shell | | |
| Open Database File… | shell | ⌘O | |
| Find… | shell | ⌘F | |
| Hide/Show Completed | shell | | |
| Manual/Automatic Sorting | shell | | |
| Hide/Show Sidebar | shell | ⌃⌘S | |
| Hide/Show Favorites Bar | shell | | |
| Compact/Full Controls | shell | | *(not implemented yet)* |
| Kanban/List View | shell | ⇧⌘K | |
| Mark Complete/Incomplete | shell | | BOOL |
| Set/Clear High Priority | shell | | BOOL |
| Add Tag ▸ (every tag) | shell | | tag id |
| Edit Tag… | sidebar | | tag id (none: the selected tag) |
| Done / Status / Due Date / Completion Date columns | task list | | |

**Which tag a new task gets.**  New Task gives its task
`task_sel_new_task_tag(app, selection)` (`task_pane.h`): the selected tag,
or for a view the default tag (`TASK_CFG_DEFAULT_TAG`, a NAME, chosen in
Settings: a tag exists only while a task carries it, so the default may
name one that has gone, and `task_db_task_create` makes it again).  With
neither there is no New Task (the status bar says why).  Quick Task always
gives the default tag,
and opens the editor in the lower-right corner of `NSScreen.mainScreen`
(`tasks_editor_open_new(…, true)`); `src/hotkey.m` registers its shortcut
system-wide through Carbon while `TASK_CFG_QUICK_TASK_HOTKEY` is on.

A bare key is never destructive: Delete is ⌘⌫ through the menu, not a
`keyDown:` case.  Delete Task is disabled while any text is being edited (the
key window's first responder is an `NSText`), because ⌘⌫ is also "delete to
the start of the line" there; a disabled item lets the key through.  To add a command: one enum member, one table row, one case
in its owner's `perform…` and (if its state varies) `update_state…`.

---

## 8. Config, look, utilities

**Config** (`src/task_config.h`): every fixed key has a `TASK_CFG_*` name and
its default lives in the registry in `task_config.c` — call sites never pass a
default.  Read with `task_config_get` / `_get_bool` / `_get_int`, write with
`task_config_set` / `_set_bool` / `_set_int` (written through at once).
Adding a key = one `#define` + one registry row if it has a default.

**Look** (`src/look.h`): every size, timing and non-system colour.  Use
semantic system colours where one fits; otherwise `tasks_color(TasksColor…)`,
which adapts to light/dark by itself.  The dark values were chosen for this
port (GTK had light values only) and are open to the owner's taste.  Add a
constant there rather than a literal in your file.

**Utilities** (`src/ui_util.h`): `tasks_string`, `tasks_title_string`,
`tasks_tag_title`, `tasks_app_icon` — this app's own.  Everything both
apps share is in `../shared_ui/src` (`shared_ui.h`,
`shared_tool_panel.h`, `shared_look.h`), compiled in from
`SHARED_UI_DIR`: `shared_ui_symbol`, `shared_ui_image_view`,
`shared_ui_notice`, `shared_ui_confirm`, `shared_ui_pick_path`,
`shared_ui_place_lower_right`, `shared_ui_emoji_field`, the status
labels, `SharedTable`, `SharedSplitController`.  Every dialog takes a
completion block and the call returns at once.  **No system alerts and no
sheets** (owner decision, 2026-10-16, as Notes): a notice, a question or a
small form is a tool panel (`SharedToolPanel`) — a floating utility
window with a centred symbol-and-headline header — never `NSAlert`, an alert sheet, a
custom sheet or `runModal`.  `shared_ui_notice` and `shared_ui_confirm`
are built on it; Edit Tag and the date pickers build their own with
`panel_with_title:…`, whose handler returns a problem to keep the panel
open.  In a tool panel Escape and the close box are no answer (Cancel),
never the default button, and a destructive button is never the default.
The file chooser is the system's (`shared_ui_pick_path`, app-modal).
Every table is a `SharedTable` (or a subclass of it), which selects a
clicked row on mouse-down and opens the selection on Return.

**Focus rings**: `NSFocusRingTypeNone` on toolbar text fields ONLY (the search
field has it).  Every other field keeps the system ring: use a stock bezeled
`NSTextField` and do not set `focusRingType`.

---

## 9. Ownership and memory conventions

Files has a footprint problem traced to one pattern (rule 9.6), and its
singletons never remove their observers.  Do not copy Files code on trust:
read it for the problems below first.

### 9.1 Who retains windows

| Object | Owner | Released when |
|---|---|---|
| `TaskApp` | app delegate | `applicationWillTerminate:`, after the window shut down |
| `TasksLibraryWindow` | app delegate (`library`) | quit |
| panes (sidebar, list, board) | the library window (strong properties) | with it |
| editor controllers | the editor module's registry, keyed by task id | `windowWillClose:` removes the entry |
| Settings controller | one module-level `static` | `windowWillClose:` sets it to nil |
| tool panels | `tool_panel.m`'s set of live panels | `windowWillClose:` |

`NSWindow.windowController` and `NSWindow.delegate` do not retain.  A window
controller nobody owns is freed under its window: put it in a registry or a
static and take it out in `windowWillClose:` — as the LAST statement, since
nothing else may be keeping `self` alive.

### 9.2 Back references are weak

Panes hold the library window `weak`.  Views hold their controller `weak`.
Targets and delegates are weak by AppKit's design; keep them so.

### 9.3 Blocks

A block that is stored or runs later (`dispatch_after`, completion handlers
kept in a property, observers) captures `self` weakly and re-checks:

```objc
__weak TasksLibraryWindow *ws = self;
dispatch_after(…, dispatch_get_main_queue(), ^{
    TasksLibraryWindow *s = ws;
    if (!s || s.status_message_id != message_id) return;   /* gone, or superseded */
    …
});
```

A `tasks_ui_*` completion block is released when the dialog ends, so it cannot
form a cycle — but the window may have shut down meanwhile: check
`is_shut_down` (or equivalent) before acting.

### 9.4 Observers, timers, C callbacks

- Notification observers added by a per-window object are removed in its
  `shut_down`.  Block observers: keep the token, remove it, nil it.
- An `NSTimer` retains its target; a repeating one must be invalidated in
  `shut_down`.  Prefer `dispatch_after` with a superseding id.
- A `task_loop` timer or `task_app_listen_*` subscription given
  `(__bridge void *)self` holds an UNRETAINED pointer.  Cancel / unlisten in
  `shut_down`, before the object can go away (the library window does both).
- `dealloc` is not teardown.  Every window-scoped object has an idempotent
  `shut_down`, called from `windowWillClose:`.  `dealloc` frees only what the
  object alone owns (a pane's row list); it must not write config, close
  windows or touch the `TaskApp`, which may already be gone.  The library
  window's `dealloc` asserts that `shut_down` ran.

### 9.5 C memory in Objective-C objects

A `task_row_list_t *` (or any core allocation) held by a view is freed when
replaced, in `shut_down`, and in `dealloc`.  Core strings: `g_free` for
`gchar *` returns, `free` for the new plain-C modules — the header says which.
CF objects from a `Create`/`Copy` call are released by hand or bridged with
`CFBridgingRelease`.

### 9.6 Images — the Files footprint bug

An `NSImageView` created with a zero frame (`-init`, `+imageViewWithImage:`),
sized only by Auto Layout, and given its image before any layout pass has no
target size: AppKit renders the image's LARGEST representation (1024 × 1024 px
= 4 MB for a system icon), caches it, and the layer keeps a second copy.  A
table of such cells cost Files 86–127 MB instead of 11.

- **Every image view is made with `shared_ui_image_view(image, width, height)`**
  (`ui_util.h`): framed first, image set last, size constraints included.
  Never `[[NSImageView alloc] init]` or `imageViewWithImage:`.
- **Bundled icons come from `shared_ui_symbol(name, size)`**, which returns a small
  bitmap scaled once to the drawing size (the PNGs are 512 × 512).  Use it for
  buttons, toolbar items and menu items too; never `imageNamed:` for them.
- A file's icon (attachments): `[NSWorkspace iconForFile:]` handed to
  `shared_ui_image_view` at the row's icon size — never to a zero-frame view.
- No unbounded caches: bound with `NSCache.countLimit` or by construction.

### 9.7 How to check

```sh
make ui-check          # in-process: ownership is exact, heap budget, 4 MB regions
```

`tools/ui_check.m` opens and closes the real windows repeatedly and checks
that closed windows are RELEASED (weak references go nil), no listener stays
subscribed, the heap stays within a budget, and no 4 MB region exists outside
the allocator's arenas.  **Extend it when you add a window or a view that owns
something.**  Its heap budget is loose because AppKit itself grows the heap
(its header comment has the measurements); the ownership checks are the exact
ones.

With Developer Tools access (the tools below hang without it, as they did in
the foundation session), also run:

```sh
make seed && make run &                       # then use the UI: open/close windows
leaks <pid>                                   # expect "0 leaks"
footprint <pid>                               # before and after exercising the UI
vmmap <pid> | grep -E '\[ *(4112|4096)K'      # ignore MALLOC_TINY / MALLOC_SMALL lines
TASKS_DATA_DIR="$PWD/dev/data" leaks --atExit -- \
    build/ui_check.app/Contents/MacOS/ui_check "$PWD/dev/snapshots"
```

Anonymous (`VM_ALLOCATE`), `CG image` or `CoreAnimation` regions of 4096K /
4112K mean rule 9.6 has been broken.  `MALLOC_SMALL` and `MALLOC_TINY` regions
are 4 MB by design on macOS 26 and mean nothing.  `ui_check <dir> <seconds>`
stays alive that long so these tools can be pointed at it.

---

## 10. Work packages and file ownership

Each package owns the files listed and replaces its stub `.m`.  Headers are
frozen: propose changes to the integrator.  Everything under "lift into the
core" is pure logic in the GTK file that must become plain C in `src/` with a
test in `tests/` — not be rewritten inside a view.

### WP1 — Shell: library window, menus, shared UI

Owns: `main.m`, `app_delegate.[hm]`, `library_window.[hm]`, `commands.[hm]`,
`ui_util.[hm]`, `row_text.[hm]`, `look.[hm]`, `Makefile`, `resources/`,
`README.md`, `BUILD.md`, `tools/ui_check.m`.
Reference: `main.c`, `app.c` (GTK helpers), `library_window.c`.

The skeleton already has: launch/quit order, the menu bar, the shared state,
refresh orchestration, command dispatch and validation, the task commands
(new, delete, info, mark, favourite, priority, move, clear completed), the
task context menu, toolbar + search field, sidebar show/hide, geometry
persistence, the teardown order.

Still to do:
- **Compact mode** (§11) and its floating New/Delete pair.
- **Open Database File…** — done: pick a `.db`, verify it read-only (only a
  stamped shared database passes; an old `tasks.db` is refused in the shared
  module's words), close editors, open it, re-arm workers, health check,
  then offer "Make Default", which writes the shared pointer file
  (`shared_db_set_default_path`) both apps follow — never while
  `CAMBIO_SHARED_DB` names the file.
- First-run choice (map D7): today a missing database is simply created in
  the data directory.  Decide with the owner whether the Welcome choice
  (open existing / create new) is wanted.
- Status message: hold 3 s then FADE over 1 s (the skeleton clears it).
- Sort-toggle and search interplay already follows the reference; the search
  field's Return/Escape handling is in place.
- Dark-mode toolbar icons: the bundled PNGs are dark artwork and nearly
  vanish on a dark toolbar (see `dev/snapshots/library-list-dark.png`).
- Window title, minimum sizes, About panel polish, Help menu content.

### WP2 — Sidebar

Owns: `sidebar.[hm]`, `favorites_bar.[hm]`.  Reference: `sidebar.c`.
Lists and groups are gone (tags replaced them): a source list of the
views, the non-selectable Tags header and every tag; selection restored
across reloads (`task_sidebar_pick`); Edit Tag in a tool panel, with the
emoji slot (`shared_ui_emoji_field`); the Favorites bar of favorite tags; the
toolbar's sidebar toggle.

### WP3 — Task list

Owns: `task_list.[hm]`.  Reference: `task_list.c`.
Columns (handle, done checkbox, the tall text cell, Status, Due, Completed),
variable row heights, stripes, header sorting, column-visibility menu and
commands, manual-sort drag + `task_order_save`, the checkbox's in-place update,
double-click / Return to open the editor, right-click selection rule.
Lift into the core: the five sort comparators (`task_list.c:1210–1267`) over
`task_row_t`.
The Due cell's colour is `tasks_due_color(row->due_urgency)`.

### WP4 — Kanban

Owns: `kanban.[hm]`.  Reference: `kanban.c`.
Lanes (index = `TaskStatus`), cards drawn through `tasks_row_text(…,
TasksRowLayoutTruncate)`, selection set and shift-range, the drag (ghost,
marker, Escape, one exit path), the two-half drop, Done-lane cap and its
link, diffing refresh, the shadow setting (re-read in `show_rows:`).
Lift into the core: order apply/save and the drop rules
(`kanban.c:655–831`), Done-lane ordering (`1122–1133`).

### WP5 — Editor

Owns: `editor_window.[hm]`.  Reference: `editor_window.c`.
Everything in map §3.8; keep the registry ownership the stub shows.  The top
edge stays fixed when the window grows or shrinks.  Saves fire
`task_app_notify_tasks`.
Lift into the core: date/time entry parsing and the recurrence-lead logic
(`editor_window.c:92–272, 448–472`).
`tasks_editor_open_new` must add Cancel (drop the pending save, close, THEN
delete the task).  After a subtask tick, resync the parent's status control.

### WP6 — Settings

Owns: `settings_window.[hm]`.  Reference: `settings_window.c`.
Appearance (bold titles, Kanban shadows, the business-day forecast, the
note-context title prefix), the Database plate, backups (map §3.9).  After
a change that alters rows call `task_app_notify_tasks` — it also reloads
the day planner and retitles the reminders (`tasks_notify_retitle`).  The backup settings are the
shared database's (`shared_backup_policy_get` / `shared_settings_set`, and
`task_backup_set_dir` for the folder), which Notes follows too; the poll reads them afresh, so nothing is re-armed.  The Kanban shadow needs no call into the board:
the board re-reads `TASK_CFG_KANBAN_SHADOW` on every `show_rows:`.
Lift into the core: `dir_shares_fate`, `health_stamp`
(`settings_window.c:120–136, 206–225`).

### Shared rules for all packages

- New pure logic → a core `.c/.h` pair + a `tests/<name>_test.c` (use
  `tests/check.h`, `tests/fixture.h`).
- Before writing a helper, look in `ui_util.h`, `look.h`, `task_*.h`.  If the
  one there does not fit, ask for it to be extended.
- Status messages: carry the GTK wording over verbatim.

---

## 11. Compact mode — the specification

The spec is the behaviour of `porting/gtk_reference/library_window.c`
(`compact_layout_apply`, `on_menu_toggle_compact`, the size code in
`task_library_window_new` and `on_library_destroy`).  The older GTK documents
that say Compact "leaves the sidebar alone" are wrong.

Entering Compact (`TASK_CFG_COMPACT` → 1):
1. Save the current window size as `compact_pre_w` / `compact_pre_h`.
2. Hide the toolbar and hide the sidebar.
3. Show the floating controls: New Task and Delete Task as a two-button
   plate, `tasks_look_float_margin` from the bottom-right of the task pane.
4. Clear an active search (the field that would clear it is now hidden),
   through the same path as the user clearing it.
5. Resize the window to `compact_w` × `compact_h` (default 600 × 500).

Leaving Compact:
1. Save the current window size as `compact_w` / `compact_h`.
2. Show the toolbar; hide the floating controls.
3. SHOW the sidebar and record `sidebar_visible = 1`.
4. Resize to `compact_pre_w` × `compact_pre_h` (default 980 × 640).

Launch in Compact uses `compact_w` / `compact_h`; closing in Compact also
saves the size to them.  One applier sets all of it and titles the command
("Compact Controls" / "Full Controls").  In AppKit the resize is a plain
`setFrame:display:animate:` keeping the top-left corner.

One deliberate difference from the GTK code: in FULL mode the skeleton
restores the sidebar's saved visibility at launch (`sidebar_visible`, default
shown).  The GTK HEAD shows it unconditionally, which its own porting map
flags as a side effect.

---

## 12. Mac conventions (they win over GTK parity)

- Menus: App, File, Edit, View, Window, Help.  About / Settings / Quit are in
  the App menu, not File.
- Shortcuts as in §7.  ⌘W closes the key window; closing the library window
  quits (editors are flushed first).
- Window titles: the library window is "Tasks AppKit"; an editor's is its
  task's title; Settings is "Settings".  No "Tasks - " prefix.
- Dialog buttons are named for their action ("Delete", "Create"), with
  Cancel; destructive ones use `shared_ui_confirm(…, destructive:YES)`.
  In a confirm Cancel is the default button (Return), as in Notes: a key
  press never performs the action (owner decision, 2026-10-16).
- No system alerts (owner decision, 2026-10-13): every message or question
  is a tool panel (§8).
- No GTK workarounds: no custom tooltips, double-click timers, select-on-press
  helpers, or classic-scrollbar forcing.

---

## 13. Style (the owner's rules; reviewers check them)

- snake_case for our own variables, functions, methods, properties, files.
  AppKit overrides and delegate methods keep Apple's names.  Classes and
  `NS_ENUM`s are PascalCase with the `Tasks` prefix.
- K&R braces; a function or method DEFINITION has its return type on its own
  line and its opening brace on its own line.
- Every function and method has a comment block: purpose, `Inputs:`,
  `Output:` with the error convention.  The full block is in the header for
  public API; the definition repeats a one-line summary and points there.
  Comments say WHY.  A changed behaviour is not finished until its comment is.
- `/* */` comments only.  Section rules like
  `/* ── Commands ───── */`, never `#pragma mark`.
- No dead code, no commented-out code, no unused functions, no "just in
  case".  When something is replaced, the old path goes in the same commit.
- One definition of every constant, struct and helper.
- One module per `.h`/`.m` pair, flat `src/`.  ARC.  No nibs, xibs or
  storyboards.  No categories.  `TasksTaskPane` is the one protocol.
- Zero warnings from `make`.

---

## 14. Verification before you hand back

```sh
make clean && make      # zero warnings
make test               # core tests
make seed               # fresh sandbox database (45 tasks, a dozen tags; 3 Notes items)
make ui-check           # snapshots in dev/snapshots, ownership + memory checks
make run                # the app, against dev/data
```

`make ui-check` writes PNGs of each window (light, dark, list, board, editor,
Settings) with no screen-recording permission: look at them.  `tools/winshot.sh`
screenshots the running app where that permission exists.

---

## 15. Deviations from the map's §8.5, and known gaps

- **§8.5(1) Core API types**: unchanged (GLib types stay this phase, by the
  owner's decision).  New code uses plain C types.
- **§8.5(2) `TaskApp`**: holds no editor registry and no window handle; the
  AppKit layer owns its windows (§9.1).  `lib_of(app)` has no successor: the
  listeners carry the window as context and are removed before it goes.
- **§8.5(3) Main loop**: `task_loop_timer_once/_every/_cancel`,
  `task_loop_main_async`, `task_loop_background` — named for the module, with
  integer ids instead of pointers.
- **§8.5(4) Config**: not shared with the GTK app (own directory); no
  "ini beside the binary", no defaults file.
- **§8.5(5)**: `library_priv.h` has no successor; its shared state and
  functions are the public interface of `TasksLibraryWindow`.
  `lib_row_order_*` became `task_order_*`; the collection half of
  `lib_refresh_tasks` became `task_pane_collect`.
- **§8.5(9)**: `task_library_apply_kanban_shadow` is gone — the board re-reads
  the setting on refresh and Settings fires `task_app_notify_tasks`.
  `tasks_editor_close(task_id)` was added (delete closes the editor first).
- **Interface-phase scaffolding.**  Some things exist with no caller yet,
  because they are the agreed interface for packages not written: the
  config keys for Compact mode, column visibility and the Kanban shadow;
  most `tasks_look_*` constants and `TasksColor` members; `tasks_due_color`,
  `shared_ui_pick_path`, `shared_ui_image_view`; the sidebar and column rows of
  the command table; `task_order_*` (tests only so far).  The no-unused-code
  rule applies again at integration: whatever still has no caller then is
  deleted.
- **Open Database — shared databases only (owner decision, 2026-10-12).**
  The file Notes and Tasks share carries `application_id` and
  `user_version` stamps (`shared_db.h`); any other file — the GTK app's or
  this app's old `tasks.db`, a Notes `notes.db` — is refused untouched.  Old
  files are converted by `migrate_shared` (`../shared_data/migrate/`).  The
  location fence round the GTK app's live data stays.
- **First-run Welcome screen (owner decision).**  When no database is found
  at launch, a Welcome tool panel offers "Open Existing Database…" (an
  NSOpenPanel; the file chosen becomes the default for both apps; Cancel
  comes back to the question), "Create New Database" (task_db_open creates
  the shared file at `shared_db_path()`) and Quit, which Escape and the
  close box also mean.  The launch carries on from the answer
  (`finish_launch_at:choice:`).  This prevents users
  with an existing GTK database from being silently given a second, empty
  one.
- Not swept: comments inside the carried core files (`db.c`, `recur.c`,
  `backup.c`, …) still mention sync, plugins and GTK-era names in places.
- The unused sync-era `task_db_*` functions listed in map §2.2 are still in
  `db.c` (the brief kept `db.c` untouched); they are dead code to remove in
  the GLib phase.
