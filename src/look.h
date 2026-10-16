#import <Cocoa/Cocoa.h>
#include "task_dates.h"

/*
 * look — the app's look in one place: sizes, spacing, timings, and the
 * colours that are not plain system colours.
 *
 * Numbers come from the GTK build's inline CSS (porting/gtk_reference) and
 * from the Files look-and-feel spec (porting/FILES_PLAYBOOK.md §6).  Every
 * UI module takes its metrics and colours from here, so a value exists
 * once and the panes cannot drift apart.
 *
 * COLOUR RULES
 *  - Use semantic system colours wherever one fits: labelColor,
 *    secondaryLabelColor, separatorColor, windowBackgroundColor,
 *    textBackgroundColor, controlAccentColor.  They follow light/dark and
 *    the user's accent on their own.
 *  - Everything else is a TasksColor below, each with a light and a dark
 *    value (the GTK build had light values only; the dark ones were chosen
 *    for this port).  tasks_color returns a dynamic NSColor, so no view
 *    needs appearance-change code.
 *  - Dim text with ALPHA (see row_text.h), never a fixed grey.
 */

/* ── Library window ─────────────────────────────────────────────────── */

static const CGFloat tasks_look_window_min_h     = 320.0;  /* content, full mode;
                                                             * the least width is
                                                             * the task pane plus
                                                             * the day planner when
                                                             * shown (below)      */
static const CGFloat tasks_look_pane_min_w       = 320.0;  /* the task pane; so
                                                             * also the content's
                                                             * least width in
                                                             * Compact mode, which
                                                             * shows nothing else */
static const CGFloat tasks_look_compact_min_h    = 240.0;  /* content, Compact    */
static const CGFloat tasks_look_toolbar_icon     = 16.0;   /* symbol point size of
                                                             * Compact mode's
                                                             * floating buttons (the
                                                             * toolbar sizes its own) */
static const CGFloat tasks_look_toolbar_search_w = 220.0;  /* toolbar search field */
static const CGFloat tasks_look_status_h         = 20.0;   /* status bar; a 1 pt
                                                             * separator above it */
static const CGFloat tasks_look_status_font      = 11.0;   /* secondaryLabelColor;
                                                             * the Files and Notes
                                                             * apps' status size  */
static const CGFloat tasks_look_status_inset     = 8.0;    /* left and right      */
static const NSTimeInterval tasks_look_status_hold = 3.0;  /* a message stays ... */
static const NSTimeInterval tasks_look_status_fade = 1.0;  /* ... then fades out  */
static const CGFloat tasks_look_float_margin     = 20.0;   /* Compact mode's
                                                             * floating controls,
                                                             * from right/bottom  */
static const CGFloat tasks_look_float_radius     = 8.0;    /* their plate's corner */
static const CGFloat tasks_look_float_button     = 32.0;   /* each button's edge  */
static const CGFloat tasks_look_float_pad        = 2.0;    /* plate edge to button,
                                                             * and between buttons */

/* ── Sidebar and Favorites bar ──────────────────────────────────────── */

static const CGFloat tasks_look_sidebar_w        = 180.0;  /* the tag sidebar, until
                                                             * it is dragged: the
                                                             * Files and Notes apps'
                                                             * initial width      */
static const CGFloat tasks_look_sidebar_min_w    = 160.0;  /* the narrowest drag  */
static const CGFloat tasks_look_sidebar_row_h    = 26.0;   /* a sidebar row       */
static const CGFloat tasks_look_sidebar_text_pad = 4.0;    /* a row's text, from
                                                             * the left           */
static const CGFloat tasks_look_sidebar_count_pad = 6.0;   /* its count, from the
                                                             * right              */
static const CGFloat tasks_look_favorites_bar_h  = 28.0;   /* the button strip; a
                                                             * 1 pt rule under it */
static const CGFloat tasks_look_favorite_btn_h   = 24.0;   /* a favorite's button */
static const CGFloat tasks_look_favorite_btn_min_w = 60.0;
static const CGFloat tasks_look_favorite_gap     = 4.0;    /* between buttons     */

/* ── Task rows (the list and the cards share row_text.h) ────────────── */

static const CGFloat tasks_look_row_font         = 12.0;   /* the title line      */
static const CGFloat tasks_look_row_small_font   = 11.0;   /* every other line    */
static const CGFloat tasks_look_row_line_gap     = 1.0;    /* between lines       */
static const CGFloat tasks_look_row_pad_v        = 8.0;    /* above and below the
                                                             * text in a list row */
static const CGFloat tasks_look_row_pad_h        = 4.0;
static const CGFloat tasks_look_header_inset     = 4.0;    /* extra leading inset
                                                             * in column headers   */
static const CGFloat tasks_look_check_col_w      = 28.0;   /* the done checkbox   */
static const CGFloat tasks_look_drag_rule        = 2.0;    /* row drop marker     */

/* ── Kanban board ───────────────────────────────────────────────────── */

static const CGFloat tasks_look_lane_pad         = 6.0;    /* lane border to card */
static const CGFloat tasks_look_lane_gap         = 6.0;    /* between lanes/cards */
static const CGFloat tasks_look_card_pad         = 8.0;    /* card border to text */
static const CGFloat tasks_look_card_grip_pad    = 5.0;    /* round the grip      */
static const CGFloat tasks_look_card_mark_h      = 3.0;    /* insertion bar       */
static const CGFloat tasks_look_card_ghost_alpha = 0.65;   /* the dragged copy    */
static const CGFloat tasks_look_card_src_alpha   = 0.40;   /* the card left behind */
static const CGFloat tasks_look_card_shadow_blur = 4.0;    /* offset (2, -2),
                                                             * TasksColorCardShadow */
static const CGFloat tasks_look_badge_radius     = 8.0;    /* multi-drag count    */
static const NSInteger tasks_look_done_cap       = 10;     /* Done lane shows this
                                                             * many until expanded */

/* ── Editor, Settings and Reminders windows ─────────────────────────── */

static const CGFloat tasks_look_ui_font          = 11.0;   /* the app's own small
                                                             * text, as Notes' every
                                                             * label: the Favorites
                                                             * bar, the reminders
                                                             * window, the hero    */
static const CGFloat tasks_look_screen_margin    = 12.0;   /* a window placed in a
                                                             * screen's lower-right
                                                             * corner (reminders,
                                                             * Quick Task), from the
                                                             * work area's edges;
                                                             * Notes places its
                                                             * editors the same   */

static const CGFloat tasks_look_editor_w         = 490.0;  /* height fits content */
static const NSInteger tasks_look_editor_notes_lines = 8;
static const CGFloat tasks_look_settings_w       = 470.0;
static const CGFloat tasks_look_settings_max_h   = 600.0;  /* then it scrolls     */
static const CGFloat tasks_look_form_margin      = 16.0;   /* window edge to form */
static const CGFloat tasks_look_form_spacing     = 6.0;    /* between form rows   */
static const CGFloat tasks_look_section_gap      = 18.0;   /* between sections    */
static const CGFloat tasks_look_tool_emoji_w     = 44.0;   /* Edit Tag's emoji slot */
static const CGFloat tasks_look_reminders_w      = 389.0;  /* the reminders window */
static const CGFloat tasks_look_reminders_h      = 186.0;
static const CGFloat tasks_look_reminders_min_w  = 243.0;
static const CGFloat tasks_look_reminders_min_h  = 154.0;
static const CGFloat tasks_look_reminders_row_h  = 27.0;   /* one reminder        */
static const CGFloat tasks_look_reminders_icon   = 14.0;   /* its symbol          */

/* ── Tool panels and the blue hero strip (tool_panel.h) ─────────────── */

static const CGFloat tasks_look_hero_h           = 43.0;   /* the strip           */
static const CGFloat tasks_look_hero_pad         = 16.0;   /* left and right inset */
static const CGFloat tasks_look_hero_gap         = 3.0;    /* title to subtitle   */
static const CGFloat tasks_look_hero_icon        = 22.0;   /* its symbol          */
static const CGFloat tasks_look_hero_icon_gap    = 10.0;   /* symbol to the text  */
static const CGFloat tasks_look_tool_min_w       = 340.0;
static const CGFloat tasks_look_tool_pad         = 16.0;   /* around the message  */
static const CGFloat tasks_look_tool_bar_h       = 40.0;   /* the button bar      */
static const CGFloat tasks_look_tool_btn_h       = 25.0;
static const CGFloat tasks_look_tool_btn_w       = 83.0;   /* narrowest button    */
static const CGFloat tasks_look_tool_btn_gap     = 8.0;
static const CGFloat tasks_look_tool_btn_margin  = 12.0;   /* to the window edge  */
static const CGFloat tasks_look_tool_btn_pad     = 24.0;   /* a long title to its
                                                             * button's edges     */
static const CGFloat tasks_look_tool_msg_w       = 388.0;  /* wrapped message     */
static const CGFloat tasks_look_tool_msg_max_h   = 240.0;  /* taller ones scroll  */

/* ── Day planner ────────────────────────────────────────────────────── */

static const CGFloat    tasks_look_day_planner_w           = 320.0;  /* default panel width      */
static const CGFloat    tasks_look_day_planner_min_w       = 300.0;  /* minimum panel width      */
static const CGFloat    tasks_look_day_planner_hour_h      = 60.0;   /* points per hour          */
static const NSInteger  tasks_look_day_planner_start_h     = 6;      /* business hours from 6 AM */
static const NSInteger  tasks_look_day_planner_end_h       = 21;     /* ... to 9 PM; widened to fit the day's blocks */
static const CGFloat    tasks_look_day_planner_label_w     = 44.0;   /* hour label column        */
static const CGFloat    tasks_look_day_planner_pad_v       = 16.0;   /* canvas top/bottom pad; clears the first hour label */
static const CGFloat    tasks_look_day_planner_resize_h    = 8.0;    /* edge hit-zone for resize */
static const CGFloat    tasks_look_day_planner_block_inset = 2.0;    /* block l/r inset          */
static const CGFloat    tasks_look_day_planner_time_w      = 2.0;    /* current-time line height */

/* ── Colours ────────────────────────────────────────────────────────── */

typedef NS_ENUM(NSInteger, TasksColor) {
    TasksColorRowStripe = 0,        /* even list rows (not over selection) */
    TasksColorHeaderFill,           /* the list's column headers           */
    TasksColorDragRow,              /* the row held during a manual drag   */
    TasksColorDueOverdue,           /* Due cell text: before today         */
    TasksColorDueToday,             /* ... today                           */
    TasksColorDueAhead,             /* ... after today                     */
    TasksColorLink,                 /* "Advanced", "Show all N completed"  */
    TasksColorLaneFill,             /* a Kanban lane's tint                */
    TasksColorLaneBorder,
    TasksColorLaneTarget,           /* the lane a drag would land in       */
    TasksColorCardBorder,
    TasksColorCardBorderHover,
    TasksColorCardSelected,         /* a selected card's fill; its border
                                     * is controlAccentColor               */
    TasksColorCardShadow,
    TasksColorBadge,                /* count on a multi-card drag ghost    */
    TasksColorHero,                 /* the hero strip's fill (white text)  */
    TasksColorHeroSubtitle,         /* the hero's second line              */
    TasksColorHeroProblem,          /* ... when it reports a problem       */
    TasksColorCount
};

/*
 * tasks_color — one of the app's own colours, adapting to light and dark.
 * A card's fill is textBackgroundColor and the insertion marker is
 * controlAccentColor: those are system colours and are not listed here.
 *
 * Inputs:
 *   which — the colour
 *
 * Output:
 *   shared dynamic NSColor (never nil).  Main thread.
 */
NSColor *tasks_color(TasksColor which);

/*
 * tasks_color_dimmed — a system colour at a fraction of its strength, STILL
 * DYNAMIC.  -colorWithAlphaComponent: on a system colour resolves it on the
 * spot, for whichever appearance is current, and the result no longer
 * follows light and dark; this returns a dynamic colour that resolves the
 * base each time it is drawn, scaling the base's own alpha.  One colour per
 * base and strength, kept: a dynamic colour made of a block is equal to
 * nothing but itself, so callers that compare attributed strings (the
 * Kanban card's "unchanged, do not redraw") need the same object back.
 *
 * Inputs:
 *   base      — a system colour (labelColor, controlAccentColor, ...)
 *   alpha_pct — the strength, 0-100; 100 returns the base itself
 *
 * Output:
 *   shared dynamic NSColor.  Main thread.
 */
NSColor *tasks_color_dimmed(NSColor *base, int alpha_pct);

/*
 * tasks_due_color — the Due cell's text colour for a due date.
 *
 * Inputs:
 *   urgency — task_due_urgency() of the due moment
 *
 * Output:
 *   the tint, or labelColor for task_due_none.
 */
NSColor *tasks_due_color(task_due_urgency_t urgency);
