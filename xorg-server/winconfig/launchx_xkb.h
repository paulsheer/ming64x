#ifndef LAUNCHX_XKB_H
#define LAUNCHX_XKB_H

#include "launchx_common.h"

struct options_xkb {
    char xkblayout[128];
    char xkbmodel[128];
    char xkbvariant[128];
    char xkboptions[128];
    char xkbrules[128];
    char xkbdir[256];
};

void tab_xkb(struct nk_context *ctx, struct options_xkb *opt);

#endif /* LAUNCHX_XKB_H */
