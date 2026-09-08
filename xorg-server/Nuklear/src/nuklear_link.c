#include "nuklear.h"
#include "nuklear_internal.h"

/* ===============================================================
 *
 *                              LINK
 *
 * ===============================================================*/
NK_INTERN void
nk_link_text_bounds(struct nk_rect bounds, const char *string, int len,
    struct nk_vec2 padding, nk_flags align, const struct nk_user_font *font,
    struct nk_rect *out_label, float *out_glyph_width)
{
    struct nk_rect label;
    float glyph_width;
    float text_width;
    nk_flags a = align;

    NK_ASSERT(out_label);
    NK_ASSERT(out_glyph_width);
    NK_ASSERT(font);
    if (!out_label || !out_glyph_width || !font) return;

    bounds.h = NK_MAX(bounds.h, 2 * padding.y);
    glyph_width = font->width(font->userdata, font->height, string, len);
    text_width = glyph_width + (2.0f * padding.x);

    if (!(a & (NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_RIGHT)))
        a |= NK_TEXT_ALIGN_LEFT;
    if (!(a & (NK_TEXT_ALIGN_TOP | NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_BOTTOM)))
        a |= NK_TEXT_ALIGN_TOP;

    if (a & NK_TEXT_ALIGN_LEFT) {
        label.x = bounds.x + padding.x;
        label.w = NK_MAX(0, bounds.w - 2 * padding.x);
    } else if (a & NK_TEXT_ALIGN_CENTERED) {
        label.w = NK_MAX(1, 2 * padding.x + (float)text_width);
        label.x = (bounds.x + padding.x + ((bounds.w - 2 * padding.x) - label.w) / 2);
        label.x = NK_MAX(bounds.x + padding.x, label.x);
        label.w = NK_MIN(bounds.x + bounds.w, label.x + label.w);
        if (label.w >= label.x) label.w -= label.x;
    } else {
        label.x = NK_MAX(bounds.x + padding.x, (bounds.x + bounds.w) - (2 * padding.x + (float)text_width));
        label.w = (float)text_width + 2 * padding.x;
    }

    if (a & NK_TEXT_ALIGN_TOP) {
        label.y = bounds.y + padding.y;
        label.h = NK_MIN(font->height, bounds.h - 2 * padding.y);
    } else if (a & NK_TEXT_ALIGN_MIDDLE) {
        label.y = bounds.y + bounds.h/2.0f - (float)font->height/2.0f;
        label.h = NK_MAX(bounds.h/2.0f, bounds.h - (bounds.h/2.0f + font->height/2.0f));
    } else {
        label.y = bounds.y + bounds.h - font->height;
        label.h = font->height;
    }

    *out_label = label;
    *out_glyph_width = glyph_width;
}
NK_LIB void
nk_draw_link(struct nk_command_buffer *out, const struct nk_rect *bounds,
    nk_flags state, const struct nk_style_link *style, const char *str, int len,
    nk_flags align, const struct nk_user_font *font, struct nk_color background)
{
    struct nk_text text;
    struct nk_rect label;
    struct nk_color color;
    float glyph_width;
    nk_bool draw_underline = nk_false;

    NK_ASSERT(out);
    NK_ASSERT(bounds);
    NK_ASSERT(style);
    NK_ASSERT(font);
    if (!out || !bounds || !style || !font || !str)
        return;

    if (state & NK_WIDGET_STATE_HOVER)
        color = style->text_hover;
    else if (state & NK_WIDGET_STATE_ACTIVED)
        color = style->text_active;
    else color = style->text_normal;
    color = nk_rgb_factor(color, style->color_factor);

    text.padding = style->padding;
    text.background = background;
    text.text = color;
    nk_widget_text(out, *bounds, str, len, &text, align, font);

    if (style->underline == NK_LINK_UNDERLINE_ALWAYS)
        draw_underline = nk_true;
    else if (style->underline == NK_LINK_UNDERLINE_HOVER &&
        (state & (NK_WIDGET_STATE_HOVER|NK_WIDGET_STATE_ACTIVED)))
        draw_underline = nk_true;

    if (!draw_underline || style->underline_thickness <= 0)
        return;

    nk_link_text_bounds(*bounds, str, len, style->padding, align, font, &label, &glyph_width);
    {
        float x0 = label.x;
        float x1 = label.x + NK_MIN(glyph_width, label.w);
        float y = label.y + font->height - style->underline_thickness;
        if (x1 > x0)
            nk_stroke_line(out, x0, y, x1, y, style->underline_thickness, color);
    }
}
NK_LIB nk_bool
nk_do_link(nk_flags *state, struct nk_command_buffer *out,
    struct nk_rect bounds, const char *str, int len, nk_flags align,
    const struct nk_style_link *style, const struct nk_input *in,
    const struct nk_user_font *font, struct nk_color background)
{
    struct nk_rect label;
    struct nk_rect touch;
    float glyph_width;
    nk_bool ret;

    NK_ASSERT(state);
    NK_ASSERT(style);
    NK_ASSERT(out);
    NK_ASSERT(str);
    NK_ASSERT(font);
    if (!out || !style || !font || !str || !state)
        return nk_false;

    nk_link_text_bounds(bounds, str, len, style->padding, align, font, &label, &glyph_width);
    touch.x = label.x - style->touch_padding.x;
    touch.y = label.y - style->touch_padding.y;
    touch.w = NK_MIN(glyph_width, label.w) + 2 * style->touch_padding.x;
    touch.h = font->height + 2 * style->touch_padding.y;
    if (touch.x < bounds.x) {
        touch.w -= (bounds.x - touch.x);
        touch.x = bounds.x;
    }
    if (touch.y < bounds.y) {
        touch.h -= (bounds.y - touch.y);
        touch.y = bounds.y;
    }
    if (touch.x + touch.w > bounds.x + bounds.w)
        touch.w = NK_MAX(0, bounds.x + bounds.w - touch.x);
    if (touch.y + touch.h > bounds.y + bounds.h)
        touch.h = NK_MAX(0, bounds.y + bounds.h - touch.y);

    ret = nk_button_behavior(state, touch, in, NK_BUTTON_DEFAULT);
    nk_draw_link(out, &bounds, *state, style, str, len, align, font, background);
    return ret;
}
NK_API nk_bool
nk_link_text_styled(struct nk_context *ctx, const struct nk_style_link *style,
    const char *title, int len, nk_flags align)
{
    struct nk_window *win;
    struct nk_panel *layout;
    const struct nk_input *in;
    struct nk_rect bounds;
    enum nk_widget_layout_states state;

    NK_ASSERT(ctx);
    NK_ASSERT(style);
    NK_ASSERT(ctx->current);
    NK_ASSERT(ctx->current->layout);
    if (!style || !ctx || !ctx->current || !ctx->current->layout) return 0;

    win = ctx->current;
    layout = win->layout;
    state = nk_widget(&bounds, ctx);

    if (!state) return 0;
    in = (state == NK_WIDGET_ROM || state == NK_WIDGET_DISABLED || layout->flags & NK_WINDOW_ROM) ? 0 : &ctx->input;
    return nk_do_link(&ctx->last_widget_state, &win->buffer, bounds,
                    title, len, align, style, in, ctx->style.font,
                    ctx->style.window.background);
}
NK_API nk_bool
nk_link_text(struct nk_context *ctx, const char *title, int len, nk_flags align)
{
    NK_ASSERT(ctx);
    if (!ctx) return 0;
    return nk_link_text_styled(ctx, &ctx->style.link, title, len, align);
}
NK_API nk_bool
nk_link_label_styled(struct nk_context *ctx, const struct nk_style_link *style,
    const char *title, nk_flags align)
{
    return nk_link_text_styled(ctx, style, title, nk_strlen(title), align);
}
NK_API nk_bool
nk_link_label(struct nk_context *ctx, const char *title, nk_flags align)
{
    return nk_link_text(ctx, title, nk_strlen(title), align);
}
NK_API nk_bool
nk_link_text_underline(struct nk_context *ctx, const char *title, int len,
    nk_flags align, enum nk_link_underline underline)
{
    struct nk_style_link style;
    NK_ASSERT(ctx);
    if (!ctx) return 0;
    style = ctx->style.link;
    style.underline = underline;
    return nk_link_text_styled(ctx, &style, title, len, align);
}
NK_API nk_bool
nk_link_label_underline(struct nk_context *ctx, const char *title,
    nk_flags align, enum nk_link_underline underline)
{
    return nk_link_text_underline(ctx, title, nk_strlen(title), align, underline);
}
NK_API nk_bool
nk_link_text_hover_underline(struct nk_context *ctx, const char *title,
    int len, nk_flags align)
{
    return nk_link_text_underline(ctx, title, len, align, NK_LINK_UNDERLINE_HOVER);
}
NK_API nk_bool
nk_link_label_hover_underline(struct nk_context *ctx, const char *title,
    nk_flags align)
{
    return nk_link_text_hover_underline(ctx, title, nk_strlen(title), align);
}
NK_API nk_bool
nk_link_text_no_underline(struct nk_context *ctx, const char *title,
    int len, nk_flags align)
{
    return nk_link_text_underline(ctx, title, len, align, NK_LINK_UNDERLINE_NONE);
}
NK_API nk_bool
nk_link_label_no_underline(struct nk_context *ctx, const char *title,
    nk_flags align)
{
    return nk_link_text_no_underline(ctx, title, nk_strlen(title), align);
}
NK_API nk_bool
nk_link_text_colored(struct nk_context *ctx, const char *title, int len,
    nk_flags align, struct nk_color color)
{
    struct nk_style_link style;
    NK_ASSERT(ctx);
    if (!ctx) return 0;
    style = ctx->style.link;
    style.text_normal = color;
    style.text_hover = color;
    style.text_active = color;
    return nk_link_text_styled(ctx, &style, title, len, align);
}
NK_API nk_bool
nk_link_label_colored(struct nk_context *ctx, const char *title,
    nk_flags align, struct nk_color color)
{
    return nk_link_text_colored(ctx, title, nk_strlen(title), align, color);
}
