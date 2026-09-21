# GTK4 migration — plan, inventory and running state

This file is the durable state of the GTK3 → GTK4 port of Tasks.  It exists
because the port spans many sessions and no single session's context
survives it: **every session starts by reading this file and ends by
updating it**.  The Decisions section is the important one — it is where
the API mapping choices live, so that the seventh session ports a dialog
the same way the second one did.

Notes, the companion app, made the same port first
(`~/salt_development/notes/GTK4_MIGRATION.md`, decisions D1–D39, merged
2026-09-15).  Where a Notes decision transfers verbatim it is recorded
below as INHERITED and not re-measured; where Tasks differs, the
difference is measured here.

Written 2026-09-21 from a survey of the GTK3 code.

## Why, and why not

GTK3 is in maintenance and GTK4 is where the toolkit's work lands; the two
apps are about to share one database, which is easier on one toolkit
generation.  Two things to keep in view:

- **The renderer buys nothing measurable.**  Nothing in this app's
  measured costs is drawing — the row builder's date arithmetic, the
  board's CSS blur, SQLite — and on a VM with software GL, GTK4 renders
  SLOWER than GTK3's cairo path and gets run with `GSK_RENDERER=cairo`.
- **Memory goes UP, not down.**  Measured on this Mac on 2026-09-21: Tasks
  (GTK3) idles at 25 MB resident, Notes (GTK4, cairo renderer) at 35 MB.
  The toolkit generation is the whole difference; no choice below moves
  it.  Anything sold as "GTK4 is lighter" is wrong.

## Decisions taken with the user before the port started (2026-09-21)

- **Plugins are OUT.**  The dlopen ABI, the loader and all four plugins
  (gtasks, notes mirror, forecast, overdue) were deleted in Phase 0.
  Their features return LATER as integrated core code, after this port;
  the shared Notes database comes after that.  Neither is in scope here.
- **Straight to GtkColumnView / GtkListView.**  No deprecated GTK4 API
  anywhere; the build stays `-Wdeprecated-declarations` clean with NO
  suppression, as Notes is since its D33.  Notes tried the deprecated
  GtkTreeView first and reversed it within two days (its D1 → D33).
- **Drags stay HAND-ROLLED, on `GtkGestureDrag`.**  In-process, no native
  drag surface, identical on X11/Wayland/macOS.  GTK4 native DnD
  (`GtkDragSource`/`GtkDropTarget`) is not used anywhere: a drag never
  leaves the window, and native DnD on macOS hands the gesture to an
  `NSDraggingSession` (gotcha 19 again) and allocates a drag surface per
  drag.
- **`gtk4` branch in a worktree** (`../tasks-gtk4`).  `master` stays a
  working GTK3 app until the branch runs.

## Ground rules for every session

1. Read this file.  Read CLAUDE.md, but see "Gotchas that do not carry
   over" below before applying any of its GTK gotchas to GTK4 code.
2. Work on the `gtk4` branch in its worktree, never on `master`.  THIS
   FILE is edited on `gtk4` only.
3. Do ONE checklist item, `make` clean under `-Wall -Wextra`, tick the box,
   add every non-obvious mapping to Decisions, commit.  Then `/clear`.
   Compaction summaries lose exactly the decisions this file must keep.
4. A re-derived gotcha is written into this file the day it is found, in
   the same "measured, not assumed" style as CLAUDE.md's.
5. GTK3 and GTK4 cannot link into one process, so the branch does not run
   from the start of Phase 3 until its end.  Expect to compile
   everything and run nothing in between.
6. Runtime checks go through `make run-dev` (a throwaway data directory
   under `dev/`, never `~/.local/share/tasks`) and `tools/winshot.sh`
   (captures the window by id, whichever Space it is on).

## Inventory — what the survey found (after Phase 0)

16.7 k lines.  The GTK-free model layer — `db`, `recur`, `backup`,
`search`, `task_ops`, `task_worker`, `task_view`, `core_views` — is
5.4 k lines and is untouched by the port.

| File | Lines | What breaks |
|---|---|---|
| `library_window.c` | 2.0 k | toolbar, menubar + 3 context menus, 5 `gtk_dialog_run`, `configure-event`, status fade, compact float bar, `themed_bg_css_apply` (`gtk_style_context_lookup_color`, no GTK4 equivalent) |
| `sidebar.c` | 1.0 k | `GtkTreeStore` + `GtkTreeView`, `button-press-event` context menu, emoji entry's `"gtk-emoji-chooser"` private hookup, 2 dialogs |
| `task_list.c` | 0.9 k | `GtkListStore` + `GtkTreeView`, 6 cell data funcs, header right-click via `gtk_tree_view_column_get_button`, manual-sort drag on `GtkTreeRowReference` + `gtk_list_store_move_*`, quartz header flattening |
| `kanban.c` | 1.9 k | the whole drag: `gdk_seat_grab`, `GTK_WINDOW_POPUP` ghost, `gdk_window_get_origin`, `x_root`, `gtk_widget_draw` + `"draw"`, RGBA visual, `grab-broken-event`; 3 `GtkEventBox`; `gtk_container_get_children` walks |
| `editor_window.c` | 2.3 k | 2 tree views (subtasks, attachments), 3 `GtkComboBoxText`, `GtkCalendar` API, `no_show_all` disclosure blocks + `gtk_widget_get_preferred_height`, `gtk_offscreen_window_new` probe, 2 `gtk_dialog_run`, `gtk_show_uri_on_window` |
| `settings_window.c` | 1.3 k | `GtkFileChooserDialog` + `gtk_dialog_run`, `GtkClipboard`, monitor-relative placement, `gtk_bin_get_child` |
| `app.c` / `main.c` | 1.0 k | `gtk_dialog_run` in `task_app_notice`/`_confirm` and the first-run flow (`gtk_init_check` before the GtkApplication), pixbuf → cairo icon pipeline, `GtkToolItem` factory, `GTK_OVERLAY_SCROLLING` env |
| `task_rows.c` | 0.6 k | builds `GtkListStore` rows; the completed-row fade animates a store row |

Removed in GTK4 (no shim), with the site counts:

| GTK3 | Sites | GTK4 |
|---|---|---|
| `GtkToolbar` / `GtkToolItem` | 34 | `GtkBox.toolbar` + `GtkButton`/`GtkToggleButton` |
| `GtkMenu` / `GtkMenuBar` / `GtkMenuItem` | ~200 | `GMenu` + `GAction` + `GtkPopoverMenu(Bar)` |
| `gtk_dialog_run` | 14 | async: `GtkAlertDialog`, `GtkFileDialog`, a `dialog_new` scaffold |
| `*-event` signals | 28 | `GtkGestureClick`, `GtkEventControllerKey/Motion`, `notify::default-width` |
| `GtkEventBox` | 15 | the widget itself + controllers |
| `gdk_seat_grab`, `GTK_WINDOW_POPUP`, `gdk_window_get_origin`, `x_root` | ~55 | `GtkGestureDrag` (implicit grab), overlay ghost, `gtk_widget_compute_point/bounds` |
| `gtk_container_*` / `pack_start` / `show_all` / `no_show_all` | ~200 | per-parent append, `set_visible` |
| `GtkTreeView` family incl. cell data funcs | ~500 | `GtkColumnView` / `GtkListView` over `GListStore` |
| `gtk_style_context_lookup_color` | 1 | none — CSS names `@theme_bg_color` directly |
| `gtk_offscreen_window_new` | 1 | none — delete the probe |
| `GTK_OVERLAY_SCROLLING` env | 1 | the `gtk-overlay-scrolling` GtkSettings property (verified in the 4.22 gir) |
| `gdk_pixbuf_rotate_simple` + cairo surface icons | 2 | icon theme + a pre-rotated `board.png` |

Survives deprecated in 4.10+ and is NOT to be used: `GtkTreeView`,
`GtkListStore`, `GtkTreeStore`, `GtkDialog`, `GtkComboBoxText`,
`GtkFileChooserDialog`, `gdk_texture_new_for_pixbuf` (4.20).

Survives undeprecated: `GtkTextView`, `GtkOverlay`, `GtkPaned`,
`GtkCalendar` (new `GDateTime` API), `GtkSearchEntry`, `GtkAboutDialog`,
`GtkSpinButton`, `GtkSizeGroup`, `GtkCheckButton` (no longer a
GtkToggleButton), `GtkApplication` (already in use).

## Lost, or changed on purpose

1. **Native macOS menubar via gtk-mac-integration** → GTK's own quartz
   backend (`gtk_application_set_menubar`; Notes D9).  The
   `native_menubar` setting, `HAVE_GTKOSX` and CLAUDE.md gotcha 29 go in
   Phase 2.
2. **Column-header flattening on quartz** (`header_button_flatten`) —
   deleted; there is no header button to restyle in a column view and no
   `lookup_color` to resolve the plate with.  Re-check the look under
   Adwaita on Linux, which never had it.
3. **Settings window placement against the parent's monitor** — deleted;
   GTK4 has no window positioning (Notes brought editors' placement back
   on macOS only via the NSWindow, D39; Tasks does not position anything).
4. **The completed-row fade** in the task list (20 alpha steps over a
   store row) — deleted; the row goes on the next refresh.
5. **Live-swap manual sort** — becomes marker + move on release, one
   helper shared with the sidebar's list reorder.  The `GtkTreeRowReference`
   pair and the flicker lock go.
6. **Due-date tint** — set when the row binds rather than at every paint,
   so it rolls over at the next refresh instead of the next redraw
   (Notes accepted the same).
7. **Sidebar list drag-reorder** — CLAUDE.md describes a custom DnD dest
   protocol (quirk 13, gotcha 12) that the GTK3 code no longer contains
   (checked 2026-09-21: no `enable_model_drag_*` anywhere).  Lists are
   reordered by `task_db_lists_reorder` when it returns; nothing to port.

## Gotchas that do not carry over

CLAUDE.md's gotchas were derived against GTK3.  On this branch:

| Gotcha | On GTK4 |
|---|---|
| 1 (popovers inside the toplevel) | GONE: GTK4 popovers are their own surfaces.  The emoji dialog's grow-and-shrink goes. |
| 3 (per-popup GtkMenu leak) | GONE with GtkMenu; popovers are unparented on `closed` from an idle (Notes D14). |
| 4 (clearing a store zeroes the scrollbar) | Re-measure with GListStore splice; `scroll_keep` stays until measured unnecessary. |
| 5 (`set_enable_search`) | GONE with GtkTreeView. |
| 9 (`popup-context-menu` on the toolbar) | GONE: the toolbar style menu was removed in GTK3 already. |
| 10, 12, 13 (quartz AX geometry, atom warning, default-list tombstone) | 10 irrelevant (window captured by id now); 12 GONE (no GdkAtom); 13 is sync, not in this build. |
| 15 (`show_all` early-returns on `no_show_all`) | GONE: both flags are gone; visibility is explicit.  The height rule it protected (adv_box outside the natural height) is re-stated: the fold's window sizing must be re-measured (Phase 4). |
| 18, 30 (windowed GtkEventBox ignores padding, clips box-shadow) | GONE: no windowed widgets.  `pad_widget` and the shadow WRAPPER can go — re-verify the shadow paints on a plain box before deleting `card_of`. |
| 19 (GTK DnD delegates to the platform) | Still true and still the reason the drag is hand-rolled; the mechanism is `GtkGestureDrag` now. |
| 20 (window opacity is a compositor feature) | GONE for the ghost: it is an overlay child, and `gtk_widget_set_opacity` on a non-toplevel is a render-node opacity. |
| 23, 25, 27, 31 (GLib) | Carry over unchanged. |
| 28 (measure a container after its text is final) | Carries over: `gtk_widget_measure` reads the same thing. |
| 29 (`gtkosx_application_sync_menubar` crash) | GONE with gtk-mac-integration. |
| 32 (combo vs entry plate height per theme) | Re-measure: GtkDropDown is a different widget.  `theme_field_height` and its offscreen window are deleted first; re-derive only if a mismatch shows. |

## Phases

### Phase 0 — plugin removal — DONE 2026-09-21 (9b86b50)

- [x] `src/plugins/`, the ABI, the loader, `plugin_owner`, `task_ui` deleted
- [x] Contributed chrome, panel views, row decorations, op hooks and list
      vetoes, delete hooks and generic query helpers, namespaced config,
      Settings plugin section, editor contributed sections, scheduler
      owner sweeps and run-all: all deleted
- [x] Makefile, ini defaults, .gitignore, README, Internals, User Guide
- [x] Builds clean; runs against an empty sandbox (fresh schema v12)

### Phase 1 — scaffolding — DONE 2026-09-21

- [x] This file
- [x] `make run-dev` + `tools/winshot.sh`
- [x] `library_window.c` split into `library_window.c`, `sidebar.c`,
      `task_list.c`, `kanban.c` with `library_priv.h` (mechanical, verified
      running)

### Phase 2 — actions and menus — DONE 2026-09-21 (GTK3-legal; the branch runs)

GLib `GAction`/`GMenu` render on GTK3, so this landed and was tested
before the toolkit flips.  Design: Notes CLAUDE.md "Actions, menus and
shortcuts"; the record here is the "Actions" section of library_priv.h.

- [x] `app.` commands on the application for the menubar, `win.` on the
      library window (now a GtkApplicationWindow) for the toolbar and the
      context menus; both tables bind the same functions.  No accelerators.
- [x] Menubar as one `GMenu` → `gtk_application_set_menubar`;
      gtk-mac-integration, `HAVE_GTKOSX`, `native_menubar`, gotcha 29 deleted
- [x] The five View items as `hidden-when=action-disabled` PAIRS, enabled
      by the single appliers (`lib_menu_pair_sync`)
- [x] Context menus as per-popup `GMenu`s through `task_app_menu_popup`
      (GTK3 body `gtk_menu_new_from_model`); the task actions act on the
      live selection, the group actions carry the group id as a target
- [x] Column-header hide/show as stateful `win.column-<key>` actions
- [x] Verified: the native macOS bar read back through System Events shows
      the app menu GTK builds (About / Settings… / Quit), File with its one
      separator, and View with exactly one face per pair

### Phase 3 — the per-file GTK4 pass — DONE 2026-09-21

Makefile first: `PKGS := gtk4 sqlite3`, no deprecation suppression.  Then
one agent per file against the mapping table below, each verified with
`make build/<name>.o`, joined by one `make`.

- [x] Makefile
- [x] `app.c`, `main.c` (+ the shared helpers below, ported FIRST)
- [x] `list_rows.[ch]` (new: the item objects, `task_row_factory_new`,
      `task_row_touch`)
- [x] `task_rows.c`
- [x] `settings_window.c`
- [x] `editor_window.c`
- [x] `sidebar.c`
- [x] `task_list.c`
- [x] `kanban.c`
- [x] `library_window.c`
- [x] `make` clean; branch compiles (333 KB binary, libgtk-4 only, no
      libgtk-3 or gtk-mac-integration); `make run-dev` opens the window

### Phase 4 — runtime rounds in the sandbox

- [ ] first run, both branches
- [ ] sidebar: expand / select / groups / reorder
- [ ] task list: sort, header menu, stripes, ✓, double-click, multi-select
      context menu, manual sort + persistence
- [ ] board: select, multi-select, drag, drop, Escape, cancel, shadow toggle
- [ ] editor: fold round trip EXACT, status dropdown, due + calendar,
      recurrence rows, subtask promoting the parent
- [ ] settings: backup flow, health check
- [ ] compact layout, search filter, window size persistence, quit

### Phase 5 — packaging, docs, tests, merge

- [ ] `BUILD.md`, README dependency blocks, `make install`
- [ ] `make test`: `tests/test_core.c` over the GTK-free layer
- [ ] CLAUDE.md rewritten for GTK4
- [ ] Debian build; XFCE runtime if a display is available (REQUIRED — this
      is the Linux-first app and Notes' port was never run there)
- [ ] Merge `gtk4` → `master`

## Port recipe — the per-file pass

Rules for the pass:

1. Read CLAUDE.md first — its gotchas are wrong for GTK4 exactly where the
   table above says.  Keep the house style: banner comment on every
   function, `snake_case`, K&R, no dead code, no near-duplicates.
2. Port the whole file: delete the GTK3 path in the same change, never
   leave both.  Where a behaviour cannot exist in GTK4, delete it and say
   so in the report; do not emulate.
3. NO deprecated API, no `G_GNUC_BEGIN_IGNORE_DEPRECATIONS`, no
   `-Wno-deprecated-declarations`.  `make build/<name>.o` must be
   `-Wall -Wextra` clean.
4. Never run the binary.  Nothing links until every file is done.
5. Report: what changed, every deletion of behaviour, every place you were
   unsure, and any header change you NEEDED (you may not make one — say so
   and stub locally).

Shared helpers, ported first into `app.[ch]` and `list_rows.[ch]`, copied
from Notes (`src/app.c`, `src/list_rows.c` there):

| Helper | From Notes | Replaces |
|---|---|---|
| `task_app_menu_popup(attach, model, x, y)` | `on_app_menu_popup` (D14) | `gtk_menu_popup_at_pointer` + `selection-done` |
| `task_app_double_click_watch` | `on_app_double_click_watch` (D34) | `GDK_2BUTTON_PRESS` |
| `task_app_select_on_press` | `on_app_select_on_press` (D37) | tree-view press selection |
| `task_app_set_tooltip` | `on_app_set_tooltip` (D32/D35) | `gtk_widget_set_tooltip_text` |
| `task_app_notice` | `on_app_notice` | `gtk_message_dialog_new` + run |
| `task_app_confirm(parent, title, fmt, …, TaskConfirmFn, data)` | `confirm` + `ConfirmCtx` | the blocking `gboolean` return |
| `task_app_pick_path` | `on_app_pick_path` | `GtkFileChooserDialog` + run |
| `dialog_new` scaffold | `library_window.c:1508-1552` there | `GtkDialog` + run |
| `task_row_factory_new` / `task_row_touch` | `on_row_factory_new` / `on_row_touch` | cell data funcs, `gtk_list_store_set` |
| `task_app_icon_image_sized` | `on_app_icon_image_sized` | pixbuf → cairo surface |
| `task_app_tool_item_new` | `on_app_tool_item_new` | `GtkToolButton` |

Mapping (GTK3 → GTK4):

| GTK3 | GTK4 |
|---|---|
| `gtk_widget_show_all`, `gtk_widget_set_no_show_all` | delete (visible by default; a widget an updater owns gets `gtk_widget_set_visible(w, FALSE)` at build) |
| `gtk_container_add(parent, c)` | `gtk_box_append`, `gtk_window_set_child`, `gtk_scrolled_window_set_child`, `gtk_frame_set_child`, `gtk_overlay_set_child`/`add_overlay`, `gtk_button_set_child`, `gtk_grid_attach` — by parent type |
| `gtk_box_pack_start(b, c, expand, fill, pad)` | `gtk_box_append` (or `prepend`) + `gtk_widget_set_hexpand/vexpand(c, expand)` + margins for `pad` |
| `gtk_container_get_children` walks | `gtk_widget_get_first_child` + `get_next_sibling` (no GList to free) |
| `gtk_container_set_border_width` | margins on the child |
| `gtk_paned_pack1/pack2(p, c, resize, shrink)` | `gtk_paned_set_start_child/end_child` + `set_resize_*`/`set_shrink_*` |
| `gtk_widget_destroy(w)` | toplevel: `gtk_window_destroy`; child: the parent's remove, or `gtk_widget_unparent` for a widget you parented yourself |
| `gtk_window_new(GTK_WINDOW_TOPLEVEL)` | `gtk_window_new()`; the library is a `GtkApplicationWindow` |
| `GTK_WINDOW_POPUP` (the ghost) | a `GtkFixed` overlay child of the paned's GtkOverlay, `can_target` FALSE |
| `gtk_window_move`, `get_position`, `gdk_monitor_get_workarea` | delete |
| `gtk_dialog_run` | never.  Notices → `task_app_notice`; confirmations → `task_app_confirm` + continuation; file/folder picks → `task_app_pick_path`; custom-widget dialogs (list, group name, calendar) → `dialog_new` + response continuation, the caller's tail moves into the callback, its state carried as object data on the dialog |
| `gtk_about_dialog_new` + run | same dialog, `gtk_window_present`; logo via `gtk_about_dialog_set_logo(GdkPaintable)` |
| `"button-press-event"`/`"button-release-event"` | `GtkGestureClick` (`gtk_gesture_single_set_button` 0 = any); `pressed(n_press, x, y)`; right button = `gtk_gesture_single_get_current_button() == GDK_BUTTON_SECONDARY`; modifiers `gtk_event_controller_get_current_event_state`; `gtk_gesture_set_state(CLAIMED)` where the old handler returned TRUE |
| `GDK_2BUTTON_PRESS` | `task_app_double_click_watch` (D34) — never `n_press` |
| `"key-press-event"` | `GtkEventControllerKey` `key-pressed(keyval, keycode, state)` → TRUE to stop; CAPTURE phase on the window where it must run before the focus widget |
| `"motion-notify-event"`, `enter/leave` | `GtkEventControllerMotion` |
| `"focus-out-event"` | `GtkEventControllerFocus` `leave` |
| `"configure-event"` (size persistence) | `notify::default-width` / `notify::default-height` on the window; read with `gtk_window_get_default_size` |
| `"style-updated"` | delete with `themed_bg_css_apply` — CSS names `@theme_bg_color` and `shade()` directly (4.22 parses both; an undefined colour renders TRANSPARENT, not an error) |
| `"draw"` + cairo (the ghost) | `GtkPicture` of `gdk_paintable_get_current_image(gtk_widget_paintable_new(card))` — a STATIC snapshot; badge as a `GtkLabel` in a `GtkOverlay` |
| `"grab-broken-event"` | `GtkGesture::cancel` |
| `gdk_seat_grab` / `ungrab` | delete: an active `GtkGestureDrag` owns the sequence |
| `gtk_drag_check_threshold` | unchanged, fed from `drag-update`'s offsets |
| `gdk_window_get_origin` + `gtk_widget_get_allocation` hit tests | `gtk_widget_compute_bounds(w, overlay, &rect)` in the overlay's coordinate space |
| `gtk_widget_translate_coordinates` | `gtk_widget_compute_point` |
| `gtk_widget_get_toplevel` | `gtk_widget_get_root` |
| `gdk_window_set_cursor`, `gdk_cursor_new_from_name` | `gtk_widget_set_cursor_from_name(w, "grab"/"grabbing"/"ns-resize"/NULL)` on the widget that should show it |
| `gtk_widget_get_modifier_mask(MODIFY/EXTEND_SELECTION)` | `GDK_CONTROL_MASK` (+ `GDK_META_MASK` on `__APPLE__`) / `GDK_SHIFT_MASK`, as Notes' `select_press_modifiers` |
| `gtk_menu_new` + items + `popup_at_pointer` | `GMenu` + `task_app_menu_popup` |
| `gtk_menu_bar_new` | `gtk_application_set_menubar(model)` |
| `GtkToolbar`, `GtkToolItem`, separators | `GtkBox` + `gtk_widget_add_css_class("toolbar")`; `gtk_separator_new(VERTICAL)`; expanding spacer = any widget with `hexpand`; `gtk_widget_set_focus_on_click(FALSE)` on the buttons |
| `GtkToggleToolButton` | `GtkToggleButton` |
| `GtkEventBox` | the child itself (a `GtkBox`/`GtkLabel`) + controllers |
| `gtk_widget_set_events`, `gtk_widget_add_events` | delete |
| `gtk_widget_get_allocation` | `gtk_widget_get_width/height`, `gtk_widget_compute_bounds` |
| `gtk_widget_get_preferred_height` | `gtk_widget_measure(w, VERTICAL, -1, &min, &nat, NULL, NULL)` |
| `gtk_offscreen_window_new` probe | delete (gotcha 32 re-measure) |
| `gtk_label_set_line_wrap` | `gtk_label_set_wrap` |
| `gtk_entry_get_text/set_text` | `gtk_editable_get_text/set_text` |
| `gtk_entry_set_width_chars` | unchanged, but Adwaita floors `min-width` (settings `small_spin` measured this on GTK3; re-measure, the selector is `spinbutton > text` now) |
| `gtk_toggle_button_get/set_active` on a check button | `gtk_check_button_get/set_active` |
| `GtkComboBoxText` | `GtkDropDown` (`gtk_drop_down_new_from_strings`, `notify::selected`, `gtk_drop_down_get_selected`) |
| `gtk_calendar_select_month/day`, `gtk_calendar_get_date(c, &y, &m, &d)` | `gtk_calendar_select_day(c, GDateTime)`, `GDateTime *gtk_calendar_get_date(c)` |
| `"gtk-emoji-chooser"` object data + resize dance | `gtk_entry_set_input_hints(GTK_INPUT_HINT_EMOJI)` + `insert-emoji`; delete the resize |
| `GtkClipboard` | `gdk_clipboard_set_text(gtk_widget_get_clipboard(w), s)` |
| `gtk_show_uri_on_window` | `gtk_show_uri(window, uri, GDK_CURRENT_TIME)` (void) |
| `gtk_bin_get_child` | `gtk_widget_get_first_child` |
| `gtk_frame_set_shadow_type`, `gtk_scrolled_window_set_shadow_type` | delete (`gtk_scrolled_window_set_has_frame` where a frame is wanted) |
| `gtk_scrolled_window_new(NULL, NULL)` | `gtk_scrolled_window_new()` |
| `gtk_button_set_relief(NONE)` | `gtk_button_set_has_frame(FALSE)` |
| `gtk_css_provider_load_from_data(p, s, -1, NULL)` | `gtk_css_provider_load_from_string(p, s)` |
| `gtk_style_context_add_provider_for_screen` | `_for_display(gdk_display_get_default(), …)`, ONE stylesheet per module, rules on `task-*` classes |
| `gtk_widget_get_style_context` + `add_class`/`remove_class` | `gtk_widget_add_css_class` / `remove_css_class` |
| `task_app_widget_add_css` (per-widget provider) | delete; classes + the module stylesheet |
| `gtk_icon_theme_get_default` + `prepend_search_path` | `gtk_icon_theme_get_for_display` + `add_search_path` (`icons/` as an UNTHEMED path — GTK 4.22 scans a search path's top level by basename, verified in Notes) |
| `gtk_window_set_default_icon_from_file` | copy `document.png` to `icons/theme/hicolor/512x512/apps/tasks.png`, `gtk_window_set_default_icon_name("tasks")` |
| `gdk_pixbuf_new_from_file_at_size` + `gdk_cairo_surface_create_from_pixbuf` + `gtk_image_new_from_surface` | `gtk_image_new_from_icon_name` + `gtk_image_set_pixel_size` |
| `gdk_pixbuf_rotate_simple` (the pane button's board face) | a DERIVED FILE `icons/board.png` (menu.png a quarter turn clockwise, made once, like add.png from remove.png); `"task-icon-rotation"` data goes |
| `GTK_OVERLAY_SCROLLING=0` env | `g_object_set(gtk_settings_get_default(), "gtk-overlay-scrolling", FALSE, NULL)` in `startup` — one call reaches every scroller |
| `gtk_init_check` + `gtk_dialog_run` before the GtkApplication (first run) | the Notes chain (`main.c:246-350` there): `g_application_hold`, async dialogs, both branches converge on one continuation that opens the db and the window; every exit releases.  The db open moves into `activate` |
| `GtkTreeView` + `GtkListStore` (task list) | `GtkColumnView` over `GListStore` of `TaskRow` → `GtkSortListModel` driven by the view's sorter → `GtkMultiSelection`; columns from `task_row_factory_new`; `GtkCustomSorter` per column on the row's raw field; stripes by CSS `row:nth-child(even):not(:selected)`; header menu via `gtk_column_view_column_set_header_menu` over the stateful actions; row-wide controllers on EVERY cell (D33) |
| `GtkTreeStore` + `GtkTreeView` (sidebar) | `GtkListView` over `GtkTreeListModel` of `TaskSbRow` {kind, id, name, emoji, children}; `GtkSingleSelection` with `can_unselect` FALSE; header rows unselectable by REVERTING (Notes `on_sidebar_selection_changed`); expansion snapshot keyed by kind+id; startup fit from `map` |
| `gtk_list_store_reorder` (manual order) | `g_list_store_splice` with the permuted array |
| `GtkTreeRowReference` + `gtk_list_store_move_*` (manual-sort drag) | ONE helper `task_reorder_drag(view, store, on_done)` shared by the list and the sidebar: `GtkGestureDrag` on the handle, `gtk_widget_pick` to find the row, `drop-before`/`drop-after` CSS class as the marker, splice on release |
| editor subtasks / attachments tree views | plain `GtkBox` rows (check + `GtkEntry` + up/down/remove; label + open/remove) — tens of rows, no factory |
| `task_rows_toggle_done(store, iter)` | `task_rows_toggle_done(app, row)` on the `TaskRow` object |
| the completed-row fade (`start_fade`) | delete |
| `on_library_destroy` | may touch only what it holds a reference to (D21) |

File assignments (one agent each; every agent reads this section,
CLAUDE.md, and the whole file it owns before editing):

| Agent | Files | Notes |
|---|---|---|
| A | `src/app.c`, `src/main.c`, `src/app.h`, `src/list_rows.[ch]` | the helper table above; `startup`: settings, icon theme, stylesheet; the first-run chain; `task_app_confirm` async (its callers in B–F split at the call) |
| B | `src/settings_window.c` | sections inline; `task_app_pick_path`; clipboard; delete placement; check buttons |
| C | `src/editor_window.c` | GtkDropDown ×3; disclosure blocks on `set_visible` + `gtk_widget_measure` + `gtk_window_set_default_size(w, 490, -1)` to re-shrink; subtasks/attachments as boxes; calendar via `dialog_new`; emoji hint |
| D | `src/sidebar.c` | GtkListView/TreeListModel; the dialogs via `dialog_new`; delete confirmations via `task_app_confirm`; the emoji entry |
| E | `src/task_list.c`, `src/task_rows.c` | GtkColumnView; `TaskRow`; `task_reorder_drag`; header menus; `header_button_flatten` and `themed_bg_css_apply`'s task-list use deleted |
| F | `src/kanban.c` | cards as plain boxes; `GtkGestureDrag`; overlay ghost; `compute_bounds` hit tests; `card_drag_stop` stays the ONE exit |
| G | `src/library_window.c` | toolbar box; status bar with a `GtkRevealer` crossfade; `notify::default-*`; paned children; dialogs async; About; compact float bar CSS |

The join: `make`, fix link errors, then `make run-dev` and the Phase 4
checklist by hand.

## Decisions

Append-only.  One entry per non-obvious mapping, with the reason.  A later
session that finds an entry wrong REPLACES it and says why — it does not
add a second idiom.

- **D1 · 2026-09-21 — INHERITED from Notes D13: `<Primary>` is Control on
  GTK4 everywhere; Command is `<Meta>`.**  Tasks has no accelerators
  today, so this matters only if one is added.
- **D2 · 2026-09-21 — INHERITED from Notes D14: context popovers are
  parented to the window's child box, never to the clicked widget.**
  `task_app_menu_popup` translates the point with
  `gtk_widget_compute_point`, holds its own reference and unparents from an
  idle on `closed`.
- **D3 · 2026-09-21 — INHERITED from Notes D21: GTK4 emits a window's
  `destroy` AFTER its dispose has torn the child tree down.**  A destroy
  handler may only touch what it holds a reference to.
- **D4 · 2026-09-21 — INHERITED from Notes D26: on macOS the cairo
  renderer and `gtk-hint-font-metrics` FALSE**, set in `main`/`startup`.
  Not re-measured; the comparison was on the same display.
- **D5 · 2026-09-21 — INHERITED from Notes D32/D35: tooltips go through
  `task_app_set_tooltip`**, which refuses a tooltip within 550 ms of the
  last one hiding and shows none in an inactive window.  Both are macOS
  backend behaviours; the helper is a no-op cost elsewhere.
- **D6 · 2026-09-21 — INHERITED from Notes D33: the list widgets are
  GtkColumnView / GtkListView over GListModels.**  An in-place change is
  `task_row_touch` (the row's "changed" signal AND items-changed for that
  one item, both required — the item manager reuses a re-added item's
  widget WITHOUT rebinding it); a rebuild is one `g_list_store_splice`; a
  column's factory cannot reach the row widget, so row-wide controllers go
  on every cell; a list view measures only realized rows, so the startup
  fit is queued from `map`.
- **D7 · 2026-09-21 — INHERITED from Notes D34: double-clicks are counted
  by the app, never by `n_press`.**  The GDK macOS backend stamps a motion
  event with the buttons held when it is TRANSLATED, so the tiny drag
  inside a quick first click can arrive buttonless and reset every
  GtkGestureClick.
- **D8 · 2026-09-21 — INHERITED from Notes D36/D37: rows select on the
  PRESS (`task_app_select_on_press`), and there is no focus ring on rows
  or cards.**  Never install the press helper on a widget that contains a
  gesture of its own — the claim cancels everything below it.
- **D9 · 2026-09-21 — The board's ghost is an overlay child, not a
  window.**  GTK4 has no `GTK_WINDOW_POPUP`, and the drag never leaves the
  window: the ghost is a `GtkPicture` of a STATIC snapshot
  (`gdk_paintable_get_current_image` of a `GtkWidgetPaintable`, so dimming
  the source card does not dim the ghost) inside a small `GtkOverlay` with
  the count badge as a label, placed on a `GtkFixed` that is an overlay
  child of the paned's GtkOverlay with `can_target` FALSE, and moved with
  `gtk_fixed_move`.  Opacity is `gtk_widget_set_opacity` on that child —
  a render-node opacity in GTK4, not a compositor feature, so gotcha 20
  dissolves.  Hit-testing uses `gtk_widget_compute_bounds` against the
  overlay's coordinate space, which is what replaces root coordinates.
- **D10 · 2026-09-21 — Manual sort and the sidebar's list reorder share
  ONE drag helper**, `task_reorder_drag`: `GtkGestureDrag` on the handle,
  `gtk_widget_pick` for the row under the pointer, a `drop-before` /
  `drop-after` CSS class as the marker, `g_list_store_splice` on release.
  No live swapping: the visible feedback is the marker, which is cheaper,
  and it is exactly the shape the board's slot marker already has.
- **D11 · 2026-09-21 — Classic scrollbars are the `gtk-overlay-scrolling`
  GtkSettings property, set once in `startup`.**  The env var Tasks used
  is GTK3-only; the property exists in 4.22 (checked in the gir) and
  reaches every scrolled window in the process, which was the whole point
  of the env var.
- **D12 · 2026-09-21 — The native macOS menubar is GTK's own
  (`gtk_application_set_menubar`), on GTK3 already** — INHERITED from
  Notes D9, and confirmed here: with gtk-mac-integration gone the quartz
  backend put the model in the native bar and built the application menu
  from `app.about` / `app.preferences` / `app.quit`.  The one Gtk-CRITICAL
  it prints on MacPorts' GTK (`gtk_menu_tracker_remove_items: assertion
  '*change_point != NULL' failed`, Notes D10) is dropped by
  `quartz_log_filter` in main.c, matched on both literal fragments so an
  unrelated CRITICAL from that function still shows.  There is NO
  `native_menubar` choice any more: native on macOS, drawn by the
  GtkApplicationWindow on Linux, nothing to choose between.
- **D13 · 2026-09-21 — A hidden-when pair has ONE writer, and the sort
  pair's is `task_pane_mode_apply`, not `manual_sort_icon_refresh`.**  The
  sort items are greyed while Kanban is on or a search is up, and "greyed"
  is "neither face": the applier that knows the reason must be the one
  that decides which face is on, or the two writers fight.  It runs last
  on every path that changes the mode or the reason.  The greying's
  explanatory tooltip moved to the toolbar button, since an item from a
  menu model carries no tooltip.
- **D14 · 2026-09-21 — Context-menu actions act on the CURRENT selection,
  not on ids carried by the item.**  The menu is modal, so the selection
  it opened on is the one its item acts on, whichever pane it came from;
  the board's card selection already answers through `selected_task_ids`.
  Group actions are the exception and carry the group id as an `x`
  target, because the clicked group need not be selected.  Behaviour
  change to know: "Move to List" is now OMITTED rather than greyed when
  nothing could move (a single selected subtask, or no other list) — a
  submenu from a model has no action of its own to grey through.
- **D15 · 2026-09-21 — `gtk_widget_get_modifier_mask` and the
  `GDK_MODIFIER_INTENT_*` constants were removed in GTK4.**  The
  board's multi-select used them to get the platform's modify and extend
  keys.  In GTK4 the mask is queried directly: MODIFY = `GDK_CONTROL_MASK |
  GDK_META_MASK` (Ctrl on X11/Wayland, Cmd on macOS — GTK4 translates the
  hardware key through `GDK_META_MASK`), EXTEND = `GDK_SHIFT_MASK`.
  No helper needed; both values are compile-time constants.
- **D16 · 2026-09-21 — `GtkWidgetPaintable *` vs `GdkPaintable *`: the
  constructor returns `GtkWidgetPaintable *` but callers that treat it as
  a paintable need `GdkPaintable *`.**  `gtk_widget_paintable_new` is typed
  `GtkWidgetPaintable *`; a `GdkPaintable *` field takes `GDK_PAINTABLE(…)`
  cast.  Both compile without warning; the cast is required at the
  assignment site, not at every use.
- **D17 · 2026-09-21 — `gtk_label_set_track_visited_links` was removed in
  GTK4.**  The call in kanban.c (on the "Show All" link inside the Done
  lane) was simply deleted.  GTK4 does not expose the visited-link colouring
  knob on `GtkLabel`; the link renders correctly without it.
- **D18 · 2026-09-21 — `gtk_entry_get/set_text` became
  `gtk_editable_get/set_text(GTK_EDITABLE(entry), …)` and
  `gtk_entry_set_width_chars` became `gtk_editable_set_width_chars` in
  GTK4.**  `GtkEntry` implements `GtkEditable`; all six width-chars call
  sites and every get/set_text in `editor_window.c` use the editable
  interface.  `gtk_calendar_get_date()` returns `GDateTime *` in GTK4
  (caller must unref); the prior `GtkCalendar`-specific output params are
  gone.

## Session log

One line per session: date, phase, item, outcome.

- 2026-09-21 — plan agreed with the user; Phase 0 (plugin removal,
  9b86b50) and Phase 1 (this file, run-dev, winshot, the library split)
  done.
- 2026-09-21 — Phase 2 (actions and menus, on GTK3) done; D12–D14.
- 2026-09-21 — Phase 3 (per-file GTK4 pass) done; D15–D18; commit 2c95d93.
  `make` clean, 333 KB binary, libgtk-4 only; `make run-dev` opens the
  window with toolbar, column headers, and empty task list visible.
