#ifndef LAUNCHX_SCREEN_H
#define LAUNCHX_SCREEN_H

#include "launchx_common.h"

struct screen_geometry_entry {
    char width[16];
    char height[16];
    char x[16];
    char y[16];
    char monitor[16];
};

struct options_screen_windowing {
    struct screen_geometry_entry screens[3];
    int fullscreen_enabled;
    int rootless_enabled;
    int multiwindow_enabled;
    int nodecoration_enabled;
    int multimonitors_enabled;
    int resize_sel;
    int depth_sel;
    char refresh[16];
    int engine_sel;
    char dpi[16];
    int xinerama_sel;
    int disablexinerama_enabled;
    int lesspointer_enabled;
    int swcursor_enabled;
    int nocursor_enabled;
};

static const char *resize_items[] = {"none", "scrollbars", "randr"};
#define RESIZE_COUNT (sizeof(resize_items) / sizeof(resize_items[0]))

static const char *depth_items[] = {"Auto", "8", "15", "16", "24", "32"};
#define DEPTH_COUNT (sizeof(depth_items) / sizeof(depth_items[0]))

static const char *engine_items[] = {"Auto detect", "Shadow GDI (1)", "Shadow DirectDraw4 (4)"};
#define ENGINE_COUNT (sizeof(engine_items) / sizeof(engine_items[0]))

static const char *xinerama_items[] = {"Disabled", "Enabled"};
#define XINERAMA_COUNT (sizeof(xinerama_items) / sizeof(xinerama_items[0]))

void tab_screen_and_windowing(struct nk_context *ctx,
    struct options_screen_windowing *opt);
int screen_geometry_set(const struct options_screen_windowing *opt);

#endif /* LAUNCHX_SCREEN_H */
