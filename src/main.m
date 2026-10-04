#import <Cocoa/Cocoa.h>
#import "app_delegate.h"

/*
 * main — application entry point.  There is no nib: the delegate builds the
 * menu bar and the windows in code.
 *
 * Inputs:
 *   argc — argument count (unused; the app takes no arguments)
 *   argv — argument vector (unused)
 *
 * Output: never returns normally; the app exits through NSApp terminate:.
 */
int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app         = [NSApplication sharedApplication];
        TasksAppDelegate *delegate = [[TasksAppDelegate alloc] init];
        app.delegate               = delegate;   /* weak: `delegate` keeps it alive */
        [app run];
    }
    return 0;
}
