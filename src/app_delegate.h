#import <Cocoa/Cocoa.h>

/*
 * app_delegate — the application's life: launch (config, database, the
 * core's registries, the menu bar, the library window, the workers), the
 * commands that belong to the app rather than a window (About, Settings),
 * and quit (flush, disarm, close the database).
 *
 * The delegate OWNS the TaskApp and the library window controller.
 */
@interface TasksAppDelegate : NSObject <NSApplicationDelegate>
@end
