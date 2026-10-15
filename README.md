# Tasks

A macOS task manager — tags, tall task rows, a Kanban board, subtasks, due
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

- the main window: toolbar, search field, sidebar (the views, then every
  tag), the Favorites bar, task list (tall rows), status bar, the standard
  menus;
- task commands from the menus, the toolbar and the context menu: new,
  delete, mark complete, high priority, add tag, clear completed;
  show/hide completed; show/hide sidebar and Favorites bar; list/board
  toggle;
- recurring tasks roll forward while the app runs.

Not yet ported (placeholders): the Kanban board, the task editor, Settings,
sorting and manual order in the list, Compact Controls.  The plan and the interfaces the
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
refused untouched (as is an empty file: the shared module creates a missing
database complete, in one step), and is brought across with the shared
module's migration tool instead.  Changes Notes (or a script) makes to the
tasks appear within a second, keeping the selection and the scroll; a change
to notes alone refreshes only the "From note: …" lines of open editors.  An edit never overwrites a field you did
not change, even one you were in the middle of editing.  When a change cannot
be saved because another app held the file for too long, the status bar says
so and the panes show what is really stored; an editor keeps the change,
saves it again with the next change or on closing, and will not close while
it cannot.

**Tags are how tasks are organised**; there are no lists.  Every task has
at least one tag, and any number.  The sidebar at the left shows three
views — **Favorites** (every task with a favorite tag), **All Tasks** and
the **3-Day Forecast** — then every tag, by name.  Selecting a tag shows
every task carrying it, the way a list would (sorting, Kanban, search, its
own manual order); each row says what it is tagged.

- The editor's **Tags** field gives a task its tags.  Typing offers the
  tags that already exist (pick one with the arrow keys or a click);
  Return makes what you typed a tag, a new one when no tag has that name.
  Names ignore case: "work" typed is the existing "Work".  Delete a tag's
  token to take it off — except the last: a task keeps at least one.
- Select tasks and right-click, **Add Tag ▸** gives them all a tag.
- Right-click a tag in the sidebar, **Edit Tag…**: give it an emoji, and
  make it a favorite or not.  A tag's name never changes.
- **Favorite** tags have a button each in the **Favorites bar** above the
  task pane (View → Hide/Show Favorites Bar).  The sidebar toggles with the
  button at the far left of the toolbar (⌃⌘S).
- A subtask is born with its parent's tags.  New Task gives the selected
  tag, or with a view selected the **default tag** chosen in Settings; Quick
  Task always gives the default tag.
- A tag exists only while a task carries it: when the last one loses it —
  untagged or deleted — the tag goes.
- A task Notes writes carries the tag **❗ Action Items**.

Lists are gone.  Opening an older database for the first time (schema 101
or before) turns each list into a tag on its tasks — its emoji kept — and
each list group into a tag on the tasks of its lists; an empty list leaves
nothing.  The old per-task favourite star is dropped: favorites are tags
now.

A task written as an action item in a note shows "From note: …" in its
editor.  Its title, completion and due date stay in step with the note's
line; deleting it here makes Notes drop the item mark from the line.
Settings → Appearance → **Show the note's context before Notes action
items' titles** (on by default) shows the name of the note's folder before
such an item's title wherever tasks are listed — the list, the Kanban, the
3-Day Forecast, any tag, the day planner, reminders — and the search matches it: an
item in a note in "Captain's Log" reads "Captain's Log - lock down my diet
RE keto".  It is display only: the stored title, the note's line, the
editor's title field and Google Calendar keep the plain title.  A title that
already starts with the folder's name and a "-" or ":" is left as it is; a
note at the top level gives no context.  While it is on, a folder renamed
in Notes renames the prefix within a second.

**Backups** are on by default: every hour a verified copy of the shared
database is written to `backups/` in the database file's own folder (or a
folder chosen in Settings), named `shared_data-YYYYMMDD-HHMMSSZ.db` in UTC
and keeping the newest 48.  Notes and Tasks share these settings, and
whichever app is running makes the backup when it is due.  **Back Up Now**
in Settings makes one at once; Settings also shows when the last backup was
made and why the last pass failed, if it did — both when a copy was made
but old backups could not be removed.

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
settings, which live in the shared database so that Notes follows them too.
The Settings window (⌘,) holds the appearance options (bold titles, Kanban
shadows, business-day forecast, the note-context title prefix), New Tasks
(the default tag, and whether Quick Task's ⌃⌥⌘T works from any app),
the database's health
and fingerprint, the backups, notifications and Google Calendar; the View
menu holds the rest.  Every key and its default is listed in
`src/task_config.h`.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| ⌘N | New Task (with the selected tag; with a view selected, the default tag) |
| ⌃⌥⌘T | Quick Task — a task with the default tag whatever is selected, its editor in the lower-right corner of the screen; works from any app while Tasks is running |
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
