#import "ui_util.h"
#import "look.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/* The bundled artwork that is the app's icon, and its size in an alert. */
static NSString * const app_icon_name = @"document";
static const CGFloat alert_icon_size  = 64.0;

/* Icons are rendered at this many pixels per point: sharp on a Retina
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

/* ── Images ─────────────────────────────────────────────────────────── */

/*
 * scaled_icon — a copy of a bundled image holding ONE bitmap of
 * size x size points at icon_scale, so the full-size source can be let go.
 *
 * Inputs:
 *   source — the image as loaded from the bundle
 *   size   — edge length in points
 *
 * Output:
 *   new NSImage, or nil if the bitmap cannot be created.
 */
static NSImage *
scaled_icon(NSImage *source, CGFloat size)
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
 * tasks_icon — a bundled icon, scaled once and cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_icon(NSString *name, CGFloat size)
{
    /* Bounded by construction: ten icons, a handful of sizes. */
    static NSMutableDictionary<NSString *, NSImage *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; });

    NSString *key  = [NSString stringWithFormat:@"%@@%g", name, size];
    NSImage  *icon = cache[key];
    if (icon) return icon;

    /* Loaded from its file rather than with imageNamed:, which would keep
     * the full-size image in AppKit's named-image cache for good. */
    NSString *path   = [NSBundle.mainBundle pathForResource:name ofType:@"png"];
    NSImage  *source = path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
    if (!source) return nil;

    icon = scaled_icon(source, size);
    if (icon) cache[key] = icon;
    return icon;
}

/*
 * tasks_app_icon — the app's own icon.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_app_icon(CGFloat size)
{
    return tasks_icon(app_icon_name, size);
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

/* ── Dialogs ────────────────────────────────────────────────────────── */

/*
 * new_alert — an NSAlert showing the app's own icon (see tasks_app_icon).
 *
 * Inputs:
 *   title   — the headline
 *   message — the detail, or nil
 *
 * Output:
 *   new NSAlert.
 */
static NSAlert *
new_alert(NSString *title, NSString *message)
{
    NSAlert *alert        = [[NSAlert alloc] init];
    NSImage *icon         = tasks_app_icon(alert_icon_size);
    if (icon) alert.icon  = icon;
    alert.messageText     = title;
    alert.informativeText = message ?: @"";
    return alert;
}

/*
 * present_alert — show an alert as a sheet on a window, or app-modal when
 * there is none, and hand the response to a block.
 *
 * Inputs:
 *   alert   — the configured alert
 *   parent  — window for the sheet, or nil
 *   handler — receives the response; not nil
 */
static void
present_alert(NSAlert *alert, NSWindow *parent, void (^handler)(NSModalResponse response))
{
    if (parent) {
        [alert beginSheetModalForWindow:parent completionHandler:handler];
    } else {
        handler([alert runModal]);
    }
}

/*
 * tasks_ui_notice — an OK alert.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_notice(NSWindow *parent, NSString *title, NSString *message, void (^done)(void))
{
    present_alert(new_alert(title, message), parent, ^(NSModalResponse response) {
        if (done) done();
    });
}

/*
 * tasks_ui_confirm — an action / Cancel alert.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_confirm(NSWindow *parent, NSString *title, NSString *message, NSString *action,
                 BOOL destructive, void (^done)(BOOL confirmed))
{
    NSAlert  *alert  = new_alert(title, message);
    NSButton *act    = [alert addButtonWithTitle:action];
    NSButton *cancel = [alert addButtonWithTitle:@"Cancel"];

    cancel.keyEquivalent = @"\e";
    if (destructive) {
        alert.alertStyle          = NSAlertStyleWarning;
        act.hasDestructiveAction  = YES;
    }
    present_alert(alert, parent, ^(NSModalResponse response) {
        done(response == NSAlertFirstButtonReturn);
    });
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
 * TasksSheetPanel — the window behind tasks_ui_sheet.  Its two buttons and
 * the Escape key end the sheet on the window it is attached to; the sheet's
 * completion handler does the rest.  AppKit keeps the panel alive while it
 * is up, so nothing has to own it.
 */
@interface TasksSheetPanel : NSPanel
@end

@implementation TasksSheetPanel

/*
 * accept_action: — the accept button: end the sheet with OK.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
accept_action:(id)sender
{
    [self.sheetParent endSheet:self returnCode:NSModalResponseOK];
}

/*
 * cancelOperation: — Escape, and the Cancel button: end the sheet with
 * Cancel.
 *
 * Inputs:
 *   sender — the button or responder that sent it (unused)
 */
- (void)
cancelOperation:(id)sender
{
    [self.sheetParent endSheet:self returnCode:NSModalResponseCancel];
}

@end

/*
 * tasks_ui_sheet — custom content above Cancel / accept, as a sheet.
 * Inputs are documented in ui_util.h.
 */
void
tasks_ui_sheet(NSWindow *parent, NSView *content, NSString *accept, void (^done)(BOOL accepted))
{
    TasksSheetPanel *panel =
        [[TasksSheetPanel alloc] initWithContentRect:NSMakeRect(0, 0, 320, 120)
                                           styleMask:NSWindowStyleMaskTitled
                                             backing:NSBackingStoreBuffered
                                               defer:YES];

    NSButton *cancel_btn = [NSButton buttonWithTitle:@"Cancel"
                                              target:panel
                                              action:@selector(cancelOperation:)];
    NSButton *accept_btn = [NSButton buttonWithTitle:accept
                                              target:panel
                                              action:@selector(accept_action:)];
    accept_btn.keyEquivalent = @"\r";

    /* The buttons hug the right edge: an empty view takes the spare width. */
    NSView *spacer = [[NSView alloc] initWithFrame:NSZeroRect];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[spacer, cancel_btn, accept_btn]];
    buttons.orientation  = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *stack = [NSStackView stackViewWithViews:@[content, buttons]];
    stack.orientation  = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment    = NSLayoutAttributeLeading;
    stack.spacing      = tasks_look_form_margin;
    stack.edgeInsets   = NSEdgeInsetsMake(tasks_look_form_margin, tasks_look_form_margin,
                                          tasks_look_form_margin, tasks_look_form_margin);
    [buttons.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;

    panel.contentView = stack;
    [panel setContentSize:stack.fittingSize];
    panel.initialFirstResponder = content.nextValidKeyView;

    [parent beginSheet:panel completionHandler:^(NSModalResponse response) {
        done(response == NSModalResponseOK);
    }];
}
