# Tasks

A macOS task manager — lists, tall task rows, a Kanban board, subtasks, due
dates, recurring tasks — with a native AppKit interface.  Its tasks live in
the one SQLite file it shares with the Notes app: an action item written in
a note is an ordinary task here, and an edit made in either app appears in
the other.  It began as a port of the GTK4 [Tasks](https://github.com/IANatCAMBIO/Tasks)
app; the AppKit app is now the project's main line.

## Branches

| Branch | What it is |
|---|---|
| `master` | The AppKit app for macOS.  This is the project. |
| `gtk4-testing` | The original GTK4 app (Linux and macOS), kept as a testing branch: the behavioural reference for the port, and a place to try ideas against the GTK code.  It is not the product and does not receive AppKit work. |

**Status: in progress.**  The non-UI core is carried over from the GTK app and
runs under the Cocoa run loop; parts of the AppKit interface are still
skeletons with placeholder panes.  What works today:

- the main window: toolbar, search field, sidebar (flat list of views and
  lists), task list (tall rows, read-only), status bar, the standard menus;
- task commands from the menus, the toolbar and the context menu: new,
  delete, mark complete, favourite, high priority, move to list, clear
  completed; show/hide completed; show/hide sidebar; list/board toggle;
- recurring tasks roll forward while the app runs.

Not yet ported (placeholders): the Kanban board, the task editor, Settings,
list groups and list editing in the sidebar, sorting and manual order in the
list, Compact Controls.  The plan and the interfaces the
UI is being built against are in [porting/INTERFACES.md](porting/INTERFACES.md).

The code is plain C for the core (with GLib, to be removed in a later phase)
and Objective-C written in a C style for the interface.  No Xcode project, no
nibs.

## Requirements

- macOS 14 or later to build against; the built app runs on the macOS version
  it was built on, or later (see [BUILD.md](BUILD.md), "Platform notes").
- Xcode Command Line Tools (`xcode-select --install`).
- MacPorts with `glib2`, `sqlite3` and `pkgconf`:

  ```sh
  sudo port install glib2 sqlite3 pkgconf
  ```
- The `shared_data` repository checked out beside this one
  (`../shared_data`): the shared database module is compiled in from it
  (see [BUILD.md](BUILD.md)).

## Build and try it

```sh
make          # builds Tasks.app
make seed     # creates a test database in dev/data
make run      # runs the app against dev/data
make test     # core tests
```

`make run` never touches real data: it points the app at `dev/data` inside
the source tree, and at a shared database file there
(`dev/data/shared_data.db`).

## Install

```sh
make install  # copies Tasks.app to ~/Applications
```

A running installed copy is asked to quit first and reopened afterwards.

## Google Calendar

The day planner syncs with Google Calendar, both ways.  Meetings in the
calendars you choose are drawn in the planner so you can schedule around
them.  Tasks you drag onto the planner become events in your calendar, and
moving, stretching or removing them updates the event.  Moving or deleting
that event in Google Calendar updates the planner.

To set it up:

1. The app needs a Google OAuth client, a "Desktop app" client from the
   Google Cloud console with the **Google Calendar API** enabled.  Either
   put its two values in `client_credentials.mk` in the source folder and
   rebuild:

   ```make
   GOOGLE_CLIENT_ID     = <id>.apps.googleusercontent.com
   GOOGLE_CLIENT_SECRET = <secret>
   ```

   or drop the console's downloaded client file into the data folder,
   renamed to `client_secret.apps.googleusercontent.com.json`.  Both are
   gitignored.  The client that the GTK app's Google Tasks sync used works
   once the Calendar API is enabled on its project.
2. Open Settings, press **Sign In to Google…** and approve the request in
   your browser.
3. Tick **Sync the day planner with Google Calendar**.  After the first
   sync, choose the calendar tasks go to and the calendars to show.

The sign-in is kept in the macOS keychain, one entry per data folder, so a
`make run` sandbox has to be signed in separately and can never push test
tasks into the calendar the installed app uses.

## Where the data lives

**The tasks** are in the database Notes and Tasks share:

```
~/Library/Application Support/camb.io/
    shared_data.db   the shared database (with its -wal and -shm files)
    location         optional: the path of a database kept elsewhere
    backups/         backups, by default
```

Both apps open the same file and follow the same `location` pointer, which
**File > Open Database File…** writes when you answer **Make Default**.  The
file must be on a local disk and not inside iCloud Drive, Dropbox or
another synced folder: the app refuses to open it there.  Only a shared
database can be opened; an old Tasks `tasks.db` or a Notes `notes.db` is
refused untouched, and is brought across with the shared module's migration
tool instead.  Changes made by Notes (or by a script) appear within a
second, and an edit never overwrites a field you did not change.

A task written as an action item in a note shows "From note: …" in its
editor.  Its title, completion and due date stay in step with the note's
line; deleting it here makes Notes drop the item mark from the line.

**Backups** are on by default: every hour a verified copy of the shared
database is written to `backups/` (or a folder chosen in Settings), keeping
the newest 48.  Notes and Tasks share these settings, and whichever app is
running makes the backup when it is due.  **Back Up Now** in Settings makes
one at once.

**Tasks' own settings** stay in Tasks' folder (attachments are links to
files wherever they are, and are not copied):

```
~/Library/Application Support/Tasks/
    tasks.ini        the settings
    client_secret.apps.googleusercontent.com.json   optional Google client
```

Setting `TASKS_DATA_DIR` moves Tasks' folder and `CAMBIO_SHARED_DB` names
the database file; `make run`, `make seed`, `make test` and `make ui-check`
set both to places under `dev/`.  While `CAMBIO_SHARED_DB` is set, **Make
Default** is refused with the shared module's reason, and backups go to
the folder set for that database or, when none is set, to `backups/`
beside it — never to the per-user folder.

**This app does not share data with the GTK Tasks app** (the `gtk4-testing`
branch) and cannot open that app's folder (`~/.local/share/tasks`): the
database code refuses any path inside it.

## Configuration

Settings are changed in the app and stored in `tasks.ini` (group `[tasks]`,
written by the app; comments are not preserved), apart from the backup
settings, which live in the shared database so that Notes follows them too.  The Settings window is not
ported yet, so for now the few settings that have a control are the View-menu
toggles.  Every key and its default is listed in `src/task_config.h`.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| ⌘N | New Task (in the selected list) |
| ⇧⌘N | New List… *(not ported yet)* |
| ⌘I | Info… — open the task editor |
| ⌘⌫ | Delete the selected task(s) |
| ⌘F | Find — search the selected view |
| ⌃⌘S | Hide / Show Sidebar |
| ⇧⌘K | Kanban View / List View |
| ⌘, | Settings… |
| ⌘W | Close the window (closing the main window quits) |

## Documents

- [BUILD.md](BUILD.md) — toolchain, dependencies, targets, platform notes.
- [porting/INTERFACES.md](porting/INTERFACES.md) — the contract for the UI
  work: frozen headers, ownership rules, the command table, work packages.
- [porting/GTK_PORTING_MAP.md](porting/GTK_PORTING_MAP.md) — what the GTK
  app does, feature by feature.
- [porting/FILES_PLAYBOOK.md](porting/FILES_PLAYBOOK.md) — AppKit
  conventions and lessons from the Files app.
- `porting/gtk_reference/` — the GTK sources and documents, kept as the
  behavioural reference until the port is complete.  They describe the GTK
  app, not this one.  The full GTK4 app is on the `gtk4-testing` branch.

## License

BSD 3-Clause — see [LICENSE](LICENSE).
