# Tasks — User Guide

Everyday use of Tasks: the library, the task editor, settings,
storage, Google Tasks sync, and the Notes integration. For build
instructions and OAuth client setup see the [README](README.md); for
the database schema and the sync engine see [Internals](Internals.md).

## Library window

- **Sidebar** — bold rolled-up views at the top: **⭐️ Favorites**
  (present only while something is favorited), **🔮 All Tasks**,
  **☀️ Due Today** (rolling over at local midnight; Settings can widen
  it to include everything past due) and **🌤️ Weekly Forecast** —
  the current week, Sunday through Saturday, as seven day lists
  stacked down the page, each headed by its day and date (today is
  marked) and showing the tasks due that day; check off tasks in
  place or double-click to edit. The Weekly Forecast view can be
  switched off in Settings → Appearance, which removes it from the
  sidebar entirely. Your
  real lists nest under a collapsible **Lists** header, each shown
  with its emoji when one is set. Lists sort alphabetically until you
  drag one into place — from then on your custom order sticks (it is
  local-only; Google Tasks has no list order to sync). One honest
  caveat if you launch from a terminal on macOS: dragging a list may
  print a `Gdk-CRITICAL … gdk_atom_intern` line on the console. It is
  harmless noise from a bug in the Mac build of the GTK library
  itself, not in Tasks — the drag works fine. Right-click to manage
  lists: the **Lists** header offers *New List* and *New Group*, and a
  list offers *Edit List* (name plus an emoji picker), *Delete List*,
  *Move to Group* and *Remove from Group*; double-clicking a list opens
  that same Edit dialog. *File → New List…* does the same thing.
  The sidebar starts hidden; the toolbar's **Sidebar** button (or
  *View → Show Sidebar*) toggles it and the choice persists.
- **List groups** — lists can be filed under named groups, which show
  as their own expandable rows under the **Lists** header. Right-click
  a group to *Rename Group* or *Remove Group* (removing the group keeps
  its lists — they just move back up to the top level). Selecting a
  group row doesn't change the task pane; it is only a container.
  Expansion state, for the Lists header and each group, is remembered
  across refreshes and forced open when your selected list is inside.
- **Task rows** are tall: title (optionally bold — see Settings),
  notes preview, attachment count, and up to four subtask lines with
  their own checkboxes rendered inline. Tasks marked **High
  Priority** (a checkbox in the editor, or the right-click menu) sort
  to the top of every list they appear in and wear a 🚨 beside the
  title. The flag is local to Tasks — Google Tasks has no priority, so
  it never syncs. A task mirrored from a Notes action item wears a
  ❗ beside its title in every view, so it is always clear which tasks
  came from your notes. Columns: a done checkbox, the task, the status,
  and the due date; right-click any column header to hide or show the
  Done, Status, Due Date and Completion Date columns (the Task column
  always stays). **Status** — *New*, *In Progress* or *Done* — starts
  hidden, because the checkbox beside each task is the same thing seen
  as a tick: a task shows ticked exactly when its status is Done.
  Ticking the box sets the status to Done; unticking a task that was
  ticked sets it to *In Progress*, on the reasoning that a task you had
  marked finished has plainly been started. *New* is set from the
  editor's Status dropdown. Turn the Status column on to sort by it —
  it sorts New → In Progress → Done, in that order rather than
  alphabetically. Favoriting is done from the
  editor window's **Favorite** checkbox or the task's right-click menu.
  Rows alternate white/light-blue; click the Due header to sort
  (soonest first, undated rows last). Due dates are color-coded: green
  while the date is still ahead, gold on the day itself, red once it
  has passed.
- **Manual Sort** — the toolbar's sort-mode toggle (or *View → Manual
  Sort*) switches the task pane to hand ordering: a ⠿ handle column
  appears and you drag rows into the order you want. The order is
  remembered per view — each list, All Tasks, Favorites, Due Today and
  the Notes section keep their own — and is local to Tasks.
- **Toolbar** — the Sidebar toggle, then Sync, a visibility toggle that
  shows or hides completed tasks (it applies to every view, Notes
  items included), the Manual Sort toggle, then New Task and Delete
  Task. At the far right, the logo button opens the About dialog
  (program info plus live database statistics). Button style —
  icons, icons + text, or text — is set in Settings or by
  right-clicking an empty spot on the toolbar. The Sync button can be
  hidden in Settings → Google Tasks.
- **Status bar** — the left side describes the current view and
  selection; the right side shows the latest event message (a sync
  result, a save failure), which fades out after a few seconds.
- **Kanban View** (*View → Kanban View*) — shows whatever you have
  selected as a board of three lanes, **New**, **In Progress** and
  **Done**, instead of a list. Each task becomes a card, and each lane
  is headed by its name and a count.
  - **Drag a card from one lane to another to change its status.**
    Dropping a card on *Done* completes the task exactly as ticking its
    checkbox would, and dropping it back on *In Progress* reopens it.
  - **Drag a card up or down within a lane to reorder it**, and drag
    between lanes to land at a particular position rather than just "in
    that column". While you drag, the lane you are over is tinted and a
    blue bar shows the exact slot the card will drop into, so you can put
    something at the very top or bottom of a column deliberately. The
    order is remembered per view, separately from the list view's
    *Manual Sort* order — rearranging a board never disturbs a list you
    have hand-sorted.
  - The pointer turns into an open hand over a card and closes while you
    drag it; the card you are carrying follows the pointer as a
    see-through copy, and the original stays dimmed in place until you
    drop. Press **Escape** mid-drag to abandon it, changing nothing.
  - **Double-click a card** to open its editor, the same as
    double-clicking a row. A single click selects the card, which is
    what **Delete Task** acts on while the board is up.
  - The board follows your sidebar selection, so it works for a single
    list and for the rolled-up views (*All Tasks*, *Favorites*, *Due
    Today*, *Action Items*) alike. **Weekly Forecast** is the one
    exception — it keeps its seven-day panel, since a week of dated days
    is not something three status lanes can express. Leave the forecast
    and the board comes back.
  - *Show Completed* still applies: with completed tasks hidden the Done
    lane is empty. It stays on screen so you can still drag onto it —
    the card just disappears once it lands, the same as a row does in
    the list.
  - The *Manual Sort* toggle governs the LIST view only. The board is
    always drag-orderable, and keeps its own order — the two never
    overwrite each other.
  - The choice persists between launches.
- **View menu** — *Show Completed* and *Manual Sort* mirror their
  toolbar buttons, *Show Sidebar* mirrors the Sidebar toggle, and
  *Compact Layout* strips the window down to the task list: the whole
  toolbar and the sidebar go away, and a small floating bar with just
  **New Task** and **Delete Task** sits 20 px in from the bottom-right
  corner. Turning Compact Layout back off restores the toolbar and
  puts the sidebar back the way you had it; *Show Sidebar* still works
  while compact if you want the lists pane over the task list. The
  choice persists between launches.
- **Multi-select** (Cmd/Shift-click) for bulk actions via the
  right-click menu: mark complete or incomplete, favorite or
  unfavorite, set or clear **High Priority**, **Move to List**, Delete.
  With a single task selected the favorite and priority items show only
  the direction that applies. **Open in Google Tasks** opens a single
  selected task in the browser (for tasks that have synced).
- **Double-click a task** to open its editor window.
- Menus: *File → New Task*, *New List…*, *Sync Now*, *Clear Completed
  Tasks*, *Open Database File…*, *Settings…*, *About*, and *Quit*. With
  gtk-mac-integration built in, the menu moves into the native macOS
  menu bar.

## Editor windows

Every task opens in its own window, centered on the screen, with a
standard titlebar (no GNOME header bars anywhere) — and only one
window per task: opening it again focuses the one you already have.

- **Fields** — title, **Status** (a *New* / *In Progress* / *Done*
  dropdown — the only place *New* can be chosen outright), **Favorite**,
  **High Priority**, and a
  due date you can type (`YYYY-MM-DD`) or pick from the calendar button.
  The entry is forgiving: while it holds a partial or invalid date
  nothing is clobbered — the stored date only changes once the text
  parses.
- **Notes** — free multiline text below the field row, eight lines tall
  to start with; it scrolls past that, and grows if you enlarge the
  window.
- **Advanced** — Subtasks and Attachments start folded away behind the
  **Advanced ▾** link at the bottom-left; clicking it drops the window
  open to show both, and clicking again folds it back to the size it
  was. A task that already has subtasks or attachments opens expanded,
  so you never have to click to see what is already there.
- **Save** sits under the notes box, at its right edge, in every editor:
  it closes the window (your edits are already saved as you type, so it
  is a "done here" button, not the only way to persist). Closing the
  window with its close box does exactly the same thing.
- **New Task** windows add **Cancel** to the right of Save. **Cancel
  closes the window and deletes the task again** — the "never mind"
  button for a task you just created. Only new tasks get it; nothing in
  an existing task's editor can delete it.
- **Subtasks** — add, rename, toggle and remove; exactly one level
  (subtasks cannot have subtasks, and Google Tasks agrees).
- **Attachments** — file references (add/remove/open); the files stay
  where they are, and the references never leave the machine.
- **From Google** — a read-only section for synced tasks showing
  what Google knows and the app doesn't edit: the completion time,
  a Docs/Chat assignment origin, and any Google-attached links (for
  example the Gmail message a task was created from).
- **Autosave** — edits persist about half a second after you stop
  typing; the Status dropdown and the Favorite and High Priority
  checkboxes save immediately.

## Settings (*File → Settings…*)

- **Appearance** — toolbar button style (icons / icons + text /
  text), bold task titles in the list, show/hide the Weekly Forecast
  sidebar row, whether **Due Today** also includes everything past due,
  and — when built with gtk-mac-integration — a native macOS menu bar
  option.
- **Database** — shows the current database file path and lets you
  move it to a different folder. Switching always removes the old
  file: if the target folder is empty the current database is copied
  there; if it already contains a database you choose whether to use
  the existing one or overwrite it with your current data.
- **Notes** — mirror its action items as ordinary tasks, point the
  app at the `notes` command (a path or a name on PATH), choose which
  list new items are filed into, set how often changes are sent back,
  and show or hide the sidebar's Action Items view.
- **Google Tasks** — the sync master switch, Sign In / Sign Out, the
  auto-sync interval in minutes (default 5; 0 turns the timer off while
  the toolbar Sync button always works), and whether the Sync button
  appears in the toolbar at all.

All changes apply live and persist (in `tasks.ini` next to the
binary). Toolbar icons are PNGs bundled in `icons/` — replaceable by
dropping in files.

## Storage

Everything lives in a single SQLite database:

- `~/.local/share/tasks/tasks.db` (GLib's user-data directory).
  Any standard SQLite tool can read it — the schema is documented in
  [Internals](Internals.md). Back it up by copying the file while the
  app is closed.
- Settings live in `tasks.ini` next to the binary (portable mode),
  falling back to `~/.config/tasks/` when that directory is not
  writable; it is seeded from `tasks.ini.defaults` on first launch
  and rewritten by the app as you change things.

### Backups

*Settings → Database* can keep rotating backups of your database. It is
**off** by default.

Turn on **Back up the database automatically** and the app writes a
verified copy named `tasks-YYYYMMDD-HHMMSS.db` on your chosen interval,
keeping only the most recent few. Set the interval to `0` to back up only
when you press **Back Up Now**.

If you don't choose a folder, backups go to the default database location
in your home directory (`~/.local/share/tasks`) — so switching it on
always does something. **Choose Folder…** points them somewhere else.

Three things worth knowing:

- **Point it at a disk independent of wherever your database lives.** If
  the database is in iCloud Drive (or Dropbox, or a network share) and
  the backups are too, one mishap can take both. An external drive is the
  strongest choice; the home-directory default is already independent of
  a synced database. If the backup folder ends up being the *same* folder
  as the database, Settings says so — it is still a real, separate,
  verified file, but it cannot survive losing that folder.
- **It cannot fill your disk.** The "keeping N files" setting is a hard
  cap, and old backups are only deleted *after* a new one has been
  written and verified — so a spell of failing backups can never eat the
  history you already have.
- **An idle app writes nothing.** A backup pass whose database has not
  changed since the last one does nothing at all.

Every backup is a complete, ordinary SQLite database. To restore one,
quit the app, and either copy it over your `tasks.db` or point *Settings
→ Database* at a folder containing it renamed to `tasks.db`.

### Upgrading from Lists (version 3.x)

The app used to be called **Lists**, and its files were named after it.
Version 4.0 carries them over on the first launch — you should not have
to do anything, and you stay signed in to Google:

- `lists.db` is **renamed** to `tasks.db`, wherever it lives (including
  a custom folder you chose in Settings → Database).
- `lists.ini` is **copied** to `tasks.ini`, and the settings inside it
  are moved from the old `[lists]` section to `[tasks]`. Your Google
  sign-in, your database location and every preference come with it.
  The original `lists.ini` is deliberately left where it is, untouched,
  as a fallback; a copy of the file as it stood just before the section
  was rewritten is saved as `tasks.ini.pre-4.0.bak`. **Both hold your
  Google refresh token**, so treat them like passwords — don't put them
  somewhere shared.
- Each of these happens only when the new file is not already there, so
  it cannot run twice or overwrite work you have done since.

If the database rename fails (a permissions problem, or a sync folder
that has the file locked), the app says so on the console and opens
your old `lists.db` where it is rather than starting empty. Nothing is
lost — quit, fix the cause, and relaunch.

An upgrade from before 3.0, when the app was **Hacienda**, still works
too, with one difference: it needs a fresh Google sign-in, because that
version's saved token belonged to a different Google client and Google
will not honor it.

## Google Tasks sync

Sign in once (see the [README](README.md) for the OAuth client setup
if you built the app yourself); after that a periodic auto-sync runs
while signed in, and *File → Sync Now* or the toolbar button run one
on demand. The GUI stays live throughout — sync happens on a worker
thread.

What maps: tasklists ↔ lists, tasks ↔ tasks (with the same single
level of subtasks), due date ↔ due date, and the *Done* status ↔
completed. Google's own status is only ever "completed" or "not
completed", so *New* and *In Progress* both reach it as not completed
and the difference between the two stays on this machine. Changing the
status still counts as changing the task, though — the row is marked as
edited and picked up by the next sync like any other change, even when
the only thing Google ends up hearing is what it already knew. Four
things are **local-only** and never leave the machine: Favorite flags,
High Priority flags, list emoji, and attachments — Google Tasks has no
equivalent. Your list order and any manual task order are local-only
too. Two honest API caveats, confirmed against the docs: Google's
`due` field is date-only (a time of day would be discarded), and there
is no starring or priority field to map Favorite/High Priority onto.

How it behaves:

- After the first full pass, syncs are **incremental** — only items
  changed since the last pass transfer.
- **Absence never deletes.** Only explicit deletes propagate: delete
  a task here and it disappears from Google; delete it on Google and
  it disappears here. A task deleted on Google *without* a trace
  (say, by another client while you were offline) is pushed back
  rather than silently dropped.
- Conflicts resolve **newest-wins** per item; a deletion beats a
  concurrent edit. Edits are etag-guarded, so a push that lost the
  race defers to the remote copy and the next pull reconciles.
- **Clear Completed Tasks** uses the server-side clear when signed
  in; **Move to List** uses the server-side move (falling back to
  delete-and-recreate when offline). Tasks cleared on Google's side
  stay cleared — they are never resurrected here.
- Google's default tasklist cannot be deleted (their rule, enforced
  by their API); Tasks refuses up front rather than failing
  mid-sync.

Signing out drops the tokens; the grant can also be revoked at
myaccount.google.com/permissions at any time.

## Notes action items

If you keep meeting notes in
[Notes](https://github.com/IANatCAMBIO/Records), its `!`
action items can live here as **ordinary tasks**. Enable the
integration in Settings and Tasks mirrors each item into a real list —
the managed **Action Items** list by default, or any list you pick
under "Mirror action items into".

Because they are ordinary tasks, they behave like everything else:
notes, subtasks, attachments, Favorites, High Priority, moving them to
another list, and Google Tasks sync all work. Nothing is locked.

- **Where they are** — "Mirror action items into" picks the list they
  live in; changing it moves the items you already have, not just the
  next new one. Moving a single item to another list yourself sticks —
  it is only overridden the next time you change that setting. The
  sidebar's **Action Items** row is a view, not a list: it gathers
  every mirrored item wherever it has ended up, so nothing gets lost.
  Turn the row off in Settings → Notes if you don't want it.
- **What flows back to Notes** — ticking an item done and changing
  its due date, because those are the only two things the Notes
  command line can write. The item's **text belongs to the note**: edit
  it in Notes, not here, or your change will be overwritten the next
  time the two sync.
- **When it flows back** — not instantly. Changes are cached and sent
  in a batch on the interval in Settings → Notes ("Sync action items
  every N minutes", 5 by default; set 0 to only sync when you press
  Sync). If Notes can't be reached, your change simply waits and goes
  out on a later pass — it is never dropped.
- **Deleting** — an item deleted in Notes disappears from Tasks on
  the next pass, along with any notes or subtasks you attached to it.
  Deleting the task in Tasks keeps it deleted: it will not come back on
  the next sync, even though the item still exists in Notes.
- Everything goes through the `notes` command-line interface —
  never its database file — so a running Notes GUI and Tasks
  cooperate safely (the CLI forwards to the GUI over its socket).
  This needs a current version of Notes; against an older one Tasks
  says so and leaves your tasks alone rather than guessing which item
  is which.
