# Tasks — User Guide

The full reference for Tasks. See [README.md](README.md) to install it and
[BUILD.md](BUILD.md) to build it.

- [Tags](#tags)
- [The day planner and Calendar](#the-day-planner-and-calendar)
- [Reminders](#reminders)
- [Where the data lives](#where-the-data-lives)
- [Working with Notes](#working-with-notes)
- [Cleanup: deleting completed tasks](#cleanup-deleting-completed-tasks)
- [Backups](#backups)
- [Configuration](#configuration)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Branches and other documents](#branches-and-other-documents)

## Tags

**Tags are how tasks are organised**; there are no lists. A task carries
any number of tags, none included: a task without a tag is listed under
All Tasks alone. The sidebar at the left shows three views — **All
Tasks**, **All Favorites** (every task with a favorite tag) and the
**3-Day Forecast** — then every tag, by name, with how many open tasks
carry it (Settings → Appearance → **Show task counts next to tags in the
sidebar**, on by default). Selecting a tag shows every task carrying it,
the way a list would (sorting, Kanban, search, its own manual order); each
row says what it is tagged.

- The editor's **Tags** field gives a task its tags. Typing offers the
  tags that already exist (pick one with the arrow keys or a click);
  Return makes what you typed a tag, a new one when no tag has that name.
  Names ignore case: "work" typed is the existing "Work". Delete a tag's
  token to take it off.
- Select tasks and right-click, **Add Tag ▸** gives them all a tag.
- Right-click a tag in the sidebar, **Edit Tag…**: give it an emoji (a
  click in the emoji slot opens the emoji picker), and make it a favorite
  or not. A tag's name never changes.
- Right-click a tag, **Clear Tag from All Tasks**: after a confirmation
  ("Are you sure you want to remove the tag … from N tasks?") the tag
  comes off every task that carries it, so it leaves this sidebar. It
  stays in the tag lists (Add Tag, a new task) while a note carries it;
  once no task and no note does, it is deleted.
- **Favorite** tags have a button each in the **Favorites bar** above the
  task pane (View → Hide/Show Favorites Bar), and so can the All Tasks,
  All Favorites and 3-Day Forecast views; right-click a tag or a view for
  **Add to Favorites**, a button for **Remove from Favorites**. Drag a
  button along the bar to put the favorites in your order (new ones join
  at the end: views, then tags by name). The sidebar toggles with the
  button at the far left of the toolbar (⌃⌘S); drag its right edge to
  make it wider or narrower, and it keeps that width.
- A subtask is born with its parent's tags. New Task gives the selected
  tag, or none with a view selected; Quick Task gives none.
- **Tags are shared with Notes**: a note can carry the same tags (its Tags
  field and its `#tags`), and an emoji set in either app shows in both.
  The sidebar and the Favorites bar show the tags your tasks carry;
  wherever you choose a tag (the Tags field, Add Tag) every tag is
  offered, Notes' too. Favorites are each app's own.
- A tag exists while a task or a note carries it: when the last one lets
  go — untagged or deleted — the tag goes.
- A task Notes writes carries the tag **❗ Action Items**.

### Migrating from lists

Lists are gone, and so are Notes' folders. Opening an older database for
the first time (schema 100) makes a verified backup, then turns each list
into a tag on its tasks — its emoji kept — each list group into a tag on
the tasks of its lists, and every folder into a tag on the notes in it.
Each action item written in a note in a folder gets the folder's name
written before its title once ("Captain's Log - lock down my diet RE
keto") unless the title already starts with it; from then on titles are
yours. The old per-task favourite star is dropped: favorites are tags.

## Task commands

The menus, the toolbar and the context menu carry the same set: **New
Task**, **Delete**, **Mark Complete**, **High Priority**, **Add Tag ▸**
and **Clear Completed**. **Show / Hide Completed** filters them out of the
pane, **Hide / Show Sidebar** (⌃⌘S) and **Hide / Show Favorites Bar** give
the task pane the window, and **Kanban View / List View** (⇧⌘K) switches
between the two. Recurring tasks roll forward while the app runs.

View → **Hide Subtasks** takes the rows a subtask gets of its own — the
`↳` rows in a tag, in All Favorites and in the 3-Day Forecast, which All
Tasks never shows — out of the list and off the board, so each family
reads as one row. Nothing is lost: a parent's row lists its subtasks as
always (up to four, then "+N more"), they are still searched, and ticking
one off in the editor still promotes the parent to In Progress. Note that
a subtask comes due on its own: with the rows hidden, a subtask due
tomorrow whose parent has no due date is not listed in the 3-Day Forecast
at all.

## The day planner and Calendar

The day planner syncs with the **macOS Calendar**, both ways, and so with
every account set up in the Calendar app (Google, iCloud, Exchange…),
which does the syncing with the service. Tasks has no sign-in of its own.
Meetings in the calendars you choose are drawn in the planner so you can
schedule around them. Tasks you drag onto the planner become events in the
calendar you choose, and moving, stretching or removing them updates the
event. Moving or deleting that event in Calendar (or in Google Calendar,
once Calendar has synced it) updates the planner.

To set it up, open Settings → Calendar and tick **Sync the day planner
with Calendar**. macOS asks once for access to your calendars; then choose
the calendar tasks go to and the calendars to show. Notes has the same
section: the switch and the shown calendars are one setting both apps
share, kept in their shared database. If access was refused, **Open
Privacy Settings…** goes to System Settings → Privacy & Security →
Calendars.

Tasks pushed to Google by the earlier Google sync are found again in the
macOS Calendar by their Google id on the first sync, so none is pushed
twice.

The calendar is the real one even in a `make run` sandbox: leave the sync
off there, or sandbox tasks become events in your calendar.

## Reminders

Tasks reminds you about a task that is **due today** at 8:00 AM, and about
a **scheduled** one ten minutes before it starts. Reminders arrive either
in the app's own **Reminders** panel in the lower-right corner of the
screen, like an Outlook meeting reminder, or through the macOS
Notification Center — Settings → Notifications chooses which.

The panel lists every pending reminder and its title counts them: **Tasks
- 3 Reminders**. Select one to see it in the blue area at the top,
double-click it to open its task, and use **Snooze** to be reminded again
in 5 minutes to 2 hours, **Dismiss** to let it go, or **Mark Complete** to
finish the task from the panel.

A task that is still not done is reminded again at 8:00 AM the next
morning, and goes on being reminded while it stays overdue. Those repeats
do not pile up: a task has at most **one row** in the panel, and a row
reminded more than once carries a red circled count — ③ — of the
reminders standing behind it (its tooltip gives the exact number).
Dismissing the row dismisses all of them.

Closing the panel with its red button only puts it away; the reminders in
it are still pending and the next one brings it back. **Dismiss** or
**Dismiss All** is what clears them.

## Where the data lives

**The tasks** are in the database Notes and Tasks share:

```
~/Library/Application Support/camb.io/
    shared_data.db   the shared database (with its -wal and -shm files)
    location         optional: the path of a database kept elsewhere
    backups/         backups, by default
```

Both apps open the same file and follow the same `location` pointer, which
**File > Open Database File…** writes when you answer **Make Default**.
The file must be on a local disk and not inside iCloud Drive, Dropbox or
another synced folder: the app refuses to open it there. Only a shared
database can be opened; an old Tasks `tasks.db` or a Notes `notes.db` is
refused untouched (as is an empty file: the shared module creates a
missing database complete, in one step), and is brought across with the
shared module's migration tool instead.

**Tasks' own settings** stay in Tasks' folder (attachments are links to
files wherever they are, and are not copied):

```
~/Library/Application Support/Tasks/
    tasks.ini        the settings
```

Setting `TASKS_DATA_DIR` moves Tasks' folder and `CAMBIO_SHARED_DB` names
the database file; `make run`, `make seed`, `make test` and `make ui-check`
set both to places under `dev/`. While `CAMBIO_SHARED_DB` is set, **Make
Default** is refused with the shared module's reason, and backups go to
the folder set for that database or, when none is set, to `backups/`
beside it — never to the per-user folder.

**This app does not share data with the GTK Tasks app** (the
`gtk4-testing` branch) and cannot open that app's folder
(`~/.local/share/tasks`): the database code refuses any path inside it.

## Working with Notes

Changes Notes (or a script) makes to the tasks appear within a second,
keeping the selection and the scroll; a change to notes alone refreshes
only the "From note: …" lines of open editors. An edit never overwrites a
field you did not change, even one you were in the middle of editing. When
a change cannot be saved because another app held the file for too long,
the status bar says so and the panes show what is really stored; an editor
keeps the change, saves it again with the next change or on closing, and
will not close while it cannot.

A task written as an action item in a note shows "From note: …" in its
editor. Its title, completion and due date stay in step with the note's
line; deleting it here — by hand or through
[Cleanup](#cleanup-deleting-completed-tasks) — makes Notes drop the item
mark from the line and keep the text, leaving ✅ in its place when the task
was completed and 🪦 when it was deleted before it was done.

## Cleanup: deleting completed tasks

A completed task is only **hidden** (View > Show Completed): nothing takes
it out of the database, so a file you have used for years holds every task
you ever ticked. The **Cleanup** section of Settings (⌘,) is what removes
them.

**Off until you turn it on.** Tick *Delete completed tasks once they are
old* and set how old under it — a number and days, months or years, six
months by default. Months and years are calendar months and years, so
"6 months" means six months back from today rather than 180 days. The line
under the setting always says what that amounts to right now: how many
completed tasks are already older than the cutoff, and the date they are
older than.

Turning the switch on asks first, naming how many tasks it is about to
delete; answering **Cancel** leaves the switch off rather than putting the
question off until the tasks have gone. After that a pass runs at every
launch and once a day while the app is open. **Clean Up Now** runs one
pass immediately and works with the switch off, so a one-off tidy-up does
not mean turning automatic deletion on. Deleted tasks are **not**
recoverable from the app — they are in your backups and nowhere else.

What a pass takes, and what it leaves:

| | |
|---|---|
| A completed top-level task older than the cutoff | deleted, with its subtasks |
| A completed **subtask** of a task still being worked on | kept — a family goes whole or not at all |
| A **recurring** task sitting completed between occurrences | kept — it comes back round, so it is not finished work |
| A completed task **tagged, pinned or attached** | deleted like any other; its attachments are links, so the files themselves are untouched |
| A completed task with a **day-planner session** | deleted; the calendar event it made is deleted with it on the next sync pass |
| A tag whose last task a pass removes | removed with it, as everywhere else in the app |
| An **action item from a note** | the task is deleted; the note keeps the text, with a ✅ where the `!` was |

**Action items are the special case.** A task created by a `!` line in a
note cannot simply be removed, because the line still names it and Notes
would write it back as a new task. A pass therefore marks it deleted and
leaves the link, and Notes answers that the way it answers any deletion
from Tasks: it takes the `!` off the line and leaves the sentence exactly
as you typed it. Because a cleaned-up task was completed, Notes writes a
**✅** where the `!` was, so the line still reads as finished work rather
than as a sentence someone once wrote. (A task deleted while it was still
open gets 🪦 instead, so a note says afterwards which of its items were
finished and which were given up on. Cleanup only ever takes completed
tasks, so only a deletion by hand in Tasks leaves that one.) The task is gone from Tasks, the writing stays
in the note. The row itself is cleared from the database by the next pass,
once Notes has unmarked the line — so an action item's last trace leaves the
file the first time Notes runs after the cleanup, not before.

The age a task is judged by is when it was last completed. A task
completed by a writer that recorded no completion time is judged by when
it last changed instead, which is never earlier than its completion — so
nothing is ever deleted sooner than the retention allows.

## Backups

**Backups** are on by default: every hour a verified copy of the shared
database is written to `backups/` in the database file's own folder (or a
folder chosen in Settings), named `shared_data-YYYYMMDD-HHMMSSZ.db` in UTC
and keeping the newest 48. Notes and Tasks share these settings, and
whichever app is running makes the backup when it is due. **Back Up Now**
in Settings makes one at once; Settings also shows when the last backup
was made and why the last pass failed, if it did — both when a copy was
made but old backups could not be removed.

## Configuration

Settings are changed in the app and stored in `tasks.ini` (group
`[tasks]`, written by the app; comments are not preserved), apart from the
backup settings, which live in the shared database so that Notes follows
them too. The Settings window (⌘,) holds the appearance options (bold
titles, Kanban shadows, business-day forecast, the sidebar's tag counts),
New Tasks (whether Quick Task's ⌃⌥⌘T works from any app), the database's
health and fingerprint, Cleanup (how long completed tasks are kept), the
backups, notifications and the Calendar; the View menu holds the rest. Every key and its default is listed in
`src/task_config.h`.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| ⌘N | New Task (with the selected tag; with a view selected, none) |
| ⌃⌥⌘T | Quick Task — a task without a tag whatever is selected, its editor in the lower-right corner of the screen; works from any app while Tasks is running |
| ⌘I | Info… — open the task editor |
| ⌘⌫ | Delete the selected task(s) |
| ⌘F | Find — search the selected view |
| ⌃⌘S | Hide / Show Sidebar |
| ⇧⌘K | Kanban View / List View |
| ⌘, | Settings… |
| ⌘W | Close the window (closing the main window quits) |

## Branches and other documents

| Branch | What it is |
|---|---|
| `master` | The AppKit app for macOS. This is the project. |
| `gtk4-testing` | The original GTK4 app (Linux and macOS), kept as a testing branch: the behavioural reference for the port, and a place to try ideas against the GTK code. It is not the product and does not receive AppKit work. |

The code is plain C for the core (with GLib, to be removed in a later
phase) and Objective-C written in a C style for the interface. No Xcode
project, no nibs.

- [BUILD.md](BUILD.md) — toolchain, dependencies, targets, platform notes.
- [porting/INTERFACES.md](porting/INTERFACES.md) — the contract for the UI
  work: frozen headers, ownership rules, the command table, work packages.
- [porting/GTK_PORTING_MAP.md](porting/GTK_PORTING_MAP.md) — what the GTK
  app does, feature by feature.
- [porting/FILES_PLAYBOOK.md](porting/FILES_PLAYBOOK.md) — AppKit
  conventions and lessons from the Files app.
- `porting/gtk_reference/` — the GTK sources and documents, kept as the
  behavioural reference until the port is complete. They describe the GTK
  app, not this one. The full GTK4 app is on the `gtk4-testing` branch.
