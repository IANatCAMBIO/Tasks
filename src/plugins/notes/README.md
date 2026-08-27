# Notes Action Items Sync

Mirrors the companion Notes app's action items as ordinary tasks, with
their own notes, subtasks, attachments, pin and priority.

An action item is a `!` line in a note. Tasks lists them through the
`notes` command-line program and keeps a task in step with each one.

## What it does

Mirrored tasks are marked with ❗ in the task list, innermost of the
row's glyphs — nearest the title, because it says what the row **is**
rather than how you have flagged it. The full stack reads
↳ 🚨 ⭐️ ❗ Title.

An **Action Items** view appears in the sidebar, listing every mirrored
item wherever it actually lives. Each row keeps its *in &lt;list&gt;*
line, since that is the only thing saying which list a task really sits
in.

## Who owns what

| Field | Owner |
|---|---|
| Title | **Notes.** A title edited in Tasks is overwritten on the next pass — the item's text belongs to the note it lives in, so edit it there. |
| Done | **Shared.** Ticking a task off is pushed back to Notes; ticking it off in Notes is pulled in. |
| Due date | **Shared**, the same way. |
| Notes, subtasks, attachments, pin, priority | **Tasks only.** These never leave. |

Notes' done flag is **binary**, so the mirror speaks only in whether a
task is Done. Moving a task between *New* and *In Progress* is not a
pending write and never leaves this machine.

Existence is Notes' to decide: an item that leaves Notes tombstones its
task. The reverse has no command, so deleting a mirrored task in Tasks
parks its identity instead — otherwise the very next pass would helpfully
re-create what you just deleted.

## Settings

All in **File → Settings…**, in this plugin's own **Notes** section.

| Key | Default | Meaning |
|---|---|---|
| `notes_plugin_enabled` | `1` | Load the plugin at all. Off means it is never opened — not merely idle. |
| `notes_sync` | `0` | Run the mirror. |
| `notes_cli` | *(unset)* | Path to the `notes` program. Unset searches `PATH`. |
| `notes_embed_list` | *(unset)* | Which list mirrored items are filed into. Unset means a managed **Action Items** list, created on first use. Changing this moves the existing items too. |
| `notes_sync_interval_min` | `5` | Minutes between passes. `0` = only when you press Sync. |
| `notes_meta_row` | `1` | Show the Action Items view in the sidebar. |

The mirror runs **before** the Google Tasks sync, so one press of Sync
carries a new action item all the way to Google rather than taking two.

## Requirements

The `notes` program, from the companion Notes app. Everything goes
through it — never the Notes database file, because Notes' GUI and CLI
share a single-writer design in which command-line calls route through
the running GUI.

One consequence is worth knowing: a call is answered by whichever Notes
instance owns the socket, **not** by the binary on disk. An old GUI left
running will therefore make a new CLI feature look missing.

## Installing

Copy `notes.so` — and this file, if you want the README link to work —
into the `plugins` folder next to the Tasks program, then restart Tasks.

## Notes for plugin authors

This is the larger worked example, next to `overdue`:

- It is **several source files in one module**: `notes.c` is the mirror,
  `bnotes.c` the CLI wrapper. They share one host table and one identity
  through `plugin_ctx.h`.
- It owns **its own tables** (`notes_task`, `notes_deleted`), created from
  its `db_open` hook and reached with the host's generic `exec` /
  `exec_query` / `scalar`. Nothing about Notes is on a core row.
- Its worker runs on the app's **one scheduler**, which owns the timer
  and the database path — so a database switch cannot leave it pointing
  at a file that has moved.
- It spawns a process, so its worker has **its own SQLite connection** and
  marshals results back to the main thread. A connection never crosses
  threads.
- `init()` only registers: no CLI is spawned there. The first pass
  happens on the scheduler's own initial run.

See `src/plugins/notes/` and `src/plugin.h`.
