#import "sidebar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "task_config.h"
#include "task_sidebar.h"
#include "task_view.h"

/* ── Constants ───────────────────────────────────────────────────────── */

static NSString * const cell_id       = @"sidebar.row";
static NSString * const header_id     = @"sidebar.header";
static NSString * const header_title  = @"Tags";
static NSString * const count_id      = @"sidebar.count";   /* a tag cell's count label */
static const CGFloat    count_gap     = 4.0;                /* text to count            */

/* ── The sidebar ─────────────────────────────────────────────────────── */

@interface TasksSidebar () <NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate>
@property (nonatomic, weak)   TasksLibraryWindow *library;
@property (nonatomic, strong) SharedTable         *table;
@property (nonatomic, strong) NSMutableData      *rows;      /* task_sel_t[], on screen */
@property (nonatomic, copy)   NSArray<NSString *> *titles;   /* one per row             */
@property (nonatomic, copy)   NSArray<NSNumber *> *view_counts;  /* a view's open tasks, by
                                                                  * view index; empty while
                                                                  * counts are not shown  */
@property (nonatomic, copy)   NSDictionary<NSNumber *, NSNumber *> *counts; /* tag id → open
                                                              * tasks; empty while the
                                                              * counts are not shown    */
@property (nonatomic, assign) BOOL                showing;   /* selecting a row from the
                                                              * window: not a choice    */
@property (nonatomic, assign) BOOL                is_shut_down;
@end

@implementation TasksSidebar

/*
 * init_with_library: — build the sidebar: a source-list table in a
 * scroll view, on the sidebar material.
 * Inputs/output are documented in sidebar.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library = library;
    _rows    = [NSMutableData data];
    _titles  = @[];
    _counts  = @{};
    _view_counts = @[];

    NSVisualEffectView *back = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    back.material     = NSVisualEffectMaterialSidebar;
    back.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    back.translatesAutoresizingMaskIntoConstraints = NO;

    self.table = [[SharedTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:cell_id];
    [self.table addTableColumn:column];
    self.table.style            = NSTableViewStyleSourceList;
    self.table.headerView       = nil;
    self.table.rowHeight        = shared_look_sidebar_row_h;
    self.table.backgroundColor  = NSColor.clearColor;
    self.table.dataSource       = self;
    self.table.delegate         = self;
    self.table.allowsEmptySelection = NO;
    self.table.menu             = [[NSMenu alloc] initWithTitle:@""];
    self.table.menu.delegate    = self;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = self.table;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.drawsBackground     = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:back];
    [self addSubview:scroll];
    for (NSView *v in @[back, scroll]) {
        [NSLayoutConstraint activateConstraints:@[
            [v.topAnchor constraintEqualToAnchor:self.topAnchor],
            [v.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [v.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [v.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        ]];
    }
    return self;
}

/* ── Rows ────────────────────────────────────────────────────────────── */

/*
 * row_count — how many rows are on screen.
 *
 * Output:
 *   the count.
 */
- (NSInteger)
row_count
{
    return (NSInteger)(self.rows.length / sizeof(task_sel_t));
}

/*
 * sel_at: — the selection a row stands for.
 *
 * Inputs:
 *   row — a row index, in range
 *
 * Output:
 *   its selection.
 */
- (task_sel_t)
sel_at:(NSInteger)row
{
    return ((const task_sel_t *)self.rows.bytes)[row];
}

/*
 * reload — rebuild the rows: the views, then the Tags header and the tags
 * when there are any.
 * Documented in sidebar.h.
 */
- (void)
reload
{
    if (!self.library || self.is_shut_down) return;

    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    [self.rows setLength:0];

    for (guint i = 0; i < task_view_count(); i++) {
        task_sel_t sel = { task_sel_view, (int64_t)i };
        [self.rows appendBytes:&sel length:sizeof sel];
        [titles addObject:tasks_string(task_view_nth(i)->label)];
    }
    GPtrArray *tags = task_db_tags(self.library.app->db, TASK_TAGS_CARRIED);
    if (tags->len > 0) {               /* no header over nothing */
        task_sel_t header = { task_sel_header, 0 };
        [self.rows appendBytes:&header length:sizeof header];
        [titles addObject:header_title];
    }
    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        task_sel_t     sel = { task_sel_tag, tag->id };
        [self.rows appendBytes:&sel length:sizeof sel];
        [titles addObject:tasks_tag_title(tag)];
    }
    task_ptr_array_free_tags(tags);
    self.titles = titles;
    [self read_counts];

    self.showing = YES;
    [self.table reloadData];
    self.showing = NO;

    task_sel_t wanted = self.library.selection;
    int        pick   = task_sidebar_pick(self.rows.bytes, (int)[self row_count], wanted);
    if (pick >= 0) {
        task_sel_t chosen = [self sel_at:pick];
        if (chosen.kind != wanted.kind || chosen.id != wanted.id) {
            [self.library select:chosen];   /* updates the display itself */
            return;
        }
    }
    [self update_selection];
}

/*
 * read_counts — the open-task counts: each view's (its query, less the
 * Done tasks) and each tag's, from the database; none while the setting
 * hides them.  As Notes' sidebar counts its fixed rows and its tags.
 */
- (void)
read_counts
{
    NSMutableDictionary<NSNumber *, NSNumber *> *counts      = [NSMutableDictionary dictionary];
    NSMutableArray<NSNumber *>                  *view_counts = [NSMutableArray array];
    TaskApp *app = self.library.app;

    if (task_config_get_bool(TASK_CFG_SIDEBAR_COUNTS)) {
        for (guint i = 0; i < task_view_count(); i++) {
            const TaskView *view  = task_view_nth(i);
            GPtrArray      *tasks = view->query(app, view->user_data);
            int             open  = 0;
            for (guint t = 0; t < tasks->len; t++) {
                if (((const Task *)g_ptr_array_index(tasks, t))->status != TASK_STATUS_DONE) open++;
            }
            task_ptr_array_free_tasks(tasks);
            [view_counts addObject:@(open)];
        }
        GHashTable    *map = task_db_tag_open_counts(app->db);
        GHashTableIter it;
        gpointer       key, value;
        g_hash_table_iter_init(&it, map);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            counts[@((int64_t)GPOINTER_TO_INT(key))] = @(GPOINTER_TO_INT(value));
        }
        g_hash_table_destroy(map);
    }
    self.counts      = counts;
    self.view_counts = view_counts;
}

/*
 * count_of: — the count a row shows, or nil for none (the header, or
 * while counts are hidden).
 *
 * Inputs:
 *   sel — the row's selection
 *
 * Output: the count.
 */
- (NSNumber *)
count_of:(task_sel_t)sel
{
    switch (sel.kind) {
    case task_sel_view:
        return (NSUInteger)sel.id < self.view_counts.count ? self.view_counts[(NSUInteger)sel.id] : nil;
    case task_sel_tag:
        return self.counts[@(sel.id)];
    default:
        return nil;
    }
}

/*
 * refresh_counts — re-read and redraw the counts only.
 * Documented in sidebar.h.
 */
- (void)
refresh_counts
{
    if (!self.library || self.is_shut_down) return;

    [self read_counts];
    NSMutableIndexSet *counted = [NSMutableIndexSet indexSet];
    for (NSInteger row = 0; row < [self row_count]; row++) {
        if ([self sel_at:row].kind != task_sel_header) [counted addIndex:(NSUInteger)row];
    }
    self.showing = YES;
    [self.table reloadDataForRowIndexes:counted columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    self.showing = NO;
}

/*
 * update_selection — select the window's selection's row.
 * Documented in sidebar.h.
 */
- (void)
update_selection
{
    if (!self.library || self.is_shut_down) return;

    task_sel_t wanted = self.library.selection;
    for (NSInteger row = 0; row < [self row_count]; row++) {
        task_sel_t sel = [self sel_at:row];
        if (sel.kind != wanted.kind || sel.id != wanted.id) continue;
        if (self.table.selectedRow != row) {
            self.showing = YES;
            [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                    byExtendingSelection:NO];
            self.showing = NO;
        }
        [self.table scrollRowToVisible:row];
        return;
    }
}

/* ── Table data and delegate ─────────────────────────────────────────── */

/*
 * numberOfRowsInTableView: — the rows on screen.
 *
 * Inputs:
 *   table — the table (unused)
 *
 * Output:
 *   the count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    return [self row_count];
}

/*
 * tableView:isGroupRow: — the Tags header is a group row: drawn as a
 * section heading, never selected.
 *
 * Inputs:
 *   table — the table (unused)
 *   row   — the row
 *
 * Output:
 *   YES for the header.
 */
- (BOOL)
tableView:(NSTableView *)table isGroupRow:(NSInteger)row
{
    return [self sel_at:row].kind == task_sel_header;
}

/*
 * tableView:shouldSelectRow: — every row but the header.
 *
 * Inputs:
 *   table — the table (unused)
 *   row   — the row
 *
 * Output:
 *   NO for the header.
 */
- (BOOL)
tableView:(NSTableView *)table shouldSelectRow:(NSInteger)row
{
    return [self sel_at:row].kind != task_sel_header;
}

/*
 * tableView:viewForTableColumn:row: — a row's cell: its label and, for a
 * view or a tag while the counts are shown, its open tasks' count at the
 * right.  To
 * VoiceOver the cell is one static text — the bare name and the count —
 * not the emoji, the name and the count apart.
 *
 * Inputs:
 *   table  — the table
 *   column — the only column (unused)
 *   row    — the row
 *
 * Output:
 *   the cell view.
 */
- (NSView *)
tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    BOOL      header = [self sel_at:row].kind == task_sel_header;
    NSString *ident  = header ? header_id : cell_id;
    NSTableCellView *cell = [table makeViewWithIdentifier:ident owner:nil];

    if (cell == nil) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = ident;
        cell.accessibilityElement = YES;
        cell.accessibilityRole    = NSAccessibilityStaticTextRole;
        NSTextField *text = [NSTextField labelWithString:@""];
        text.lineBreakMode = NSLineBreakByTruncatingTail;
        text.accessibilityElement = NO;
        text.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:text];
        cell.textField = text;
        NSTextField *count = [NSTextField labelWithString:@""];
        count.identifier = count_id;
        count.font       = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                                           weight:NSFontWeightRegular];
        count.textColor  = NSColor.secondaryLabelColor;
        count.alignment  = NSTextAlignmentRight;
        count.accessibilityElement = NO;
        count.translatesAutoresizingMaskIntoConstraints = NO;
        [count setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
        [cell addSubview:count];
        [NSLayoutConstraint activateConstraints:@[
            [text.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor
                                                constant:shared_look_sidebar_text_pad],
            [text.trailingAnchor constraintLessThanOrEqualToAnchor:count.leadingAnchor
                                                            constant:-count_gap],
            [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [count.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor
                                                 constant:-shared_look_sidebar_count_pad],
            [count.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    NSNumber    *n     = [self count_of:[self sel_at:row]];
    NSTextField *count = nil;
    for (NSView *v in cell.subviews) {
        if ([v.identifier isEqualToString:count_id]) count = (NSTextField *)v;
    }
    count.stringValue = n != nil ? n.stringValue : @"";
    cell.textField.stringValue = self.titles[(NSUInteger)row];
    cell.textField.font = header ? [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize]
                                 : [NSFont systemFontOfSize:NSFont.systemFontSize];
    cell.textField.textColor = header ? NSColor.secondaryLabelColor : NSColor.labelColor;
    NSString *name = shared_ui_label_name(self.titles[(NSUInteger)row]);
    cell.accessibilityValue = n != nil ? [NSString stringWithFormat:@"%@, %@", name, n] : name;
    return cell;
}

/*
 * tableView:typeSelectStringForTableColumn:row: — type-select finds a
 * row by its bare name, so typing a letter reaches a row whose label
 * starts with an emoji.
 *
 * Inputs:
 *   table  — the table (unused)
 *   column — the column (unused)
 *   row    — the row
 *
 * Output:
 *   the row's name.
 */
- (NSString *)
tableView:(NSTableView *)table typeSelectStringForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    return shared_ui_label_name(self.titles[(NSUInteger)row]);
}

/*
 * tableViewSelectionDidChange: — the user chose a row: tell the window.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
tableViewSelectionDidChange:(NSNotification *)note
{
    if (self.showing || self.is_shut_down) return;

    NSInteger row = self.table.selectedRow;
    if (row >= 0) [self.library select:[self sel_at:row]];
}

/* ── The context menu ────────────────────────────────────────────────── */

/*
 * menuNeedsUpdate: — the table's menu is about to open: Edit Tag…, Add to
 * Favorites (a favorite is removed from the Favorites bar's own menu) and
 * Clear Tag from All Tasks for a right-clicked tag; Add to Favorites alone
 * for a right-clicked view; nothing elsewhere.
 *
 * Inputs:
 *   menu — the table's menu
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];

    NSInteger row = self.table.clickedRow;
    if (row < 0 || self.is_shut_down || !self.library) return;
    task_sel_t sel = [self sel_at:row];
    if (sel.kind == task_sel_header) return;
    if (sel.kind == task_sel_view) {
        [menu addItem:shared_command_menu_item(@"add-favorite", @(task_sel_button_id(sel)))];
        return;
    }
    [menu addItem:shared_command_menu_item(@"edit-tag", @(sel.id))];
    [menu addItem:shared_command_menu_item(@"add-favorite", @(sel.id))];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:shared_command_menu_item(@"clear-tag", @(sel.id))];
}

/* ── Edit Tag ────────────────────────────────────────────────────────── */

/*
 * show_tag_panel: — the Edit Tag panel: the same grid as Notes' (Tag:,
 * Emoji (optional):, Favorite:), OK and Cancel, a problem reported in the
 * hero strip.
 * Documented in sidebar.h.
 */
- (void)
show_tag_panel:(int64_t)tag_id
{
    TasksLibraryWindow *library = self.library;
    TaskTag            *tag     = library != nil ? task_db_tag_get(library.app->db, tag_id) : NULL;
    if (tag == NULL) return;

    NSFont      *font  = [NSFont systemFontOfSize:shared_look_ui_font];
    NSTextField *emoji = shared_ui_emoji_field(tasks_string(tag->emoji));
    emoji.identifier   = @"tag-emoji";

    NSButton *favorite = [NSButton checkboxWithTitle:@"Show in the Favorites bar"
                                              target:nil action:NULL];
    favorite.font       = font;
    favorite.identifier = @"tag-favorite";
    favorite.state      = tag->favorite ? NSControlStateValueOn : NSControlStateValueOff;

    NSTextField *(^label)(NSString *) = ^NSTextField *(NSString *text) {
        NSTextField *l = [NSTextField labelWithString:text];
        l.font = font;
        return l;
    };
    NSTextField *name = label(tasks_string(tag->name));
    name.font    = [NSFont boldSystemFontOfSize:shared_look_ui_font];
    name.toolTip = @"A tag keeps its name: it is the same tag on every task carrying it.";
    task_tag_free(tag);

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[label(@"Tag:"),              name],
        @[label(@"Emoji (optional):"), emoji],
        @[label(@"Favorite:"),         favorite],
    ]];
    grid.rowSpacing    = shared_look_tool_btn_gap;
    grid.columnSpacing = shared_look_tool_btn_gap;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger r = 0; r < grid.numberOfRows; r++) [grid rowAtIndex:r].yPlacement = NSGridCellPlacementCenter;

    __weak TasksSidebar *ws = self;
    SharedToolPanel *panel = [SharedToolPanel
        panel_with_title:@"Edit Tag" headline:@"Edit Tag"
                subtitle:@"The emoji shows in Notes and Tasks; a tag's name never changes."
                  symbol:@"tag" content:grid buttons:@[@"OK", @"Cancel"] destructive:-1
                 handler:^NSString *(NSInteger index) {
        TasksSidebar *s = ws;
        if (index != 0 || !s || s.is_shut_down) return nil;    /* Cancel, or gone */
        TaskApp  *app    = s.library.app;
        NSString *picked = [emoji.stringValue stringByTrimmingCharactersInSet:
                            NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!task_db_tag_set_emoji(app->db, tag_id, picked.UTF8String) ||
            !task_db_tag_set_favorite(app->db, tag_id, favorite.state == NSControlStateValueOn))
            return @"The tag could not be changed.";
        [s.library full_refresh];
        return nil;
    }];
    [panel show_over:library.window];
    [panel.window makeFirstResponder:emoji];
}

/*
 * shut_down — stop responding.
 * Documented in sidebar.h.
 */
- (void)
shut_down
{
    self.is_shut_down = YES;
    self.table.dataSource = nil;
    self.table.delegate   = nil;
    self.table.menu.delegate = nil;
}

@end

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * tag_of — the tag an Edit Tag item means: its parameter, else the tag
 * selected in the sidebar.
 *
 * Inputs:
 *   sidebar — the sidebar
 *   item    — the item, or nil
 *
 * Output: the tag id, or 0 when there is none.
 */
static int64_t
tag_of(TasksSidebar *sidebar, id item)
{
    NSNumber *param = shared_command_param(item);
    if (param != nil) return param.longLongValue;
    task_sel_t sel = sidebar.library.selection;
    return sel.kind == task_sel_tag ? sel.id : 0;
}

/*
 * run_edit_tag — Edit Tag…
 *
 * Inputs:
 *   target — the sidebar
 *   sender — the item
 */
static void
run_edit_tag(id target, id sender)
{
    TasksSidebar *sidebar = target;
    int64_t       tag_id  = tag_of(sidebar, sender);
    if (!sidebar.is_shut_down && sidebar.library != nil && tag_id != 0) {
        [sidebar show_tag_panel:tag_id];
    }
}

/*
 * clear_tag — take a tag off every live task that carries it, in one
 * transaction.  The schema's triggers delete the tag itself once no note
 * carries it either.
 *
 * Inputs:
 *   sidebar — the sidebar
 *   tag_id  — the tag
 *
 * Output: YES when every task was cleared, NO when a write failed (the
 * transaction is then rolled back).
 */
static BOOL
clear_tag(TasksSidebar *sidebar, int64_t tag_id)
{
    TaskDatabase *db    = sidebar.library.app->db;
    GPtrArray    *tasks = task_db_tasks_tagged(db, tag_id);
    BOOL          began = task_db_transaction_begin(db);
    BOOL          ok    = YES;

    for (guint i = 0; ok && i < tasks->len; i++) {
        Task *task = g_ptr_array_index(tasks, i);
        ok = task_db_task_tag_remove(db, task->id, tag_id) == 1;
    }
    if (began) ok = task_db_transaction_end(db, ok) && ok;
    task_ptr_array_free_tasks(tasks);
    return ok;
}

/*
 * run_clear_tag — Clear Tag from All Tasks: ask, then take the tag off every
 * task that carries it.  The tag leaves this sidebar at once; it stays in
 * the tag lists until no note carries it either.
 *
 * Inputs:
 *   target — the sidebar
 *   sender — the item
 */
static void
run_clear_tag(id target, id sender)
{
    TasksSidebar *sidebar = target;
    int64_t       tag_id  = tag_of(sidebar, sender);
    if (sidebar.is_shut_down || sidebar.library == nil || tag_id == 0) return;

    TaskDatabase *db  = sidebar.library.app->db;
    TaskTag      *tag = task_db_tag_get(db, tag_id);
    if (tag == NULL) return;
    GPtrArray *tasks = task_db_tasks_tagged(db, tag_id);
    guint      n     = tasks->len;
    NSString  *name  = tasks_string(tag->name);
    NSString  *title = [NSString stringWithFormat:@"Are you sure you want to remove the tag %@ from %u %@?",
                        name, n, n == 1 ? @"task" : @"tasks"];
    NSString  *note  = [NSString stringWithFormat:
        @"Removing from All Tasks will hide %@ in the sidebar, but it will still be available in "
        @"the tags list. To delete, it must be removed from All Tasks and All Notes.", name];
    task_ptr_array_free_tasks(tasks);
    task_tag_free(tag);

    shared_ui_confirm(sidebar.window, title, note, @"Remove", YES, ^(BOOL confirmed) {
        if (!confirmed || sidebar.is_shut_down || sidebar.library == nil) return;
        if (!clear_tag(sidebar, tag_id)) {
            task_app_status(sidebar.library.app, "The tag could not be cleared");
        }
        [sidebar.library full_refresh];
    });
}

/*
 * validate_has_tag — there is a tag for the command to mean.
 *
 * Inputs:
 *   target — the sidebar
 *   item   — the item
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_has_tag(id target, id item, SharedCommandState *state)
{
    return tag_of(target, item) != 0;
}

/*
 * button_of — the selection a Favorites item means: its parameter (a
 * button id, task_sel_button_id), else the row selected in the sidebar.
 *
 * Inputs:
 *   sidebar — the sidebar
 *   item    — the item, or nil
 *
 * Output: the selection; of kind task_sel_header when there is none.
 */
static task_sel_t
button_of(TasksSidebar *sidebar, id item)
{
    NSNumber *param = shared_command_param(item);
    if (param != nil && param.longLongValue != 0) return task_sel_from_button_id(param.longLongValue);
    task_sel_t sel = sidebar.library.selection;
    return task_sel_button_id(sel) != 0 ? sel : (task_sel_t){ task_sel_header, 0 };
}

/*
 * run_set_favorite — Add to Favorites / Remove from Favorites: set the
 * tag's favorite flag (Tasks' own, as the Edit Tag panel's box does) or
 * list the view in TASK_CFG_FAVORITE_VIEWS, and refresh the sidebar and the
 * Favorites bar.  Adding a favorite again changes nothing: a favorite is
 * removed only by its own command (the Favorites bar's menu, the File menu).
 *
 * Inputs:
 *   target — the sidebar
 *   sender — the item; its command says which way
 */
static void
run_set_favorite(id target, id sender)
{
    TasksSidebar *sidebar  = target;
    BOOL          favorite = [shared_command_ident(sender) isEqualToString:@"add-favorite"];
    if (sidebar.is_shut_down || sidebar.library == nil) return;
    task_sel_t sel = button_of(sidebar, sender);
    if (sel.kind == task_sel_view) {
        task_view_set_favorite(task_sel_view_of(sel), favorite);
    } else if (sel.kind == task_sel_tag &&
               !task_db_tag_set_favorite(sidebar.library.app->db, sel.id, favorite)) {
        task_app_status(sidebar.library.app, "The tag could not be changed");
    }
    [sidebar.library full_refresh];
}

/*
 * is_favorite — whether the tag or view an item means (button_of) is a
 * favorite.
 *
 * Inputs:
 *   sidebar  — the sidebar
 *   item     — the item, or nil for the selected row
 *   favorite — receives the flag
 *
 * Output: NO when there is no such tag or view.
 */
static BOOL
is_favorite(TasksSidebar *sidebar, id item, BOOL *favorite)
{
    if (sidebar.library == nil) return NO;
    task_sel_t sel = button_of(sidebar, item);
    if (sel.kind == task_sel_view) {
        const TaskView *view = task_sel_view_of(sel);
        *favorite = task_view_is_favorite(view);
        return view != NULL;
    }
    if (sel.kind != task_sel_tag) return NO;
    TaskTag *tag = task_db_tag_get(sidebar.library.app->db, sel.id);
    if (tag == NULL) return NO;
    *favorite = tag->favorite != FALSE;
    task_tag_free(tag);
    return YES;
}

/*
 * validate_add_favorite — a tag or view that is not yet a favorite.
 *
 * Inputs:
 *   target — the sidebar
 *   item   — the item, or nil
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_add_favorite(id target, id item, SharedCommandState *state)
{
    BOOL favorite = NO;
    return is_favorite(target, item, &favorite) && !favorite;
}

/*
 * validate_remove_favorite — a tag or view that is a favorite.
 *
 * Inputs:
 *   target — the sidebar
 *   item   — the item, or nil
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_remove_favorite(id target, id item, SharedCommandState *state)
{
    BOOL favorite = NO;
    return is_favorite(target, item, &favorite) && favorite;
}

/*
 * tasks_sidebar_commands — see sidebar.h.
 */
const shared_command_handler_t *
tasks_sidebar_commands(size_t *n)
{
    static const shared_command_handler_t table[] = {
        { @"edit-tag",        run_edit_tag,        validate_has_tag         },
        { @"clear-tag",       run_clear_tag,       validate_has_tag         },
        { @"add-favorite",    run_set_favorite,    validate_add_favorite    },
        { @"remove-favorite", run_set_favorite,    validate_remove_favorite },
    };
    *n = sizeof table / sizeof table[0];
    return table;
}
