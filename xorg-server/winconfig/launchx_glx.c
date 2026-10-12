#include "launchx_glx.h"

void
tab_glx(struct nk_context *ctx, struct options_glx *opt)
{
    heading(ctx, "OpenGL / GLX");

    checkbox_option(ctx, "Native WGL for GLX (-wgl)", &opt->wgl_enabled, "Enable the GLX extension to use the native Windows WGL interface for\nhardware-accelerated OpenGL.");

    checkbox_option(ctx, "WGL swrast for GLX (-swrastwgl)", &opt->swrastwgl_enabled, "Enable the GLX extension to use the native Windows WGL interface with\nthe swrast software renderer.");

    checkbox_option(ctx, "Allow indirect GLX contexts (+iglx)", &opt->iglx_enabled, "+iglx: allow creating indirect GLX contexts (default). -iglx: prohibit\ncreating indirect GLX contexts.");
}
