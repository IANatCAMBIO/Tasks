#import "row_text.h"
#import "look.h"
#import "ui_util.h"

/*
 * row_font — the font of a line.
 *
 * Inputs:
 *   small  — the small size (every line but the title)
 *   bold   — bold weight
 *   italic — italic
 *
 * Output:
 *   the font (never nil; falls back to the upright face if the system
 *   font has no italic).
 */
static NSFont *
row_font(BOOL small, BOOL bold, BOOL italic)
{
    CGFloat size = small ? tasks_look_row_small_font : tasks_look_row_font;
    NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];

    if (italic) {
        NSFontDescriptor *desc = [font.fontDescriptor
            fontDescriptorWithSymbolicTraits:font.fontDescriptor.symbolicTraits |
                                             NSFontDescriptorTraitItalic];
        font = [NSFont fontWithDescriptor:desc size:size] ?: font;
    }
    return font;
}

/*
 * paragraph_style — the paragraph style of every line of a row.
 *
 * Inputs:
 *   layout — wrap or truncate
 *
 * Output:
 *   new paragraph style.
 */
static NSParagraphStyle *
paragraph_style(TasksRowLayout layout)
{
    NSMutableParagraphStyle *style = [[NSMutableParagraphStyle alloc] init];

    style.lineBreakMode    = layout == TasksRowLayoutTruncate ? NSLineBreakByTruncatingTail
                                                              : NSLineBreakByWordWrapping;
    style.paragraphSpacing = tasks_look_row_line_gap;
    return style;
}

/*
 * tasks_row_text — the attributed string for one row.
 * Inputs/output are documented in row_text.h.
 */
NSAttributedString *
tasks_row_text(const task_row_t *row, BOOL emphasized, TasksRowLayout layout)
{
    NSColor *base = emphasized ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor;
    NSParagraphStyle *para = paragraph_style(layout);
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];

    for (size_t i = 0; i < row->line_count; i++) {
        const task_row_line_t *line = &row->lines[i];
        NSFont *font = row_font(line->small, line->bold, line->italic);

        if (i > 0) [text.mutableString appendString:@"\n"];

        /* The glyph prefix: the line's size, never bold, struck or dimmed. */
        NSString *prefix = tasks_string(line->prefix);
        if (prefix.length > 0) {
            [text appendAttributedString:[[NSAttributedString alloc]
                initWithString:prefix
                    attributes:@{ NSFontAttributeName:            row_font(line->small, NO, NO),
                                  NSForegroundColorAttributeName: base,
                                  NSParagraphStyleAttributeName:  para }]];
        }

        NSMutableDictionary<NSAttributedStringKey, id> *attrs = [@{
            NSFontAttributeName:            font,
            NSForegroundColorAttributeName:
                [base colorWithAlphaComponent:line->alpha_pct / 100.0],
            NSParagraphStyleAttributeName:  para,
        } mutableCopy];
        if (line->strike) attrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);

        [text appendAttributedString:[[NSAttributedString alloc]
            initWithString:tasks_string(line->text) attributes:attrs]];
    }

    /* The separating newlines carry the paragraph style of their line. */
    [text addAttribute:NSParagraphStyleAttributeName
                 value:para
                 range:NSMakeRange(0, text.length)];
    return text;
}

/*
 * tasks_row_text_height — the height the text needs at a width.
 * Inputs/output are documented in row_text.h.
 */
CGFloat
tasks_row_text_height(NSAttributedString *text, CGFloat width)
{
    NSRect bounds = [text boundingRectWithSize:NSMakeSize(width, CGFLOAT_MAX)
                                       options:NSStringDrawingUsesLineFragmentOrigin |
                                               NSStringDrawingUsesFontLeading];
    return ceil(NSHeight(bounds));
}
