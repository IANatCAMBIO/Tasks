#import "ui_util.h"
#import "look.h"

#include "task_config.h"

/* The bundled artwork that is the app's icon: its file name, from the
 * Makefile (APP_ICON). */
static NSString * const app_icon_name = @TASKS_APP_ICON;

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

/* ── Images ─────────────────────────────────────────────────────────── */

/*
 * tasks_app_icon — the app's own icon at a size.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
tasks_app_icon(CGFloat size)
{
    return shared_ui_app_icon(app_icon_name, size);
}



