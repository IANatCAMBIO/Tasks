#import "task_list.h"
#import "editor_window.h"
#import "look.h"
#import "row_text.h"

/*
 * STUB of the task list (work package "task list": see
 * porting/INTERFACES.md).  It honours the frozen interface with the least
 * that lets the rest of the app run: one column of tall rows drawn through
 * the shared renderer, multi-selection, the shared context menu, and
 * double-click to open the editor.  No checkbox, status or date columns, no
 * header sorting, no manual-sort drag or saved order, no stripes, no
 * column menu — the column commands report themselves disabled.  The
 * list's owner replaces this file.
 */

static NSString * const column_ident = @"task";

@interface TasksTaskList () <NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate>
@property (nonatomic, weak) TasksLibraryWindow *library;
@property (nonatomic, strong) NSTableView *table;
@property (nonatomic, strong) NSTableColumn *column;
@end

@implementation TasksTaskList {
    task_row_list_t *_rows;         /* owned; NULL while the pane is hidden */
}

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in task_list.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library = library;

    self.column = [[NSTableColumn alloc] initWithIdentifier:column_ident];
    self.column.resizingMask = NSTableColumnAutoresizingMask;

    self.table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.table.headerView              = nil;
    self.table.style                   = NSTableViewStylePlain;
    self.table.allowsMultipleSelection = YES;
    self.table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    self.table.dataSource              = self;
    self.table.delegate                = self;
    self.table.target                  = self;
    self.table.doubleAction            = @selector(row_double_clicked:);
    [self.table addTableColumn:self.column];

    /* Filled each time it opens (menuNeedsUpdate:). */
    self.table.menu          = [[NSMenu alloc] initWithTitle:@""];
    self.table.menu.delegate = self;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    scroll.autoresizingMask    = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.documentView        = self.table;
    [self addSubview:scroll];
    return self;
}

/*
 * dealloc — a pane released without shut_down still frees its rows.
 */
- (void)
dealloc
{
    task_row_list_free(_rows);
}

/* ── TasksTaskPane ───────────────────────────────────────────────────── */

/*
 * show_rows: — replace the rows, keeping the selection by task id.
 * The contract is documented in library_window.h.
 */
- (void)
show_rows:(task_row_list_t *)rows
{
    NSArray<NSNumber *> *selected = [self selected_task_ids];

    task_row_list_free(_rows);
    _rows = rows;
    [self.table reloadData];

    /* reloadData keeps row NUMBERS selected, not tasks: reselect by id. */
    NSMutableIndexSet *indexes = [NSMutableIndexSet indexSet];
    for (size_t i = 0; rows != NULL && i < rows->count; i++) {
        if ([selected containsObject:@(rows->rows[i]->id)]) [indexes addIndex:i];
    }
    [self.table selectRowIndexes:indexes byExtendingSelection:NO];
}

/*
 * selected_task_ids — the selected rows' task ids, top to bottom.
 * Output is documented in library_window.h.
 */
- (NSArray<NSNumber *> *)
selected_task_ids
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];

    if (_rows == NULL) return ids;
    [self.table.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger row, BOOL *stop) {
        if (row < self->_rows->count) [ids addObject:@(self->_rows->rows[row]->id)];
    }];
    return ids;
}

/*
 * view_changed — another sidebar row was selected: the old view's selection
 * does not carry over.
 */
- (void)
view_changed
{
    [self.table deselectAll:nil];
}

/*
 * cancel_drag — the stub has no drag to cancel.
 */
- (void)
cancel_drag
{
}

/*
 * shut_down — free the rows; the table is emptied so nothing draws them.
 */
- (void)
shut_down
{
    task_row_list_free(_rows);
    _rows = NULL;
    [self.table reloadData];
}

/* ── Table ───────────────────────────────────────────────────────────── */

/*
 * numberOfRowsInTableView: — how many rows.
 *
 * Inputs:
 *   table — the table (unused)
 *
 * Output:
 *   the row count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    return _rows != NULL ? (NSInteger)_rows->count : 0;
}

/*
 * text_width — the width the row text has to wrap in: the column, less the
 * padding either side.
 *
 * Output:
 *   the width in points.
 */
- (CGFloat)
text_width
{
    return self.column.width - 2.0 * tasks_look_row_pad_h;
}

/*
 * tableView:heightOfRow: — a row is as tall as its text at the current
 * width, plus the padding above and below.
 *
 * Inputs:
 *   table — the table (unused)
 *   row   — the row index
 *
 * Output:
 *   the height in points.
 */
- (CGFloat)
tableView:(NSTableView *)table heightOfRow:(NSInteger)row
{
    NSAttributedString *text = tasks_row_text(_rows->rows[row], NO, TasksRowLayoutWrap);

    return tasks_row_text_height(text, [self text_width]) + 2.0 * tasks_look_row_pad_v;
}

/*
 * tableView:viewForTableColumn:row: — the tall cell: a label showing the
 * shared renderer's text.
 *
 * Inputs:
 *   table  — the table
 *   column — the single column (unused)
 *   row    — the row index
 *
 * Output:
 *   a reused or new cell view.
 */
- (NSView *)
tableView:(NSTableView *)table
viewForTableColumn:(NSTableColumn *)column
              row:(NSInteger)row
{
    NSTableCellView *cell = [table makeViewWithIdentifier:column_ident owner:self];

    if (!cell) {
        NSTextField *label = [NSTextField wrappingLabelWithString:@""];
        label.translatesAutoresizingMaskIntoConstraints = NO;

        cell            = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = column_ident;
        cell.textField  = label;
        [cell addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor
                                                constant:tasks_look_row_pad_h],
            [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor
                                                 constant:-tasks_look_row_pad_h],
            [label.topAnchor constraintEqualToAnchor:cell.topAnchor
                                            constant:tasks_look_row_pad_v],
        ]];
    }
    cell.textField.attributedStringValue =
        tasks_row_text(_rows->rows[row], NO, TasksRowLayoutWrap);
    return cell;
}

/*
 * tableViewColumnDidResize: — the text wraps differently at a new width, so
 * every row's height has to be measured again.
 *
 * Inputs:
 *   note — NSTableViewColumnDidResizeNotification (unused)
 */
- (void)
tableViewColumnDidResize:(NSNotification *)note
{
    NSRange all = NSMakeRange(0, (NSUInteger)self.table.numberOfRows);

    [self.table noteHeightOfRowsWithIndexesChanged:[NSIndexSet indexSetWithIndexesInRange:all]];
}

/*
 * row_double_clicked: — open the editor for the row that was double-clicked.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
row_double_clicked:(id)sender
{
    NSInteger row = self.table.clickedRow;
    TasksLibraryWindow *library = self.library;

    if (row < 0 || _rows == NULL || !library) return;
    tasks_editor_open(library.app, _rows->rows[row]->id);
}

/*
 * menuNeedsUpdate: — the context menu is about to open.  AppKit does not
 * change the selection on a right-click, so do it here: a click inside the
 * selection keeps it, a click outside collapses it to the clicked row.
 * Then fill the menu with the window's shared task menu.
 *
 * Inputs:
 *   menu — the table's context menu
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    NSInteger row = self.table.clickedRow;

    [menu removeAllItems];
    if (row < 0) return;
    if (![self.table isRowSelected:row]) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                byExtendingSelection:NO];
    }
    NSMenu *source = [self.library task_context_menu];
    for (NSMenuItem *item in [source.itemArray copy]) {
        [source removeItem:item];
        [menu addItem:item];
    }
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * perform_command:param: — the stub has no optional columns, so it carries
 * out no command; they are disabled by update_state:for_command:.
 * Inputs are documented in task_list.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
}

/*
 * update_state:for_command: — every column command is unavailable in the
 * stub.
 * Inputs are documented in task_list.h.
 */
- (void)
update_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    state.enabled = NO;
}

@end
