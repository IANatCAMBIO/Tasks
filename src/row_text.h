#import <Cocoa/Cocoa.h>
#include "task_rows.h"

/*
 * row_text — THE renderer of a task row's tall cell: it turns the core's
 * toolkit-neutral description (task_rows.h) into an attributed string.
 *
 * The task list and the Kanban cards both call this and nothing else to
 * draw a row's text, and both measure with tasks_row_text_height, so a
 * row reads the same in the two panes and a cell is always as tall as the
 * text it was measured for.  Fonts and sizes come from look.h.
 *
 * Each line is ONE line, cut short with an ellipsis when the pane is too
 * narrow for it, never wrapped: a row's height is its line count's, the
 * same at any width.
 *
 * Emphasis is applied exactly as the description says: bold, italic,
 * strike-through, the small size, and ALPHA for dimming — applied to
 * whatever the base text colour is, so dimmed text stays readable on a
 * selected row.
 */

/*
 * tasks_row_text — the attributed string for one row.
 *
 * Inputs:
 *   row        — the row description; not NULL
 *   emphasized — YES when the row is selected in the key window, so the
 *                text is drawn in the selected-text colour; NO uses
 *                labelColor
 *
 * Output:
 *   new attributed string: one paragraph per line of the description,
 *   each truncating at its tail.
 */
NSAttributedString *tasks_row_text(const task_row_t *row, BOOL emphasized);

/*
 * tasks_row_text_height — the height a tasks_row_text string needs, rounded
 * up to whole points: one line per paragraph, whatever the width.
 *
 * Inputs:
 *   text — a string made by tasks_row_text
 *
 * Output:
 *   the height in points.
 */
CGFloat tasks_row_text_height(NSAttributedString *text);
