#import "sidebar.h"
#import "editor_window.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "task_order.h"
#include "task_sidebar.h"

/* The outline's one column, and the reuse identifier of its cells. */
static NSString * const column_ident = @"row";

/* The pasteboard type of a list being dragged.  Private to the app: a list
 * can only be dropped back into this outline. */
static NSPasteboardType const list_drag_type = @"com.cambio.tasks-appkit.sidebar-list";

/* The words the sidebar itself supplies. */
static NSString * const header_label      = @"Lists";
static NSString * const emoji_placeholder = @"…";
static NSString * const emoji_tooltip     = @"Optional emoji — click to pick";

/* Row metrics.  The height suits the 13 pt row font with an emoji in it;
 * the padding either side of a row's text is the Files table cell's. */
static const CGFloat row_h    = 24.0;
static const CGFloat cell_pad = 0.0;

/* The sheets' fields. */
static const CGFloat emoji_field_w = 40.0;
static const CGFloat name_field_w  = 240.0;

/* The sidebar is the split view's first pane, so its width is the position
 * of the first divider. */
static const NSInteger sidebar_divider = 0;

/* ── Rows ───────────────────────────────────────────────────────────── */

/*
 * TasksSidebarItem — one row of the outline: a view, the Lists header, a
 * group or a list.  A record with no behaviour; the outline needs objects
 * to tell its rows apart.  Rebuilt from the database on every reload, so a
 * row is identified across reloads by `sel`, never by its object.
 */
@interface TasksSidebarItem : NSObject
/* The selection this row stands for: kind and id (task_pane.h). */
@property (nonatomic, assign) task_sel_t sel;
/* For a list: the group it is filed under, 0 when ungrouped. */
@property (nonatomic, assign) int64_t group_id;
/* What the row says. */
@property (nonatomic, copy) NSString *label;
/* The rows under the header or a group, in display order; nil for a view
 * or a list, which can never have any. */
@property (nonatomic, strong) NSMutableArray<TasksSidebarItem *> *children;
@end

@implementation TasksSidebarItem
@end

/*
 * new_item — one row.
 *
 * Inputs:
 *   kind      — what the row is
 *   ident     — the view index, list id or group id; 0 for the header
 *   label     — what it says
 *   container — YES for the header and groups, which hold other rows
 *
 * Output:
 *   new item.
 */
static TasksSidebarItem *
new_item(task_sel_kind_t kind, int64_t ident, NSString *label, BOOL container)
{
    TasksSidebarItem *item = [[TasksSidebarItem alloc] init];

    item.sel      = (task_sel_t){ kind, ident };
    item.label    = label;
    item.children = container ? [NSMutableArray array] : nil;
    return item;
}

/*
 * show_item — make a text field say a row's label the way the row is drawn.
 * The ONE place that decides a row's text and font, shared by the cells
 * and by the width fitting so the two cannot measure different things.
 *
 * Views and groups are bold, as in the GTK build; the Lists header takes
 * the small secondary look of a source list's section header.
 *
 * Inputs:
 *   item  — the row
 *   field — the cell's label, or the field used for measuring
 */
static void
show_item(TasksSidebarItem *item, NSTextField *field)
{
    switch (item.sel.kind) {
    case task_sel_header:
        field.font      = [NSFont systemFontOfSize:NSFont.smallSystemFontSize
                                            weight:NSFontWeightSemibold];
        field.textColor = NSColor.secondaryLabelColor;
        break;
    case task_sel_view:
    case task_sel_group:
        field.font      = [NSFont boldSystemFontOfSize:tasks_look_sidebar_font];
        field.textColor = NSColor.labelColor;
        break;
    case task_sel_list:
        field.font      = [NSFont systemFontOfSize:tasks_look_sidebar_font];
        field.textColor = NSColor.labelColor;
        break;
    }
    field.stringValue = item.label;
}

/* ── The emoji field ────────────────────────────────────────────────── */

/*
 * TasksEmojiField — the list sheet's emoji slot: a small text field that
 * holds at most one character and opens the system's emoji picker when
 * clicked.
 *
 * The picker (the Character Viewer) types into whatever text is being
 * edited, so the slot has to be a real text field; this class adds only
 * what makes it behave like a slot rather than a line of text.
 */
@interface TasksEmojiField : NSTextField
@end

@implementation TasksEmojiField

/*
 * mouseDown: — a click starts editing as usual, selects what is there so
 * that the next character replaces it, and opens the emoji picker.
 *
 * Inputs:
 *   event — the mouse-down
 */
- (void)
mouseDown:(NSEvent *)event
{
    [super mouseDown:event];
    [self.currentEditor selectAll:nil];
    /* The system emoji picker opens with Ctrl+Cmd+Space; no programmatic
     * open here because the Character Viewer panel has no close API. */
}

/*
 * textDidChange: — keep only the last character entered, then give up the
 * keyboard.
 *
 * "Character" is a composed sequence: an emoji with a skin tone or a flag
 * is several code points and must stay whole.  Editing ends once there is
 * a character because a field that is still being edited does not see the
 * next click (its editor does), and so could not open the picker again.
 * That happens on the next run-loop pass: ending an edit from inside the
 * editor's own change notification re-enters it.
 *
 * Inputs:
 *   note — NSTextDidChangeNotification; its object is the field editor
 */
- (void)
textDidChange:(NSNotification *)note
{
    NSText   *editor = note.object;
    NSString *text   = editor.string;

    if (text.length > 0) {
        NSRange last = [text rangeOfComposedCharacterSequenceAtIndex:text.length - 1];
        if (last.location > 0) editor.string = [text substringWithRange:last];

        __weak TasksEmojiField *ws = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            TasksEmojiField *s = ws;
            if (s.currentEditor) [s.window makeFirstResponder:nil];
        });
    }
    [super textDidChange:note];
}

@end

/* ── Sheets ─────────────────────────────────────────────────────────── */

/*
 * sheet_content — the content of one of the sidebar's sheets: a headline
 * saying what the sheet is for (a sheet has no title bar to say it), above
 * a row of fields.
 *
 * Inputs:
 *   headline — "New List", "Rename Group", ...
 *   fields   — the fields, left to right; each defines its own width
 *
 * Output:
 *   new view for tasks_ui_sheet, sized by its content.
 */
static NSView *
sheet_content(NSString *headline, NSArray<NSView *> *fields)
{
    NSTextField *title = [NSTextField labelWithString:headline];
    title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];

    NSStackView *row = [NSStackView stackViewWithViews:fields];
    row.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing      = tasks_look_form_spacing;

    NSStackView *content = [NSStackView stackViewWithViews:@[title, row]];
    content.orientation  = NSUserInterfaceLayoutOrientationVertical;
    content.alignment    = NSLayoutAttributeLeading;
    content.spacing      = tasks_look_form_spacing;
    return content;
}

/*
 * name_field — a sheet's name field: a stock bezeled text field of the
 * sheets' common width.
 *
 * Inputs:
 *   text        — the name to start with; @"" for none
 *   placeholder — what the empty field says
 *
 * Output:
 *   new field.
 */
static NSTextField *
name_field(NSString *text, NSString *placeholder)
{
    NSTextField *field = [NSTextField textFieldWithString:text];

    field.placeholderString = placeholder;
    [field.widthAnchor constraintEqualToConstant:name_field_w].active = YES;
    return field;
}

/*
 * trimmed — a name as typed, without the white space around it.
 *
 * Inputs:
 *   field — a sheet's name field
 *
 * Output:
 *   the name; empty when nothing but white space was typed.
 */
static NSString *
trimmed(NSTextField *field)
{
    return [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

/* ── The sidebar ────────────────────────────────────────────────────── */

@interface TasksSidebar () <NSOutlineViewDataSource, NSOutlineViewDelegate, NSMenuDelegate>
@property (nonatomic, weak) TasksLibraryWindow *library;
@property (nonatomic, strong) NSOutlineView *outline;
@property (nonatomic, strong) NSScrollView *scroll;
/* The top-level rows: the visible views, then the Lists header. */
@property (nonatomic, copy) NSArray<TasksSidebarItem *> *roots;
/* The Lists header: its children are the groups, then the ungrouped lists. */
@property (nonatomic, strong) TasksSidebarItem *header;
/* What the user has folded away, kept across reloads (which rebuild every
 * row): the header, and groups by id.  Everything starts open. */
@property (nonatomic, assign) BOOL header_collapsed;
@property (nonatomic, strong) NSMutableSet<NSNumber *> *collapsed_groups;
/* YES while reload rebuilds the outline, so that the rows it expands and
 * the row it selects are not taken for the user's doing. */
@property (nonatomic, assign) BOOL reloading;
/* The list being dragged, from the drag's start to its end; nil otherwise. */
@property (nonatomic, strong) TasksSidebarItem *dragged;
/* A label that is never shown: show_item fills it to measure a row. */
@property (nonatomic, strong) NSTextField *measure_field;
/* YES while a width fit is waiting for the next run-loop pass. */
@property (nonatomic, assign) BOOL fit_pending;
/* The content width the pane was last fitted to; the pane is fitted again
 * only when the rows ask for a different one. */
@property (nonatomic, assign) CGFloat fitted_width;
@property (nonatomic, assign) BOOL is_shut_down;
@end

@implementation TasksSidebar

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in sidebar.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library          = library;
    _roots            = @[];
    _collapsed_groups = [NSMutableSet set];
    _measure_field    = [NSTextField labelWithString:@""];

    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:column_ident];
    column.resizingMask   = NSTableColumnAutoresizingMask;

    self.outline = [[NSOutlineView alloc] initWithFrame:NSZeroRect];
    self.outline.headerView              = nil;
    self.outline.style                   = NSTableViewStyleSourceList;
    /* Custom: the row height and fonts are the app's (look.h), not the
     * system's sidebar-size setting, which would also undo the bold rows. */
    self.outline.rowSizeStyle            = NSTableViewRowSizeStyleCustom;
    self.outline.rowHeight               = row_h;
    /* The views sit above the Lists header; a header that stayed pinned to
     * the top while they scrolled away under it would read as theirs. */
    self.outline.floatsGroupRows         = NO;
    self.outline.allowsEmptySelection    = NO;
    self.outline.allowsMultipleSelection = NO;
    /* The pane fits its own width (apply_fit); the outline must not also
     * resize its column whenever a row is expanded. */
    self.outline.autoresizesOutlineColumn = NO;
    self.outline.columnAutoresizingStyle  = NSTableViewFirstColumnOnlyAutoresizingStyle;
    [self.outline addTableColumn:column];
    self.outline.outlineTableColumn = column;
    self.outline.dataSource   = self;
    self.outline.delegate     = self;
    self.outline.target       = self;
    self.outline.doubleAction = @selector(row_double_clicked:);

    /* Filled each time it opens (menuNeedsUpdate:). */
    self.outline.menu          = [[NSMenu alloc] initWithTitle:@""];
    self.outline.menu.delegate = self;

    /* Lists are dragged within the outline only. */
    [self.outline registerForDraggedTypes:@[list_drag_type]];
    [self.outline setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    [self.outline setDraggingSourceOperationMask:NSDragOperationNone forLocal:NO];

    self.scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    self.scroll.hasVerticalScroller = YES;
    self.scroll.autohidesScrollers  = YES;
    self.scroll.drawsBackground     = NO;
    self.scroll.documentView        = self.outline;
    self.scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:self.scroll];
    [NSLayoutConstraint activateConstraints:@[
        [self.scroll.topAnchor constraintEqualToAnchor:self.topAnchor
                                              constant:tasks_look_sidebar_top_pad],
        [self.scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

/* ── The rows ────────────────────────────────────────────────────────── */

/*
 * build_items — rebuild the rows from the database: the views that are
 * visible right now (Favorites exists only while something is pinned),
 * then the Lists header holding the groups, each with its lists, and
 * after them the lists filed under no group.
 *
 * A list naming a group that does not exist is shown ungrouped rather than
 * not at all.  Groups the user had folded but which are gone are forgotten.
 *
 * Inputs:
 *   app — the application context
 */
- (void)
build_items:(TaskApp *)app
{
    NSMutableArray<TasksSidebarItem *> *roots = [NSMutableArray array];
    NSMutableDictionary<NSNumber *, TasksSidebarItem *> *groups_by_id =
        [NSMutableDictionary dictionary];

    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *view = task_view_nth(i);
        if (view->visible != NULL && !view->visible(app, view->user_data)) continue;
        [roots addObject:new_item(task_sel_view, i, tasks_string(view->label), NO)];
    }

    self.header = new_item(task_sel_header, 0, header_label, YES);
    [roots addObject:self.header];

    GPtrArray *groups = task_db_groups(app->db);
    for (guint i = 0; i < groups->len; i++) {
        const TaskGroup *group = g_ptr_array_index(groups, i);
        TasksSidebarItem *item = new_item(task_sel_group, group->id, tasks_string(group->name), YES);
        groups_by_id[@(group->id)] = item;
        [self.header.children addObject:item];
    }
    task_ptr_array_free_groups(groups);

    GPtrArray *lists = task_db_lists(app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        char *label = task_sidebar_list_label(list);
        TasksSidebarItem *item  = new_item(task_sel_list, list->id, tasks_string(label), NO);
        TasksSidebarItem *group = groups_by_id[@(list->group_id)];
        free(label);

        item.group_id = group ? list->group_id : 0;
        [(group ?: self.header).children addObject:item];
    }
    task_ptr_array_free_lists(lists);

    [self.collapsed_groups intersectSet:[NSSet setWithArray:groups_by_id.allKeys]];
    self.roots = roots;
}

/*
 * group_count — how many of the header's children are groups.  They come
 * first, so this is also the index of the first ungrouped list.
 *
 * Output:
 *   the count.
 */
- (NSInteger)
group_count
{
    NSInteger count = 0;

    for (TasksSidebarItem *item in self.header.children) {
        if (item.sel.kind == task_sel_group) count++;
    }
    return count;
}

/*
 * container_of_group: — the row a list of a given group sits under.
 *
 * Inputs:
 *   group_id — the list's group, or 0 for an ungrouped list
 *
 * Output:
 *   the group's row; the header for 0 (or a group that has no row).
 */
- (TasksSidebarItem *)
container_of_group:(int64_t)group_id
{
    for (TasksSidebarItem *item in self.header.children) {
        if (item.sel.kind == task_sel_group && item.sel.id == group_id) return item;
    }
    return self.header;
}

/*
 * index_of_list:in: — where a list sits among a container's children.
 *
 * Inputs:
 *   list_id   — the list
 *   container — the header or a group
 *
 * Output:
 *   the index, or NSNotFound when the list is not directly under it.
 */
- (NSUInteger)
index_of_list:(int64_t)list_id in:(TasksSidebarItem *)container
{
    return [container.children indexOfObjectPassingTest:
        ^BOOL(TasksSidebarItem *item, NSUInteger index, BOOL *stop) {
            return item.sel.kind == task_sel_list && item.sel.id == list_id;
        }];
}

/*
 * selected_list — the row of the window's selection when that is a list.
 *
 * Output:
 *   the list's row, or nil when a view or a group is selected.
 */
- (TasksSidebarItem *)
selected_list
{
    int64_t list_id = task_sel_list_id(self.library.selection);

    if (list_id == 0) return nil;
    for (TasksSidebarItem *item in self.header.children) {
        NSArray<TasksSidebarItem *> *level = item.children ?: @[item];
        for (TasksSidebarItem *list in level) {
            if (list.sel.kind == task_sel_list && list.sel.id == list_id) return list;
        }
    }
    return nil;
}

/*
 * is_item:inside: — whether a row sits under another, at any depth.
 *
 * Inputs:
 *   item      — the row
 *   container — the possible ancestor
 *
 * Output:
 *   YES when container is the row's parent or its parent's parent.
 */
- (BOOL)
is_item:(TasksSidebarItem *)item inside:(TasksSidebarItem *)container
{
    for (id up = [self.outline parentForItem:item]; up != nil;
         up = [self.outline parentForItem:up]) {
        if (up == container) return YES;
    }
    return NO;
}

/* ── Reload ──────────────────────────────────────────────────────────── */

/*
 * restore_expansion — after a rebuild, open what was open.  The header
 * and the groups are open unless the user folded them; whatever holds the
 * window's selection is opened regardless, because a row has to be on
 * screen to be selected.
 *
 * Inputs:
 *   wanted — the window's selection
 */
- (void)
restore_expansion:(task_sel_t)wanted
{
    BOOL in_lists = wanted.kind == task_sel_list || wanted.kind == task_sel_group;

    if (!self.header_collapsed || in_lists) [self.outline expandItem:self.header];

    for (TasksSidebarItem *group in self.header.children) {
        if (group.sel.kind != task_sel_group) continue;
        BOOL holds_selection = wanted.kind == task_sel_list &&
                               [self index_of_list:wanted.id in:group] != NSNotFound;
        if (![self.collapsed_groups containsObject:@(group.sel.id)] || holds_selection) {
            [self.outline expandItem:group];
        }
    }
}

/*
 * row_to_select_hiding: — the row that should be selected, by the
 * fallback order of task_sidebar_pick, given the window's selection and
 * the rows on screen.
 *
 * Inputs:
 *   hidden — a container about to be folded, whose rows must not be
 *            chosen; nil when every row on screen may be
 *
 * Output:
 *   the row index, or -1 when no row can be selected.
 */
- (NSInteger)
row_to_select_hiding:(TasksSidebarItem *)hidden
{
    NSInteger count = self.outline.numberOfRows;
    if (count == 0) return -1;

    /* A row that may not be chosen is passed as the header, the one kind
     * the pick never returns. */
    task_sel_t *rows = malloc((size_t)count * sizeof *rows);
    if (rows == NULL) abort();
    for (NSInteger row = 0; row < count; row++) {
        TasksSidebarItem *item = [self.outline itemAtRow:row];
        rows[row] = hidden != nil && [self is_item:item inside:hidden]
            ? (task_sel_t){ task_sel_header, 0 } : item.sel;
    }

    NSInteger picked = task_sidebar_pick(rows, (int)count, self.library.selection);
    free(rows);
    return picked;
}

/*
 * select_row: — select one row of the outline.
 *
 * Inputs:
 *   row — a valid row index
 */
- (void)
select_row:(NSInteger)row
{
    [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
              byExtendingSelection:NO];
}

/*
 * reload — rebuild the rows and re-select the window's selection.
 * The contract is documented in sidebar.h.
 *
 * The rows are new objects after every rebuild, so nothing of the outline's
 * own state carries over: what was open is opened again, and the selection
 * is found again by kind and id, never by row number.
 */
- (void)
reload
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down) return;

    task_sel_t wanted = library.selection;

    self.reloading = YES;
    [self build_items:library.app];
    [self.outline reloadData];
    [self restore_expansion:wanted];
    NSInteger row = [self row_to_select_hiding:nil];
    if (row >= 0) [self select_row:row];
    self.reloading = NO;

    if (row >= 0) {
        task_sel_t picked = ((TasksSidebarItem *)[self.outline itemAtRow:row]).sel;
        if (picked.kind != wanted.kind || picked.id != wanted.id) [library select:picked];
    }
    [self queue_fit];
}

/* ── Outline data source ─────────────────────────────────────────────── */

/*
 * children_of: — the rows under an outline item.
 *
 * Inputs:
 *   item — a row, or nil for the top level
 *
 * Output:
 *   its children; nil for a view or a list.
 */
- (NSArray<TasksSidebarItem *> *)
children_of:(TasksSidebarItem *)item
{
    return item ? item.children : self.roots;
}

/*
 * outlineView:numberOfChildrenOfItem: — how many rows sit under an item.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — a row, or nil for the top level
 *
 * Output:
 *   the count.
 */
- (NSInteger)
outlineView:(NSOutlineView *)outline
numberOfChildrenOfItem:(id)item
{
    return (NSInteger)[self children_of:item].count;
}

/*
 * outlineView:child:ofItem: — one of the rows under an item.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   index   — which child
 *   item    — a row, or nil for the top level
 *
 * Output:
 *   the child row.
 */
- (id)
outlineView:(NSOutlineView *)outline
      child:(NSInteger)index
     ofItem:(id)item
{
    return [self children_of:item][(NSUInteger)index];
}

/*
 * outlineView:isItemExpandable: — whether a row gets a disclosure control:
 * the header and groups do while they hold something.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the row
 *
 * Output:
 *   YES for a container that is not empty.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline
isItemExpandable:(id)item
{
    return ((TasksSidebarItem *)item).children.count > 0;
}

/* ── Outline delegate ────────────────────────────────────────────────── */

/*
 * outlineView:viewForTableColumn:item: — the cell for a row: one label,
 * truncated at the tail when the pane is too narrow for it.
 *
 * Inputs:
 *   outline — the outline
 *   column  — the single column; nil for the header, which spans the row
 *             (unused)
 *   item    — the row
 *
 * Output:
 *   a reused or new cell showing the row.
 */
- (NSView *)
outlineView:(NSOutlineView *)outline
viewForTableColumn:(NSTableColumn *)column
       item:(id)item
{
    NSTableCellView *cell = [outline makeViewWithIdentifier:column_ident owner:self];

    if (!cell) {
        NSTextField *label  = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.translatesAutoresizingMaskIntoConstraints = NO;
        /* The pane's width decides the label's, never the other way round. */
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];

        cell            = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = column_ident;
        cell.textField  = label;
        [cell addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:cell_pad],
            [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-cell_pad],
            [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    show_item(item, cell.textField);
    return cell;
}

/*
 * outlineView:isGroupItem: — the Lists header is a section header: AppKit
 * draws it as one and never selects it.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the row
 *
 * Output:
 *   YES for the header.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline
isGroupItem:(id)item
{
    return ((TasksSidebarItem *)item).sel.kind == task_sel_header;
}

/*
 * outlineView:shouldSelectItem: — every row but the header can be selected.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the row
 *
 * Output:
 *   NO for the header.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline
shouldSelectItem:(id)item
{
    return ((TasksSidebarItem *)item).sel.kind != task_sel_header;
}

/*
 * outlineViewSelectionDidChange: — the user chose a row: tell the window,
 * which owns the selection.
 *
 * Inputs:
 *   note — NSOutlineViewSelectionDidChangeNotification (unused)
 */
- (void)
outlineViewSelectionDidChange:(NSNotification *)note
{
    NSInteger row = self.outline.selectedRow;

    if (self.reloading || row < 0) return;
    [self.library select:((TasksSidebarItem *)[self.outline itemAtRow:row]).sel];
}

/*
 * outlineViewItemWillCollapse: — the user is folding the header or a
 * group.  If the selected row is inside, it is about to leave the screen,
 * and one row must always be selected: move the selection first.  Folding
 * a group selects the group, which shows the same tasks and more; folding
 * the header leaves only the views to choose from.
 *
 * Inputs:
 *   note — NSOutlineViewItemWillCollapseNotification; its "NSObject" is
 *          the row being folded
 */
- (void)
outlineViewItemWillCollapse:(NSNotification *)note
{
    TasksSidebarItem *folding = note.userInfo[@"NSObject"];
    NSInteger         row     = self.outline.selectedRow;

    if (row < 0 || ![self is_item:[self.outline itemAtRow:row] inside:folding]) return;

    NSInteger target = folding.sel.kind == task_sel_group
        ? [self.outline rowForItem:folding] : [self row_to_select_hiding:folding];
    if (target >= 0) [self select_row:target];
}

/*
 * note_item:collapsed: — remember that a container was folded or opened,
 * and fit the pane to the rows now on screen.
 *
 * Inputs:
 *   item      — the header or a group
 *   collapsed — YES when it was folded
 */
- (void)
note_item:(TasksSidebarItem *)item collapsed:(BOOL)collapsed
{
    if (item.sel.kind == task_sel_header) {
        self.header_collapsed = collapsed;
    } else if (collapsed) {
        [self.collapsed_groups addObject:@(item.sel.id)];
    } else {
        [self.collapsed_groups removeObject:@(item.sel.id)];
    }
    if (!self.reloading) [self queue_fit];      /* reload fits once, at its end */
}

/*
 * outlineViewItemDidCollapse: — a container was folded.
 *
 * Inputs:
 *   note — NSOutlineViewItemDidCollapseNotification; its "NSObject" is the
 *          row
 */
- (void)
outlineViewItemDidCollapse:(NSNotification *)note
{
    [self note_item:note.userInfo[@"NSObject"] collapsed:YES];
}

/*
 * outlineViewItemDidExpand: — a container was opened (by the user, or by
 * reload opening the one that holds the selection).
 *
 * Inputs:
 *   note — NSOutlineViewItemDidExpandNotification; its "NSObject" is the
 *          row
 */
- (void)
outlineViewItemDidExpand:(NSNotification *)note
{
    [self note_item:note.userInfo[@"NSObject"] collapsed:NO];
}

/*
 * row_double_clicked: — a double-click on a list opens its Edit List
 * sheet; on the header or a group it folds or opens it.
 *
 * Inputs:
 *   sender — the outline (unused)
 */
- (void)
row_double_clicked:(id)sender
{
    NSInteger row = self.outline.clickedRow;
    if (row < 0) return;

    TasksSidebarItem *item = [self.outline itemAtRow:row];
    if (item.sel.kind == task_sel_list) {
        [self.library perform_command:TasksCommandEditList param:nil];
    } else if ([self.outline isItemExpanded:item]) {
        [self.outline collapseItem:item];
    } else {
        [self.outline expandItem:item];
    }
}

/* ── Context menu ────────────────────────────────────────────────────── */

/*
 * group_menu_excluding: — the "Move to Group" submenu: every group but the
 * one the list is already in.
 *
 * Inputs:
 *   group_id — the list's own group, or 0
 *   target   — who receives the items' command (the library window)
 *
 * Output:
 *   the submenu; empty when there is nowhere to move to.
 */
- (NSMenu *)
group_menu_excluding:(int64_t)group_id target:(id)target
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];

    for (TasksSidebarItem *group in self.header.children) {
        if (group.sel.kind != task_sel_group || group.sel.id == group_id) continue;
        NSMenuItem *item = tasks_command_menu_item(TasksCommandMoveToGroup, @(group.sel.id), target);
        item.title = group.label;
        [menu addItem:item];
    }
    return menu;
}

/*
 * menuNeedsUpdate: — fill the context menu for the row under the pointer.
 * AppKit does not select on a right-click, so the row is selected first
 * (the header cannot be): every command then acts on the selection, the
 * same thing its menu-bar version acts on.
 *
 * Any row, and the empty space below the rows, offers New List and New
 * Group; a list adds its own commands, a group its own.
 *
 * Inputs:
 *   menu — the outline's menu
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    TasksLibraryWindow *library = self.library;
    NSInteger           row     = self.outline.clickedRow;
    TasksSidebarItem   *item    = row >= 0 ? [self.outline itemAtRow:row] : nil;

    [menu removeAllItems];
    if (!library || self.is_shut_down) return;

    if (item != nil && item.sel.kind != task_sel_header && row != self.outline.selectedRow) {
        [self select_row:row];
    }

    [menu addItem:tasks_command_menu_item(TasksCommandNewList, nil, library)];
    [menu addItem:tasks_command_menu_item(TasksCommandNewGroup, nil, library)];

    if (item.sel.kind == task_sel_list) {
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_menu_item(TasksCommandEditList, nil, library)];
        [menu addItem:tasks_command_menu_item(TasksCommandDeleteList, nil, library)];

        NSMenu *groups = [self group_menu_excluding:item.group_id target:library];
        if (groups.numberOfItems > 0 || item.group_id != 0) {
            [menu addItem:NSMenuItem.separatorItem];
        }
        if (groups.numberOfItems > 0) {
            NSMenuItem *holder = [[NSMenuItem alloc]
                initWithTitle:tasks_command_spec(TasksCommandMoveToGroup)->title
                       action:NULL
                keyEquivalent:@""];
            holder.submenu = groups;
            [menu addItem:holder];
        }
        if (item.group_id != 0) {
            [menu addItem:tasks_command_menu_item(TasksCommandRemoveFromGroup, nil, library)];
        }
    } else if (item.sel.kind == task_sel_group) {
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_menu_item(TasksCommandRenameGroup, @(item.sel.id), library)];
        [menu addItem:tasks_command_menu_item(TasksCommandDeleteGroup, @(item.sel.id), library)];
    }
}

/* ── List and group sheets ───────────────────────────────────────────── */

/*
 * save_list:name:emoji: — the list sheet was accepted: create the list, or
 * update the one being edited.  A new list becomes the selection.  An
 * empty name does nothing.
 *
 * Inputs:
 *   list_id — the list being edited, or 0 to create one
 *   name    — the name as typed, already trimmed
 *   emoji   — the emoji, @"" for none
 */
- (void)
save_list:(int64_t)list_id name:(NSString *)name emoji:(NSString *)emoji
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down || name.length == 0) return;

    TaskApp *app = library.app;
    if (list_id != 0) {
        task_db_list_update(app->db, list_id, name.UTF8String, emoji.UTF8String);
        [library full_refresh];
        task_app_status(app, "Updated list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
        return;
    }

    int64_t created = task_db_list_create(app->db, name.UTF8String, emoji.UTF8String);
    if (created == 0) {
        task_app_status(app, "Could not create the list \xe2\x80\x94 database write failed");
        return;
    }
    [library select:(task_sel_t){ task_sel_list, created }];
    [library full_refresh];
    [self.outline scrollRowToVisible:self.outline.selectedRow];
    task_app_status(app, "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
}

/*
 * show_list_sheet: — the New List / Edit List sheet: an emoji slot and the
 * name.  The name has the keyboard when the sheet opens.
 *
 * Inputs:
 *   list_id — the list to edit, or 0 for a new one
 */
- (void)
show_list_sheet:(int64_t)list_id
{
    TasksLibraryWindow *library = self.library;
    TaskList *list = list_id != 0 ? task_db_list_get(library.app->db, list_id) : NULL;
    if (list_id != 0 && list == NULL) return;

    TasksEmojiField *emoji = [[TasksEmojiField alloc] initWithFrame:NSZeroRect];
    emoji.stringValue       = tasks_string(list != NULL ? list->emoji : "");
    emoji.placeholderString = emoji_placeholder;
    emoji.toolTip           = emoji_tooltip;
    emoji.alignment         = NSTextAlignmentCenter;
    [emoji.widthAnchor constraintEqualToConstant:emoji_field_w].active = YES;

    NSTextField *name = name_field(tasks_string(list != NULL ? list->name : ""), @"List name");
    task_list_free(list);

    tasks_ui_sheet(library.window,
                   sheet_content(list_id != 0 ? @"Edit List" : @"New List", @[emoji, name]),
                   list_id != 0 ? @"Save" : @"Create", ^(BOOL accepted) {
        if (accepted) [self save_list:list_id name:trimmed(name) emoji:emoji.stringValue];
    });

    /* The emoji slot is the sheet's first field and would take the
     * keyboard.  The name is what is typed, so it gets it instead, one
     * run-loop pass later: the sheet sets its own first responder as it
     * appears, and would undo a choice made now. */
    __weak NSTextField *weak_name = name;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSTextField *field = weak_name;
        [field.window makeFirstResponder:field];
    });
}

/*
 * save_group:name: — the group sheet was accepted: create the group, or
 * rename the one being edited.  An empty name does nothing.
 *
 * Inputs:
 *   group_id — the group being renamed, or 0 to create one
 *   name     — the name as typed, already trimmed
 */
- (void)
save_group:(int64_t)group_id name:(NSString *)name
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down || name.length == 0) return;

    TaskApp *app = library.app;
    if (group_id != 0) {
        task_db_group_rename(app->db, group_id, name.UTF8String);
    } else if (task_db_group_create(app->db, name.UTF8String) == 0) {
        task_app_status(app, "Failed to create group");
        return;
    }
    [library full_refresh];
}

/*
 * show_group_sheet: — the New Group / Rename Group sheet: one name field.
 *
 * Inputs:
 *   group_id — the group to rename, or 0 for a new one
 */
- (void)
show_group_sheet:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    TaskGroup *group = group_id != 0 ? task_db_group_get(library.app->db, group_id) : NULL;
    if (group_id != 0 && group == NULL) return;

    NSTextField *name = name_field(tasks_string(group != NULL ? group->name : ""), @"Group name");
    task_group_free(group);

    tasks_ui_sheet(library.window,
                   sheet_content(group_id != 0 ? @"Rename Group" : @"New Group", @[name]),
                   group_id != 0 ? @"Rename" : @"Create", ^(BOOL accepted) {
        if (accepted) [self save_group:group_id name:trimmed(name)];
    });
}

/* ── Deleting ────────────────────────────────────────────────────────── */

/*
 * close_editors_of_list: — close the editor of every task in a list that
 * is about to be deleted.  A closing editor flushes its pending save, and
 * that must land before the tasks go, not on rows already deleted.
 *
 * Inputs:
 *   app     — the application context
 *   list_id — the list
 */
static void
close_editors_of_list(TaskApp *app, int64_t list_id)
{
    GPtrArray *tasks = task_db_tasks_in_list_all(app->db, list_id);

    for (guint i = 0; i < tasks->len; i++) {
        tasks_editor_close(((const Task *)g_ptr_array_index(tasks, i))->id);
    }
    task_ptr_array_free_tasks(tasks);
}

/*
 * delete_list: — ask, then delete a list and every task in it, and forget
 * the orders saved for it.  The selection falls to the first list (reload).
 *
 * Inputs:
 *   list_id — the list
 */
- (void)
delete_list:(int64_t)list_id
{
    TasksLibraryWindow *library = self.library;
    TaskList *list = task_db_list_get(library.app->db, list_id);
    if (list == NULL) return;

    NSString *name  = tasks_string(list->name);
    NSString *title = [NSString stringWithFormat:@"Delete the list “%@” and all of its tasks?",
                       name];
    task_list_free(list);

    tasks_ui_confirm(library.window, title, @"You cannot undo this action.", @"Delete", YES,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *window = self.library;
        if (!confirmed || !window || self.is_shut_down) return;

        TaskApp *app = window.app;
        close_editors_of_list(app, list_id);
        task_db_list_delete(app->db, list_id);
        task_order_drop((task_sel_t){ task_sel_list, list_id });
        [window full_refresh];
        task_app_status(app, "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
    });
}

/*
 * delete_group: — ask, then remove a group and forget the orders saved for
 * its aggregate view.  Its lists stay, ungrouped; if the group was the
 * selection, that falls to the first list (reload).
 *
 * Inputs:
 *   group_id — the group
 */
- (void)
delete_group:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    TaskGroup *group = task_db_group_get(library.app->db, group_id);
    if (group == NULL) return;

    NSString *title = [NSString stringWithFormat:@"Remove the group “%@”?",
                       tasks_string(group->name)];
    task_group_free(group);

    tasks_ui_confirm(library.window, title, @"Its lists will become ungrouped.", @"Remove", NO,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *window = self.library;
        if (!confirmed || !window || self.is_shut_down) return;

        task_db_group_delete(window.app->db, group_id);
        task_order_drop((task_sel_t){ task_sel_group, group_id });
        [window full_refresh];
    });
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * set_group_of_selected_list: — file the selected list under a group, or
 * under none.
 *
 * Inputs:
 *   group_id — the group, or 0 to take the list out of its group
 */
- (void)
set_group_of_selected_list:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    int64_t list_id = task_sel_list_id(library.selection);
    if (list_id == 0) return;

    task_db_list_set_group(library.app->db, list_id, group_id);
    [library full_refresh];
}

/*
 * perform_command:param: — carry out one of the sidebar's commands.
 * Inputs are documented in sidebar.h.
 *
 * Edit List and Delete List act on the window's selection.  Delete List
 * with a GROUP selected removes that group: its title says so
 * (update_state:for_command:).
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down) return;

    task_sel_t selection = library.selection;
    switch (command) {
    case TasksCommandNewList:
        [self show_list_sheet:0];
        break;
    case TasksCommandNewGroup:
        [self show_group_sheet:0];
        break;
    case TasksCommandEditList:
        if (selection.kind == task_sel_list) [self show_list_sheet:selection.id];
        break;
    case TasksCommandDeleteList:
        if (selection.kind == task_sel_list) [self delete_list:selection.id];
        if (selection.kind == task_sel_group) [self delete_group:selection.id];
        break;
    case TasksCommandMoveToGroup:
        [self set_group_of_selected_list:param.longLongValue];
        break;
    case TasksCommandRemoveFromGroup:
        [self set_group_of_selected_list:0];
        break;
    case TasksCommandRenameGroup:
        [self show_group_sheet:param.longLongValue];
        break;
    case TasksCommandDeleteGroup:
        [self delete_group:param.longLongValue];
        break;
    default:
        break;                      /* not one of the sidebar's */
    }
}

/*
 * update_state:for_command: — the current look of one of the sidebar's
 * commands.  A command that has nothing to act on is disabled rather than
 * left to refuse when chosen.
 * Inputs are documented in sidebar.h.
 */
- (void)
update_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    task_sel_t selection = self.library.selection;

    switch (command) {
    case TasksCommandEditList:
    case TasksCommandMoveToGroup:
        state.enabled = selection.kind == task_sel_list;
        break;
    case TasksCommandDeleteList:
        state.enabled = selection.kind == task_sel_list || selection.kind == task_sel_group;
        if (selection.kind == task_sel_group) {
            state.title = tasks_command_spec(TasksCommandDeleteGroup)->title;
        }
        break;
    case TasksCommandRemoveFromGroup:
        state.enabled = [self selected_list].group_id != 0;
        break;
    case TasksCommandRenameGroup:
    case TasksCommandDeleteGroup:
        /* Their items exist only in a group's context menu, which selects
         * the group before it opens. */
        state.enabled = selection.kind == task_sel_group;
        break;
    default:
        break;                      /* New List, New Group: always available */
    }
}

/* ── Drag-reorder of lists ───────────────────────────────────────────── */

/*
 * outlineView:pasteboardWriterForItem: — what a dragged row puts on the
 * pasteboard.  Only lists drag.  The list's id rides along as the item's
 * content; the drop itself uses the row recorded when the drag began,
 * since a drag never leaves this outline.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the row the drag starts on
 *
 * Output:
 *   a pasteboard item for a list; nil for any other row, which refuses
 *   the drag.
 */
- (id<NSPasteboardWriting>)
outlineView:(NSOutlineView *)outline
pasteboardWriterForItem:(id)item
{
    TasksSidebarItem *row = item;
    if (row.sel.kind != task_sel_list) return nil;

    NSPasteboardItem *writer = [[NSPasteboardItem alloc] init];
    [writer setString:@(row.sel.id).stringValue forType:list_drag_type];
    return writer;
}

/*
 * outlineView:draggingSession:willBeginAtPoint:forItems: — a drag began:
 * remember the list being dragged.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   session — the drag (unused)
 *   point   — where it began (unused)
 *   items   — the dragged rows: one list
 */
- (void)
outlineView:(NSOutlineView *)outline
draggingSession:(NSDraggingSession *)session
willBeginAtPoint:(NSPoint)point
   forItems:(NSArray *)items
{
    self.dragged = items.firstObject;
}

/*
 * outlineView:draggingSession:endedAtPoint:operation: — the drag is over,
 * dropped or not: forget the list.
 *
 * Inputs:
 *   outline   — the outline (unused)
 *   session   — the drag (unused)
 *   point     — where it ended (unused)
 *   operation — what the drop did (unused)
 */
- (void)
outlineView:(NSOutlineView *)outline
draggingSession:(NSDraggingSession *)session
endedAtPoint:(NSPoint)point
  operation:(NSDragOperation)operation
{
    self.dragged = nil;
}

/*
 * resolve_drop_on:index:container:child: — turn the outline's proposed
 * drop into the place the dragged list would take: a container (a group,
 * or the header for "ungrouped") and a position among its children.
 *
 *   - between two rows of a group, or of the ungrouped lists: that place;
 *   - ON a list: in front of it, in its container (the GTK build's rule:
 *     "drop before the marked row, adopt its group");
 *   - ON a group or the header: at its end;
 *   - below the last row: the end of the ungrouped lists;
 *   - among the groups themselves: the head of the ungrouped lists, since
 *     ungrouped lists are always shown after the groups;
 *   - on or among the views: refused.
 *
 * A drop that would leave the list exactly where it is is refused too, so
 * the outline shows no insertion mark for it.
 *
 * Inputs:
 *   item      — the proposed parent row; nil for the top level
 *   index     — the proposed child index, or NSOutlineViewDropOnItemIndex
 *   container — receives the header or a group
 *   child     — receives the index among the container's children
 *
 * Output:
 *   YES when the drop is allowed and the two outputs are set.
 */
- (BOOL)
resolve_drop_on:(TasksSidebarItem *)item
          index:(NSInteger)index
      container:(TasksSidebarItem * __autoreleasing *)container
          child:(NSInteger *)child
{
    TasksSidebarItem *dragged = self.dragged;
    BOOL              on_item = index == NSOutlineViewDropOnItemIndex;
    TasksSidebarItem *into;
    NSInteger         at;

    if (dragged == nil) return NO;

    if (item == nil) {
        if (!on_item && index < (NSInteger)self.roots.count) return NO;
        into = self.header;
        at   = (NSInteger)into.children.count;
    } else if (item.sel.kind == task_sel_list) {
        into = [self container_of_group:item.group_id];
        at   = (NSInteger)[self index_of_list:item.sel.id in:into];
    } else if (item.sel.kind == task_sel_view) {
        return NO;
    } else {
        into = item;
        at   = on_item ? (NSInteger)into.children.count : index;
        if (into == self.header) at = MAX(at, [self group_count]);
    }

    TasksSidebarItem *from = [self container_of_group:dragged.group_id];
    NSInteger         own  = (NSInteger)[self index_of_list:dragged.sel.id in:from];
    if (into == from && (at == own || at == own + 1)) return NO;

    *container = into;
    *child     = at;
    return YES;
}

/*
 * outlineView:validateDrop:proposedItem:proposedChildIndex: — whether the
 * dragged list may be dropped here, moving the insertion mark to where it
 * would really land.
 *
 * Inputs:
 *   outline — the outline
 *   info    — the drag (unused: the list was recorded when it began)
 *   item    — the proposed parent row; nil for the top level
 *   index   — the proposed child index, or NSOutlineViewDropOnItemIndex
 *
 * Output:
 *   NSDragOperationMove, or NSDragOperationNone to refuse.
 */
- (NSDragOperation)
outlineView:(NSOutlineView *)outline
validateDrop:(id<NSDraggingInfo>)info
proposedItem:(id)item
proposedChildIndex:(NSInteger)index
{
    TasksSidebarItem *container = nil;
    NSInteger         child     = 0;

    if (![self resolve_drop_on:item index:index container:&container child:&child]) {
        return NSDragOperationNone;
    }
    /* A drop ON a group or the header keeps its highlight round the row
     * (a folded group has no place to draw a line in); every other drop
     * shows the line where the list will go. */
    if (!(index == NSOutlineViewDropOnItemIndex && item == container)) {
        [outline setDropItem:container dropChildIndex:child];
    }
    return NSDragOperationMove;
}

/*
 * lists_before:child: — how many lists are shown above a place in the
 * outline: the place's index in the lists' one saved order.
 *
 * Inputs:
 *   container — the header or a group
 *   child     — an index among its children (for the header, not before
 *               the first ungrouped list)
 *
 * Output:
 *   the count.
 */
- (int)
lists_before:(TasksSidebarItem *)container child:(NSInteger)child
{
    NSInteger before = 0;

    for (TasksSidebarItem *group in self.header.children) {
        if (group.sel.kind != task_sel_group) break;
        if (group == container) return (int)(before + child);
        before += (NSInteger)group.children.count;
    }
    return (int)(before + child - [self group_count]);
}

/*
 * outlineView:acceptDrop:item:childIndex: — the dragged list was dropped:
 * file it under the group it landed in, save the lists' new order, and
 * rebuild.
 *
 * The rebuild waits for the next run-loop pass, because it replaces every
 * row of the outline that is still in the middle of handling this drop.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   info    — the drag (unused: the list was recorded when it began)
 *   item    — the parent row the mark was on; nil for the top level
 *   index   — the child index, or NSOutlineViewDropOnItemIndex
 *
 * Output:
 *   YES when the list was moved.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline
 acceptDrop:(id<NSDraggingInfo>)info
       item:(id)item
 childIndex:(NSInteger)index
{
    TasksLibraryWindow *library   = self.library;
    TasksSidebarItem   *container = nil;
    NSInteger           child     = 0;

    if (!library || self.is_shut_down ||
        ![self resolve_drop_on:item index:index container:&container child:&child]) {
        return NO;
    }

    /* Every list id, in the order shown: grouped lists group by group,
     * then the ungrouped ones. */
    NSMutableData *order = [NSMutableData data];
    for (TasksSidebarItem *row in self.header.children) {
        for (TasksSidebarItem *list in row.children ?: @[row]) {
            int64_t list_id = list.sel.id;
            [order appendBytes:&list_id length:sizeof list_id];
        }
    }
    int64_t *ids   = order.mutableBytes;
    int      count = (int)(order.length / sizeof *ids);

    TaskDatabase *db       = library.app->db;
    int64_t       list_id  = self.dragged.sel.id;
    int64_t       group_id = container.sel.kind == task_sel_group ? container.sel.id : 0;

    if (group_id != self.dragged.group_id) task_db_list_set_group(db, list_id, group_id);

    /* The order is saved even when the move left it as it was (the last
     * list of a group dropped at the head of the ungrouped ones): until an
     * order has been saved the database lists alphabetically, which would
     * put a list that changed group somewhere other than where it was
     * dropped.  The cast: GLib spells the same 64-bit integer as a
     * different C type. */
    task_sidebar_move_list(ids, count, list_id, [self lists_before:container child:child]);
    task_db_lists_reorder(db, (const gint64 *)ids, (gsize)count);

    __weak TasksLibraryWindow *weak_library = library;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weak_library full_refresh];
    });
    return YES;
}

/* ── Width fitting ───────────────────────────────────────────────────── */

/*
 * natural_width — the pane width at which the widest row on screen shows
 * its whole label.
 *
 * Each row is measured with the font it is drawn in (show_item), rounded
 * up to whole points: 42.3 pt of text does not fit in 42.3 pt.  What the
 * outline keeps for itself in that row — indentation, the disclosure
 * control, the style's insets, the spacing between cells — is read from
 * its own geometry rather than assumed.  A vertical scroller that takes
 * room is added on top.
 *
 * Output:
 *   the width in points.
 */
- (CGFloat)
natural_width
{
    NSOutlineView *outline = self.outline;
    CGFloat        widest  = 0.0;

    for (NSInteger row = 0; row < outline.numberOfRows; row++) {
        show_item([outline itemAtRow:row], self.measure_field);
        CGFloat text     = ceil(self.measure_field.fittingSize.width);
        CGFloat overhead = NSWidth(outline.bounds) - NSWidth([outline frameOfCellAtColumn:0 row:row]);
        widest = MAX(widest, overhead + cell_pad + text + cell_pad);
    }

    BOOL overflows = NSHeight(outline.frame) > NSHeight(self.scroll.contentView.bounds);
    return [NSScrollView frameSizeForContentSize:NSMakeSize(widest, NSHeight(self.scroll.frame))
                         horizontalScrollerClass:Nil
                           verticalScrollerClass:overflows ? NSScroller.class : Nil
                                      borderType:self.scroll.borderType
                                     controlSize:NSControlSizeRegular
                                   scrollerStyle:self.scroll.scrollerStyle].width;
}

/*
 * enclosing_split — the split view this pane is a pane of.  Not simply the
 * superview: a split view controller wraps each pane in a view of its own.
 *
 * Output:
 *   the split view, or nil while the pane is in none.
 */
- (NSSplitView *)
enclosing_split
{
    NSView *view = self.superview;

    while (view != nil && ![view isKindOfClass:NSSplitView.class]) view = view.superview;
    return (NSSplitView *)view;
}

/*
 * apply_fit — move the split view's divider so the rows on screen fit:
 * never narrower than the pane's minimum, never more than its share of the
 * window.
 *
 * The divider moves only when the rows ask for a different width than at
 * the last fit, so a refresh that changes no label leaves a width the user
 * dragged alone.  A hidden pane is not fitted (a collapsed split item has
 * no divider position to set); it is fitted at the next change once shown.
 */
- (void)
apply_fit
{
    NSSplitView *split = [self enclosing_split];

    if (self.is_shut_down || !split || self.hiddenOrHasHiddenAncestor) return;
    [split layoutSubtreeIfNeeded];

    CGFloat natural = [self natural_width];
    if (natural == self.fitted_width) return;
    self.fitted_width = natural;

    CGFloat widest = floor(NSWidth(split.frame) * tasks_look_sidebar_max_frac);
    [split setPosition:MAX(tasks_look_sidebar_min_w, MIN(natural, widest))
      ofDividerAtIndex:sidebar_divider];
}

/*
 * queue_fit — fit the pane's width on the next run-loop pass, once however
 * many changes ask for it in this one.  It waits because the outline has
 * to lay its rows out before they can be measured, and at launch the
 * window itself has not been laid out yet.
 */
- (void)
queue_fit
{
    if (self.fit_pending) return;
    self.fit_pending = YES;

    __weak TasksSidebar *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TasksSidebar *s = ws;
        if (!s) return;
        s.fit_pending = NO;
        [s apply_fit];
    });
}

/* ── Lifetime ────────────────────────────────────────────────────────── */

/*
 * shut_down — the window is closing: stop answering the outline, so that
 * nothing it does on the way out (a selection change, a menu) reaches the
 * window again.  Idempotent.
 */
- (void)
shut_down
{
    self.is_shut_down          = YES;
    self.dragged               = nil;
    self.outline.delegate      = nil;
    self.outline.menu.delegate = nil;
}

@end
