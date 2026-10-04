#import "settings_window.h"
#import "look.h"
#import "ui_util.h"

#include "task_config.h"

/*
 * STUB of the Settings window (work package "settings": see
 * porting/INTERFACES.md).  It honours the frozen interface and shows the
 * OWNERSHIP of a one-off window — a single module-level reference, dropped
 * when the window closes — with the least content: where the data lives.
 * No settings can be changed yet.  The window's owner replaces this file.
 */

@interface TasksSettingsWindow : NSWindowController <NSWindowDelegate>
@end

/* The one Settings window while it is open; nil otherwise.  This reference
 * is what owns the controller. */
static TasksSettingsWindow *settings_window;

@implementation TasksSettingsWindow

/*
 * init_with_app: — build the Settings window.
 *
 * Inputs:
 *   app — the application context (its open database is described)
 *
 * Output:
 *   the controller, owned by the module-level reference.
 */
- (instancetype)
init_with_app:(TaskApp *)app
{
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, tasks_look_settings_w, 140)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:YES];

    self = [super initWithWindow:window];
    if (!self) return nil;

    window.title    = @"Settings";
    window.delegate = self;

    NSTextField *summary = [NSTextField wrappingLabelWithString:
        [NSString stringWithFormat:@"Settings (stub)\n\nDatabase: %@\nSettings file: %@",
            tasks_string(app->db->path), tasks_string(task_config_path())]];
    summary.selectable = YES;
    summary.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:summary];
    [NSLayoutConstraint activateConstraints:@[
        [summary.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor
                                              constant:tasks_look_form_margin],
        [summary.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor
                                               constant:-tasks_look_form_margin],
        [summary.topAnchor constraintEqualToAnchor:window.contentView.topAnchor
                                          constant:tasks_look_form_margin],
    ]];
    [window center];
    return self;
}

/*
 * windowWillClose: — the window is closing: drop the module's reference,
 * which releases this controller.  The last thing done here, because
 * nothing else keeps `self` alive afterwards.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    settings_window = nil;
}

@end

/*
 * tasks_settings_window_open — show or raise the Settings window.
 * Inputs are documented in settings_window.h.
 */
void
tasks_settings_window_open(TaskApp *app)
{
    if (!settings_window) settings_window = [[TasksSettingsWindow alloc] init_with_app:app];
    [settings_window showWindow:nil];
}
