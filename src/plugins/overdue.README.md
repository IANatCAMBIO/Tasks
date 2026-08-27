# Overdue

A sidebar view of every task past its due date, across every list,
soonest deadline first.

## What it shows

A task appears here when it has a due date and that date is earlier than
today. Tasks with no due date never appear: "no deadline" is not the same
as "missed one".

Each row keeps its *in &lt;list&gt;* line, because the view gathers tasks
from everywhere and that line is the only thing saying where a task
actually lives. Completed tasks follow the app's own **Show Completed**
setting, like every other view.

The row sits between **Due Today** and **Weekly Forecast**.

## Settings

One, and it is the plugin's own checkbox in **File → Settings… →
Plugins**:

| Key | Default | Meaning |
|---|---|---|
| `overdue_plugin_enabled` | `1` | Load the plugin. Off means it is never opened — not merely hidden — and its sidebar row goes with it, immediately. |

There is deliberately **no settings section** for this plugin. It used to
carry an `overdue_show_row` key with a section of its own, which did the
same thing as the checkbox above; that key is no longer read. A plugin
contributes a settings section only if it has something to configure
beyond being switched on.

## Installing

Copy `overdue.so` — and this file, if you want the README link to work —
into the `plugins` folder next to the Tasks program, then restart Tasks.

## Notes for plugin authors

This plugin is deliberately small, and doubles as the worked example for
the plugin API:

- It imports **nothing** from the host. Everything it does goes through
  the `TaskHostApi` table handed to `task_plugin_entry()`.
- Its config keys are namespaced automatically by its id, so asking for
  `show_row` reads `overdue_show_row` without the plugin needing to know
  what other keys exist.
- It is a **query view**: it answers with an array of tasks and the host
  renders them with the same code as every other list, so it gets the
  striping, the ✓ column, manual sort and the Kanban board for free.
- `init()` only registers. It opens no file, spawns no process and
  touches no network — `init()` runs before the window is shown, so
  anything slow there is startup the user waits through.

See `src/plugins/overdue.c` and `src/plugin.h`.
