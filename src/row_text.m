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
 * dimmed — a text colour at a line's strength, STILL DYNAMIC.
 *
 * -colorWithAlphaComponent: on a system colour resolves it on the spot,
 * for whichever appearance is current at that moment, and the result no
 * longer follows light and dark: rows built in a light window stayed black
 * when the window went dark.  So the dimmed colour is a dynamic one that
 * resolves the base each time it is drawn.  The strength scales the base's
 * own alpha (labelColor is not fully opaque), so a dimmed line is never
 * stronger than an undimmed one.
 *
 * Inputs:
 *   base      — the row's text colour (a system colour)
 *   alpha_pct — the line's strength, 0-100
 *
 * Output:
 *   the base itself at full strength, else a new dynamic colour.
 */
static NSColor *
dimmed(NSColor *base, int alpha_pct)
{
    if (alpha_pct >= 100) return base;

    CGFloat strength = alpha_pct / 100.0;
    return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        __block NSColor *resolved = nil;
        [appearance performAsCurrentDrawingAppearance:^{
            NSColor *now = [base colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            resolved = [now colorWithAlphaComponent:now.alphaComponent * strength];
        }];
        return resolved;
    }];
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

        /* A character appended to the plain string takes the attributes of
         * the one before it, the paragraph style among them. */
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
            NSForegroundColorAttributeName: dimmed(base, line->alpha_pct),
            NSParagraphStyleAttributeName:  para,
        } mutableCopy];
        if (line->strike) attrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);

        [text appendAttributedString:[[NSAttributedString alloc]
            initWithString:tasks_string(line->text) attributes:attrs]];
    }
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
