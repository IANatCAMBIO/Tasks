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
    /* Eight faces, each made once: a row's string is built on every
     * refresh, and the italic face goes through a font descriptor. */
    static NSFont *faces[8];
    unsigned       which = (small ? 4 : 0) | (bold ? 2 : 0) | (italic ? 1 : 0);
    if (faces[which] != nil) return faces[which];

    CGFloat size = shared_look_ui_font;         /* every line the apps' size; a small one is told by its alpha */
    NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];

    if (italic) {
        NSFontDescriptor *desc = [font.fontDescriptor
            fontDescriptorWithSymbolicTraits:font.fontDescriptor.symbolicTraits |
                                             NSFontDescriptorTraitItalic];
        font = [NSFont fontWithDescriptor:desc size:size] ?: font;
    }
    faces[which] = font;
    return font;
}

/*
 * paragraph_style — the paragraph style of every line of a row: one line,
 * cut short with an ellipsis.
 *
 * Output:
 *   the shared style.
 */
static NSParagraphStyle *
paragraph_style(void)
{
    static NSParagraphStyle *style;
    if (style != nil) return style;

    NSMutableParagraphStyle *made = [[NSMutableParagraphStyle alloc] init];
    made.lineBreakMode    = NSLineBreakByTruncatingTail;
    made.paragraphSpacing = tasks_look_row_line_gap;
    style = [made copy];
    return style;
}

/*
 * tasks_row_text — the attributed string for one row.
 * Inputs/output are documented in row_text.h.
 */
NSAttributedString *
tasks_row_text(const task_row_t *row, BOOL emphasized)
{
    NSColor *base = emphasized ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor;
    NSParagraphStyle *para = paragraph_style();
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
            NSForegroundColorAttributeName: shared_ui_color_dimmed(base, line->alpha_pct),
            NSParagraphStyleAttributeName:  para,
        } mutableCopy];
        if (line->strike) attrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);

        [text appendAttributedString:[[NSAttributedString alloc]
            initWithString:tasks_string(line->text) attributes:attrs]];
    }
    return text;
}

/*
 * tasks_row_text_height — the height the text needs.
 * Inputs/output are documented in row_text.h.
 */
CGFloat
tasks_row_text_height(NSAttributedString *text)
{
    NSRect bounds = [text boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)
                                       options:NSStringDrawingUsesLineFragmentOrigin |
                                               NSStringDrawingUsesFontLeading];
    return ceil(NSHeight(bounds));
}
