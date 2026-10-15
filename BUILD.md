# Tasks — Build Guide

## Toolchain

| Tool | Version |
|------|---------|
| macOS | 14.0 or later (developed on macOS 26) |
| Compiler | Apple clang from the Xcode Command Line Tools (developed with clang 21) |
| make | the system `make` |
| pkg-config | MacPorts `pkgconf` |

No Xcode project is used.  The Command Line Tools are enough:

```sh
xcode-select --install
```

## Dependencies

| Library | Used for | Where it comes from |
|---|---|---|
| Cocoa, UniformTypeIdentifiers | the interface | macOS SDK |
| Carbon (`RegisterEventHotKey` only) | Quick Task's system-wide shortcut, ⌃⌥⌘T (`src/hotkey.m`); needs no Accessibility permission | macOS SDK |
| libdispatch, CoreFoundation | timers and background work in the core; the tests' run loop | macOS SDK |
| GLib 2 (`glib-2.0`; developed with 2.88) | utilities in the core — strings, arrays, key file, dates | MacPorts `glib2` |
| SQLite 3 (3.35 or later; developed with 3.53) | storage | MacPorts `sqlite3` |
| `shared_db` (the shared database module: schema, stamps, path, backups, check) | the database Notes and Tasks share | the `shared_data` repository beside this one, compiled in from source (below) |
| libcurl, Security | the Google Calendar sync's HTTPS calls; the keychain item holding its sign-in | macOS SDK (the system libcurl, which trusts the keychain's certificates) |

```sh
sudo port install glib2 sqlite3 pkgconf
```

GLib is a phase-one dependency: the core was carried from the GTK app and
still uses it.  GTK, GDK, Pango and GIO are NOT used or linked.  After
installing or upgrading a port, run `make clean && make`.

MacPorts installs `pkg-config` in `/opt/local/bin`; the Makefile finds it
there when it is not on `PATH`.

### The shared database module

`shared_data/src/shared_db.c` is compiled into the core with this
project's flags (it becomes `build/shared_db.o`, linked into the app, the
tests, the seed tool and ui-check), and `shared_data/src` is on the include
path.  The Makefile takes it from

```make
SHARED_DATA_DIR ?= $(CURDIR)/../shared_data
```

so the default layout is the two repositories side by side; point
`SHARED_DATA_DIR` elsewhere on the command line (`make
SHARED_DATA_DIR=/path/to/shared_data`).  The build stops with a message when
`shared_db.c` is not there.  The module is plain C11 and SQLite, the same
MacPorts SQLite this app links.  After pulling a new `shared_data`, run
`make clean && make`.

Tasks needs `shared_data` at schema version 102: tags (`task_tag_names`
with emoji and favorite, `task_tags`) are its only organisation.  The
module brings a version-100 or -101 file up to date in place, in one
transaction, the first time an app opens it read-write — the step to 102
also turns every list and group into a tag on its tasks — and until then
read-only tools (the skill's scripts) refuse it.  A file at 102 is refused
by a Notes or Tasks built against an older `shared_data`, so rebuild Notes
from the same `shared_data` as well.

## Targets

| Target | What it does |
|--------|--------------|
| `make` | Build `Tasks.app` in the source directory |
| `make run` | Build, then run the app against the sandbox data in `dev/data` (seeding it first if empty) |
| `make seed` | Recreate the sandbox test database: 45 tasks with a dozen tags (emojis, two favorites), three of them Notes action items (one in a folder) |
| `make test` | Build and run the core tests (`tests/*_test.c`); no window is opened |
| `make ui-check` | Build the real windows in-process against the sandbox: writes PNG snapshots to `dev/snapshots` and checks window ownership and memory |
| `make install` | Copy the bundle to `~/Applications` (quits and reopens a running copy) |
| `make clean` | Remove `build/` and the bundle |

`make run`, `make seed`, `make test` and `make ui-check` always set
`TASKS_DATA_DIR` to a folder under `dev/` and `CAMBIO_SHARED_DB` to a
database file inside it (`dev/data/shared_data.db`; each test its own
scratch file under `dev/test-data`), so no development run uses the
per-user data folder or the per-user shared database.  With no backup
folder set, the shared module puts a sandbox database's backups in
`backups/` beside it (`dev/data/backups`).  `dev/` is gitignored.

The build must produce no compiler warnings.  One line of output is not a
warning: `codesign` prints "replacing existing signature" each time it
re-signs the bundle.

## Output

```
Tasks.app/
└── Contents/
    ├── Info.plist      resources/Info.plist + identity, version, build date
    ├── MacOS/Tasks
    └── Resources/      document.png, the app's icon
build/                  objects, dependency files, test binaries, seed, ui_check.app
dev/                    sandbox data, test scratch folders, snapshots
```

## Identity and version

The app's name, bundle name, bundle identifier and data-folder name are
defined once, at the top of the `Makefile` (`APP_NAME`, `APP`, `BUNDLE_ID`,
`DATA_DIR_NAME`).  They reach the code as `-D` macros and reach `Info.plist`
through `plutil`; nothing else spells them.  To rename the app, change them
there and run `make clean && make`.

The version is the one line in `VERSION`.  The build time is written to
`Info.plist` as `TasksBuildDate` and shown in the About panel.

## Google OAuth client

The Google Calendar sync needs an OAuth client.  `client_credentials.mk`
(gitignored, optional) in the source folder supplies one at build time:

```make
GOOGLE_CLIENT_ID     = <id>.apps.googleusercontent.com
GOOGLE_CLIENT_SECRET = <secret>
```

Only `build/oauth.o` is compiled with the two values, and it is rebuilt
when the file changes.  Without the file the app looks for the Google Cloud
console's client file in the data folder (`src/oauth.h`).  The client must
be a "Desktop app" client with the Google Calendar API enabled on its
project.

## Compiler flags

`-O2 -Wall -Wextra -Werror=unguarded-availability -mmacosx-version-min=14.0
-MMD -MP`, plus `-std=c11` for C and `-fobjc-arc -Wno-unused-parameter` for
Objective-C (the unused parameters are the ones AppKit's callback signatures
require).  `-MMD -MP` writes header-dependency files, so editing a header
rebuilds what includes it; objects also depend on the `Makefile`, so a flag
change rebuilds everything.

## Code signing

The bundle is signed with the first "Apple Development" identity in the
keychain, or ad hoc (`-`) when there is none.  Override with:

```sh
make CODESIGN_IDENTITY="…"     # list: security find-identity -v -p codesigning
```

A stable identity matters because macOS ties privacy grants to the signature:
an ad-hoc signature changes with every build.  No hardened runtime, no
entitlements, no notarisation — this is a locally built app.

## Tests

```sh
make test
```

The programs, all linked against the core only (no AppKit), include:

| Program | Covers |
|---|---|
| `core_test` | the GTK app's cases: search, recurrence arithmetic, opening the shared database and refusing an old v14 file; the display title (`task_display_title`: the note-context prefix, an empty context, an already-prefixed title, case folding, separators) (GLib `g_test`) |
| `loop_test` | the main-loop service: one-shot and repeating timers, cancellation, background hand-back |
| `config_test` | the data-folder policy and override, the fence round the GTK app's data, config defaults and write-through (the note-context setting among them) |
| `shared_test` | the shared database: the column-diff update (two connections change different fields and both survive) and its stamping rules; Notes action items (the Action Items tag the schema gives them, the note lookup, the note context every read carries — a folder, the top level, a task not from Notes, a folder renamed — the tombstone, the purge Tasks refuses); refreshing only on a change to Tasks' data (`change_rev`), never on its own writes; writes failing, and then succeeding, while a second process (this test re-run with `--hold`) holds the write lock past the busy timeout — this test takes about 20 s |
| `backup_test` | backups through the shared module: refused destinations (Tasks' live-data fence and the module's folder checks), Back Up Now and its UTC file name, retention, the periodic poll |
| `switch_test` | Open Database File: refusals (an old `tasks.db` and an empty file among them, left untouched), a switch, Make Default under the sandbox |
| `rows_test` | the row description (lines, emphasis, the tags line, notes preview, the note-context prefix) and the pane collection (filters, a search matching the prefix, status line, the Favorites view); which tag New Task gives (the selected tag, else the default tag) |
| `tags_test` | tags: adding by name (any case, trimmed), the refusals, a subtask's inherited tags, removal and the last tag kept, a tag going with its last task, emoji and favorite, a tag's view and its saved order |
| `sidebar_test` | a tag's label, the selection fallback after a rebuild |
| `order_test` | saved orders: key spelling, the permutation, dropping keys |
| `app_test` | the event bus, the outside-change watch (`data_version`), due-date urgency, the recurrence pass waking on a main-loop timer |
| `gcal_test` | the calendar sync without the network: every planning rule, reading events and writing request bodies, the sync's tables (dirty check, compare-and-set reschedule, event cache, account), the sign-in's loopback listener |

They work in scratch folders under `dev/test-data` and refuse to run unless
`TASKS_DATA_DIR` names the sandbox and `CAMBIO_SHARED_DB` a file inside it
(`tests/check.h`).

`make ui-check` is the interface's check; see `porting/INTERFACES.md` §9.7.

## Source layout

```
src/
  task_paths.[ch]      data folder; the fence round the GTK app's data
  task_config.[ch]     settings (tasks.ini) and the registry of keys
  task_app.[ch]        TaskApp: the database handle, the event bus, the outside-change watch
  task_loop.[ch]       timers / main-queue / background work on libdispatch
  task_dates.[ch]      date helpers
  db.[ch]              Tasks' queries over the shared database (shared_db.h opens it)
  search.[ch]          search query language
  recur.[ch]           recurring tasks
  backup.[ch]          backups through the shared module, on a background thread
  task_worker.[ch]     scheduler for the recurrence pass and the backup poll
  task_switch.[ch]     File > Open Database File, Make Default
  task_view.[ch]       registry of sidebar views
  core_views.[ch]      the three built-in views
  task_ops.[ch]        clear-completed, the default tag
  task_rows.[ch]       toolkit-neutral description of a task row
  task_pane.[ch]       sidebar selection type (a view or a tag); collects its rows
  task_sidebar.[ch]    the sidebar's selection fallback
  task_order.[ch]      saved manual / Kanban orders
  http.[ch]            one blocking HTTPS request (libcurl)
  json.[ch]            minimal JSON parser and string escaping
  oauth.[ch]           Google sign-in: PKCE, loopback redirect, keychain
  gcal_plan.[ch]       the calendar sync's per-task decision
  gcal_store.[ch]      the calendar sync's tables: calendars, events, bindings
  gcal_api.[ch]        the Google Calendar REST calls
  gcal_sync.[ch]       the calendar sync: worker, kick, background pass

  main.m               entry point
  app_delegate.[hm]    launch, menu bar, About, quit
  library_window.[hm]  main window: shared state, refresh, commands (skeleton)
  commands.[hm]        the command table (menus, toolbar, validation)
  look.[hm]            sizes, timings, colours
  ui_util.[hm]         icons, image views, the shared table view, dialogs
  tool_panel.[hm]      the app's own dialog (never a system alert) and its blue hero strip
  hotkey.[hm]          Quick Task's system-wide shortcut
  row_text.[hm]        the renderer of a task row's text
  sidebar.[hm]         sidebar: the views and every tag, Edit Tag
  favorites_bar.[hm]   Favorites bar: the favorite tags
  task_list.[hm]       task list          (stub)
  kanban.[hm]          Kanban board       (stub)
  day_planner.[hm]     day planner, with Google Calendar events
  editor_window.[hm]   task editor        (stub)
  settings_window.[hm] Settings           (stub)
tests/                 core tests; check.h and fixture.h are the harness
tools/
  seed.c               writes the sandbox test database
  ui_check.m           in-process window snapshots and ownership/memory checks
  winshot.sh, winlist.swift   screenshot a window of the running app
resources/Info.plist
icons/                 PNG artwork: document.png is the app's icon and the one file
                       bundled; the rest are spares.  Command icons are SF Symbols
porting/               the porting documents and the GTK reference sources
```

Every `src/*.c` is compiled as core and every `src/*.m` as interface, so a
file left in `src/` is built: delete what is no longer used.

## Platform notes

- **macOS only.**  The GTK app's Linux support does not carry over; the GTK4
  app lives on the `gtk4-testing` branch, which has its own build
  instructions.
- **The app runs on the macOS it was built on, or later — not yet on 14.**
  The sources are compiled for macOS 14.0 and use no newer API, but the
  binary is linked for the build machine's macOS (`LINK_TARGET` in the
  Makefile, also written to `LSMinimumSystemVersion`), because the MacPorts
  GLib and SQLite libraries are built for the system they were installed on.
  The app also loads those two libraries from `/opt/local/lib`, so it runs
  only on a Mac with those ports installed.  Both limits go when GLib is
  removed and the system SQLite is used.
- clang cannot check availability for messages to `NSApp`
  (a `__kindof` receiver); review those by hand.  `[NSApp activate]` needs
  macOS 14, which is why the deployment target is 14.0.
- The date helpers and the tests assume the local time zone is resolvable.
- `leaks`, `vmmap`, `footprint` and a debugger need Developer Tools access
  (`DevToolsSecurity -status`); without it they wait for an authorisation
  prompt.  `make ui-check` needs none.
