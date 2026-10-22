#import "app_delegate.h"
#import "commands.h"
#import "shared_hotkey.h"
#import "library_window.h"
#import "settings_window.h"
#import "task_notify.h"
#import "ui_util.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "backup.h"
#include "core_views.h"
#include "db.h"
#include "gcal_sync.h"
#include "recur.h"
#include "task_config.h"
#include "task_switch.h"
#include "task_paths.h"
#include "task_worker.h"

#include <glib.h>
#include <signal.h>

/* The About panel's credits: the app's motto (carried from the GTK build),
 * then two links. */
static NSString * const about_motto       = @"Gettin' shit done since 2026!";

/* The help page in Contents/Resources (resources/help.html). */
static NSString * const help_page_name = @"help";
static NSString * const help_page_type = @"html";

/* The Info.plist key the Makefile stamps with the build time (UTC, ISO 8601). */
static NSString * const build_date_key = @"TasksBuildDate";

/* The size of the app icon in the Dock tile and the About panel. */
static const CGFloat app_icon_size = 128.0;

/*
 * quick_task_wanted — whether the Quick Task setting is on.
 *
 * Output: the setting.
 */
static BOOL
quick_task_wanted(void)
{
    return task_config_get_bool(TASK_CFG_QUICK_TASK_HOTKEY);
}

/* Quick Task's system-wide shortcut, ⌃⌥⌘T.  The menu item's copy is in commands.m. */
static const shared_hotkey_t quick_task_hotkey = {
    kVK_ANSI_T, controlKey | optionKey | cmdKey, 'TskQ', quick_task_wanted,
};

@interface TasksAppDelegate () <NSUserInterfaceValidations, NSMenuDelegate>
/* The application context: the open database and the event bus.  Owned;
 * NULL before start_core succeeds and after tear_down. */
@property (nonatomic, assign) TaskApp *app;
/* The one library window.  Owned; nil until the core has started. */
@property (nonatomic, strong) TasksLibraryWindow *library;
/* Delivers SIGTERM on the main queue as a normal quit. */
@property (nonatomic, strong) dispatch_source_t sigterm_source;
@end

static const shared_command_handler_t *app_commands(size_t *n);

@implementation TasksAppDelegate

/* ── Launch ──────────────────────────────────────────────────────────── */

/*
 * start_config — bring the config and the non-database registries up.
 * Called at launch before the database is opened.  The database path is
 * resolved here (shared_db_path: $CAMBIO_SHARED_DB, else the pointer file
 * both apps follow, else the per-user default) and returned so the caller
 * can check whether it exists before deciding to show the Welcome screen.
 *
 * Inputs:
 *   out_path — receives the resolved database path (caller must free);
 *              NULL on failure.
 *
 * Output:
 *   nil on success (self.app is set, config is open); otherwise a message
 *   saying what failed.
 */
- (NSString *)
start_config:(char **)out_path
{
    *out_path = NULL;

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
    task_notify_init(app);
    gcal_sync_init(app);
    self.app = app;

    char *why = NULL;
    *out_path = shared_db_path(&why);
    if (*out_path == NULL) {
        NSString *message = [NSString stringWithFormat:
            @"The shared database cannot be located:\n%@", tasks_string(why)];
        free(why);
        return message;
    }
    return nil;
}

/*
 * open_db_at: — open (or create) the database at `path`.  Only the open:
 * the workers are armed and the health reported once the library window
 * exists, so that what they say has somewhere to land.
 *
 * Inputs:
 *   path — filesystem path to the .db file (UTF-8)
 *
 * Output:
 *   nil on success; otherwise a message saying what failed.
 */
- (NSString *)
open_db_at:(const gchar *)path
{
    GError *err   = NULL;
    self.app->db  = task_db_open(path, &err);
    if (self.app->db == NULL) {
        NSString *message = [NSString stringWithFormat:@"%@",
            tasks_string(err != NULL ? err->message : "The database cannot be opened.")];
        g_clear_error(&err);
        return message;
    }
    return nil;
}

/* How the database to open was arrived at. */
typedef NS_ENUM(NSInteger, TasksWelcomeChoice) {
    TasksWelcomeCreate = 0,         /* the default path; a new database is
                                     * created there if it is missing     */
    TasksWelcomeOpen                /* an existing file the user picked    */
};

/*
 * ask_welcome: — first-run welcome, in a tool panel: open an existing
 * database, create a new one at the default path, or quit (also what
 * Escape and the close box do).  Launch carries on from the answer
 * (finish_launch_at:choice:); cancelling the file chooser comes back to
 * the question rather than creating a database the user did not ask for.
 *
 * Inputs:
 *   default_path — the path "Create New Database" uses (its name is shown)
 */
- (void)
ask_welcome:(NSString *)default_path
{
    NSString *message = [NSString stringWithFormat:
        @"No database was found.  You can open an existing database shared "
        @"by Notes and Tasks, or create a new one named \u201C%@\u201D.",
        default_path.lastPathComponent];

    __weak TasksAppDelegate *ws = self;
    [SharedToolPanel show_message:@"Welcome to " @TASKS_APP_NAME message:message
                          symbol:@"tray.full.fill"
                         buttons:@[@"Open Existing Database\u2026", @"Create New Database", @"Quit"]
                     destructive:-1 over:nil
                         handler:^(NSInteger index) {
        TasksAppDelegate *s = ws;
        if (index == 1) {
            [s finish_launch_at:default_path choice:TasksWelcomeCreate];
            return;
        }
        if (index != 0) {
            [NSApp terminate:nil];
            return;
        }
        /* The file chooser is the system's: there is no app-drawn one. */
        shared_ui_pick_path(SharedPickOpenFile, @"Choose a shared Notes and Tasks database to open.",
                            @"Open", @[[UTType typeWithFilenameExtension:@"db"]], nil,
                            ^(NSString *path) {
            if (path) {
                [s finish_launch_at:path choice:TasksWelcomeOpen];
            } else {
                [s ask_welcome:default_path];
            }
        });
    }];
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
 * applicationDidFinishLaunching: — start the core and build the menu bar,
 * then hand over to finish_launch_at:choice: — at once, or, on first run
 * (no shared database found), once the Welcome panel (ask_welcome:) has
 * been answered.
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
    tasks_commands_install();
    [self build_menu_bar];
    [self watch_sigterm];

    char     *default_path = NULL;
    NSString *config_err   = [self start_config:&default_path];
    if (config_err) {
        shared_ui_notice(nil, @"Could not start", config_err, ^{
            [NSApp terminate:nil];
        });
        return;
    }

    /* First run: the shared database does not exist yet, so ask before
     * creating one. */
    NSString *path = tasks_string(default_path);
    BOOL first_run = !g_file_test(default_path, G_FILE_TEST_EXISTS);
    free(default_path);
    if (first_run) {
        [self ask_welcome:path];
    } else {
        [self finish_launch_at:path choice:TasksWelcomeCreate];
    }
}

/*
 * finish_launch_at:choice: — the rest of the launch, once the database to
 * open is known: open it (an existing file picked at the Welcome panel
 * becomes the default for both apps, through the pointer file
 * shared_db_set_default_path), build the library window, then arm the
 * workers and check the database.  The workers are armed AFTER the window
 * exists so that a status message posted while arming has somewhere to
 * land.  A database that cannot be opened is reported and the app quits.
 *
 * Inputs:
 *   open_path — the database file
 *   choice    — how it was arrived at
 */
- (void)
finish_launch_at:(NSString *)open_path choice:(TasksWelcomeChoice)choice
{
    const char *path   = open_path.fileSystemRepresentation;
    NSString   *db_err = [self open_db_at:path];
    /* An existing database chosen at the Welcome panel is the one both
     * apps open from now on — or the Welcome panel would be back next
     * time.  The shared module refuses while $CAMBIO_SHARED_DB fixes the
     * file; the database is then open for this session only. */
    BOOL made_default = NO;
    if (db_err == nil && choice == TasksWelcomeOpen && task_switch_can_be_default(path)) {
        char *why = NULL;
        made_default = shared_db_set_default_path(path, &why) == 0;
        if (!made_default) NSLog(@"[launch] cannot make %s the default: %s", path, why);
        free(why);
    }
    BOOL for_session_only = db_err == nil && choice == TasksWelcomeOpen && !made_default;

    if (db_err) {
        shared_ui_notice(nil, @"Could not open the database", db_err, ^{
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

    /* The workers and the health check come AFTER the window: what they
     * report lands in its status bar and its sheets. */
    task_worker_arm_all(self.app, shared_db_file(self.app->db->shared));
    task_app_watch_start(self.app);
    [self.library report_database_health];
    [self start_hotkey];
    if (for_session_only) {
        task_app_status(self.app, "%s is open for this session only",
                        shared_db_file(self.app->db->shared));
    }

    gint n_tasks = 0, n_tags = 0;
    task_db_totals(self.app->db, &n_tasks, &n_tags);
    NSLog(@"[launch] config %@, database %@: %d tasks with %d tags",
          tasks_string(task_config_path()), tasks_string(shared_db_file(self.app->db->shared)),
          n_tasks, n_tags);
}

/*
 * start_hotkey — arm Quick Task's system-wide shortcut (shared_hotkey.h), which
 * runs Quick Task in the library window from any app.  A shortcut another
 * app already holds is reported in the status bar.
 */
- (void)
start_hotkey
{
    __weak TasksAppDelegate *ws = self;
    BOOL ok = shared_hotkey_start(&quick_task_hotkey, ^{
        [ws.library perform_command:@"quick-task" param:nil];
    });
    if (!ok) {
        task_app_status(self.app, "The Quick Task shortcut is in use by another app");
    }
}

/*
 * applicationDidBecomeActive: — the user has come back to the app, perhaps
 * from Notes: look for outside changes now rather than at the next tick of
 * the watch.
 *
 * Inputs:
 *   note — NSApplicationDidBecomeActiveNotification (unused)
 */
- (void)
applicationDidBecomeActive:(NSNotification *)note
{
    if (self.library != nil) task_app_check_outside(self.app);
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
    shared_hotkey_stop();
    task_app_watch_stop(self.app);
    [self.library shut_down];
    self.library = nil;
    task_worker_disarm_all();
    gcal_sync_shut_down();
    task_app_free(self.app);
    self.app = NULL;
    task_config_close();
    NSLog(@"[quit] database closed");
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * run_app_command: — the action of every command item (commands.h: all of
 * them are app scope).  The app's own commands are carried out here; every
 * other command goes to the library window, whichever window is key.
 *
 * Inputs:
 *   sender — an item made from the command table
 */
- (void)
run_app_command:(id)sender
{
    size_t n = 0;
    const shared_command_handler_t *table = app_commands(&n);
    if (!shared_command_run(table, n, self, sender)) [self.library run_command:sender];
}

/*
 * validateUserInterfaceItem: — enables the command items.  The app's own
 * commands are available whenever the core is up; the rest are the
 * library window's to answer.  Items with other actions (Quit, Close, the
 * Edit menu) are validated by AppKit and never reach here.
 *
 * Inputs:
 *   item — an item whose target is the delegate
 *
 * Output:
 *   YES to enable the item.
 */
- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (self.app == NULL) return NO;
    size_t n = 0;
    BOOL   handled = NO;
    BOOL   enabled = shared_command_validate(app_commands(&n), n, self, item, &handled);
    if (handled) return enabled;
    return self.library != nil && [self.library validateUserInterfaceItem:item];
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
    shared_ui_show_about(about_motto, tasks_app_icon(app_icon_size), build_date_key);
}

/*
 * show_help — Help > Tasks Help: open the bundled help page in the
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

/* Shorthand for the standard items' modifier masks, as in commands.m. */
enum {
    cmd       = NSEventModifierFlagCommand,
    cmd_shift = NSEventModifierFlagCommand | NSEventModifierFlagShift,
    cmd_ctrl  = NSEventModifierFlagCommand | NSEventModifierFlagControl,
    cmd_opt   = NSEventModifierFlagCommand | NSEventModifierFlagOption,
};

/*
 * standard_item — a menu item for one of AppKit's own actions (Quit, Close,
 * Copy, ...), sent down the responder chain.
 *
 * Inputs:
 *   title  — menu item title
 *   action — AppKit's selector
 *   key    — key equivalent character, @"" for none
 *   mods   — the shortcut's modifiers, the full mask (as the command
 *            table's and Notes' make_item)
 *
 * Output:
 *   new NSMenuItem with a nil target.
 */
static NSMenuItem *
standard_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];

    item.keyEquivalentModifierMask = mods;
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
 * destinations_item: — an "Add Tag" item for the main menu: its submenu is
 * left empty here and filled each time it opens (menuNeedsUpdate:), so it
 * always lists the tags there are now.  The submenu carries the
 * command's identifier, which is how menuNeedsUpdate: knows what to fill
 * it with.
 *
 * Inputs:
 *   ident — "add-tag"
 *
 * Output:
 *   new NSMenuItem holding the submenu.
 */
- (NSMenuItem *)
destinations_item:(NSString *)ident
{
    const shared_command_t *command = shared_command_find(ident);
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:command->title
                                                    action:NULL
                                             keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:command->title];

    menu.identifier = ident;
    menu.delegate   = self;
    holder.submenu  = menu;
    return holder;
}

/*
 * menuNeedsUpdate: — the "Add Tag" submenu is about to open: have the library
 * window fill it with the current destinations.
 *
 * Inputs:
 *   menu — a submenu made by destinations_item:
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    if (shared_command_find(menu.identifier)) {
        [self.library fill_menu:menu with_destinations_of:menu.identifier];
    }
}

/*
 * menuHasKeyEquivalent:forEvent:target:action: — AppKit asks whether a
 * "Add Tag" submenu claims a key press.  Its items have no shortcuts, and
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
 * Command items come from the command table and target the delegate
 * (run_app_command:), so they work whichever window is key.  The Edit menu's items use AppKit's
 * standard selectors with a nil target: a focused text field's editor gets
 * them, which is what makes Cut/Copy/Paste/Undo work in every field.
 */
- (void)
build_menu_bar
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *(^command)(NSString *) = ^NSMenuItem *(NSString *ident) {
        return shared_command_menu_item(ident, nil);
    };

    /* ── App menu ── */
    NSMenu *app_menu = add_menu(bar, @TASKS_APP_NAME);
    [app_menu addItem:command(@"about")];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:command(@"settings")];
    [app_menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:NULL keyEquivalent:@""];
    services.submenu     = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu   = services.submenu;
    [app_menu addItem:services];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:standard_item(@"Hide " @TASKS_APP_NAME, @selector(hide:), @"h", cmd)];
    [app_menu addItem:standard_item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                                    cmd_opt)];
    [app_menu addItem:standard_item(@"Show All", @selector(unhideAllApplications:), @"", cmd)];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:standard_item(@"Quit " @TASKS_APP_NAME, @selector(terminate:), @"q", cmd)];

    /* ── File menu ── what is created, then the selected task, the selected
     * tag, and the file all of it is kept in. */
    NSMenu *file_menu = add_menu(bar, @"File");
    [file_menu addItem:command(@"new-task")];
    [file_menu addItem:command(@"quick-task")];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(@"task-info")];
    [file_menu addItem:command(@"delete-task")];
    [file_menu addItem:command(@"clear-completed")];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(@"edit-tag")];
    [file_menu addItem:command(@"add-favorite")];
    [file_menu addItem:command(@"remove-favorite")];
    [file_menu addItem:command(@"clear-tag")];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:command(@"open-database")];
    [file_menu addItem:NSMenuItem.separatorItem];
    [file_menu addItem:standard_item(@"Close Window", @selector(performClose:), @"w", cmd)];

    /* ── Edit menu ── the standard items, then what changes the selected
     * tasks.  Both directions of each flag are offered: the selection may
     * hold tasks in either state. */
    NSMenu *edit_menu = add_menu(bar, @"Edit");
    [edit_menu addItem:standard_item(@"Undo", @selector(undo:), @"z", cmd)];
    [edit_menu addItem:standard_item(@"Redo", @selector(redo:), @"z", cmd_shift)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItem:standard_item(@"Cut", @selector(cut:), @"x", cmd)];
    [edit_menu addItem:standard_item(@"Copy", @selector(copy:), @"c", cmd)];
    [edit_menu addItem:standard_item(@"Paste", @selector(paste:), @"v", cmd)];
    [edit_menu addItem:standard_item(@"Delete", @selector(delete:), @"", cmd)];
    [edit_menu addItem:standard_item(@"Select All", @selector(selectAll:), @"a", cmd)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItem:command(@"find")];
    [edit_menu addItem:NSMenuItem.separatorItem];
    for (NSString *flag in @[@"mark-done", @"set-priority"]) {
        [edit_menu addItem:tasks_command_flag_item(flag, YES, NO)];
        [edit_menu addItem:tasks_command_flag_item(flag, NO, NO)];
    }
    [edit_menu addItem:[self destinations_item:@"add-tag"]];

    /* ── View menu ── what the task pane shows, then how the window looks. */
    NSMenu *view_menu = add_menu(bar, @"View");
    [view_menu addItem:command(@"toggle-completed")];
    NSMenuItem *columns = [[NSMenuItem alloc] initWithTitle:@"Columns" action:NULL keyEquivalent:@""];
    columns.submenu     = [[NSMenu alloc] initWithTitle:@"Columns"];
    [columns.submenu addItem:command(@"column-done")];
    [columns.submenu addItem:command(@"column-tags")];
    [columns.submenu addItem:command(@"column-status")];
    [columns.submenu addItem:command(@"column-due")];
    [columns.submenu addItem:command(@"column-completed")];
    [columns.submenu addItem:command(@"column-created")];
    [view_menu addItem:columns];
    [view_menu addItem:NSMenuItem.separatorItem];
    [view_menu addItem:command(@"toggle-sidebar")];
    [view_menu addItem:command(@"toggle-favorites-bar")];
    [view_menu addItem:command(@"toggle-compact")];
    [view_menu addItem:command(@"toggle-pane")];
    [view_menu addItem:command(@"toggle-day-planner")];
    [view_menu addItem:NSMenuItem.separatorItem];
    [view_menu addItem:standard_item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                                     cmd_ctrl)];

    /* ── Window menu ── AppKit lists the open windows (the library window,
     * the task editors, Settings) below these and keeps the list current. */
    NSMenu *window_menu = add_menu(bar, @"Window");
    [window_menu addItem:standard_item(@"Minimize", @selector(performMiniaturize:), @"m", cmd)];
    [window_menu addItem:standard_item(@"Zoom", @selector(performZoom:), @"", cmd)];
    [window_menu addItem:NSMenuItem.separatorItem];
    [window_menu addItem:standard_item(@"Bring All to Front", @selector(arrangeInFront:), @"", cmd)];
    NSApp.windowsMenu = window_menu;

    /* ── Help menu ── AppKit adds the menu-search field above the item. */
    NSMenu *help_menu = add_menu(bar, @"Help");
    [help_menu addItem:command(@"help")];
    NSApp.helpMenu = help_menu;

    NSApp.mainMenu = bar;
}

@end

/* ── The app's own command handlers ──────────────────────────────────── */

/*
 * run_about — About.
 *
 * Inputs:
 *   target — the delegate
 *   sender — the item (unused)
 */
static void
run_about(id target, id sender)
{
    [(TasksAppDelegate *)target show_about];
}

/*
 * run_settings — Settings…
 *
 * Inputs:
 *   target — the delegate
 *   sender — the item (unused)
 */
static void
run_settings(id target, id sender)
{
    tasks_settings_window_open(((TasksAppDelegate *)target).app);
}

/*
 * run_help — the bundled help page.
 *
 * Inputs:
 *   target — the delegate
 *   sender — the item (unused)
 */
static void
run_help(id target, id sender)
{
    [(TasksAppDelegate *)target show_help];
}

/*
 * app_commands — the delegate's handler table.
 *
 * Inputs:
 *   n — receives how many
 *
 * Output: the table (static).
 */
static const shared_command_handler_t *
app_commands(size_t *n)
{
    static const shared_command_handler_t table[] = {
        { @"about",    run_about,    NULL },
        { @"settings", run_settings, NULL },
        { @"help",     run_help,     NULL },
    };
    *n = sizeof table / sizeof table[0];
    return table;
}
