# Tasks AppKit

An experimental macOS-only fork of [Tasks](https://github.com/IANatCAMBIO/Tasks):
the same task manager — lists, tall task rows, a Kanban board, subtasks, due
dates, recurring tasks, all in one SQLite file — with the GTK4 interface being
replaced by a native AppKit one.

**Status: foundation only.**  The non-UI core is carried over and runs under
the Cocoa run loop; the AppKit interface is a working skeleton with
placeholder panes.  What works today:

- the main window: toolbar, search field, sidebar (flat list of views and
  lists), task list (tall rows, read-only), status bar, the standard menus;
- task commands from the menus, the toolbar and the context menu: new,
  delete, mark complete, favourite, high priority, move to list, clear
  completed; show/hide completed; show/hide sidebar; list/board toggle;
- recurring tasks roll forward while the app runs.

Not yet ported (placeholders): the Kanban board, the task editor, Settings,
list groups and list editing in the sidebar, sorting and manual order in the
list, Compact Controls, Open Database File.  The plan and the interfaces the
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

## Build and try it

```sh
make          # builds TasksAppKit.app
make seed     # creates a test database in dev/data
make run      # runs the app against dev/data
make test     # core tests
```

`make run` never touches real data: it points the app at `dev/data` inside
the source tree.

## Install

```sh
make install  # copies TasksAppKit.app to ~/Applications
```

A running installed copy is asked to quit first and reopened afterwards.

## Where the data lives

Everything the app keeps is in one folder:

```
~/Library/Application Support/TasksAppKit/
    tasks.db        the database
    tasks.ini       the settings
    backups/        rotating backups, when switched on
```

On first run the folder and an empty database are created.

Setting the environment variable `TASKS_DATA_DIR` to another folder moves all
of it there; that is what `make run`, `make seed` and `make test` do.

**The fork does not share data with the GTK Tasks app** and cannot open that
app's folder (`~/.local/share/tasks`): the database code refuses any path
inside it.  This is deliberate while the fork is experimental.  To try the
fork on a copy of real tasks, copy `tasks.db` yourself into another folder
and point `TASKS_DATA_DIR` at it.

## Configuration

Settings are changed in the app and stored in `tasks.ini` (group `[tasks]`,
written by the app; comments are not preserved).  The Settings window is not
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
  app, not this fork.

## License

BSD 3-Clause — see [LICENSE](LICENSE).
