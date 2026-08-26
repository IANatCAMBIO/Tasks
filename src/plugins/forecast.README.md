# Weekly Forecast

This week at a glance: seven day sections, Sunday through Saturday, each
listing everything due on that day. Today's heading carries a blue dot
and a *— Today* tag.

The whole week scrolls as one page — the day sections are never
individually scrolled, so nothing is hidden inside a small box. A day
with nothing due still shows a single dimmed *No tasks due* row, so the
week always reads as seven days.

## How it differs from the other views

Most sidebar views are a list of tasks that the app renders — as a list,
or as a Kanban board if you have that on. This one is a **panel**: it
supplies its own layout, so turning Kanban on does not disturb it, and
leaving the forecast puts the board back.

Two consequences worth knowing:

- The day lists keep **no selection**. Seven lists would otherwise each
  hold their own, leaving up to seven highlighted rows at once. Ticking a
  checkbox and double-clicking to open a task both work without one, but
  **Delete Task has nothing to act on** while the forecast is showing.
- There is no drag-reorder here. Manual and Kanban ordering are orderings
  *of a task list*, and this is a calendar.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `forecast_plugin_enabled` | `1` | Load the plugin at all. Off means it is never opened, not merely hidden. |
| `forecast_show_row` | `1` | Keep it loaded but hide its sidebar row. |

The second has a checkbox in **File → Settings… → Weekly Forecast**; the
first is in the **Plugins** list just above it.

> Before this shipped as a plugin the setting was called
> `weekly_forecast`. That key is no longer read. Both default to on, so
> there is nothing to do unless you had deliberately turned the view off.

## Installing

Copy `forecast.so` and this file into the plugins folder shown in
**File → Settings… → Plugins**, then restart Tasks.

## Notes for plugin authors

This is the worked example for a **panel view**, and for reusing the
host's task rows:

- `panel_new` builds the widget; `panel_refresh` refills it. State lives
  on the widget via `g_object_set_data_full`, not in a file static — a
  static would quietly assume there is only ever one panel.
- Every row comes from `host->rows->*`. Nothing here draws a task cell by
  hand, which is what keeps these rows identical to the main task list
  instead of drifting from it.
- ONE `TaskRowCtx` is built for the whole week and shared across all
  seven stores. Seven contexts would be seven times the queries for the
  same answers.
- The ✓ column calls `host->rows->toggle_done` rather than writing a
  status, so this checkbox means exactly what every other checkbox in the
  app means.

See `src/plugins/forecast.c` and `src/plugin.h`.
