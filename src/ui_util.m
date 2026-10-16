#import "ui_util.h"
#import "look.h"
#import "tool_panel.h"

#include "task_config.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/* The bundled artwork that is the app's icon. */
static NSString * const app_icon_name = @"document";

/* The app icon is rendered at this many pixels per point: sharp on a Retina
 * display, and still small on a 1x one. */
static const CGFloat icon_scale = 2.0;

/* ── Text ───────────────────────────────────────────────────────────── */

/*
 * tasks_string — core text as an NSString, never nil.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
tasks_string(const char *text)
{
    if (text == NULL) return @"";
    return [NSString stringWithUTF8String:text]
        ?: [NSString stringWithCString:text encoding:NSISOLatin1StringEncoding]
        ?: @"";
}

/*
 * tasks_title_string — a task's title as shown.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
tasks_title_string(const Task *task, NSString *untitled)
{
    return task->title != NULL && *task->title != '\0' ? tasks_string(task->title)
                                                       : untitled;
}

/*
 * tasks_tag_title — a tag's label, for a menu item or a button.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
tasks_tag_title(const TaskTag *tag)
{
    gchar    *label = task_tag_label(tag);
    NSString *title = tasks_string(label);
    g_free(label);
    return title;
}

/*
 * tasks_label_name — the name after a label's "emoji  " prefix.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
tasks_label_name(NSString *label)
{
    NSRange gap = [label rangeOfString:@"  "];
    return gap.location == NSNotFound ? label : [label substringFromIndex:NSMaxRange(gap)];
}

/* ── Images ─────────────────────────────────────────────────────────── */

/*
 * cached_icon — the image kept under a name and size, made on first use.
 * Icons are asked for on every toolbar validation, so each is built once.
 * Bounded by construction: a dozen names, a handful of sizes.
 *
 * Inputs:
 *   name — the icon's name (a symbol name, or the app icon's file name)
 *   size — its size in points
 *   make — builds the image when it is not cached yet; may return nil,
 *          which is not cached
 *
 * Output:
 *   the shared image, or nil when `make` could not build it.
 */
static NSImage *
cached_icon(NSString *name, CGFloat size, NSImage *(^make)(void))
{
    static NSMutableDictionary<NSString *, NSImage *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; });

    NSString *key  = [NSString stringWithFormat:@"%@@%g", name, size];
    NSImage  *icon = cache[key];
    if (!icon) {
        icon = make();
        if (icon) cache[key] = icon;
    }
    return icon;
}

/*
 * tasks_icon — an SF Symbol as a template image, cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_icon(NSString *name, CGFloat size)
{
    return cached_icon(name, size, ^NSImage *{
        NSImageSymbolConfiguration *config =
            [NSImageSymbolConfiguration configurationWithPointSize:size
                                                            weight:NSFontWeightRegular];
        return [[NSImage imageWithSystemSymbolName:name accessibilityDescription:nil]
                   imageWithSymbolConfiguration:config];
    });
}

/*
 * tasks_symbol — an SF Symbol at the toolbar's own size, cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_symbol(NSString *name)
{
    return cached_icon(name, 0.0, ^NSImage *{
        return [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    });
}

/*
 * scaled_bitmap — a copy of an image holding ONE bitmap of size x size
 * points at icon_scale, so the full-size source can be let go.
 *
 * Inputs:
 *   source — the image as loaded from the bundle
 *   size   — edge length in points
 *
 * Output:
 *   new NSImage, or nil if the bitmap cannot be created.
 */
static NSImage *
scaled_bitmap(NSImage *source, CGFloat size)
{
    NSInteger pixels = (NSInteger)ceil(size * icon_scale);
    NSBitmapImageRep *rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:pixels
                                                pixelsHigh:pixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSCalibratedRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    if (!rep) return nil;
    rep.size = NSMakeSize(size, size);

    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    ctx.imageInterpolation = NSImageInterpolationHigh;
    NSGraphicsContext.currentContext = ctx;
    [source drawInRect:NSMakeRect(0, 0, size, size)
              fromRect:NSZeroRect
             operation:NSCompositingOperationCopy
              fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];

    NSImage *icon = [[NSImage alloc] initWithSize:NSMakeSize(size, size)];
    [icon addRepresentation:rep];
    return icon;
}

/*
 * tasks_app_icon — the app's own icon, scaled once and cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_app_icon(CGFloat size)
{
    return cached_icon(app_icon_name, size, ^NSImage *{
        /* Loaded from its file rather than with imageNamed:, which would
         * keep the full-size image in AppKit's named-image cache for good. */
        NSString *path   = [NSBundle.mainBundle pathForResource:app_icon_name ofType:@"png"];
        NSImage  *source = path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
        return source ? scaled_bitmap(source, size) : nil;
    });
}

/*
 * tasks_image_view — an image view framed before it gets its image.
 * Inputs/output are documented in ui_util.h.
 */
NSImageView *
tasks_image_view(NSImage *image, CGFloat size)
{
    NSImageView *view = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, size, size)];
    view.imageScaling = NSImageScaleProportionallyDown;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [view.widthAnchor constraintEqualToConstant:size].active  = YES;
    [view.heightAnchor constraintEqualToConstant:size].active = YES;
    view.image = image;             /* last: the view now has a target size */
    return view;
}

/* ── Tables ─────────────────────────────────────────────────────────── */

/*
 * delegate_selection — the rows a table's delegate lets a click on one row
 * select, asked the way NSTableView asks it: the delegate's
 * tableView:selectionIndexesForProposedSelection: when it has one, else
 * its tableView:shouldSelectRow:.
 *
 * Inputs:
 *   table — the table
 *   row   — the clicked row, >= 0
 *
 * Output:
 *   the rows to select: just `row` when the delegate has neither method,
 *   possibly empty when it refuses.
 */
static NSIndexSet *
delegate_selection(NSTableView *table, NSInteger row)
{
    id<NSTableViewDelegate> delegate = table.delegate;
    NSIndexSet *proposed = [NSIndexSet indexSetWithIndex:(NSUInteger)row];

    if ([delegate respondsToSelector:@selector(tableView:selectionIndexesForProposedSelection:)]) {
        return [delegate tableView:table selectionIndexesForProposedSelection:proposed];
    }
    if ([delegate respondsToSelector:@selector(tableView:shouldSelectRow:)] &&
        ![delegate tableView:table shouldSelectRow:row]) {
        return [NSIndexSet indexSet];
    }
    return proposed;
}

@implementation TasksTable

/*
 * mouseDown: — selects the row under a plain left click at once, then lets
 * NSTableView handle the click as usual (drags, double-clicks).  Left
 * alone: a row that is already selected, so the whole selection can still
 * be dragged (NSTableView narrows it on mouse-up if there was no drag);
 * ⌘-, ⇧- and ^-clicks, which extend the selection or open the menu; a
 * press on a button in a cell; a row the delegate refuses.
 *
 * Inputs:
 *   event — the mouse-down event
 */
- (void)
mouseDown:(NSEvent *)event
{
    NSEventModifierFlags mods = event.modifierFlags & (NSEventModifierFlagCommand |
                                                       NSEventModifierFlagShift |
                                                       NSEventModifierFlagControl);
    NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];

    if (mods == 0 && event.clickCount == 1 && row >= 0 && ![self isRowSelected:row]) {
        NSView *hit = [self hitTest:[self.superview convertPoint:event.locationInWindow
                                                        fromView:nil]];
        NSIndexSet *rows = [hit isKindOfClass:NSButton.class] ? nil
                                                               : delegate_selection(self, row);
        if (rows.count > 0) [self selectRowIndexes:rows byExtendingSelection:NO];
    }
    [super mouseDown:event];
}

@end

/* ── Dialogs ────────────────────────────────────────────────────────── */

/*
 * tasks_ui_notice — an OK tool panel.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_notice(NSWindow *parent, NSString *title, NSString *message, void (^done)(void))
{
    [TasksToolPanel show_message:title message:message symbol:@"info.circle.fill"
                         buttons:@[@"OK"] destructive:-1 over:parent
                         handler:^(NSInteger index) { if (done) done(); }];
}

/*
 * tasks_ui_confirm — a Cancel (default) / action tool panel.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_confirm(NSWindow *parent, NSString *title, NSString *message, NSString *action,
                 BOOL destructive, void (^done)(BOOL confirmed))
{
    [TasksToolPanel show_message:title message:message
                          symbol:destructive ? @"exclamationmark.triangle.fill"
                                             : @"questionmark.circle.fill"
                         buttons:@[@"Cancel", action]   /* [0] = default (Return) */
                     destructive:destructive ? 1 : -1
                            over:parent
                         handler:^(NSInteger index) { done(index == 1); }];
}

/*
 * tasks_ui_pick_path — an open panel for one file or folder.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_pick_path(NSWindow *parent, TasksPickKind kind, NSString *message, NSString *action,
                   NSArray<NSString *> *extensions, NSString *start_dir,
                   void (^done)(NSString *path))
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    BOOL folder        = kind == TasksPickFolder;

    panel.canChooseFiles          = !folder;
    panel.canChooseDirectories    = folder;
    panel.canCreateDirectories    = folder;
    panel.allowsMultipleSelection = NO;
    panel.prompt                  = action;
    if (message) panel.message = message;
    if (start_dir) panel.directoryURL = [NSURL fileURLWithPath:start_dir isDirectory:YES];

    if (!folder && extensions.count > 0) {
        NSMutableArray<UTType *> *types = [NSMutableArray array];
        for (NSString *ext in extensions) {
            UTType *type = [UTType typeWithFilenameExtension:ext];
            if (type) [types addObject:type];
        }
        panel.allowedContentTypes = types;
    }

    void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
        done(response == NSModalResponseOK ? panel.URL.path : nil);
    };
    if (parent) {
        [panel beginSheetModalForWindow:parent completionHandler:finish];
    } else {
        finish([panel runModal]);
    }
}

/*
 * TasksEmojiField — the emoji slot behind tasks_emoji_field: a text field
 * that keeps one character and opens the emoji picker when clicked.
 *
 * In its compact form (the one Ctrl+Cmd+Space shows) the picker closes
 * itself once an emoji is chosen; the slot then ends its edit, so the
 * expanded Character Viewer window, which has no API to close it, types
 * nowhere until the slot is clicked again.
 */
@interface TasksEmojiField : NSTextField
@end

@implementation TasksEmojiField

/*
 * mouseDown: — a click starts editing as usual, selects what is there so
 * that the chosen emoji replaces it, and opens the system's emoji picker
 * over the slot.
 *
 * Inputs:
 *   event — the mouse-down
 */
- (void)
mouseDown:(NSEvent *)event
{
    [super mouseDown:event];
    [self.currentEditor selectAll:nil];
    [NSApp orderFrontCharacterPalette:nil];
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

/*
 * tasks_emoji_field — the emoji slot, filled in.
 * Inputs/output are documented in ui_util.h.
 */
NSTextField *
tasks_emoji_field(NSString *emoji)
{
    TasksEmojiField *field = [[TasksEmojiField alloc] initWithFrame:NSZeroRect];
    field.stringValue       = emoji ?: @"";
    field.placeholderString = @"…";
    field.toolTip           = @"Optional emoji — click to pick";
    field.alignment         = NSTextAlignmentCenter;
    field.font              = [NSFont systemFontOfSize:tasks_look_ui_font + 3];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    [field.widthAnchor constraintEqualToConstant:tasks_look_tool_emoji_w].active = YES;
    return field;
}

/*
 * tasks_ui_place_lower_right — a window to a screen's lower-right corner.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_place_lower_right(NSWindow *window, NSScreen *screen)
{
    if (screen == nil) return;

    NSRect area = screen.visibleFrame;
    NSSize size = window.frame.size;
    [window setFrameOrigin:NSMakePoint(
        MAX(NSMaxX(area) - size.width - tasks_look_screen_margin, NSMinX(area)),
        MIN(NSMinY(area) + tasks_look_screen_margin, NSMaxY(area) - size.height))];
}
