# Tasks AppKit — Build Guide

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
| libdispatch, CoreFoundation | timers and background work in the core; the tests' run loop | macOS SDK |
| GLib 2 (`glib-2.0`; developed with 2.88) | utilities in the core — strings, arrays, key file, dates | MacPorts `glib2` |
| SQLite 3 (3.35 or later; developed with 3.53) | storage | MacPorts `sqlite3` |

```sh
sudo port install glib2 sqlite3 pkgconf
```

GLib is a phase-one dependency: the core was carried from the GTK app and
still uses it.  GTK, GDK, Pango and GIO are NOT used or linked.  After
installing or upgrading a port, run `make clean && make`.

MacPorts installs `pkg-config` in `/opt/local/bin`; the Makefile finds it
there when it is not on `PATH`.

## Targets

| Target | What it does |
|--------|--------------|
| `make` | Build `TasksAppKit.app` in the source directory |
| `make run` | Build, then run the app against the sandbox data in `dev/data` (seeding it first if empty) |
| `make seed` | Recreate the sandbox test database: 2 groups, 8 lists, 42 tasks |
| `make test` | Build and run the core tests (`tests/*_test.c`); no window is opened |
| `make ui-check` | Build the real windows in-process against the sandbox: writes PNG snapshots to `dev/snapshots` and checks window ownership and memory |
| `make install` | Copy the bundle to `~/Applications` (quits and reopens a running copy) |
| `make clean` | Remove `build/` and the bundle |

`make run`, `make seed`, `make test` and `make ui-check` always set
`TASKS_DATA_DIR` to a folder under `dev/`, so no development run uses the
per-user data folder.  `dev/` is gitignored.

The build must produce no compiler warnings.  One line of output is not a
warning: `codesign` prints "replacing existing signature" each time it
re-signs the bundle.

## Output

```
TasksAppKit.app/
└── Contents/
    ├── Info.plist      resources/Info.plist + identity, version, build date
    ├── MacOS/TasksAppKit
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

Six programs, all linked against the core only (no AppKit):

| Program | Covers |
|---|---|
| `core_test` | the GTK app's 18 cases: search, recurrence arithmetic, database schema (GLib `g_test`) |
| `loop_test` | the main-loop service: one-shot and repeating timers, cancellation, background hand-back |
| `config_test` | the data-folder policy and override, the fence round the GTK app's data, config defaults and write-through |
| `rows_test` | the row description (lines, emphasis, notes preview) and the pane collection (filters, status line) |
| `order_test` | saved orders: key spelling, the permutation, dropping keys |
| `app_test` | the event bus, due-date urgency, the recurrence pass waking on a main-loop timer |

They work in scratch folders under `dev/test-data` and refuse to run without
`TASKS_DATA_DIR`.

`make ui-check` is the interface's check; see `porting/INTERFACES.md` §9.7.

## Source layout

```
src/
  task_paths.[ch]      data folder; the fence round the GTK app's data
  task_config.[ch]     settings (tasks.ini) and the registry of keys
  task_app.[ch]        TaskApp: the database handle and the event bus
  task_loop.[ch]       timers / main-queue / background work on libdispatch
  task_dates.[ch]      date helpers
  db.[ch]              SQLite schema and CRUD
  search.[ch]          search query language
  recur.[ch]           recurring tasks
  backup.[ch]          rotating backups
  task_worker.[ch]     scheduler for the recurrence pass and backups
  task_view.[ch]       registry of sidebar views
  core_views.[ch]      the three built-in views
  task_ops.[ch]        move-to-list, clear-completed
  task_rows.[ch]       toolkit-neutral description of a task row
  task_pane.[ch]       sidebar selection type; collects a selection's rows
  task_order.[ch]      saved manual / Kanban orders

  main.m               entry point
  app_delegate.[hm]    launch, menu bar, About, quit
  library_window.[hm]  main window: shared state, refresh, commands (skeleton)
  commands.[hm]        the command table (menus, toolbar, validation)
  look.[hm]            sizes, timings, colours
  ui_util.[hm]         icons, image views, dialogs
  row_text.[hm]        the renderer of a task row's text
  lists_bar.[hm]       lists bar: views, groups and lists, their sheets
  task_list.[hm]       task list          (stub)
  kanban.[hm]          Kanban board       (stub)
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

- **macOS only.**  The GTK app's Linux support does not carry over.
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
