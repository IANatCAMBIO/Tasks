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
 * read_counts — the tags' open-task counts from the database, or none
 * while the setting hides them.
 */
- (void)
read_counts
{
    NSMutableDictionary<NSNumber *, NSNumber *> *counts = [NSMutableDictionary dictionary];

    if (task_config_get_bool(TASK_CFG_SIDEBAR_COUNTS)) {
        GHashTable    *map = task_db_tag_open_counts(self.library.app->db);
        GHashTableIter it;
        gpointer       key, value;
        g_hash_table_iter_init(&it, map);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            counts[@((int64_t)GPOINTER_TO_INT(key))] = @(GPOINTER_TO_INT(value));
        }
        g_hash_table_destroy(map);
    }
    self.counts = counts;
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
    NSMutableIndexSet *tag_rows = [NSMutableIndexSet indexSet];
    for (NSInteger row = 0; row < [self row_count]; row++) {
        if ([self sel_at:row].kind == task_sel_tag) [tag_rows addIndex:(NSUInteger)row];
    }
    self.showing = YES;
    [self.table reloadDataForRowIndexes:tag_rows columnIndexes:[NSIndexSet indexSetWithIndex:0]];
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
 * tag while the counts are shown, its open tasks' count at the right.  To
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
    task_sel_t   sel   = [self sel_at:row];
    NSNumber    *n     = sel.kind == task_sel_tag ? self.counts[@(sel.id)] : nil;
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
 * menuNeedsUpdate: — the table's menu is about to open: Edit Tag… for a
 * right-clicked tag, nothing elsewhere.
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
    if (sel.kind != task_sel_tag) return;
    [menu addItem:tasks_command_menu_item(TasksCommandEditTag, @(sel.id), self.library)];
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
        panel_with_title:@"Edit Tag" hero_title:@"Edit Tag"
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

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * perform_command:param: — carry out a sidebar command.
 * Documented in sidebar.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    if (self.is_shut_down || !self.library || param == nil) return;
    if (command == TasksCommandEditTag) [self show_tag_panel:param.longLongValue];
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
