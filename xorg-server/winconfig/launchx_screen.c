#include "launchx_screen.h"

int
screen_geometry_set(const struct options_screen_windowing *opt)
{
    int i;

    for (i = 0; i < 3; ++i)
        if (opt->screens[i].width[0] || opt->screens[i].height[0] ||
            opt->screens[i].x[0] || opt->screens[i].y[0] ||
            opt->screens[i].monitor[0])
            return 1;
    return 0;
}

static char *
screen_geometry_field(struct options_screen_windowing *opt, int s, int f)
{
    struct screen_geometry_entry *e = &opt->screens[s];

    switch (f) {
    case 0: return e->width;
    case 1: return e->height;
    case 2: return e->x;
    case 3: return e->y;
    default: return e->monitor;
    }
}

static const char *scr_geom_screen_label[3] = {
    "Screen 1", "Screen 2", "Screen 3",
};

static const char *scr_geom_field_label[5] = {
    "Width", "Height", "X-offset", "Y-offset", "Monitor",
};

static const char *scr_geom_tip[5] = {
    "Screen width in pixels. Leave empty to derive it from the height (3:2).",
    "Screen height in pixels. Leave empty to derive it from the width (3:2).",
    "Horizontal position of the screen within the virtual desktop.",
    "Vertical position of the screen within the virtual desktop.",
    "Windows monitor (1-based) to place this screen on; empty = default.",
};

void
tab_screen_and_windowing(struct nk_context *ctx,
    struct options_screen_windowing *opt)
{
    int fs, rl, mw, nd, lp, resize_scrollbars, scr;
    int s;

    heading(ctx, "Screen & windowing modes");

    fs = opt->fullscreen_enabled;
    rl = opt->rootless_enabled;
    mw = opt->multiwindow_enabled;
    nd = opt->nodecoration_enabled;
    lp = opt->lesspointer_enabled;
    resize_scrollbars = (opt->resize_sel == 1);
    scr = screen_geometry_set(opt);

    if (mw || fs || rl || opt->multimonitors_enabled)
        nk_widget_disable_begin(ctx);
    for (s = 0; s < 3; ++s) {
        nk_style_push_font(ctx, launchx_bold_font());
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, scr_geom_screen_label[s], NK_TEXT_LEFT);
        nk_style_pop_font(ctx);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[0], screen_geometry_field(opt, s, 0), 16, scr_geom_tip[0], NK_TEXT_RIGHT);
        text_option_cell(ctx, scr_geom_field_label[1], screen_geometry_field(opt, s, 1), 16, scr_geom_tip[1], NK_TEXT_RIGHT);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[2], screen_geometry_field(opt, s, 2), 16, scr_geom_tip[2], NK_TEXT_RIGHT);
        text_option_cell(ctx, scr_geom_field_label[3], screen_geometry_field(opt, s, 3), 16, scr_geom_tip[3], NK_TEXT_RIGHT);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[4], screen_geometry_field(opt, s, 4), 16, scr_geom_tip[4], NK_TEXT_RIGHT);
    }
    if (mw || fs || rl || opt->multimonitors_enabled)
        nk_widget_disable_end(ctx);

    if (mw || rl || nd || lp || resize_scrollbars || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Run in fullscreen mode (-fullscreen)", &opt->fullscreen_enabled, "Make the X server window fill the entire Windows desktop.");
    if (mw || rl || nd || lp || resize_scrollbars || scr)
        nk_widget_disable_end(ctx);

    if (mw || fs || nd || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Transparent root window (-rootless)", &opt->rootless_enabled, "Run rootless: the root window is hidden and only top-level X windows\nshow. Needs an external window manager; not with -multiwindow or\n-fullscreen.");
    if (mw || fs || nd || scr)
        nk_widget_disable_end(ctx);

    if (rl || fs || nd || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Run in multiwindow mode (-multiwindow)", &opt->multiwindow_enabled, "Run multiwindow: each top-level X window becomes its own Windows\nwindow with a built-in window manager. Not with -rootless or\n-fullscreen.");
    if (rl || fs || nd || scr)
        nk_widget_disable_end(ctx);

    if (mw || rl || fs)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "No window border/titlebar (-nodecoration)", &opt->nodecoration_enabled, "Show the X window with no Windows border or title bar. Ignored when\n-fullscreen is set.");
    if (mw || rl || fs)
        nk_widget_disable_end(ctx);

    if (scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Use entire virtual screen (-multimonitors)", &opt->multimonitors_enabled, "Create one screen covering all monitors, with fake XINERAMA data\ndescribing each monitor.");
    if (scr)
        nk_widget_disable_end(ctx);

    if (fs)
        nk_widget_disable_begin(ctx);
    combobox_option(ctx, "Resize mode (-resize)", resize_items, RESIZE_COUNT, &opt->resize_sel, "How the X screen resizes: scrollbars adds window scrollbars, randr\nuses the RANDR extension. Default is randr.");
    if (fs)
        nk_widget_disable_end(ctx);

    if (!fs)
        nk_widget_disable_begin(ctx);
    combobox_option(ctx, "Bit depth (-depth)", depth_items, DEPTH_COUNT, &opt->depth_sel, "Color depth in bits per pixel for fullscreen mode with a DirectDraw\nengine. Ignored without -fullscreen.");
    if (!fs)
        nk_widget_disable_end(ctx);

    if (!fs)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Refresh rate (-refresh)", opt->refresh, (int)sizeof(opt->refresh), "Refresh rate (Hz) for fullscreen mode with a DirectDraw engine.\nIgnored without -fullscreen.");
    if (!fs)
        nk_widget_disable_end(ctx);

    combobox_option(ctx, "Engine (-engine)", engine_items, ENGINE_COUNT, &opt->engine_sel, "Override the automatically selected drawing engine: 1 = Shadow GDI,\n4 = Shadow DirectDraw4 Non-Locking.");

    text_option(ctx, "Screen resolution DPI (-dpi)", opt->dpi, (int)sizeof(opt->dpi), "Screen resolution in dots per inch, for all screens.");

    combobox_option(ctx, "XINERAMA (+xinerama/-xinerama)", xinerama_items, XINERAMA_COUNT, &opt->xinerama_sel, "Enable (+) or disable (-) the XINERAMA extension.");

    checkbox_option(ctx, "Disable XINERAMA extension (-disablexineramaextension)", &opt->disablexinerama_enabled, "Disable the XINERAMA extension.");

    if (fs)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Hide Windows pointer (-lesspointer)", &opt->lesspointer_enabled, "Also hide the Windows pointer over inactive X windows, preventing a\nghost cursor. Only applies with -swcursor.");
    if (fs)
        nk_widget_disable_end(ctx);

    checkbox_option(ctx, "X11 software cursor (-swcursor)", &opt->swcursor_enabled, "Use the X11 software cursor instead of the Windows cursor.");

    checkbox_option(ctx, "Disable cursor (-nocursor)", &opt->nocursor_enabled, "Disable the X cursor so it is not drawn on any screen.");
}
