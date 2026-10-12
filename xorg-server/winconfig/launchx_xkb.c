#include "launchx_xkb.h"

void
tab_xkb(struct nk_context *ctx, struct options_xkb *opt)
{
    heading(ctx, "XKB keyboard layout");

    text_option(ctx, "Layout (-xkblayout)", opt->xkblayout, (int)sizeof(opt->xkblayout), "Keyboard layout to load at startup (e.g. de, us). Defaults to a layout\nmatching your current Windows layout.");

    text_option(ctx, "Model (-xkbmodel)", opt->xkbmodel, (int)sizeof(opt->xkbmodel), "Keyboard model to load (default pc105).");

    text_option(ctx, "Variant (-xkbvariant)", opt->xkbvariant, (int)sizeof(opt->xkbvariant), "Keyboard layout variant (e.g. nodeadkeys). Defaults to unset.");

    text_option(ctx, "Options (-xkboptions)", opt->xkboptions, (int)sizeof(opt->xkboptions), "XKB options to load at startup.");

    text_option(ctx, "Rules (-xkbrules)", opt->xkbrules, (int)sizeof(opt->xkbrules), "XKB rules file to use (default xorg).");

    text_option(ctx, "Base directory (-xkbdir)", opt->xkbdir, (int)sizeof(opt->xkbdir), "Base directory for XKB keyboard layout files.");
}
