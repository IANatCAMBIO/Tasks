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

An **All Action Items** view appears in the sidebar, listing every
mirrored item wherever it actually lives. Each row keeps its *in
&lt;list&gt;* line, since that is the only thing saying which list a task
really sits in. It is *All* Action Items because **Action Items** is also
the name of the list the default rule files into, and the two are
different things: the list is where unclaimed items are *put*, the view
is *everything* mirrored, wherever a rule sent it.

## Filing rules

**Filing rules decide which list each action item lands in** — the whole
answer, in one table. A rule is a piece of text and a list: an item whose
text contains that text is filed in that list.

A rule for `scotia` pointed at a **Scotia** list claims *"Scotia — Ian to
update the configuration"*. Case is ignored and the text can sit
anywhere in the line, so the customer's name does not have to come
first.

The last row is the **default rule**, written *Anything else*: it catches
every item no other rule claims, and starts out pointing at a managed
**Action Items** list created the first time the mirror runs. It cannot
be **removed** — something has to catch the rest — but it is otherwise an
ordinary row: send it to whatever list you like, and if you type a
pattern into it, it becomes a normal rule and a fresh *Anything else*
appears below it, keeping the same list.

- **The first rule that matches wins**, reading the table top down.
  `scotia` and `scotia bank` can plainly both claim one item, so put the
  one you mean higher up — **Move Up** and **Move Down** are how you say
  which. Matching stops at the first hit.
- **A rule moves the items already mirrored**, not just the next new one.
  Writing a rule for work you have already collected is the whole point
  of having one.
- **A move you make by hand sticks.** Nothing re-files anything until you
  change a rule — or until Notes rewords the item, since its text is what
  a rule matches on.
- A rule whose **list has been deleted** files nothing, and says so in
  the table so it can be repointed or removed. The default rule instead
  falls back to the managed list, because its items have to go
  somewhere.

Edit them in **File → Settings… → Notes**: both cells are edited in
place, with **Add Rule**, **Remove Rule**, **Move Up** and **Move Down**
below. A rule you add but never type into is never saved, and emptying an
existing rule's text deletes it — *Anything else* is the one row allowed
to have no text of its own, and emptying that one changes nothing.

Rules live in the **database**, not in `tasks.ini`, because a rule names
a list by its id and list ids belong to the file the lists live in — kept
in the ini they would point at whatever happened to hold that id after a
database switch.

## Who owns what

| Field | Owner |
|---|---|
| Title | **Shared.** Renaming a task is pushed back into its '!' line in the note, keeping the item's due date and done state; renaming it in Notes is pulled in. A rename made here wins a tie. |
| Done | **Shared.** Ticking a task off is pushed back to Notes; ticking it off in Notes is pulled in. |
| Due date | **Shared**, the same way. |
| Notes, subtasks, attachments, pin, priority | **Tasks only.** These never leave. |

Notes' done flag is **binary**, so the mirror speaks only in whether a
task is Done. Moving a task between *New* and *In Progress* is not a
pending write and never leaves this machine.

**Existence is Notes' to decide, and that is the whole rule.** What the
listing holds is what the mirror holds: an item that leaves Notes
tombstones its task, and an item still in Notes gets a task back on the
next pass even if you deleted it here. There is no hidden list of items
Tasks is declining to mirror, so "why is this action item not in Tasks?"
never has an invisible answer.

So the mirror is not a place to tidy up: **tick an action item off in
Notes** and the task follows. Deleting the task in Tasks is undone within
the sync interval. Note that a completed action item still has a task —
if you keep completed tasks hidden (the toolbar's completed-visibility
toggle), a list of finished action items reads as an empty one.

## Settings

All in **File → Settings…**, in this plugin's own **Notes** section.

| Key | Default | Meaning |
|---|---|---|
| `notes_plugin_enabled` | `1` | Load the plugin at all. Off means it is never opened — not merely idle. |
| `notes_sync` | `0` | Run the mirror. |
| `notes_cli` | *(unset)* | Path to the `notes` program, or a bare name to search `PATH`. Unset means `notes` on `PATH`. **Notes binary path** in Settings sets it — type or paste a path (a line of `which -a notes` does nicely), or press **Browse…** to find it on disk. |
| `notes_sync_interval_min` | `5` | Minutes between passes. `0` = only when you press Sync. |
| `notes_meta_row` | `1` | Show the All Action Items view in the sidebar. |
| `notes_toolbar_button` | `1` | Show the **Sync Notes** button in the toolbar. **Notes → Sync Now** does the same thing and stays either way — this setting is about the toolbar, not about whether you can sync by hand. |

Where mirrored items are filed is **not** an ini key: it is the filing
rules table above, in the database.

## Syncing by hand

**Notes → Sync Now** in the menu bar runs a pass immediately, and a
**Sync Notes** toolbar button does the same (composition.png, left of
Google's). Both call one function, so they cannot come to mean different
things; the button greys while a pass is in flight and comes back when it
ends, however it was started.

The mirror runs **before** the Google Tasks sync, so one press of each
carries a new action item all the way to Google rather than taking two —
which is also why the Notes button sits to the LEFT of Google's and the
Notes menu before it in the bar: the toolbar reads in the order the two
passes actually run.

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

- It is **several source files in one module**: `notes_to_tasks.c` is the
  mirror, `notes_api.c` the CLI wrapper — and the split is enforced, not
  just intended. `notes_api.c` names no `TaskDatabase`, no GTK and no
  sqlite; `notes_to_tasks.c` names no `g_spawn`, no `g_strsplit` and no
  `argv`. One file knows how to talk to the Notes program, the other
  knows what to do with the answers, so a change to the CLI's wire format
  lands in 186 lines. They share one host table and one identity
  through `plugin_ctx.h`.
- It owns **its own tables** (`notes_task`, `notes_rule`), created from
  its `db_open` hook and reached with the host's generic `exec` /
  `exec_query` / `scalar`. Nothing about Notes is on a core row. The same
  hook DROPS `notes_deleted`, which it used to own: a table nothing reads
  is worse than no table, because the next person to open the database
  has to work out which. `notes_rule` is also the example of a setting that
  belongs in the **database** rather than the ini: it references a list
  by id, so it has to travel with the file those ids mean something in.
  It replaced an ini key and a combo of its own — two places answering
  one question, one of them not in the table that claimed to say.
- Its worker runs on the app's **one scheduler**, which owns the timer
  and the database path — so a database switch cannot leave it pointing
  at a file that has moved.
- It spawns a process, so its worker has **its own SQLite connection** and
  marshals results back to the main thread. A connection never crosses
  threads.
- `init()` only registers: no CLI is spawned there. The first pass
  happens on the scheduler's own initial run.

See `src/plugins/notes/` and `src/plugin.h`.
