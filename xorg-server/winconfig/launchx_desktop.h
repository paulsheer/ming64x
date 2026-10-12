#ifndef LAUNCHX_DESKTOP_H
#define LAUNCHX_DESKTOP_H

#include "launchx_common.h"

struct options_desktop_integration {
    int clipboard_enabled;
    int primary_enabled;
    int codepage_enabled;
    int hostintitle_enabled;
    int trayicon_enabled;
    char icon_spec[256];
    int compositewm_enabled;
    int compositealpha_enabled;
    char clipupdates[16];
};

void tab_desktop_integration(struct nk_context *ctx,
    struct options_desktop_integration *opt);

#endif /* LAUNCHX_DESKTOP_H */
