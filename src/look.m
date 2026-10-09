#import "look.h"

/* How a colour is defined. */
enum {
    color_fixed = 0,                /* the light and dark RGBA below       */
    color_accent                    /* controlAccentColor at light[3] alpha */
};

/* One colour: its light and dark values as { red, green, blue, alpha }. */
typedef struct {
    int     kind;
    CGFloat light[4];
    CGFloat dark[4];
} color_spec_t;

/* Indexed by TasksColor.  Light values are the GTK build's; fills that the
 * GTK CSS expressed as "foreground at N %" are black at that alpha in light
 * and white at it in dark, which is what the theme colour resolved to. */
static const color_spec_t color_specs[TasksColorCount] = {
    [TasksColorRowStripe]         = { color_fixed,  { 0.953, 0.953, 0.953, 1.00 }, { 0.145, 0.145, 0.145, 1.00 } },
    [TasksColorDragRow]           = { color_fixed,  { 0.992, 0.902, 0.541, 1.00 }, { 0.478, 0.384, 0.125, 1.00 } },
    [TasksColorDueOverdue]        = { color_fixed,  { 0.753, 0.110, 0.157, 1.00 }, { 0.965, 0.380, 0.318, 1.00 } },
    [TasksColorDueToday]          = { color_fixed,  { 0.820, 0.604, 0.000, 1.00 }, { 0.973, 0.894, 0.361, 1.00 } },
    [TasksColorDueAhead]          = { color_fixed,  { 0.149, 0.635, 0.412, 1.00 }, { 0.341, 0.890, 0.537, 1.00 } },
    [TasksColorSidebarBackground] = { color_fixed,  { 0.933, 0.925, 0.918, 1.00 }, { 0.137, 0.137, 0.145, 1.00 } },
    [TasksColorSidebarText]       = { color_fixed,  { 0.255, 0.255, 0.255, 1.00 }, { 0.870, 0.870, 0.870, 1.00 } },
    [TasksColorSidebarSelection]  = { color_fixed,  { 0.337, 0.514, 0.878, 1.00 }, { 0.337, 0.514, 0.878, 1.00 } },
    [TasksColorLink]              = { color_fixed,  { 0.110, 0.443, 0.847, 1.00 }, { 0.471, 0.682, 0.929, 1.00 } },
    [TasksColorLaneFill]          = { color_fixed,  { 0.0, 0.0, 0.0, 0.05 },       { 1.0, 1.0, 1.0, 0.05 } },
    [TasksColorLaneBorder]        = { color_fixed,  { 0.0, 0.0, 0.0, 0.20 },       { 1.0, 1.0, 1.0, 0.20 } },
    [TasksColorLaneTarget]        = { color_accent, { 0.0, 0.0, 0.0, 0.22 },       { 0.0, 0.0, 0.0, 0.0 } },
    [TasksColorCardBorder]        = { color_fixed,  { 0.0, 0.0, 0.0, 0.30 },       { 1.0, 1.0, 1.0, 0.30 } },
    [TasksColorCardBorderHover]   = { color_fixed,  { 0.0, 0.0, 0.0, 0.55 },       { 1.0, 1.0, 1.0, 0.55 } },
    [TasksColorCardSelected]      = { color_accent, { 0.0, 0.0, 0.0, 0.16 },       { 0.0, 0.0, 0.0, 0.0 } },
    [TasksColorCardShadow]        = { color_fixed,  { 0.0, 0.0, 0.0, 0.38 },       { 0.0, 0.0, 0.0, 0.60 } },
    [TasksColorBadge]             = { color_fixed,  { 0.184, 0.365, 0.753, 0.95 }, { 0.184, 0.365, 0.753, 0.95 } },
};

/*
 * make_color — build the dynamic NSColor for one spec.
 *
 * Inputs:
 *   spec — the colour's definition (static storage; the provider block
 *          keeps the pointer)
 *
 * Output:
 *   new NSColor that resolves per appearance.
 */
static NSColor *
make_color(const color_spec_t *spec)
{
    if (spec->kind == color_accent) {
        CGFloat alpha = spec->light[3];
        return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
            return [NSColor.controlAccentColor colorWithAlphaComponent:alpha];
        }];
    }
    return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:
            @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        const CGFloat *c = [match isEqualToString:NSAppearanceNameDarkAqua] ? spec->dark
                                                                           : spec->light;
        return [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
    }];
}

/*
 * tasks_color — the shared dynamic colour, built once.
 * Inputs/output are documented in look.h.
 */
NSColor *
tasks_color(TasksColor which)
{
    static NSArray<NSColor *> *colors;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray<NSColor *> *built = [NSMutableArray arrayWithCapacity:TasksColorCount];
        for (NSInteger i = 0; i < TasksColorCount; i++) {
            [built addObject:make_color(&color_specs[i])];
        }
        colors = [built copy];
    });
    return colors[(NSUInteger)which];
}

/*
 * tasks_color_dimmed — a base colour at a strength, shared.
 * Inputs/output are documented in look.h.
 */
NSColor *
tasks_color_dimmed(NSColor *base, int alpha_pct)
{
    if (alpha_pct >= 100) return base;

    static NSMapTable<NSColor *, NSMutableDictionary<NSNumber *, NSColor *> *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMapTable strongToStrongObjectsMapTable]; });

    NSMutableDictionary<NSNumber *, NSColor *> *by_alpha = [cache objectForKey:base];
    if (by_alpha == nil) {
        by_alpha = [NSMutableDictionary dictionary];
        [cache setObject:by_alpha forKey:base];
    }
    NSColor *known = by_alpha[@(alpha_pct)];
    if (known != nil) return known;

    CGFloat strength = alpha_pct / 100.0;
    NSColor *made = [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        __block NSColor *resolved = nil;
        [appearance performAsCurrentDrawingAppearance:^{
            NSColor *now = [base colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            resolved = [now colorWithAlphaComponent:now.alphaComponent * strength];
        }];
        return resolved;
    }];
    by_alpha[@(alpha_pct)] = made;
    return made;
}

/*
 * tasks_due_color — the Due cell's tint.
 * Inputs/output are documented in look.h.
 */
NSColor *
tasks_due_color(task_due_urgency_t urgency)
{
    switch (urgency) {
    case task_due_overdue: return tasks_color(TasksColorDueOverdue);
    case task_due_today:   return tasks_color(TasksColorDueToday);
    case task_due_ahead:   return tasks_color(TasksColorDueAhead);
    case task_due_none:    break;
    }
    return NSColor.labelColor;
}
