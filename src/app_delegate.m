#import "app_delegate.h"
#import "commands.h"
#import "library_window.h"
#import "settings_window.h"
#import "ui_util.h"

#include "backup.h"
#include "core_views.h"
#include "recur.h"
#include "task_config.h"
#include "task_paths.h"
#include "task_worker.h"

#include <signal.h>

/* The About panel's credits: the app's motto (carried from the GTK build),
 * then two links. */
static NSString * const about_motto       = @"Gettin' shit done since 2026!";
static NSString * const about_site_url    = @"https://camb.io";
static NSString * const about_license_url = @"https://opensource.org/license/bsd-3-clause";

/* The help page in Contents/Resources (resources/help.html). */
static NSString * const help_page_name = @"help";
static NSString * const help_page_type = @"html";

/* The Info.plist key the Makefile stamps with the build time (UTC, ISO 8601). */
static NSString * const build_date_key = @"TasksBuildDate";

/* The size of the app icon in the Dock tile and the About panel. */
static const CGFloat app_icon_size = 128.0;

@interface TasksAppDelegate () <NSMenuItemValidation, NSMenuDelegate>
/* The application context: the open database and the event bus.  Owned;
 * NULL before start_core succeeds and after tear_down. */
@property (nonatomic, assign) TaskApp *app;
/* The one library window.  Owned; nil until the core has started. */
@property (nonatomic, strong) TasksLibraryWindow *library;
/* Delivers SIGTERM on the main queue as a normal quit. */
@property (nonatomic, strong) dispatch_source_t sigterm_source;
@end

@implementation TasksAppDelegate

/* ── Launch ──────────────────────────────────────────────────────────── */

/*
 * start_core — bring the non-UI core up: the config (which settles where
 * the data lives), the registries, and the database.  Nothing here touches
 * a window, and the order is the GTK build's: config first because
 * everything reads it, registries before any timer or thread exists.
 *
 * The database is opened where the config says, or created there when it
 * does not exist yet.
 *
 * Output:
 *   nil on success (self.app is set, its database open); otherwise a
 *   message saying what failed, with self.app left NULL.
 */
- (NSString *)
start_core
{
    if (!task_config_init()) {
        char *dir = task_data_dir();
        NSString *message = [NSString stringWithFormat:
            @"The data folder cannot be used:\n%@", tasks_string(dir)];
        free(dir);
        return message;
    }

    TaskApp *app = task_app_new();
    task_core_views_init();
    task_recur_init(app);
    task_backup_init(app);

    gchar  *db_dir = task_config_get(TASK_CFG_DB_DIR);
    gchar  *path   = task_db_resolve_path(db_dir);
    GError *err    = NULL;

    app->db = path != NULL ? task_db_open(path, &err) : NULL;
    NSString *message = nil;
    if (app->db == NULL) {
        message = [NSString stringWithFormat:@"%@",
            tasks_string(err != NULL ? err->message : "The database path cannot be determined.")];
        g_clear_error(&err);
        task_app_free(app);
    } else {
        self.app = app;
    }
    g_free(path);
    g_free(db_dir);
    return message;
}

/*
 * watch_sigterm — turn SIGTERM (make install, logout, `kill`) into a normal
 * quit, so pending edits are saved and the database is closed.  A dispatch
 * source delivers the signal on the main queue; the default action is
 * switched off so the process is not killed first.
 */
- (void)
watch_sigterm
{
    signal(SIGTERM, SIG_IGN);
    self.sigterm_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0,
                                                 dispatch_get_main_queue());
    dispatch_source_set_event_handler(self.sigterm_source, ^{
        [NSApp terminate:nil];
    });
    dispatch_resume(self.sigterm_source);
}

/*
 * applicationDidFinishLaunching: — start the core, build the menu bar and
 * the library window, then arm the background workers and check the
 * database.  The workers are armed AFTER the window exists so that a
 * status message posted while arming has somewhere to land.
 *
 * If the core cannot start, the reason is shown and the app quits.  By then
 * the data directory may have been created; no existing data has been
 * changed.
 *
 * Inputs:
 *   note — NSApplicationDidFinishLaunchingNotification (unused)
 */
- (void)
applicationDidFinishLaunching:(NSNotification *)note
{
    NSImage *icon = tasks_app_icon(app_icon_size);
    if (icon) NSApp.applicationIconImage = icon;
    [self build_menu_bar];
    [self watch_sigterm];

    NSString *failure = [self start_core];
    if (failure) {
        [NSApp activate];
        tasks_ui_notice(nil, @"Could not open the database", failure, ^{
            [NSApp terminate:nil];
        });
        return;
    }

    self.library = [[TasksLibraryWindow alloc] init_with_app:self.app];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(library_will_close:)
                                               name:NSWindowWillCloseNotification
                                             object:self.library.window];
    [self.library showWindow:nil];
    [NSApp activate];

    task_worker_arm_all(self.app, self.app->db->path);
    [self.library report_database_health];

    gint n_tasks = 0, n_lists = 0;
    task_db_totals(self.app->db, &n_tasks, &n_lists);
    NSLog(@"[launch] config %@, database %@: %d tasks in %d lists",
          tasks_string(task_config_path()), tasks_string(self.app->db->path),
          n_tasks, n_lists);
}

/*
 * applicationSupportsSecureRestorableState: — the app keeps no restorable
 * state of its own (window sizes are in the config); answering YES is what
 * AppKit asks of every app and avoids its launch warning.
 *
 * Inputs:
 *   app — NSApplication (unused)
 *
 * Output: YES.
 */
- (BOOL)
applicationSupportsSecureRestorableState:(NSApplication *)app
{
    return YES;
}

/* ── Quit ────────────────────────────────────────────────────────────── */

/*
 * library_will_close: — the library window is closing, which ends the app.
 * The quit is queued for the next run-loop pass rather than made from
 * inside the notification, so the window finishes closing (and shutting
 * itself down, which closes every editor) before the app comes down.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification for the library window (unused)
 */
- (void)
library_will_close:(NSNotification *)note
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

/*
 * applicationWillTerminate: — the one place the app comes down, whichever
 * way it was asked to quit (the menu, the Dock, the window closing,
 * SIGTERM).  In order: the library window shuts down (flushing every
 * editor's pending save while the database is still open), the workers'
 * timers are cancelled, the database is closed, the config is freed.
 *
 * Inputs:
 *   note — NSApplicationWillTerminateNotification (unused)
 */
- (void)
applicationWillTerminate:(NSNotification *)note
{
    if (self.app == NULL) return;

    [NSNotificationCenter.defaultCenter removeObserver:self];
    [self.library shut_down];
    self.library = nil;
    task_worker_disarm_all();
    task_app_free(self.app);
    self.app = NULL;
    task_config_close();
    NSLog(@"[quit] database closed");
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * run_command: — the action of every main-menu command item.  The app's
 * own commands are carried out here; every other command goes to the
 * library window, whichever window is key.
 *
 * Inputs:
 *   sender — a menu item made by tasks_command_menu_item
 */
- (void)
run_command:(id)sender
{
    TasksCommand command = (TasksCommand)[sender tag];

    switch (command) {
    case TasksCommandAbout:
        [self show_about];
        break;
    case TasksCommandSettings:
        tasks_settings_window_open(self.app);
        break;
    case TasksCommandHelp:
        [self show_help];
        break;
    default:
        [self.library run_command:sender];
        break;
    }
}

/*
 * validateMenuItem: — enables the main menu's command items.  The app's own
 * commands are available whenever the core is up; the rest are the library
 * window's to answer.  Items with other actions (Quit, Close, the Edit
 * menu) are validated by AppKit and never reach here.
 *
 * Inputs:
 *   item — a menu item whose target is the delegate
 *
 * Output:
 *   YES to enable the item.
 */
- (BOOL)
validateMenuItem:(NSMenuItem *)item
{
    const tasks_command_spec_t *spec = tasks_command_spec((TasksCommand)item.tag);

    if (spec == NULL || self.app == NULL) return NO;
    if (spec->owner == TasksCommandOwnerApp) return YES;
    return [self.library validateMenuItem:item];
}

/*
 * about_credits — the About panel's credits, centred in the small system
 * font: the motto, then "camb.io · BSD License", each a link.
 *
 * Output:
 *   attributed string for NSAboutPanelOptionCredits.
 */
static NSAttributedString *
about_credits(void)
{
    NSMutableParagraphStyle *centred = [[NSMutableParagraphStyle alloc] init];
    centred.alignment = NSTextAlignmentCenter;
    NSDictionary *plain = @{
        NSFontAttributeName:            [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
        NSForegroundColorAttributeName: NSColor.labelColor,
        NSParagraphStyleAttributeName:  centred,
    };
    NSArray<NSArray<NSString *> *> *links = @[
        @[@"camb.io",     about_site_url],
        @[@"BSD License", about_license_url],
    ];

    NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
        initWithString:[about_motto stringByAppendingString:@"\n\n"] attributes:plain];
    for (NSArray<NSString *> *link in links) {
        if (link != links.firstObject) {
            [credits appendAttributedString:
                [[NSAttributedString alloc] initWithString:@"  ·  " attributes:plain]];
        }
        NSMutableDictionary *attrs = [plain mutableCopy];
        attrs[NSLinkAttributeName] = [NSURL URLWithString:link[1]];
        [credits appendAttributedString:
            [[NSAttributedString alloc] initWithString:link[0] attributes:attrs]];
    }
    return credits;
}

/*
 * show_about — the standard About panel.  Its version line reads
 * "Version <VERSION> (built <date>)": the version comes from Info.plist and
 * the date from its TasksBuildDate key, both written by the Makefile.  The
 * icon is passed explicitly so the panel does not put it on the grey
 * backdrop macOS 26 gives a non-squircle bundle icon.
 */
- (void)
show_about
{
    NSMutableDictionary *options = [NSMutableDictionary dictionary];
    options[NSAboutPanelOptionCredits] = about_credits();

    NSImage *icon = tasks_app_icon(app_icon_size);
    if (icon) options[NSAboutPanelOptionApplicationIcon] = icon;

    NSString *stamp = NSBundle.mainBundle.infoDictionary[build_date_key];
    NSDate *built   = stamp ? [[[NSISO8601DateFormatter alloc] init] dateFromString:stamp] : nil;
    if (built) {
        options[NSAboutPanelOptionApplicationVersion] =
            [@"built " stringByAppendingString:
                [NSDateFormatter localizedStringFromDate:built
                                               dateStyle:NSDateFormatterMediumStyle
                                               timeStyle:NSDateFormatterShortStyle]];
    }
    [NSApp orderFrontStandardAboutPanelWithOptions:options];
}

/*
 * show_help — Help > Tasks AppKit Help: open the bundled help page in the
 * user's browser.  (A Help Book would need an indexed help bundle; one
 * page does not earn that.)
 */
- (void)
show_help
{
    NSURL *page = [NSBundle.mainBundle URLForResource:help_page_name withExtension:help_page_type];

    if (page) [NSWorkspace.sharedWorkspace openURL:page];
}

/* ── Menu bar ────────────────────────────────────────────────────────── */

/*
 * standard_item — a menu item for one of AppKit's own actions (Quit, Close,
 * Copy, ...), sent down the responder chain.
 *
 * Inputs:
 *   title  — menu item title
 *   action — AppKit's selector
 *   key    — key equivalent character, @"" for none
 *   mods   — modifiers in addition to Command
 *
 * Output:
 *   new NSMenuItem with a nil target.
 */
static NSMenuItem *
standard_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];

    item.keyEquivalentModifierMask = NSEventModifierFlagCommand | mods;
    return item;
}

/*
 * add_menu — append a titled menu to the menu bar.
 *
 * Inputs:
 *   bar   — the menu bar
 *   title — the menu's title
 *
 * Output:
 *   the new, empty menu.
 */
static NSMenu *
add_menu(NSMenu *bar, NSString *title)
{
    NSMenuItem *holder = [[NSMenuItem alloc] init];
    NSMenu *menu       = [[NSMenu alloc] initWithTitle:title];

    holder.submenu = menu;
    [bar addItem:holder];
    return menu;
}

/*
 * destinations_item: — a "Move to" item for the main menu: its submenu is
 * left empty here and filled each time it opens (menuNeedsUpdate:), so it
 * always lists the lists or groups there are now.  The submenu carries the
 * command's identifier, which is how menuNeedsUpdate: knows what to fill
 * it with.
 *
 * Inputs:
 *   command — TasksCommandMoveToList or TasksCommandMoveToGroup
 *
 * Output:
 *   new NSMenuItem holding the submenu.
 */
- (NSMenuItem *)
destinations_item:(TasksCommand)command
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:spec->title
                                                    action:NULL
                                             keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:spec->title];

    menu.identifier = spec->ident;
    menu.delegate   = self;
    holder.submenu  = menu;
    return holder;
}

/*
 * menuNeedsUpdate: — a "Move to" submenu is about to open: have the library
 * window fill it with the current destinations.
 *
 * Inputs:
 *   menu — a submenu made by destinations_item:
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    const tasks_command_spec_t *spec = tasks_command_with_ident(menu.identifier);

    if (spec) [self.library fill_menu:menu with_destinations_of:spec->command];
}

/*
 * menuHasKeyEquivalent:forEvent:target:action: — AppKit asks whether a
 * "Move to" submenu claims a key press.  Its items have no shortcuts, and
 * saying so here stops AppKit filling the submenu (a database query) on
 * every key press just to look.
 *
 * Inputs:
 *   menu   — a submenu made by destinations_item: (unused)
 *   event  — the key press (unused)
 *   target — would receive the claiming item's target (unused)
 *   action — would receive the claiming item's action (unused)
 *
 * Output: NO.
 */
- (BOOL)
menuHasKeyEquivalent:(NSMenu *)menu
            forEvent:(NSEvent *)event
              target:(id *)target
              action:(SEL *)action
{
    return NO;
}

/*
 * build_menu_bar — the whole menu bar, in code: the standard App, File,
 * Edit, View, Window and Help menus.  Every command in the command table
 * is in one of them.
 *
 * Command items come from the command table and target the delegate, so
 * they work whichever window is key.  The Edit menu's items use AppKit's
 * standard selectors with a nil target: a focused text field's editor gets
 * them, which is what makes Cut/Copy/Paste/Undo work in every field.
 */
- (void)
build_menu_bar
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *(^command)(TasksCommand) = ^NSMenuItem *(TasksCommand cmd) {
        return tasks_command_menu_item(cmd, nil, self);
    };

    /* ── App menu ── */
    NSMenu *app_menu = add_menu(bar, @TASKS_APP_NAME);
    [app_menu addItem:command(TasksCommandAbout)];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:command(TasksCommandSettings)];
    [app_menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:NULL keyEquivalent:@""];
    services.submenu     = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu   = services.submenu;
    [app_menu addItem:services];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:standard_item(@"Hide " @TASKS_APP_NAME, @selector(hide:), @"h", 0)];
    [app_menu addItem:standard_item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                                    NSEventModifierFlagOption)];
    [app_menu addItem:standard_item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:standard_item(@"Quit " @TASKS_APP_NAME, @selector(terminate:), @"q", 0)];

    /* ── File menu ── what is created, then the selected task, the selected
     * list, the selected group, and the file all of it is kept in. */
    NSMenu *file_menu = add_menu(bar, @"File");
    [file_menu addItem:command(TasksCommandNewTask)];
    [file_menu addItem:command(TasksCommandNewList)];
    [file_menu addItem:command(TasksCommandNewGroup)];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(TasksCommandTaskInfo)];
    [file_menu addItem:command(TasksCommandDeleteTask)];
    [file_menu addItem:command(TasksCommandClearCompleted)];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(TasksCommandEditList)];
    [file_menu addItem:[self destinations_item:TasksCommandMoveToGroup]];
    [file_menu addItem:command(TasksCommandRemoveFromGroup)];
    [file_menu addItem:command(TasksCommandDeleteList)];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(TasksCommandRenameGroup)];
    [file_menu addItem:command(TasksCommandDeleteGroup)];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(TasksCommandOpenDatabase)];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:standard_item(@"Close Window", @selector(performClose:), @"w", 0)];

    /* ── Edit menu ── the standard items, then what changes the selected
     * tasks.  Both directions of each flag are offered: the selection may
     * hold tasks in either state. */
    NSMenu *edit_menu = add_menu(bar, @"Edit");
    [edit_menu addItem:standard_item(@"Undo", @selector(undo:), @"z", 0)];
    [edit_menu addItem:standard_item(@"Redo", @selector(redo:), @"z", NSEventModifierFlagShift)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItem:standard_item(@"Cut", @selector(cut:), @"x", 0)];
    [edit_menu addItem:standard_item(@"Copy", @selector(copy:), @"c", 0)];
    [edit_menu addItem:standard_item(@"Paste", @selector(paste:), @"v", 0)];
    [edit_menu addItem:standard_item(@"Delete", @selector(delete:), @"", 0)];
    [edit_menu addItem:standard_item(@"Select All", @selector(selectAll:), @"a", 0)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItem:command(TasksCommandFind)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    for (NSNumber *flag in @[@(TasksCommandMarkDone), @(TasksCommandSetPinned),
                             @(TasksCommandSetPriority)]) {
        [edit_menu addItem:tasks_command_flag_item(flag.integerValue, YES, NO, self)];
        [edit_menu addItem:tasks_command_flag_item(flag.integerValue, NO, NO, self)];
    }
    [edit_menu addItem:[self destinations_item:TasksCommandMoveToList]];

    /* ── View menu ── what the task pane shows, then how the window looks. */
    NSMenu *view_menu = add_menu(bar, @"View");
    [view_menu addItem:command(TasksCommandToggleCompleted)];
    [view_menu addItem:command(TasksCommandToggleSort)];
    NSMenuItem *columns = [[NSMenuItem alloc] initWithTitle:@"Columns" action:NULL keyEquivalent:@""];
    columns.submenu     = [[NSMenu alloc] initWithTitle:@"Columns"];
    [columns.submenu addItem:command(TasksCommandColumnDone)];
    [columns.submenu addItem:command(TasksCommandColumnStatus)];
    [columns.submenu addItem:command(TasksCommandColumnDue)];
    [columns.submenu addItem:command(TasksCommandColumnCompleted)];
    [view_menu addItem:columns];
    [view_menu addItem:NSMenuItem.separatorItem];
    [view_menu addItem:command(TasksCommandToggleSidebar)];
    [view_menu addItem:command(TasksCommandToggleCompact)];
    [view_menu addItem:command(TasksCommandTogglePane)];
    [view_menu addItem:NSMenuItem.separatorItem];
    [view_menu addItem:standard_item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                                     NSEventModifierFlagControl)];

    /* ── Window menu ── AppKit lists the open windows (the library window,
     * the task editors, Settings) below these and keeps the list current. */
    NSMenu *window_menu = add_menu(bar, @"Window");
    [window_menu addItem:standard_item(@"Minimize", @selector(performMiniaturize:), @"m", 0)];
    [window_menu addItem:standard_item(@"Zoom", @selector(performZoom:), @"", 0)];
    [window_menu addItem:NSMenuItem.separatorItem];
    [window_menu addItem:standard_item(@"Bring All to Front", @selector(arrangeInFront:), @"", 0)];
    NSApp.windowsMenu = window_menu;

    /* ── Help menu ── AppKit adds the menu-search field above the item. */
    NSMenu *help_menu = add_menu(bar, @"Help");
    [help_menu addItem:command(TasksCommandHelp)];
    NSApp.helpMenu = help_menu;

    NSApp.mainMenu = bar;
}

@end
