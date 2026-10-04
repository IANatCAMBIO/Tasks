#import <Cocoa/Cocoa.h>

/*
 * app_delegate — the application's life: launch (config, database, the
 * core's registries, the menu bar, the library window, the workers), the
 * commands that belong to the app rather than a window (About, Settings,
 * Help), and quit (flush, disarm, close the database).
 *
 * The delegate OWNS the TaskApp and the library window controller.
 */
@interface TasksAppDelegate : NSObject <NSApplicationDelegate>

/*
 * build_menu_bar — build the whole menu bar in code and install it as the
 * application's main menu: the App, File, Edit, View, Window and Help
 * menus, holding every command in the command table (commands.h).  Called
 * once at launch; declared here so that tools/ui_check.m can check the
 * menus without launching the app.
 */
- (void)build_menu_bar;

@end
