# Tasks — User Guide

Everyday use of Tasks: the library, the task editor, settings and
storage. For build instructions see the [README](README.md); for the
database schema see [Internals](Internals.md).

## Library window

- **Sidebar** — bold rolled-up views at the top: **⭐️ Favorites**
  (present only while something is favorited), **🔮 All Tasks** and
  **☀️ Due Today** (rolling over at local midnight; Settings can widen
  it to include everything past due). Your
  real lists nest under a collapsible **Lists** header, each shown
  with its emoji when one is set. Lists sort alphabetically until you
  drag one into place — from then on your custom order sticks. One honest
  caveat if you launch from a terminal on macOS: dragging a list may
  print a `Gdk-CRITICAL … gdk_atom_intern` line on the console. It is
  harmless noise from a bug in the Mac build of the GTK library
  itself, not in Tasks — the drag works fine. Right-click to manage
  lists: the **Lists** header offers *New List* and *New Group*, and a
  list offers *Edit List* (name plus an emoji picker), *Delete List*,
  *Move to Group* and *Remove from Group*; double-clicking a list opens
  that same Edit dialog. *File → New List…* does the same thing.
  The sidebar starts hidden; the toolbar's **Sidebar** button (or
  *View → Show Sidebar*, which reads *Hide Sidebar* once it is up)
  toggles it and the choice persists.
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
  title. Columns: a done checkbox, the task, the status,
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
  remembered per view — each list, All Tasks, Favorites and Due Today
  keep their own.
- **Toolbar** — New Task and Delete Task, then past a divider the Sidebar
  toggle, a visibility toggle that shows or hides completed tasks (it
  applies to every view), the Manual Sort toggle, and the **pane toggle** — the
  twin of *View → Kanban View / List View*.
  For the pane toggle the picture is always the view you
  will get. On the list it shows the list icon turned on its side, three
  columns standing up, and takes you to the Kanban board; on the board it
  shows the same icon upright, a list again, and brings the list back. The
  menu item and this button change together. At the far right is the
  **search box** — see *Searching* below. The toolbar is icons only,
  with a tooltip on every button saying more than a one-word label
  would; there is nothing to configure about its appearance. The About
  dialog (program info plus live database statistics) is at *File →
  About*.
- **Status bar** — the left side describes the current view and
  selection; the right side shows the latest event message (a backup
  result, a save failure), which fades out after a few seconds.
- **Kanban View** (*View → Kanban View*; the same item reads *List View*
  while the board is up, and takes you back) — shows whatever you have
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
  - Each card has a small **⠿ grip** down its left edge, and that is where
    drags start — just like the handle column in the list's Manual Sort.
    The pointer turns into an open hand over the grip and closes while you
    drag; everywhere else on the card the pointer stays a normal arrow, so
    a click selects and a double-click opens. The card you are carrying
    follows the pointer as a see-through copy, and the original stays
    dimmed in place until you drop. Press **Escape** mid-drag to abandon
    it, changing nothing.
  - **Double-click a card** to open its editor, the same as
    double-clicking a row. A single click selects the card, which is
    what **Delete Task** acts on while the board is up.
  - **Select several cards** the way you would rows: **Cmd-click**
    (Ctrl-click on Linux) adds a card to the selection or takes it back
    out, and **Shift-click** selects everything between the last card you
    clicked and this one, within that lane. Selected cards are
    highlighted.
  - **Right-click a card** for the same menu a task row gives you — Info,
    mark complete or incomplete, favorite, high priority, Move to List,
    Delete. With several cards selected it acts on
    all of them and says so (*Delete 3 Tasks*), and right-clicking inside
    a selection keeps that selection rather than collapsing it onto the
    one card. **Info** stays single-card.
  - **Dragging works on the whole selection too**: grip any one of the
    selected cards and they all travel together, with a badge on the card
    you are carrying showing how many. Drop them and they all take the
    lane's status, landing at the slot you chose in the order they were
    in.
  - The board follows your sidebar selection, so it works for a single
    list and for the rolled-up views (*All Tasks*, *Favorites*, *Due
    Today*) alike.
  - The completed-visibility setting still applies: with completed
    tasks hidden the Done lane is empty. It stays on screen so you can still drag onto it —
    the card just disappears once it lands, the same as a row does in
    the list.
  - The *Manual Sort* toggle governs the LIST view only. The board is
    always drag-orderable, and keeps its own order — the two never
    overwrite each other.
  - The choice persists between launches.
- **View menu** — every item here is named for what clicking it will
  *do*, not for the state you are in, so there is no checkbox to
  interpret: the menu always reads as an offer.
  Top to bottom, the first two are what the task list shows —
  **Hide Completed** / **Show Completed**, and the sort toggle, which
  reads **Manual Sorting** while sorting is automatic (by column header)
  and **Automatic Sorting** while you are dragging rows by hand. Both
  mirror
  their toolbar buttons, and the label changes whichever control you
  use. The sort toggle greys out while the Kanban board is on, since the
  board is always drag-sorted and keeps its own order.
  Below a divider are the three that change the window itself —
  **Hide Sidebar** / **Show Sidebar**, **Compact Controls** / **Full
  Controls**, and **Kanban View** / **List View**, which swaps the task
  pane between the list and the board. The sidebar item mirrors the
  Sidebar toggle, and **Compact Controls** strips the window down to the
  task list: the whole toolbar goes away and a small floating bar with
  just **New Task** and **Delete Task** sits 20 px in from the
  bottom-right corner. **Full Controls** brings the toolbar back. The
  sidebar is left alone either way — it follows the sidebar item in both
  modes, so you can keep the lists pane over the task list while
  compact. The choice persists between launches.
- **Multi-select** (Cmd/Shift-click) for bulk actions via the
  right-click menu: mark complete or incomplete, favorite or
  unfavorite, set or clear **High Priority**, **Move to List**, Delete.
  With a single task selected the favorite and priority items show only
  the direction that applies.
- **Ticking a subtask starts its parent** — completing any subtask moves
  the parent task from *New* to *In Progress* on its own, since work has
  plainly begun. A parent already *In Progress* stays there, and a parent
  you have already marked *Done* is left alone: finishing one more subtask
  is progress, not a reason to reopen it. Unticking a subtask does not
  move the parent either — that is your call to make.
- **Searching** — the box at the right of the toolbar filters the view
  you are already in, in place. It does not open a window or add a
  sidebar row, which is why it says *Search this view*: **All Tasks** is
  a view like any other, so selecting that and typing is how you search
  everything. It looks at each task's title, its notes and its subtasks'
  titles; a subtask match brings up its parent, since the pane lists
  top-level tasks. Words **and** together rather than or, `"a quoted
  phrase"` counts as one term, and `-word` excludes. The `-` only means
  *exclude* at the start of a word, so *well-known* searches for the
  hyphen. Case and accents are ignored. The status bar counts the
  matches, the Kanban board filters exactly the same way, and hand
  sorting is suspended while a search is up — a drag then would only see
  the matches and would throw away the position of everything hidden.
  Clear the box to bring it back.
- **Double-click a task** to open its editor window.
- Menus: *File → New Task*, *New List…*, *Clear Completed
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
  parses. A task you have completed also shows **when**, at the right of
  the checkbox row — "Completed Aug 27, 2026" while it is Done, and
  "Last completed Aug 27, 2026" once you put it back to *New* or *In
  Progress*. The date is kept for good: reopening a task does not
  un-complete it, and completing it again simply moves the date on. The
  optional **Completion Date** column in the task list shows the same
  thing.
- **Notes** — free multiline text below the field row, eight lines tall
  to start with; it scrolls past that, and grows if you enlarge the
  window.
- **Advanced** — Subtasks, Attachments and Recurrence start folded away
  behind the **Advanced ▾** link at the bottom-left; clicking it drops
  the window open to show all three, and clicking again folds it back to
  the size it was. A task that already has subtasks, attachments or a
  repeat opens expanded, so you never have to click to see what is
  already there.
- **Save** sits under the notes box, at its right edge, in every editor:
  it closes the window (your edits are already saved as you type, so it
  is a "done here" button, not the only way to persist). Closing the
  window with its close box does exactly the same thing.
- **New Task** windows add **Cancel** to the right of Save. **Cancel
  closes the window and deletes the task again** — the "never mind"
  button for a task you just created. Only new tasks get it; nothing in
  an existing task's editor can delete it.
- **Recurrence** — the last section, at the foot of the window: make the
  task come back round. It opens with **Repeat this task**; tick it and
  the schedule appears underneath — two rows saying when the task
  repeats, read top to bottom the way you would say it out loud, then one
  set slightly apart saying what happens to a completed task beforehand:

  **Starting `YYYY-MM-DD` at `HH:MM`** is the anchor — the "Monday" and
  the "9:00 AM" of *every Monday at 9:00 AM*. The date box is its own
  calendar (click it, there is no separate button), and leaving it
  **empty** is the ordinary case: the schedule then anchors on the task's
  own due date. A start still in the future is the *first* repeat, not a
  week after it. The time is **08:00** unless you change it; a repeat
  measured in days, weeks, months or years lands on it every time, and
  one measured in minutes or hours starts from it and steps on from
  there.

  **Repeat every `N` minutes/hours/days/weeks/months/years** is the
  schedule itself. There are no preset choices to pick from — this one
  row says any of them.

  **Reset to New … beforehand** is how the repeat actually reaches you:
  that long before each repeat, a task you have already **completed** is
  put back to *New* and its due date moves to the repeat. A week by
  default, or half the repeat when that is shorter, so *every hour*
  starts you at 30 minutes rather than a week. A task you have *not*
  finished keeps whatever status it has — the due date still rolls
  forward, but *New* stays *New* and *In Progress* stays *In Progress*.

  The dimmed lines underneath always say what the schedule is and where
  it lands next ("Every Monday at 9:00 AM" / "Next Sep 14, 2026 — resets
  to New Sep 7, 2026 at 9:00 AM"). If the head start will not fit inside
  the repeat — five days ahead of a repeat that comes every three — it is
  shortened to fit and the line says so.

  There is nothing in *Settings* for any of this, and nothing to switch
  repeats on with: a schedule set here *is* the instruction to act on it.
  Tasks works out when the next repeat falls due and wakes up then, so a
  database with nothing recurring in it has nothing running in the
  background, and a repeat that came round while Tasks was closed is
  applied at the next launch.

  Missed repeats are caught up, not replayed — coming
  back to Tasks after a fortnight away rolls a daily task forward once,
  to today, rather than fourteen times.
- **Subtasks** — add, rename, toggle and remove; exactly one level
  (subtasks cannot have subtasks).
- **Attachments** — file references (add/remove/open); the files stay
  where they are, and the references never leave the machine.
- **Autosave** — edits persist about half a second after you stop
  typing; the Status dropdown and the Favorite and High Priority
  checkboxes save immediately.

## Settings (*File → Settings…*)

- **Appearance** — bold task titles in the list, drop shadows on Kanban
  cards, whether **Due Today** also includes everything past due, and —
  when built with gtk-mac-integration — a native macOS menu bar option.
- **Database** — what the database currently is: whether it passed its
  health check and when, the file it lives in, how many tasks and lists
  it holds, its size on disk and its SHA-256, with **Update** to re-read
  all of it. The same two checks run automatically every time Tasks
  opens — there is no switch for that, and nothing is said about it
  unless something is wrong. There is nothing here for *moving* the
  file: your database lives at
  `~/.local/share/tasks/tasks.db`, and to work from one somewhere else
  you open it with *File → Open Database File…* and choose **Set as
  Default** to keep using it at the next launch.

All changes apply live and persist (in `tasks.ini` — see
[Storage](#storage) for where that is). Toolbar icons are PNGs bundled in
`icons/` — replaceable by dropping in files.

## Storage

One directory holds everything Tasks keeps for you:
**`~/.local/share/tasks`** (GLib's user-data directory). There is one
place to back up and one place to look.

- `~/.local/share/tasks/tasks.db` — your tasks. Any standard SQLite tool
  can read it; the schema is documented in
  [Internals](Internals.md). Back it up by copying the file while the
  app is closed.
- `~/.local/share/tasks/tasks.ini` — your settings, seeded from
  `tasks.ini.defaults` on first launch and rewritten by the app as you
  change things.
**Portable mode.** If there is no `tasks.ini` in that directory but there
*is* one next to the binary, the one beside the binary is used and
written to instead — so a copy of Tasks on a USB stick, or a source tree
you are working in, keeps the settings it came with. The app looks in the
shared directory first, falls back to the one beside the binary, and only
creates a new file in the shared directory when neither exists.

To move an existing setup into the shared directory, quit Tasks and move
`tasks.ini` there yourself; nothing is copied automatically.

### Backups

*Settings → Database* can keep rotating backups of your database. It is
**off** by default.

Turn on **Back up the database automatically** and the app writes a
verified copy named `tasks-YYYYMMDD-HHMMSS.db` on your chosen interval,
keeping only the most recent few. Set the interval to `0` to back up only
when you press **Back Up Now**.

If you don't pick a folder, backups go to
`~/.local/share/tasks/backups` — so switching it on always does
something. That keeps them out of the way of your database, but it is
*beside* it rather than away from it, which Settings tells you, and it is
the arrangement you most want to change: **Change Folder…** points them
somewhere else.

Three things worth knowing:

- **Point it somewhere your database is not.** A backup's whole value is
  being in a different place from the file it copies, and by default it
  is not: the backups sit in a folder inside `~/.local/share/tasks`,
  which is where the database lives, and a folder goes with its parent.
  Settings says so whenever the backups are inside the database's own
  directory — they are still real, separate, verified files, but they
  cannot survive losing that folder, and losing a folder is the ordinary
  way this goes wrong. An external drive is the strongest
  choice; a synced folder (iCloud Drive, Dropbox, a network share) is a
  good one *provided the database is not in it too*, since one mishap
  would otherwise take both.
- **It cannot fill your disk.** The "keeping N files" setting is a hard
  cap, and old backups are only deleted *after* a new one has been
  written and verified — so a spell of failing backups can never eat the
  history you already have.
- **An idle app writes nothing.** A backup pass whose database has not
  changed since the last one does nothing at all.

Every backup is a complete, ordinary SQLite database. To restore one,
quit the app, and either copy it over your `tasks.db` or point *Settings
→ Database* at a folder containing it renamed to `tasks.db`.
