#include "launchx_fonts.h"

static int
is_hex_color(const char *s)
{
    int i;

    if (!s || strlen(s) != 6)
        return 0;
    for (i = 0; i < 6; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int
color_option(struct nk_context *ctx, const char *label, char *hex,
    const int hex_size, const char *tooltip)
{
    struct nk_rect b;
    struct nk_colorf cf;
    struct nk_color col;
    char tmp[12];
    char old[12];
    int changed;

    if (!is_hex_color(hex)) {
        strncpy(hex, "0F0F1E", hex_size - 1);
        hex[hex_size - 1] = '\0';
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, label, NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 120, 1);
    b = nk_widget_bounds(ctx);
    option_tooltip(ctx, b, tooltip);

    strncpy(old, hex, sizeof(old) - 1);
    old[sizeof(old) - 1] = '\0';

    col = nk_rgb_hex(hex);
    cf = nk_color_picker(ctx, nk_color_cf(col), NK_RGB);
    col = nk_rgb_cf(cf);
    nk_color_hex_rgb(tmp, col);

    changed = (strcmp(old, tmp) != 0);
    strncpy(hex, tmp, hex_size - 1);
    hex[hex_size - 1] = '\0';
    return changed;
}

void
tab_fonts_rendering(struct nk_context *ctx, struct options_fonts_rendering *opt)
{
    heading(ctx, "Fonts, rendering & appearance");

    text_option(ctx, "Font path (-fp)", opt->font_path, (int)sizeof(opt->font_path), "Set the search path for fonts. It should be a comma-separated list of\ndirectories and/or font server addresses.");

    combobox_option(ctx, "Render policy (-render)", render_items, RENDER_COUNT, &opt->render_sel, "Set the color allocation policy used by the Render extension. Valid\nvalues: default, mono, gray, color.");

    combobox_option(ctx, "Defer glyphs (-deferglyphs)", deferglyphs_items, DEFERGLYPHS_COUNT, &opt->deferglyphs_sel, "Defer glyph loading for the given font type: none (never), all (every\nfont), or 16 (16-bit fonts only).");

    text_option(ctx, "Fake screen fps (-fakescreenfps)", opt->fakescreenfps, (int)sizeof(opt->fakescreenfps), "Fake the default screen's frame rate to this many frames per second.\nThe default uses the real refresh rate.");

    combobox_option(ctx, "Backing store (+bs/-bs)", backingstore_items, BACKINGSTORE_COUNT, &opt->backingstore_sel, "+bs: enable backing store support on all screens. -bs: disable backing\nstore support on all screens.");

    if (color_option(ctx, "Root background (-bgcolor)", opt->root_background, (int)sizeof(opt->root_background), "Set the root window background color in hexadecimal RRGGBB format."))
        opt->retro_enabled = 0;

    checkbox_option(ctx, "Start with classic stipple (-retro)", &opt->retro_enabled, "Start with the classic stipple pattern and visible cursor. The default is\na black root window.");

    text_option(ctx, "Color visual class (-cc)", opt->color_visual_class, (int)sizeof(opt->color_visual_class), "Set the visual class for the root window of color screens, using the X\nprotocol class number.");
}
