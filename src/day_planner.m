#import "day_planner.h"
#import "editor_window.h"
#import "kanban.h"
#import "look.h"
#import "task_list.h"
#import "ui_util.h"

#include "db.h"
#include "gcal_store.h"
#include "gcal_sync.h"
#include "task_app.h"
#include "task_config.h"

/* Each further task of one drop starts this much after the one before. */
static const int drop_stagger_minutes = 15;

@interface TasksPlannerHost ()
@property (nonatomic, weak) TasksLibraryWindow *library;
@end

@implementation TasksPlannerHost

/*
 * init_with_library: — build the panel; it takes rows of the task list and
 * cards of the board.
 * Inputs/output are documented in day_planner.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super init];
    if (!self) return nil;
    _library         = library;
    _view            = [[SharedDayPlanner alloc] init_with_delegate:self];
    _view.drop_types = @[tasks_row_drag_type, tasks_card_drag_type];
    return self;
}

/*
 * refresh_schedule_views — a session was written to the database: the task
 * pane (the list's Schedule column, the board's ⏰) and every open editor's
 * Scheduling section show it too.  The planner's own blocks are the caller's.
 */
- (void)
refresh_schedule_views
{
    [self.library refresh_tasks];
    tasks_editor_refresh_schedules();
}

/*
 * day_planner:items_from:to: — the day's scheduled tasks, and its Google
 * Calendar events (the sync's cache, gcal_store.h) while the sync is
 * switched on.  Signing out empties the cache, so the switch is the only
 * thing to ask.
 *
 * Inputs:
 *   planner — the panel (unused)
 *   lo, hi  — the day's local midnight and the next
 *
 * Output:
 *   the items.
 */
- (NSArray<SharedDayItem *> *)
day_planner:(SharedDayPlanner *)planner items_from:(int64_t)lo to:(int64_t)hi
{
    NSMutableArray<SharedDayItem *> *items = [NSMutableArray array];
    TaskDatabase *db    = self.library.app->db;
    GPtrArray    *tasks = task_db_tasks_scheduled_for_day(db, lo, hi);
    for (guint i = 0; i < tasks->len; i++) {
        Task          *t    = g_ptr_array_index(tasks, i);
        SharedDayItem *item = [[SharedDayItem alloc] init];
        item.task_id    = t->id;
        item.title      = tasks_title_string(t, @"Untitled");
        item.start_unix = t->scheduled_start;
        item.duration   = t->scheduled_duration > 0 ? t->scheduled_duration
                                                    : SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT;
        item.done       = t->status == TASK_STATUS_DONE;
        [items addObject:item];
    }
    task_ptr_array_free_tasks(tasks);

    if (task_config_get_bool(TASK_CFG_GCAL_ENABLED)) {
        GPtrArray *events = gcal_store_events_for_day(db, lo, hi);
        for (guint i = 0; i < events->len; i++) {
            const gcal_event_t *e    = g_ptr_array_index(events, i);
            SharedDayItem      *item = [[SharedDayItem alloc] init];
            item.calendar_id = tasks_string(e->calendar_id);
            item.event_id    = tasks_string(e->event_id);
            item.title       = tasks_string(e->title);
            item.start_unix  = e->start;
            item.duration    = (int)((e->end - e->start) / 60);
            item.all_day     = e->all_day;
            item.color       = tasks_string(e->color);
            [items addObject:item];
        }
        g_ptr_array_free(events, TRUE);
    }
    return items;
}

/*
 * day_planner:moved_task:start:duration:stretched: — write a moved or
 * stretched session, show it in the other views, and let the calendar
 * sync push it.
 *
 * Inputs:
 *   planner   — the panel (unused)
 *   task_id   — the task
 *   start     — its new start
 *   duration  — its new length, minutes
 *   stretched — YES for a stretch (the failure message names the length)
 */
- (void)
day_planner:(SharedDayPlanner *)planner moved_task:(int64_t)task_id start:(int64_t)start
   duration:(int)duration stretched:(BOOL)stretched
{
    TaskApp *app = self.library.app;
    if (!task_db_task_set_scheduled(app->db, task_id, start, duration))
        task_app_write_failed(app, stretched ? "The new length" : "The new time");
    [self refresh_schedule_views];
    gcal_sync_kick();
}

/*
 * day_planner:unschedule_tasks: — clear the sessions of the selected
 * blocks and reload the views.
 *
 * Inputs:
 *   planner  — the panel
 *   task_ids — the tasks
 */
- (void)
day_planner:(SharedDayPlanner *)planner unschedule_tasks:(NSSet<NSNumber *> *)task_ids
{
    TaskApp *app    = self.library.app;
    BOOL     failed = NO;
    for (NSNumber *tid in task_ids)
        failed = !task_db_task_set_scheduled(app->db, tid.longLongValue, 0, 0) || failed;
    if (failed) task_app_write_failed(app, "Removing from the schedule");
    [planner reload];
    [self refresh_schedule_views];
    gcal_sync_kick();
}

/*
 * day_planner:drop:start: — schedule each dropped task at the target time,
 * a quarter hour apart.  A card dragged from the Kanban board that is
 * still New also moves to In Progress (a list row keeps its status), and
 * the whole window is then refreshed so the card changes lane — on the
 * next run-loop pass, because this runs inside the drag's own handler.
 *
 * Inputs:
 *   planner — the panel
 *   info    — the dragging session; its pasteboard holds the task ids
 *   start   — the time under the drop
 *
 * Output:
 *   YES when there was a task to schedule.
 */
- (BOOL)
day_planner:(SharedDayPlanner *)planner drop:(id<NSDraggingInfo>)info start:(int64_t)start
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    NSPasteboard *pb = info.draggingPasteboard;

    /* Kanban puts a comma-separated list in one item */
    NSString *kanban_str = [pb stringForType:tasks_card_drag_type];
    NSUInteger n_cards   = 0;
    if (kanban_str.length > 0) {
        for (NSString *part in [kanban_str componentsSeparatedByString:@","]) {
            int64_t tid = part.longLongValue;
            if (tid > 0) [ids addObject:@(tid)];
        }
        n_cards = ids.count;
    }

    /* Task list puts one item per row */
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        NSString *s = [item stringForType:tasks_row_drag_type];
        if (s.length > 0) {
            int64_t tid = s.longLongValue;
            if (tid > 0 && ![ids containsObject:@(tid)])
                [ids addObject:@(tid)];
        }
    }

    if (ids.count == 0) return NO;

    TasksLibraryWindow *library    = self.library;
    TaskApp            *app        = library.app;
    NSInteger           offset_min = 0;
    BOOL                promoted   = NO;
    BOOL                failed     = NO;
    for (NSUInteger i = 0; i < ids.count; i++) {
        int64_t tid = ids[i].longLongValue;
        failed = !task_db_task_set_scheduled(app->db, tid, start + offset_min * 60,
                                             SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT) ||
                 failed;
        offset_min += drop_stagger_minutes;
        if (i >= n_cards) continue;

        Task *task = task_db_task_get(app->db, tid);
        if (task != NULL && task->status == TASK_STATUS_NEW) {
            failed   = !task_db_task_set_status(app->db, tid, TASK_STATUS_IN_PROGRESS) || failed;
            promoted = YES;
        }
        task_free(task);
    }
    if (failed) {
        task_app_write_failed(app, "The schedule");   /* refreshes everything */
    } else if (promoted) {
        __weak TasksLibraryWindow *weak_library = library;
        dispatch_async(dispatch_get_main_queue(), ^{ [weak_library full_refresh]; });
    } else {
        [planner reload];
        [self refresh_schedule_views];
    }
    gcal_sync_kick();
    return YES;
}

/*
 * day_planner:open_task: — a double-click on a task's block: its editor.
 *
 * Inputs:
 *   planner — the panel (unused)
 *   task_id — the task
 */
- (void)
day_planner:(SharedDayPlanner *)planner open_task:(int64_t)task_id
{
    tasks_editor_open(self.library.app, task_id);
}

@end
