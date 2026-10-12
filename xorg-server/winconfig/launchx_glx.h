#ifndef LAUNCHX_GLX_H
#define LAUNCHX_GLX_H

#include "launchx_common.h"

struct options_glx {
    int wgl_enabled;
    int swrastwgl_enabled;
    int iglx_enabled;
};

void tab_glx(struct nk_context *ctx, struct options_glx *opt);

#endif /* LAUNCHX_GLX_H */
