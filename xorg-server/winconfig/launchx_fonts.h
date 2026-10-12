#ifndef LAUNCHX_FONTS_H
#define LAUNCHX_FONTS_H

#include "launchx_common.h"

struct options_fonts_rendering {
    char font_path[1024];
    int render_sel;
    int deferglyphs_sel;
    char fakescreenfps[16];
    int backingstore_sel;
    char root_background[16];
    int retro_enabled;
    char color_visual_class[16];
};

static const char *render_items[] = {"default", "mono", "gray", "color"};
#define RENDER_COUNT (sizeof(render_items) / sizeof(render_items[0]))

static const char *deferglyphs_items[] = {"none", "all", "16"};
#define DEFERGLYPHS_COUNT (sizeof(deferglyphs_items) / sizeof(deferglyphs_items[0]))

static const char *backingstore_items[] = {"Default", "Enable (+bs)", "Disable (-bs)"};
#define BACKINGSTORE_COUNT (sizeof(backingstore_items) / sizeof(backingstore_items[0]))

void tab_fonts_rendering(struct nk_context *ctx, struct options_fonts_rendering *opt);

#endif /* LAUNCHX_FONTS_H */
